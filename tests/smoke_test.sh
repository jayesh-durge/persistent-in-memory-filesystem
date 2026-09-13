#!/usr/bin/env bash
# PISF Client-Server Automated Smoke Test Suite
# Tests Unix domain socket communication, message framing, command loop, error handling, and shutdown.

set -euo pipefail

# Locate build directory
BUILD_DIR="${1:-./build}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    echo "Make sure to build the project first: cmake --build ${BUILD_DIR}"
    exit 1
fi

# Set up isolated temporary test environment
TEST_TMP_DIR=$(mktemp -d /tmp/pisf-test.XXXXXX)
TEST_SOCK="${TEST_TMP_DIR}/test.sock"
SERVER_PID=""

# Ensure cleanup on any exit
cleanup() {
    local exit_code=$?
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TEST_TMP_DIR}"
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

wait_for_server() {
    local log_file="$1"
    for _ in {1..30}; do
        if grep -q "Server listening" "${log_file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

echo "=========================================================="
echo " Running PISF Client-Server Smoke Tests"
echo " Build directory: ${BUILD_DIR}"
echo " Test socket:     ${TEST_SOCK}"
echo "=========================================================="

# Test 1: Client connection failure handling when server is unavailable
echo -n "[Test 1/6] Client connection failure when server is unavailable... "
if "${CLIENT_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (client should have failed when server is down)"
    exit 1
fi
echo "PASSED"

# Test 2: Server startup, Unix domain socket creation, and PING/PONG
echo -n "[Test 2/6] Server startup and single PING/PONG... "
"${SERVER_BIN}" -s "${TEST_SOCK}" >"${TEST_TMP_DIR}/server.log" 2>&1 &
SERVER_PID=$!

if ! wait_for_server "${TEST_TMP_DIR}/server.log"; then
    echo "FAILED (server did not start listening within timeout)"
    cat "${TEST_TMP_DIR}/server.log"
    exit 1
fi

PING_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" PING)
if ! echo "${PING_RESP}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (unexpected response: ${PING_RESP})"
    exit 1
fi
echo "PASSED"

# Test 3: Multiple commands in one session, unknown command handling, and QUIT
echo -n "[Test 3/6] Multi-command session, unknown command, and QUIT... "
SESSION_OUTPUT=$(printf "PING\nNON_EXISTENT_CMD\nPING\nQUIT\n" | "${CLIENT_BIN}" -s "${TEST_SOCK}")

# Verify 1st PING returned PONG
if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (missing initial PONG)"
    exit 1
fi

# Verify unknown command returned proper error response
if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] ERR unknown command 'NON_EXISTENT_CMD'"; then
    echo "FAILED (missing error for unknown command. Got: ${SESSION_OUTPUT})"
    exit 1
fi

# Verify QUIT returned BYE
if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] BYE"; then
    echo "FAILED (missing BYE response to QUIT. Got: ${SESSION_OUTPUT})"
    exit 1
fi
echo "PASSED"

# Test 4: Prevent duplicate server on active socket
echo -n "[Test 4/6] Duplicate server refused on active socket... "
if "${SERVER_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (second server should have been refused)"
    exit 1
fi
echo "PASSED"

# Test 5: Graceful server shutdown via SHUTDOWN command and socket cleanup
echo -n "[Test 5/6] Graceful server shutdown via SHUTDOWN command... "
SHUTDOWN_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" SHUTDOWN)
if ! echo "${SHUTDOWN_RESP}" | grep -q "\[Response\] OK SHUTDOWN"; then
    echo "FAILED (unexpected response to SHUTDOWN: ${SHUTDOWN_RESP})"
    exit 1
fi

wait "${SERVER_PID}" || true
SERVER_PID=""

if [[ -e "${TEST_SOCK}" ]]; then
    echo "FAILED (socket file was not unlinked on shutdown)"
    exit 1
fi
echo "PASSED"

# Test 6: Stale socket detection and recovery, followed by SIGTERM shutdown
echo -n "[Test 6/6] Stale socket recovery and SIGTERM shutdown... "
# Create an orphaned socket file
python3 -c "import socket; s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind('${TEST_SOCK}'); s.close()"
if [[ ! -S "${TEST_SOCK}" ]]; then
    echo "FAILED (could not create dummy socket for stale test)"
    exit 1
fi

# Server should detect stale socket, remove it, bind, and listen successfully
"${SERVER_BIN}" -s "${TEST_SOCK}" >"${TEST_TMP_DIR}/server_stale.log" 2>&1 &
SERVER_PID=$!

if ! wait_for_server "${TEST_TMP_DIR}/server_stale.log"; then
    echo "FAILED (server did not start listening after stale recovery)"
    cat "${TEST_TMP_DIR}/server_stale.log"
    exit 1
fi

STALE_PING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" PING)
if ! echo "${STALE_PING}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (server failed to respond after stale socket recovery: ${STALE_PING})"
    cat "${TEST_TMP_DIR}/server_stale.log"
    exit 1
fi

# Terminate server via SIGTERM signal
kill -TERM "${SERVER_PID}"
wait "${SERVER_PID}" || true
SERVER_PID=""

if [[ -e "${TEST_SOCK}" ]]; then
    echo "FAILED (socket file remained after SIGTERM shutdown)"
    exit 1
fi
echo "PASSED"

echo "=========================================================="
echo " All smoke tests passed successfully!"
echo "=========================================================="
exit 0
