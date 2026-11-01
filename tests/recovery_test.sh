#!/usr/bin/env bash
# PISF Milestone 6 — End-to-End Filesystem Recovery & Persistent Metadata Tests
# Tests the complete restart lifecycle, state restoration, node ID preservation,
# corrupt/missing metadata rejection, and error handling using live binaries.

set -euo pipefail

BUILD_DIR="${1:-./build}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    exit 1
fi

TEST_TMP_DIR=$(mktemp -d /tmp/pisf-recovery-test.XXXXXX)
SERVER_PID=""

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
    for _ in {1..40}; do
        if grep -q "Server listening" "${log_file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

echo "=========================================================="
echo " Running PISF End-to-End Recovery & Restart Test Suite"
echo " Build directory: ${BUILD_DIR}"
echo " Test root:       ${TEST_TMP_DIR}"
echo "=========================================================="

# -------------------------------------------------------------
# Test 1: Required Milestone 6 Sequence (Steps 1 through 10)
# -------------------------------------------------------------
echo -n "[Test 1/7] Required sequence: create, write, shutdown, restart, verify, extend... "
T1_DIR="${TEST_TMP_DIR}/test1_data"
T1_SOCK="${TEST_TMP_DIR}/test1.sock"
mkdir -p "${T1_DIR}"

# Step 1: Start fresh server
"${SERVER_BIN}" -s "${T1_SOCK}" -d "${T1_DIR}" >"${TEST_TMP_DIR}/t1_srv1.log" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/t1_srv1.log"; then
    echo "FAILED (server 1 failed to start)"
    cat "${TEST_TMP_DIR}/t1_srv1.log"
    exit 1
fi

# Step 2: Create /notes
MKDIR_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" MKDIR /notes)
if ! echo "${MKDIR_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (MKDIR /notes failed: ${MKDIR_OUT})"
    exit 1
fi

# Step 3: Create /notes/cn.txt
CREATE_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" CREATE /notes/cn.txt)
if ! echo "${CREATE_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (CREATE /notes/cn.txt failed: ${CREATE_OUT})"
    exit 1
fi

STAT1_BEFORE=$("${CLIENT_BIN}" -s "${T1_SOCK}" STAT /notes/cn.txt)
CN_ID=$(echo "${STAT1_BEFORE}" | sed -n 's/.*id=\([0-9]\+\).*/\1/p')

# Step 4: Write content to /notes/cn.txt
WRITE_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" WRITE /notes/cn.txt "PersistentContentForMilestone6")
if ! echo "${WRITE_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (WRITE /notes/cn.txt failed: ${WRITE_OUT})"
    exit 1
fi

# Step 5: Shut down server gracefully
SHUT_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" SHUTDOWN)
if ! echo "${SHUT_OUT}" | grep -q "\[Response\] OK SHUTDOWN"; then
    echo "FAILED (SHUTDOWN failed: ${SHUT_OUT})"
    exit 1
fi
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

# Verify files exist on disk before restart
if [[ ! -f "${T1_DIR}/metadata.pisf" ]] || [[ ! -f "${T1_DIR}/data.blob" ]]; then
    echo "FAILED (metadata.pisf or data.blob missing on disk after shutdown)"
    exit 1
fi

# Step 6: Start server again with the same data directory
"${SERVER_BIN}" -s "${T1_SOCK}" -d "${T1_DIR}" >"${TEST_TMP_DIR}/t1_srv2.log" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/t1_srv2.log"; then
    echo "FAILED (server 2 restart failed to start)"
    cat "${TEST_TMP_DIR}/t1_srv2.log"
    exit 1
fi

# Step 7: Verify LS /notes still lists cn.txt
LS_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" LS /notes)
if ! echo "${LS_OUT}" | grep -q "\[Response\] cn.txt"; then
    echo "FAILED (LS /notes did not list cn.txt: ${LS_OUT})"
    exit 1
fi

# Step 8: Verify STAT /notes/cn.txt reports a file
STAT2_AFTER=$("${CLIENT_BIN}" -s "${T1_SOCK}" STAT /notes/cn.txt)
if ! echo "${STAT2_AFTER}" | grep -q "type=file"; then
    echo "FAILED (STAT did not report file: ${STAT2_AFTER})"
    exit 1
fi
CN_ID_AFTER=$(echo "${STAT2_AFTER}" | sed -n 's/.*id=\([0-9]\+\).*/\1/p')
if [[ "${CN_ID}" != "${CN_ID_AFTER}" ]]; then
    echo "FAILED (NodeId changed across restart: before=${CN_ID}, after=${CN_ID_AFTER})"
    exit 1
fi

# Step 9: Verify READ /notes/cn.txt returns the original content
READ_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" READ /notes/cn.txt)
if ! echo "${READ_OUT}" | grep -q "\[Response\] PersistentContentForMilestone6"; then
    echo "FAILED (READ did not return original content: ${READ_OUT})"
    exit 1
fi

# Step 10: Create another file after restart and verify NodeId allocation does not collide
CREATE2_OUT=$("${CLIENT_BIN}" -s "${T1_SOCK}" CREATE /notes/another.txt)
if ! echo "${CREATE2_OUT}" | grep -q "\[Response\] OK"; then
    echo "FAILED (post-restart CREATE failed: ${CREATE2_OUT})"
    exit 1
fi

STAT_ANOTHER=$("${CLIENT_BIN}" -s "${T1_SOCK}" STAT /notes/another.txt)
ANOTHER_ID=$(echo "${STAT_ANOTHER}" | sed -n 's/.*id=\([0-9]\+\).*/\1/p')
if [[ "${ANOTHER_ID}" -le "${CN_ID_AFTER}" ]]; then
    echo "FAILED (NodeId allocation collided or regressed: another_id=${ANOTHER_ID}, restored_id=${CN_ID_AFTER})"
    exit 1
fi

# Clean shutdown
"${CLIENT_BIN}" -s "${T1_SOCK}" SHUTDOWN >/dev/null 2>&1 || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
echo "PASSED"

# -------------------------------------------------------------
# Test 2: Empty Files & Deeply Nested Directories
# -------------------------------------------------------------
echo -n "[Test 2/7] Empty files and nested directories across restart... "
T2_DIR="${TEST_TMP_DIR}/test2_data"
T2_SOCK="${TEST_TMP_DIR}/test2.sock"
mkdir -p "${T2_DIR}"

"${SERVER_BIN}" -s "${T2_SOCK}" -d "${T2_DIR}" >"${TEST_TMP_DIR}/t2_srv1.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t2_srv1.log"

"${CLIENT_BIN}" -s "${T2_SOCK}" MKDIR /a >/dev/null
"${CLIENT_BIN}" -s "${T2_SOCK}" MKDIR /a/b >/dev/null
"${CLIENT_BIN}" -s "${T2_SOCK}" MKDIR /a/b/c >/dev/null
"${CLIENT_BIN}" -s "${T2_SOCK}" CREATE /a/b/c/nested.txt >/dev/null
"${CLIENT_BIN}" -s "${T2_SOCK}" WRITE /a/b/c/nested.txt DeepContent123 >/dev/null
"${CLIENT_BIN}" -s "${T2_SOCK}" CREATE /empty.txt >/dev/null

"${CLIENT_BIN}" -s "${T2_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

# Restart and verify
"${SERVER_BIN}" -s "${T2_SOCK}" -d "${T2_DIR}" >"${TEST_TMP_DIR}/t2_srv2.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t2_srv2.log"

if ! "${CLIENT_BIN}" -s "${T2_SOCK}" LS /a/b | grep -q "\[Response\] c"; then
    echo "FAILED (LS /a/b failed)"
    exit 1
fi

if ! "${CLIENT_BIN}" -s "${T2_SOCK}" READ /a/b/c/nested.txt | grep -q "\[Response\] DeepContent123"; then
    echo "FAILED (READ /a/b/c/nested.txt failed)"
    exit 1
fi

STAT_EMPTY=$("${CLIENT_BIN}" -s "${T2_SOCK}" STAT /empty.txt)
if ! echo "${STAT_EMPTY}" | grep -q "name=empty.txt type=file"; then
    echo "FAILED (STAT /empty.txt failed: ${STAT_EMPTY})"
    exit 1
fi

READ_EMPTY=$("${CLIENT_BIN}" -s "${T2_SOCK}" READ /empty.txt)
if [[ "${READ_EMPTY}" != "[Response] " ]]; then
    echo "FAILED (READ /empty.txt did not return empty payload: '${READ_EMPTY}')"
    exit 1
fi

"${CLIENT_BIN}" -s "${T2_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
echo "PASSED"

# -------------------------------------------------------------
# Test 3: Multiple Consecutive Restarts
# -------------------------------------------------------------
echo -n "[Test 3/7] Multiple consecutive restarts with incremental mutations... "
T3_DIR="${TEST_TMP_DIR}/test3_data"
T3_SOCK="${TEST_TMP_DIR}/test3.sock"
mkdir -p "${T3_DIR}"

# Session 1: Create step1
"${SERVER_BIN}" -s "${T3_SOCK}" -d "${T3_DIR}" >"${TEST_TMP_DIR}/t3_srv1.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t3_srv1.log"
"${CLIENT_BIN}" -s "${T3_SOCK}" CREATE /step1.txt >/dev/null
"${CLIENT_BIN}" -s "${T3_SOCK}" WRITE /step1.txt Step1DataPayload >/dev/null
"${CLIENT_BIN}" -s "${T3_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true

# Session 2: Create step2 and replace step1
"${SERVER_BIN}" -s "${T3_SOCK}" -d "${T3_DIR}" >"${TEST_TMP_DIR}/t3_srv2.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t3_srv2.log"
"${CLIENT_BIN}" -s "${T3_SOCK}" CREATE /step2.txt >/dev/null
"${CLIENT_BIN}" -s "${T3_SOCK}" WRITE /step2.txt Step2DataPayload >/dev/null
"${CLIENT_BIN}" -s "${T3_SOCK}" WRITE /step1.txt Step1OverwrittenPayload >/dev/null
"${CLIENT_BIN}" -s "${T3_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true

# Session 3: Verify both files
"${SERVER_BIN}" -s "${T3_SOCK}" -d "${T3_DIR}" >"${TEST_TMP_DIR}/t3_srv3.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t3_srv3.log"

READ_S1=$("${CLIENT_BIN}" -s "${T3_SOCK}" READ /step1.txt)
if ! echo "${READ_S1}" | grep -q "Step1OverwrittenPayload"; then
    echo "FAILED (step1 overwrite not preserved across restarts: ${READ_S1})"
    exit 1
fi

READ_S2=$("${CLIENT_BIN}" -s "${T3_SOCK}" READ /step2.txt)
if ! echo "${READ_S2}" | grep -q "Step2DataPayload"; then
    echo "FAILED (step2 not preserved across restarts: ${READ_S2})"
    exit 1
fi

"${CLIENT_BIN}" -s "${T3_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
echo "PASSED"

# -------------------------------------------------------------
# Test 4: Fresh Data Directory Creation
# -------------------------------------------------------------
echo -n "[Test 4/7] Fresh data directory initialization... "
T4_DIR="${TEST_TMP_DIR}/test4_fresh_data"
T4_SOCK="${TEST_TMP_DIR}/test4.sock"

# Directory does not even exist yet
"${SERVER_BIN}" -s "${T4_SOCK}" -d "${T4_DIR}" >"${TEST_TMP_DIR}/t4_srv.log" 2>&1 &
SERVER_PID=$!
if ! wait_for_server "${TEST_TMP_DIR}/t4_srv.log"; then
    echo "FAILED (fresh server failed to start)"
    exit 1
fi

PING_RES=$("${CLIENT_BIN}" -s "${T4_SOCK}" PING)
if ! echo "${PING_RES}" | grep -q "PONG"; then
    echo "FAILED (fresh server did not respond to PING)"
    exit 1
fi

if [[ ! -f "${T4_DIR}/metadata.pisf" ]] || [[ ! -f "${T4_DIR}/data.blob" ]]; then
    echo "FAILED (fresh server did not initialize metadata.pisf and data.blob)"
    exit 1
fi

ROOT_STAT=$("${CLIENT_BIN}" -s "${T4_SOCK}" STAT /)
if ! echo "${ROOT_STAT}" | grep -q "name=/ type=directory id=1"; then
    echo "FAILED (root node invalid in fresh directory: ${ROOT_STAT})"
    exit 1
fi

"${CLIENT_BIN}" -s "${T4_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
echo "PASSED"

# -------------------------------------------------------------
# Test 5: Corrupt or Truncated Metadata Startup Refusal
# -------------------------------------------------------------
echo -n "[Test 5/7] Corrupt or truncated metadata startup refusal... "
T5_DIR="${TEST_TMP_DIR}/test5_corrupt_data"
T5_SOCK="${TEST_TMP_DIR}/test5.sock"
mkdir -p "${T5_DIR}"

# Write corrupted metadata file
CORRUPT_BYTES="CorruptedNonBinaryMetadataContentPISF"
echo -n "${CORRUPT_BYTES}" > "${T5_DIR}/metadata.pisf"
touch "${T5_DIR}/data.blob"

if "${SERVER_BIN}" -s "${T5_SOCK}" -d "${T5_DIR}" >"${TEST_TMP_DIR}/t5_srv.log" 2>&1; then
    echo "FAILED (server should have refused startup on corrupted metadata)"
    exit 1
fi

# Verify error message logged
if ! grep -q "invalid metadata\|checksum mismatch" "${TEST_TMP_DIR}/t5_srv.log"; then
    echo "FAILED (server did not log descriptive metadata error: $(cat "${TEST_TMP_DIR}/t5_srv.log"))"
    exit 1
fi

# Verify corrupted file was NOT silently overwritten or deleted
REMAINING_BYTES=$(cat "${T5_DIR}/metadata.pisf")
if [[ "${REMAINING_BYTES}" != "${CORRUPT_BYTES}" ]]; then
    echo "FAILED (corrupted metadata was overwritten or modified)"
    exit 1
fi
echo "PASSED"

# -------------------------------------------------------------
# Test 6: Missing Metadata with Existing data.blob Startup Refusal
# -------------------------------------------------------------
echo -n "[Test 6/7] Missing metadata with existing data.blob refusal... "
T6_DIR="${TEST_TMP_DIR}/test6_missing_meta"
T6_SOCK="${TEST_TMP_DIR}/test6.sock"
mkdir -p "${T6_DIR}"

# Create blob but NO metadata.pisf
echo "ImportantExistingBlobBytes12345" > "${T6_DIR}/data.blob"

if "${SERVER_BIN}" -s "${T6_SOCK}" -d "${T6_DIR}" >"${TEST_TMP_DIR}/t6_srv.log" 2>&1; then
    echo "FAILED (server should have refused startup when metadata.pisf is missing in non-empty directory)"
    exit 1
fi

if ! grep -q "missing metadata" "${TEST_TMP_DIR}/t6_srv.log"; then
    echo "FAILED (expected missing metadata error log, got: $(cat "${TEST_TMP_DIR}/t6_srv.log"))"
    exit 1
fi

# Verify data.blob was not modified or overwritten
if ! grep -q "ImportantExistingBlobBytes12345" "${T6_DIR}/data.blob"; then
    echo "FAILED (data.blob was modified or overwritten on failed startup)"
    exit 1
fi

# Verify metadata.pisf was NOT created
if [[ -f "${T6_DIR}/metadata.pisf" ]]; then
    echo "FAILED (metadata.pisf was created despite startup error)"
    exit 1
fi
echo "PASSED"

# -------------------------------------------------------------
# Test 7: Invalid Content Extents Startup Refusal
# -------------------------------------------------------------
echo -n "[Test 7/7] Invalid content extent exceeding blob startup refusal... "
T7_DIR="${TEST_TMP_DIR}/test7_bad_extent"
T7_SOCK="${TEST_TMP_DIR}/test7.sock"
mkdir -p "${T7_DIR}"

# 1. Start server, create a file and write content
"${SERVER_BIN}" -s "${T7_SOCK}" -d "${T7_DIR}" >"${TEST_TMP_DIR}/t7_srv1.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${TEST_TMP_DIR}/t7_srv1.log"
"${CLIENT_BIN}" -s "${T7_SOCK}" CREATE /target.txt >/dev/null
"${CLIENT_BIN}" -s "${T7_SOCK}" WRITE /target.txt InitialExtentData1234567890 >/dev/null
"${CLIENT_BIN}" -s "${T7_SOCK}" SHUTDOWN >/dev/null
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

# 2. Truncate data.blob so that metadata extent exceeds blob length
truncate -s 5 "${T7_DIR}/data.blob"

# 3. Attempt restart
if "${SERVER_BIN}" -s "${T7_SOCK}" -d "${T7_DIR}" >"${TEST_TMP_DIR}/t7_srv2.log" 2>&1; then
    echo "FAILED (server should have refused startup when blob size is less than metadata extent)"
    exit 1
fi

if ! grep -q "exceeds blob size" "${TEST_TMP_DIR}/t7_srv2.log"; then
    echo "FAILED (expected exceeds blob size error log, got: $(cat "${TEST_TMP_DIR}/t7_srv2.log"))"
    exit 1
fi
echo "PASSED"

echo "=========================================================="
echo " All recovery and restart tests passed successfully!"
echo "=========================================================="
exit 0
