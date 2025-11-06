# output_rtsp Plugin

High-performance RTSP streaming server with **RFC 2435 compliant JPEG over RTP** and **HTTP snapshot support**. Features optimized RTP packetization, TCP/UDP transport, and SIMD-accelerated operations.

## 🚀 Key Features

- **📡 RTSP Protocol**: Full RTSP server implementation (OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN)
- **🎥 RFC 2435 Compliant**: Proper JPEG over RTP packetization with correct Type mapping
- **🌐 HTTP Snapshot**: HTTP `/snapshot` endpoint on the same port for easy frame capture
- **⚡ SIMD Optimized**: SSE2/NEON accelerated memory operations
- **🔄 TCP/UDP Support**: Automatically uses transport mode requested by client
- **📊 Multi-Client**: Support for up to 10 concurrent RTSP clients
- **🎯 TurboJPEG**: Hardware-accelerated JPEG recompression with baseline format

## 📋 Parameters

| Parameter | Short | Description | Default |
|-----------|-------|-------------|---------|
| `--port` | `-p` | RTSP server port | 8554 |

## 🎮 Usage Examples

### Basic RTSP Streaming

```bash
# RTSP server on default port 8554
./mjpg_streamer -i "./plugins/input_avf.dylib -r 1280x720 -f 30" \
                -o "./plugins/output_rtsp.dylib -p 8554"

# Custom port
./mjpg_streamer -i "./plugins/input_uvc.so -d /dev/video0" \
                -o "./plugins/output_rtsp.so -p 554"
```

**Note:** The plugin automatically uses the transport mode requested by the client (TCP or UDP) in the RTSP SETUP request. No manual configuration is needed.

### Client Connection

```bash
# VLC
vlc rtsp://127.0.0.1:8554/stream

# FFmpeg
ffmpeg -i rtsp://127.0.0.1:8554/stream -f mp4 output.mp4

# FFplay
ffplay rtsp://127.0.0.1:8554/stream
```

## 📸 HTTP Snapshot Endpoint

The plugin provides an HTTP snapshot endpoint on the same port as RTSP:

```bash
# Get current frame as JPEG
curl http://127.0.0.1:8554/snapshot -o snapshot.jpg

# Check snapshot availability
curl -I http://127.0.0.1:8554/snapshot
```

**HTTP Response:**
- `200 OK`: JPEG snapshot returned
- `503 Service Unavailable`: No frame available yet
- `404 Not Found`: Invalid path (only `/snapshot` is supported)

## 🔧 Technical Details

- **RFC 2435 Compliant**: Proper JPEG over RTP packetization
- **SIMD Optimized**: SSE2/NEON accelerated memory operations
- **TurboJPEG**: Hardware-accelerated JPEG recompression
- **Frame Processing**: Baseline JPEG conversion with proper fragmentation

## 📊 Performance

- **HD Streaming (1280x720@30fps)**: 5-10% CPU
- **Full HD (1920x1080@30fps)**: 10-15% CPU
- **Memory**: ~10MB static buffers + ~50KB per client

