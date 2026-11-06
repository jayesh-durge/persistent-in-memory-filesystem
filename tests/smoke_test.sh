#!/usr/bin/env bash
# PISF Client-Server Automated Smoke & Concurrency Test Suite
# Tests Unix domain socket communication, message framing, multi-client concurrency,
# command loop, error handling, idle client unblocking, and graceful shutdown.

set -euo pipefail

# Locate build directory
BUILD_DIR="${1:-./build/test/bin}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    echo "Build the test binaries first with: make test-build"
    exit 1
fi

# Set up isolated temporary test environment
TEST_TMP_DIR=$(mktemp -d /tmp/pisf-test.XXXXXX)
TEST_SOCK="${TEST_TMP_DIR}/test.sock"
TEST_DATA_DIR="${TEST_TMP_DIR}/storage"
SERVER_PID=""

# Ensure cleanup on any exit
cleanup() {
    local exit_code=$?
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    if [[ -n "${RESTART_SRV_PID:-}" ]] && kill -0 "${RESTART_SRV_PID}" 2>/dev/null; then
        kill -TERM "${RESTART_SRV_PID}" 2>/dev/null || true
        wait "${RESTART_SRV_PID}" 2>/dev/null || true
    fi
    if [[ -n "${M7_SRV_PID:-}" ]] && kill -0 "${M7_SRV_PID}" 2>/dev/null; then
        kill -TERM "${M7_SRV_PID}" 2>/dev/null || true
        wait "${M7_SRV_PID}" 2>/dev/null || true
    fi
    if [[ "${exit_code}" -ne 0 ]]; then
        echo "Test failure log dump:"
        for f in "${TEST_TMP_DIR}"/*.log; do
            if [[ -f "${f}" ]]; then
                echo "=== ${f} ==="
                cat "${f}"
            fi
        done
    fi
    rm -rf "${TEST_TMP_DIR}"
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

wait_for_server() {
    local log_file="$1"
    for _ in {1..100}; do
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
echo " Storage dir:     ${TEST_DATA_DIR}"
echo "=========================================================="

# Test 1: Client connection failure handling when server is unavailable
echo -n "[Test 1/20] Client connection failure when server is unavailable... "
if "${CLIENT_BIN}" -s "${TEST_SOCK}" >/dev/null 2>&1; then
    echo "FAILED (client should have failed when server is down)"
    exit 1
fi
echo "PASSED"

# Test 2: Server startup, Unix domain socket creation, and single PING/PONG
echo -n "[Test 2/20] Server startup and single PING/PONG... "
"${SERVER_BIN}" -s "${TEST_SOCK}" -d "${TEST_DATA_DIR}" >"${TEST_TMP_DIR}/server.log" 2>&1 &
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
echo -n "[Test 3/20] Multi-command session, unknown command, and QUIT... "
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
echo -n "[Test 4/20] Duplicate server refused on active socket... "
if "${SERVER_BIN}" -s "${TEST_SOCK}" -d "${TEST_DATA_DIR}" >/dev/null 2>&1; then
    echo "FAILED (second server should have been refused)"
    exit 1
fi
echo "PASSED"

# Test 5: Client A remains idle while Client B connects and executes PING
echo -n "[Test 5/20] Idle Client A does not block concurrent Client B... "
# Client A connects and sits idle holding the connection
python3 -c "
import socket, struct, time

def recv_exact(sock, length):
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError('unexpected end of framed message')
        data += chunk
    return data

def send_frame(sock, payload):
    payload = payload if isinstance(payload, bytes) else payload.encode()
    sock.sendall(b'\x01' + struct.pack('!I', len(payload)) + payload)

def recv_frame(sock):
    header = recv_exact(sock, 5)
    assert header[0] == 1
    return recv_exact(sock, struct.unpack('!I', header[1:])[0]).decode()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
# Tell shell we are connected
with open('${TEST_TMP_DIR}/client_a_connected', 'w') as f:
    f.write('connected')
time.sleep(1.0)
# Send PING after delay
send_frame(s, 'PING')
resp = recv_frame(s)
assert 'PONG' in resp
send_frame(s, 'QUIT')
assert recv_frame(s) == 'BYE'
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
echo -n "[Test 6/20] 5 concurrent clients issuing multiple commands... "
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
echo -n "[Test 7/20] QUIT closes only requesting client... "
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
echo -n "[Test 8/20] Abrupt client disconnect handled cleanly... "
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
echo -n "[Test 9/20] Filesystem tree creation, listing, sorting, and STAT... "
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
echo -n "[Test 10/20] Filesystem error handling (missing, non-directory, duplicates)... "
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
echo -n "[Test 11/20] Invalid path syntax, normalization, and malformed commands... "
# Relative paths are accepted and still report a missing intermediate directory.
REL_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR relative/path)
if ! echo "${REL_MKDIR}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (missing intermediate directory should be reported: ${REL_MKDIR})"
    exit 1
fi

# Dot navigation is normalized by the server before resolving the path.
DOT_CREATE=$("${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /projects/./foo)
if ! echo "${DOT_CREATE}" | grep -q "\[Response\] OK"; then
    echo "FAILED ('.' path component should normalize: ${DOT_CREATE})"
    exit 1
fi
"${CLIENT_BIN}" -s "${TEST_SOCK}" RM /projects/foo >/dev/null

DOTDOT_MKDIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" MKDIR /projects/../foo)
if ! echo "${DOTDOT_MKDIR}" | grep -q "\[Response\] OK"; then
    echo "FAILED ('..' path component should normalize: ${DOTDOT_MKDIR})"
    exit 1
fi
"${CLIENT_BIN}" -s "${TEST_SOCK}" RMDIR /foo >/dev/null

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
echo -n "[Test 12/20] Concurrent clients creating identical path (contested race)... "
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
echo -n "[Test 13/20] Concurrent independent filesystem operations... "
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

# Test 14: File WRITE and READ operations over socket
echo -n "[Test 14/20] File content operations (empty read, write, read, replace, spaces)... "
"${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /test_content.txt >/dev/null

# 1. Read newly created empty file
READ_EMPTY=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /test_content.txt)
if [[ "${READ_EMPTY}" != "[Response] "* ]] && [[ "${READ_EMPTY}" != "[Response]"* ]]; then
    echo "FAILED (READ empty file should produce empty response, got: ${READ_EMPTY})"
    exit 1
fi

# 2. Write content
WRITE_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /test_content.txt Hello from PISF)
if ! echo "${WRITE_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (WRITE failed: ${WRITE_RESP})"
    exit 1
fi

# 3. Read content back
READ_RESP=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /test_content.txt)
if ! echo "${READ_RESP}" | grep -q "\[Response\] Hello from PISF"; then
    echo "FAILED (READ did not return expected content: ${READ_RESP})"
    exit 1
fi

# 4. Replace content
"${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /test_content.txt Replaced version with different text >/dev/null
READ_REPLACED=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /test_content.txt)
if ! echo "${READ_REPLACED}" | grep -q "\[Response\] Replaced version with different text"; then
    echo "FAILED (READ after replace failed: ${READ_REPLACED})"
    exit 1
fi

# 5. Content containing multiple spaces
"${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /test_content.txt "  spaces   inside   content  " >/dev/null
READ_SPACES=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /test_content.txt)
if ! echo "${READ_SPACES}" | grep -q "\[Response\]   spaces   inside   content  "; then
    echo "FAILED (READ spaces failed: ${READ_SPACES})"
    exit 1
fi
echo "PASSED"

# Test 15: File content error handling (missing, directory, missing args, newlines)
echo -n "[Test 15/20] File content error handling (missing, directory, malformed, newlines)... "
# Missing file
READ_MISSING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /missing_file.txt)
if ! echo "${READ_MISSING}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (READ missing should return ERR not found: ${READ_MISSING})"
    exit 1
fi

WRITE_MISSING=$("${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /missing_file.txt some_data)
if ! echo "${WRITE_MISSING}" | grep -q "\[Response\] ERR not found"; then
    echo "FAILED (WRITE missing should return ERR not found: ${WRITE_MISSING})"
    exit 1
fi

# Directory target
READ_DIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ /projects)
if ! echo "${READ_DIR}" | grep -q "\[Response\] ERR not a file"; then
    echo "FAILED (READ directory should return ERR not a file: ${READ_DIR})"
    exit 1
fi

WRITE_DIR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /projects some_data)
if ! echo "${WRITE_DIR}" | grep -q "\[Response\] ERR not a file"; then
    echo "FAILED (WRITE directory should return ERR not a file: ${WRITE_DIR})"
    exit 1
fi

# Missing path argument
READ_NO_ARG=$("${CLIENT_BIN}" -s "${TEST_SOCK}" READ)
if ! echo "${READ_NO_ARG}" | grep -q "\[Response\] ERR missing path argument"; then
    echo "FAILED (READ without arg should return ERR missing path argument: ${READ_NO_ARG})"
    exit 1
fi

WRITE_NO_ARG=$("${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE)
if ! echo "${WRITE_NO_ARG}" | grep -q "\[Response\] ERR missing path argument"; then
    echo "FAILED (WRITE without arg should return ERR missing path argument: ${WRITE_NO_ARG})"
    exit 1
fi

# Framing carries newlines safely; the filesystem rejects unsupported file content.
NEWLINE_CLI_ERR=$("${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /test_content.txt $'line1\nline2' 2>&1 || true)
if ! echo "${NEWLINE_CLI_ERR}" | grep -q "ERR invalid content"; then
    echo "FAILED (filesystem should reject newline content cleanly: ${NEWLINE_CLI_ERR})"
    exit 1
fi
echo "PASSED"

# Test 16: Concurrent clients reading and writing the same file over sockets
echo -n "[Test 16/20] Concurrent socket clients reading and writing same file... "
"${CLIENT_BIN}" -s "${TEST_SOCK}" CREATE /concurrent_file.txt >/dev/null
"${CLIENT_BIN}" -s "${TEST_SOCK}" WRITE /concurrent_file.txt "SEED_PAYLOAD" >/dev/null

CONC_PIDS=()
# 3 writer clients
for w in {1..3}; do
    python3 -c "
import socket, struct

def recv_exact(sock, length):
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk: raise EOFError('unexpected end of frame')
        data += chunk
    return data

def send_frame(sock, payload):
    payload = payload if isinstance(payload, bytes) else payload.encode()
    sock.sendall(b'\x01' + struct.pack('!I', len(payload)) + payload)

def recv_frame(sock):
    header = recv_exact(sock, 5)
    assert header[0] == 1
    return recv_exact(sock, struct.unpack('!I', header[1:])[0]).decode()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
for i in range(40):
    cmd = f'WRITE /concurrent_file.txt WRITER_${w}_PAYLOAD_INDEX_{i}'
    send_frame(s, cmd)
    assert recv_frame(s) == 'OK'
s.close()
" >"${TEST_TMP_DIR}/conc_writer_${w}.log" 2>&1 &
    CONC_PIDS+=($!)
done

# 3 reader clients
for r in {1..3}; do
    python3 -c "
import socket, struct

def recv_exact(sock, length):
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk: raise EOFError('unexpected end of frame')
        data += chunk
    return data

def send_frame(sock, payload):
    payload = payload if isinstance(payload, bytes) else payload.encode()
    sock.sendall(b'\x01' + struct.pack('!I', len(payload)) + payload)

def recv_frame(sock):
    header = recv_exact(sock, 5)
    assert header[0] == 1
    return recv_exact(sock, struct.unpack('!I', header[1:])[0]).decode()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('${TEST_SOCK}')
for i in range(40):
    send_frame(s, 'READ /concurrent_file.txt')
    line = recv_frame(s)
    assert line == 'SEED_PAYLOAD' or line.startswith('WRITER_'), f'Corrupted line: {line}'
s.close()
" >"${TEST_TMP_DIR}/conc_reader_${r}.log" 2>&1 &
    CONC_PIDS+=($!)
done

for pid in "${CONC_PIDS[@]}"; do
    wait "${pid}"
done
echo "PASSED"

# Test 17: Graceful server shutdown via SHUTDOWN command with an idle client connected
echo -n "[Test 17/20] SHUTDOWN unblocks idle client and cleans up... "
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

# Test 18: Stale socket recovery and SIGTERM shutdown with an idle client connected
echo -n "[Test 18/20] Stale socket recovery and SIGTERM shutdown with idle client... "
# Create an orphaned socket file
python3 -c "import socket; s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind('${TEST_SOCK}'); s.close()"
if [[ ! -S "${TEST_SOCK}" ]]; then
    echo "FAILED (could not create dummy socket for stale test)"
    exit 1
fi

# Server should detect stale socket, remove it, bind, and listen successfully
"${SERVER_BIN}" -s "${TEST_SOCK}" -d "${TEST_DATA_DIR}" >"${TEST_TMP_DIR}/server_stale.log" 2>&1 &
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

# Test 19: Persistent blob content survives server restart (namespace is in-memory only)
echo -n "[Test 19/20] Persistent blob survives server restart and preserves bytes... "
RESTART_DATA_DIR="${TEST_TMP_DIR}/restart_storage"
RESTART_SOCK="${TEST_TMP_DIR}/restart.sock"

# 1. Start server instance 1
"${SERVER_BIN}" -s "${RESTART_SOCK}" -d "${RESTART_DATA_DIR}" >"${TEST_TMP_DIR}/server_restart1.log" 2>&1 &
RESTART_SRV_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/server_restart1.log"; then
    echo "FAILED (restart server 1 failed to start)"
    cat "${TEST_TMP_DIR}/server_restart1.log"
    exit 1
fi

# 2. Create file and write content
"${CLIENT_BIN}" -s "${RESTART_SOCK}" CREATE /restart_doc.txt >/dev/null
WRITE1_OUT=$("${CLIENT_BIN}" -s "${RESTART_SOCK}" WRITE /restart_doc.txt PersistentContent12345)
if ! echo "${WRITE1_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (initial write failed: ${WRITE1_OUT})"
    exit 1
fi

READ1_OUT=$("${CLIENT_BIN}" -s "${RESTART_SOCK}" READ /restart_doc.txt)
if ! echo "${READ1_OUT}" | grep -q "\[Response\] PersistentContent12345"; then
    echo "FAILED (initial read failed: ${READ1_OUT})"
    exit 1
fi

# 3. Cleanly shut down server 1
"${CLIENT_BIN}" -s "${RESTART_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${RESTART_SRV_PID}" 2>/dev/null || true

# 4. Verify blob exists on disk and contains written bytes
BLOB_FILE="${RESTART_DATA_DIR}/data.blob"
if [[ ! -f "${BLOB_FILE}" ]]; then
    echo "FAILED (data.blob missing on disk after shutdown)"
    exit 1
fi
if ! grep -q "PersistentContent12345" "${BLOB_FILE}"; then
    echo "FAILED (data.blob does not contain written bytes on disk)"
    exit 1
fi

# 5. Start server instance 2 (simulating restart)
"${SERVER_BIN}" -s "${RESTART_SOCK}" -d "${RESTART_DATA_DIR}" >"${TEST_TMP_DIR}/server_restart2.log" 2>&1 &
RESTART_SRV_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/server_restart2.log"; then
    echo "FAILED (restart server 2 failed to start)"
    cat "${TEST_TMP_DIR}/server_restart2.log"
    exit 1
fi

# 6. Verify persistent metadata: tree and file content survive restart
READ_RESTORED_OUT=$("${CLIENT_BIN}" -s "${RESTART_SOCK}" READ /restart_doc.txt 2>&1 || true)
if ! echo "${READ_RESTORED_OUT}" | grep -q "\[Response\] PersistentContent12345"; then
    echo "FAILED (expected PersistentContent12345 restored across restart, got: ${READ_RESTORED_OUT})"
    exit 1
fi

# 7. Update existing file in session 2 and verify appended content
WRITE2_OUT=$("${CLIENT_BIN}" -s "${RESTART_SOCK}" WRITE /restart_doc.txt NewAppendedBytes67890)
if ! echo "${WRITE2_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (post-restart write failed: ${WRITE2_OUT})"
    exit 1
fi

READ2_OUT=$("${CLIENT_BIN}" -s "${RESTART_SOCK}" READ /restart_doc.txt)
if ! echo "${READ2_OUT}" | grep -q "\[Response\] NewAppendedBytes67890"; then
    echo "FAILED (post-restart read failed: ${READ2_OUT})"
    exit 1
fi

# 8. Shut down server 2
"${CLIENT_BIN}" -s "${RESTART_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${RESTART_SRV_PID}" 2>/dev/null || true

# 9. Verify on disk that both session 1 and session 2 bytes are preserved in data.blob
if ! grep -q "PersistentContent12345" "${BLOB_FILE}"; then
    echo "FAILED (original content lost from data.blob)"
    exit 1
fi
if ! grep -q "NewAppendedBytes67890" "${BLOB_FILE}"; then
    echo "FAILED (new content missing from data.blob)"
    exit 1
fi
echo "PASSED"

# Test 20: Namespace mutations (RM, RMDIR, MV) over socket with server restart
echo -n "[Test 20/20] Namespace mutations (RM, RMDIR, MV) over socket with server restart... "
M7_DATA_DIR="${TEST_TMP_DIR}/m7_storage"
M7_SOCK="${TEST_TMP_DIR}/m7.sock"

"${SERVER_BIN}" -s "${M7_SOCK}" -d "${M7_DATA_DIR}" >"${TEST_TMP_DIR}/server_m7_1.log" 2>&1 &
M7_SRV_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/server_m7_1.log"; then
    echo "FAILED (m7 server 1 failed to start)"
    cat "${TEST_TMP_DIR}/server_m7_1.log"
    exit 1
fi

# RM test
"${CLIENT_BIN}" -s "${M7_SOCK}" CREATE /del_me.txt >/dev/null
"${CLIENT_BIN}" -s "${M7_SOCK}" WRITE /del_me.txt "ToDelete" >/dev/null
RM_RESP=$("${CLIENT_BIN}" -s "${M7_SOCK}" RM /del_me.txt)
if ! echo "${RM_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (RM failed: ${RM_RESP})"
    exit 1
fi
READ_DEL_RESP=$("${CLIENT_BIN}" -s "${M7_SOCK}" READ /del_me.txt 2>&1 || true)
if ! echo "${READ_DEL_RESP}" | grep -q "ERR not found"; then
    echo "FAILED (file still readable after RM: ${READ_DEL_RESP})"
    exit 1
fi

# RMDIR test
"${CLIENT_BIN}" -s "${M7_SOCK}" MKDIR /dir_a >/dev/null
"${CLIENT_BIN}" -s "${M7_SOCK}" CREATE /dir_a/file.txt >/dev/null
RMDIR_NONEMPTY=$("${CLIENT_BIN}" -s "${M7_SOCK}" RMDIR /dir_a 2>&1 || true)
if ! echo "${RMDIR_NONEMPTY}" | grep -q "ERR directory not empty"; then
    echo "FAILED (RMDIR succeeded on non-empty directory: ${RMDIR_NONEMPTY})"
    exit 1
fi
"${CLIENT_BIN}" -s "${M7_SOCK}" RM /dir_a/file.txt >/dev/null
RMDIR_RESP=$("${CLIENT_BIN}" -s "${M7_SOCK}" RMDIR /dir_a)
if ! echo "${RMDIR_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (RMDIR failed on empty directory: ${RMDIR_RESP})"
    exit 1
fi

# MV test
"${CLIENT_BIN}" -s "${M7_SOCK}" CREATE /mv_src.txt >/dev/null
"${CLIENT_BIN}" -s "${M7_SOCK}" WRITE /mv_src.txt "MovedData" >/dev/null
"${CLIENT_BIN}" -s "${M7_SOCK}" MKDIR /mv_dst_dir >/dev/null
MV_RESP=$("${CLIENT_BIN}" -s "${M7_SOCK}" MV /mv_src.txt /mv_dst_dir/mv_renamed.txt)
if ! echo "${MV_RESP}" | grep -q "\[Response\] OK"; then
    echo "FAILED (MV failed: ${MV_RESP})"
    exit 1
fi
MV_READ_RESP=$("${CLIENT_BIN}" -s "${M7_SOCK}" READ /mv_dst_dir/mv_renamed.txt)
if ! echo "${MV_READ_RESP}" | grep -q "MovedData"; then
    echo "FAILED (MV content mismatch: ${MV_READ_RESP})"
    exit 1
fi

# Shut down server 1
"${CLIENT_BIN}" -s "${M7_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${M7_SRV_PID}" 2>/dev/null || true

# Restart server 2 to verify persistence of RM, RMDIR, MV
"${SERVER_BIN}" -s "${M7_SOCK}" -d "${M7_DATA_DIR}" >"${TEST_TMP_DIR}/server_m7_2.log" 2>&1 &
M7_SRV_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/server_m7_2.log"; then
    echo "FAILED (m7 server 2 failed to start)"
    cat "${TEST_TMP_DIR}/server_m7_2.log"
    exit 1
fi

# Verify after restart: deleted file still gone
READ_DEL_RESTART=$("${CLIENT_BIN}" -s "${M7_SOCK}" READ /del_me.txt 2>&1 || true)
if ! echo "${READ_DEL_RESTART}" | grep -q "ERR not found"; then
    echo "FAILED (deleted file reappeared after restart: ${READ_DEL_RESTART})"
    exit 1
fi

# Verify after restart: moved file still at destination with correct content
READ_MV_RESTART=$("${CLIENT_BIN}" -s "${M7_SOCK}" READ /mv_dst_dir/mv_renamed.txt)
if ! echo "${READ_MV_RESTART}" | grep -q "MovedData"; then
    echo "FAILED (moved file not accessible after restart: ${READ_MV_RESTART})"
    exit 1
fi

# Test 21: Required shell commands, long listing, and connection-local cwd
echo -n "[Test 21/21] Required shell commands and per-client sessions... "
CORE_SHELL_OUTPUT=$(printf "pwd\nmkdir shell_dir\ncd shell_dir\npwd\ntouch note.txt\ntouch note.txt\nls\nls -l\ncd ..\nrm -rf shell_dir\nls\nQUIT\n" | "${CLIENT_BIN}" -s "${M7_SOCK}")
if ! echo "${CORE_SHELL_OUTPUT}" | grep -Fq "[Response] /shell_dir" || \
   ! echo "${CORE_SHELL_OUTPUT}" | grep -Fq "[Response] file 0 note.txt" || \
   ! echo "${CORE_SHELL_OUTPUT}" | grep -Fq "[Response] mv_dst_dir" || \
   echo "${CORE_SHELL_OUTPUT}" | grep -Fq "[Response] mv_dst_dir shell_dir" || \
   ! echo "${CORE_SHELL_OUTPUT}" | grep -Fq "[Response] OK"; then
    echo "FAILED (required command sequence returned unexpected output: ${CORE_SHELL_OUTPUT})"
    exit 1
fi

python3 -c "
import socket, struct

def exact(sock, size):
    data = b''
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part: raise EOFError('unexpected end of frame')
        data += part
    return data

def send(sock, message):
    payload = message.encode()
    sock.sendall(b'\x01' + struct.pack('!I', len(payload)) + payload)

def receive(sock):
    header = exact(sock, 5)
    assert header[0] == 1
    return exact(sock, struct.unpack('!I', header[1:])[0]).decode()

a = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
b = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
a.connect('${M7_SOCK}')
b.connect('${M7_SOCK}')
send(a, 'cd /mv_dst_dir')
assert receive(a) == 'OK'
send(b, 'pwd')
assert receive(b) == '/', 'new client must start at root'
send(a, 'pwd')
assert receive(a) == '/mv_dst_dir', 'cwd must persist for this connection'
send(b, 'rm -rf /mv_dst_dir')
assert receive(b) == 'OK'
send(a, 'pwd')
assert receive(a).startswith('ERR current directory was removed')
send(a, 'pwd')
assert receive(a) == '/', 'stale cwd should reset to root'
send(a, 'QUIT')
assert receive(a) == 'BYE'
send(b, 'QUIT')
assert receive(b) == 'BYE'
a.close()
b.close()
"
if [[ $? -ne 0 ]]; then
    echo "FAILED (per-client cwd or deleted-cwd recovery failed)"
    exit 1
fi
echo "PASSED"

# Clean shutdown
"${CLIENT_BIN}" -s "${M7_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${M7_SRV_PID}" 2>/dev/null || true
echo "PASSED"

echo "=========================================================="
echo " All concurrency and smoke tests passed successfully!"
echo "=========================================================="
exit 0
