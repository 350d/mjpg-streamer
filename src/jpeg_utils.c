/*******************************************************************************
# Linux-UVC streaming input-plugin for MJPG-streamer                           #
#                                                                              #
# This package work with the Logitech UVC based webcams with the mjpeg feature #
#                                                                              #
#   Orginally Copyright (C) 2005 2006 Laurent Pinchart &&  Michel Xhaard       #
#   Modifications Copyright (C) 2006  Gabriel A. Devenyi                       #
#   Modifications Copyright (C) 2007  Tom Stöveken                             #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
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
#include <dlfcn.h>
#include <string.h>
#include <math.h>
#include "utils.h"

/* V4L2 format constants */
#define V4L2_PIX_FMT_MJPEG   0x47504A4D  /* Motion-JPEG */
#define V4L2_PIX_FMT_JPEG    0x4745504A  /* JFIF JPEG */
#define V4L2_PIX_FMT_YUYV    0x56595559  /* YUV 4:2:2 */
#define V4L2_PIX_FMT_UYVY    0x59565955  /* YUV 4:2:2 */
#define V4L2_PIX_FMT_RGB24   0x33424752  /* RGB 24-bit */
#define V4L2_PIX_FMT_BGR24   0x33524742  /* BGR 24-bit */

/* CRITICAL: We use ONLY TurboJPEG - no libjpeg fallback */
/* If TurboJPEG is not available, the build will fail or runtime will abort */
#ifndef HAVE_TURBOJPEG
#error "TurboJPEG is REQUIRED! Please install libturbojpeg-dev package. This project does not support libjpeg fallback."
#endif

#include <turbojpeg.h>
#define JPEG_LIBRARY_TURBO 1

/* Linux-specific includes */
#ifdef __linux__
    #include <linux/types.h>          /* for videodev2.h */
    #include <linux/videodev2.h>
    #include "plugins/input_uvc/v4l2uvc.h"
#endif

/* CRITICAL: We use ONLY TurboJPEG - no libjpeg fallback */
/* Cached TurboJPEG handles for performance */
static tjhandle cached_decompress_handle = NULL;
static tjhandle cached_compress_handle = NULL;

/* Forward declarations for cached handle functions */
static tjhandle get_cached_decompress_handle(void);
static tjhandle get_cached_compress_handle(void);


/* CRITICAL: We use ONLY TurboJPEG - no libjpeg fallback */
#ifdef __linux__
/******************************************************************************
Description.: TurboJPEG implementation of compress_image_to_jpeg
Input Value.: video structure from v4l2uvc.c/h, destination buffer and buffersize
Return Value: the buffer will contain the compressed data
******************************************************************************/
int compress_image_to_jpeg(struct vdIn *vd, unsigned char *buffer, int size, int quality)
{
    tjhandle handle = NULL;
    unsigned char *rgb_buffer = NULL;
    unsigned long jpeg_size = 0;
    int result = -1;
    
    /* Create TurboJPEG handle */
    handle = tjInitCompress();
    if (!handle) {
        printf("TurboJPEG: tjInitCompress() failed\n");
        return -1;
    }
    
    /* Convert YUV to RGB if needed */
    if (vd->formatIn == V4L2_PIX_FMT_YUYV || vd->formatIn == V4L2_PIX_FMT_UYVY) {
        /* Allocate RGB buffer */
        rgb_buffer = malloc(vd->width * vd->height * 3);
        if (!rgb_buffer) {
            /* Don't destroy cached handle */
            return -1;
        }
        
        /* Convert YUV to RGB (simplified conversion) */
        unsigned char *yuv = vd->framebuffer;
        unsigned char *rgb = rgb_buffer;
        int i;
        for (i = 0; i < vd->width * vd->height; i++) {
            int y = yuv[i * 2];
            int u = yuv[i * 2 + 1] - 128;
            int v = yuv[(i * 2 + 2) % (vd->width * vd->height * 2)] - 128;
            
            int r = y + (int)(1.402 * v);
            int g = y - (int)(0.344 * u) - (int)(0.714 * v);
            int b = y + (int)(1.772 * u);
            
            rgb[i * 3] = (r < 0) ? 0 : (r > 255) ? 255 : r;
            rgb[i * 3 + 1] = (g < 0) ? 0 : (g > 255) ? 255 : g;
            rgb[i * 3 + 2] = (b < 0) ? 0 : (b > 255) ? 255 : b;
        }
        
        /* Compress RGB to JPEG */
        unsigned char *jpeg_buffer = NULL;
        unsigned long jpeg_size_long = 0;
        if (compress_rgb_to_jpeg(rgb_buffer, vd->width, vd->height, quality,
                                 &jpeg_buffer, &jpeg_size_long) == 0) {
            if (jpeg_size_long <= (unsigned long)size) {
                memcpy(buffer, jpeg_buffer, jpeg_size_long);
                jpeg_size = (int)jpeg_size_long;
                result = 0;
            }
            free(jpeg_buffer);
        }
        
        free(rgb_buffer);
    } else if (vd->formatIn == V4L2_PIX_FMT_MJPEG || vd->formatIn == V4L2_PIX_FMT_JPEG) {
        /* Already JPEG - just copy */
        if (vd->framebuffer && vd->sizeIn > 0 && vd->sizeIn <= size) {
            memcpy(buffer, vd->framebuffer, vd->sizeIn);
            jpeg_size = vd->sizeIn;
            result = 0;
        }
    }
    
    tjDestroy(handle);
    return result;
}
#endif

/******************************************************************************
Description.: Decompress JPEG to grayscale with scaling
Input Value.: JPEG data, size, scale factor, output pointers
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height, int known_width, int known_height)
{
    /* Validate arguments */
    if (!jpeg_data || jpeg_size <= 0 || !gray_data || !width || !height) {
        return -1;
    }
    
    /* Use TurboJPEG 2.x API */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    int result = -1;
    
    /* Get cached decompress handle */
    handle = get_cached_decompress_handle();
    if (!handle) {
        return -1;
    }
    
    /* Get dimensions and subsampling from JPEG header */
    int jpeg_width = 0, jpeg_height = 0, subsamp = 0;
    if (tjDecompressHeader2(handle, jpeg_data, jpeg_size, &jpeg_width, &jpeg_height, &subsamp) != 0) {
        return -1;
    }
    
    /* Use provided dimensions if available, otherwise use JPEG dimensions */
    int target_width = (known_width > 0) ? known_width : jpeg_width;
    int target_height = (known_height > 0) ? known_height : jpeg_height;
    
    /* Apply scale factor */
    if (scale_factor > 0) {
        target_width /= scale_factor;
        target_height /= scale_factor;
    }
    
    /* Allocate output buffer */
    output_data = malloc(target_width * target_height);
    if (!output_data) {
        return -1;
    }
    
    /* Decompress to grayscale */
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, target_width, 0, target_height, TJPF_GRAY, 0);
    
    if (result == 0) {
        *gray_data = output_data;
        *width = target_width;
        *height = target_height;
    } else {
        free(output_data);
    }
    
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Decompress JPEG to Y component (grayscale)
Input Value.: JPEG data, size, scale factor, output pointers
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decode_to_y_component(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **y_data, int *width, int *height, int known_width, int known_height)
{
    /* Validate arguments */
    if (!jpeg_data || jpeg_size <= 0 || !y_data || !width || !height) {
        return -1;
    }
    
    /* Use TurboJPEG 2.x API */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    int result = -1;
    
    /* Get cached decompress handle */
    handle = get_cached_decompress_handle();
    if (!handle) {
        return -1;
    }
    
    /* Get dimensions and subsampling from JPEG header */
    int jpeg_width = 0, jpeg_height = 0, subsamp = 0;
    if (tjDecompressHeader2(handle, jpeg_data, jpeg_size, &jpeg_width, &jpeg_height, &subsamp) != 0) {
        return -1;
    }
    
    /* Use provided dimensions if available, otherwise use JPEG dimensions */
    int target_width = (known_width > 0) ? known_width : jpeg_width;
    int target_height = (known_height > 0) ? known_height : jpeg_height;
    
    /* Apply scale factor */
    if (scale_factor > 0) {
        target_width /= scale_factor;
        target_height /= scale_factor;
    }
    
    /* Allocate output buffer */
    output_data = malloc(target_width * target_height);
    if (!output_data) {
        return -1;
    }
    
    /* Decompress to grayscale (Y component) */
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, target_width, 0, target_height, TJPF_GRAY, 0);
    
    if (result == 0) {
        *y_data = output_data;
        *width = target_width;
        *height = target_height;
    } else {
        free(output_data);
    }
    
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Decode any format to Y component
Input Value.: data, size, scale factor, output pointers, known dimensions and format
Return Value: 0 if ok, -1 on error
******************************************************************************/
int decode_any_to_y_component(unsigned char *data, int data_size, int scale_factor,
                              unsigned char **y_data, int *width, int *height, int known_width, int known_height, int known_format)
{
    /* Validate arguments */
    if (!data || data_size <= 0 || !y_data || !width || !height) {
        return -1;
    }
    
    /* Check if data is JPEG */
    if (data_size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        /* JPEG data - use JPEG decoder */
        return jpeg_decode_to_y_component(data, data_size, scale_factor, y_data, width, height, known_width, known_height);
    }
    
    /* For other formats, return error */
    return -1;
}

/* Global cache for quantization tables (per-frame tables stored in rtp_jpeg_frame_t) */
static uint8_t g_qt_luma[64];
static uint8_t g_qt_chroma[64];
static int     g_have_luma = 0;
static int     g_have_chroma = 0;
static int     g_qt_precision = 0; /* 0 = 8-bit, 1 = 16-bit */

/* RFC 2435 requires zigzag order for QT elements */
/* Must be declared before rtpjpeg_qt_to_zigzag function */
const uint8_t rfc2435_zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

/******************************************************************************
Description.: Convert 16-bit quantization value to 8-bit
Input Value.: v16 - 16-bit quantization value
Return Value: 8-bit value (1..255, never 0)
******************************************************************************/
static inline uint8_t qt_to_8bit(uint16_t v16)
{
    /* JPEG quant value cannot be 0 */
    if (v16 == 0) return 1;
    
    /* Convert 16->8: take upper byte with rounding */
    uint8_t v8 = (uint8_t)((v16 + 0x80) >> 8);
    if (v8 == 0) v8 = 1;
    return v8;
}

/******************************************************************************
Description.: Sanitize quantization table: replace zeros with 1
Input Value.: qt - 64 bytes quantization table (modified in place)
Return Value: None
******************************************************************************/
static void sanitize_qt_8bit(uint8_t *qt)
{
    for (int i = 0; i < 64; i++) {
        if (qt[i] == 0) qt[i] = 1;
    }
}

/******************************************************************************
Description.: Convert quantization table from natural order (DQT) to zigzag order (RFC 2435)
Input Value.: src_nat - 64 bytes from JPEG DQT (natural order)
              dst_zig - 64 bytes output buffer for RTP (zigzag order)
Return Value: None
******************************************************************************/
void rtpjpeg_qt_to_zigzag(const uint8_t *src_nat, uint8_t *dst_zig)
{
    /* src_nat — 64 bytes from JPEG DQT (natural order),
       dst_zig — 64 bytes for RTP (zigzag order) */
    for (int i = 0; i < 64; i++) {
        uint8_t val = src_nat[rfc2435_zigzag[i]];
        /* CRITICAL: Ensure no zeros in zigzag-ordered output */
        if (val == 0) {
            val = 1; /* Replace zero with 1 */
        }
        dst_zig[i] = val;
    }
}

/******************************************************************************
Description.: Extract DQT (Define Quantization Table) segments from JPEG
Input Value.: JPEG data pointer, size
Return Value: None (tables cached in global variables)
******************************************************************************/
void rtpjpeg_cache_qtables_from_jpeg(const uint8_t *p, size_t sz)
{
    /* Reset cache */
    g_have_luma = 0;
    g_have_chroma = 0;
    g_qt_precision = 0;
    
    if (!p || sz < 4) return;
    
    /* Find SOI marker */
    if (p[0] != 0xFF || p[1] != 0xD8) return; /* Not a JPEG */
    
    size_t i = 2; /* Start after SOI */
    
    /* Parse JPEG segments until SOS */
    while (i + 3 < sz) {
        /* Skip padding bytes (0xFF) */
        if (p[i] != 0xFF) {
            i++;
            continue;
        }
        
        /* Skip multiple 0xFF bytes */
        while (i < sz && p[i] == 0xFF) i++;
        if (i >= sz) break;
        
        uint8_t marker = p[i++];
        
        /* SOS reached - stop parsing */
        if (marker == 0xDA) break;
        
        /* RSTn markers - no length field */
        if (marker >= 0xD0 && marker <= 0xD7) continue;
        
        /* Need at least 2 bytes for segment length */
        if (i + 1 >= sz) break;
        
        /* Read segment length (big-endian) */
        uint16_t seglen = ((uint16_t)p[i] << 8) | (uint16_t)p[i + 1];
        i += 2;
        
        if (seglen < 2 || i + (size_t)(seglen - 2) > sz) break;
        
        /* DQT marker (0xDB) */
        if (marker == 0xDB) {
            /* Parse DQT segment - may contain multiple tables */
            size_t off = 0;
            while (off < (size_t)(seglen - 2)) {
                if (i + off >= sz) break;
                
                /* Read Pq (precision) and Tq (table ID) */
                uint8_t pq_tq = p[i + off++];
                uint8_t pq = (pq_tq >> 4) & 0x0F;  /* Precision: 0=8-bit, 1=16-bit */
                uint8_t tq = pq_tq & 0x0F;         /* Table ID: 0=luma, 1=chroma */
                
                /* Handle both 8-bit and 16-bit tables */
                if (pq == 0) {
                    /* 8-bit table: 64 bytes */
                    size_t need = 64;
                    if (off + need > (size_t)(seglen - 2)) break;
                    
                    uint8_t *dst = NULL;
                    if (tq == 0) {
                        dst = g_qt_luma;
                        g_have_luma = 1;
                    } else if (tq == 1) {
                        dst = g_qt_chroma;
                        g_have_chroma = 1;
                    }
                    
                    if (dst) {
                        /* Copy 8-bit table */
                        memcpy(dst, p + i + off, 64);
                        /* CRITICAL: Sanitize immediately after copy - ensure no zeros */
                        for (int k = 0; k < 64; k++) {
                            if (dst[k] == 0) dst[k] = 1;
                        }
                        /* Sanitize: replace zeros with 1 (double check) */
                        sanitize_qt_8bit(dst);
                    }
                    off += need;
                } else {
                    /* 16-bit table: 128 bytes -> convert to 8-bit */
                    size_t need = 128;
                    if (off + need > (size_t)(seglen - 2)) break;
                    
                    uint8_t *dst = NULL;
                    if (tq == 0) {
                        dst = g_qt_luma;
                        g_have_luma = 1;
                    } else if (tq == 1) {
                        dst = g_qt_chroma;
                        g_have_chroma = 1;
                    }
                    
                    if (dst) {
                        /* Convert 16-bit to 8-bit: read 16-bit values and normalize */
                        const uint8_t *src_16bit = p + i + off;
                        for (int k = 0; k < 64; k++) {
                            uint16_t v16 = ((uint16_t)src_16bit[2*k] << 8) | (uint16_t)src_16bit[2*k + 1];
                            dst[k] = qt_to_8bit(v16);
                            /* CRITICAL: Ensure no zeros after conversion */
                            if (dst[k] == 0) dst[k] = 1;
                        }
                        /* Sanitize: replace zeros with 1 (shouldn't be any after qt_to_8bit, but just in case) */
                        sanitize_qt_8bit(dst);
                    }
                    g_qt_precision = 1; /* Remember that source had 16-bit */
                    off += need;
                }
            }
        }
        
        /* Move to next segment */
        i += (seglen - 2);
    }
}

/******************************************************************************
Description.: Get cached quantization tables
Input Value.: Output pointers for luma/chroma tables and flags
Return Value: 0 on success, -1 if no tables cached
******************************************************************************/
int rtpjpeg_get_cached_qtables(const uint8_t **luma, const uint8_t **chroma,
                                int *have_luma, int *have_chroma, int *precision)
{
    if (!luma || !chroma || !have_luma || !have_chroma || !precision) return -1;
    
    *luma = g_have_luma ? g_qt_luma : NULL;
    *chroma = g_have_chroma ? g_qt_chroma : NULL;
    *have_luma = g_have_luma;
    *have_chroma = g_have_chroma;
    *precision = g_qt_precision;
    
    return (g_have_luma || g_have_chroma) ? 0 : -1;
}

/******************************************************************************
Description.: Decompress JPEG to RGB
Input Value.: JPEG data, size, output pointers for RGB data and dimensions
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height, int known_width, int known_height)
{
    /* Validate arguments */
    if (!jpeg_data || jpeg_size <= 0 || !rgb_data || !width || !height) {
        return -1;
    }
    
    /* Use TurboJPEG 2.x API */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    int result = -1;
    
    /* Get cached decompress handle */
    handle = get_cached_decompress_handle();
    if (!handle) {
        return -1;
    }
    
    /* Use provided dimensions - no need to parse JPEG header again */
    /* Dimensions are already known from global metadata */
    *width = known_width;
    *height = known_height;
    
    /* Allocate output buffer */
    output_data = malloc(*width * *height * 3);
    if (!output_data) {
        /* Don't destroy cached handle */
        return -1;
    }
    
    /* Decompress to RGB */
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, *width, 0, *height, TJPF_RGB, 0);
    
    if (result == 0) {
        *rgb_data = output_data;
    } else {
        free(output_data);
    }
    
    /* Don't destroy cached handle */
    return (result == 0) ? 0 : -1;
}

/* CRITICAL: We use ONLY TurboJPEG - no libjpeg fallback */
/* These functions are no longer needed - TurboJPEG is required at compile time */

/******************************************************************************
Description.: Compress RGB data to JPEG using TurboJPEG or libjpeg
Input Value.: RGB data, dimensions, quality, output pointers for JPEG data and size
Return Value: 0 if ok, -1 on error
******************************************************************************/
int compress_rgb_to_jpeg(unsigned char *rgb_data, int width, int height, int quality,
                        unsigned char **jpeg_data, unsigned long *jpeg_size)
{
    /* Validate arguments */
    if (!rgb_data || width <= 0 || height <= 0 || quality < 1 || quality > 100 || !jpeg_data || !jpeg_size) {
        return -1;
    }
    
    /* Use TurboJPEG 2.x API */
    /* In TurboJPEG 2.x, we need to ensure we're not using YUV path and not reusing handles */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    unsigned long output_size = 0;
    int result = -1;
    
    /* CRITICAL: Create a NEW handle for each compression to avoid subsamp contamination */
    /* Reusing handles with different subsamp can cause TurboJPEG to ignore TJSAMP_422 */
    handle = tjInitCompress();
    if (!handle) {
        return -1;
    }
    
    /* CRITICAL: Force 4:2:2 subsampling - use TJSAMP_422 and verify result */
    /* 4:2:2 is optimal for video compression and prevents format switching between frames */
    /* TurboJPEG 2.x may ignore TJSAMP_422 if handle was previously used with different subsamp */
    /* CRITICAL: Use RGB input (TJPF_RGB), NOT YUV - TurboJPEG takes subsamp from YUV buffer if used */
    /* Use 0 flags for clean baseline JPEG */
    result = tjCompress2(handle, rgb_data, width, 0, height, TJPF_RGB, 
                        &output_data, &output_size, TJSAMP_422, quality, 
                        0);
    
    if (result == 0) {
        *jpeg_data = output_data;
        *jpeg_size = output_size;
    }
    
    /* CRITICAL: Always destroy handle after use to prevent subsamp contamination */
    /* Reusing handles can cause TurboJPEG to ignore TJSAMP_422 on subsequent calls */
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Strip JPEG to RTP/JPEG format (RFC 2435)
Input Value.: Full JPEG (SOI...EOI), dimensions, output buffer and size
Return Value: 0 if ok, -1 on error

STEP 1: Simple version - just trim JPEG to first EOI marker
******************************************************************************/

int jpeg_strip_to_rtp(const unsigned char *jfif, size_t jfif_sz,
                     unsigned char *out, size_t *out_sz,
                     uint16_t w, uint16_t h, int subsamp)
{
    if (!jfif || !out || !out_sz || jfif_sz < 4 || w == 0 || h == 0) {
        return -1;
    }

    /* 1) Find first SOI (FF D8). If input doesn't start with SOI, resync to the first SOI. */
    size_t offset = 0;
    if (!(jfif[0] == 0xFF && jfif[1] == 0xD8)) {
        size_t i;
        for (i = 0; i + 1 < jfif_sz; ++i) {
            if (jfif[i] == 0xFF && jfif[i + 1] == 0xD8) {
                offset = i;
                break;
            }
        }
        if (offset == 0 && !(jfif[0] == 0xFF && jfif[1] == 0xD8)) {
#ifdef OPRINT
            OPRINT("[RTP WARNING] SOI not found in input JPEG\n");
#else
            printf("[RTP WARNING] SOI not found in input JPEG\n");
#endif
            return -1;
        }
    }

    const unsigned char *p = jfif + offset;
    size_t sz = jfif_sz - offset;

    /* 2) Walk through metadata segments until SOS (FF DA). */
    size_t pos = 2; /* after SOI */
    while (pos + 1 < sz) {
        if (p[pos] != 0xFF) {
            /* tolerate fill/data until next marker prefix */
            pos++;
            continue;
        }

        /* skip fill bytes FF FF ... */
        while (pos < sz && p[pos] == 0xFF) pos++;
        if (pos >= sz) break;

        unsigned char marker = p[pos++];
        if (marker == 0xDA) {
            /* SOS: start of entropy-coded scan */
            break;
        }
        if (marker == 0xD9) {
            /* EOI encountered before SOS - degenerate case, trim here */
            size_t eoi_pos = pos;
            if (eoi_pos > sz) eoi_pos = sz;
            memcpy(out, p, eoi_pos);
            *out_sz = eoi_pos;
            return 0;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            /* RSTn have no length */
            continue;
        }

        /* All other markers carry a 2-byte length (including itself) */
        if (pos + 1 >= sz) {
#ifdef OPRINT
            OPRINT("[RTP WARNING] Truncated JPEG segment header before SOS\n");
#else
            printf("[RTP WARNING] Truncated JPEG segment header before SOS\n");
#endif
            break;
        }
        uint16_t seglen = (uint16_t)((p[pos] << 8) | p[pos + 1]);
        pos += 2;

        if (seglen < 2 || pos + (size_t)(seglen - 2) > sz) {
#ifdef OPRINT
            OPRINT("[RTP WARNING] Truncated JPEG segment body before SOS (len=%u)\n", seglen);
#else
            printf("[RTP WARNING] Truncated JPEG segment body before SOS (len=%u)\n", seglen);
#endif
            break;
        }
        pos += (size_t)(seglen - 2);
    }

    /* 3) In the scan: find the first non-stuffed EOI (FF D9 not preceded by FF 00). */
    size_t i = pos;
    size_t eoi_pos = 0;
    while (i + 1 < sz) {
        if (p[i] == 0xFF) {
            unsigned char b = p[i + 1];

            if (b == 0x00) {
                /* Stuffed 0xFF data byte inside entropy data */
                i += 2;
                continue;
            }
            if (b == 0xD9) {
                /* Real EOI */
                eoi_pos = i + 2;
                break;
            }
            if (b >= 0xD0 && b <= 0xD7) {
                /* Restart markers inside scan */
                i += 2;
                continue;
            }

            /* Any other marker inside the scan: skip marker and continue searching */
            i += 2;
            continue;
        }
        i++;
    }

    /* 4) Fallbacks: if no explicit EOI found, accept trailing EOI or entire buffer. */
    if (eoi_pos == 0) {
        if (sz >= 2 && p[sz - 2] == 0xFF && p[sz - 1] == 0xD9) {
            eoi_pos = sz;
        } else {
#ifdef OPRINT
            OPRINT("[RTP WARNING] EOI not found in scan, using end of buffer as frame boundary\n");
#else
            printf("[RTP WARNING] EOI not found in scan, using end of buffer as frame boundary\n");
#endif
            eoi_pos = sz;
        }
    }

    /* 5) Copy strictly up to the first EOI (do NOT include any bytes after EOI). */
    if (eoi_pos > sz) eoi_pos = sz;
    memcpy(out, p, eoi_pos);
    *out_sz = eoi_pos;

#ifdef OPRINT
    OPRINT("[RTP INFO] JPEG sanitized: in=%zu, out=%zu, offset=%zu\n", jfif_sz, *out_sz, offset);
#else
    printf("[RTP INFO] JPEG sanitized: in=%zu, out=%zu, offset=%zu\n", jfif_sz, *out_sz, offset);
#endif
    return 0;
}

/******************************************************************************
Description.: Get cached decompress handle (performance optimization)
Input Value.: None
Return Value: TurboJPEG decompress handle
******************************************************************************/
static tjhandle get_cached_decompress_handle(void)
{
    if (!cached_decompress_handle) {
        cached_decompress_handle = tjInitDecompress();
    }
    return cached_decompress_handle;
}

/******************************************************************************
Description.: Get cached compress handle (performance optimization)
Input Value.: None
Return Value: TurboJPEG compress handle
******************************************************************************/
static tjhandle get_cached_compress_handle(void)
{
    if (!cached_compress_handle) {
        cached_compress_handle = tjInitCompress();
    }
    return cached_compress_handle;
}

/******************************************************************************
Description.: Cleanup cached TurboJPEG handles
Input Value.: None
Return Value: None
******************************************************************************/
void cleanup_turbojpeg_handles(void)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    if (cached_decompress_handle) {
        tjDestroy(cached_decompress_handle);
        cached_decompress_handle = NULL;
    }
    if (cached_compress_handle) {
        tjDestroy(cached_compress_handle);
        cached_compress_handle = NULL;
    }
}

/* Default JPEG quantization tables (quality ~75) */
const unsigned char jpeg_default_qt_luma[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};

const unsigned char jpeg_default_qt_chroma[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

/******************************************************************************
Description.: Get JPEG header info using TurboJPEG
Input Value.: JPEG data, size, output pointers for width, height, subsamp
Return Value: 0 if ok, -1 on error
******************************************************************************/
int turbojpeg_header_info(const unsigned char *jpeg_data, int jpeg_size,
                          int *width, int *height, int *subsamp)
{
    if (!jpeg_data || jpeg_size <= 0 || !width || !height || !subsamp) return -1;
    tjhandle handle = get_cached_decompress_handle();
    if (!handle) return -1;
    int rc = tjDecompressHeader2(handle, (unsigned char *)jpeg_data, (unsigned long)jpeg_size,
                                 width, height, subsamp);
    return (rc == 0) ? 0 : -1;
}

#include <stdint.h>

typedef struct {
	uint8_t hs[3], vs[3];
	uint8_t ncomp;
} jpeg_sampling_t;

static int parse_sof0_sampling(const uint8_t *p, size_t sz, jpeg_sampling_t *s)
{
	size_t i = 2;
	while (i + 3 < sz) {
		if (p[i] != 0xFF) { i++; continue; }
		while (i < sz && p[i] == 0xFF) i++;
		if (i >= sz) break;
		uint8_t m = p[i++];
		if (m == 0xDA) break; // SOS
		if (m == 0xC0) { // SOF0
			if (i + 7 >= sz) return -1;
			uint16_t seglen = (p[i] << 8) | p[i + 1];
			i += 2;
			if (seglen < 8 || i + seglen - 2 > sz) return -1;
			i += 3 + 2 + 2; // precision + height + width
			uint8_t n = p[i++];
			s->ncomp = n > 3 ? 3 : n;
			for (uint8_t k = 0; k < s->ncomp; k++) {
				if (i + 2 >= sz) return -1;
				i++; // component id
				uint8_t hv = p[i++];
				s->hs[k] = hv >> 4;
				s->vs[k] = hv & 0x0F;
				i++; // Q table id
			}
			return 0;
		} else {
			if (i + 1 >= sz) return -1;
			uint16_t seglen = (p[i] << 8) | p[i + 1];
			i += 2 + (seglen > 2 ? seglen - 2 : 0);
		}
	}
	return -1;
}

static uint8_t rtp_jpeg_type_from_sampling(const jpeg_sampling_t *s)
{
	if (s->ncomp == 3 && s->hs[0] == 2 && s->vs[0] == 1 && s->hs[1] == 1 && s->vs[1] == 1)
		return 1; // 4:2:2
	if (s->ncomp == 3 && s->hs[0] == 2 && s->vs[0] == 2 && s->hs[1] == 1 && s->vs[1] == 1)
		return 0; // 4:2:0
	if (s->ncomp == 3 && s->hs[0] == 1 && s->vs[0] == 1)
		return 2; // 4:4:4
	if (s->ncomp == 1)
		return 3; // grayscale
	return 1;
}