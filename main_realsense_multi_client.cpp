// main_realsense.cpp
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <openssl/md5.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <librealsense2/rs.hpp> // <-- RealSense SDK

#include "network_helper.hpp"

// Network Protocol Structures (unchanged)
struct CameraRequestData {
  int width;
  int height;
  int fps;
  int bitrate;
  int enableMvHevc;
  int renderMode;
  int port;
  std::string camera;
  std::string ip;

  CameraRequestData()
      : width(0), height(0), fps(0), bitrate(0), enableMvHevc(0), renderMode(0),
        port(0) {}
};

struct NetworkDataProtocol {
  std::string command;
  int length;
  std::vector<uint8_t> data;

  NetworkDataProtocol() : length(0) {}
  NetworkDataProtocol(const std::string &cmd, const std::vector<uint8_t> &d)
      : command(cmd), data(d), length(d.size()) {}
};

// Deserializers (unchanged)
class CameraRequestDeserializer {
public:
  static CameraRequestData deserialize(const std::vector<uint8_t> &data) {
    if (data.size() < 10) {
      throw std::invalid_argument("Data is too small for valid camera request");
    }

    size_t offset = 0;

    // Check magic bytes (0xCA, 0xFE)
    if (data[offset] != 0xCA || data[offset + 1] != 0xFE) {
      throw std::invalid_argument("Invalid magic bytes");
    }
    offset += 2;

    // Check protocol version
    uint8_t version = data[offset++];
    if (version != 1) {
      throw std::invalid_argument("Unsupported protocol version");
    }

    CameraRequestData result;

    // Read integer fields (7 * 4 bytes)
    if (offset + 28 > data.size()) {
      throw std::invalid_argument("Data too small for integer fields");
    }

    result.width = readInt32(data, offset);
    result.height = readInt32(data, offset + 4);
    result.fps = readInt32(data, offset + 8);
    result.bitrate = readInt32(data, offset + 12);
    result.enableMvHevc = readInt32(data, offset + 16);
    result.renderMode = readInt32(data, offset + 20);
    result.port = readInt32(data, offset + 24);
    offset += 28;

    // Read strings with compact encoding
    result.camera = readCompactString(data, offset);
    result.ip = readCompactString(data, offset);

    return result;
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }

    // Little-endian format (matching C# BitConverter default)
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }

  static std::string readCompactString(const std::vector<uint8_t> &data,
                                       size_t &offset) {
    if (offset >= data.size()) {
      throw std::out_of_range("Not enough data to read string length");
    }

    uint8_t length = data[offset++];
    if (length == 0) {
      return std::string();
    }

    if (offset + length > data.size()) {
      throw std::out_of_range("Not enough data to read string content");
    }

    std::string result(reinterpret_cast<const char *>(&data[offset]), length);
    offset += length;
    return result;
  }
};

class NetworkDataProtocolDeserializer {
public:
  static NetworkDataProtocol deserialize(const std::vector<uint8_t> &buffer) {
    if (buffer.size() < 8) {
      throw std::invalid_argument("Buffer too small for valid protocol data");
    }

    size_t offset = 0;

    int32_t commandLength = readInt32(buffer, offset);
    offset += 4;

    if (commandLength < 0 || offset + commandLength > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }

    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;

    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }

    int32_t dataLength = readInt32(buffer, offset);
    offset += 4;

    if (dataLength < 0 || offset + dataLength > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }

    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }

    return NetworkDataProtocol(command, data);
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }

    // Little-endian format
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }
};

// Global camera configuration
CameraRequestData current_camera_config;

// Thread-safe global state
std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> encoding_enabled{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};

// Thread management
std::unique_ptr<std::thread> listen_thread;
std::unique_ptr<std::thread> streaming_thread;
std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;
std::mutex senders_mutex;

// Network components
std::vector<std::unique_ptr<TCPClient>> senders;
std::vector<bool> senders_need_timestamp; // parallel vector for include_timestamp
std::vector<int> senders_frame_skip;      // frame skip ratio per client
std::vector<int> senders_frame_counter;   // current frame counter per client
std::unique_ptr<TCPServer> server_ptr;
std::string send_to_pico = "192.168.8.100";  // default: localhost
int send_to_port = 12345;  // default port
struct Target {
  std::string ip;
  int port;
  bool include_timestamp; // true => send [8B timestamp BE][4B len BE][payload]
  int frame_skip;  // 1 = every frame (60Hz), 2 = every other frame (30Hz), etc.

  // default constructor
  Target() : ip(), port(0), include_timestamp(false), frame_skip(1) {}

  Target(const std::string &ip_, int port_, bool incl_ts = false, int skip = 1)
      : ip(ip_), port(port_), include_timestamp(incl_ts), frame_skip(skip) {}
};
std::vector<Target> senders_targets; // 与 senders, senders_need_timestamp 平行，记录每个已连接 sender 对应的 Target
std::unique_ptr<std::thread> connect_monitor_thread;

// initialize empty; we'll populate according to CLI or control messages
std::vector<Target> targets;


bool initialize_sender() {
    int retry = 1000;

    while (retry > 0 && !stop_requested.load()) {

        // build local copy of targets to avoid holding lock during connect attempts
        std::vector<Target> local_targets;
        {
            std::lock_guard<std::mutex> lock(senders_mutex);
            local_targets = targets;
        }

        std::vector<std::unique_ptr<TCPClient>> new_senders;
        std::vector<bool> new_senders_need_ts; // parallel vector for include_timestamp
        std::vector<int> new_senders_skip;     // parallel vector for frame_skip
        std::vector<int> new_senders_counter;  // parallel vector for frame_counter
        bool any_connected = false;

        for (auto &t : local_targets) {
            try {
                std::unique_ptr<TCPClient> client(new TCPClient(t.ip, t.port));
                std::cout << "Connecting to " << t.ip << ":" << t.port << std::endl;
                client->connect();
                new_senders.push_back(std::move(client));
                new_senders_need_ts.push_back(t.include_timestamp);
                new_senders_skip.push_back(t.frame_skip);
                new_senders_counter.push_back(0);  // initialize counter to 0
                any_connected = true;
            }
            catch (const TCPException &e) {
                std::cerr << "Failed to connect to " << t.ip << ":" << t.port
                          << " - " << e.what() << std::endl;
                // Do not treat as fatal for other targets; continue trying others
            }
        }

        if (any_connected) {
            // swap in the successfully connected clients under lock
            {
                std::lock_guard<std::mutex> lock(senders_mutex);
                senders.swap(new_senders);
                // store flags in parallel container; ensure same order
                senders_need_timestamp = new_senders_need_ts;
                senders_frame_skip = new_senders_skip;
                senders_frame_counter = new_senders_counter;
            }
            return true;
        }

        // no client connected this attempt; retry
        std::this_thread::sleep_for(std::chrono::seconds(1));
        retry--;
    }

    return false;
}

template <typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// Forward declarations
void handleOpenCamera(const std::vector<uint8_t> &data);
void handleCloseCamera(const std::vector<uint8_t> &data);
void startStreamingThread();
void stopStreamingThread();
void streamingThreadFunction();
void listenThreadFunction(const std::string &listen_address);

void onDataCallback(const std::string &command) {
  std::vector<uint8_t> binaryData(command.begin(), command.end());

  for (size_t i = 0; i < std::min(binaryData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(binaryData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  if (binaryData.size() < 4) {
    std::cerr << "Data too small to contain length header" << std::endl;
    return;
  }

  uint32_t bodyLength = (static_cast<uint32_t>(binaryData[0]) << 24) |
                        (static_cast<uint32_t>(binaryData[1]) << 16) |
                        (static_cast<uint32_t>(binaryData[2]) << 8) |
                        static_cast<uint32_t>(binaryData[3]);

  if (4 + bodyLength > binaryData.size()) {
    std::cerr << "Data too small for declared body length. Expected: "
              << (4 + bodyLength) << ", got: " << binaryData.size()
              << std::endl;
    return;
  }

  std::vector<uint8_t> protocolData(binaryData.begin() + 4,
                                    binaryData.begin() + 4 + bodyLength);

  for (size_t i = 0; i < std::min(protocolData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(protocolData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  try {
    if (protocolData.size() >= 8) {
      int32_t cmdLen = (protocolData[0]) | (protocolData[1] << 8) |
                       (protocolData[2] << 16) | (protocolData[3] << 24);
      int32_t dataLenPos = 4 + cmdLen;
      std::cout << "Command length: " << cmdLen << std::endl;
      if (static_cast<size_t>(dataLenPos + 4) <= protocolData.size()) {
        int32_t dataLen = (protocolData[dataLenPos]) |
                          (protocolData[dataLenPos + 1] << 8) |
                          (protocolData[dataLenPos + 2] << 16) |
                          (protocolData[dataLenPos + 3] << 24);
        std::cout << "Data length: " << dataLen << std::endl;
      }
    }

    NetworkDataProtocol protocol =
        NetworkDataProtocolDeserializer::deserialize(protocolData);

    std::cout << "Received protocol command: '" << protocol.command
              << "' (length: " << protocol.command.length() << ")" << std::endl;

    if (protocol.command == "OPEN_CAMERA") {
      handleOpenCamera(protocol.data);
    } else if (protocol.command == "CLOSE_CAMERA") {
      handleCloseCamera(protocol.data);
    } else {
      std::cout << "Unknown protocol command: " << protocol.command
                << std::endl;
    }
    return;
  } catch (const std::exception &e) {
    std::cout << "Failed to parse as NetworkDataProtocol: " << e.what()
              << std::endl;
  }
}

void onDisconnectCallback() {
  std::cout << "Client disconnected, stopping streaming" << std::endl;
  stopStreamingThread();
}

void listenThreadFunction(const std::string &listen_address) {
  std::cout << "Listen thread started on " << listen_address << std::endl;

  while (!stop_requested.load()) {
    try {
      server_ptr = make_unique_helper<TCPServer>(listen_address);
      server_ptr->setDataCallback(onDataCallback);
      server_ptr->setDisconnectCallback(onDisconnectCallback);
      server_ptr->start();
      std::cout << "TCPServer is listening on " << listen_address << std::endl;

      while (!stop_requested.load() && server_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (server_ptr) {
        server_ptr->stop();
        server_ptr = nullptr;
      }

      if (!stop_requested.load()) {
        std::cout << "Waiting for new connection..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

    } catch (const std::exception &e) {
      std::cerr << "Listen thread error: " << e.what() << std::endl;
      if (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
  }

  std::cout << "Listen thread stopped" << std::endl;
}

void handle_sigint(int) {
  std::cout << "\nSIGINT received. Stopping all threads..." << std::endl;
  stop_requested.store(true);

  stopStreamingThread();

  if (server_ptr) {
    server_ptr->stop();
    server_ptr = nullptr;
  }

  streaming_cv.notify_all();
}

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  (void)user_data;
  
  // Early exit check to avoid processing when disabled
  if (!send_enabled.load() || stop_requested.load()) {
    // Still need to pull and release the sample to avoid pipeline blocking
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample) gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;

    // read PTS from buffer (ns) and convert to ms
    guint64 pts_ns = GST_BUFFER_PTS(buffer);
    uint64_t timestamp_ms = gst_util_uint64_scale(pts_ns, 1, GST_MSECOND);

    if (data && size > 0) {
      try {
        // Snapshot current senders and their flags under lock (minimize lock time)
        std::vector<TCPClient*> local_clients;
        std::vector<bool> local_need_ts;
        std::vector<int> local_frame_skip;
        std::vector<int> local_frame_counter;  // Copy counters locally to avoid locking in send loop
        {
          std::lock_guard<std::mutex> lock(senders_mutex);
          if (senders.empty()) {
            // No clients to send to, release and return
            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
            return GST_FLOW_OK;
          }
          local_clients.reserve(senders.size());
          for (size_t i = 0; i < senders.size(); ++i) {
            if (senders[i] && senders[i]->isConnected()) {
              local_clients.push_back(senders[i].get());
              bool need_ts = (i < senders_need_timestamp.size()) ? senders_need_timestamp[i] : false;
              local_need_ts.push_back(need_ts);
              int skip = (i < senders_frame_skip.size()) ? senders_frame_skip[i] : 1;
              local_frame_skip.push_back(skip);
              // Increment and copy frame counter for this client
              if (i < senders_frame_counter.size()) {
                senders_frame_counter[i]++;
                local_frame_counter.push_back(senders_frame_counter[i]);
              } else {
                local_frame_counter.push_back(1);
              }
            }
          }
        }

        // If no connected clients, skip sending
        if (local_clients.empty()) {
          gst_buffer_unmap(buffer, &map);
          gst_sample_unref(sample);
          return GST_FLOW_OK;
        }

        // For each client build and send corresponding packet
        // Use try-catch around individual sends to prevent one failure from blocking others
        std::vector<size_t> failed_indices;
        static std::atomic<int> send_count{0};
        static std::atomic<int> drop_count{0};
        
        for (size_t i = 0; i < local_clients.size(); ++i) {
          TCPClient *c = local_clients[i];
          if (!c || !c->isConnected()) {
            failed_indices.push_back(i);
            continue;
          }
          
          // Frame skip logic: use locally cached counter (no locking needed)
          int skip = (i < local_frame_skip.size()) ? local_frame_skip[i] : 1;
          int counter = (i < local_frame_counter.size()) ? local_frame_counter[i] : 1;
          if (skip > 1 && (counter % skip) != 0) {
            continue;  // Skip this frame for this client
          }
          
          try {
            if (local_need_ts[i]) {
              // build [8B ts BE][4B len BE][payload]
              size_t header_size = 8 + 4;
              std::vector<uint8_t> packet(header_size + size);
              // 8B timestamp big-endian
              for (int b = 0; b < 8; ++b) {
                packet[b] = static_cast<uint8_t>((timestamp_ms >> (56 - 8*b)) & 0xFF);
              }
              uint32_t payload_len = static_cast<uint32_t>(size);
              packet[8] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
              packet[9] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
              packet[10] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
              packet[11] = static_cast<uint8_t>((payload_len) & 0xFF);
              std::copy(data, data + size, packet.begin() + header_size);
              
              // Send with timeout (already set in TCPClient::connect, 5 seconds)
              c->sendData(packet);
              send_count++;
            } else {
              // legacy: [4B len BE][payload]
              uint32_t payload_len = static_cast<uint32_t>(size);
              std::vector<uint8_t> packet(4 + size);
              packet[0] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
              packet[1] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
              packet[2] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
              packet[3] = static_cast<uint8_t>((payload_len) & 0xFF);
              std::copy(data, data + size, packet.begin() + 4);
              
              c->sendData(packet);
              send_count++;
            }
          } catch (const TCPException &e) {
            // Network errors: mark as failed but don't log every time to avoid spam
            if (send_count.load() % 100 == 0 || failed_indices.empty()) {
              std::cerr << "[Send] TCP error to client " << i << ": " << e.what() << std::endl;
            }
            failed_indices.push_back(i);
          } catch (const std::exception &e) {
            std::cerr << "[Send] Unexpected error to client " << i << ": " << e.what() << std::endl;
            failed_indices.push_back(i);
          }
        }

        // Remove failed clients from global senders (match by pointer identity)
        if (!failed_indices.empty()) {
          std::lock_guard<std::mutex> lock(senders_mutex);
          std::vector<std::unique_ptr<TCPClient>> survivors;
          std::vector<bool> survivors_flags;
          std::vector<int> survivors_skip;
          std::vector<int> survivors_counter;
          std::vector<Target> survivors_targets;
          survivors.reserve(senders.size());
          for (size_t idx = 0; idx < senders.size(); ++idx) {
            if (!senders[idx]) continue;
            TCPClient* ptr = senders[idx].get();
            bool failed = false;
            for (size_t fi : failed_indices) {
              if (fi < local_clients.size() && local_clients[fi] == ptr) {
                failed = true; break;
              }
            }
            if (!failed) {
              // move client to survivors
              survivors.push_back(std::move(senders[idx]));
              bool flag = (idx < senders_need_timestamp.size()) ? senders_need_timestamp[idx] : false;
              survivors_flags.push_back(flag);
              int skip = (idx < senders_frame_skip.size()) ? senders_frame_skip[idx] : 1;
              survivors_skip.push_back(skip);
              int counter = (idx < senders_frame_counter.size()) ? senders_frame_counter[idx] : 0;
              survivors_counter.push_back(counter);
              Target t;
              if (idx < senders_targets.size()) t = senders_targets[idx];
              survivors_targets.push_back(t);
            } else {
              try { 
                if (senders[idx]->isConnected()) {
                  senders[idx]->disconnect();
                }
              } catch(...) {}
            }
          }
          senders.swap(survivors);
          senders_need_timestamp.swap(survivors_flags);
          senders_frame_skip.swap(survivors_skip);
          senders_frame_counter.swap(survivors_counter);
          senders_targets.swap(survivors_targets);

          
          if (senders.empty() && send_count.load() > 0) {
            std::cerr << "[Send] All clients disconnected. Sent: " << send_count.load() 
                      << ", Dropped: " << drop_count.load() << std::endl;
          }
        }

        // Periodic statistics (every 300 frames ~ 10 seconds at 30fps)
        if (send_count.load() % 300 == 0 && send_count.load() > 0) {
          std::cout << "[Stats] Frames sent: " << send_count.load() 
                    << ", Active clients: " << local_clients.size() << std::endl;
        }

      } catch (const std::exception &e) {
        std::cerr << "[Send] Top-level error: " << e.what() << std::endl;
        // Don't stop streaming on isolated errors
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

void handleOpenCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling OPEN_CAMERA command" << std::endl;

  try {
    CameraRequestData cameraConfig =
        CameraRequestDeserializer::deserialize(data);

    std::cout << "Camera config - Width: " << cameraConfig.width
              << ", Height: " << cameraConfig.height
              << ", FPS: " << cameraConfig.fps
              << ", Bitrate: " << cameraConfig.bitrate
              << ", IP: " << cameraConfig.ip << ", Port: " << cameraConfig.port
              << ", type: " << cameraConfig.camera << std::endl;
    if (cameraConfig.camera != "REALSENSE" && cameraConfig.camera != "RS") {
      std::cout << "Unsupported camera type: " << cameraConfig.camera
                << ". Only 'REALSENSE' or 'RS' is supported." << std::endl;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config = cameraConfig;
    }

    // Update targets (single target) in a thread-safe manner
    {
      std::lock_guard<std::mutex> lock(senders_mutex);
      targets.clear();
      targets.push_back({cameraConfig.ip, cameraConfig.port, false, 2});  // default 30Hz
    }

    // Optionally restart streaming thread (if you prefer immediate reconnect)
    stopStreamingThread();
    startStreamingThread();

  } catch (const std::exception &e) {
    std::cerr << "Failed to parse camera config: " << e.what() << std::endl;
    // fallback: if there is already CLI server/port, use them
    if (!send_to_pico.empty() && send_to_port > 0) {
      std::lock_guard<std::mutex> lock(senders_mutex);
      targets.clear();
      targets.push_back({send_to_pico, send_to_port, false, 2});  // default 30Hz
      startStreamingThread();
    } else {
      std::cerr << "No valid pico configuration available, cannot start streaming"
                << std::endl;
    }
  }
}

void handleCloseCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling CLOSE_CAMERA command" << std::endl;
  stopStreamingThread();
}

// Attempt to connect missing targets in the background and add them to senders
void connectMonitorThreadFunction() {
  const int retry_interval_ms = 1000; // retry every 1s
  std::cout << "Connect monitor thread started" << std::endl;
  while (streaming_active.load() && !stop_requested.load()) {
      // Snapshot targets under lock
      std::vector<Target> local_targets;
      {
          std::lock_guard<std::mutex> lock(senders_mutex);
          local_targets = targets;
      }

      // For each target, if not already connected, try to connect
      for (const auto &t : local_targets) {
          if (stop_requested.load() || !streaming_active.load()) break;

          bool already_connected = false;
          {
              std::lock_guard<std::mutex> lock(senders_mutex);
              for (size_t i = 0; i < senders_targets.size(); ++i) {
                  if (senders_targets[i].ip == t.ip && senders_targets[i].port == t.port) {
                      // If we have an entry, ensure the corresponding client is connected
                      if (i < senders.size() && senders[i] && senders[i]->isConnected()) {
                          already_connected = true;
                          break;
                      } else {
                          // entry exists but client not connected: remove it to allow reconnect
                          // We'll remove outside of this inner block to avoid invalidating indices
                          // Mark for removal by setting ip empty (will be cleaned below)
                      }
                  }
              }
          }
          if (already_connected) continue;

          // Try connecting to this target
          try {
              std::unique_ptr<TCPClient> client(new TCPClient(t.ip, t.port));
              std::cout << "Connect monitor: attempting to connect to " << t.ip << ":" << t.port << std::endl;
              client->connect();
              if (client->isConnected()) {
                  std::lock_guard<std::mutex> lock(senders_mutex);
                  // Double-check not already inserted by another attempt
                  bool duplicated = false;
                  for (const auto &et : senders_targets) {
                      if (et.ip == t.ip && et.port == t.port) { duplicated = true; break; }
                  }
                  if (!duplicated) {
                      senders.push_back(std::move(client));
                      senders_need_timestamp.push_back(t.include_timestamp);
                      senders_frame_skip.push_back(t.frame_skip);
                      senders_frame_counter.push_back(0);
                      senders_targets.push_back(t);
                      std::cout << "Connect monitor: connected to " << t.ip << ":" << t.port << std::endl;
                  } else {
                      // If duplicated, the client we created is unused and will be freed on scope exit
                      std::cout << "Connect monitor: target already connected by another routine: " << t.ip << ":" << t.port << std::endl;
                  }
              }
          } catch (const TCPException &e) {
              // connection failed: ignore, will retry next loop
              // avoid spamming log: only occasionally log
              static int attempt_count = 0;
              attempt_count++;
              if (attempt_count % 30 == 0) {
                  std::cerr << "Connect monitor: failed to connect to " << t.ip << ":" << t.port << " - " << e.what() << std::endl;
              }
          } catch (const std::exception &e) {
              std::cerr << "Connect monitor unexpected error: " << e.what() << std::endl;
          }

          if (stop_requested.load() || !streaming_active.load()) break;
      }

      // Sleep before next attempt
      int slept = 0;
      const int step = 200;
      while (slept < retry_interval_ms && streaming_active.load() && !stop_requested.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(step));
          slept += step;
      }
  }
  std::cout << "Connect monitor thread exiting" << std::endl;
}

void startStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);
  if (streaming_thread && streaming_thread->joinable()) {
    std::cout << "Streaming thread already running" << std::endl;
    return;
  }

  streaming_active.store(true);
  streaming_thread = make_unique_helper<std::thread>(streamingThreadFunction);
  std::cout << "Started streaming thread" << std::endl;

  // Start connect monitor thread to attempt future connections without stopping streaming
  if (!connect_monitor_thread) {
    connect_monitor_thread = make_unique_helper<std::thread>(connectMonitorThreadFunction);
    std::cout << "Started connect monitor thread" << std::endl;
  }
}

void stopStreamingThread() {
  // Lock to synchronize with other code that may access senders/streaming_thread
  std::lock_guard<std::mutex> lock(streaming_mutex);

  // Signal thread to stop reading/encoding/sending
  streaming_active.store(false);
  encoding_enabled.store(false);
  send_enabled.store(false);
  // NOTE: Do NOT set stop_requested here - it affects the entire program

  // Disconnect and clear all sender clients (need senders_mutex)
  {
    std::lock_guard<std::mutex> senders_lock(senders_mutex);
    if (!senders.empty()) {
      for (auto &client : senders) {
        if (client) {
          try {
            if (client->isConnected()) {
              client->disconnect();
            }
          } catch (const std::exception &e) {
            std::cerr << "Exception while disconnecting a sender: " << e.what() << std::endl;
          } catch (...) {
            std::cerr << "Unknown exception while disconnecting a sender" << std::endl;
          }
        }
      }
      // Remove all clients
      senders.clear();
      senders.shrink_to_fit();
      senders_need_timestamp.clear();
      senders_frame_skip.clear();
      senders_frame_counter.clear();
      senders_targets.clear();
    }
  }

  // Notify thread(s) that might be waiting and join
  if (streaming_thread && streaming_thread->joinable()) {
    streaming_cv.notify_all();   // wake up any waiters inside the thread
    try {
      streaming_thread->join();
    } catch (const std::exception &e) {
      std::cerr << "Exception while joining streaming thread: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "Unknown exception while joining streaming thread" << std::endl;
    }
    streaming_thread.reset();
    std::cout << "Stopped streaming thread" << std::endl;
  }

  // join connect monitor thread if running
  if (connect_monitor_thread && connect_monitor_thread->joinable()) {
    try {
      connect_monitor_thread->join();
    } catch (const std::exception &e) {
      std::cerr << "Exception while joining connect monitor thread: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "Unknown exception while joining connect monitor thread" << std::endl;
    }
    connect_monitor_thread.reset();
    std::cout << "Stopped connect monitor thread" << std::endl;
  }
  
}

std::string buildPipelineString(const CameraRequestData &config,
                                bool preview_enabled) {
  std::string pipeline_str = "appsrc name=mysource is-live=true format=time ";

  pipeline_str +=
      "caps=video/x-raw,format=BGRA,width=" + std::to_string(config.width) +
      ",height=" + std::to_string(config.height) +
      ",framerate=" + std::to_string(config.fps) + "/1 ! ";

  pipeline_str += "videoconvert ! nvvidconv ! "
                  "video/x-raw(memory:NVMM),format=NV12 ! tee name=t ";

  std::string encoder = config.enableMvHevc ? "nvv4l2h265enc" : "nvv4l2h264enc";
  std::string parser = config.enableMvHevc ? "h265parse" : "h264parse";

  pipeline_str +=
      "t. ! queue max-size-buffers=10 max-size-time=166666666 leaky=downstream ! " + encoder + " maxperf-enable=1 insert-sps-pps=true ";
  pipeline_str +=
      "idrinterval=1 bitrate=" + std::to_string(config.bitrate) + " ! ";
  pipeline_str +=
      parser + " ! appsink name=mysink emit-signals=true sync=false max-buffers=4 drop=true ";

  if (preview_enabled) {
    pipeline_str +=
        "t. ! queue ! nvvidconv ! videoconvert ! autovideosink sync=false ";
  }

  return pipeline_str;
}

void updateRealSenseConfiguration(const CameraRequestData &config) {
  std::cout << "RealSense would be configured with:" << std::endl;
  std::cout << "  Resolution: " << config.width << "x" << config.height
            << std::endl;
  std::cout << "  FPS: " << config.fps << std::endl;
  std::cout << "  Bitrate: " << config.bitrate << std::endl;
  std::cout << "  HEVC: " << (config.enableMvHevc ? "enabled" : "disabled")
            << std::endl;
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  signal(SIGINT, handle_sigint);

  bool preview_enabled_local = false;
  bool listen_enabled = false;
  bool send_enabled_mode = false;
  std::string listen_address = "";

  std::string client_ip = "192.168.8.101";  // default: empty (optional)
  int client_port = 12345;  // default: 0 (optional)

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled_local = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--send") {
      send_enabled_mode = true;
    } else if (arg == "--pico" && i + 1 < argc) {
      send_to_pico = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      send_to_port = std::stoi(argv[++i]);
    } else if (arg == "--client_ip" && i + 1 < argc) {
      client_ip = argv[++i];
    } else if (arg == "--client_port" && i + 1 < argc) {
      client_port = std::stoi(argv[++i]);
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout << "  --preview      Enable video preview\n";
      std::cout << "  --listen ADDR  Listen to control commands on address "
                   "(IP:PORT)\n";
      std::cout << "  --send         Send video stream directly to pico\n";
      std::cout << "  --pico IP    pico IP address (default: 192.168.8.100)\n";
      std::cout << "  --port PORT    pico port (default: 12345)\n";
      std::cout << "  --client_ip IP Record IP address (default: 192.168.8.101)\n";
      std::cout << "  --client_port PORT Record port (default: 12345)\n";
      std::cout << "  --help         Show this help message\n";
      return 0;
    }
  }

  if (!listen_enabled && !send_enabled_mode) {
    std::cerr << "Error: Either --listen or --send option is required"
              << std::endl;
    std::cerr << "Use --help to see usage options" << std::endl;
    return -1;
  }

  // Use defaults if not provided via command line
  if (send_enabled_mode && send_to_pico.empty()) {
    send_to_pico = "192.168.8.100";  // use default
  }
  if (send_enabled_mode && send_to_port == 0) {
    send_to_port = 12345;  // use default
  }

  if (send_enabled_mode) {
    std::cout << "Starting direct video streaming to " << send_to_pico << ":"
              << send_to_port << "..." << std::endl;

    {
      std::lock_guard<std::mutex> lock(senders_mutex);
      targets.emplace_back(send_to_pico, send_to_port, false, 2); // legacy receiver: 30Hz (skip=2)
      // optionally add record target (with timestamp) if provided
      if (!client_ip.empty() && client_port > 0) {
        targets.emplace_back(client_ip, client_port, true, 2); // record receiver: 60Hz (skip=1)
      }
    }
    
    preview_enabled.store(preview_enabled_local);

    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config.width = 640;  // sensible default for RS color
      current_camera_config.height = 480;
      current_camera_config.fps = 60;
      current_camera_config.bitrate = 10000000;  // 10 Mbps for 60fps
      current_camera_config.enableMvHevc = 0;
      current_camera_config.renderMode = 0;
      current_camera_config.camera = "REALSENSE";
      current_camera_config.ip = send_to_pico;
      current_camera_config.port = send_to_port;
    }

    startStreamingThread();

    std::cout << "Streaming started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else if (listen_enabled) {
    std::cout << "Starting threaded video streaming server..." << std::endl;

    preview_enabled.store(preview_enabled_local);

    listen_thread =
        make_unique_helper<std::thread>(listenThreadFunction, listen_address);

    std::cout << "Server started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (listen_thread && listen_thread->joinable()) {
      listen_thread->join();
    }
  }

  std::cout << "Shutting down..." << std::endl;

  stopStreamingThread();

  std::cout << "All threads stopped. Exiting." << std::endl;
  return 0;
}

// Helper: convert rs2::video_frame -> cv::Mat (BGRA)
static cv::Mat rsFrameToCvMatBGRA(const rs2::video_frame &f) {
  int w = f.get_width();
  int h = f.get_height();
  int format = f.get_profile().format(); // rs2_format enum

  cv::Mat mat;
  if (format == RS2_FORMAT_RGB8) {
    // RGB -> convert to BGRA
    cv::Mat rgb(h, w, CV_8UC3, (void *)f.get_data());
    cv::cvtColor(rgb, mat, cv::COLOR_RGB2BGRA);
  } else if (format == RS2_FORMAT_BGR8) {
    cv::Mat bgr(h, w, CV_8UC3, (void *)f.get_data());
    cv::cvtColor(bgr, mat, cv::COLOR_BGR2BGRA);
  } else if (format == RS2_FORMAT_RGBA8) {
    mat = cv::Mat(h, w, CV_8UC4, (void *)f.get_data()).clone();
  } else if (format == RS2_FORMAT_BGRA8) {
    mat = cv::Mat(h, w, CV_8UC4, (void *)f.get_data()).clone();
  } else {
    // Fallback: try to treat as 3-channel
    try {
      cv::Mat unknown(h, w, CV_8UC3, (void *)f.get_data());
      cv::cvtColor(unknown, mat, cv::COLOR_BGR2BGRA);
    } catch (...) {
      mat = cv::Mat();
    }
  }
  return mat;
}

void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  try {
    if (!initialize_sender()) {
      std::cerr << "Failed to initialize sender, streaming thread stopping"
                << std::endl;
      return;
    }

    encoding_enabled.store(true);
    send_enabled.store(true);

    // Read camera config
    CameraRequestData config;
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      config = current_camera_config;
    }

    updateRealSenseConfiguration(config);

    // Setup RealSense
    rs2::pipeline pipe;
    rs2::config rs_cfg;

    int width = (config.width > 0) ? config.width : 640;
    int height = (config.height > 0) ? config.height : 480;
    int fps = (config.fps > 0) ? config.fps : 30;

    // Request color stream
    // rs_cfg.enable_stream(RS2_STREAM_COLOR, width, height, RS2_FORMAT_BGR8, fps);
    rs_cfg.enable_stream(RS2_STREAM_COLOR, width, height, RS2_FORMAT_BGRA8, fps);

    // Start pipeline
    rs2::pipeline_profile profile;
    try {
      profile = pipe.start(rs_cfg);
    } catch (const rs2::error &e) {
      std::cerr << "RealSense failed to start: " << e.what() << std::endl;
      return;
    }

    // Build GStreamer pipeline string
    std::string pipeline_str;
    if (config.width > 0 && config.height > 0) {
      pipeline_str = buildPipelineString(config, preview_enabled.load());
      std::cout << "Pipeline from command: " << pipeline_str << std::endl;
    } else {
      if (preview_enabled.load()) {
        pipeline_str =
            "appsrc name=mysource is-live=true format=time "
            "caps=video/x-raw,format=BGRA,width=640,height=480,framerate=30/1 "
            "! "
            "videoconvert ! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
            "tee name=t "
            "t. ! queue ! nvv4l2h264enc maxperf-enable=1 insert-sps-pps=true "
            "idrinterval=1 bitrate=4000000 ! h264parse ! appsink name=mysink "
            "emit-signals=true sync=false "
            "t. ! queue ! nvvidconv ! videoconvert ! autovideosink sync=false ";
      } else {
        pipeline_str =
            "appsrc name=mysource is-live=true format=time "
            "caps=video/x-raw,format=BGRA,width=640,height=480,framerate=30/1 "
            "! "
            "videoconvert ! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
            "tee name=t "
            "t. ! queue ! nvv4l2h264enc maxperf-enable=1 insert-sps-pps=true "
            "idrinterval=1 bitrate=4000000 ! h264parse ! appsink name=mysink "
            "emit-signals=true sync=false ";
      }
    }

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (!pipeline) {
      std::cerr << "Failed to create pipeline in streaming thread: "
                << (error ? error->message : "unknown") << std::endl;
      g_clear_error(&error);
      pipe.stop();
      return;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");

    if (!appsrc || !appsink) {
      std::cerr << "Failed to get appsrc or appsink elements" << std::endl;
      gst_object_unref(pipeline);
      pipe.stop();
      return;
    }

    // Configure appsrc to prevent buffer buildup and ensure smooth streaming
    g_object_set(appsrc,
                 "stream-type", 0,  // GST_APP_STREAM_TYPE_STREAM
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 "do-timestamp", FALSE,  // We manually set PTS, don't auto-timestamp
                 "min-percent", 50,  // Start pushing when queue is 50% full
                 nullptr);

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);
    
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      std::cerr << "Failed to set pipeline to PLAYING state" << std::endl;
      gst_object_unref(appsrc);
      gst_object_unref(appsink);
      gst_object_unref(pipeline);
      pipe.stop();
      return;
    }

    int frame_id = 0;
    int dropped_frames = 0;
    std::cout << "Starting RealSense streaming loop..." << std::endl;

    while (streaming_active.load() && !stop_requested.load()) {
      // Wait for next set of frames with timeout to allow thread to exit
      rs2::frameset frames;
      if (pipe.poll_for_frames(&frames)) {
        rs2::video_frame color_frame = frames.get_color_frame();
        if (color_frame) {
          cv::Mat cv_image = rsFrameToCvMatBGRA(color_frame);
          if (!cv_image.empty() && encoding_enabled.load()) {
            // Check if pipeline can accept more data (prevent buildup)
            // Use larger threshold to reduce unnecessary frame drops that cause stuttering
            guint64 current_bytes = gst_app_src_get_current_level_bytes(GST_APP_SRC(appsrc));
            const guint64 max_bytes = static_cast<guint64>(cv_image.total() * cv_image.elemSize() * 5); // ~5 frames
            
            if (current_bytes > max_bytes) {
              // Pipeline is backing up, skip this frame to prevent further buildup
              dropped_frames++;
              if (dropped_frames % 100 == 0) { // Less frequent logging
                std::cerr << "[Stream] Pipeline backing up, dropped " << dropped_frames 
                          << " frames. Queue: " << (current_bytes / 1024) << " KB" << std::endl;
              }
              continue;
            }
            
            GstBuffer *buffer = gst_buffer_new_allocate(
                nullptr, cv_image.total() * cv_image.elemSize(), nullptr);
            if (!buffer) {
              dropped_frames++;
              continue;
            }
            
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
              memcpy(map.data, cv_image.data,
                     cv_image.total() * cv_image.elemSize());
              gst_buffer_unmap(buffer, &map);

              // Set PTS / duration using continuous frame_id for smooth playback
              // Using frame_id instead of RealSense timestamp ensures continuous time progression
              GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame_id, GST_SECOND, (fps > 0 ? fps : 30));
              GST_BUFFER_DURATION(buffer) =
                  gst_util_uint64_scale(1, GST_SECOND, (fps > 0 ? fps : 30));

              GstFlowReturn push_ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
              if (push_ret == GST_FLOW_OK) {
                frame_id++; // Increment before using in PTS calculation next time
                if (frame_id % 300 == 0) {
                  double fps_actual = 300.0 / 10.0; // 300 frames in ~10 seconds
                  std::cout << "[Stream] Pushed " << frame_id << " frames" 
                            << (dropped_frames > 0 ? (", dropped: " + std::to_string(dropped_frames)) : "")
                            << ", FPS: ~" << std::fixed << std::setprecision(1) << fps_actual << std::endl;
                }
              } else {
                gst_buffer_unref(buffer);
                dropped_frames++;
                if (push_ret == GST_FLOW_FLUSHING) {
                  std::cerr << "[Stream] Pipeline is flushing, stopping" << std::endl;
                  break;
                } else if (push_ret != GST_FLOW_OK) {
                  // Log non-OK returns occasionally
                  if (dropped_frames % 100 == 0) {
                    std::cerr << "[Stream] Push buffer returned: " << push_ret << std::endl;
                  }
                }
              }
            } else {
              gst_buffer_unref(buffer);
              dropped_frames++;
            }
          }
        }
      } else {
        // no frame, give CPU breathing room
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    std::cout << "Streaming loop ended, cleaning up..." << std::endl;

    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(appsrc);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);

    // Stop RealSense pipeline
    try {
      pipe.stop();
    } catch (...) {
    }

  } catch (const std::exception &e) {
    std::cerr << "Streaming thread error: " << e.what() << std::endl;
  }

  std::cout << "Streaming thread finished" << std::endl;
}
