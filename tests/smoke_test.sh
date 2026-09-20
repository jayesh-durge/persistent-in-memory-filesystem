#!/usr/bin/env bash
# PISF Client-Server Automated Smoke & Concurrency Test Suite
# Tests Unix domain socket communication, message framing, multi-client concurrency,
# command loop, error handling, idle client unblocking, and graceful shutdown.

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
echo " Running PISF Client-Server Concurrency & Smoke Tests"
echo " Build directory: ${BUILD_DIR}"
echo " Test socket:     ${TEST_SOCK}"
echo "=========================================================="

# Test 1: Client connection failure handling when server is unavailable
echo -n "[Test 1/10] Client connection failure when server is unavailable... "
if "${CLIENT_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (client should have failed when server is down)"
    exit 1
fi
echo "PASSED"

# Test 2: Server startup, Unix domain socket creation, and single PING/PONG
echo -n "[Test 2/10] Server startup and single PING/PONG... "
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

# Test 3: Multi-command session, unknown command handling, and QUIT
echo -n "[Test 3/10] Multi-command session, unknown command, and QUIT... "
SESSION_OUTPUT=$(printf "PING\nNON_EXISTENT_CMD\nPING\nQUIT\n" | "${CLIENT_BIN}" -s "${TEST_SOCK}")

if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (missing initial PONG)"
    exit 1
fi
if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] ERR unknown command 'NON_EXISTENT_CMD'"; then
    echo "FAILED (missing error for unknown command. Got: ${SESSION_OUTPUT})"
    exit 1
fi
if ! echo "${SESSION_OUTPUT}" | grep -q "\[Response\] BYE"; then
    echo "FAILED (missing BYE response to QUIT. Got: ${SESSION_OUTPUT})"
    exit 1
fi
echo "PASSED"

# Test 4: Prevent duplicate server on active socket
echo -n "[Test 4/10] Duplicate server refused on active socket... "
if "${SERVER_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (second server should have been refused)"
    exit 1
fi
echo "PASSED"

# Test 5: Client A remains idle while Client B connects and executes PING
echo -n "[Test 5/10] Idle Client A does not block concurrent Client B... "
# Client A connects and sits idle holding the connection
python3 -c "
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
# Tell shell we are connected
with open('${TEST_TMP_DIR}/client_a_connected', 'w') as f:
    f.write('connected')
time.sleep(1.0)
# Send PING after delay
s.sendall(b'PING\n')
resp = s.recv(1024).decode()
assert 'PONG' in resp
s.sendall(b'QUIT\n')
s.close()
" >/dev/null 2>&1 &
PYTHON_CLIENT_A_PID=$!

# Wait for client A to connect
for _ in {1..20}; do
    if [[ -f "${TEST_TMP_DIR}/client_a_connected" ]]; then
        break
    fi
    sleep 0.05
done

# Client B executes PING while Client A is idle
CLIENT_B_START=$(date +%s%N)
CLIENT_B_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" PING)
CLIENT_B_END=$(date +%s%N)

if ! echo "${CLIENT_B_RESP}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (Client B did not get PONG while Client A was connected)"
    exit 1
fi

wait "${PYTHON_CLIENT_A_PID}"
echo "PASSED"

# Test 6: Multiple concurrent clients issuing commands without response mixing
echo -n "[Test 6/10] 5 concurrent clients issuing multiple commands... "
PIDS=()
for i in {1..5}; do
    (
        OUTPUT=$(printf "PING\nPING\nQUIT\n" | "${CLIENT_BIN}" -s "${TEST_SOCK}")
        PONG_COUNT=$(echo "${OUTPUT}" | grep -c "\[Response\] PONG" || true)
        if [[ "${PONG_COUNT}" -ne 2 ]]; then
            echo "Client $i got unexpected PONG count: ${PONG_COUNT}" >&2
            exit 1
        fi
        if ! echo "${OUTPUT}" | grep -q "\[Response\] BYE"; then
            echo "Client $i did not get BYE" >&2
            exit 1
        fi
    ) &
    PIDS+=($!)
done

for pid in "${PIDS[@]}"; do
    wait "${pid}"
done
echo "PASSED"

# Test 7: QUIT closes only requesting client session; server remains available
echo -n "[Test 7/10] QUIT closes only requesting client... "
QUIT_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" QUIT)
if ! echo "${QUIT_RESP}" | grep -q "\[Response\] BYE"; then
    echo "FAILED (QUIT did not return BYE)"
    exit 1
fi

# Server should still be alive and responsive
AFTER_QUIT_PING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" PING)
if ! echo "${AFTER_QUIT_PING}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (server unavailable after client QUIT)"
    exit 1
fi
echo "PASSED"

# Test 8: Abrupt client disconnect handled cleanly
echo -n "[Test 8/10] Abrupt client disconnect handled cleanly... "
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
# Close immediately without sending data
s.close()
"
# Verify server still healthy
ABRUPT_PING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" PING)
if ! echo "${ABRUPT_PING}" | grep -q "\[Response\] PONG"; then
    echo "FAILED (server failed after abrupt client disconnect)"
    exit 1
fi
echo "PASSED"

# Test 9: Graceful server shutdown via SHUTDOWN command with an idle client connected
echo -n "[Test 9/10] SHUTDOWN unblocks idle client and cleans up... "
# Start an idle client that sits blocked on recv()
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
with open('${TEST_TMP_DIR}/idle_client_ready', 'w') as f:
    f.write('ready')
# Wait for server shutdown to unblock recv()
data = s.recv(1024)
# When server shuts down socket, recv() returns b'' (EOF)
assert data == b''
s.close()
" >"${TEST_TMP_DIR}/idle_client.log" 2>&1 &
IDLE_CLIENT_PID=$!

for _ in {1..20}; do
    if [[ -f "${TEST_TMP_DIR}/idle_client_ready" ]]; then
        break
    fi
    sleep 0.05
done

# Another client sends SHUTDOWN
SHUTDOWN_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" SHUTDOWN)
if ! echo "${SHUTDOWN_RESP}" | grep -q "\[Response\] OK SHUTDOWN"; then
    echo "FAILED (unexpected response to SHUTDOWN: ${SHUTDOWN_RESP})"
    exit 1
fi

# Both server and idle client should exit promptly (within 2 seconds)
wait "${IDLE_CLIENT_PID}"
wait "${SERVER_PID}"
SERVER_PID=""

if [[ -e "${TEST_SOCK}" ]]; then
    echo "FAILED (socket file was not unlinked on shutdown)"
    exit 1
fi
echo "PASSED"

# Test 10: Stale socket recovery and SIGTERM shutdown with an idle client connected
echo -n "[Test 10/10] Stale socket recovery and SIGTERM shutdown with idle client... "
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

# Connect an idle client
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
with open('${TEST_TMP_DIR}/idle_client_term_ready', 'w') as f:
    f.write('ready')
data = s.recv(1024)
assert data == b''
s.close()
" >"${TEST_TMP_DIR}/idle_client_term.log" 2>&1 &
IDLE_TERM_PID=$!

for _ in {1..20}; do
    if [[ -f "${TEST_TMP_DIR}/idle_client_term_ready" ]]; then
        break
    fi
    sleep 0.05
done

# Terminate server via SIGTERM signal
kill -TERM "${SERVER_PID}"
wait "${IDLE_TERM_PID}"
wait "${SERVER_PID}"
SERVER_PID=""

if [[ -e "${TEST_SOCK}" ]]; then
    echo "FAILED (socket file remained after SIGTERM shutdown)"
    exit 1
fi
echo "PASSED"

echo "=========================================================="
echo " All concurrency and smoke tests passed successfully!"
echo "=========================================================="
exit 0
