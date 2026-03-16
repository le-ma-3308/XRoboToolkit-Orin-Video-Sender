###############################################################################
#
# Standalone Makefile for OrinVideoSender (RealSense)
# Self-contained build configuration
# Modified for librealsense2
#
###############################################################################

# Compiler settings
CXX = g++
APP := OrinVideoSender

###############################################################################
# Source selection: change to your RealSense main file here
SRCS := \
	main_realsense_multi_client.cpp
###############################################################################

OBJS := $(SRCS:.cpp=.o)

# Include paths
CPPFLAGS := -std=c++11 \
	-I./asio-1.30.2/include \
	-I/usr/include/opencv4 \
	-I/usr/local/cuda/include \
	$(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 glib-2.0 2>/dev/null || echo "") \
	$(shell pkg-config --cflags libzmq 2>/dev/null || echo "") \
	$(shell pkg-config --cflags librealsense2 2>/dev/null || echo "")

# Compiler flags
CXXFLAGS := -Wall -Wextra -O2 -g

# FFmpeg flags (optional)
CXXFLAGS += $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null || echo "")

# Library paths and libraries
LDFLAGS := -L/usr/local/cuda/lib64

# FFmpeg libraries (must come first to avoid conflicts)
LDFLAGS += $(shell pkg-config --libs libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null || echo "-lavcodec -lavformat -lavutil -lswscale -lavdevice")

# Core libraries
LDFLAGS += \
	-lrealsense2 \
	-lcuda -lcudart \
	-lopencv_core -lopencv_imgproc -lopencv_videoio -lopencv_imgcodecs \
	-lssl -lcrypto \
	-lpthread \
	-lstdc++

# GStreamer libraries
LDFLAGS += $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 glib-2.0 2>/dev/null || echo "-lgstreamer-1.0 -lgstapp-1.0 -lglib-2.0")

# ZMQ library (optional)
LDFLAGS += $(shell pkg-config --libs libzmq 2>/dev/null || echo "-lzmq")

all: $(APP)

debug: CXXFLAGS += -DDEBUG -g3 -O0
debug: $(APP)

%.o: %.cpp
	@echo "Compiling: $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(APP): $(OBJS)
	@echo "Linking: $@"
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -rf $(APP) $(OBJS)

install: $(APP)
	@echo "Installing $(APP)..."
	install -D $(APP) /usr/local/bin/$(APP)

.PHONY: all debug clean install
