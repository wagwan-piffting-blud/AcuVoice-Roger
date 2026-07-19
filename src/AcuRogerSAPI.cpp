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

// ---------- <pron> support: SAPI phonemes -> avcore \avprn= inline tag ----------
//
// SAPI hands <pron sym="..."> to the engine as State.pPhoneIds (an SPPHONEID/WCHAR
// array) under SPVA_Pronounce, with *empty text*. avcore has no phoneme-input API on
// the text path, but its control-tag parser (armed by flags bit1) supports
//     \avprn=<word>=<transcription>\
// which registers a HIGHEST-priority pronunciation override: FUN_10042460 consults it
// before userdict/dict/aux/LTS. So we register a nonce word -> the AcuVoice
// transcription, then speak the nonce. The transcription uses the human Userdict.txt
// alphabet (data_format_report.md 3.3) and MUST NOT contain '\' (the tag terminator),
// so we emit only primary-stress '/'. The \avprn key is tolower'd by the engine, so the
// nonce is lowercase.
static ISpPhoneConverter* g_phone = NULL;              // en-US SAPI phoneme <-> id converter
static const char* const  AVPRN_NONCE = "qzjx";        // lowercase, non-lexical, tokenizes as one word

// Create the en-US SAPI phoneme converter by binding the Language=409 token from the
// phone-converter category. ISpPhoneConverter has no SetLanguageId -- the language lives
// in its object token. Its default alphabet is the classic SAPI set (what <pron sym="...">
// uses), so IdToPhone yields "f r ay 1 d ey 2"-style symbols. Falls back gracefully to
// NULL (caller skips the pron) if no such token is registered.
static bool EnsurePhoneConv()
{
    if (g_phone) return true;
    ISpObjectTokenCategory* cat = NULL;
    if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), NULL, CLSCTX_ALL,
                                __uuidof(ISpObjectTokenCategory), (void**)&cat)) || !cat)
        return false;
    if (SUCCEEDED(cat->SetId(SPCAT_PHONECONVERTERS, FALSE))) {
        IEnumSpObjectTokens* en = NULL;
        if (SUCCEEDED(cat->EnumTokens(L"Language=409", NULL, &en)) && en) {
            ISpObjectToken* tok = NULL;
            if (en->Next(1, &tok, NULL) == S_OK && tok) {
                ISpPhoneConverter* pc = NULL;
                if (SUCCEEDED(tok->CreateInstance(NULL, CLSCTX_ALL,
                                                  __uuidof(ISpPhoneConverter), (void**)&pc)) && pc)
                    g_phone = pc;                          // holds ref for process lifetime
                tok->Release();
            }
            en->Release();
        }
    }
    cat->Release();
    return g_phone != NULL;
}

// SAPI American-English phoneme -> AcuVoice transcription. Vowel values verified against
// avcore's own segmentation (synth_to_phon, web/native/acu_phon.c): e.g. day->da~,
// Friday->fri~da~, bird->bu|d, father->fot~u|, this->t~is. To re-derive/extend, dump a
// word that isolates the phoneme and read off the vowel/consonant letters.
struct PhMap { const char* sapi; const char* acu; int vowel; };
static const PhMap g_phmap[] = {
    // vowels / diphthongs
    {"iy","e~",1}, {"ih","i",1},  {"ey","a~",1}, {"eh","e",1},  {"ae","a",1},
    {"aa","o",1},  {"ao","o",1},  {"ah","u",1},  {"ax","u",1},  {"uh","u",1},
    {"uw","u~",1}, {"er","u|",1},  {"ay","i~",1}, {"aw","aw",1}, {"oy","oy",1},
    {"ow","o~",1},   // aw/oy use w/y glide forms, NOT the authentic u/i offglides (au/oi):
                     // the \avprn normalizer splits word-final V+V into two syllables, but
                     // the "aw"/"oy" units exist and stay one syllable (verified vs real).
    // consonants
    {"b","b",0},  {"ch","c~",0}, {"d","d",0},  {"dh","t~",0}, {"f","f",0}, {"g","g",0},
    {"hh","h",0}, {"h","h",0},   {"jh","j",0}, {"k","k",0},  {"l","l",0}, {"m","m",0},
    {"n","n",0},  {"ng","n~",0}, {"p","p",0},  {"r","r",0},  {"s","s",0}, {"sh","s~",0},
    {"t","t",0},  {"th","t'",0}, {"v","v",0},  {"w","w",0},  {"y","y",0}, {"z","z",0},
    {"zh","z~",0},
};

static const PhMap* MapPhone(const char* s)
{
    for (unsigned i = 0; i < sizeof(g_phmap)/sizeof(g_phmap[0]); i++)
        if (strcmp(g_phmap[i].sapi, s) == 0) return &g_phmap[i];
    return NULL;
}

// Translate a SAPI phoneme-id array to AcuVoice transcription. Returns chars written
// (0 = untranslatable -> caller skips the fragment). Never emits '\'.
static int BuildAcuPron(const SPPHONEID* ids, char* out, int outsz)
{
    if (!ids || !*ids || !EnsurePhoneConv()) return 0;
    WCHAR wsym[512]; wsym[0] = 0;
    if (FAILED(g_phone->IdToPhone(ids, wsym))) return 0;    // -> space-separated syms, e.g. L"f r ay 1 d ey 2"
    int n = 0, stressAt = -1; bool laterVowel = false;
    char tok[16]; int tl = 0;
    for (int i = 0; ; i++) {
        WCHAR w = wsym[i];
        if (w != L' ' && w != 0) { if (tl < 15) tok[tl++] = (char)towlower(w); continue; }
        tok[tl] = 0;
        if (tl) {
            if (tok[0] == '1') stressAt = n;                    // boundary falls just AFTER the stressed vowel
            else if (tok[0] == '2') { /* secondary: dropped ('\' is illegal in the value) */ }
            else {
                const PhMap* m = MapPhone(tok);
                if (m) {
                    if (m->vowel && stressAt >= 0) laterVowel = true;   // a syllable follows the stressed one
                    for (const char* p = m->acu; *p && n < outsz-2; p++) out[n++] = *p;
                }
            }
        }
        tl = 0;
        if (w == 0) break;
    }
    out[n] = 0;
    // Human notation puts '/' right after the primary-stressed vowel (ha/re~, mo~tu|o~/lu),
    // and only when another syllable follows -- monosyllables (be~t) take no boundary.
    if (stressAt > 0 && stressAt < n && laterVowel && n < outsz-2) {
        memmove(out+stressAt+1, out+stressAt, n-stressAt+1);
        out[stressAt] = '/';
        n++;
    }
    return n;
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
    HANDLE  heap = GetProcessHeap();
    const int SIL = 350;                               // |u-law amplitude| below this = silence

    // ---- 1) gather fragments -> wide buffer + per-char source offsets ----
    // Text fragments copy through verbatim. A <pron sym="..."> arrives as an
    // SPVA_Pronounce fragment with phonemes in State.pPhoneIds and EMPTY text; we turn
    // it into avcore's \avprn= inline pronunciation tag + a nonce word to speak (see
    // BuildAcuPron and the per-chunk tag flag in step 3). Count pron fragments to size
    // the buffer for the injected tags.
    size_t wlen = 0; unsigned nPron = 0;
    for (const SPVTEXTFRAG* f = pFrag; f; f = f->pNext) {
        wlen += f->ulTextLen;
        if (f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds && *f->State.pPhoneIds) nPron++;
    }
    if (wlen == 0 && nPron == 0) return S_OK;
    const size_t PRONMAX = 128;                        // worst-case chars per injected \avprn tag + nonce
    size_t cap = wlen + (size_t)nPron * PRONMAX + 1;
    wchar_t* wbuf   = (wchar_t*)HeapAlloc(heap, 0, cap * sizeof(wchar_t));
    ULONG*   srcoff = (ULONG*)  HeapAlloc(heap, 0, cap * sizeof(ULONG));
    if (!wbuf || !srcoff) { if(wbuf)HeapFree(heap,0,wbuf); if(srcoff)HeapFree(heap,0,srcoff); return E_OUTOFMEMORY; }
    size_t wpos = 0;
    for (const SPVTEXTFRAG* f = pFrag; f; f = f->pNext) {
        if (f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds && *f->State.pPhoneIds) {
            char pron[96];
            if (BuildAcuPron(f->State.pPhoneIds, pron, sizeof(pron))) {
                char tag[160];
                int tn = _snprintf(tag, sizeof(tag), " \\avprn=%s=%s\\ %s ", AVPRN_NONCE, pron, AVPRN_NONCE);
                for (int j = 0; j < tn && j >= 0 && wpos < cap-1; j++) {
                    wbuf[wpos] = (wchar_t)(unsigned char)tag[j]; srcoff[wpos] = f->ulTextSrcOffset; wpos++;
                }
            }
            continue;                                  // pron fragments carry no text
        }
        for (ULONG j = 0; j < f->ulTextLen && f->pTextStart && wpos < cap-1; j++) {
            wbuf[wpos]   = f->pTextStart[j];
            srcoff[wpos] = f->ulTextSrcOffset + j;
            wpos++;
        }
    }
    wbuf[wpos] = 0;

    // ---- 2) split into chunks (<= CHUNKMAX chars, broken at sentence/clause/space). ----
    // avcore infinite-loops on long, expansion-dense text (numbers/dates/abbrevs) in a
    // single synth call: its token-accumulation loop reaches a state where neither exit
    // condition (text-exhausted / segment-full) fires. Keeping each call small avoids it.
    const size_t   CHUNKMAX = 200;
    const unsigned MAXCH = 8192;
    size_t* chStart = (size_t*)HeapAlloc(heap,0,MAXCH*sizeof(size_t));
    size_t* chLen   = (size_t*)HeapAlloc(heap,0,MAXCH*sizeof(size_t));
    char*   chPunct = (char*)  HeapAlloc(heap,0,MAXCH);  // 1 = chunk ends at punctuation (keep avcore's pause)
    unsigned nCh = 0;
    for (size_t i = 0; i < wpos && nCh < MAXCH; ) {
        while (i < wpos && iswspace(wbuf[i])) i++;
        if (i >= wpos) break;
        size_t start = i;
        if (wpos - start <= CHUNKMAX) { chStart[nCh]=start; chLen[nCh]=wpos-start; chPunct[nCh]=1; nCh++; break; }
        size_t limit = start + CHUNKMAX, brk = 0; char punct = 0;
        for (size_t j = start; j < limit; j++) { wchar_t c=wbuf[j]; if(c==L'.'||c==L'!'||c==L'?'||c==L';'||c==L':'||c==L'\n') brk=j+1; }
        if (brk) punct = 1;
        if (!brk) { for (size_t j=start;j<limit;j++) if(wbuf[j]==L',') brk=j+1; if(brk) punct=1; }
        if (!brk) for (size_t j=limit;j>start;j--) if(iswspace(wbuf[j-1])){ brk=j; break; }  // forced split (punct stays 0)
        if (brk <= start) brk = limit;                 // hard cap (single huge token)
        chStart[nCh]=start; chLen[nCh]=brk-start; chPunct[nCh]=punct; nCh++;
        i = brk;
    }

    ULONGLONG ei = 0; site->GetEventInterest(&ei);
    bool wantWords = (ei & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
    USHORT vol = 100; site->GetVolume(&vol);           // rate/pitch accepted but ignored (engine limitation)

    // reusable per-chunk word arrays
    const unsigned MAXW = 2048;
    ULONG*    wPos  = (ULONG*)   HeapAlloc(heap,0,MAXW*sizeof(ULONG));   // source char position
    ULONG*    wLen  = (ULONG*)   HeapAlloc(heap,0,MAXW*sizeof(ULONG));   // word char length
    ULONG*    wBuf  = (ULONG*)   HeapAlloc(heap,0,MAXW*sizeof(ULONG));   // word start index in wbuf
    unsigned* wWt   = (unsigned*)HeapAlloc(heap,0,MAXW*sizeof(unsigned));// duration weight
    unsigned* wSamp = (unsigned*)HeapAlloc(heap,0,MAXW*sizeof(unsigned));// audio sample offset within chunk
    short pcm[4096]; const unsigned PCH = 4096;
    ULONGLONG audioBase = 0;                            // cumulative output bytes across chunks

    for (unsigned c = 0; c < nCh && SUCCEEDED(hr); c++) {
        if (site->GetActions() & SPVES_ABORT) break;
        size_t cs = chStart[c], cl = chLen[c];

        // ---- 3) synthesize this chunk ----
        wchar_t saved = wbuf[cs+cl]; wbuf[cs+cl] = 0;
        int abytes = WideCharToMultiByte(CP_ACP,0,wbuf+cs,-1,NULL,0,NULL,NULL);
        char* abuf = (char*)HeapAlloc(heap,0,abytes>0?abytes:1);
        WideCharToMultiByte(CP_ACP,0,wbuf+cs,-1,abuf,abytes,NULL,NULL);
        void* sbuf=NULL; unsigned slen=0;
        // Enable avcore control-tag parsing (flags bit1) only for chunks that carry an
        // injected \avprn tag. Tag-free text keeps flags=0 -> byte-identical to before,
        // so stray backslashes in ordinary text can never be misread as tags.
        unsigned char synflags = 0;
        for (size_t j = cs; j < cs+cl; j++) if (wbuf[j] == L'\\') { synflags = 2; break; }
        EnterCriticalSection(&g_avLock); unsigned rc=g_synth(abuf,&sbuf,&slen,synflags); LeaveCriticalSection(&g_avLock);
        HeapFree(heap,0,abuf);
        wbuf[cs+cl] = saved;
        if (rc != 0 || !sbuf) { if(sbuf){EnterCriticalSection(&g_avLock);g_free(&sbuf);LeaveCriticalSection(&g_avLock);} hr=E_FAIL; break; }
        const unsigned char* ub = (const unsigned char*)sbuf;

        // If this chunk was a forced mid-phrase split (no trailing punctuation) and isn't
        // the last chunk, avcore appended a full ~680 ms sentence-pause that doesn't belong
        // mid-sentence. Trim it back to a natural word juncture so the splice is seamless.
        if (!chPunct[c] && c + 1 < nCh) {
            unsigned ts = slen;
            while (ts > 0) { int a = g_ulaw[ub[ts-1]]; if (a < 0) a = -a; if (a >= SIL) break; ts--; }
            if (ts + 120 < slen) slen = ts + 120;       // keep ~15 ms juncture
        }

        unsigned nWords = 0;
        if (wantWords) {
            // tokenize the chunk's words (track source offset + wbuf index)
            for (size_t i = cs; i < cs+cl && nWords < MAXW; ) {
                while (i < cs+cl && iswspace(wbuf[i])) i++;
                if (i >= cs+cl) break;
                size_t st = i; unsigned alnum = 0; bool istag = (wbuf[i] == L'\\');
                while (i < cs+cl && !iswspace(wbuf[i])) { if (iswalnum(wbuf[i])) alnum++; i++; }
                if (alnum > 0 && !istag) { wPos[nWords]=srcoff[st]; wLen[nWords]=(ULONG)(i-st); wBuf[nWords]=(ULONG)st; wWt[nWords]=alnum; nWords++; }
            }
            // weight each word by its actual synthesized duration (robust to expansion)
            for (unsigned k = 0; k < nWords; k++) {
                size_t bs=wBuf[k], bl=wLen[k];
                wchar_t sv = wbuf[bs+bl]; wbuf[bs+bl] = 0;
                int ab = WideCharToMultiByte(CP_ACP,0,wbuf+bs,-1,NULL,0,NULL,NULL);
                char* asw = (char*)HeapAlloc(heap,0,ab>0?ab:1);
                if (asw) {
                    WideCharToMultiByte(CP_ACP,0,wbuf+bs,-1,asw,ab,NULL,NULL);
                    void* wbf=NULL; unsigned wn=0, sp=0;
                    EnterCriticalSection(&g_avLock);
                    unsigned wr=g_synth(asw,&wbf,&wn,0);
                    if (wr==0 && wbf){ const unsigned char* wp=(const unsigned char*)wbf; for(unsigned i=0;i<wn;i++){int a=g_ulaw[wp[i]];if(a<0)a=-a;if(a>=SIL)sp++;} g_free(&wbf); }
                    LeaveCriticalSection(&g_avLock);
                    HeapFree(heap,0,asw);
                    wWt[k] = sp ? sp : 1;
                }
                wbuf[bs+bl] = sv;
            }
            // distribute words across the chunk's SPEECH time (silence doesn't advance)
            unsigned totalSpeech=0; for(unsigned i=0;i<slen;i++){int a=g_ulaw[ub[i]];if(a<0)a=-a;if(a>=SIL)totalSpeech++;}
            unsigned totalWt=0; for(unsigned k=0;k<nWords;k++) totalWt+=wWt[k]; if(!totalWt)totalWt=1;
            unsigned ii=0, ss=0, cum=0;
            for (unsigned k=0;k<nWords;k++){ unsigned target=(unsigned)((unsigned long long)totalSpeech*cum/totalWt);
                while(ii<slen && ss<target){int a=g_ulaw[ub[ii]];if(a<0)a=-a;if(a>=SIL)ss++;ii++;}
                wSamp[k]=ii; cum+=wWt[k]; }
        }

        // ---- 4) PCM16 out; fire word events at (audioBase + word offset) ----
        unsigned off=0, nextW=0;
        while (off < slen) {
            if (site->GetActions() & SPVES_ABORT) break;
            unsigned cend = (slen-off > PCH) ? off+PCH : slen;
            while (wantWords && nextW < nWords && wSamp[nextW] < cend) {
                SPEVENT ev = {};
                ev.eEventId             = SPEI_WORD_BOUNDARY;
                ev.elParamType          = SPET_LPARAM_IS_UNDEFINED;
                ev.ullAudioStreamOffset = audioBase + (ULONGLONG)wSamp[nextW] * sizeof(short);
                ev.wParam               = (WPARAM)wLen[nextW];
                ev.lParam               = (LPARAM)wPos[nextW];
                site->AddEvents(&ev, 1);
                nextW++;
            }
            unsigned n = cend - off; const unsigned char* u = ub + off;
            for (unsigned k=0;k<n;k++){ int v=g_ulaw[u[k]]; if(vol!=100)v=(v*vol)/100; pcm[k]=(short)v; }
            ULONG wrn=0; hr = site->Write(pcm, n*sizeof(short), &wrn);
            if (FAILED(hr)) break;
            off += n;
        }
        audioBase += (ULONGLONG)slen * sizeof(short);
        EnterCriticalSection(&g_avLock); g_free(&sbuf); LeaveCriticalSection(&g_avLock);
    }

    HeapFree(heap,0,wbuf);  HeapFree(heap,0,srcoff);
    HeapFree(heap,0,chStart); HeapFree(heap,0,chLen); HeapFree(heap,0,chPunct);
    HeapFree(heap,0,wPos);  HeapFree(heap,0,wLen);  HeapFree(heap,0,wBuf);
    HeapFree(heap,0,wWt);   HeapFree(heap,0,wSamp);
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
