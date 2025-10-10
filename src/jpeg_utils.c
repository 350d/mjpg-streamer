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

/* Try to include TurboJPEG first, fallback to libjpeg */
#ifdef HAVE_TURBOJPEG
    #include <turbojpeg.h>
    #define JPEG_LIBRARY_TURBO 1
#elif defined(__linux__)
    #include <jpeglib.h>
    #define JPEG_LIBRARY_TURBO 0
#endif

/* Linux-specific includes */
#ifdef __linux__
    #include <linux/types.h>          /* for videodev2.h */
    #include <linux/videodev2.h>
    #include "plugins/input_uvc/v4l2uvc.h"
#endif

/* Global JPEG library state */
static int jpeg_library_detected = 0;
static int jpeg_library_type = 0;  /* 0 = libjpeg, 1 = turbojpeg */
static void *turbojpeg_handle = NULL;

#ifdef __linux__
#if JPEG_LIBRARY_TURBO
/* TurboJPEG implementation */
#else
/* libjpeg implementation */
#define OUTPUT_BUF_SIZE  4096

typedef struct {
    struct jpeg_destination_mgr pub; /* public fields */

    JOCTET * buffer;    /* start of buffer */

    unsigned char *outbuffer;
    int outbuffer_size;
    unsigned char *outbuffer_cursor;
    int *written;

} mjpg_destination_mgr;

typedef mjpg_destination_mgr * mjpg_dest_ptr;
#endif

#if !JPEG_LIBRARY_TURBO
/******************************************************************************
Description.:
Input Value.:
Return Value:
******************************************************************************/
METHODDEF(void) init_destination(j_compress_ptr cinfo)
{
    mjpg_dest_ptr dest = (mjpg_dest_ptr) cinfo->dest;

    /* Allocate the output buffer --- it will be released when done with image */
    dest->buffer = (JOCTET *)(*cinfo->mem->alloc_small)((j_common_ptr) cinfo, JPOOL_IMAGE, OUTPUT_BUF_SIZE * sizeof(JOCTET));

    *(dest->written) = 0;

    dest->pub.next_output_byte = dest->buffer;
    dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
}

/******************************************************************************
Description.: called whenever local jpeg buffer fills up
Input Value.:
Return Value:
******************************************************************************/
METHODDEF(boolean) empty_output_buffer(j_compress_ptr cinfo)
{
    mjpg_dest_ptr dest = (mjpg_dest_ptr) cinfo->dest;

    memcpy(dest->outbuffer_cursor, dest->buffer, OUTPUT_BUF_SIZE);
    dest->outbuffer_cursor += OUTPUT_BUF_SIZE;
    *(dest->written) += OUTPUT_BUF_SIZE;

    dest->pub.next_output_byte = dest->buffer;
    dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;

    return TRUE;
}

/******************************************************************************
Description.: called by jpeg_finish_compress after all data has been written.
              Usually needs to flush buffer.
Input Value.:
Return Value:
******************************************************************************/
METHODDEF(void) term_destination(j_compress_ptr cinfo)
{
    mjpg_dest_ptr dest = (mjpg_dest_ptr) cinfo->dest;
    size_t datacount = OUTPUT_BUF_SIZE - dest->pub.free_in_buffer;

    /* Write any data remaining in the buffer */
    memcpy(dest->outbuffer_cursor, dest->buffer, datacount);
    dest->outbuffer_cursor += datacount;
    *(dest->written) += datacount;
}

/******************************************************************************
Description.: Prepare for output to a stdio stream.
Input Value.: buffer is the already allocated buffer memory that will hold
              the compressed picture. "size" is the size in bytes.
Return Value: -
******************************************************************************/
GLOBAL(void) dest_buffer(j_compress_ptr cinfo, unsigned char *buffer, int size, int *written)
{
    mjpg_dest_ptr dest;

    if(cinfo->dest == NULL) {
        cinfo->dest = (struct jpeg_destination_mgr *)(*cinfo->mem->alloc_small)((j_common_ptr) cinfo, JPOOL_PERMANENT, sizeof(mjpg_destination_mgr));
    }

    dest = (mjpg_dest_ptr) cinfo->dest;
    dest->pub.init_destination = init_destination;
    dest->pub.empty_output_buffer = empty_output_buffer;
    dest->pub.term_destination = term_destination;
    dest->outbuffer = buffer;
    dest->outbuffer_size = size;
    dest->outbuffer_cursor = buffer;
    dest->written = written;
}

#ifdef __linux__
/******************************************************************************
Description.: yuv2jpeg function is based on compress_yuyv_to_jpeg written by
              Gabriel A. Devenyi.
              modified to support other formats like RGB5:6:5 by Miklós Márton
              It uses the destination manager implemented above to compress
              YUYV data to JPEG. Most other implementations use the
              "jpeg_stdio_dest" from libjpeg, which can not store compressed
              pictures to memory instead of a file.
Input Value.: video structure from v4l2uvc.c/h, destination buffer and buffersize
              the buffer must be large enough, no error/size checking is done!
Return Value: the buffer will contain the compressed data
******************************************************************************/
int compress_image_to_jpeg(struct vdIn *vd, unsigned char *buffer, int size, int quality)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *line_buffer, *yuyv;
    int z;
    static int written;

    line_buffer = calloc(vd->width * 3, 1);
    yuyv = vd->framebuffer;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    /* jpeg_stdio_dest (&cinfo, file); */
    dest_buffer(&cinfo, buffer, size, &written);

    cinfo.image_width = vd->width;
    cinfo.image_height = vd->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    z = 0;
    if (vd->formatIn == V4L2_PIX_FMT_YUYV) {
        while(cinfo.next_scanline < vd->height) {
            int x;
            unsigned char *ptr = line_buffer;


            for(x = 0; x < vd->width; x++) {
                int r, g, b;
                int y, u, v;

                if(!z)
                    y = yuyv[0] << 8;
                else
                    y = yuyv[2] << 8;
                u = yuyv[1] - 128;
                v = yuyv[3] - 128;

                r = (y + (359 * v)) >> 8;
                g = (y - (88 * u) - (183 * v)) >> 8;
                b = (y + (454 * u)) >> 8;

                *(ptr++) = (r > 255) ? 255 : ((r < 0) ? 0 : r);
                *(ptr++) = (g > 255) ? 255 : ((g < 0) ? 0 : g);
                *(ptr++) = (b > 255) ? 255 : ((b < 0) ? 0 : b);

                if(z++) {
                    z = 0;
                    yuyv += 4;
                }
            }

            row_pointer[0] = line_buffer;
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    } else if (vd->formatIn == V4L2_PIX_FMT_RGB24) {
        while(cinfo.next_scanline < vd->height) {
            int x;
            unsigned char *ptr = line_buffer;

            for(x = 0; x < vd->width; x++) {
                *(ptr++) = yuyv[0];
                *(ptr++) = yuyv[1];
                *(ptr++) = yuyv[2];
                yuyv += 3;
            }

            row_pointer[0] = line_buffer;
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    } else if (vd->formatIn == V4L2_PIX_FMT_RGB565) {
        while(cinfo.next_scanline < vd->height) {
            int x;
            unsigned char *ptr = line_buffer;

            for(x = 0; x < vd->width; x++) {
                /*
                unsigned int tb = ((unsigned char)raw[i+1] << 8) + (unsigned char)raw[i];
                r =  ((unsigned char)(raw[i+1]) & 248);
                g = (unsigned char)(( tb & 2016) >> 3);
                b =  ((unsigned char)raw[i] & 31) * 8;
                */
                unsigned int twoByte = (yuyv[1] << 8) + yuyv[0];
                *(ptr++) = (yuyv[1] & 248);
                *(ptr++) = (unsigned char)((twoByte & 2016) >> 3);
                *(ptr++) = ((yuyv[0] & 31) * 8);
                yuyv += 2;
            }

            row_pointer[0] = line_buffer;
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    }  else if (vd->formatIn == V4L2_PIX_FMT_UYVY) {
        while(cinfo.next_scanline < vd->height) {
            int x;
            unsigned char *ptr = line_buffer;


            for(x = 0; x < vd->width; x++) {
                int r, g, b;
                int y, u, v;

                if(!z)
                    y = yuyv[1] << 8;
                else
                    y = yuyv[3] << 8;
                u = yuyv[0] - 128;
                v = yuyv[2] - 128;

                r = (y + (359 * v)) >> 8;
                g = (y - (88 * u) - (183 * v)) >> 8;
                b = (y + (454 * u)) >> 8;

                *(ptr++) = (r > 255) ? 255 : ((r < 0) ? 0 : r);
                *(ptr++) = (g > 255) ? 255 : ((g < 0) ? 0 : g);
                *(ptr++) = (b > 255) ? 255 : ((b < 0) ? 0 : b);

                if(z++) {
                    z = 0;
                    yuyv += 4;
                }
            }

            row_pointer[0] = line_buffer;
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    free(line_buffer);

    return (written);
}
#endif /* !JPEG_LIBRARY_TURBO */

#if JPEG_LIBRARY_TURBO
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
            tjDestroy(handle);
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
        tjDestroy(handle);
        return -1;
    }
    
    /* Compress to JPEG */
    result = tjCompress2(handle, rgb_buffer, vd->width, 0, vd->height, 
                        TJPF_RGB, &buffer, &jpeg_size, TJSAMP_444, quality, 0);
    
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
    tjDestroy(handle);
    
    return result;
}
#endif /* JPEG_LIBRARY_TURBO */
#endif /* __linux__ */

/******************************************************************************
Description.: Get JPEG dimensions from JPEG data
Input Value.: JPEG data, size, output pointers for width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_get_dimensions(unsigned char *jpeg_data, int jpeg_size, int *width, int *height)
{
#if JPEG_LIBRARY_TURBO
    /* TurboJPEG implementation */
    tjhandle handle = NULL;
    int result = -1;
    
    handle = tjInitDecompress();
    if (!handle) {
        return -1;
    }
    
    result = tjDecompressHeader3(handle, jpeg_data, jpeg_size, width, height, NULL, NULL);
    
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
#else
    /* libjpeg implementation */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    
    /* Initialize the JPEG decompression object */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    
    /* Set input source */
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    
    /* Read header */
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    
    /* Get dimensions from header */
    *width = cinfo.image_width;
    *height = cinfo.image_height;
    
    /* Cleanup */
    jpeg_destroy_decompress(&cinfo);
    
    return 0;
#endif
}

/******************************************************************************
Description.: Validate JPEG data integrity
Input Value.: JPEG data, size
Return Value: 0 if valid, -1 if invalid
******************************************************************************/
int jpeg_validate_data(unsigned char *jpeg_data, int jpeg_size)
{
#if JPEG_LIBRARY_TURBO
    /* TurboJPEG implementation */
    tjhandle handle = NULL;
    int result = -1;
    
    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }
    
    /* Check for JPEG end marker */
    if(jpeg_size < 2 || jpeg_data[jpeg_size-2] != 0xFF || jpeg_data[jpeg_size-1] != 0xD9) {
        return -1;
    }
    
    handle = tjInitDecompress();
    if (!handle) {
        return -1;
    }
    
    result = tjDecompressHeader3(handle, jpeg_data, jpeg_size, NULL, NULL, NULL, NULL);
    
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
#else
    /* libjpeg implementation */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    
    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }
    
    /* Check for JPEG end marker */
    if(jpeg_size < 2 || jpeg_data[jpeg_size-2] != 0xFF || jpeg_data[jpeg_size-1] != 0xD9) {
        return -1;
    }

    /* Initialize the JPEG decompression object */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    /* Set input source */
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    /* Try to read header */
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Try to start decompression */
    if(!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    
    /* Clean up */
    jpeg_destroy_decompress(&cinfo);
    return 0;
#endif
}

/******************************************************************************
Description.: Decode JPEG to grayscale using libjpeg with integrated scaling
Input Value.: JPEG data, size, scale factor, output pointers for gray data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height)
{
#if JPEG_LIBRARY_TURBO
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
    
    handle = tjInitDecompress();
    if (!handle) {
        return -1;
    }
    
    /* Get dimensions */
    int orig_width, orig_height;
    result = tjDecompressHeader3(handle, jpeg_data, jpeg_size, &orig_width, &orig_height, NULL, NULL);
    if (result != 0) {
        tjDestroy(handle);
        return -1;
    }
    
    /* Calculate scaled dimensions */
    *width = orig_width / scale_factor;
    *height = orig_height / scale_factor;
    
    /* Allocate output buffer */
    output_data = malloc(*width * *height);
    if (!output_data) {
        tjDestroy(handle);
        return -1;
    }
    
    /* Decompress to grayscale with scaling */
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, *width, 0, *height, TJPF_GRAY, 0);
    
    if (result == 0) {
        *gray_data = output_data;
    } else {
        free(output_data);
    }
    
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
#else
    /* libjpeg implementation */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *output_data = NULL;
    int row_stride;

    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }

    /* Initialize the JPEG decompression object */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    /* Set input source */
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    /* Read header */
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Force grayscale output */
    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.output_components = 1;

    /* Set scaling for faster processing */
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale_factor;

    /* Start decompression */
    if(!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    *width = cinfo.output_width;
    *height = cinfo.output_height;
    row_stride = cinfo.output_width * cinfo.output_components;

    /* Allocate output buffer for scaled image */
    output_data = (unsigned char*)malloc(cinfo.output_height * row_stride);
    if(!output_data) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Read scanlines with integrated scaling */
    while(cinfo.output_scanline < cinfo.output_height) {
        row_pointer[0] = &output_data[cinfo.output_scanline * row_stride];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *gray_data = output_data;
    return 0;
#endif
}

/******************************************************************************
Description.: Decompress JPEG to RGB using libjpeg
Input Value.: JPEG data, size, output pointers for RGB data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height)
{
#if JPEG_LIBRARY_TURBO
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
    
    handle = tjInitDecompress();
    if (!handle) {
        return -1;
    }
    
    /* Get dimensions */
    result = tjDecompressHeader3(handle, jpeg_data, jpeg_size, width, height, NULL, NULL);
    if (result != 0) {
        tjDestroy(handle);
        return -1;
    }
    
    /* Allocate output buffer */
    output_data = malloc(*width * *height * 3);
    if (!output_data) {
        tjDestroy(handle);
        return -1;
    }
    
    /* Decompress to RGB */
    result = tjDecompress2(handle, jpeg_data, jpeg_size, output_data, *width, 0, *height, TJPF_RGB, 0);
    
    if (result == 0) {
        *rgb_data = output_data;
    } else {
        free(output_data);
    }
    
    tjDestroy(handle);
    return (result == 0) ? 0 : -1;
#else
    /* libjpeg implementation */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *output_data = NULL;
    int row_stride;

    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return -1;
    }

    /* Initialize the JPEG decompression object */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    /* Set input source */
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    /* Read header */
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Force RGB output */
    cinfo.out_color_space = JCS_RGB;
    cinfo.output_components = 3;

    /* Start decompression */
    if(!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    *width = cinfo.output_width;
    *height = cinfo.output_height;
    row_stride = cinfo.output_width * cinfo.output_components;

    /* Allocate output buffer */
    output_data = (unsigned char*)malloc(cinfo.output_height * row_stride);
    if(!output_data) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Read scanlines */
    while(cinfo.output_scanline < cinfo.output_height) {
        row_pointer[0] = &output_data[cinfo.output_scanline * row_stride];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *rgb_data = output_data;
    return 0;
#endif
}
#endif /* __linux__ */

/******************************************************************************
Description.: Detect available JPEG library (TurboJPEG or libjpeg)
Input Value.: None
Return Value: 0 if libjpeg, 1 if turbojpeg, -1 if none available
******************************************************************************/
int detect_jpeg_library(void)
{
    if(jpeg_library_detected) {
        return jpeg_library_type;
    }

    /* Try to load TurboJPEG dynamically */
    turbojpeg_handle = dlopen("libturbojpeg.so", RTLD_LAZY);
    if(turbojpeg_handle) {
        /* Check if we can get the version function */
        void *version_func = dlsym(turbojpeg_handle, "tjGetVersion");
        if(version_func) {
            jpeg_library_type = 1;  /* TurboJPEG available */
            jpeg_library_detected = 1;
            printf("JPEG: Using TurboJPEG library\n");
            return 1;
        }
        dlclose(turbojpeg_handle);
        turbojpeg_handle = NULL;
    }

    /* Fallback to libjpeg */
    jpeg_library_type = 0;  /* libjpeg */
    jpeg_library_detected = 1;
    printf("JPEG: Using libjpeg library\n");
    return 0;
}

/******************************************************************************
Description.: Check if JPEG library is available
Input Value.: None
Return Value: 1 if available, 0 if not
******************************************************************************/
int jpeg_library_available(void)
{
    if(!jpeg_library_detected) {
        detect_jpeg_library();
    }
    return jpeg_library_detected;
}
