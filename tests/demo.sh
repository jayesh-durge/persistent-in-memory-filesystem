#!/usr/bin/env bash
# PISF Milestone 10 Checkpoint C — Reproducible Filesystem Demonstration Script
#
# Deterministically demonstrates the end-to-end PISF lifecycle:
#  1. Isolated temporary environment setup.
#  2. Server startup & client connectivity.
#  3. Nested directory creation.
#  4. File creation.
#  5. Content write & verified read (including content with spaces).
#  6. File rename (MV).
#  7. Directory move across subtrees (MV).
#  8. File deletion (RM).
#  9. Empty directory removal (RMDIR).
# 10. Graceful server shutdown.
# 11. Server restart with existing persistent data directory.
# 12. Full crash recovery verification (surviving files, directories, contents).
# 13. Persistent storage statistics inspection.
# 14. Safe environment cleanup without orphaned processes.

set -euo pipefail

# ANSI color codes for clear terminal demonstration output
BOLD="\033[1m"
GREEN="\033[0;32m"
CYAN="\033[0;36m"
YELLOW="\033[0;33m"
RESET="\033[0m"

BUILD_DIR="${1:-./build}"
SERVER_BIN="${BUILD_DIR}/pisf-server"
CLIENT_BIN="${BUILD_DIR}/pisf-client"

if [[ ! -x "${SERVER_BIN}" ]] || [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Server or client executable not found in ${BUILD_DIR}"
    echo "Build first: cmake --build ${BUILD_DIR}"
    exit 1
fi

# 1. Create a temporary data directory
DEMO_TMP_DIR=$(mktemp -d /tmp/pisf-demo.XXXXXX)
DEMO_DATA_DIR="${DEMO_TMP_DIR}/storage"
DEMO_SOCK="${DEMO_TMP_DIR}/demo.sock"
SERVER_PID=""
mkdir -p "${DEMO_DATA_DIR}"

cleanup() {
    local exit_code=$?
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${DEMO_TMP_DIR}"
    if [[ "${exit_code}" -ne 0 ]]; then
        echo -e "\n${YELLOW}[DEMO ERROR] Demonstration encountered a failure.${RESET}"
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
    echo "Timeout waiting for server to become ready (log: ${log_file})"
    cat "${log_file}" || true
    return 1
}

# Run client command and return response payload
exec_cmd() {
    local cmd_str="$*"
    echo -e "  ${CYAN}$ pisf-client ${cmd_str}${RESET}"
    local raw
    raw=$("${CLIENT_BIN}" -s "${DEMO_SOCK}" "$@" 2>&1)
    local resp
    resp=$(echo "${raw}" | sed -e 's/^\[Response\] //')
    echo -e "  -> ${GREEN}${resp}${RESET}"
    LAST_RESP="${resp}"
}

assert_eq() {
    local expected="$1"
    local actual="$2"
    local msg="$3"
    if [[ "${actual}" != "${expected}" ]]; then
        echo "Assertion failed: ${msg} (expected '${expected}', got '${actual}')"
        exit 1
    fi
}

assert_contains() {
    local needle="$1"
    local haystack="$2"
    local msg="$3"
    if ! echo "${haystack}" | grep -q "${needle}"; then
        echo "Assertion failed: ${msg} (pattern '${needle}' not found in '${haystack}')"
        exit 1
    fi
}

echo -e "${BOLD}======================================================================${RESET}"
echo -e "${BOLD}         PISF — Persistent In-Memory Filesystem Demonstration         ${RESET}"
echo -e "${BOLD}======================================================================${RESET}"
echo -e "Demo root directory: ${DEMO_TMP_DIR}"
echo -e "Storage directory:   ${DEMO_DATA_DIR}"
echo -e "Socket path:         ${DEMO_SOCK}\n"

# 2. Start the server
echo -e "${BOLD}[Step 1/14] Starting PISF Server daemon...${RESET}"
"${SERVER_BIN}" -s "${DEMO_SOCK}" -d "${DEMO_DATA_DIR}" > "${DEMO_TMP_DIR}/server1.log" 2>&1 &
SERVER_PID=$!

# 3. Wait until server is ready
wait_for_server "${DEMO_TMP_DIR}/server1.log"
echo -e "Server started successfully (PID: ${SERVER_PID}).\n"

# 4. Connect using actual PISF client
echo -e "${BOLD}[Step 2/14] Testing connection liveness via PING...${RESET}"
exec_cmd PING
assert_eq "PONG" "${LAST_RESP}" "PING liveness"
echo ""

# 5. Create nested directories
echo -e "${BOLD}[Step 3/14] Creating directory hierarchy (MKDIR)...${RESET}"
exec_cmd MKDIR /workspace
assert_eq "OK" "${LAST_RESP}" "MKDIR /workspace"
exec_cmd MKDIR /workspace/src
assert_eq "OK" "${LAST_RESP}" "MKDIR /workspace/src"
exec_cmd MKDIR /workspace/docs
assert_eq "OK" "${LAST_RESP}" "MKDIR /workspace/docs"
exec_cmd MKDIR /trash
assert_eq "OK" "${LAST_RESP}" "MKDIR /trash"
echo ""

# 6. Create files
echo -e "${BOLD}[Step 4/14] Creating files (CREATE)...${RESET}"
exec_cmd CREATE /workspace/src/main.cpp
assert_eq "OK" "${LAST_RESP}" "CREATE main.cpp"
exec_cmd CREATE /workspace/docs/architecture.md
assert_eq "OK" "${LAST_RESP}" "CREATE architecture.md"
exec_cmd CREATE /trash/temp.log
assert_eq "OK" "${LAST_RESP}" "CREATE temp.log"
echo ""

# 7. Write and read contents
echo -e "${BOLD}[Step 5/14] Writing and reading file contents (WRITE, READ)...${RESET}"
CODE_PAYLOAD="int main() { return 0; }"
exec_cmd WRITE /workspace/src/main.cpp "${CODE_PAYLOAD}"
assert_eq "OK" "${LAST_RESP}" "WRITE main.cpp"

DOC_PAYLOAD="# PISF Architecture: Persistent append-only blob storage"
exec_cmd WRITE /workspace/docs/architecture.md "${DOC_PAYLOAD}"
assert_eq "OK" "${LAST_RESP}" "WRITE architecture.md"

LOG_PAYLOAD="Temporary session log to be discarded"
exec_cmd WRITE /trash/temp.log "${LOG_PAYLOAD}"
assert_eq "OK" "${LAST_RESP}" "WRITE temp.log"

exec_cmd READ /workspace/src/main.cpp
assert_eq "${CODE_PAYLOAD}" "${LAST_RESP}" "READ main.cpp"

exec_cmd READ /workspace/docs/architecture.md
assert_eq "${DOC_PAYLOAD}" "${LAST_RESP}" "READ architecture.md"
echo ""

# 8. Rename a file
echo -e "${BOLD}[Step 6/14] Renaming file (MV /workspace/docs/architecture.md -> README.md)...${RESET}"
exec_cmd MV /workspace/docs/architecture.md /workspace/docs/README.md
assert_eq "OK" "${LAST_RESP}" "MV file"

exec_cmd READ /workspace/docs/README.md
assert_eq "${DOC_PAYLOAD}" "${LAST_RESP}" "READ renamed file"

exec_cmd READ /workspace/docs/architecture.md
assert_contains "ERR not found" "${LAST_RESP}" "Old path should return ERR not found"
echo ""

# 9. Move a directory
echo -e "${BOLD}[Step 7/14] Moving directory subtree (MV /workspace/docs -> /archive/docs)...${RESET}"
exec_cmd MKDIR /archive
assert_eq "OK" "${LAST_RESP}" "MKDIR /archive"

exec_cmd MV /workspace/docs /archive/docs
assert_eq "OK" "${LAST_RESP}" "MV directory"

exec_cmd LS /archive
assert_contains "docs" "${LAST_RESP}" "LS /archive contains docs"

exec_cmd READ /archive/docs/README.md
assert_eq "${DOC_PAYLOAD}" "${LAST_RESP}" "READ moved directory file"

exec_cmd LS /workspace
assert_eq "src" "${LAST_RESP}" "/workspace should only contain src"
echo ""

# 10. Delete a file
echo -e "${BOLD}[Step 8/14] Deleting regular file (RM /trash/temp.log)...${RESET}"
exec_cmd RM /trash/temp.log
assert_eq "OK" "${LAST_RESP}" "RM file"

exec_cmd READ /trash/temp.log
assert_contains "ERR not found" "${LAST_RESP}" "Deleted file should not be found"
echo ""

# 11. Remove an empty directory
echo -e "${BOLD}[Step 9/14] Removing empty directory (RMDIR /trash)...${RESET}"
exec_cmd RMDIR /trash
assert_eq "OK" "${LAST_RESP}" "RMDIR empty directory"

exec_cmd LS /
assert_contains "archive" "${LAST_RESP}" "LS root contains archive"
assert_contains "workspace" "${LAST_RESP}" "LS root contains workspace"
if echo "${LAST_RESP}" | grep -q "trash"; then
    echo "Assertion failed: /trash should not exist in root listing"
    exit 1
fi
echo ""

# 12. Stop the server
echo -e "${BOLD}[Step 10/14] Shutting down server gracefully (SHUTDOWN)...${RESET}"
exec_cmd SHUTDOWN
assert_eq "OK SHUTDOWN" "${LAST_RESP}" "SHUTDOWN server"
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
echo "Server terminated cleanly. Data persisted on disk."
echo ""

# 13. Restart server with same data directory
echo -e "${BOLD}[Step 11/14] Restarting PISF Server using identical data directory...${RESET}"
"${SERVER_BIN}" -s "${DEMO_SOCK}" -d "${DEMO_DATA_DIR}" > "${DEMO_TMP_DIR}/server2.log" 2>&1 &
SERVER_PID=$!
wait_for_server "${DEMO_TMP_DIR}/server2.log"
echo "Server restarted and recovered persistent state."
echo ""

# 14. Verify surviving files and directories are recovered
echo -e "${BOLD}[Step 12/14] Verifying recovered namespace and file contents across restart...${RESET}"
exec_cmd LS /
assert_contains "archive" "${LAST_RESP}" "Recovered root has archive"
assert_contains "workspace" "${LAST_RESP}" "Recovered root has workspace"

exec_cmd LS /workspace
assert_eq "src" "${LAST_RESP}" "Recovered /workspace has src"

exec_cmd LS /archive
assert_eq "docs" "${LAST_RESP}" "Recovered /archive has docs"

exec_cmd READ /workspace/src/main.cpp
assert_eq "${CODE_PAYLOAD}" "${LAST_RESP}" "Recovered main.cpp content intact"

exec_cmd READ /archive/docs/README.md
assert_eq "${DOC_PAYLOAD}" "${LAST_RESP}" "Recovered README.md content intact"

exec_cmd STAT /archive/docs/README.md
assert_contains "type=file" "${LAST_RESP}" "STAT README.md confirms type=file"
echo ""

# 15. Display storage statistics
echo -e "${BOLD}[Step 13/14] Querying persistent storage statistics (STATS)...${RESET}"
exec_cmd STATS
assert_contains "total_blob_bytes=" "${LAST_RESP}" "STATS total_blob_bytes"
assert_contains "live_bytes=" "${LAST_RESP}" "STATS live_bytes"
assert_contains "active_generation=" "${LAST_RESP}" "STATS active_generation"
echo ""

# 16. Clean shutdown & cleanup
echo -e "${BOLD}[Step 14/14] Final shutdown and environment cleanup...${RESET}"
exec_cmd SHUTDOWN
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
rm -rf "${DEMO_TMP_DIR}"

echo -e "\n${BOLD}${GREEN}======================================================================${RESET}"
echo -e "${BOLD}${GREEN}   PISF DEMONSTRATION COMPLETE: ALL 14 STEPS VERIFIED SUCCESSFULLY!   ${RESET}"
echo -e "${BOLD}${GREEN}======================================================================${RESET}"
exit 0
