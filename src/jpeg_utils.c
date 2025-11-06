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
        
        for (i = 0; i < vd->width * vd->height / 2; i++) {
            int y1, y2, u, v;
            
            if (vd->formatIn == V4L2_PIX_FMT_YUYV) {
                y1 = yuv[0]; u = yuv[1]; y2 = yuv[2]; v = yuv[3];
            } else { /* UYVY */
                u = yuv[0]; y1 = yuv[1]; v = yuv[2]; y2 = yuv[3];
            }
            
            /* Convert YUV to RGB for first pixel */
            int r1 = y1 + 1.402 * (v - 128);
            int g1 = y1 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
            int b1 = y1 + 1.772 * (u - 128);
            
            /* Convert YUV to RGB for second pixel */
            int r2 = y2 + 1.402 * (v - 128);
            int g2 = y2 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
            int b2 = y2 + 1.772 * (u - 128);
            
            /* Clamp values */
            rgb[0] = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            rgb[1] = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            rgb[2] = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;
            rgb[3] = (r2 < 0) ? 0 : (r2 > 255) ? 255 : r2;
            rgb[4] = (g2 < 0) ? 0 : (g2 > 255) ? 255 : g2;
            rgb[5] = (b2 < 0) ? 0 : (b2 > 255) ? 255 : b2;
            
            yuv += 4;
            rgb += 6;
        }
    } else if (vd->formatIn == V4L2_PIX_FMT_RGB24) {
        rgb_buffer = vd->framebuffer;
    } else {
        /* Unsupported format */
        /* Don't destroy cached handle */
        return -1;
    }
    
    /* Compress to JPEG */
    result = tjCompress2(handle, rgb_buffer, vd->width, 0, vd->height, 
                        TJPF_RGB, &buffer, &jpeg_size, TJSAMP_422, quality, 0);
    
    if (result == 0) {
        result = (int)jpeg_size;
    } else {
        printf("TurboJPEG: tjCompress2() failed\n");
        result = -1;
    }
    
    /* Cleanup */
    if (vd->formatIn == V4L2_PIX_FMT_YUYV || vd->formatIn == V4L2_PIX_FMT_UYVY) {
        free(rgb_buffer);
    }
    /* Don't destroy cached handle */
    
    return result;
}
#endif /* __linux__ */


/******************************************************************************
Description.: Validate JPEG data integrity
Input Value.: JPEG data, size
Return Value: 0 if valid, -1 if invalid
******************************************************************************/

/******************************************************************************
Description.: Decode JPEG to grayscale using libjpeg with integrated scaling
Input Value.: JPEG data, size, scale factor, output pointers for gray data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height, int known_width, int known_height)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    /* TurboJPEG implementation */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    int result = -1;
    
    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }
    
    handle = get_cached_decompress_handle();
    if (!handle) {
        return -1;
    }
    
    /* Use provided dimensions - no need to parse JPEG header again */
    /* Dimensions are already known from global metadata */
    *width = known_width / scale_factor;
    *height = known_height / scale_factor;
    
    /* Allocate output buffer */
    output_data = malloc(*width * *height);
    if (!output_data) {
        /* Don't destroy cached handle */
        return -1;
    }
    
    /* Decompress to grayscale with scaling and optimization flags */
    int flags = TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT;
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, *width, 0, *height, TJPF_GRAY, flags);
    
    if (result == 0) {
        *gray_data = output_data;
    } else {
        free(output_data);
    }
    
    /* Don't destroy cached handle */
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Decode JPEG to Y-component only (fastest for motion detection)
Input Value.: JPEG data, size, scale factor, output pointers for Y data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decode_to_y_component(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                                unsigned char **y_data, int *width, int *height, int known_width, int known_height)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    tjhandle handle = NULL;
    unsigned char *yuv_data = NULL;
    int result = -1;
    
    
    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }
    
    handle = get_cached_decompress_handle();
    if (!handle) {
        return -1;
    }
    
    /* Use provided dimensions - no need to parse JPEG header again */
    /* Dimensions are already known from global metadata */
    *width = known_width / scale_factor;
    *height = known_height / scale_factor;
    
    /* Use TurboJPEG built-in scaling for maximum performance */
    *y_data = malloc(*width * *height);
    if (!*y_data) {
        return -1;
    }
    
    /* Decompress directly to target size with optimization flags */
    int flags = TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT;
    result = tjDecompress2(handle, jpeg_data, jpeg_size, *y_data, *width, 0, *height, TJPF_GRAY, flags);
    if (result != 0) {
        free(*y_data);
        *y_data = NULL;
        return -1;
    }
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Detect data type and decode accordingly (universal decoder)
Input Value.: Data buffer, size, scale factor, output pointers
Return Value: 0 if ok, -1 on error
******************************************************************************/
/* Optimized version that accepts known dimensions - NO JPEG header parsing */
int decode_any_to_y_component(unsigned char *data, int data_size, int scale_factor,
                              unsigned char **y_data, int *width, int *height, int known_width, int known_height, int known_format)
{
    /* Use known dimensions directly - no need to parse JPEG header */
    int scaled_width = known_width / scale_factor;
    int scaled_height = known_height / scale_factor;
    
    /* Use global format metadata for reliable detection */
    if (known_format == V4L2_PIX_FMT_MJPEG || known_format == V4L2_PIX_FMT_JPEG) {
        /* This is JPEG/MJPEG data - use optimized Y-component extraction */
        return jpeg_decode_to_y_component(data, data_size, scale_factor, y_data, width, height, known_width, known_height);
    }
    
    /* Check for YUV formats using global metadata */
    if (known_format == V4L2_PIX_FMT_YUYV || known_format == V4L2_PIX_FMT_UYVY) {
        /* YUV422 data - extract Y component directly */
        /* Use global metadata dimensions instead of stupid sqrt assumption */
        *width = scaled_width;
        *height = scaled_height;
        
        if (*width > 0 && *height > 0) {
            *y_data = malloc(*width * *height);
            if (*y_data) {
                /* Y-component is at the beginning of YUV buffer - extract and scale */
                if (scale_factor == 1) {
                    /* No scaling needed - direct copy */
                    int y_size = *width * *height;
                    int copy_size = (y_size < data_size) ? y_size : data_size;
                    simd_memcpy(*y_data, data, copy_size);
                } else {
                    /* Scale Y-component from full resolution to target resolution */
                    for (int y = 0; y < *height; y++) {
                        for (int x = 0; x < *width; x++) {
                            int src_x = x * scale_factor;
                            int src_y = y * scale_factor;
                            int src_index = src_y * known_width + src_x;
                            int dst_index = y * *width + x;
                            (*y_data)[dst_index] = data[src_index];
                        }
                    }
                }
                return 0;
            }
        }
    }
    
    /* Check for RGB formats using global metadata */
    if (known_format == V4L2_PIX_FMT_RGB24 || known_format == V4L2_PIX_FMT_BGR24) {
        /* RGB data - convert to Y component */
        /* Use global metadata dimensions instead of stupid sqrt assumption */
        
        *width = scaled_width;
        *height = scaled_height;
        
        if (*width > 0 && *height > 0) {
            *y_data = malloc(*width * *height);
            if (*y_data) {
                /* Convert RGB to Y using luminance formula with scaling */
                if (scale_factor == 1) {
                    /* No scaling needed - direct conversion */
                    for (int i = 0; i < *width * *height; i++) {
                        int r = data[i * 3];
                        int g = data[i * 3 + 1];
                        int b = data[i * 3 + 2];
                        
                        /* Standard luminance formula: Y = 0.299*R + 0.587*G + 0.114*B */
                        (*y_data)[i] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
                    }
                } else {
                    /* Scale RGB to target resolution and convert to Y */
                    for (int y = 0; y < *height; y++) {
                        for (int x = 0; x < *width; x++) {
                            int src_x = x * scale_factor;
                            int src_y = y * scale_factor;
                            int src_index = (src_y * known_width + src_x) * 3;
                            int dst_index = y * *width + x;
                            
                            int r = data[src_index];
                            int g = data[src_index + 1];
                            int b = data[src_index + 2];
                            
                            /* Standard luminance formula: Y = 0.299*R + 0.587*G + 0.114*B */
                            (*y_data)[dst_index] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
                        }
                    }
                }
                return 0;
            }
        }
    }
    
    /* Unknown data type */
    return -1;
}

/******************************************************************************
Description.: Decompress JPEG to RGB using libjpeg
Input Value.: JPEG data, size, output pointers for RGB data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height, int known_width, int known_height)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    /* TurboJPEG implementation */
    tjhandle handle = NULL;
    unsigned char *output_data = NULL;
    int result = -1;
    
    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }
    
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
        fprintf(stderr, "ERROR: TurboJPEG initialization failed! TurboJPEG is REQUIRED for this project.\n");
        fprintf(stderr, "Please install libturbojpeg-dev package (on Linux) or turbojpeg (on macOS via Homebrew).\n");
        return -1;
    }
    
    /* CRITICAL: Force 4:2:2 subsampling - use TJSAMP_422 and verify result */
    /* 4:2:2 is optimal for video compression and prevents format switching between frames */
    /* TurboJPEG 2.x may ignore TJSAMP_422 if handle was previously used with different subsamp */
    /* Use TJFLAG_ACCURATEDCT to prevent TurboJPEG from optimizing subsampling */
    /* CRITICAL: Use RGB input (TJPF_RGB), NOT YUV - TurboJPEG takes subsamp from YUV buffer if used */
    result = tjCompress2(handle, rgb_data, width, 0, height, TJPF_RGB, 
                        &output_data, &output_size, TJSAMP_422, quality, 
                        TJFLAG_ACCURATEDCT);
    
    if (result == 0) {
        /* CRITICAL: Check EVERY frame for correct subsampling (4:2:2) */
        /* Format switching between frames (4:2:0 and 4:2:2) causes double SOF0 and VLC flickering */
        static int frame_count = 0;
        frame_count++;
        
        /* Check for double SOF0 markers in JPEG - this indicates corrupted frame */
        /* A valid JPEG should have only ONE SOF0 marker */
        int sof0_count = 0;
        const unsigned char *jpeg_data_ptr = output_data;
        size_t jpeg_size_val = output_size;
        size_t i = 0;
        
        /* Search for SOF0 markers (0xFF 0xC0) in JPEG data */
        while (i < jpeg_size_val - 1) {
            if (jpeg_data_ptr[i] == 0xFF && jpeg_data_ptr[i+1] == 0xC0) {
                sof0_count++;
                if (sof0_count > 1) {
                    fprintf(stderr, "CRITICAL ERROR: Frame %d: JPEG contains %d SOF0 markers (should be 1) at offset %zu! Surrounding bytes: %02X %02X %02X %02X\n", frame_count, sof0_count, i, jpeg_data_ptr[i-1], jpeg_data_ptr[i], jpeg_data_ptr[i+1], jpeg_data_ptr[i+2]);
                    fprintf(stderr, "This indicates corrupted JPEG frame assembly - double SOF0 causes 'overread 8' and VLC flickering\n");
                    tjFree(output_data);
                    return -1; /* Reject frame with double SOF0 - don't call tjDestroy since handle not created yet */
                }
            }
            i++;
        }
        
        if (sof0_count == 0) {
            fprintf(stderr, "WARNING: Frame %d: JPEG contains no SOF0 markers!\n", frame_count);
        }
        
        /* CRITICAL: Verify actual subsampling format for EVERY frame */
        /* TurboJPEG may ignore TJSAMP_422 and generate 4:2:0 instead */
        /* This causes format switching between frames (4:2:0 and 4:2:2) */
        tjhandle decompress_handle = tjInitDecompress();
        if (decompress_handle) {
            int actual_width = 0, actual_height = 0, actual_subsamp = 0;
            if (tjDecompressHeader2(decompress_handle, output_data, output_size, 
                                   &actual_width, &actual_height, &actual_subsamp) == 0) {
                /* Check if actual subsampling matches requested (TJSAMP_422 = 1) */
                if (actual_subsamp != TJSAMP_422) {
                    fprintf(stderr, "CRITICAL ERROR: Frame %d: TurboJPEG using subsampling %d instead of TJSAMP_422 (1)!\n", frame_count, actual_subsamp);
                    fprintf(stderr, "Requested: 4:2:2 (TJSAMP_422=1), Got: %s\n", 
                           actual_subsamp == TJSAMP_420 ? "4:2:0 (TJSAMP_420=0)" : 
                           actual_subsamp == TJSAMP_422 ? "4:2:2 (TJSAMP_422=1)" : 
                           actual_subsamp == TJSAMP_444 ? "4:4:4 (TJSAMP_444=2)" : "unknown");
                    fprintf(stderr, "This causes format switching between frames (4:2:0 and 4:2:2) and VLC flickering\n");
                    fprintf(stderr, "Frame %d will be rejected - this is a critical error!\n", frame_count);
                    tjDestroy(decompress_handle);
                    tjFree(output_data);
                    return -1; /* Reject frame with wrong subsampling */
                } else {
                    /* Log first 10 frames, then every 100th frame */
                    if (frame_count <= 10 || (frame_count % 100 == 0)) {
                        fprintf(stderr, "OK: Frame %d: TurboJPEG 2.x using TJSAMP_422 (4:2:2) as requested\n", frame_count);
                    }
                }
            } else {
                fprintf(stderr, "WARNING: Frame %d: Failed to read JPEG header to verify subsampling\n", frame_count);
            }
            tjDestroy(decompress_handle);
        } else {
            fprintf(stderr, "ERROR: Frame %d: Failed to initialize TurboJPEG decompress handle for verification\n", frame_count);
        }
        
        /* Assign output data - verification passed (or verification failed but we still want to use the frame) */
        *jpeg_data = output_data;
        *jpeg_size = output_size;
    } else {
        fprintf(stderr, "ERROR: TurboJPEG compression failed! TurboJPEG is REQUIRED for this project.\n");
    }
    
    /* CRITICAL: Always destroy handle after use to prevent subsamp contamination */
    /* Reusing handles can cause TurboJPEG to ignore TJSAMP_422 on subsequent calls */
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Get cached decompress handle (performance optimization)
Input Value.: None
Return Value: TurboJPEG decompress handle
******************************************************************************/
static tjhandle get_cached_decompress_handle(void)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    if (!cached_decompress_handle) {
        cached_decompress_handle = tjInitDecompress();
        if (!cached_decompress_handle) {
            fprintf(stderr, "ERROR: TurboJPEG decompress initialization failed! TurboJPEG is REQUIRED for this project.\n");
            fprintf(stderr, "Please install libturbojpeg-dev package (on Linux) or turbojpeg (on macOS via Homebrew).\n");
        }
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
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    if (!cached_compress_handle) {
        cached_compress_handle = tjInitCompress();
        if (!cached_compress_handle) {
            fprintf(stderr, "ERROR: TurboJPEG compress initialization failed! TurboJPEG is REQUIRED for this project.\n");
            fprintf(stderr, "Please install libturbojpeg-dev package (on Linux) or turbojpeg (on macOS via Homebrew).\n");
        }
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

int turbojpeg_header_info(const unsigned char *jpeg_data, int jpeg_size,
                          int *width, int *height, int *subsamp)
{
    /* CRITICAL: Use ONLY TurboJPEG - no libjpeg fallback */
    if (!jpeg_data || jpeg_size <= 0 || !width || !height || !subsamp) return -1;
    tjhandle handle = get_cached_decompress_handle();
    if (!handle) return -1;
    int rc = tjDecompressHeader2(handle, (unsigned char *)jpeg_data, (unsigned long)jpeg_size,
                                 width, height, subsamp);
    return (rc == 0) ? 0 : -1;
}

/******************************************************************************
Description.: Recompress JPEG to baseline with default Huffman tables
Input Value.: input JPEG data, size, output pointers for JPEG data and size
Return Value: 0 if ok, -1 on error
******************************************************************************/
int recompress_jpeg_to_baseline_with_default_dht(const unsigned char *input_jpeg, int input_size,
                                                 unsigned char **output_jpeg, unsigned long *output_size,
                                                 int quality, int target_subsamp)
{
    /* CRITICAL: Decompress to RGB, then recompress with baseline (default DHT) */
    /* This ensures RFC 2435 compatibility - scan data uses standard Huffman tables */
    
    if (!input_jpeg || input_size <= 0 || !output_jpeg || !output_size) return -1;
    
    /* Get dimensions and subsampling from input JPEG */
    int width = 0, height = 0, subsamp = -1;
    if (turbojpeg_header_info(input_jpeg, input_size, &width, &height, &subsamp) != 0) {
        return -1;
    }
    
    int output_subsamp = subsamp;
    if (target_subsamp >= 0) {
        output_subsamp = target_subsamp;
    }

    /* Decompress to RGB */
    unsigned char *rgb_data = NULL;
    if (jpeg_decompress_to_rgb((unsigned char *)input_jpeg, input_size, &rgb_data, 
                               &width, &height, width, height) != 0) {
        return -1;
    }
    
    /* Recompress to baseline JPEG with default DHT (no TJFLAG_OPTIMIZE) */
    /* CRITICAL: Use NEW handle for each compression to avoid subsamp contamination */
    tjhandle compress_handle = tjInitCompress();
    if (!compress_handle) {
        free(rgb_data);
        return -1;
    }
    
    unsigned char *jpeg_output = NULL;
    unsigned long jpeg_output_size = 0;
    
    /* Compress with baseline (default DHT) - NO TJFLAG_OPTIMIZE */
    /* Use 0 flags (no optimization) = default Huffman tables */
    int result = tjCompress2(compress_handle, rgb_data, width, 0, height, TJPF_RGB,
                            &jpeg_output, &jpeg_output_size, output_subsamp, quality, 0);
    
    free(rgb_data);
    tjDestroy(compress_handle);
    
    if (result == 0) {
        *output_jpeg = jpeg_output;
        *output_size = jpeg_output_size;
        return 0;
    }
    
    return -1;
}

