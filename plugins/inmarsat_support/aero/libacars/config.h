/*
 *  This file is a part of libacars
 *
 *  Copyright (c) 2018-2021 Tomasz Lemiech <szpajder@gmail.com>
 */
#ifndef _CONFIG_H
#define _CONFIG_H

#define WITH_ZLIB
// #define WITH_LIBXML2
/* #undef IS_BIG_ENDIAN */

#ifndef __MINGW32__
#define HAVE_STRSEP
#endif

#ifdef __MINGW32__
/* #undef LFIND_NMEMB_SIZE_SIZE_T */
#define LFIND_NMEMB_SIZE_UINT
#else
#define LFIND_NMEMB_SIZE_SIZE_T
/* #undef LFIND_NMEMB_SIZE_UINT */
#endif

#endif // !_CONFIG_H
