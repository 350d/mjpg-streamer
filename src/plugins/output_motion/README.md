# Motion Detection Output Plugin

This plugin provides motion detection capabilities for mjpg-streamer. It analyzes incoming video frames to detect motion and can save motion frames and/or send webhook notifications.

## Features

- **Motion Detection**: Analyzes frame differences to detect motion
- **Configurable Parameters**: Adjustable sensitivity, thresholds, and intervals
- **Frame Saving**: Optional saving of motion frames with timestamps
- **Webhook Notifications**: HTTP GET/POST notifications when motion is detected (HTTP only)
- **Cooldown Period**: Prevents spam by limiting motion events
- **Optimized Processing**: Scales down frames for faster processing

## Parameters

| Parameter | Short | Description | Default |
|-----------|-------|-------------|---------|
| `--scale` | `-s` | Scale down factor (1-16) | 4 |
| `--threshold` | `-l` | Brightness difference threshold in % (1-100) | 5 |
| `--noise` | `-t` | Noise threshold in % (0.0-100.0) | 5 |
| `--interval` | `-i` | Check every N frames | 1 |
| `--folder` | `-f` | Folder to save motion frames | - |
| `--webhook` | `-w` | Webhook URL for motion events | - |
| `--post` | `-p` | Use POST instead of GET for webhook | GET |
| `--cooldown` | `-c` | Cooldown between events (seconds) | 2 |
| `--input` | `-n` | Input plugin number | 0 |
| `--size-threshold` | `-z` | JPEG size change threshold in 0.1% units (0-1000) | 1 (0.1%) |

## Usage Examples

### Basic Motion Detection
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" -o "output_motion.so"
```

### Save Motion Frames
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -f /tmp/motion_frames"
```

### Webhook Notifications (GET)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -w http://localhost:8080/motion"
```

### Webhook Notifications (POST)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -w http://localhost:8080/motion -p"
```

### High Sensitivity with Custom Settings
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -s 2 -l 3 -t 3 -c 1"
```

### Performance Optimization with Size Threshold
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 1 -s 4 -l 5 -t 5"  # 0.1% threshold (default)
```

### Higher Sensitivity (More Processing)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 0 -s 4 -l 5 -t 5"  # 0.0% threshold (process all frames)
```

### Lower Sensitivity (Less Processing)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 5 -s 4 -l 5 -t 5"  # 0.5% threshold
```

### Combined Usage
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -f /tmp/motion -w http://localhost:8080/motion -p -s 4 -l 5 -t 5 -z 3"
```

## How It Works

1. **JPEG Size Check**: First, the plugin compares the current JPEG frame size with the previous frame
   - If the size change is below the threshold (`-z` in 0.1% units), motion analysis is skipped (performance optimization)
   - Default threshold is 1 (0.1%) - extremely sensitive to any changes
   - This prevents unnecessary processing only when the scene is completely static
2. **Frame Processing**: Each frame is converted to grayscale and scaled down by the specified factor
3. **Motion Calculation**: The plugin compares consecutive frames pixel by pixel
4. **Threshold Check**: If the difference exceeds the noise threshold, motion is detected
5. **Actions**: On motion detection:
   - Optionally saves the frame with timestamp and motion level
   - Optionally sends a webhook notification with motion data

## Webhook Format

When motion is detected, the plugin sends a request to the specified webhook URL:

### GET Request (default)
```
GET http://localhost:8080/motion?timestamp=2024-01-15%2014:30:25&motion_level=75.0&threshold=5.0
```

### POST Request (with -p flag)
```
POST http://localhost:8080/motion
Content-Type: application/x-www-form-urlencoded

timestamp=2024-01-15 14:30:25&motion_level=75.0&threshold=5.0
```

## File Naming

Saved motion frames use the following naming convention:
```
YYYYMMDD_HHMMSS_motion_LEVEL.jpg
```

Example: `20240115_143025_motion_75.0%.jpg`

## Performance Notes

- **Scale Factor**: Higher values (e.g., 8) reduce processing load but may miss small movements
- **Check Interval**: Skipping frames (e.g., `-i 5`) reduces CPU usage
- **Thresholds**: Lower noise threshold (e.g., 3%) increases sensitivity but may cause false positives
- **Cooldown**: Prevents rapid-fire notifications for continuous motion

## Dependencies

- **libcurl**: For webhook notifications
- **pthread**: For multi-threading support

## Installation

The plugin is automatically built when compiling mjpg-streamer with CMake. Ensure libcurl development headers are installed:

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev

# CentOS/RHEL
sudo yum install libcurl-devel

# macOS
brew install curl
```

## Troubleshooting

### High CPU Usage
- Increase scale factor (`-s 8`)
- Increase check interval (`-i 5`)
- Increase noise threshold (`-t 7`)

### False Positives
- Increase brightness threshold (`-l 10`)
- Increase noise threshold (`-t 7`)
- Increase cooldown period (`-c 5`)

### Missing Motion
- Decrease brightness threshold (`-l 3`)
- Decrease noise threshold (`-t 3`)
- Decrease scale factor (`-s 2`)

### Webhook Issues
- Check URL accessibility
- Verify network connectivity
- Check webhook server logs
