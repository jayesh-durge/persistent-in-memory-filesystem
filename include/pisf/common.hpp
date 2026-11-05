#ifndef PISF_COMMON_HPP
#define PISF_COMMON_HPP

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace pisf {

// Determines the default Unix domain socket path.
// Prefers $XDG_RUNTIME_DIR/pisf.sock, falling back to /tmp/pisf-<uid>.sock.
inline std::string get_default_socket_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/pisf.sock";
    }
    return "/tmp/pisf-" + std::to_string(getuid()) + ".sock";
}

// Determines the default persistent storage directory.
// Prefers $PISF_DATA_DIR, then $XDG_DATA_HOME/pisf, falling back to /tmp/pisf-data-<uid>.
inline std::string get_default_storage_dir() {
    const char* env_dir = std::getenv("PISF_DATA_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        return std::string(env_dir);
    }
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data != nullptr && xdg_data[0] != '\0') {
        return std::string(xdg_data) + "/pisf";
    }
    return "/tmp/pisf-data-" + std::to_string(getuid());
}

enum class SocketStatus {
    DoesNotExist,
    Stale,
    ActiveServerPresent,
    Error
};

// Inspects whether the given socket path already exists and if an active server is listening.
inline SocketStatus inspect_socket_path(const std::string& path, std::string& err_msg) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return SocketStatus::DoesNotExist;
        }
        err_msg = "stat() failed for '" + path + "': " + std::strerror(errno);
        return SocketStatus::Error;
    }

    if (!S_ISSOCK(st.st_mode)) {
        err_msg = "Target path '" + path + "' exists but is not a Unix domain socket";
        return SocketStatus::Error;
    }

    // Attempt to connect to check if a live server is listening
    int test_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (test_fd < 0) {
        err_msg = "socket() probe failed: " + std::string(std::strerror(errno));
        return SocketStatus::Error;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(test_fd);
        err_msg = "Socket path exceeds sun_path buffer limit";
        return SocketStatus::Error;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int res = ::connect(test_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    int connect_err = errno;
    ::close(test_fd);

    if (res == 0) {
        err_msg = "Another server is currently running and listening on '" + path + "'";
        return SocketStatus::ActiveServerPresent;
    }

    if (connect_err == ECONNREFUSED || connect_err == ENOENT) {
        // Socket file exists but connection was refused -> stale socket
        return SocketStatus::Stale;
    }

    err_msg = "Probe connection to '" + path + "' failed: " + std::strerror(connect_err);
    return SocketStatus::Error;
}

// Prepares a sockaddr_un structure from a path string, checking length limits.
inline bool init_sockaddr(const std::string& path, struct sockaddr_un& addr, std::string& err_msg) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        err_msg = "Socket path length (" + std::to_string(path.size()) +
                  ") exceeds max buffer size (" + std::to_string(sizeof(addr.sun_path) - 1) + ")";
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    return true;
}

// Sends a message over a stream socket with an appended newline delimiter.
// Loops until all bytes are sent, handling partial writes and EINTR.
inline bool send_line(int fd, const std::string& message, std::string& err_msg) {
    std::string payload = message;
    if (payload.empty() || payload.back() != '\n') {
        payload += '\n';
    }

    const char* ptr = payload.data();
    size_t remaining = payload.size();

    while (remaining > 0) {
        ssize_t sent = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent > 0) {
            ptr += sent;
            remaining -= static_cast<size_t>(sent);
        } else if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_msg = "send() failed: " + std::string(std::strerror(errno));
            return false;
        } else {
            err_msg = "send() returned 0 (peer closed connection)";
            return false;
        }
    }
    return true;
}

// Maximum permitted command-line length in bytes (excluding newline delimiter)
constexpr size_t MAX_COMMAND_LINE_LENGTH = 64 * 1024; // 64 KiB

enum class ReadStatus {
    Success,
    PeerClosed,
    Interrupted,
    Error
};

// Reads a single newline-delimited line from the socket using an internal buffer.
// Handles partial recv() chunks, multiple lines in buffer, EINTR, and enforces
// max_line_len limit (defaulting to MAX_COMMAND_LINE_LENGTH).
inline ReadStatus read_line(int fd, std::string& line_out, std::string& buffer, std::string& err_msg,
                            size_t max_line_len = MAX_COMMAND_LINE_LENGTH) {
    while (true) {
        // Check if newline already present in buffer
        size_t newline_pos = buffer.find('\n');
        if (newline_pos != std::string::npos) {
            size_t effective_len = newline_pos;
            if (effective_len > 0 && buffer[effective_len - 1] == '\r') {
                --effective_len;
            }
            if (effective_len > max_line_len) {
                err_msg = "command line exceeds maximum allowed limit of " +
                          std::to_string(max_line_len) + " bytes";
                return ReadStatus::Error;
            }

            line_out = buffer.substr(0, newline_pos);
            // Strip trailing carriage return if present (\r\n)
            if (!line_out.empty() && line_out.back() == '\r') {
                line_out.pop_back();
            }
            buffer.erase(0, newline_pos + 1);
            return ReadStatus::Success;
        }

        // Reject input that exceeds limit even before a newline has arrived
        if (buffer.size() > max_line_len) {
            err_msg = "command line exceeds maximum allowed limit of " +
                      std::to_string(max_line_len) + " bytes (no newline encountered)";
            return ReadStatus::Error;
        }

        // Receive more bytes from socket
        char chunk[256];
        ssize_t bytes_read = ::recv(fd, chunk, sizeof(chunk), 0);

        if (bytes_read > 0) {
            buffer.append(chunk, static_cast<size_t>(bytes_read));
            // Loop back to check for newline or size limit
        } else if (bytes_read == 0) {
            // Peer orderly shutdown
            if (!buffer.empty()) {
                size_t effective_len = buffer.size();
                if (!buffer.empty() && buffer.back() == '\r') {
                    --effective_len;
                }
                if (effective_len > max_line_len) {
                    err_msg = "command line exceeds maximum allowed limit of " +
                              std::to_string(max_line_len) + " bytes";
                    return ReadStatus::Error;
                }

                line_out = buffer;
                if (!line_out.empty() && line_out.back() == '\r') {
                    line_out.pop_back();
                }
                buffer.clear();
                return ReadStatus::Success;
            }
            return ReadStatus::PeerClosed;
        } else {
            if (errno == EINTR) {
                return ReadStatus::Interrupted;
            }
            err_msg = "recv() failed: " + std::string(std::strerror(errno));
            return ReadStatus::Error;
        }
    }
}

} // namespace pisf

#endif // PISF_COMMON_HPP
