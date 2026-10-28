cmake_minimum_required(VERSION 3.16)

project(pisf
    VERSION 0.1.0
    DESCRIPTION "Persistent In-memory/Storage Filesystem skeleton"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threading support
find_package(Threads REQUIRED)

# Common compiler warnings for robust systems programming
add_library(pisf_compiler_flags INTERFACE)
target_compile_options(pisf_compiler_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
)

# Common include directory
include_directories(${PROJECT_SOURCE_DIR}/include)

# Server executable target
add_executable(pisf-server
    src/server.cpp
    src/filesystem.cpp
    src/storage.cpp
    src/metadata.cpp
)
target_link_libraries(pisf-server PRIVATE pisf_compiler_flags Threads::Threads)

# Client executable target
add_executable(pisf-client
    src/client.cpp
)
target_link_libraries(pisf-client PRIVATE pisf_compiler_flags)

# Testing configuration
enable_testing()

add_executable(storage_test
    tests/storage_test.cpp
    src/storage.cpp
)
target_link_libraries(storage_test PRIVATE pisf_compiler_flags Threads::Threads)

add_test(
    NAME storage_test
    COMMAND storage_test
)

add_executable(metadata_test
    tests/metadata_test.cpp
    src/metadata.cpp
    src/filesystem.cpp
    src/storage.cpp
)
target_link_libraries(metadata_test PRIVATE pisf_compiler_flags Threads::Threads)

add_test(
    NAME metadata_test
    COMMAND metadata_test
)

add_executable(filesystem_test
    tests/filesystem_test.cpp
    src/filesystem.cpp
    src/storage.cpp
    src/metadata.cpp
)
target_link_libraries(filesystem_test PRIVATE pisf_compiler_flags Threads::Threads)

add_test(
    NAME filesystem_test
    COMMAND filesystem_test
)

add_test(
    NAME smoke_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(
    NAME recovery_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery_test.sh ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(
    NAME lifecycle_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_test.sh ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(
    NAME client_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_test.sh ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(
    NAME demo_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/demo.sh ${CMAKE_CURRENT_BINARY_DIR}
)

