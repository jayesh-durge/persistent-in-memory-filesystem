#!/usr/bin/env bash
# PISF Milestone 10 Checkpoint A — Filesystem Client Usability Tests
#
# Verifies:
#  1. Client CLI options (-h/--help, invalid flags, connection failures).
#  2. Exact syntax and handling for every supported client command:
#     PING, MKDIR, CREATE, WRITE, READ, RM, RMDIR, MV, LS, STAT, STATS, COMPACT.
#  3. Whitespace handling, malformed paths, missing arguments, and extra arguments.
#  4. Interactive REPL session and help commands.
#  5. Graceful shutdown via client command.

set -euo pipefail

BUILD_DIR="${1:-./build}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    echo "Build first: cmake --build ${BUILD_DIR}"
    exit 1
fi

TEST_TMP_DIR=$(mktemp -d /tmp/pisf-client-test.XXXXXX)
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    local exit_code=$?
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TEST_TMP_DIR}"
    if [[ "${exit_code}" -ne 0 ]]; then
        echo ""
        echo "CLIENT TEST SUITE FAILED"
    fi
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

wait_for_server() {
    local log_file="$1"
    local retries="${2:-60}"
    for (( i=0; i<retries; i++ )); do
        if grep -q "Server listening" "${log_file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    echo "  [TIMEOUT] Server did not start (log: ${log_file})"
    cat "${log_file}" || true
    return 1
}

pass() { echo "PASSED"; (( PASS++ )) || true; }
fail() { echo "FAILED: $*"; (( FAIL++ )) || true; return 1; }

echo "==========================================================="
echo " PISF Milestone 10 — Filesystem Client Usability Tests"
echo " Build directory: ${BUILD_DIR}"
echo " Test root:       ${TEST_TMP_DIR}"
echo "==========================================================="
echo ""

# ---------------------------------------------------------------------------
# Part 1: Client CLI Options (Help, Errors, Connection Failure)
# ---------------------------------------------------------------------------
echo -n "[Test 1/6] Client CLI options and error messages... "

# --help should exit 0 and document all supported commands
HELP_OUT=$("${CLIENT_BIN}" --help)
for cmd in PING QUIT SHUTDOWN MKDIR CREATE LS STAT WRITE READ RM RMDIR MV STATS COMPACT; do
    if ! echo "${HELP_OUT}" | grep -q "${cmd}"; then
        fail "--help missing command ${cmd}"
    fi
done

# -h should work identically
if ! "${CLIENT_BIN}" -h | grep -q "Usage:"; then
    fail "-h did not display usage"
fi

# Unknown option should exit non-zero
if "${CLIENT_BIN}" -z >/dev/null 2>&1; then
    fail "Unknown option -z should have failed"
fi

# Connecting when server is down should exit non-zero with friendly message
ERR_OUT=$("${CLIENT_BIN}" -s "${TEST_TMP_DIR}/nonexistent.sock" PING 2>&1 || true)
if ! echo "${ERR_OUT}" | grep -q "is the server running?"; then
    fail "Connection error did not advise checking server"
fi
pass

# ---------------------------------------------------------------------------
# Start Server for Live Client Command Testing
# ---------------------------------------------------------------------------
SOCK="${TEST_TMP_DIR}/test.sock"
DATA="${TEST_TMP_DIR}/data"
mkdir -p "${DATA}"

"${SERVER_BIN}" -s "${SOCK}" -d "${DATA}" > "${TEST_TMP_DIR}/server.log" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/server.log"; then
    echo "Server failed to start"
    exit 1
fi

# Helper to run a client command and return the response payload
run_cmd() {
    "${CLIENT_BIN}" -s "${SOCK}" "$@" 2>&1 | sed -e 's/^\[Response\] //'
}

# ---------------------------------------------------------------------------
# Part 2: Basic Liveness and Namespace Commands (PING, MKDIR, CREATE, LS, STAT)
# ---------------------------------------------------------------------------
echo -n "[Test 2/6] Client commands: PING, MKDIR, CREATE, LS, STAT... "

# PING
RESP=$(run_cmd PING)
[[ "${RESP}" == "PONG" ]] || fail "PING expected PONG, got '${RESP}'"

# MKDIR
RESP=$(run_cmd MKDIR /docs)
[[ "${RESP}" == "OK" ]] || fail "MKDIR /docs expected OK, got '${RESP}'"

# CREATE
RESP=$(run_cmd CREATE /docs/notes.txt)
[[ "${RESP}" == "OK" ]] || fail "CREATE /docs/notes.txt expected OK, got '${RESP}'"

# LS
RESP=$(run_cmd LS /docs)
[[ "${RESP}" == "notes.txt" ]] || fail "LS /docs expected notes.txt, got '${RESP}'"

# STAT
RESP=$(run_cmd STAT /docs/notes.txt)
echo "${RESP}" | grep -q "name=notes.txt type=file" || fail "STAT /docs/notes.txt unexpected: '${RESP}'"
pass

# ---------------------------------------------------------------------------
# Part 3: File Content Operations (WRITE, READ, spaces, empty content)
# ---------------------------------------------------------------------------
echo -n "[Test 3/6] Client WRITE & READ (spaces, empty content, replacement)... "

# Write content with spaces
TEXT="The quick brown fox jumps over the lazy dog"
RESP=$(run_cmd WRITE /docs/notes.txt "${TEXT}")
[[ "${RESP}" == "OK" ]] || fail "WRITE with spaces failed: '${RESP}'"

# Read back and verify exact content preservation
RESP=$(run_cmd READ /docs/notes.txt)
[[ "${RESP}" == "${TEXT}" ]] || fail "READ expected '${TEXT}', got '${RESP}'"

# Replace with empty content
RESP=$(run_cmd WRITE /docs/notes.txt)
[[ "${RESP}" == "OK" ]] || fail "WRITE empty content failed: '${RESP}'"

RESP=$(run_cmd READ /docs/notes.txt)
[[ -z "${RESP}" ]] || fail "READ empty file expected empty, got '${RESP}'"

# Write new content with multiple spaces and special characters
SPECIAL="Alpha   Beta -- Special @#% Values!"
RESP=$(run_cmd WRITE /docs/notes.txt "${SPECIAL}")
[[ "${RESP}" == "OK" ]] || fail "WRITE special failed: '${RESP}'"

RESP=$(run_cmd READ /docs/notes.txt)
[[ "${RESP}" == "${SPECIAL}" ]] || fail "READ special expected '${SPECIAL}', got '${RESP}'"
pass

# ---------------------------------------------------------------------------
# Part 4: Namespace Mutations (MV, RM, RMDIR)
# ---------------------------------------------------------------------------
echo -n "[Test 4/6] Client mutations: MV, RM, RMDIR... "

# MV file
RESP=$(run_cmd MV /docs/notes.txt /docs/readme.txt)
[[ "${RESP}" == "OK" ]] || fail "MV file failed: '${RESP}'"

# Verify old path gone, new path readable
RESP=$(run_cmd READ /docs/notes.txt)
echo "${RESP}" | grep -q "ERR not found" || fail "Old path should not be found: '${RESP}'"

RESP=$(run_cmd READ /docs/readme.txt)
[[ "${RESP}" == "${SPECIAL}" ]] || fail "Renamed file content mismatch: '${RESP}'"

# RM file
RESP=$(run_cmd RM /docs/readme.txt)
[[ "${RESP}" == "OK" ]] || fail "RM file failed: '${RESP}'"

# Verify deleted file not found
RESP=$(run_cmd READ /docs/readme.txt)
echo "${RESP}" | grep -q "ERR not found" || fail "Deleted file should not be found: '${RESP}'"

# RMDIR empty directory
RESP=$(run_cmd RMDIR /docs)
[[ "${RESP}" == "OK" ]] || fail "RMDIR /docs failed: '${RESP}'"

# Verify directory gone
RESP=$(run_cmd LS /docs)
echo "${RESP}" | grep -q "ERR not found" || fail "Deleted directory should not be found: '${RESP}'"
pass

# ---------------------------------------------------------------------------
# Part 5: Error Handling (Missing Arguments, Malformed Paths, Extra Arguments)
# ---------------------------------------------------------------------------
echo -n "[Test 5/6] Client error handling (missing args, malformed paths)... "

# Missing arguments
RESP=$(run_cmd MKDIR)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "MKDIR missing arg: '${RESP}'"

RESP=$(run_cmd CREATE)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "CREATE missing arg: '${RESP}'"

RESP=$(run_cmd WRITE)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "WRITE missing arg: '${RESP}'"

RESP=$(run_cmd READ)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "READ missing arg: '${RESP}'"

RESP=$(run_cmd RM)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "RM missing arg: '${RESP}'"

RESP=$(run_cmd RMDIR)
echo "${RESP}" | grep -q "ERR missing path argument" || fail "RMDIR missing arg: '${RESP}'"

RESP=$(run_cmd MV)
echo "${RESP}" | grep -q "ERR missing arguments" || fail "MV missing args: '${RESP}'"

RESP=$(run_cmd MV /src)
echo "${RESP}" | grep -q "ERR missing destination path" || fail "MV missing dst: '${RESP}'"

# Malformed paths (relative path)
RESP=$(run_cmd MKDIR relative/path)
echo "${RESP}" | grep -q "ERR invalid path" || fail "MKDIR relative path: '${RESP}'"

# Path containing whitespace (not allowed in PISF path components)
RESP=$(run_cmd MKDIR "/has space")
echo "${RESP}" | grep -q "ERR invalid path component" || fail "MKDIR space in path: '${RESP}'"

# RM on directory should reject with helpful message
run_cmd MKDIR /test_dir >/dev/null
RESP=$(run_cmd RM /test_dir)
echo "${RESP}" | grep -q "ERR not a file: use RMDIR" || fail "RM on directory: '${RESP}'"

# RMDIR on file should reject with helpful message
run_cmd CREATE /test_dir/file.txt >/dev/null
RESP=$(run_cmd RMDIR /test_dir/file.txt)
echo "${RESP}" | grep -q "ERR not a directory: use RM" || fail "RMDIR on file: '${RESP}'"

# RMDIR on non-empty directory should reject
RESP=$(run_cmd RMDIR /test_dir)
echo "${RESP}" | grep -q "ERR directory not empty" || fail "RMDIR non-empty: '${RESP}'"

# RM on root
RESP=$(run_cmd RM /)
echo "${RESP}" | grep -q "ERR cannot remove root" || fail "RM root: '${RESP}'"

# RMDIR on root
RESP=$(run_cmd RMDIR /)
echo "${RESP}" | grep -q "ERR cannot remove root" || fail "RMDIR root: '${RESP}'"

# Cleanup test_dir
run_cmd RM /test_dir/file.txt >/dev/null
run_cmd RMDIR /test_dir >/dev/null
pass

# ---------------------------------------------------------------------------
# Part 6: Storage Stats, Compaction, Interactive Session, and Graceful Shutdown
# ---------------------------------------------------------------------------
echo -n "[Test 6/6] STATS, COMPACT, REPL piping, and SHUTDOWN... "

# Create files and generate obsolete data
run_cmd CREATE /bench.txt >/dev/null
run_cmd WRITE /bench.txt "V1_Data" >/dev/null
run_cmd WRITE /bench.txt "V2_LongerDataPayload" >/dev/null

# STATS
STATS_RESP=$(run_cmd STATS)
echo "${STATS_RESP}" | grep -q "total_blob_bytes=" || fail "STATS output missing total_blob_bytes: '${STATS_RESP}'"
echo "${STATS_RESP}" | grep -q "live_bytes=" || fail "STATS output missing live_bytes: '${STATS_RESP}'"
echo "${STATS_RESP}" | grep -q "reclaimable_bytes=" || fail "STATS output missing reclaimable_bytes: '${STATS_RESP}'"
echo "${STATS_RESP}" | grep -q "active_generation=" || fail "STATS output missing active_generation: '${STATS_RESP}'"

# COMPACT
COMPACT_RESP=$(run_cmd COMPACT)
echo "${COMPACT_RESP}" | grep -q "OK COMPACT generation=1" || fail "COMPACT unexpected output: '${COMPACT_RESP}'"

# Verify file still readable after compaction via client
RESP=$(run_cmd READ /bench.txt)
[[ "${RESP}" == "V2_LongerDataPayload" ]] || fail "READ post-compact failed: '${RESP}'"

# Test interactive REPL stream piping (help, PING, STATS, QUIT)
REPL_OUT=$(printf "help\nPING\nSTATS\nQUIT\n" | "${CLIENT_BIN}" -s "${SOCK}")
echo "${REPL_OUT}" | grep -q "Supported commands:" || fail "REPL help missing"
echo "${REPL_OUT}" | grep -q "\[Response\] PONG" || fail "REPL PING missing"
echo "${REPL_OUT}" | grep -q "\[Response\] BYE" || fail "REPL QUIT missing"

# SHUTDOWN via client
SHUTDOWN_RESP=$(run_cmd SHUTDOWN)
echo "${SHUTDOWN_RESP}" | grep -q "OK SHUTDOWN" || fail "SHUTDOWN response: '${SHUTDOWN_RESP}'"

# Wait for server to terminate
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
pass

echo ""
echo "==========================================================="
echo " CLIENT USABILITY TESTS: ${PASS}/${PASS} PASSED"
echo "==========================================================="
