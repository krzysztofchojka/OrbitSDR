/* OrbitSDR: declaration of ftx_stpcpy() (compat.c). Included into every ft8_lib file
   with -include (see scripts/ft8lib.sh): the library calls stpcpy(), which is renamed
   to ftx_stpcpy because some C libraries (MinGW) lack it, and GCC 14+ refuses calls
   to undeclared functions. */
#ifndef FTX_COMPAT_H
#define FTX_COMPAT_H
char* ftx_stpcpy(char* dst, const char* src);
#endif
