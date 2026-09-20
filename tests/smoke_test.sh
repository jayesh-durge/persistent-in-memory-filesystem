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
echo -n "[Test 1/15] Client connection failure when server is unavailable... "
if "${CLIENT_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (client should have failed when server is down)"
    exit 1
fi
echo "PASSED"

# Test 2: Server startup, Unix domain socket creation, and single PING/PONG
echo -n "[Test 2/15] Server startup and single PING/PONG... "
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
echo -n "[Test 3/15] Multi-command session, unknown command, and QUIT... "
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
echo -n "[Test 4/15] Duplicate server refused on active socket... "
if "${SERVER_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (second server should have been refused)"
    exit 1
fi
echo "PASSED"

# Test 5: Client A remains idle while Client B connects and executes PING
echo -n "[Test 5/15] Idle Client A does not block concurrent Client B... "
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
echo -n "[Test 6/15] 5 concurrent clients issuing multiple commands... "
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
echo -n "[Test 7/15] QUIT closes only requesting client... "
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
echo -n "[Test 8/15] Abrupt client disconnect handled cleanly... "
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

# Test 9: Root initialization, directory/file creation, sorted listing, and STAT metadata
echo -n "[Test 9/15] Filesystem tree creation, listing, sorting, and STAT... "
ROOT_STAT=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /)
if ! echo "${ROOT_STAT}" | grep -q "\[Response\] name=/ type=directory id=1"; then
    echo "FAILED (unexpected root stat: ${ROOT_STAT})"
    exit 1
fi

ROOT_LS_EMPTY=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS /)
if ! echo "${ROOT_LS_EMPTY}" | grep -q "\[Response\] *$"; then
    echo "FAILED (expected empty listing for initial root, got: ${ROOT_LS_EMPTY})"
    exit 1
fi

MKDIR_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /projects)
if ! echo "${MKDIR_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (MKDIR /projects failed: ${MKDIR_RESP})"
    exit 1
fi

CREATE_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /projects/demo.txt)
if ! echo "${CREATE_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (CREATE /projects/demo.txt failed: ${CREATE_RESP})"
    exit 1
fi

CREATE_NOTES=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /notes.txt)
if ! echo "${CREATE_NOTES}" | grep -q "\[Response\] OK"; then
    echo "FAILED (CREATE /notes.txt failed: ${CREATE_NOTES})"
    exit 1
fi

# Listing root should produce sorted names: notes.txt projects
LS_ROOT=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS /)
if ! echo "${LS_ROOT}" | grep -q "\[Response\] notes.txt projects"; then
    echo "FAILED (LS / did not return sorted children. Got: ${LS_ROOT})"
    exit 1
fi

# Listing /projects should return demo.txt
LS_PROJ=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS /projects)
if ! echo "${LS_PROJ}" | grep -q "\[Response\] demo.txt"; then
    echo "FAILED (LS /projects failed. Got: ${LS_PROJ})"
    exit 1
fi

# Inspect STAT metadata for directory and file
STAT_PROJ=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /projects)
if ! echo "${STAT_PROJ}" | grep -q "\[Response\] name=projects type=directory id="; then
    echo "FAILED (STAT /projects unexpected: ${STAT_PROJ})"
    exit 1
fi

STAT_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /projects/demo.txt)
if ! echo "${STAT_FILE}" | grep -q "\[Response\] name=demo.txt type=file id="; then
    echo "FAILED (STAT /projects/demo.txt unexpected: ${STAT_FILE})"
    exit 1
fi

# Identical names in different directories
"${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /docs >/dev/null
CREATE_DOCS_DEMO=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /docs/demo.txt)
if ! echo "${CREATE_DOCS_DEMO}" | grep -q "\[Response\] OK"; then
    echo "FAILED (CREATE /docs/demo.txt failed: ${CREATE_DOCS_DEMO})"
    exit 1
fi

STAT_DOCS_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /docs/demo.txt)
if [[ "${STAT_FILE}" == "${STAT_DOCS_FILE}" ]]; then
    echo "FAILED (identical names in different dirs must have distinct node IDs)"
    exit 1
fi
echo "PASSED"

# Test 10: Filesystem error handling (missing paths, non-directories, duplicate names)
echo -n "[Test 10/15] Filesystem error handling (missing, non-directory, duplicates)... "
# Missing paths
LS_MISSING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS /missing)
if ! echo "${LS_MISSING}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (LS /missing should return ERR not found, got: ${LS_MISSING})"
    exit 1
fi

STAT_MISSING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /missing)
if ! echo "${STAT_MISSING}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (STAT /missing should return ERR not found, got: ${STAT_MISSING})"
    exit 1
fi

# Creating under missing parent
MKDIR_MISSING_PARENT=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /missing/child)
if ! echo "${MKDIR_MISSING_PARENT}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (MKDIR under missing parent should fail: ${MKDIR_MISSING_PARENT})"
    exit 1
fi

# Creating child under a file
MKDIR_UNDER_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /notes.txt/sub)
if ! echo "${MKDIR_UNDER_FILE}" | grep -q "\[Response\] ERR not a directory"; then
    echo "FAILED (MKDIR under file should fail: ${MKDIR_UNDER_FILE})"
    exit 1
fi

CREATE_UNDER_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /notes.txt/sub.txt)
if ! echo "${CREATE_UNDER_FILE}" | grep -q "\[Response\] ERR not a directory"; then
    echo "FAILED (CREATE under file should fail: ${CREATE_UNDER_FILE})"
    exit 1
fi

# Listing a file
LS_ON_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS /notes.txt)
if ! echo "${LS_ON_FILE}" | grep -q "\[Response\] ERR not a directory"; then
    echo "FAILED (LS on file should fail: ${LS_ON_FILE})"
    exit 1
fi

# Duplicate name within same directory
DUP_FILE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /projects/demo.txt)
if ! echo "${DUP_FILE}" | grep -q "\[Response\] ERR already exists"; then
    echo "FAILED (duplicate CREATE did not return ERR already exists: ${DUP_FILE})"
    exit 1
fi

DUP_DIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /projects)
if ! echo "${DUP_DIR}" | grep -q "\[Response\] ERR already exists"; then
    echo "FAILED (duplicate MKDIR did not return ERR already exists: ${DUP_DIR})"
    exit 1
fi
echo "PASSED"

# Test 11: Invalid path syntax, normalization, and malformed commands
echo -n "[Test 11/15] Invalid path syntax, normalization, and malformed commands... "
# Relative path rejection
REL_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR relative/path)
if ! echo "${REL_MKDIR}" | grep -q "\[Response\] ERR invalid path"; then
    echo "FAILED (relative path should be rejected: ${REL_MKDIR})"
    exit 1
fi

# '.' and '..' rejection
DOT_CREATE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /projects/./foo)
if ! echo "${DOT_CREATE}" | grep -q "\[Response\] ERR invalid path component"; then
    echo "FAILED ('.' should be rejected: ${DOT_CREATE})"
    exit 1
fi

DOTDOT_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /projects/../foo)
if ! echo "${DOTDOT_MKDIR}" | grep -q "\[Response\] ERR invalid path component"; then
    echo "FAILED ('..' should be rejected: ${DOTDOT_MKDIR})"
    exit 1
fi

# Missing path arguments
MISSING_ARG_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR)
if ! echo "${MISSING_ARG_MKDIR}" | grep -q "\[Response\] ERR missing path argument"; then
    echo "FAILED (MKDIR without path should fail: ${MISSING_ARG_MKDIR})"
    exit 1
fi

# Root creation rejection
ROOT_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /)
if ! echo "${ROOT_MKDIR}" | grep -q "\[Response\] ERR already exists"; then
    echo "FAILED (MKDIR / should fail: ${ROOT_MKDIR})"
    exit 1
fi

ROOT_CREATE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /)
if ! echo "${ROOT_CREATE}" | grep -q "\[Response\] ERR cannot create file at root"; then
    echo "FAILED (CREATE / should fail: ${ROOT_CREATE})"
    exit 1
fi

# Repeated slash normalization
NORM_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR //norm_dir//)
if ! echo "${NORM_MKDIR}" | grep -q "\[Response\] OK"; then
    echo "FAILED (MKDIR //norm_dir// should succeed: ${NORM_MKDIR})"
    exit 1
fi
NORM_CREATE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE //norm_dir///norm_file.txt/)
if ! echo "${NORM_CREATE}" | grep -q "\[Response\] OK"; then
    echo "FAILED (CREATE with repeated slashes should succeed: ${NORM_CREATE})"
    exit 1
fi
STAT_NORM=$("${CLIENT_BIN}" -s "${TEST_SOCK}" STAT /norm_dir/norm_file.txt)
if ! echo "${STAT_NORM}" | grep -q "\[Response\] name=norm_file.txt type=file"; then
    echo "FAILED (STAT on normalized path failed: ${STAT_NORM})"
    exit 1
fi
echo "PASSED"

# Test 12: Concurrent clients creating identical path (contested race condition)
echo -n "[Test 12/15] Concurrent clients creating identical path (contested race)... "
RACE_PIDS=()
for i in {1..10}; do
    "${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /race_file.txt >"${TEST_TMP_DIR}/race_${i}.log" 2>&1 &
    RACE_PIDS+=($!)
done

for pid in "${RACE_PIDS[@]}"; do
    wait "${pid}"
done

OK_COUNT=0
EXISTS_COUNT=0
for i in {1..10}; do
    if grep -q "\[Response\] OK" "${TEST_TMP_DIR}/race_${i}.log"; then
        OK_COUNT=$((OK_COUNT + 1))
    elif grep -q "\[Response\] ERR already exists" "${TEST_TMP_DIR}/race_${i}.log"; then
        EXISTS_COUNT=$((EXISTS_COUNT + 1))
    fi
done

if [[ "${OK_COUNT}" -ne 1 ]] || [[ "${EXISTS_COUNT}" -ne 9 ]]; then
    echo "FAILED (expected exactly 1 OK and 9 ERR already exists; got OK=${OK_COUNT}, EXISTS=${EXISTS_COUNT})"
    exit 1
fi
echo "PASSED"

# Test 13: Concurrent independent filesystem operations without corruption
echo -n "[Test 13/15] Concurrent independent filesystem operations... "
CONC_FS_PIDS=()
for c in {1..6}; do
    (
        "${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR "/worker_${c}" >/dev/null
        for f in {1..10}; do
            "${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE "/worker_${c}/file_${f}.txt" >/dev/null
        done
        LISTING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" LS "/worker_${c}")
        COUNT=$(echo "${LISTING}" | tr ' ' '\n' | grep -c "file_" || true)
        if [[ "${COUNT}" -ne 10 ]]; then
            echo "Worker $c got incomplete file count: ${COUNT}" >&2
            exit 1
        fi
    ) &
    CONC_FS_PIDS+=($!)
done

for pid in "${CONC_FS_PIDS[@]}"; do
    wait "${pid}"
done
echo "PASSED"

# Test 14: Graceful server shutdown via SHUTDOWN command with an idle client connected
echo -n "[Test 14/15] SHUTDOWN unblocks idle client and cleans up... "
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

# Test 15: Stale socket recovery and SIGTERM shutdown with an idle client connected
echo -n "[Test 15/15] Stale socket recovery and SIGTERM shutdown with idle client... "
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
