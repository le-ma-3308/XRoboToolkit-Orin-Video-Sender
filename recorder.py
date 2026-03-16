#!/usr/bin/env python3
"""
Video Recorder Script
Receives timestamped video stream from the C++ sender and records it.
Protocol: [8B timestamp BE][4B len BE][payload]
"""

import socket
import struct
import cv2
import numpy as np
import argparse
import os
import time
import subprocess
import tempfile
from datetime import datetime
from pathlib import Path


class VideoRecorder:
    def __init__(self, host='', port=12345, output_dir='recordings', 
                 save_frames=True, display=False, codec='H264', image_format='jpg'):
        self.host = host
        self.port = port
        self.output_dir = Path(output_dir)
        self.save_frames = save_frames
        self.display = display
        self.codec = codec.upper()
        self.image_format = image_format.lower()
        
        # Create output directory
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Socket
        self.server_sock = None  # Server socket for listening
        self.sock = None  # Client socket after accepting connection
        self.connected = False
        
        # Frame counter
        self.frame_count = 0
        self.start_time = None
        
        # Statistics
        self.stats = {
            'frames_received': 0,
            'frames_saved': 0,
            'bytes_received': 0,
            'errors': 0,
            'start_timestamp': None,
            'last_timestamp': None
        }
        
        # Check if ffmpeg is available
        self.ffmpeg_available = self._check_ffmpeg()
        if not self.ffmpeg_available:
            print("Warning: ffmpeg not found. Frame decoding may not work properly.")
            print("Install ffmpeg: sudo apt-get install ffmpeg")
    
    def listen(self):
        """Listen for incoming connection from the video sender (acts as TCP server)"""
        try:
            # Create server socket
            self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Bind to the port (listen on all interfaces if host is empty, otherwise bind to specific IP)
            bind_address = '' if not self.host or self.host == '0.0.0.0' else self.host
            self.server_sock.bind((bind_address, self.port))
            self.server_sock.listen(1)  # Listen for 1 connection
            
            print(f"Listening on {self.host if self.host else '0.0.0.0'}:{self.port}...")
            print("Waiting for sender to connect...")
            
            # Accept connection from sender
            self.sock, client_address = self.server_sock.accept()
            self.sock.settimeout(10.0)  # 10 second timeout for operations
            self.connected = True
            print(f"Sender connected from {client_address[0]}:{client_address[1]}")
            return True
        except Exception as e:
            print(f"Failed to listen/accept connection: {e}")
            self.connected = False
            return False
    
    def _check_ffmpeg(self):
        """Check if ffmpeg is available"""
        try:
            subprocess.run(['ffmpeg', '-version'], 
                          stdout=subprocess.DEVNULL, 
                          stderr=subprocess.DEVNULL,
                          timeout=2)
            return True
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False
    
    def disconnect(self):
        """Disconnect from the sender"""
        self.connected = False
        if self.sock:
            try:
                self.sock.close()
            except:
                pass
            self.sock = None
        if hasattr(self, 'server_sock') and self.server_sock:
            try:
                self.server_sock.close()
            except:
                pass
            self.server_sock = None
        print("Disconnected")
    
    def _read_exact(self, n_bytes):
        """Read exactly n_bytes from socket"""
        data = b''
        while len(data) < n_bytes:
            try:
                chunk = self.sock.recv(n_bytes - len(data))
                if not chunk:
                    raise ConnectionError("Connection closed by sender")
                data += chunk
            except socket.timeout:
                raise TimeoutError("Socket timeout while reading")
            except Exception as e:
                raise ConnectionError(f"Error reading from socket: {e}")
        return data
    
    def _parse_timestamp(self, timestamp_bytes):
        """Parse 8-byte big-endian timestamp to milliseconds"""
        return struct.unpack('>Q', timestamp_bytes)[0]  # >Q = big-endian unsigned long long
    
    def _parse_length(self, length_bytes):
        """Parse 4-byte big-endian length"""
        return struct.unpack('>I', length_bytes)[0]  # >I = big-endian unsigned int
    
    def _init_video_writer(self, width=640, height=480, fps=30):
        """Initialize video writer for saving frames"""
        if not self.save_frames:
            return
        
        timestamp_str = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_file = self.output_dir / f"recording_{timestamp_str}.mp4"
        
        # Try to use the same codec for writing
        try:
            self.video_writer = cv2.VideoWriter(
                str(output_file),
                self.fourcc,
                fps,
                (width, height)
            )
            if not self.video_writer.isOpened():
                # Fallback to XVID if codec not available
                print(f"Warning: {self.codec} codec not available, using XVID")
                self.video_writer = cv2.VideoWriter(
                    str(output_file),
                    cv2.VideoWriter_fourcc(*'XVID'),
                    fps,
                    (width, height)
                )
            print(f"Recording to: {output_file}")
        except Exception as e:
            print(f"Failed to initialize video writer: {e}")
            self.video_writer = None
    
    def _decode_frame_ffmpeg(self, encoded_data):
        """Decode H.264/H.265 frame using ffmpeg"""
        if not self.ffmpeg_available:
            return None
        
        try:
            # Determine codec
            codec_name = 'h264' if self.codec == 'H264' else 'hevc'
            
            # Use ffmpeg to decode the frame
            # Create a pipe to ffmpeg
            cmd = [
                'ffmpeg',
                '-f', codec_name,
                '-i', 'pipe:0',  # Read from stdin
                '-frames:v', '1',  # Decode only one frame
                '-f', 'image2pipe',  # Output as image
                '-vcodec', 'png',  # Output PNG format
                '-pix_fmt', 'bgr24',  # BGR format for OpenCV
                'pipe:1'  # Write to stdout
            ]
            
            process = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            
            stdout, stderr = process.communicate(input=encoded_data, timeout=5)
            
            if process.returncode != 0:
                return None
            
            # Decode PNG from stdout
            nparr = np.frombuffer(stdout, np.uint8)
            frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            return frame
            
        except Exception as e:
            return None
    
    def _decode_frame_opencv(self, encoded_data):
        """Try to decode frame using OpenCV (may not work for raw H.264/H.265)"""
        try:
            # This is a fallback - OpenCV may not decode raw H.264/H.265 NAL units
            # But we can try with VideoCapture
            nparr = np.frombuffer(encoded_data, np.uint8)
            # This won't work for H.264/H.265, but we try anyway
            return None
        except:
            return None
    
    def receive_and_process(self):
        """Main loop: receive packets and process them"""
        if not self.connected:
            print("Not connected. Call connect() first.")
            return
        
        print("Starting to receive video stream...")
        print("Press Ctrl+C to stop recording")
        
        buffer = b''
        expected_header_size = 12  # 8 bytes timestamp + 4 bytes length
        
        try:
            while self.connected:
                try:
                    # Read header (timestamp + length)
                    if len(buffer) < expected_header_size:
                        data = self.sock.recv(4096)
                        if not data:
                            print("Connection closed by sender")
                            break
                        buffer += data
                    
                    # Check if we have enough for header
                    if len(buffer) < expected_header_size:
                        continue
                    
                    # Parse timestamp (8 bytes, big-endian)
                    timestamp_bytes = buffer[0:8]
                    timestamp_ms = self._parse_timestamp(timestamp_bytes)
                    
                    # Parse payload length (4 bytes, big-endian)
                    length_bytes = buffer[8:12]
                    payload_length = self._parse_length(length_bytes)
                    
                    # Check if payload length is reasonable
                    if payload_length > 10 * 1024 * 1024:  # 10MB max
                        print(f"Warning: Suspicious payload length: {payload_length} bytes")
                        buffer = buffer[expected_header_size:]
                        continue
                    
                    # Read payload
                    total_needed = expected_header_size + payload_length
                    if len(buffer) < total_needed:
                        # Need to read more data
                        remaining = total_needed - len(buffer)
                        data = self._read_exact(remaining)
                        buffer += data
                    
                    # Extract payload
                    payload = buffer[expected_header_size:total_needed]
                    buffer = buffer[total_needed:]
                    
                    # Process the frame
                    self._process_frame(timestamp_ms, payload)
                    
                    # Update statistics
                    self.stats['frames_received'] += 1
                    self.stats['bytes_received'] += len(payload)
                    if self.stats['start_timestamp'] is None:
                        self.stats['start_timestamp'] = timestamp_ms
                    self.stats['last_timestamp'] = timestamp_ms
                    
                except TimeoutError:
                    print("Timeout waiting for data")
                    continue
                except ConnectionError as e:
                    print(f"Connection error: {e}")
                    break
                except Exception as e:
                    print(f"Error processing frame: {e}")
                    self.stats['errors'] += 1
                    import traceback
                    traceback.print_exc()
                    continue
                    
        except KeyboardInterrupt:
            print("\nStopping recording...")
        finally:
            self._print_statistics()
            self.disconnect()
    
    def _process_frame(self, timestamp_ms, encoded_data):
        """Process a received frame - decode and save as image"""
        # Convert timestamp to readable format for filename
        # Use timestamp in milliseconds as filename
        timestamp_str = f"{timestamp_ms}"
        
        if self.frame_count == 0:
            self.start_time = time.time()
            readable_time = datetime.fromtimestamp(timestamp_ms / 1000.0).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            print(f"First frame received at timestamp: {readable_time} ({timestamp_ms} ms)")
        
        # Decode the frame
        frame = None
        if self.save_frames or self.display:
            # Try ffmpeg first (most reliable)
            if self.ffmpeg_available:
                frame = self._decode_frame_ffmpeg(encoded_data)
            
            # Fallback to OpenCV if ffmpeg didn't work
            if frame is None:
                frame = self._decode_frame_opencv(encoded_data)
        
        # Save decoded frame as image
        if self.save_frames and frame is not None:
            # Create filename with timestamp
            filename = f"{timestamp_str}.{self.image_format}"
            image_path = self.output_dir / filename
            
            # Save image
            if self.image_format == 'jpg' or self.image_format == 'jpeg':
                cv2.imwrite(str(image_path), frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
            elif self.image_format == 'png':
                cv2.imwrite(str(image_path), frame)
            else:
                # Default to jpg
                image_path = self.output_dir / f"{timestamp_str}.jpg"
                cv2.imwrite(str(image_path), frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
            
            self.stats['frames_saved'] += 1
        
        # Display frame if requested
        if self.display and frame is not None:
            cv2.imshow('Video Stream', frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                print("Display window closed by user")
                self.connected = False
        
        # Display frame info
        if self.frame_count % 30 == 0:  # Print every 30 frames
            elapsed = time.time() - self.start_time if self.start_time else 0
            fps = self.frame_count / elapsed if elapsed > 0 else 0
            decode_status = "decoded" if frame is not None else "failed"
            print(f"Frame {self.frame_count}: timestamp={timestamp_str}, "
                  f"size={len(encoded_data)} bytes, fps={fps:.2f}, decode={decode_status}")
        
        self.frame_count += 1
    
    def _print_statistics(self):
        """Print recording statistics"""
        print("\n" + "="*50)
        print("Recording Statistics:")
        print("="*50)
        print(f"Frames received: {self.stats['frames_received']}")
        print(f"Frames saved: {self.stats['frames_saved']}")
        print(f"Total bytes: {self.stats['bytes_received']:,} ({self.stats['bytes_received']/1024/1024:.2f} MB)")
        print(f"Errors: {self.stats['errors']}")
        if self.stats['start_timestamp'] and self.stats['last_timestamp']:
            duration_ms = self.stats['last_timestamp'] - self.stats['start_timestamp']
            duration_sec = duration_ms / 1000.0
            print(f"Duration: {duration_sec:.2f} seconds")
            if duration_sec > 0:
                avg_fps = self.stats['frames_received'] / duration_sec
                print(f"Average FPS: {avg_fps:.2f}")
        print(f"Output directory: {self.output_dir}")
        print("="*50)


def main():
    parser = argparse.ArgumentParser(description='Video Recorder - Receives timestamped video stream')
    parser.add_argument('--host', type=str, default='',
                        help='IP address to bind to (default: empty = listen on all interfaces)')
    parser.add_argument('--port', type=int, default=12345,
                        help='Port to listen on (default: 12345)')
    parser.add_argument('--output', type=str, default='recordings',
                        help='Output directory for recordings (default: recordings)')
    parser.add_argument('--save', action='store_true', default=True,
                        help='Save frames to files (default: True)')
    parser.add_argument('--no-save', dest='save', action='store_false',
                        help='Do not save frames')
    parser.add_argument('--display', action='store_true',
                        help='Display frames (requires decoded frames)')
    parser.add_argument('--codec', type=str, default='H264', choices=['H264', 'H265', 'HEVC'],
                        help='Video codec (default: H264)')
    parser.add_argument('--format', type=str, default='jpg', choices=['jpg', 'jpeg', 'png'],
                        help='Image format for saved frames (default: jpg)')
    
    args = parser.parse_args()
    
    recorder = VideoRecorder(
        host=args.host,
        port=args.port,
        output_dir=args.output,
        save_frames=args.save,
        display=args.display,
        codec=args.codec,
        image_format=args.format
    )
    
    if recorder.listen():
        recorder.receive_and_process()
    else:
        print("Failed to listen/accept connection. Exiting.")
        return 1
    
    return 0


if __name__ == '__main__':
    exit(main())

