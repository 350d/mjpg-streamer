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
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <dirent.h>
#include <sys/queue.h>
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#ifdef HAVE_JPEG
#include <jpeglib.h>
#include <jerror.h>
#endif

#ifdef __linux__
#include <linux/types.h>
#include <linux/videodev2.h>
#else
// Define basic types for non-Linux systems
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
#endif

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "MOTION output plugin"

// Forward declarations
int send_webhook_notification_sync(
#ifdef HAVE_CURL
    CURL *curl_handle, 
#endif
    double motion_level, time_t timestamp);

// Webhook queue structure
struct webhook_item {
    double motion_level;
    time_t timestamp;
    TAILQ_ENTRY(webhook_item) entries;
};

// Webhook queue
TAILQ_HEAD(webhook_queue, webhook_item) webhook_head = TAILQ_HEAD_INITIALIZER(webhook_head);
pthread_mutex_t webhook_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t webhook_cond = PTHREAD_COND_INITIALIZER;
static pthread_t webhook_thread;
static int webhook_thread_running = 0;
static int webhook_in_progress = 0;  // Flag to prevent overlapping webhooks
static time_t last_webhook_time = 0; // Last webhook timestamp

// Motion detection parameters
static int scale_factor = 4;           // -d: downscale factor (default 4)
static int brightness_threshold = 5;   // -l: motion detection threshold in % (default 5%)
static int overload_threshold = 50;    // -o: overload threshold in % (default 50%)
static int check_interval = 1;         // -s: skip frame interval (default 1)
static char *save_folder = NULL;       // -f: folder to save motion frames
static char *webhook_url = NULL;       // -w: webhook URL for motion events
static int webhook_post = 0;           // -p: use POST instead of GET

// Internal state
static pthread_t worker;
static globals *pglobal = NULL;
static int input_number = 0;
static int frame_counter = 0;
static unsigned char *prev_frame = NULL;
static unsigned char *current_frame = NULL;
static int prev_width = 0, prev_height = 0;
static int scaled_width = 0, scaled_height = 0;
static time_t last_motion_time = 0;
static int motion_cooldown = 5; // seconds between motion events
static int size_threshold = 1;  // -z: size change threshold in 0.1% units (default: 0.1%)
static int prev_jpeg_size = 0;  // previous JPEG frame size for comparison
static int prev_size_threshold = 0;  // cached threshold for optimization
static int cached_abs_threshold = 0;  // cached absolute threshold for motion detection
static int cached_early_exit_threshold = 0;  // cached early exit threshold

#ifdef HAVE_JPEG
/******************************************************************************
Description.: Get JPEG dimensions from JPEG data
Input Value.: JPEG data, size, output pointers for width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
static int get_jpeg_dimensions(unsigned char *jpeg_data, int jpeg_size, int *width, int *height)
{
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
}
#else
static int get_jpeg_dimensions(unsigned char *jpeg_data, int jpeg_size, int *width, int *height)
{
    /* JPEG support not available - return error */
    return -1;
}
#endif

/******************************************************************************
Description.: Check if JPEG size changed significantly
Input Value.: current JPEG size, previous JPEG size, threshold percentage
Return Value: 1 if significant change, 0 if minimal change
******************************************************************************/
static int is_jpeg_size_changed(int current_size, int previous_size, int threshold_percent)
{
    if(previous_size == 0) {
        // First frame - always process
        return 1;
    }
    
    if(current_size == 0) {
        // Invalid size - skip processing
        return 0;
    }
    
    // Quick check: if sizes are identical, no change
    if(current_size == previous_size) {
        return 0;
    }
    
    // Calculate percentage change (multiply by 1000 for 0.1% precision)
    long long size_diff = abs(current_size - previous_size);
    long long change_percent_x10 = (size_diff * 1000LL) / previous_size;
    
    // Return 1 if change is above threshold (threshold is in 0.1% units)
    return (change_percent_x10 >= threshold_percent);
}

// CURL handle is now created per webhook thread

/******************************************************************************
Description.: print a help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-d | --downscale ].....: scale down factor (default: 4)\n" \
            " [-l | --motion ]........: motion detection threshold in %% (default: 5%%)\n" \
            " [-o | --overload ]......: overload threshold in %% (default: 50%%)\n" \
            " [-s | --skipframe ].....: check every N frames (default: 1)\n" \
            " [-f | --folder ]........: folder to save motion frames\n" \
            " [-w | --webhook ].......: webhook URL for motion events\n" \
            " [-p | --post ]..........: use POST instead of GET for webhook\n" \
            " [-c | --cooldown ]......: cooldown between events in seconds (default: 5)\n" \
            " [-n | --input ].........: read frames from the specified input plugin\n" \
            " [-z | --jpeg-size-check]: JPEG file size change threshold in 0.1%% units (default: 1 = 0.1%%)\n" \
            "                           Skip motion analysis if JPEG size change is below this threshold\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: clean up allocated resources
Input Value.: unused argument
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    OPRINT("cleaning up resources allocated by worker thread\n");

    if(prev_frame != NULL) {
        free(prev_frame);
        prev_frame = NULL;
    }
    
    if(current_frame != NULL) {
        free(current_frame);
        current_frame = NULL;
    }

    // Clean up webhook queue
    pthread_mutex_lock(&webhook_mutex);
    while(!TAILQ_EMPTY(&webhook_head)) {
        struct webhook_item *item = TAILQ_FIRST(&webhook_head);
        TAILQ_REMOVE(&webhook_head, item, entries);
        free(item);
    }
    pthread_mutex_unlock(&webhook_mutex);

#ifdef HAVE_CURL
    curl_global_cleanup();
#endif
}

/******************************************************************************
Description.: Convert RGB to grayscale and scale down
Input Value.: source buffer, destination buffer, dimensions
Return Value: -
******************************************************************************/
void convert_to_grayscale_scale(unsigned char *src, unsigned char *dst, 
                               int src_width, int src_height, int scale)
{
    int dst_width = src_width / scale;
    int dst_height = src_height / scale;
    int min_val = 255, max_val = 0;
    int total_val = 0;
    
    for(int y = 0; y < dst_height; y++) {
        for(int x = 0; x < dst_width; x++) {
            int src_x = x * scale;
            int src_y = y * scale;
            
            // Simple RGB to grayscale conversion (R*0.299 + G*0.587 + B*0.114)
            int r = src[(src_y * src_width + src_x) * 3];
            int g = src[(src_y * src_width + src_x) * 3 + 1];
            int b = src[(src_y * src_width + src_x) * 3 + 2];
            
            unsigned char gray = (unsigned char)(r * 0.299 + g * 0.587 + b * 0.114);
            dst[y * dst_width + x] = gray;
            
            // Track min/max for debugging
            if(gray < min_val) min_val = gray;
            if(gray > max_val) max_val = gray;
            total_val += gray;
        }
    }
    
    // Debug output for grayscale conversion
    double avg_val = (double)total_val / (double)(dst_width * dst_height);
    DBG("Grayscale debug: min=%d, max=%d, avg=%.2f, range=%d\n", 
        min_val, max_val, avg_val, max_val - min_val);
}

/******************************************************************************
Description.: Validate JPEG data integrity
Input Value.: JPEG data, size
Return Value: 0 if valid, -1 if invalid
******************************************************************************/
int validate_jpeg_data(unsigned char *jpeg_data, int jpeg_size)
{
#ifdef HAVE_JPEG
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
#else
    return -1;
#endif
}

/******************************************************************************
Description.: Decode JPEG to grayscale using libjpeg with integrated scaling
Input Value.: JPEG data, size, scale factor, output pointers for gray data, width, height
Return Value: 0 if ok, -1 on error
******************************************************************************/
int decode_jpeg_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                              unsigned char **gray_data, int *width, int *height)
{
#ifdef HAVE_JPEG
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *output_data = NULL;
    int row_stride;

    /* Check for valid JPEG data */
    if(jpeg_size < 4) {
        DBG("JPEG data too small: %d bytes\n", jpeg_size);
        return -1;
    }
    
    /* Check for JPEG magic bytes */
    if(jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        DBG("Invalid JPEG magic bytes: %02X %02X\n", jpeg_data[0], jpeg_data[1]);
        return -1;
    }

    /* Initialize the JPEG decompression object */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    /* Set input source */
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    /* Read header */
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        DBG("Failed to read JPEG header, size: %d bytes\n", jpeg_size);
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
        DBG("Failed to start JPEG decompression\n");
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

    /* Finish decompression */
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *gray_data = output_data;
    return 0;
#else
    return -1; // JPEG not available
#endif
}

/******************************************************************************
Description.: Convert MJPEG to grayscale and scale down
Input Value.: source buffer, destination buffer, dimensions
Return Value: -
******************************************************************************/
void convert_mjpeg_to_grayscale_scale(unsigned char *src, unsigned char *dst, 
                                     int src_width, int src_height, int scale)
{
#ifdef HAVE_JPEG
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *rgb_buffer = NULL;
    int dst_width = src_width / scale;
    int dst_height = src_height / scale;
    
    // Initialize JPEG decompression
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    
    // Set source buffer
    jpeg_mem_src(&cinfo, src, src_width * src_height);
    
    // Read JPEG header
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        LOG("Failed to read JPEG header\n");
        jpeg_destroy_decompress(&cinfo);
        return;
    }
    
    // Start decompression
    jpeg_start_decompress(&cinfo);
    
    // Allocate RGB buffer
    rgb_buffer = malloc(cinfo.output_width * cinfo.output_height * 3);
    if(rgb_buffer == NULL) {
        LOG("Failed to allocate RGB buffer\n");
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return;
    }
    
    // Read RGB data
    int row = 0;
    while(cinfo.output_scanline < cinfo.output_height) {
        row_pointer[0] = &rgb_buffer[row * cinfo.output_width * 3];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
        row++;
    }
    
    // Finish decompression
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    
    // Convert RGB to grayscale and scale down
    convert_to_grayscale_scale(rgb_buffer, dst, cinfo.output_width, cinfo.output_height, scale);
    
    free(rgb_buffer);
#else
    // Fallback: simple approximation (not accurate)
    int dst_width = src_width / scale;
    int dst_height = src_height / scale;
    
    for(int y = 0; y < dst_height; y++) {
        for(int x = 0; x < dst_width; x++) {
            int src_x = x * scale;
            int src_y = y * scale;
            
            // Simple brightness extraction from MJPEG (approximation)
            int pixel_offset = src_y * src_width + src_x;
            if(pixel_offset < src_width * src_height) {
                dst[y * dst_width + x] = src[pixel_offset] & 0xFF;
            }
        }
    }
#endif
}

/******************************************************************************
Description.: Calculate motion level between two frames
Input Value.: current frame, previous frame, dimensions
Return Value: motion level in percentage (0.0 - 100.0)
******************************************************************************/
double calculate_motion_level(unsigned char *current, unsigned char *prev, 
                            int width, int height)
{
    int total_pixels = width * height;
    int changed_pixels = 0;
    int max_diff = 0;
    int total_diff = 0;
    
    // Use cached threshold values for better performance
    int abs_threshold = cached_abs_threshold;
    // Calculate early exit threshold based on current frame dimensions
    int early_exit_threshold = (overload_threshold * total_pixels) / 100;
    
    for(int i = 0; i < total_pixels; i++) {
        int diff = abs((int)current[i] - (int)prev[i]);
        total_diff += diff;
        if(diff > max_diff) max_diff = diff;
        if(diff > abs_threshold) {
            changed_pixels++;
            // Early exit if we already know it's overloaded
            if(changed_pixels >= early_exit_threshold) {
                return overload_threshold + 1.0; // Return overloaded value
            }
        }
    }
    
    // Debug output for black frames
    if(max_diff > 0) {
        double avg_diff = (double)total_diff / (double)total_pixels;
        DBG("Motion debug: max_diff=%d, avg_diff=%.2f, changed_pixels=%d/%d (%.1f%%), threshold=%d\n", 
            max_diff, avg_diff, changed_pixels, total_pixels, 
            ((double)changed_pixels / (double)total_pixels) * 100.0, abs_threshold);
    }
    
    // Return percentage of changed pixels
    return ((double)changed_pixels / (double)total_pixels) * 100.0;
}

/******************************************************************************
Description.: Save motion frame to file
Input Value.: frame data, motion level
Return Value: 0 on success, -1 on error
******************************************************************************/
int save_motion_frame(unsigned char *frame_data, int frame_size, double motion_level)
{
    if(save_folder == NULL) {
        return 0; // No save folder specified
    }
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/%04d%02d%02d_%02d%02d%02d_motion_%.1f%%.jpg",
             save_folder,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             motion_level);
    
    /* JPEG data already validated before motion analysis */
    FILE *file = fopen(filename, "wb");
    if(file == NULL) {
        OPRINT("could not create motion frame file %s\n", filename);
        return -1;
    }
    
    fwrite(frame_data, 1, frame_size, file);
    fclose(file);
    
    OPRINT("motion frame saved: %s (level: %.1f%%)\n", filename, motion_level);
    return 0;
}

/******************************************************************************
Description.: Add webhook notification to queue (async)
Input Value.: motion level
Return Value: 0 on success, -1 on error
******************************************************************************/
int send_webhook_notification_async(double motion_level)
{
    if(webhook_url == NULL) {
        return 0; // No webhook URL specified
    }
    
    time_t now = time(NULL);
    
    pthread_mutex_lock(&webhook_mutex);
    
    // Check if webhook is already in progress
    if(webhook_in_progress) {
        pthread_mutex_unlock(&webhook_mutex);
        DBG("webhook already in progress, skipping notification\n");
        return 0;
    }
    
    // Check if we sent webhook recently (within cooldown period)
    if(now - last_webhook_time < motion_cooldown) {
        pthread_mutex_unlock(&webhook_mutex);
        DBG("webhook cooldown active, skipping notification\n");
        return 0;
    }
    
    // Create webhook item
    struct webhook_item *item = malloc(sizeof(struct webhook_item));
    if(item == NULL) {
        pthread_mutex_unlock(&webhook_mutex);
        LOG("Failed to allocate memory for webhook item\n");
        return -1;
    }
    
    item->motion_level = motion_level;
    item->timestamp = now;
    
    // Add to queue and mark as in progress
    TAILQ_INSERT_TAIL(&webhook_head, item, entries);
    webhook_in_progress = 1;
    last_webhook_time = now;
    
    pthread_cond_signal(&webhook_cond);
    pthread_mutex_unlock(&webhook_mutex);
    
    return 0;
}

/******************************************************************************
Description.: Webhook worker thread
Input Value.: thread parameter
Return Value: NULL
******************************************************************************/
void* webhook_worker_thread(void *param)
{
#ifdef HAVE_CURL
    CURL *curl_handle = NULL;
    
    curl_handle = curl_easy_init();
    if(curl_handle == NULL) {
        LOG("Failed to initialize CURL for webhook thread\n");
        return NULL;
    }
    
    // Set CURL options
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "mjpg-streamer-motion/1.0");
#endif
    
    while(webhook_thread_running) {
        struct webhook_item *item = NULL;
        
        // Wait for webhook items
        pthread_mutex_lock(&webhook_mutex);
        while(TAILQ_EMPTY(&webhook_head) && webhook_thread_running) {
            pthread_cond_wait(&webhook_cond, &webhook_mutex);
        }
        
        if(!TAILQ_EMPTY(&webhook_head)) {
            item = TAILQ_FIRST(&webhook_head);
            TAILQ_REMOVE(&webhook_head, item, entries);
        }
        pthread_mutex_unlock(&webhook_mutex);
        
        if(item != NULL) {
            // Send webhook
#ifdef HAVE_CURL
            send_webhook_notification_sync(curl_handle, item->motion_level, item->timestamp);
#endif
            free(item);
            
            // Mark webhook as completed
            pthread_mutex_lock(&webhook_mutex);
            webhook_in_progress = 0;
            pthread_mutex_unlock(&webhook_mutex);
        }
    }
    
#ifdef HAVE_CURL
    if(curl_handle != NULL) {
        curl_easy_cleanup(curl_handle);
    }
#endif
    
    return NULL;
}

/******************************************************************************
Description.: Send webhook notification (sync - called from worker thread)
Input Value.: curl handle, motion level, timestamp
Return Value: 0 on success, -1 on error
******************************************************************************/
int send_webhook_notification_sync(
#ifdef HAVE_CURL
    CURL *curl_handle, 
#endif
    double motion_level, time_t timestamp)
{
#ifndef HAVE_CURL
    return 0; // CURL not available
#else
    if(webhook_url == NULL || curl_handle == NULL) {
        return 0; // No webhook URL specified
    }
    
    char timestamp_str[64];
    struct tm *tm_info = localtime(&timestamp);
    strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // HTTPS support removed - only HTTP webhooks supported
    
    if(webhook_post) {
        // POST request - prepare data
        char data[512];
        snprintf(data, sizeof(data),
                 "timestamp=%s&motion_level=%.1f",
                 timestamp_str, motion_level);
        
        curl_easy_setopt(curl_handle, CURLOPT_URL, webhook_url);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, data);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(data));
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, NULL);
    } else {
        // GET request - use original URL as provided by user
        curl_easy_setopt(curl_handle, CURLOPT_URL, webhook_url);
        curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
    }
    
    CURLcode res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        OPRINT("webhook %s request failed: %s\n", 
               webhook_post ? "POST" : "GET", curl_easy_strerror(res));
        return -1;
    }
    
    OPRINT("webhook %s notification sent (motion level: %.1f%%)\n", 
           webhook_post ? "POST" : "GET", motion_level);
    return 0;
#endif
}

/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs frames and detects motion
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    int ok = 1, frame_size = 0, prev_frame_size = 0;
    unsigned char *scaled_frame = NULL;
    double motion_level = 0.0;

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(ok >= 0 && !pglobal->stop) {
        DBG("waiting for fresh frame\n");

        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db);

        /* read buffer */
        frame_size = pglobal->in[input_number].size;
        
        /* check if frame size is within reasonable limits */
        if(frame_size > 10 * 1024 * 1024) { // 10MB limit
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            DBG("frame size too large: %d bytes, skipping\n", frame_size);
            continue;
        }
        
        /* allocate buffer for frame copy */
        if(current_frame == NULL || prev_frame_size != frame_size) {
            if(current_frame != NULL) free(current_frame);
            current_frame = malloc(frame_size);
            if(current_frame == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory for frame buffer\n");
                break;
            }
            prev_frame_size = frame_size;
        }
        
        /* copy frame to our local buffer */
        memcpy(current_frame, pglobal->in[input_number].buf, frame_size);

        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        frame_counter++;

        /* Check if we should process this frame */
        if(frame_counter % check_interval != 0) {
            continue;
        }

        /* Check if JPEG size changed significantly - early optimization */
        if(!is_jpeg_size_changed(frame_size, prev_jpeg_size, size_threshold)) {
            DBG("JPEG size change below threshold (%d%%), skipping motion analysis\n", size_threshold);
            continue;
        }
        
        /* Update previous JPEG size for next comparison */
        prev_jpeg_size = frame_size;

        /* Initialize scaled frame buffer if needed */
        if(scaled_frame == NULL) {
            // Get actual width and height from JPEG header (like QR plugin does)
            int width, height;
            if(get_jpeg_dimensions(pglobal->in[input_number].buf, pglobal->in[input_number].size, &width, &height) < 0) {
                LOG("failed to get JPEG dimensions\n");
                continue;
            }
            
            scaled_width = width / scale_factor;
            scaled_height = height / scale_factor;
            scaled_frame = malloc(scaled_width * scaled_height);
            if(scaled_frame == NULL) {
                LOG("not enough memory for scaled frame\n");
                break;
            }
        }

        /* Validate JPEG data before analysis */
        if(validate_jpeg_data(current_frame, frame_size) < 0) {
            DBG("skipping corrupted JPEG frame for motion analysis, frame_size: %d\n", frame_size);
            continue;
        }

        /* Convert current frame to grayscale with integrated scaling */
        // Decode JPEG directly to scaled grayscale data
        unsigned char *gray_data = NULL;
        int width, height;
        
        if(decode_jpeg_to_gray_scaled(current_frame, frame_size, scale_factor, &gray_data, &width, &height) < 0) {
            DBG("failed to decode JPEG for motion detection, frame_size: %d\n", frame_size);
            continue;
        }
        
        // Use the already scaled dimensions
        scaled_width = width;
        scaled_height = height;
        
        if(scaled_frame == NULL || prev_width != scaled_width || prev_height != scaled_height) {
            if(scaled_frame != NULL) free(scaled_frame);
            scaled_frame = malloc(scaled_width * scaled_height);
            if(scaled_frame == NULL) {
                LOG("not enough memory for scaled frame\n");
                free(gray_data);
                break;
            }
            prev_width = scaled_width;
            prev_height = scaled_height;
        }
        
        // Copy the already scaled data (no additional downsampling needed)
        memcpy(scaled_frame, gray_data, scaled_width * scaled_height);
        
        free(gray_data);

        /* Initialize previous frame if this is the first frame */
        if(prev_frame == NULL) {
            prev_frame = malloc(scaled_width * scaled_height);
            if(prev_frame == NULL) {
                LOG("not enough memory for previous frame\n");
                free(scaled_frame);
                break;
            }
            memcpy(prev_frame, scaled_frame, scaled_width * scaled_height);
            continue;
        }

        /* Calculate motion level */
        motion_level = calculate_motion_level(scaled_frame, prev_frame, scaled_width, scaled_height);

        /* Check if motion detected and not overloaded */
        if(motion_level > brightness_threshold && motion_level < overload_threshold) {
            time_t now = time(NULL);
            
            /* Check cooldown */
            if(now - last_motion_time >= motion_cooldown) {
                last_motion_time = now;
                
                OPRINT("motion detected! level: %.1f%% (threshold: %d%%)\n", 
                       motion_level, brightness_threshold);
                
                /* Save motion frame if folder specified */
                if(save_folder != NULL) {
                    save_motion_frame(current_frame, frame_size, motion_level);
                }
                
                /* Send webhook notification if URL specified */
                if(webhook_url != NULL) {
                    send_webhook_notification_async(motion_level);
                }
            }
            /* Always update previous frame to prevent accumulation */
            /* The issue was that we were not updating prev_frame during motion */
            memcpy(prev_frame, scaled_frame, scaled_width * scaled_height);
        } else if(motion_level >= overload_threshold) {
            /* Motion level too high - likely lighting change, ignore */
            OPRINT("motion overload detected! level: %.1f%% (overload threshold: %d%%) - ignoring\n", 
                   motion_level, overload_threshold);
            /* Still update previous frame to prevent accumulation */
            memcpy(prev_frame, scaled_frame, scaled_width * scaled_height);
        } else {
            /* Update previous frame when no motion detected */
            memcpy(prev_frame, scaled_frame, scaled_width * scaled_height);
        }
    }

    /* cleanup now */
    pthread_cleanup_pop(1);

    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: this function is called first, in order to initialise
              this plugin and pass a parameter string
Input Value.: parameters
Return Value: 0 if everything is ok, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param, int id)
{
    int i;

    param->argv[0] = OUTPUT_PLUGIN_NAME;
    pglobal = param->global;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }
    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"downscale", required_argument, 0, 0},
            {"motion", required_argument, 0, 0},
            {"overload", required_argument, 0, 0},
            {"skipframe", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"webhook", required_argument, 0, 0},
            {"post", no_argument, 0, 0},
            {"cooldown", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
            {"jpeg-size-check", required_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;
        

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* downscale factor */
            case 0:
                scale_factor = atoi(optarg);
                if(scale_factor < 1) scale_factor = 1;
                if(scale_factor > 16) scale_factor = 16;
                break;
            /* motion detection threshold */
            case 1:
                brightness_threshold = atoi(optarg);
                if(brightness_threshold < 1) {
                    OPRINT("WARNING: motion threshold %d is too low, setting to 1\n", brightness_threshold);
                    brightness_threshold = 1;
                }
                if(brightness_threshold > 100) brightness_threshold = 100;
                // Update cached threshold
                cached_abs_threshold = (brightness_threshold * 255) / 100;
                break;
            /* overload threshold */
            case 2:
                overload_threshold = atoi(optarg);
                if(overload_threshold < 1) {
                    OPRINT("WARNING: overload threshold %d is too low, setting to 1\n", overload_threshold);
                    overload_threshold = 1;
                }
                if(overload_threshold > 100) overload_threshold = 100;
                break;
            /* skip frame interval */
            case 3:
                check_interval = atoi(optarg);
                if(check_interval < 1) check_interval = 1;
                break;
            /* save folder */
            case 4:
                save_folder = strdup(optarg);
                break;
            /* webhook URL */
            case 5:
                webhook_url = strdup(optarg);
                break;
            /* webhook POST method */
            case 6:
                webhook_post = 1;
                break;
            /* cooldown */
            case 7:
                motion_cooldown = atoi(optarg);
                if(motion_cooldown < 0) motion_cooldown = 0;
                break;
            /* input plugin number */
            case 8:
                input_number = atoi(optarg);
                break;
            /* size threshold */
            case 9:
                size_threshold = atoi(optarg);
                if(size_threshold < 0) size_threshold = 0;
                if(size_threshold > 1000) size_threshold = 1000; // 1000 = 100%
                break;
            /* help */
            case 10:
                help();
                return 1;
        }
    }

    if(!(input_number < param->global->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", 
               input_number, param->global->incnt);
        return 1;
    }

    /* Initialize webhook thread if webhook URL is specified */
#ifdef HAVE_CURL
    if(webhook_url != NULL) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        
        // Start webhook worker thread
        webhook_thread_running = 1;
        if(pthread_create(&webhook_thread, NULL, webhook_worker_thread, NULL) != 0) {
            OPRINT("ERROR: could not create webhook thread\n");
            curl_global_cleanup();
            return 1;
        }
        
        OPRINT("webhook thread started\n");
    }
#else
    if(webhook_url != NULL) {
        OPRINT("WARNING: webhook URL specified but CURL not available\n");
        webhook_url = NULL; // Disable webhook functionality
    }
#endif

    /* Create save folder if specified */
    if(save_folder != NULL) {
        struct stat st = {0};
        if(stat(save_folder, &st) == -1) {
            if(mkdir(save_folder, 0755) == -1) {
                OPRINT("ERROR: could not create save folder %s\n", save_folder);
                return 1;
            }
        }
    }

    // Initialize cached threshold values for better performance
    cached_abs_threshold = (brightness_threshold * 255) / 100;
    cached_early_exit_threshold = 0; // Will be calculated per frame based on dimensions

    OPRINT("input plugin.....: %d: %s\n", input_number, param->global->in[input_number].plugin);
    OPRINT("downscale factor: %d\n", scale_factor);
    OPRINT("motion threshold: %d\n", brightness_threshold);
    OPRINT("overload threshold: %d\n", overload_threshold);
    OPRINT("skip frame......: %d\n", check_interval);
    OPRINT("motion cooldown..: %d seconds\n", motion_cooldown);
    if(save_folder != NULL) {
        OPRINT("save folder......: %s\n", save_folder);
    }
    if(webhook_url != NULL) {
        OPRINT("webhook URL......: %s\n", webhook_url);
        OPRINT("webhook method...: %s\n", webhook_post ? "POST" : "GET");
    }

    return 0;
}

/******************************************************************************
Description.: this function is called to start the output plugin
Input Value.: -
Return Value: 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching worker thread\n");
    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        OPRINT("could not start worker thread\n");
        return 1;
    }
    pthread_detach(worker);

    return 0;
}

/******************************************************************************
Description.: this function is called to stop the output plugin
Input Value.: -
Return Value: 0
******************************************************************************/
int output_stop(int id)
{
    DBG("stopping worker thread\n");
    pthread_cancel(worker);
    
    // Stop webhook thread
    if(webhook_thread_running) {
        webhook_thread_running = 0;
        pthread_mutex_lock(&webhook_mutex);
        webhook_in_progress = 0;  // Reset flag
        pthread_cond_signal(&webhook_cond);
        pthread_mutex_unlock(&webhook_mutex);
        pthread_join(webhook_thread, NULL);
        OPRINT("webhook thread stopped\n");
    }
    
    return 0;
}

/******************************************************************************
Description.: this function is called to unload this plugin
Input Value.: -
Return Value: 0
******************************************************************************/
int output_cmd(int plugin_id, unsigned int control_id, unsigned int group, int value, char *valueStr)
{
    DBG("command (%d, value: %d) for group %d triggered for plugin instance #%02d\n", 
        control_id, value, group, plugin_id);
    
    /* No commands supported yet */
    return 0;
}
