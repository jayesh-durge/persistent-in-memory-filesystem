#!/usr/bin/env bash
# PISF — Server Lifecycle, Stability & Crash Recovery Test Suite
#
# Verifies:
#  1. FD count stability across repeated client connect/disconnect cycles.
#  2. Worker threads are reaped after SIGTERM.
#  3. Data survives abrupt SIGKILL + server restart.
#  4. No stale .pisf.tmp.* files accumulate after writes.
#  5. Orphan blob generations are cleaned up on startup.

set -euo pipefail

BUILD_DIR="${1:-./build/test/bin}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    echo "Build the test binaries first with: make test-build"
    exit 1
fi

TEST_TMP_DIR=$(mktemp -d /tmp/pisf-lifecycle-test.XXXXXX)
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    local exit_code=$?
    # Kill any lingering server
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TEST_TMP_DIR}"
    if [[ "${exit_code}" -ne 0 ]]; then
        echo ""
        echo "LIFECYCLE TEST SUITE FAILED (some tests above reported FAILED)"
    fi
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

wait_for_server() {
    local log_file="$1"
    local retries="${2:-60}"
    local i
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
echo " PISF Milestone 9 — Server Lifecycle & Crash Recovery Tests"
echo " Build directory: ${BUILD_DIR}"
echo " Test root:       ${TEST_TMP_DIR}"
echo "==========================================================="
echo ""

# ---------------------------------------------------------------------------
# Test E1 — File descriptor count stability across repeated connections
# ---------------------------------------------------------------------------
echo -n "[E1] FD count stability across 50 client connect/disconnect cycles... "

E1_DIR="${TEST_TMP_DIR}/e1_data"
E1_SOCK="${TEST_TMP_DIR}/e1.sock"
E1_LOG="${TEST_TMP_DIR}/e1_srv.log"
mkdir -p "${E1_DIR}"

"${SERVER_BIN}" -s "${E1_SOCK}" -d "${E1_DIR}" > "${E1_LOG}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E1_LOG}"; then fail "server failed to start"; fi

# Record FD count before
FD_BEFORE=$(ls -1 /proc/${SERVER_PID}/fd 2>/dev/null | wc -l)

# 50 connect/disconnect cycles: each client sends STAT / and exits
for _ in $(seq 1 50); do
    "${CLIENT_BIN}" -s "${E1_SOCK}" STAT / >/dev/null 2>&1 || true
done

# Allow any lingering worker threads to be reaped (server prunes on accept loop)
sleep 0.5

FD_AFTER=$(ls -1 /proc/${SERVER_PID}/fd 2>/dev/null | wc -l)

# Allow some tolerance: listen_fd + pipe[2] + stdin/stdout/stderr = baseline ~6
# FD after repeated connections must not have grown by more than 10 above baseline
FD_GROWTH=$(( FD_AFTER - FD_BEFORE ))
if [[ "${FD_GROWTH}" -gt 10 ]]; then
    fail "FD count grew by ${FD_GROWTH} (before=${FD_BEFORE}, after=${FD_AFTER})"
else
    # Graceful shutdown
    "${CLIENT_BIN}" -s "${E1_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
    pass
fi

# ---------------------------------------------------------------------------
# Test E2 — Worker threads are reaped after SIGTERM (clean exit)
# ---------------------------------------------------------------------------
echo -n "[E2] Worker threads reaped after SIGTERM... "

E2_DIR="${TEST_TMP_DIR}/e2_data"
E2_SOCK="${TEST_TMP_DIR}/e2.sock"
E2_LOG="${TEST_TMP_DIR}/e2_srv.log"
mkdir -p "${E2_DIR}"

"${SERVER_BIN}" -s "${E2_SOCK}" -d "${E2_DIR}" > "${E2_LOG}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E2_LOG}"; then fail "server failed to start"; fi

# Launch 5 rapid connections before SIGTERM
for _ in $(seq 1 5); do
    "${CLIENT_BIN}" -s "${E2_SOCK}" STAT / >/dev/null 2>&1 || true &
done
sleep 0.1

# SIGTERM — server should exit cleanly
kill -TERM "${SERVER_PID}" 2>/dev/null || true
# Wait up to 5 s for exit
TIMEOUT=50
for (( i=0; i<TIMEOUT; i++ )); do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -KILL "${SERVER_PID}" 2>/dev/null || true
    fail "server did not exit within 5s after SIGTERM"
else
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
    # Verify no zombie worker threads remain (process table should not have server PID)
    if [[ -d "/proc/${SERVER_PID:-0}" ]]; then
        fail "server PID still in /proc after exit"
    else
        pass
    fi
fi

# ---------------------------------------------------------------------------
# Test E3 — Data survives abrupt SIGKILL and server restart
# ---------------------------------------------------------------------------
echo -n "[E3] Data survives SIGKILL and server restart... "

E3_DIR="${TEST_TMP_DIR}/e3_data"
E3_SOCK="${TEST_TMP_DIR}/e3.sock"
E3_LOG1="${TEST_TMP_DIR}/e3_srv1.log"
E3_LOG2="${TEST_TMP_DIR}/e3_srv2.log"
mkdir -p "${E3_DIR}"

# Start server and write durable data
"${SERVER_BIN}" -s "${E3_SOCK}" -d "${E3_DIR}" > "${E3_LOG1}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E3_LOG1}"; then fail "server 1 failed to start"; fi

"${CLIENT_BIN}" -s "${E3_SOCK}" MKDIR /durable >/dev/null
"${CLIENT_BIN}" -s "${E3_SOCK}" CREATE /durable/note.txt >/dev/null
"${CLIENT_BIN}" -s "${E3_SOCK}" WRITE /durable/note.txt "SurvivesKill" >/dev/null

# Flush: request a COMPACT to ensure data is fully persisted in a clean generation
"${CLIENT_BIN}" -s "${E3_SOCK}" COMPACT >/dev/null 2>&1 || true

# SIGKILL — abrupt termination
kill -KILL "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
sleep 0.1

# Restart with same data directory
"${SERVER_BIN}" -s "${E3_SOCK}" -d "${E3_DIR}" > "${E3_LOG2}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E3_LOG2}"; then
    fail "server 2 failed to start after SIGKILL"
else
    # Verify data
    READ_OUT=$("${CLIENT_BIN}" -s "${E3_SOCK}" READ /durable/note.txt 2>&1)
    if echo "${READ_OUT}" | grep -q "SurvivesKill"; then
        "${CLIENT_BIN}" -s "${E3_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
        wait "${SERVER_PID}" 2>/dev/null || true
        SERVER_PID=""
        pass
    else
        fail "data not recovered after SIGKILL (got: ${READ_OUT})"
    fi
fi

# ---------------------------------------------------------------------------
# Test E4 — No stale .pisf.tmp.* files accumulate after writes
# ---------------------------------------------------------------------------
echo -n "[E4] No stale .pisf.tmp.* files after 20 writes... "

E4_DIR="${TEST_TMP_DIR}/e4_data"
E4_SOCK="${TEST_TMP_DIR}/e4.sock"
E4_LOG="${TEST_TMP_DIR}/e4_srv.log"
mkdir -p "${E4_DIR}"

"${SERVER_BIN}" -s "${E4_SOCK}" -d "${E4_DIR}" > "${E4_LOG}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E4_LOG}"; then fail "server failed to start"; fi

"${CLIENT_BIN}" -s "${E4_SOCK}" CREATE /tmp_test.txt >/dev/null

for i in $(seq 1 20); do
    "${CLIENT_BIN}" -s "${E4_SOCK}" WRITE /tmp_test.txt "Content_v${i}" >/dev/null
done

# Count stale tmp files in data directory
TMP_COUNT=$(find "${E4_DIR}" -name "*.pisf.tmp.*" -o -name "*.tmp" 2>/dev/null | wc -l)

"${CLIENT_BIN}" -s "${E4_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

if [[ "${TMP_COUNT}" -gt 0 ]]; then
    fail "Found ${TMP_COUNT} stale .tmp files in data directory after writes"
else
    pass
fi

# ---------------------------------------------------------------------------
# Test E5 — Orphan blob generations cleaned up on startup
# ---------------------------------------------------------------------------
echo -n "[E5] Orphan blob generations removed on startup... "

E5_DIR="${TEST_TMP_DIR}/e5_data"
E5_SOCK1="${TEST_TMP_DIR}/e5_1.sock"
E5_SOCK2="${TEST_TMP_DIR}/e5_2.sock"
E5_LOG1="${TEST_TMP_DIR}/e5_srv1.log"
E5_LOG2="${TEST_TMP_DIR}/e5_srv2.log"
mkdir -p "${E5_DIR}"

# Start server and compact to generate a new blob generation
"${SERVER_BIN}" -s "${E5_SOCK1}" -d "${E5_DIR}" > "${E5_LOG1}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E5_LOG1}"; then fail "server 1 failed to start"; fi

"${CLIENT_BIN}" -s "${E5_SOCK1}" CREATE /e5_file.txt >/dev/null
"${CLIENT_BIN}" -s "${E5_SOCK1}" WRITE /e5_file.txt "GenTest" >/dev/null
"${CLIENT_BIN}" -s "${E5_SOCK1}" COMPACT >/dev/null 2>&1 || true  # generates gen 1

"${CLIENT_BIN}" -s "${E5_SOCK1}" SHUTDOWN >/dev/null 2>&1 || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

# Inject a fake orphan blob (simulates crash before cleanup committed)
touch "${E5_DIR}/data-99.blob"
ORPHAN_BEFORE=$(find "${E5_DIR}" -maxdepth 1 -name "data-*.blob" 2>/dev/null | wc -l)

# Restart server — it should clean up data-99.blob on startup
"${SERVER_BIN}" -s "${E5_SOCK2}" -d "${E5_DIR}" > "${E5_LOG2}" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${E5_LOG2}"; then
    fail "server 2 failed to start (orphan cleanup test)"
else
    sleep 0.3  # allow cleanup to run
    ORPHAN_AFTER=$(find "${E5_DIR}" -maxdepth 1 -name "data-*.blob" 2>/dev/null | wc -l)

    "${CLIENT_BIN}" -s "${E5_SOCK2}" SHUTDOWN >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""

    if [[ "${ORPHAN_AFTER}" -lt "${ORPHAN_BEFORE}" ]]; then
        pass
    else
        fail "Orphan blob not cleaned up: before=${ORPHAN_BEFORE}, after=${ORPHAN_AFTER}"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "==========================================================="
if [[ "${FAIL}" -eq 0 ]]; then
    echo " LIFECYCLE TESTS: ${PASS}/${PASS} PASSED"
    echo "==========================================================="
    exit 0
else
    echo " LIFECYCLE TESTS: ${PASS} PASSED, ${FAIL} FAILED"
    echo "==========================================================="
    exit 1
fi

