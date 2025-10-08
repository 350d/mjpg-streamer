# mjpg-streamer

A lightweight MJPEG streaming server for video capture devices, with motion detection capabilities.

## Features

- **Multiple input sources**: USB webcams (UVC), Raspberry Pi camera, HTTP streams, files
- **Multiple output formats**: HTTP streaming, file saving, motion detection, RTSP, UDP
- **Motion detection plugin**: Real-time motion detection with webhook notifications
- **Cross-platform**: Works on Linux, Raspberry Pi, and other Unix-like systems
- **Plugin architecture**: Modular design with input and output plugins

## Quick Start

### Prerequisites

Install required dependencies:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install cmake build-essential libjpeg-dev libcurl4-openssl-dev

# Raspberry Pi OS
sudo apt update
sudo apt install cmake build-essential libjpeg-dev libcurl4-openssl-dev

# For USB webcam support (optional)
sudo apt install v4l-utils

# Optional dependencies (for additional plugins)
sudo apt install libopencv-dev libgphoto2-dev
```

### Building

```bash
# Clone the repository
git clone --depth=1 https://github.com/350d/mjpg-streamer.git
cd mjpg-streamer

# Create build directory
mkdir build
cd build

# Configure and build
cmake ..
make -j$(nproc)

# Install (optional)
sudo make install
```

### Fast Build for Raspberry Pi Zero

For faster compilation on Pi Zero (single-core ARM processor):

```bash
# Use the fast build script
./build_fast.sh

# Or manually with optimized flags:
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-pipe -fno-stack-protector -O1"
make -j1  # Single thread for Pi Zero
```

### Install ccache for Even Faster Builds

```bash
# Install ccache for compilation caching
./install_ccache.sh

# Then use ccache for subsequent builds
export PATH=/usr/lib/ccache:$PATH
./build_fast.sh
```

**Note**: You may see CMake warnings about missing OpenCV or GPhoto2. These are optional dependencies for additional plugins. The core functionality will work without them.

### Running

After building, the executable will be in the `build/` directory and all plugins will be in the `plugins/` directory:

```bash
# List all plugins
ls plugins/

# Basic usage with USB webcam
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" -o "./plugins/output_http.so -p 8080"

# With motion detection
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_http.so -p 8080" \
                -o "./plugins/output_motion.so -s 4 -l 5 -w http://your-webhook-url"

# Raspberry Pi camera (if available)
./mjpg_streamer -i "./plugins/input_raspicam.so" -o "./plugins/output_http.so -p 8080"
```

## Available Plugins

### Input Plugins

- **input_uvc.so**: USB webcam support (UVC compatible)
- **input_raspicam.so**: Raspberry Pi camera module
- **input_http.so**: HTTP stream input
- **input_file.so**: File input (images/video)
- **input_testpicture.so**: Test pattern generator

### Output Plugins

- **output_http.so**: HTTP MJPEG streaming server
- **output_file.so**: Save frames to files
- **output_motion.so**: Motion detection with webhook notifications
- **output_rtsp.so**: RTSP streaming
- **output_udp.so**: UDP streaming
- **output_viewer.so**: Simple viewer window

## Motion Detection Plugin

The `output_motion` plugin provides real-time motion detection with configurable sensitivity, overload protection, and webhook notifications:

```bash
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_motion.so -d 4 -l 5 -o 50 -s 1 -f /motion -w http://webhook-url"
```

### Parameters

- `-d, --downscale N`: Scale factor (default: 4) - reduces image size for faster processing
- `-l, --motion N`: Motion detection threshold in percentage (default: 5%)
- `-o, --overload N`: Overload threshold in percentage (default: 50%) - ignores lighting changes
- `-s, --skipframe N`: Check every N frames (default: 1) - skip frames for performance
- `-f, --folder PATH`: Save motion frames to directory (optional)
- `-w, --webhook URL`: Webhook URL for notifications (optional)
- `-p, --post`: Use POST method for webhook (default: GET)
- `-c, --cooldown N`: Cooldown between events in seconds (default: 5)
- `-z, --jpeg-size-check N`: JPEG size change threshold in 0.1% units (default: 1 = 0.1%)

### Key Features

- **Overload Protection**: The `-o, --overload` parameter prevents false alarms from lighting changes (lights turning on/off)
- **Performance Optimization**: The `-z, --jpeg-size-check` parameter skips motion analysis when JPEG size changes are minimal
- **Frame Skipping**: The `-s, --skipframe` parameter allows processing every N frames for better performance
- **Cooldown**: The `-c, --cooldown` parameter prevents spam notifications with configurable delays

### Webhook Notifications

When motion is detected, the plugin can send HTTP notifications:

```bash
# GET request (default)
-o "./plugins/output_motion.so -w http://your-server.com/motion-detected"

# POST request
-o "./plugins/output_motion.so -w http://your-server.com/motion -p"
```

## Configuration Examples

### Security Camera Setup

```bash
# Save motion frames and send webhook notifications with overload protection
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0 -r 1280x720 -f 15" \
                -o "./plugins/output_http.so -p 8080" \
                -o "./plugins/output_motion.so -d 4 -l 3 -o 50 -c 5 -f /var/motion -w http://alerts.example.com/motion"
```

### Raspberry Pi Camera

```bash
# High quality stream with motion detection (if raspicam plugin is available)
./mjpg_streamer -i "./plugins/input_raspicam.so -r 1920x1080 -f 30" \
                -o "./plugins/output_http.so -p 8080" \
                -o "./plugins/output_motion.so -d 2 -l 5 -o 60 -s 2 -c 10 -w http://webhook.example.com"
```

### File Input

```bash
# Process existing video file
./mjpg_streamer -i "./plugins/input_file.so -f /path/to/video.mp4" \
                -o "./plugins/output_http.so -p 8080"
```

### Motion Detection Configuration Examples

```bash
# Sensitive motion detection (for indoor use)
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_motion.so -d 2 -l 2 -o 30 -c 3"

# Conservative motion detection (for outdoor use with lighting changes)
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_motion.so -d 4 -l 10 -o 80 -c 10"

# High performance setup (skip frames for better performance)
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_motion.so -d 4 -l 5 -s 3 -z 5"

# Complete security setup with all features
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0 -r 1280x720 -f 15" \
                -o "./plugins/output_http.so -p 8080" \
                -o "./plugins/output_motion.so -d 4 -l 5 -o 50 -s 1 -c 5 -z 1 -f /var/motion -w http://alerts.example.com/motion -p"
```

## Troubleshooting

### Plugin Loading Issues

If you get "cannot open shared object file" errors:

1. **Use full paths to plugins**:
   ```bash
   ./mjpg_streamer -i "./plugins/input_uvc.so" -o "./plugins/output_http.so"
   ```

2. **Check dependencies**:
   ```bash
   sudo apt install libjpeg-dev libcurl4-openssl-dev
   ```

3. **Verify plugin files exist**:
   ```bash
   ls -la plugins/
   ```

### Camera Issues

- **List available cameras**:
  ```bash
  ls /dev/video*
  v4l2-ctl --list-devices
  ```

- **Test camera**:
  ```bash
  v4l2-ctl --device=/dev/video0 --list-formats-ext
  ```

### Performance Issues

- **Reduce resolution**: Use `-r 640x480` instead of higher resolutions
- **Lower frame rate**: Use `-f 15` instead of 30 FPS
- **Increase motion scale**: Use `-s 8` for faster motion detection

## Project Structure

```
mjpg-streamer/
├── src/                  # Source code
│   ├── plugins/         # Input/output plugins
│   │   ├── input_uvc/   # USB webcam support
│   │   ├── input_raspicam/ # Raspberry Pi camera
│   │   ├── output_http/ # HTTP streaming
│   │   ├── output_motion/ # Motion detection
│   │   └── ...          # Other plugins
│   ├── www/             # Web interface files
│   ├── mjpg_streamer.c  # Main application
│   └── utils.c          # Utility functions
├── CMakeLists.txt       # Build configuration
├── LICENSE              # MIT License
└── README.md           # This file
```

## Development

### Adding New Plugins

1. Create a new directory in `src/plugins/`
2. Add `CMakeLists.txt` for the plugin
3. Implement the plugin interface
4. Update the main `CMakeLists.txt` to include the plugin

### Building Specific Plugins

```bash
# Build only specific plugins
make input_uvc.so
make output_motion.so
```

## License

MIT License - see LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## Support

For issues and questions:
- Check the troubleshooting section above
- Open an issue on GitHub
- Review the plugin-specific README files in `src/plugins/`