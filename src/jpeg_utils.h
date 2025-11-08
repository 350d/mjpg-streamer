/* Include TurboJPEG headers if available */
#ifdef HAVE_TURBOJPEG
    #include <turbojpeg.h>
#endif

/* JPEG library detection and initialization */
int detect_jpeg_library(void);
int jpeg_library_available(void);

/* JPEG decompression functions */
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height, int known_width, int known_height);

int jpeg_decode_to_y_component(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **y_data, int *width, int *height, int known_width, int known_height);

int decode_any_to_y_component(unsigned char *data, int data_size, int scale_factor,
                              unsigned char **y_data, int *width, int *height, int known_width, int known_height, int known_format);

int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height, int known_width, int known_height);

/* TurboJPEG handle caching functions */
void cleanup_turbojpeg_handles(void);

/* Default JPEG quantization tables (quality ~75) */
extern const unsigned char jpeg_default_qt_luma[64];
extern const unsigned char jpeg_default_qt_chroma[64];

/* TurboJPEG helpers */
/* Returns 0 on success; fills width, height, subsamp (TJSAMP_*). */
int turbojpeg_header_info(const unsigned char *jpeg_data, int jpeg_size,
                          int *width, int *height, int *subsamp);

/* Strip JPEG to RTP/JPEG format (RFC 2435) */
/* Input: Full JPEG (SOI...EOI), dimensions, subsamp */
/* Output: RTP/JPEG payload (JPEG header + optional QT + scan data, no SOI/EOI) */
int jpeg_strip_to_rtp(const unsigned char *jfif, size_t jfif_sz,
                     unsigned char *out, size_t *out_sz,
                     uint16_t w, uint16_t h, int subsamp);

/* RFC 2435 Quantization Table extraction/cache */
/* Extract DQT (FF DB) segments from JPEG and cache tables for RTP transmission */
void rtpjpeg_cache_qtables_from_jpeg(const uint8_t *p, size_t sz);
int rtpjpeg_get_cached_qtables(const uint8_t **luma, const uint8_t **chroma,
                               int *have_luma, int *have_chroma, int *precision);
/* Convert quantization table from natural order (DQT) to zigzag order (RFC 2435) */
void rtpjpeg_qt_to_zigzag(const uint8_t *src_nat, uint8_t *dst_zig);



/* JPEG utility functions */


/* JPEG compression functions */
int compress_rgb_to_jpeg(unsigned char *rgb_data, int width, int height, int quality,
                        unsigned char **jpeg_data, unsigned long *jpeg_size);

/* JPEG data structure for RGB output */
typedef struct {
    int width;
    int height;
    unsigned char *buffer;
    int buffersize;
} jpeg_rgb_image;
