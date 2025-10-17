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

/* Use centralized JPEG utilities */
#include "../../jpeg_utils.h"

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
static int sequence_frames = 1;        // -q: sequence frames for confirmation (default 1)
static int enable_blur = 0;            // -b: enable 3x3 blur filter for noise reduction
static int enable_autolevels = 0;      // -a: enable auto levels for better contrast
static char *save_folder = NULL;       // -f: folder to save motion frames and debug images
static char *webhook_url = NULL;       // -w: webhook URL for motion events
static int webhook_post = 0;           // -p: use POST instead of GET

// Zone-based motion detection
static int zones_enabled = 0;          // -z: enable zone-based motion detection
static int zone_divider = 3;           // Zone grid divider (3x3, 4x4, etc.)
static int zone_weights[16] = {1,1,1,1,1,1,1,1,1}; // Zone weights (max 4x4 = 16 zones)
static int zone_count = 9;             // Number of zones (3x3 = 9)

// Internal state
static pthread_t worker;
static globals *pglobal = NULL;
static int input_number = 0;
static int frame_counter = 0;
static int motion_sequence_count = 0;  // Counter for consecutive motion frames
static unsigned char *prev_frame = NULL;
static unsigned char *current_frame = NULL;
static unsigned char *blur_buffer = NULL;  // Buffer for blur filter
static unsigned char *autolevels_buffer = NULL;  // Buffer for auto levels
static int prev_width = 0, prev_height = 0;
static int scaled_width = 0, scaled_height = 0;
static time_t last_motion_time = 0;
static time_t last_motion_overload_time = 0;
static int motion_cooldown = 5; // seconds between motion events
static int size_threshold = 1;  // -z: size change threshold in 0.1% units (default: 0.1%)
static int prev_jpeg_size = 0;  // previous JPEG frame size for comparison

/* JPEG functions now provided by jpeg_utils.h */

/******************************************************************************
Description.: Parse zones configuration string
Input Value.: zones string (e.g., "3_010010011")
Return Value: -
******************************************************************************/
static void parse_zones_config(const char *zones_str)
{
    if (zones_str == NULL || strlen(zones_str) == 0) {
        OPRINT("ERROR: zones parameter is empty\n");
        return;
    }
    
    char *str = strdup(zones_str);
    if (str == NULL) {
        OPRINT("ERROR: failed to allocate memory for zones parsing\n");
        return;
    }
    
    // Find the underscore separator
    char *underscore = strchr(str, '_');
    if (underscore == NULL) {
        OPRINT("ERROR: zones format should be 'divider_weights' (e.g., '3_010010011')\n");
        free(str);
        return;
    }
    
    // Parse divider
    *underscore = '\0';
    int divider = atoi(str);
    if (divider < 2 || divider > 4) {
        OPRINT("ERROR: zone divider must be between 2 and 4 (got %d)\n", divider);
        free(str);
        return;
    }
    
    // Parse weights
    char *weights_str = underscore + 1;
    int expected_weights = divider * divider;
    int weights_len = strlen(weights_str);
    
    if (weights_len != expected_weights) {
        OPRINT("ERROR: expected %d weights, got %d characters\n", expected_weights, weights_len);
        free(str);
        return;
    }
    
    // Parse each weight (0-9)
    for (int i = 0; i < expected_weights; i++) {
        if (weights_str[i] < '0' || weights_str[i] > '9') {
            OPRINT("ERROR: weight %d must be a digit 0-9 (got '%c')\n", i, weights_str[i]);
            free(str);
            return;
        }
        zone_weights[i] = weights_str[i] - '0';
    }
    
    // Set configuration
    zone_divider = divider;
    zone_count = expected_weights;
    zones_enabled = 1;
    
    OPRINT("Zones configured: %dx%d grid with weights:\n", divider, divider);
    for (int y = 0; y < divider; y++) {
        char line[256] = "  ";
        int pos = 2;
        for (int x = 0; x < divider; x++) {
            int index = y * divider + x;
            pos += snprintf(line + pos, sizeof(line) - pos, "%d", zone_weights[index]);
            if (x < divider - 1) {
                pos += snprintf(line + pos, sizeof(line) - pos, " ");
            }
        }
        OPRINT("%s\n", line);
    }
    
    free(str);
}

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
            " [-l | --motion ]........: pixel brightness change threshold in %% (default: 5%%)\n" \
            " [-o | --overload ]......: overload threshold in %% (default: 50%%)\n" \
            " [-s | --sequence ]......: consecutive frames required for motion confirmation (default: 1)\n" \
            " [-n | --nframe ]........: check every N frames (default: 1)\n" \
            " [-b | --blur ]..........: enable 3x3 blur filter for noise reduction\n" \
            " [-a | --autolevels ]....: enable auto levels for better contrast\n" \
            " [-f | --folder ]........: folder to save motion frames and debug images\n" \
            " [-w | --webhook ].......: webhook URL for motion events\n" \
            " [-p | --post ]..........: use POST instead of GET for webhook\n" \
            " [-c | --cooldown ]......: cooldown between events in seconds (default: 5)\n" \
            " [-i | --input ].........: read frames from the specified input plugin\n" \
            " [-j | --jpeg-size-check]: JPEG file size change threshold in 0.1%% units (default: 1 = 0.1%%)\n" \
            "                           Skip motion analysis if JPEG size change is below this threshold\n" \
            " [-z | --zones ].........: zone-based motion detection (format: divider_weights)\n" \
            "                           Example: --zones 3_010010011 (3x3 grid, weights 0-9)\n" \
            "                           Zone weights: 0=ignore, 1-9=weight (left-to-right, top-to-bottom)\n" \
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
    
    if(blur_buffer != NULL) {
        free(blur_buffer);
        blur_buffer = NULL;
    }
    
    if(autolevels_buffer != NULL) {
        free(autolevels_buffer);
        autolevels_buffer = NULL;
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

    /* Cleanup cached TurboJPEG handles */
    cleanup_turbojpeg_handles();
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

/* validate_jpeg_data now provided by jpeg_utils.h */

/* decode_jpeg_to_gray_scaled now provided by jpeg_utils.h */

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
Description.: Apply optimized 3x3 blur filter using separable convolution
Input Value.: input frame, output frame, width, height
Return Value: 0 on success, -1 on error
******************************************************************************/
int apply_fast_blur_3x3(unsigned char *input, unsigned char *output, int width, int height)
{
    if(input == NULL || output == NULL || width <= 0 || height <= 0) {
        return -1;
    }
    
    
    // Allocate temporary buffer for horizontal pass
    unsigned char *temp = malloc(width * height);
    if(temp == NULL) {
        return -1;
    }
    
    // First pass: horizontal 1D blur (1x3 kernel)
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            int sum = 0;
            
            // Horizontal 1x3 kernel with virtual border extension
            for(int kx = -1; kx <= 1; kx++) {
                int vx = x + kx;
                // Clamp to valid range (virtual border extension) - optimized
                vx = (vx < 0) ? 0 : (vx >= width) ? width - 1 : vx;
                
                sum += input[y * width + vx];
            }
            
            // Average (divide by 3)
            temp[y * width + x] = sum / 3;
        }
    }
    
    // Second pass: vertical 1D blur (3x1 kernel)
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            int sum = 0;
            
            // Vertical 3x1 kernel with virtual border extension
            for(int ky = -1; ky <= 1; ky++) {
                int vy = y + ky;
                // Clamp to valid range (virtual border extension) - optimized
                vy = (vy < 0) ? 0 : (vy >= height) ? height - 1 : vy;
                
                sum += temp[vy * width + x];
            }
            
            // Average (divide by 3) - equivalent to 3x3 kernel divided by 9
            unsigned char blurred_pixel = sum / 3;
            output[y * width + x] = blurred_pixel;
        }
    }
    
    // Clean up temporary buffer
    free(temp);
    
    return 0;
}

/******************************************************************************
Description.: Apply auto levels to improve contrast and normalize brightness
Input Value.: input frame, output frame, width, height
Return Value: 1 if auto levels were applied, 0 if skipped (range too small)
******************************************************************************/
int apply_auto_levels(unsigned char *input, unsigned char *output, int width, int height)
{
    if(input == NULL || output == NULL || width <= 0 || height <= 0) {
        return 0;
    }
    
    int total_pixels = width * height;
    int min_val = 255, max_val = 0;
    
    // Find min and max values in the frame (optimized with if-else)
    for(int i = 0; i < total_pixels; i++) {
        if(input[i] < min_val) {
            min_val = input[i];
        } else if(input[i] > max_val) {
            max_val = input[i];
        }
        // If input[i] is between min_val and max_val, no action needed
    }
    
    // If range is too small, don't apply auto levels
    if(max_val - min_val < 10) {
        // No need to copy - just return 0 to indicate auto levels was not applied
        return 0;
    }
    
    // Apply linear stretching: [min_val, max_val] -> [0, 255]
    double scale = 255.0 / (max_val - min_val);

    for(int i = 0; i < total_pixels; i++) {
        unsigned char processed_pixel = (unsigned char)((input[i] - min_val) * scale);
        output[i] = processed_pixel;
    }
    
    return 1;
}


/******************************************************************************
Description.: Calculate motion level using pixel-by-pixel comparison
Input Value.: current frame, previous frame, dimensions
Return Value: motion level in percentage (0.0 - 100.0)
******************************************************************************/
double calculate_motion_level(unsigned char *current_frame, unsigned char *prev_frame, int width, int height)
{
    // Input validation - critical for stability
    if(width <= 0 || height <= 0 || current_frame == NULL || prev_frame == NULL) {
        return 0.0; // Invalid input
    }
    
    int total_pixels = width * height;
    int motion_pixels = 0;
    
    // Calculate motion threshold based on brightness_threshold parameter
    // Convert percentage to pixel brightness difference (0-255 range)
    int motion_pixel_threshold = (brightness_threshold * 255) / 100;
    if(motion_pixel_threshold < 1) motion_pixel_threshold = 1;
    if(motion_pixel_threshold > 255) motion_pixel_threshold = 255;
    
    if (zones_enabled) {
        // Zone-based motion detection with weighted zones
        int zone_width = width / zone_divider;
        int zone_height = height / zone_divider;
        int weighted_motion_pixels = 0;
        int total_weighted_pixels = 0;
        
        // Process each zone
        for (int zone_y = 0; zone_y < zone_divider; zone_y++) {
            for (int zone_x = 0; zone_x < zone_divider; zone_x++) {
                int zone_index = zone_y * zone_divider + zone_x;
                int zone_weight = zone_weights[zone_index];
                
                // Calculate zone boundaries
                int start_x = zone_x * zone_width;
                int end_x = (zone_x == zone_divider - 1) ? width : (zone_x + 1) * zone_width;
                int start_y = zone_y * zone_height;
                int end_y = (zone_y == zone_divider - 1) ? height : (zone_y + 1) * zone_height;
                
                if (zone_weight == 0) {
                    // Skip this zone (weight 0) - no processing needed
                    continue;
                }
                
                int zone_motion_pixels = 0;
                int zone_total_pixels = 0;
                
                // Check pixels in this zone
                for (int y = start_y; y < end_y; y++) {
                    for (int x = start_x; x < end_x; x++) {
                        int pixel_index = y * width + x;
                        zone_total_pixels++;
                        
                        if (abs(prev_frame[pixel_index] - current_frame[pixel_index]) > motion_pixel_threshold) {
                            zone_motion_pixels++;
                        }
                    }
                }
                
                // Apply zone weight
                weighted_motion_pixels += zone_motion_pixels * zone_weight;
                total_weighted_pixels += zone_total_pixels * zone_weight;
            }
        }
        
        // Calculate weighted motion level
        if (total_weighted_pixels > 0) {
            double motion_level = ((double)weighted_motion_pixels / (double)total_weighted_pixels) * 100.0;
            return motion_level;
        } else {
            return 0.0; // No zones with weight > 0
        }
    } else {
        // Traditional pixel-by-pixel comparison
        for(int i = 0; i < total_pixels; i++) {
            if(abs(prev_frame[i] - current_frame[i]) > motion_pixel_threshold) {
                motion_pixels++;
            }
        }
        
        // Calculate motion level as percentage of pixels that changed
        double motion_level = ((double)motion_pixels / (double)total_pixels) * 100.0;
        return motion_level;
    }
}


/******************************************************************************
Description.: Save debug frame (processed grayscale image) to JPEG file using TurboJPEG
Input Value.: grayscale frame data, dimensions, motion level, frame counter, suffix
Return Value: 0 on success, -1 on error
******************************************************************************/
int save_debug_frame(unsigned char *gray_data, int width, int height, double motion_level, int frame_num, const char *suffix)
{
    if(save_folder == NULL || gray_data == NULL) {
        return 0; // No save folder specified or invalid data
    }
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/debug_%04d%02d%02d_%02d%02d%02d_frame%d_motion_%.1f%%_%s.jpg",
             save_folder,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             frame_num, motion_level, suffix);
    
    // Convert grayscale to RGB for JPEG encoding
    unsigned char *rgb_data = malloc(width * height * 3);
    if(rgb_data == NULL) {
        OPRINT("could not allocate memory for RGB conversion\n");
        return -1;
    }
    
    // Convert grayscale to RGB (R=G=B=gray_value)
    for(int i = 0; i < width * height; i++) {
        rgb_data[i * 3] = gray_data[i];     // R
        rgb_data[i * 3 + 1] = gray_data[i]; // G
        rgb_data[i * 3 + 2] = gray_data[i]; // B
    }
    
    // Use TurboJPEG to compress with maximum quality
    unsigned char *jpeg_data = NULL;
    unsigned long jpeg_size = 0;
    
    if(compress_rgb_to_jpeg(rgb_data, width, height, 100, &jpeg_data, &jpeg_size) < 0) {
        OPRINT("could not compress debug frame to JPEG\n");
        free(rgb_data);
        return -1;
    }
    
    // Write JPEG data to file
    FILE *file = fopen(filename, "wb");
    if(file == NULL) {
        OPRINT("could not create debug frame file %s\n", filename);
        free(rgb_data);
        free(jpeg_data);
        return -1;
    }
    
    fwrite(jpeg_data, 1, jpeg_size, file);
    fclose(file);
    
    free(rgb_data);
    free(jpeg_data);
    
    OPRINT("debug frame saved: %s (motion: %.1f%%, size: %lu bytes)\n", filename, motion_level, jpeg_size);
    return 0;
}

int create_debug_frame_with_zones(unsigned char *gray_data, int width, int height, double motion_level, int frame_num, const char *suffix)
{
    if(save_folder == NULL || gray_data == NULL) {
        return 0; // No save folder specified or invalid data
    }
    
    // Create a copy for visualization
    unsigned char *debug_data = malloc(width * height);
    if(debug_data == NULL) {
        OPRINT("could not allocate memory for debug visualization\n");
        return -1;
    }
    
    // Copy original data
    simd_memcpy(debug_data, gray_data, width * height);
    
    // Add zone visualization if zones are enabled
    if (zones_enabled) {
        int zone_width = width / zone_divider;
        int zone_height = height / zone_divider;
        
        // Process each zone for visualization
        for (int zone_y = 0; zone_y < zone_divider; zone_y++) {
            for (int zone_x = 0; zone_x < zone_divider; zone_x++) {
                int zone_index = zone_y * zone_divider + zone_x;
                int zone_weight = zone_weights[zone_index];
                
                // Calculate zone boundaries
                int start_x = zone_x * zone_width;
                int end_x = (zone_x == zone_divider - 1) ? width : (zone_x + 1) * zone_width;
                int start_y = zone_y * zone_height;
                int end_y = (zone_y == zone_divider - 1) ? height : (zone_y + 1) * zone_height;
                
                if (zone_weight == 0) {
                    // Mark ignored zones in black
                    for (int y = start_y; y < end_y; y++) {
                        for (int x = start_x; x < end_x; x++) {
                            int pixel_index = y * width + x;
                            debug_data[pixel_index] = 0; // Black for ignored zones
                        }
                    }
                }
            }
        }
    }
    
    // Save the debug frame
    int result = save_debug_frame(debug_data, width, height, motion_level, frame_num, suffix);
    
    free(debug_data);
    return result;
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
    int frame_size = 0;
    unsigned char *scaled_frame = NULL;
    double motion_level = 0.0;

    /* Initialize SIMD capabilities */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop) {
        DBG("waiting for fresh frame\n");

        /* Wait for fresh frame using helper */
        static unsigned int last_motion_sequence = UINT_MAX;
        if (!wait_for_fresh_frame(&pglobal->in[input_number], &last_motion_sequence)) {
            /* Add small delay to prevent busy waiting */
            usleep(1000); /* 1ms delay */
            continue;
        }
        // ... existing code ...

        

        /* read buffer */
        frame_size = pglobal->in[input_number].current_size;
        
        /* check if frame size is within reasonable limits */
        if(frame_size > 10 * 1024 * 1024) { // 10MB limit
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            DBG("frame size too large: %d bytes, skipping\n", frame_size);
            continue;
        }
        
        /* allocate buffer for frame copy */
        if(current_frame == NULL || pglobal->in[input_number].prev_size != frame_size) {
            if(current_frame != NULL) free(current_frame);
            current_frame = malloc(frame_size);
            if(current_frame == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory for frame buffer\n");
                break;
            }
        }
        
        /* RELAY SYSTEM: Work directly with global buffer, no copying needed */
        /* Use global buffer directly for processing */
        unsigned char *frame_data = pglobal->in[input_number].buf;
        
        /* Copy only if we need to save motion frames (for file output) */
        if(save_folder != NULL) {
            if(current_frame == NULL || pglobal->in[input_number].prev_size != frame_size) {
                if(current_frame != NULL) free(current_frame);
                current_frame = malloc(frame_size);
                if(current_frame == NULL) {
                    pthread_mutex_unlock(&pglobal->in[input_number].db);
                    LOG("not enough memory for frame buffer\n");
                    break;
                }
            }
            simd_memcpy(current_frame, frame_data, frame_size);
        }

        frame_counter++;

        /* Check if we should process this frame */
        if(check_interval > 1 && frame_counter % check_interval != 0) {
            /* allow others to access the global buffer again */
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            
            /* Don't wait for signal if we're skipping - let others get it */
            continue;
        }

        /* Check if JPEG size changed significantly - use global metadata */
        if(!is_jpeg_size_changed(pglobal->in[input_number].current_size, pglobal->in[input_number].prev_size, size_threshold)) {
            /* allow others to access the global buffer again */
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            continue;
        }

        /* Initialize scaled frame buffer if needed */
        if(scaled_frame == NULL) {
            // Use global metadata instead of parsing JPEG header
            int width = pglobal->in[input_number].width;
            int height = pglobal->in[input_number].height;
            
            scaled_width = width / scale_factor;
            scaled_height = height / scale_factor;
            scaled_frame = malloc(scaled_width * scaled_height);
            if(scaled_frame == NULL) {
                LOG("not enough memory for scaled frame\n");
                break;
            }
        }

        /* Use global frame dimensions - no need to parse JPEG header again */
        int width = pglobal->in[input_number].width / scale_factor;
        int height = pglobal->in[input_number].height / scale_factor;
        
        
        /* Convert current frame to grayscale with integrated scaling */
        // Universal decoder - handles JPEG, MJPEG, raw RGB, raw YUV
        unsigned char *gray_data = NULL;
        
        if(decode_any_to_y_component(current_frame, frame_size, scale_factor, &gray_data, &width, &height, pglobal->in[input_number].width, pglobal->in[input_number].height, pglobal->in[input_number].format) < 0) {
            /* allow others to access the global buffer again */
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            continue;
        }
        
        // Use the already scaled dimensions
        scaled_width = width;
        scaled_height = height;
        
        // Update dimensions tracking
        if(prev_width != scaled_width || prev_height != scaled_height) {
            prev_width = scaled_width;
            prev_height = scaled_height;
        }
        
        // Use gray_data directly instead of copying - avoid unnecessary memcpy
        // This eliminates one memory copy operation per frame
        unsigned char *current_scaled_frame = gray_data;

        /* Apply blur filter if enabled */
        if(enable_blur) {
            // Initialize blur buffer if needed
            if(blur_buffer == NULL) {
                blur_buffer = malloc(scaled_width * scaled_height);
                if(blur_buffer == NULL) {
                    LOG("not enough memory for blur buffer\n");
                    free(gray_data);
                    break;
                }
            }
            
            // Apply 3x3 blur filter
            if(apply_fast_blur_3x3(current_scaled_frame, blur_buffer, scaled_width, scaled_height) == 0) {
                current_scaled_frame = blur_buffer; // Use blurred frame for motion detection
            }
        }

        /* Apply auto levels if enabled */
        if(enable_autolevels) {
            // Initialize auto levels buffer if needed
            if(autolevels_buffer == NULL) {
                autolevels_buffer = malloc(scaled_width * scaled_height);
                if(autolevels_buffer == NULL) {
                    LOG("not enough memory for auto levels buffer\n");
                    free(gray_data);
                    break;
                }
            }
            
            // Apply auto levels to improve contrast
            if(apply_auto_levels(current_scaled_frame, autolevels_buffer, scaled_width, scaled_height) == 1) {
                current_scaled_frame = autolevels_buffer; // Use auto-leveled frame for motion detection
            }
        }


        /* Initialize previous frame if this is the first frame */
        if(prev_frame == NULL) {
            prev_frame = malloc(scaled_width * scaled_height);
            if(prev_frame == NULL) {
                LOG("not enough memory for previous frame\n");
                free(gray_data);
                break;
            }
            simd_memcpy(prev_frame, current_scaled_frame, scaled_width * scaled_height);
            
            free(gray_data);
            continue;
        }

        /* Calculate motion level using pixel-by-pixel comparison */
        motion_level = calculate_motion_level(current_scaled_frame, prev_frame, scaled_width, scaled_height);
        

        /* Check motion level and handle sequence-based detection */
        if(motion_level >= overload_threshold) {
            /* Overload detected - reset sequence counter and ignore */
            motion_sequence_count = 0;
            time_t now = time(NULL);
            
            /* Check cooldown for overload messages to prevent spam */
            if(now - last_motion_overload_time >= motion_cooldown) {
                last_motion_overload_time = now;
                OPRINT("motion overload detected! level: %.1f%% (overload threshold: %d%%) - ignoring\n", 
                       motion_level, overload_threshold);
            }
            /* Update previous frame to prevent accumulation */
            simd_memcpy(prev_frame, current_scaled_frame, scaled_width * scaled_height);
        } else if(motion_level > brightness_threshold) {
            /* Motion detected - increment sequence counter */
            motion_sequence_count++;
            
            /* Check if we have enough consecutive motion frames */
            if(motion_sequence_count >= sequence_frames) {
                time_t now = time(NULL);
                
                /* Check cooldown */
                if(now - last_motion_time >= motion_cooldown) {
                    last_motion_time = now;
                    
                    OPRINT("motion detected! level: %.1f%% (threshold: %d%%, sequence: %d/%d)\n", 
                           motion_level, brightness_threshold, motion_sequence_count, sequence_frames);
                    
                    /* Save motion frame and debug frames if folder specified */
                    if(save_folder != NULL) {
                        /* Use current_frame if available, otherwise use global buffer */
                        unsigned char *motion_frame_data = (current_frame != NULL) ? current_frame : frame_data;
                        save_motion_frame(motion_frame_data, frame_size, motion_level);
                        create_debug_frame_with_zones(current_scaled_frame, scaled_width, scaled_height, motion_level, frame_counter, "current");
                        create_debug_frame_with_zones(prev_frame, scaled_width, scaled_height, motion_level, frame_counter, "previous");
                    }
                    
                    /* Send webhook notification if URL specified */
                    if(webhook_url != NULL) {
                        send_webhook_notification_async(motion_level);
                    }
                }
            }
            /* Update previous frame to prevent accumulation */
            simd_memcpy(prev_frame, current_scaled_frame, scaled_width * scaled_height);
        } else {
            /* No motion detected - reset sequence counter */
            motion_sequence_count = 0;
            /* Update previous frame when no motion detected */
            simd_memcpy(prev_frame, current_scaled_frame, scaled_width * scaled_height);
        }
        
        /* Free the gray_data after processing */
        free(gray_data);
        
        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        
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
            {"sequence", required_argument, 0, 0},
            {"nframe", required_argument, 0, 0},
            {"blur", no_argument, 0, 0},
            {"autolevels", no_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"webhook", required_argument, 0, 0},
            {"post", no_argument, 0, 0},
            {"cooldown", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
            {"jpeg-size-check", required_argument, 0, 0},
            {"zones", required_argument, 0, 0},
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
            /* pixel brightness change threshold */
            case 1:
                brightness_threshold = atoi(optarg);
                if(brightness_threshold < 1) {
                    OPRINT("WARNING: pixel brightness threshold %d is too low, setting to 1\n", brightness_threshold);
                    brightness_threshold = 1;
                }
                if(brightness_threshold > 100) brightness_threshold = 100;
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
            /* sequence frames for confirmation */
            case 3:
                sequence_frames = atoi(optarg);
                if(sequence_frames < 1) sequence_frames = 1;
                break;
            /* check every N frames */
            case 4:
                check_interval = atoi(optarg);
                if(check_interval < 1) check_interval = 1;
                break;
            /* enable blur filter */
            case 5:
                enable_blur = 1;
                break;
            /* enable auto levels */
            case 6:
                enable_autolevels = 1;
                break;
            /* save folder */
            case 7:
                save_folder = strdup(optarg);
                break;
            /* webhook URL */
            case 8:
                webhook_url = strdup(optarg);
                break;
            /* webhook POST method */
            case 9:
                webhook_post = 1;
                break;
            /* cooldown */
            case 10:
                motion_cooldown = atoi(optarg);
                if(motion_cooldown < 0) motion_cooldown = 0;
                break;
            /* input plugin number */
            case 11:
                input_number = atoi(optarg);
                break;
            /* size threshold */
            case 12:
                size_threshold = atoi(optarg);
                if(size_threshold < 0) size_threshold = 0;
                if(size_threshold > 1000) size_threshold = 1000; // 1000 = 100%
                break;
            /* zones configuration */
            case 13:
                parse_zones_config(optarg);
                break;
            /* help */
            case 14:
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


    OPRINT("input plugin.....: %d: %s\n", input_number, param->global->in[input_number].plugin);
    OPRINT("downscale factor: %d\n", scale_factor);
    OPRINT("pixel brightness threshold: %d%%\n", brightness_threshold);
    OPRINT("overload threshold: %d\n", overload_threshold);
    OPRINT("sequence frames..: %d\n", sequence_frames);
    OPRINT("skip frame......: %d\n", check_interval);
    OPRINT("blur filter......: %s\n", enable_blur ? "enabled" : "disabled");
    OPRINT("auto levels......: %s\n", enable_autolevels ? "enabled" : "disabled");
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
