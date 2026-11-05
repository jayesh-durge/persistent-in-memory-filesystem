CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -g -pthread
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build

SERVER_SOURCES := src/server.cpp src/filesystem.cpp src/storage.cpp src/metadata.cpp
CLIENT_SOURCES := src/client.cpp
SERVER_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(SERVER_SOURCES))
CLIENT_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(CLIENT_SOURCES))
DEPENDENCIES := $(SERVER_OBJECTS:.o=.d) $(CLIENT_OBJECTS:.o=.d)

.PHONY: all clean

all: $(BUILD_DIR)/fsserver $(BUILD_DIR)/fsclient

$(BUILD_DIR)/fsserver: $(SERVER_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/fsclient: $(CLIENT_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPENDENCIES)

clean:
	rm -rf $(BUILD_DIR)
