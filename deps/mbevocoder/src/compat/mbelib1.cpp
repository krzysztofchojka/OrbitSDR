// SPDX-License-Identifier: GPL-3.0-or-later
// The mbelib 1.x decoder API (include/mbelib/mbelib.h): wrappers over the mbelib-neo 2.x
// functions, which take an mbe_process_result where mbelib 1.x took errs / errs2 / err_str and
// a uvquality argument.
#include <mbelib/mbelib.h>

// The wrappers call the library's functions by their real names.
#undef mbe_processAmbe2400Dataf
#undef mbe_processAmbe2400Data
#undef mbe_processAmbe3600x2400Framef
#undef mbe_processAmbe3600x2400Frame
#undef mbe_processAmbe2450Dataf
#undef mbe_processAmbe2450Data
#undef mbe_processAmbe3600x2450Framef
#undef mbe_processAmbe3600x2450Frame
#undef mbe_processImbe4400Dataf
#undef mbe_processImbe4400Data
#undef mbe_processImbe7200x4400Framef
#undef mbe_processImbe7200x4400Frame
#undef mbe_processImbe7100x4400Framef
#undef mbe_processImbe7100x4400Frame
#undef mbe_synthesizeSpeechf
#undef mbe_synthesizeSpeech

#include <cstring>

namespace {

// mbelib 1.x reported nothing but the counts and the string; a rejected call (bits other than
// 0 / 1, inconsistent counts) is rendered as silence with no errors reported.
template <typename Sample>
void finish(int status, const mbe_process_result& r, Sample* out, int* errs, int* errs2, char* err_str, bool setCounts)
{
    if (status < 0) {
        if (out) std::memset(out, 0, 160 * sizeof(Sample));
        if (setCounts) {
            if (errs) *errs = 0;
            if (errs2) *errs2 = 0;
        }
        if (err_str) err_str[0] = '\0';
        return;
    }
    if (setCounts) {
        if (errs) *errs = r.c0_errors;
        if (errs2) *errs2 = r.total_errors;
    }
    if (err_str) mbe_formatProcessResult(err_str, MBE_V1_ERR_STR_SIZE, &r);
}

// Data paths: mbelib 1.x read *errs (C0) and *errs2 (total) from the caller's FEC stage.
mbe_process_result inputCounts(const int* errs, const int* errs2)
{
    mbe_process_result r;
    mbe_initProcessResult(&r);
    int total = errs2 && *errs2 > 0 ? *errs2 : 0;
    if (total > 144) total = 144;
    int c0 = errs && *errs > 0 ? *errs : 0;
    if (c0 > total) c0 = total;
    r.c0_errors = c0;
    r.protected_errors = total - c0;
    r.total_errors = total;
    return r;
}

template <typename Fr>
const Fr* constFrame(Fr* fr) { return reinterpret_cast<const Fr*>(fr); }   // char (*)[N] -> const char (*)[N]

} // namespace

extern "C" {

#define MBE1_DATA(name, Sample, BitsT)                                                                         \
    void mbe1_##name(Sample* aout_buf, int* errs, int* errs2, char* err_str, BitsT d, mbe_parms* cur_mp,      \
                     mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int)                                    \
    {                                                                                                         \
        mbe_process_result r = inputCounts(errs, errs2);                                                      \
        const int status = mbe_##name(aout_buf, &r, d, cur_mp, prev_mp, prev_mp_enhanced);                   \
        finish(status, r, aout_buf, errs, errs2, err_str, false);                                             \
    }

#define MBE1_FRAME(name, Sample, Row, Cols)                                                                   \
    void mbe1_##name(Sample* aout_buf, int* errs, int* errs2, char* err_str, char fr[Row][Cols], char* d,     \
                     mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int)                 \
    {                                                                                                         \
        mbe_process_result r;                                                                                 \
        mbe_initProcessResult(&r);                                                                            \
        const int status = mbe_##name(aout_buf, &r, constFrame(fr), d, cur_mp, prev_mp, prev_mp_enhanced);   \
        finish(status, r, aout_buf, errs, errs2, err_str, true);                                              \
    }

MBE1_DATA(processAmbe2400Dataf, float, char*)
MBE1_DATA(processAmbe2400Data, short, char*)
MBE1_DATA(processAmbe2450Dataf, float, char*)
MBE1_DATA(processAmbe2450Data, short, char*)
MBE1_DATA(processImbe4400Dataf, float, char*)
MBE1_DATA(processImbe4400Data, short, char*)

MBE1_FRAME(processAmbe3600x2400Framef, float, 4, 24)
MBE1_FRAME(processAmbe3600x2400Frame, short, 4, 24)
MBE1_FRAME(processAmbe3600x2450Framef, float, 4, 24)
MBE1_FRAME(processAmbe3600x2450Frame, short, 4, 24)
MBE1_FRAME(processImbe7200x4400Framef, float, 8, 23)
MBE1_FRAME(processImbe7200x4400Frame, short, 8, 23)
MBE1_FRAME(processImbe7100x4400Framef, float, 7, 24)
MBE1_FRAME(processImbe7100x4400Frame, short, 7, 24)

#undef MBE1_DATA
#undef MBE1_FRAME

void mbe1_synthesizeSpeechf(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, int)
{
    mbe_synthesizeSpeechf(aout_buf, cur_mp, prev_mp);
}

void mbe1_synthesizeSpeech(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, int)
{
    mbe_synthesizeSpeech(aout_buf, cur_mp, prev_mp);
}

void mbe_printVersion(char* str)
{
    if (!str) return;
    const char* v = mbe_versionString();
    std::size_t n = std::strlen(v);
    if (n > 31) n = 31;   // mbelib 1.x callers pass char[32]
    std::memcpy(str, v, n);
    str[n] = '\0';
}

} // extern "C"
