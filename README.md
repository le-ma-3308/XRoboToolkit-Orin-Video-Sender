# XRoboToolkit-Orin-Video-Sender
Video Previewer/Encoder/Sender on Nvidia Jetson Orin Platform

![Screenshot](Docs/screenshot.png)
> Sender (Webcam): `./OrinVideoSender --preview --send --server 192.168.1.176 --port 12345`

> Receiver (Video-Viewer): TCP - 192.168.1.176 - 12345 - 1280x720

## Features

- Support Webcam, ZED, and RealSense camera pipelines
- Preview
- H264 Encoding (via GStreamer)
- TCP/UDP sending w/ and w/o ASIO
- RealSense multi-client TCP sending
- Timestamped recording stream for offline capture
- Python helper tools for recording and raw-stream debugging


## How to

- Setup necessary environment on Orin

### Current default build

The current `Makefile` is configured for the RealSense sender and builds
`main_realsense_multi_client.cpp` by default.

- Default source: `main_realsense_multi_client.cpp`
- Single-target RealSense variant: `main_realsense_tcp.cpp`
- Legacy Webcam/ZED entrypoints are still in the repo and can be selected by
  editing `SRCS` in `Makefile`

### Dependencies

```bash
sudo apt-get install libzmq3-dev librealsense2-dev ffmpeg
```

`ffmpeg` is only needed by `recorder.py` for frame decoding.

### Build

```bash
make
./OrinVideoSender --help
```

### RealSense direct-send mode

Direct mode can send one legacy H.264 stream to Pico/Viewer and one
timestamped stream to a recorder client at the same time.

```bash
# 1) Start the recorder on the receiving machine (For testing)
python3 recorder.py --host 0.0.0.0 --port 12345 --output recordings

# 2) Start the sender on Orin
./OrinVideoSender \
    --send \
    --pico 192.168.8.100 \
    --port 12345 \
    --client_ip 192.168.123.164 \
    --client_port 12345 \
    --preview
```

The helper script below wraps the same workflow; update the IPs first:

```bash
./open_image_sender.sh
```

Direct-mode parameters:

- `--pico` / `--port`: target for the legacy H.264 receiver
- `--client_ip` / `--client_port`: target for the timestamped recorder stream

### RealSense listen mode

Use listen mode when the sender should wait for control commands from
Unity/VR:

```bash
# 192.168.1.153 is the Orin IP address
./OrinVideoSender --listen 192.168.1.153:13579 --preview
```

### Legacy webcam / ZED workflows

If you switch `SRCS` back to a legacy source file, the original workflows
still apply:

```bash
# send the video stream to both VR via TCP and my own ubuntu via ZMQ
./OrinVideoSender --listen 192.168.1.153:13579 --zmq tcp://*:5555

# Direct send the video stream
# 192.168.1.176 is the VR headset IP
./OrinVideoSender --send --server 192.168.1.176 --port 12345
```

## Python helpers

### `recorder.py`

- Receives timestamped TCP packets in the format
  `[8B timestamp BE][4B len BE][payload]`
- Decodes H.264 / H.265 with `ffmpeg`
- Saves decoded frames as `jpg` / `png`
- Can optionally display decoded frames live

```bash
python3 recorder.py --host 0.0.0.0 --port 12345 --output recordings --display
```

### `zmq_receiver.py`

- Debug tool for raw-image ZMQ streams
- Supports simple display mode and a basic image-processing mode
- Requires Python packages `pyzmq`, `opencv-python`, and `numpy`

```bash
python3 -m pip install pyzmq opencv-python numpy
python3 zmq_receiver.py
```

Update `SERVER_IP` in the script before running it.

## One More Thing 

- For software encoding ffmpeg, please refer to [RobotVisionTest](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/RobotVisionTest).

> Note: Hardware ffmpeg encoding is not availalbe yet.

> Note: Jetson Multimedia API is not in use yet.

- For encoded h264 stream receiver, please refer to [VideoPlayer](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/VideoPlayer) [TCP Only].

- For a general video player, please refer to [Video-Viewer](https://github.com/XR-Robotics/XRoboToolkit-Native-Video-Viewer) [TCP/UDP].

- The encoded h264 stream can be also played in [Unity-Client](https://github.com/XR-Robotics/XRoboToolkit-Unity-Client) [TCP Only].

## Realsense camera

- Integration note: https://github.com/XR-Robotics/XRoboToolkit-Unity-Client/issues/6
- Main entrypoints:
  - `main_realsense_tcp.cpp`: single-target TCP sender
  - `main_realsense_multi_client.cpp`: direct send to Pico plus timestamped
    recorder target
- Current direct-send defaults in code: `640x480 @ 60 FPS`, H.264, `10 Mbps`
