/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mbelib-neo installs <mbelib-neo/version.h> with MBELIB_VERSION; here those names carry
 * mbevocoder's version (<mbevocoder/version.h>). */
#ifndef MBELIB_NEO_COMPAT_VERSION_H
#define MBELIB_NEO_COMPAT_VERSION_H
#include <mbevocoder/version.h>
#define MBELIB_VERSION_MAJOR MBEVOCODER_VERSION_MAJOR
#define MBELIB_VERSION_MINOR MBEVOCODER_VERSION_MINOR
#define MBELIB_VERSION_PATCH MBEVOCODER_VERSION_PATCH
#define MBELIB_VERSION       MBEVOCODER_VERSION
#endif
