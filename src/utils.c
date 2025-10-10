/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/types.h>
#endif
#include <string.h>
#include <fcntl.h>
#ifdef __linux__
#include <wait.h>
#endif
#include <time.h>
#include <limits.h>
#ifdef __linux__
#include <linux/stat.h>
#endif
#include <sys/stat.h>

/* SIMD optimization headers */
#ifdef __SSE2__
#include <emmintrin.h>
#define HAVE_SSE2 1
#else
#define HAVE_SSE2 0
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#ifndef HAVE_NEON
#define HAVE_NEON 1
#endif
#else
#ifndef HAVE_NEON
#define HAVE_NEON 0
#endif
#endif

#include "utils.h"

/******************************************************************************
Description.:
Input Value.:
Return Value:
******************************************************************************/
void daemon_mode(void)
{
    int fr = 0;

    fr = fork();
    if(fr < 0) {
        fprintf(stderr, "fork() failed\n");
        exit(1);
    }
    if(fr > 0) {
        exit(0);
    }

    if(setsid() < 0) {
        fprintf(stderr, "setsid() failed\n");
        exit(1);
    }

    fr = fork();
    if(fr < 0) {
        fprintf(stderr, "fork() failed\n");
        exit(1);
    }
    if(fr > 0) {
        fprintf(stderr, "forked to background (%d)\n", fr);
        exit(0);
    }

    umask(0);

    fr = chdir("/");
    if(fr != 0) {
        fprintf(stderr, "chdir(/) failed\n");
        exit(0);
    }

    close(0);
    close(1);
    close(2);

    open("/dev/null", O_RDWR);

    fr = dup(0);
    fr = dup(0);
}


/*
 * Common webcam resolutions with information from
 * http://en.wikipedia.org/wiki/Graphics_display_resolution
 */
static const struct {
    const char *string;
    const int width, height;
} resolutions[] = {
    { "QQVGA", 160,  120  },
    { "QCIF",  176,  144  },
    { "CGA",   320,  200  },
    { "QVGA",  320,  240  },
    { "CIF",   352,  288  },
    { "PAL",   720,  576  },
    { "VGA",   640,  480  },
    { "SVGA",  800,  600  },
    { "XGA",   1024, 768  },
    { "HD",    1280, 720  },
    { "SXGA",  1280, 1024 },
    { "UXGA",  1600, 1200 },
    { "FHD",   1920, 1280 },
};

/******************************************************************************
Description.: convienence function for input plugins
Input Value.:
Return Value:
******************************************************************************/
void parse_resolution_opt(const char * optarg, int * width, int * height) {
    int i;

    /* try to find the resolution in lookup table "resolutions" */
    for(i = 0; i < LENGTH_OF(resolutions); i++) {
        if(strcmp(resolutions[i].string, optarg) == 0) {
            *width  = resolutions[i].width;
            *height = resolutions[i].height;
            return;
        }
    }
    
    /* parse value as decimal value */
    if (sscanf(optarg, "%dx%d", width, height) != 2) {
        fprintf(stderr, "Invalid height/width '%s' specified!\n", optarg);
        exit(EXIT_FAILURE);
    }
}

void resolutions_help(const char * padding) {
    int i;
    for(i = 0; i < LENGTH_OF(resolutions); i++) {
        fprintf(stderr, "%s ", resolutions[i].string);
        if((i + 1) % 6 == 0)
            fprintf(stderr, "\n%s", padding);
    }
    fprintf(stderr, "\n%sor a custom value like the following" \
    "\n%sexample: 640x480\n", padding, padding);
}

/******************************************************************************
 * SIMD-optimized memory copy functions
 * Universal implementation for all plugins
 ******************************************************************************/

/* SIMD capability detection */
static int simd_available = 0;
static int simd_type = 0; /* 0=none, 1=SSE2, 2=NEON */

void detect_simd_capabilities(void) {
    simd_available = 0;
    simd_type = 0;
    
#if HAVE_SSE2
    /* Check for SSE2 support */
    simd_available = 1;
    simd_type = 1;
#elif HAVE_NEON
    /* Check for NEON support */
    simd_available = 1;
    simd_type = 2;
#endif
}

/* Hybrid SIMD-optimized memory copy
 * Strategy:
 * - Small blocks (<64 bytes): Use __builtin_memcpy (compiler optimizations)
 * - Large blocks (>=64 bytes): Use SIMD instructions if available
 * - Fallback: Use __builtin_memcpy for unsupported architectures
 */
void* simd_memcpy(void* dest, const void* src, size_t n) {
    /* For small blocks, use compiler-optimized memcpy */
    if (n < 64) {
        return __builtin_memcpy(dest, src, n);
    }
    
    /* For large blocks, use SIMD if available */
    if (!simd_available) {
        return __builtin_memcpy(dest, src, n);
    }
    
#if HAVE_SSE2
    if (simd_type == 1) {
        /* SSE2 optimized copy */
        char* d = (char*)dest;
        const char* s = (const char*)src;
        
        /* Copy 16-byte chunks using SSE2 */
        while (n >= 16) {
            __m128i data = _mm_loadu_si128((__m128i*)s);
            _mm_storeu_si128((__m128i*)d, data);
            d += 16;
            s += 16;
            n -= 16;
        }
        
        /* Handle remaining bytes */
        if (n > 0) {
            memcpy(d, s, n);
        }
        return dest;
    }
#endif

#if HAVE_NEON
    if (simd_type == 2) {
        /* NEON optimized copy */
        char* d = (char*)dest;
        const char* s = (const char*)src;
        
        /* Copy 16-byte chunks using NEON */
        while (n >= 16) {
            uint8x16_t data = vld1q_u8((uint8_t*)s);
            vst1q_u8((uint8_t*)d, data);
            d += 16;
            s += 16;
            n -= 16;
        }
        
        /* Handle remaining bytes */
        if (n > 0) {
            memcpy(d, s, n);
        }
        return dest;
    }
#endif

    /* Fallback to builtin memcpy */
    return __builtin_memcpy(dest, src, n);
}
