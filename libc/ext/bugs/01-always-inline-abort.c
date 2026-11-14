typedef unsigned long size_t;
typedef unsigned char BYTE;
typedef unsigned int U32;
typedef enum { ZSTD_fast=1,
} ZSTD_strategy;
typedef struct {
unsigned searchLog;
unsigned minMatch;
} ZSTD_compressionParameters;
typedef enum {
} ZSTD_ParamSwitch_e;
typedef unsigned FSE_CTable; size_t FSE_buildCTable(FSE_CTable* ct, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog); size_t FSE_compress_usingCTable (void* dst, size_t dstCapacity, const void* src, size_t srcSize, const FSE_CTable* ct); size_t FSE_readNCount (short* normalizedCounter,
const void* rBuffer, size_t rBuffSize, int bmi2);
typedef enum {
} SeqStore_t;
typedef struct {
BYTE const* nextSrc;
BYTE const* base;
U32 dictLimit;
} ZSTD_window_t;
typedef struct ZSTD_MatchState_t ZSTD_MatchState_t;
struct ZSTD_MatchState_t {
ZSTD_window_t window;
const ZSTD_MatchState_t* dictMatchState;
ZSTD_compressionParameters cParams;
};
typedef enum {
ZSTD_noDict = 0,
ZSTD_dictMatchState = 2,
ZSTD_dedicatedDictSearch = 3
} ZSTD_dictMode_e;
typedef enum { search_hashChain=0, search_binaryTree=1, search_rowHash=2 } searchMethod_e;
static inline __attribute__((always_inline)) __attribute__((unused)) size_t ZSTD_searchMax(
ZSTD_MatchState_t* ms,
const BYTE* ip,
const BYTE* iend,
size_t* offsetPtr,
U32 const mls,
U32 const rowLog,
searchMethod_e const searchMethod,
ZSTD_dictMode_e const dictMode)
{
if (dictMode == ZSTD_noDict) {
switch (searchMethod) { case search_hashChain: switch (mls) { case 4: return ZSTD_HcFindBestMatch_noDict_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_HcFindBestMatch_noDict_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_HcFindBestMatch_noDict_6(ms, ip, iend, offsetPtr); } break; case search_binaryTree: switch (mls) { case 4: return ZSTD_BtFindBestMatch_noDict_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_BtFindBestMatch_noDict_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_BtFindBestMatch_noDict_6(ms, ip, iend, offsetPtr); } break; case search_rowHash: switch (mls) { case 4: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_noDict_4_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_noDict_4_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_noDict_4_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 5: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_noDict_5_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_noDict_5_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_noDict_5_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 6: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_noDict_6_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_noDict_6_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_noDict_6_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; } break; } do { ((void)0), __builtin_unreachable(); } while (0);
switch (searchMethod) { case search_hashChain: switch (mls) { case 4: return ZSTD_HcFindBestMatch_extDict_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_HcFindBestMatch_extDict_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_HcFindBestMatch_extDict_6(ms, ip, iend, offsetPtr); } break; case search_binaryTree: switch (mls) { case 4: return ZSTD_BtFindBestMatch_extDict_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_BtFindBestMatch_extDict_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_BtFindBestMatch_extDict_6(ms, ip, iend, offsetPtr); } break; case search_rowHash: switch (mls) { case 4: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_extDict_4_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_extDict_4_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_extDict_4_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 5: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_extDict_5_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_extDict_5_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_extDict_5_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 6: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_extDict_6_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_extDict_6_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_extDict_6_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; } break; } do { ((void)0), __builtin_unreachable(); } while (0);
switch (searchMethod) { case search_hashChain: switch (mls) { case 4: return ZSTD_HcFindBestMatch_dictMatchState_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_HcFindBestMatch_dictMatchState_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_HcFindBestMatch_dictMatchState_6(ms, ip, iend, offsetPtr); } break; case search_binaryTree: switch (mls) { case 4: return ZSTD_BtFindBestMatch_dictMatchState_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_BtFindBestMatch_dictMatchState_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_BtFindBestMatch_dictMatchState_6(ms, ip, iend, offsetPtr); } break; case search_rowHash: switch (mls) { case 4: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dictMatchState_4_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dictMatchState_4_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dictMatchState_4_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 5: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dictMatchState_5_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dictMatchState_5_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dictMatchState_5_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 6: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dictMatchState_6_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dictMatchState_6_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dictMatchState_6_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; } break; } do { ((void)0), __builtin_unreachable(); } while (0);
switch (searchMethod) { case search_hashChain: switch (mls) { case 4: return ZSTD_HcFindBestMatch_dedicatedDictSearch_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_HcFindBestMatch_dedicatedDictSearch_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_HcFindBestMatch_dedicatedDictSearch_6(ms, ip, iend, offsetPtr); } break; case search_binaryTree: switch (mls) { case 4: return ZSTD_BtFindBestMatch_dedicatedDictSearch_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_BtFindBestMatch_dedicatedDictSearch_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_BtFindBestMatch_dedicatedDictSearch_6(ms, ip, iend, offsetPtr); } break; case search_rowHash: switch (mls) { case 4: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dedicatedDictSearch_4_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dedicatedDictSearch_4_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dedicatedDictSearch_4_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 5: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dedicatedDictSearch_5_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dedicatedDictSearch_5_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dedicatedDictSearch_5_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; case 6: switch (rowLog) { case 4: return ZSTD_RowFindBestMatch_dedicatedDictSearch_6_4(ms, ip, iend, offsetPtr); case 5: return ZSTD_RowFindBestMatch_dedicatedDictSearch_6_5(ms, ip, iend, offsetPtr); case 6: return ZSTD_RowFindBestMatch_dedicatedDictSearch_6_6(ms, ip, iend, offsetPtr); } do { ((void)0), __builtin_unreachable(); } while (0); break; } break; } do { ((void)0), __builtin_unreachable(); } while (0);
}
}
size_t ZSTD_compressBlock_lazy_generic(
ZSTD_MatchState_t* ms, SeqStore_t* seqStore,
U32 rep[3],
const void* src, size_t srcSize,
const searchMethod_e searchMethod, const U32 depth,
ZSTD_dictMode_e const dictMode)
{
const BYTE* const istart = (const BYTE*)src;
const BYTE* ip = istart;
const BYTE* const iend = istart + srcSize;
const BYTE* const base = ms->window.base;
const U32 prefixLowestIndex = ms->window.dictLimit;
const U32 mls = (((4)>(((ms->cParams.minMatch)<(6) ? (ms->cParams.minMatch) : (6))) ? (4) : (((ms->cParams.minMatch)<(6) ? (ms->cParams.minMatch) : (6)))));
const U32 rowLog = (((4)>(((ms->cParams.searchLog)<(6) ? (ms->cParams.searchLog) : (6))) ? (4) : (((ms->cParams.searchLog)<(6) ? (ms->cParams.searchLog) : (6)))));
U32 offset_1 = rep[0], offset_2 = rep[1];
const int isDMS = dictMode == ZSTD_dictMatchState;
const int isDDS = dictMode == ZSTD_dedicatedDictSearch;
const int isDxS = isDMS || isDDS;
const ZSTD_MatchState_t* const dms = ms->dictMatchState;
const BYTE* const dictBase = isDxS ? dms->window.base : ((void *)0);
const BYTE* const dictEnd = isDxS ? dms->window.nextSrc : ((void *)0);
const U32 dictIndexDelta = isDxS ?
prefixLowestIndex - (U32)(dictEnd - dictBase) :
0;
size_t offBase = (((void)0), ((void)0), (1));
const U32 repIndex = (U32)(ip - base) + 1 - offset_1;
const BYTE* repMatch = ((dictMode == ZSTD_dictMatchState || dictMode == ZSTD_dedicatedDictSearch)
&& repIndex < prefixLowestIndex) ?
dictBase + (repIndex - dictIndexDelta) :
base + repIndex;
if ((ZSTD_index_overlap_check(prefixLowestIndex, repIndex))
&& (MEM_read32(repMatch) == MEM_read32(ip)) ) {
{ size_t ofbCandidate=999999999;
size_t const ml2 = ZSTD_searchMax(ms, ip, iend, &ofbCandidate, mls, rowLog, searchMethod, dictMode);
if ( (dictMode == ZSTD_noDict)
&& (offBase) && ((offset_1>0) & (MEM_read32(ip) == MEM_read32(ip - offset_1)))) {
const BYTE* repMatch = repIndex < prefixLowestIndex ?
dictBase + (repIndex - dictIndexDelta) :
base + repIndex;
}
} } }
