/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Build configuration every library source that depends on it includes first. The options are
 * compile definitions set by CMakeLists.txt; a build of these sources without it must set them
 * too, or the files would each fall back on a different default and mix behaviours (mbelib-neo's
 * synthesis with the AMBE-3000R's tone laws, mbelib's AMBE+2 bit order). */
#ifndef MBEVOCODER_INTERNAL_CONFIG_H
#define MBEVOCODER_INTERNAL_CONFIG_H

#ifndef MBEVOCODER_CHIP_MEASURED
#error "MBEVOCODER_CHIP_MEASURED is not defined: build with CMakeLists.txt, or define it 1 (the AMBE-3000R-measured library) or 0 (bit-exact with mbelib-neo)"
#endif

#endif
