// AcuRogerSAPI.cpp  -  SAPI5 TTS engine for "AcuVoice Roger"
//
// A from-scratch ISpTTSEngine COM in-proc server that drives the AcuVoice
// desktop engine (avcore.dll) and presents it to Windows as a SAPI5 voice.
//
// Pipeline per Speak():  SAPI WCHAR fragments -> ANSI -> avcore
// _txtstr_to_sndbuf (8 kHz u-law) -> u-law->PCM16 -> ISpTTSEngineSite::Write.
//
// Engine config (soundbank/dict paths + pauses) is served to avcore via the
// system IniFileMapping redirect of "acuvoice.ini" -> registry (no C:\Windows file).
//
// 32-bit only (avcore is 32-bit). Build: cl /LD /EHsc AcuRogerSAPI.cpp /link /OUT:AcuRogerSAPI.dll (MSVC++ 2017+). Register with regsvr32; then "AcuVoice Roger" appears as a SAPI voice.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

// ---- where avcore lives (dev path; installer rewrites this or uses module-relative) ----
// {68E2D748-B030-48AF-BCBD-05D07352F9A7}
static const CLSID CLSID_AcuRoger =
{ 0x68e2d748, 0xb030, 0x48af, { 0xbc, 0xbd, 0x05, 0xd0, 0x73, 0x52, 0xf9, 0xa7 } };

static const wchar_t* TOKEN_ID   = L"AcuVoiceRoger";
static const wchar_t* TOKEN_NAME = L"AcuVoice Roger";

static LONG  g_cObj    = 0;   // live objects
static LONG  g_cLock   = 0;   // class-factory locks
static HMODULE g_hSelf = NULL;

// ---------- avcore binding (lazy, process-wide, loaded once) ----------
typedef unsigned (__stdcall *txtstr_to_sndbuf_t)(const char* text, void** outBuf, unsigned* outLen, unsigned char flags);
typedef unsigned (__stdcall *free_sndbuf_t)(void** pBuf);   // frees *pBuf (takes ADDRESS of the buffer ptr)

static HMODULE             g_avcore = NULL;
static txtstr_to_sndbuf_t  g_synth  = NULL;
static free_sndbuf_t       g_free   = NULL;
static CRITICAL_SECTION    g_avLock;
static bool                g_avLockInit = false;

static bool EnsureAvcore()
{
    EnterCriticalSection(&g_avLock);
    bool ok = (g_synth != NULL);
    if (!ok) {
        if (!g_avcore) {
            // Load the patched engine core from <this dll's dir>\lib\avcore_acu.dll.
            // avcore_acu has a .acu PE section repointing its config-file string to an
            // absolute path, so it reads config by direct file I/O in any host.
            wchar_t path[MAX_PATH]; GetModuleFileNameW(g_hSelf, path, MAX_PATH);
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) { *(slash + 1) = 0; wcsncat(path, L"lib\\avcore_acu.dll", MAX_PATH - wcslen(path) - 1); }
            g_avcore = LoadLibraryW(path);
        }
        if (g_avcore) {
            g_synth = (txtstr_to_sndbuf_t)GetProcAddress(g_avcore, "_txtstr_to_sndbuf@16");
            g_free  = (free_sndbuf_t)     GetProcAddress(g_avcore, "_free_sndbuf@4");
            ok = (g_synth && g_free);
        }
    }
    LeaveCriticalSection(&g_avLock);
    return ok;
}

// ---------- u-law -> PCM16 table ----------
static short g_ulaw[256];
static void BuildUlaw()
{
    for (int i = 0; i < 256; i++) {
        int u = (~i) & 0xFF;
        int sign = u & 0x80, exp = (u >> 4) & 7, man = u & 0x0F;
        int s = (((man << 3) + 0x84) << exp) - 0x84;
        g_ulaw[i] = (short)(sign ? -s : s);
    }
}

// ============================ the engine object ============================
class CAcuRoger : public ISpTTSEngine, public ISpObjectWithToken
{
    LONG m_ref;
    ISpObjectToken* m_token;
public:
    CAcuRoger() : m_ref(1), m_token(NULL) { InterlockedIncrement(&g_cObj); }
    ~CAcuRoger() { if (m_token) m_token->Release(); InterlockedDecrement(&g_cObj); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISpTTSEngine))
            *ppv = static_cast<ISpTTSEngine*>(this);
        else if (riid == __uuidof(ISpObjectWithToken))
            *ppv = static_cast<ISpObjectWithToken*>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // ISpObjectWithToken
    STDMETHODIMP SetObjectToken(ISpObjectToken* p)
    {
        if (m_token) m_token->Release();
        m_token = p;
        if (m_token) m_token->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetObjectToken(ISpObjectToken** pp)
    {
        if (!pp) return E_POINTER;
        *pp = m_token;
        if (m_token) m_token->AddRef();
        return m_token ? S_OK : S_FALSE;
    }

    // ISpTTSEngine
    STDMETHODIMP Speak(DWORD dwSpeakFlags, REFGUID, const WAVEFORMATEX*,
                       const SPVTEXTFRAG* pFrag, ISpTTSEngineSite* site);
    STDMETHODIMP GetOutputFormat(const GUID*, const WAVEFORMATEX*,
                                 GUID* pFmtId, WAVEFORMATEX** ppCoMemWFEX);
};

STDMETHODIMP CAcuRoger::GetOutputFormat(const GUID*, const WAVEFORMATEX*,
                                        GUID* pFmtId, WAVEFORMATEX** ppwfex)
{
    if (!pFmtId || !ppwfex) return E_POINTER;
    WAVEFORMATEX* w = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!w) return E_OUTOFMEMORY;
    w->wFormatTag = WAVE_FORMAT_PCM;
    w->nChannels = 1;
    w->nSamplesPerSec = 8000;
    w->wBitsPerSample = 16;
    w->nBlockAlign = 2;
    w->nAvgBytesPerSec = 16000;
    w->cbSize = 0;
    *ppwfex = w;
    *pFmtId = SPDFID_WaveFormatEx;
    return S_OK;
}

STDMETHODIMP CAcuRoger::Speak(DWORD, REFGUID, const WAVEFORMATEX*,
                              const SPVTEXTFRAG* pFrag, ISpTTSEngineSite* site)
{
    if (!site) return E_POINTER;
    if (!EnsureAvcore()) return SPERR_NOT_FOUND;

    HRESULT hr = S_OK;

    // ---- 1) gather fragment text -> wide buffer + per-char source offsets ----
    size_t wlen = 0;
    for (const SPVTEXTFRAG* f = pFrag; f; f = f->pNext) wlen += f->ulTextLen;
    if (wlen == 0) return S_OK;
    wchar_t* wbuf   = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(wchar_t));
    ULONG*   srcoff = (ULONG*)  HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(ULONG));
    if (!wbuf || !srcoff) { if(wbuf)HeapFree(GetProcessHeap(),0,wbuf); if(srcoff)HeapFree(GetProcessHeap(),0,srcoff); return E_OUTOFMEMORY; }
    size_t wpos = 0;
    for (const SPVTEXTFRAG* f = pFrag; f; f = f->pNext) {
        for (ULONG j = 0; j < f->ulTextLen && f->pTextStart; j++) {
            wbuf[wpos]   = f->pTextStart[j];
            srcoff[wpos] = f->ulTextSrcOffset + j;
            wpos++;
        }
    }
    wbuf[wpos] = 0;

    // ---- 2) tokenize into words (maximal non-space runs containing >=1 alnum) ----
    const unsigned MAXW = 2048;
    ULONG*    wPos  = (ULONG*)   HeapAlloc(GetProcessHeap(),0,MAXW*sizeof(ULONG));    // source char position
    ULONG*    wLen  = (ULONG*)   HeapAlloc(GetProcessHeap(),0,MAXW*sizeof(ULONG));    // source char length
    unsigned* wWt   = (unsigned*)HeapAlloc(GetProcessHeap(),0,MAXW*sizeof(unsigned)); // duration weight (alnum count)
    unsigned* wSamp = (unsigned*)HeapAlloc(GetProcessHeap(),0,MAXW*sizeof(unsigned)); // audio sample offset (filled later)
    unsigned nWords = 0;
    for (size_t i = 0; i < wpos && nWords < MAXW; ) {
        while (i < wpos && iswspace(wbuf[i])) i++;
        if (i >= wpos) break;
        size_t start = i; unsigned wt = 0;
        while (i < wpos && !iswspace(wbuf[i])) { if (iswalnum(wbuf[i])) wt++; i++; }
        if (wt > 0) { wPos[nWords]=srcoff[start]; wLen[nWords]=(ULONG)(i-start); wWt[nWords]=wt; nWords++; }
    }

    // ---- 3) wide -> ANSI for avcore, then synthesize ----
    int abytes = WideCharToMultiByte(CP_ACP, 0, wbuf, -1, NULL, 0, NULL, NULL);
    char* abuf = (char*)HeapAlloc(GetProcessHeap(), 0, abytes > 0 ? abytes : 1);
    WideCharToMultiByte(CP_ACP, 0, wbuf, -1, abuf, abytes, NULL, NULL);

    void* sbuf = NULL; unsigned slen = 0;
    EnterCriticalSection(&g_avLock);                 // avcore uses process-global state; serialize
    unsigned rc = g_synth(abuf, &sbuf, &slen, 0);
    LeaveCriticalSection(&g_avLock);
    HeapFree(GetProcessHeap(), 0, abuf);

    if (rc == 0 && sbuf) {
        const unsigned char* ub = (const unsigned char*)sbuf;

        // ---- 4) map each word to an audio sample offset by SPEECH time ----
        // Silence samples don't advance the word timeline, so highlights pause
        // during the engine's comma/sentence gaps instead of running ahead.
        const int SIL = 350;                          // |amplitude| below this counts as silence
        unsigned totalSpeech = 0;
        for (unsigned i = 0; i < slen; i++) { int a = g_ulaw[ub[i]]; if (a < 0) a = -a; if (a >= SIL) totalSpeech++; }
        unsigned totalWt = 0; for (unsigned k = 0; k < nWords; k++) totalWt += wWt[k];
        if (totalWt == 0) totalWt = 1;
        {
            unsigned i = 0, speechSoFar = 0, cumBefore = 0;
            for (unsigned k = 0; k < nWords; k++) {
                unsigned target = (unsigned)((unsigned long long)totalSpeech * cumBefore / totalWt);
                while (i < slen && speechSoFar < target) { int a = g_ulaw[ub[i]]; if (a < 0) a = -a; if (a >= SIL) speechSoFar++; i++; }
                wSamp[k] = i;
                cumBefore += wWt[k];
            }
        }

        // ---- 5) PCM16 out in chunks; fire word-boundary events as we reach them ----
        USHORT vol = 100; site->GetVolume(&vol);      // rate/pitch accepted but ignored (engine limitation)
        const unsigned CHUNK = 4096;
        short pcm[4096];
        unsigned off = 0, nextW = 0;
        for (; off < slen; ) {
            if (site->GetActions() & SPVES_ABORT) break;
            unsigned chunkEnd = (slen - off > CHUNK) ? off + CHUNK : slen;
            while (nextW < nWords && wSamp[nextW] < chunkEnd) {
                SPEVENT ev = {};
                ev.eEventId            = SPEI_WORD_BOUNDARY;
                ev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
                ev.ullAudioStreamOffset = (ULONGLONG)wSamp[nextW] * sizeof(short); // output PCM16 byte offset
                ev.wParam              = (WPARAM)wLen[nextW];                       // word length (chars)
                ev.lParam              = (LPARAM)wPos[nextW];                       // word position in source text
                site->AddEvents(&ev, 1);
                nextW++;
            }
            unsigned n = chunkEnd - off;
            const unsigned char* u = ub + off;
            for (unsigned k = 0; k < n; k++) {
                int v = g_ulaw[u[k]];
                if (vol != 100) v = (v * vol) / 100;
                pcm[k] = (short)v;
            }
            ULONG written = 0;
            hr = site->Write(pcm, n * sizeof(short), &written);
            if (FAILED(hr)) break;
            off += n;
        }
        EnterCriticalSection(&g_avLock); g_free(&sbuf); LeaveCriticalSection(&g_avLock);
    } else {
        if (sbuf) { EnterCriticalSection(&g_avLock); g_free(&sbuf); LeaveCriticalSection(&g_avLock); }
        hr = E_FAIL;
    }

    HeapFree(GetProcessHeap(),0,wbuf);  HeapFree(GetProcessHeap(),0,srcoff);
    HeapFree(GetProcessHeap(),0,wPos);  HeapFree(GetProcessHeap(),0,wLen);
    HeapFree(GetProcessHeap(),0,wWt);   HeapFree(GetProcessHeap(),0,wSamp);
    return hr;
}

// ============================ class factory ============================
class CFactory : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return 2; }
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        CAcuRoger* o = new CAcuRoger();
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = o->QueryInterface(riid, ppv);
        o->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL f) { if (f) InterlockedIncrement(&g_cLock); else InterlockedDecrement(&g_cLock); return S_OK; }
};
static CFactory g_factory;

// ============================ exports ============================
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (rclsid == CLSID_AcuRoger) return g_factory.QueryInterface(riid, ppv);
    *ppv = NULL; return CLASS_E_CLASSNOTAVAILABLE;
}
STDAPI DllCanUnloadNow() { return (g_cObj == 0 && g_cLock == 0) ? S_OK : S_FALSE; }

// ---------- registration helpers ----------
static LONG SetVal(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* val)
{
    HKEY k; LONG r = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hSelf, path, MAX_PATH);

    wchar_t clsid[64];
    StringFromGUID2(CLSID_AcuRoger, clsid, 64);   // "{...}"

    // COM server (32-bit regsvr32 redirects SOFTWARE\Classes -> WOW6432Node)
    wchar_t sub[256];
    swprintf(sub, 256, L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    SetVal(HKEY_LOCAL_MACHINE, sub, NULL, TOKEN_NAME);
    swprintf(sub, 256, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    SetVal(HKEY_LOCAL_MACHINE, sub, NULL, path);
    SetVal(HKEY_LOCAL_MACHINE, sub, L"ThreadingModel", L"Both");

    // SAPI5 voice token (32-bit -> WOW6432Node\...\Speech\Voices\Tokens)
    swprintf(sub, 256, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s", TOKEN_ID);
    SetVal(HKEY_LOCAL_MACHINE, sub, NULL, TOKEN_NAME);
    SetVal(HKEY_LOCAL_MACHINE, sub, L"409", TOKEN_NAME);
    SetVal(HKEY_LOCAL_MACHINE, sub, L"CLSID", clsid);
    swprintf(sub, 256, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s\\Attributes", TOKEN_ID);
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Name",     TOKEN_NAME);
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Gender",   L"Male");
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Age",      L"Adult");
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Language", L"409");        // en-US
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Vendor",   L"Fonix / AcuVoice");
    SetVal(HKEY_LOCAL_MACHINE, sub, L"Version",  L"5.1");
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    wchar_t clsid[64]; StringFromGUID2(CLSID_AcuRoger, clsid, 64);
    wchar_t sub[256];
    swprintf(sub, 256, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s", TOKEN_ID);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub);
    swprintf(sub, 256, L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_avLock); g_avLockInit = true;
        BuildUlaw();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_avLockInit) DeleteCriticalSection(&g_avLock);
    }
    return TRUE;
}
