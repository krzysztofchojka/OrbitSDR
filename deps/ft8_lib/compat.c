/* OrbitSDR: stpcpy() is missing from some C libraries (MinGW). The library is
   compiled with -Dstpcpy=ftx_stpcpy (see scripts/ft8lib.sh). */
#include <string.h>
char* ftx_stpcpy(char* dst, const char* src)
{
    const size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}
