// chunker.h - sentence-aware text chunking shared by the WASM entry and the native test driver.
//
// avcore infinite-loops on long, expansion-dense input in a single call, so we split into
// <=CHUNKMAX-char pieces. The split MUST land on real sentence boundaries: avcore renders each
// chunk as a self-contained utterance (full sentence-pause + sentence-level prosody at the end),
// so splitting inside a sentence - e.g. at the period in "8:15 p.m." or the colon in "8:45" -
// injects a spurious 680 ms pause and shifts the whole chunk's timing. We therefore only break at
// '.', '!', '?' that are genuine sentence ends, and fall back to clause/space only for an
// over-long single sentence.
#ifndef ACU_CHUNKER_H
#define ACU_CHUNKER_H

#define ACU_CHUNKMAX 200

static int acu_is_ws(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; }

// Is text[i] a real sentence terminator (not an abbreviation period or a decimal point)?
static int acu_is_sentence_end(const char* t, int i, int n){
    char c = t[i];
    if(c!='.' && c!='!' && c!='?') return 0;
    char nx = (i+1 < n) ? t[i+1] : 0;
    // "8.5", "U.S", "p.m" - terminator immediately followed by an alnum is mid-token, not an end.
    if((nx>='0'&&nx<='9')||(nx>='a'&&nx<='z')||(nx>='A'&&nx<='Z')) return 0;
    // "8:15 p.m.," - abbreviation period directly followed by a clause mark is not a sentence end.
    if(nx==','||nx==';'||nx==':') return 0;
    int j = i+1; while(j<n && acu_is_ws((char)t[j])) j++;
    if(j>=n) return 1;                                  // end of text
    char a = t[j];
    return (a>='A'&&a<='Z') || (a>='0'&&a<='9');        // next sentence opens with a capital/number
    // (a lowercase follower means the period was an abbreviation, e.g. "p.m. tonight")
}

// Fill starts[]/lens[] with up to maxch chunk descriptors; return the chunk count.
static int acu_chunk(const char* text, int* starts, int* lens, int maxch){
    int len = 0; while(text[len]) len++;
    int i = 0, nc = 0;
    while(i < len && nc < maxch){
        while(i < len && acu_is_ws(text[i])) i++;
        if(i >= len) break;
        int start = i, end;
        if(len - start <= ACU_CHUNKMAX){
            end = len;
        } else {
            int limit = start + ACU_CHUNKMAX, brk = 0;
            for(int j=start;j<limit;j++) if(acu_is_sentence_end(text,j,len)) brk=j+1;     // last real sentence end
            if(!brk) for(int j=start;j<limit;j++){char c=text[j]; if(c==','||c==';'||c==':') brk=j+1;} // clause fallback
            if(!brk) for(int j=limit;j>start;j--) if(text[j-1]==' '){ brk=j; break; }      // space fallback
            if(brk <= start) brk = limit;                                                  // single huge token
            end = brk;
        }
        starts[nc] = start; lens[nc] = end - start; nc++;
        i = end;
    }
    return nc;
}

#endif
