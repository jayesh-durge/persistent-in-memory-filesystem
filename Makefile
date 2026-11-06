CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -g -pthread
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build

APP_DIR := $(BUILD_DIR)/app
TEST_DIR := $(BUILD_DIR)/test
TEST_BIN_DIR := $(TEST_DIR)/bin

SERVER_SOURCES := src/server.cpp src/filesystem.cpp src/storage.cpp src/metadata.cpp
CLIENT_SOURCES := src/client.cpp
APP_SERVER_OBJECTS := $(patsubst %.cpp,$(APP_DIR)/obj/%.o,$(SERVER_SOURCES))
APP_CLIENT_OBJECTS := $(patsubst %.cpp,$(APP_DIR)/obj/%.o,$(CLIENT_SOURCES))

TEST_COMMON_OBJECTS := $(patsubst %.cpp,$(TEST_DIR)/obj/%.o,src/filesystem.cpp src/storage.cpp src/metadata.cpp)
TEST_SERVER_OBJECT := $(TEST_DIR)/obj/src/server.o
TEST_CLIENT_OBJECT := $(TEST_DIR)/obj/src/client.o
TEST_STORAGE_OBJECT := $(TEST_DIR)/obj/tests/storage_test.o
TEST_METADATA_OBJECT := $(TEST_DIR)/obj/tests/metadata_test.o
TEST_FILESYSTEM_OBJECT := $(TEST_DIR)/obj/tests/filesystem_test.o

APP_DEPENDENCIES := $(APP_SERVER_OBJECTS:.o=.d) $(APP_CLIENT_OBJECTS:.o=.d)
TEST_DEPENDENCIES := $(TEST_COMMON_OBJECTS:.o=.d) $(TEST_SERVER_OBJECT:.o=.d) \
	$(TEST_CLIENT_OBJECT:.o=.d) $(TEST_STORAGE_OBJECT:.o=.d) \
	$(TEST_METADATA_OBJECT:.o=.d) $(TEST_FILESYSTEM_OBJECT:.o=.d)
TEST_CXXFLAGS := $(CXXFLAGS) -UNDEBUG

.PHONY: all app fsserver fsclient test-build tests test check clean clean-app clean-tests

all: app

app: $(APP_DIR)/fsserver $(APP_DIR)/fsclient

fsserver: $(APP_DIR)/fsserver

fsclient: $(APP_DIR)/fsclient

$(APP_DIR)/fsserver: $(APP_SERVER_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(APP_DIR)/fsclient: $(APP_CLIENT_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(APP_DIR)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

test-build: $(TEST_BIN_DIR)/pisf-server $(TEST_BIN_DIR)/pisf-client \
	$(TEST_BIN_DIR)/storage_test $(TEST_BIN_DIR)/metadata_test \
	$(TEST_BIN_DIR)/filesystem_test

tests: test-build

test: test-build
	$(TEST_BIN_DIR)/storage_test
	$(TEST_BIN_DIR)/metadata_test
	$(TEST_BIN_DIR)/filesystem_test
	bash tests/smoke_test.sh $(TEST_BIN_DIR)
	bash tests/lifecycle_test.sh $(TEST_BIN_DIR)
	bash tests/demo.sh $(TEST_BIN_DIR)
	bash tests/storage_demo.sh $(TEST_BIN_DIR)

check: test

$(TEST_BIN_DIR)/pisf-server: $(TEST_SERVER_OBJECT) $(TEST_COMMON_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $^ -o $@

$(TEST_BIN_DIR)/pisf-client: $(TEST_CLIENT_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $^ -o $@

$(TEST_BIN_DIR)/storage_test: $(TEST_STORAGE_OBJECT) $(TEST_DIR)/obj/src/storage.o
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $^ -o $@

$(TEST_BIN_DIR)/metadata_test: $(TEST_METADATA_OBJECT) $(TEST_COMMON_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $^ -o $@

$(TEST_BIN_DIR)/filesystem_test: $(TEST_FILESYSTEM_OBJECT) $(TEST_COMMON_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $^ -o $@

$(TEST_DIR)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(TEST_CXXFLAGS) -MMD -MP -c $< -o $@

-include $(APP_DEPENDENCIES) $(TEST_DEPENDENCIES)

clean-app:
	rm -rf $(APP_DIR)

clean-tests:
	rm -rf $(TEST_DIR)

clean:
	rm -rf $(BUILD_DIR)
