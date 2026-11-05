#include <pisf/common.hpp>
#include <pisf/filesystem.hpp>

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace {

// In-memory hierarchical filesystem instance
pisf::Filesystem g_fs;

// Atomic flag indicating whether server shutdown has been initiated
std::atomic<bool> g_shutdown_requested{false};

// POSIX-safe signal flag
volatile std::sig_atomic_t g_signal_received = 0;

// Self-pipe to wake the main thread out of poll() upon signal or SHUTDOWN command
int g_wake_pipe[2] = {-1, -1};

// Mutex to protect standard output so log messages from concurrent workers do not interleave
std::mutex g_log_mutex;

// Mutex protecting active client file descriptors
std::mutex g_clients_mutex;
std::unordered_set<int> g_active_client_fds;

void log_msg(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << msg << "\n";
}

void trigger_shutdown() {
    bool expected = false;
    if (g_shutdown_requested.compare_exchange_strong(expected, true)) {
        if (g_wake_pipe[1] >= 0) {
            char b = 'q';
            ssize_t written = ::write(g_wake_pipe[1], &b, 1);
            static_cast<void>(written);
        }
    }
}

void signal_handler(int sig) {
    g_signal_received = sig;
    g_shutdown_requested.store(true, std::memory_order_relaxed);
    if (g_wake_pipe[1] >= 0) {
        char b = 'q';
        // write() is async-signal-safe per POSIX.1-2008
        ssize_t written = ::write(g_wake_pipe[1], &b, 1);
        static_cast<void>(written);
    }
}

void setup_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Do not use SA_RESTART so blocking calls return EINTR on signal

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Prevent termination on writing to broken pipe
    std::signal(SIGPIPE, SIG_IGN);
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -s, --socket <path>     Set Unix domain socket path\n"
              << "  -d, --data-dir <path>   Set persistent storage directory\n"
              << "  -h, --help              Show this help message\n";
}

struct ClientSession {
    std::string cwd{"/"};
};

bool resolve_session_path(const std::string& cwd, const std::string& path, std::string& out_path,
                          std::string& err) {
    if (path.empty()) {
        err = "ERR missing path argument";
        return false;
    }
    std::vector<std::string> components;
    auto append_components = [&](const std::string& value) {
        size_t pos = 0;
        while (pos < value.size()) {
            while (pos < value.size() && value[pos] == '/') ++pos;
            size_t start = pos;
            while (pos < value.size() && value[pos] != '/') ++pos;
            if (start == pos) continue;
            std::string part = value.substr(start, pos - start);
            if (part == ".") continue;
            if (part == "..") {
                if (!components.empty()) components.pop_back();
                continue;
            }
            for (unsigned char c : part) {
                if (c <= 32 || c == 127) return false;
            }
            components.push_back(std::move(part));
        }
        return true;
    };

    if (path.front() != '/') {
        if (!append_components(cwd)) {
            err = "ERR invalid current directory";
            return false;
        }
    }
    if (!append_components(path)) {
        err = "ERR invalid path component: whitespace and control characters not allowed";
        return false;
    }

    out_path = "/";
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) out_path += '/';
        out_path += components[i];
    }
    return true;
}

// Dispatches one framed request. cwd belongs to this client connection only.
bool handle_client_command(const std::string& request, int client_fd, ClientSession& session,
                          std::string& err) {
    if (request.empty()) {
        if (!pisf::send_frame(client_fd, "ERR empty command", err)) {
            return false;
        }
        return true;
    }

    // Parse command verb and argument (delimited by whitespace)
    size_t start = request.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        if (!pisf::send_frame(client_fd, "ERR empty command", err)) {
            return false;
        }
        return true;
    }

    size_t space_pos = request.find_first_of(" \t", start);
    std::string verb;
    std::string arg;
    if (space_pos == std::string::npos) {
        size_t end = request.find_last_not_of(" \t\r\n");
        verb = request.substr(start, end - start + 1);
    } else {
        verb = request.substr(start, space_pos - start);
        size_t arg_start = request.find_first_not_of(" \t", space_pos);
        if (arg_start != std::string::npos) {
            size_t arg_end = request.find_last_not_of(" \t\r\n");
            arg = request.substr(arg_start, arg_end - arg_start + 1);
        }
    }

    for (char& c : verb) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (verb == "PING") {
        if (!arg.empty()) {
            std::string err_resp = "ERR unknown command '" + request + "'";
            log_msg("[Info] " + err_resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, err_resp, err);
        }
        if (!pisf::send_frame(client_fd, "PONG", err)) {
            return false;
        }
        log_msg("[Info] Handled PING -> PONG (fd: " + std::to_string(client_fd) + ")");
        return true;
    }

    if (verb == "QUIT") {
        pisf::send_frame(client_fd, "BYE", err);
        log_msg("[Info] Handled QUIT -> BYE (closing client connection, fd: " + std::to_string(client_fd) + ")");
        return false;
    }

    if (verb == "SHUTDOWN") {
        pisf::send_frame(client_fd, "OK SHUTDOWN", err);
        log_msg("[Info] Handled SHUTDOWN -> initiating graceful server shutdown (fd: " + std::to_string(client_fd) + ")");
        trigger_shutdown();
        return false;
    }

    if (verb == "PWD") {
        if (!arg.empty()) return pisf::send_frame(client_fd, "ERR PWD takes no arguments", err);
        if (!g_fs.exists(session.cwd)) {
            session.cwd = "/";
            return pisf::send_frame(client_fd, "ERR current directory was removed; working directory reset to /", err);
        }
        return pisf::send_frame(client_fd, session.cwd, err);
    }

    if (verb == "CD") {
        std::string requested = arg.empty() ? "/" : arg;
        if (!arg.empty() && arg.front() != '/' && !g_fs.exists(session.cwd)) {
            session.cwd = "/";
            return pisf::send_frame(client_fd, "ERR current directory was removed; use cd / to continue", err);
        }
        std::string target;
        if (!resolve_session_path(session.cwd, requested, target, err)) {
            return pisf::send_frame(client_fd, err, err);
        }
        std::string details = g_fs.stat(target);
        if (details.rfind("ERR", 0) == 0) return pisf::send_frame(client_fd, details, err);
        if (details.find("type=directory") == std::string::npos) {
            return pisf::send_frame(client_fd, "ERR not a directory", err);
        }
        session.cwd = target;
        return pisf::send_frame(client_fd, "OK", err);
    }

    if (verb == "HELP") {
        return pisf::send_frame(client_fd,
            "OK commands: pwd ls ls -l cd mkdir rm rm -rf touch PING QUIT WRITE READ STAT MV STATS COMPACT", err);
    }

    if (!g_fs.exists(session.cwd)) {
        session.cwd = "/";
        return pisf::send_frame(client_fd,
            "ERR current directory was removed; working directory reset to /", err);
    }

    if (verb == "MKDIR") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled MKDIR -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.mkdir(path);
        log_msg("[Info] Handled MKDIR '" + path + "' -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "TOUCH") {
        if (arg.empty()) return pisf::send_frame(client_fd, "ERR missing path argument", err);
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        return pisf::send_frame(client_fd, g_fs.touch(path), err);
    }

    if (verb == "CREATE") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled CREATE -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.create(path);
        log_msg("[Info] Handled CREATE '" + arg + "' -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "LS") {
        bool long_format = false;
        std::string list_path = arg;
        if (list_path == "-l") {
            long_format = true;
            list_path.clear();
        } else if (list_path.rfind("-l ", 0) == 0) {
            long_format = true;
            list_path.erase(0, 3);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, list_path.empty() ? "." : list_path, path, err))
            return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.ls(path, long_format);
        log_msg("[Info] Handled LS '" + arg + "' -> '" + resp + "' (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "STAT") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled STAT -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.stat(path);
        log_msg("[Info] Handled STAT '" + arg + "' -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "WRITE") {
        if (space_pos == std::string::npos) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled WRITE -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }

        size_t path_start = request.find_first_not_of(" \t", space_pos);
        if (path_start == std::string::npos) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled WRITE -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }

        size_t path_end = request.find_first_of(" \t", path_start);
        std::string path;
        std::string content;
        if (path_end == std::string::npos) {
            path = request.substr(path_start);
            content = "";
        } else {
            path = request.substr(path_start, path_end - path_start);
            content = request.substr(path_end + 1);
        }

        std::string absolute_path;
        if (!resolve_session_path(session.cwd, path, absolute_path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.write(absolute_path, content);
        log_msg("[Info] Handled WRITE '" + path + "' (" + std::to_string(content.size()) + " bytes) -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "RM") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled RM -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        bool recursive = false;
        std::string target_arg = arg;
        if (target_arg.rfind("-rf ", 0) == 0 || target_arg.rfind("-fr ", 0) == 0) {
            recursive = true;
            target_arg.erase(0, 4);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, target_arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string details = g_fs.stat(path);
        std::string resp;
        if (details.rfind("ERR", 0) == 0) {
            resp = details;
        } else if (recursive) {
            resp = g_fs.rm_recursive(path);
        } else if (details.find("type=directory") != std::string::npos) {
            resp = g_fs.rmdir(path);
        } else {
            resp = g_fs.rm(path);
        }
        log_msg("[Info] Handled RM '" + target_arg + "' -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "RMDIR") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled RMDIR -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.rmdir(path);
        log_msg("[Info] Handled RMDIR '" + arg + "' -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "MV") {
        // Syntax: MV <src_path> <dst_path>
        // src_path and dst_path are separated by whitespace after the verb
        if (space_pos == std::string::npos) {
            std::string resp = "ERR missing arguments: MV requires source and destination paths";
            log_msg("[Info] Handled MV -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }

        // Parse two space-separated path arguments
        size_t src_start = request.find_first_not_of(" \t", space_pos);
        if (src_start == std::string::npos) {
            std::string resp = "ERR missing arguments: MV requires source and destination paths";
            log_msg("[Info] Handled MV -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        size_t src_end = request.find_first_of(" \t", src_start);
        if (src_end == std::string::npos) {
            std::string resp = "ERR missing destination path";
            log_msg("[Info] Handled MV -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string src_arg = request.substr(src_start, src_end - src_start);

        size_t dst_start = request.find_first_not_of(" \t", src_end);
        if (dst_start == std::string::npos) {
            std::string resp = "ERR missing destination path";
            log_msg("[Info] Handled MV -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        size_t dst_end = request.find_last_not_of(" \t\r\n");
        std::string dst_arg = request.substr(dst_start, dst_end - dst_start + 1);

        std::string src_path;
        std::string dst_path;
        if (!resolve_session_path(session.cwd, src_arg, src_path, err) ||
            !resolve_session_path(session.cwd, dst_arg, dst_path, err))
            return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.mv(src_path, dst_path);
        log_msg("[Info] Handled MV '" + src_arg + "' -> '" + dst_arg + "' result=" + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "READ") {
        if (arg.empty()) {
            std::string resp = "ERR missing path argument";
            log_msg("[Info] Handled READ -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
        std::string path;
        if (!resolve_session_path(session.cwd, arg, path, err)) return pisf::send_frame(client_fd, err, err);
        std::string resp = g_fs.read(path);
        log_msg("[Info] Handled READ '" + arg + "' -> '" + resp + "' (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "STATS") {
        auto stats = g_fs.get_storage_stats();
        std::string resp = "OK total_blob_bytes=" + std::to_string(stats.total_blob_bytes) +
                           " live_bytes=" + std::to_string(stats.live_bytes) +
                           " reclaimable_bytes=" + std::to_string(stats.reclaimable_bytes) +
                           " active_generation=" + std::to_string(stats.active_generation) +
                           " live_files=" + std::to_string(stats.live_files);
        log_msg("[Info] Handled STATS -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    if (verb == "COMPACT") {
        std::string compact_err;
        if (g_fs.compact(compact_err)) {
            auto stats = g_fs.get_storage_stats();
            std::string resp = "OK COMPACT generation=" + std::to_string(stats.active_generation) +
                               " live_bytes=" + std::to_string(stats.live_bytes);
            log_msg("[Info] Handled COMPACT -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        } else {
            std::string resp = "ERR compact failed: " + compact_err;
            log_msg("[Info] Handled COMPACT -> " + resp + " (fd: " + std::to_string(client_fd) + ")");
            return pisf::send_frame(client_fd, resp, err);
        }
    }

    if (verb == "HELP") {
        std::string resp = "OK commands: PING QUIT SHUTDOWN MKDIR CREATE LS STAT WRITE READ RM RMDIR MV STATS COMPACT";
        log_msg("[Info] Handled HELP (fd: " + std::to_string(client_fd) + ")");
        return pisf::send_frame(client_fd, resp, err);
    }

    // Unknown command error handling
    std::string err_resp = "ERR unknown command '" + request + "'";
    log_msg("[Info] " + err_resp + " (fd: " + std::to_string(client_fd) + ")");
    if (!pisf::send_frame(client_fd, err_resp, err)) {
        return false;
    }
    return true;
}

// Worker thread tracking structure
struct WorkerRecord {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

// Client worker thread function.
// Ownership of client_fd belongs exclusively to this worker, which closes it upon exit.
void client_worker_func(int client_fd, std::shared_ptr<std::atomic<bool>> done) {
    // 1. Register client_fd in active set so shutdown can unblock it
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_active_client_fds.insert(client_fd);
    }
    log_msg("[Info] Client connected (fd: " + std::to_string(client_fd) + ")");

    ClientSession session;
    std::string err;
    bool keep_client_alive = true;

    // Command loop: runs until client requests QUIT/SHUTDOWN, disconnects, or server shuts down
    while (keep_client_alive && !g_shutdown_requested.load(std::memory_order_relaxed)) {
        std::string request_line;
        pisf::ReadStatus read_stat = pisf::read_frame(client_fd, request_line, err);

        if (read_stat == pisf::ReadStatus::Success) {
            log_msg("[Info] Received: '" + request_line + "' (fd: " + std::to_string(client_fd) + ")");
            keep_client_alive = handle_client_command(request_line, client_fd, session, err);
        } else if (read_stat == pisf::ReadStatus::PeerClosed) {
            log_msg("[Info] Client disconnected (EOF, fd: " + std::to_string(client_fd) + ")");
            break;
        } else if (read_stat == pisf::ReadStatus::Interrupted) {
            if (g_shutdown_requested.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        } else {
            // Read error or socket was shut down by server during shutdown sequence
            if (!g_shutdown_requested.load(std::memory_order_relaxed)) {
                log_msg("[Warning] Client read error (fd: " + std::to_string(client_fd) + "): " + err);
                std::string send_err;
                pisf::send_frame(client_fd, "ERR " + err, send_err);
            }
            break;
        }
    }

    // 2. Unregister client_fd from active set
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_active_client_fds.erase(client_fd);
    }

    // 3. Worker owns client_fd and closes it exactly once
    ::close(client_fd);
    log_msg("[Info] Closed client connection (fd: " + std::to_string(client_fd) + ")");

    // 4. Mark as completed so main thread can join
    done->store(true, std::memory_order_release);
}

// Prune any finished worker threads from tracking list
void prune_finished_workers(std::vector<WorkerRecord>& workers) {
    auto it = workers.begin();
    while (it != workers.end()) {
        if (it->done->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = workers.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::string socket_path = pisf::get_default_socket_path();
    std::string data_dir = pisf::get_default_storage_dir();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((arg == "-d" || arg == "--data-dir") && i + 1 < argc) {
            data_dir = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::string storage_err;
    if (!g_fs.init_storage(data_dir, storage_err)) {
        std::cerr << "[Error] Failed to initialize storage directory '" << data_dir << "': " << storage_err << "\n";
        return 1;
    }

    setup_signal_handlers();

    // Create self-pipe for clean wakeup of poll()
    if (::pipe2(g_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        std::cerr << "[Error] Failed to create wake pipe: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::cout << "=== PISF Server (Persistent Filesystem Daemon) ===\n"
              << "Socket path:  " << socket_path << "\n"
              << "Storage path: " << g_fs.data_dir() << "\n"
              << "[Info] Storage initialized: active generation " << g_fs.blob_generation()
              << ", " << g_fs.total_nodes() << " nodes loaded\n";

    // Check for existing socket file and handle stale socket safely
    std::string err;
    pisf::SocketStatus status = pisf::inspect_socket_path(socket_path, err);

    if (status == pisf::SocketStatus::ActiveServerPresent) {
        std::cerr << "[Error] Cannot start server: " << err << "\n";
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    } else if (status == pisf::SocketStatus::Stale) {
        std::cout << "[Notice] Removing stale socket file: " << socket_path << "\n";
        if (::unlink(socket_path.c_str()) != 0) {
            std::cerr << "[Error] Failed to remove stale socket: " << std::strerror(errno) << "\n";
            ::close(g_wake_pipe[0]);
            ::close(g_wake_pipe[1]);
            return 1;
        }
    } else if (status == pisf::SocketStatus::Error) {
        std::cerr << "[Error] Socket path check failed: " << err << "\n";
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    }

    // Create Unix domain stream socket
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[Error] socket() system call failed: " << std::strerror(errno) << "\n";
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    }

    struct sockaddr_un addr{};
    if (!pisf::init_sockaddr(socket_path, addr, err)) {
        std::cerr << "[Error] " << err << "\n";
        ::close(listen_fd);
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    }

    // Bind socket to filesystem path
    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Error] bind() failed on '" << socket_path << "': " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    }

    // Listen for client connections
    constexpr int backlog = 16;
    if (::listen(listen_fd, backlog) != 0) {
        std::cerr << "[Error] listen() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        ::unlink(socket_path.c_str());
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        return 1;
    }

    std::cout << "[Info] Server listening on Unix domain socket: " << socket_path << "\n"
              << "[Info] Waiting for client connections (press Ctrl+C to shut down)...\n";

    // Setup poll descriptors: listen_fd for new clients, g_wake_pipe[0] for shutdown signals
    struct pollfd pfds[2]{};
    pfds[0].fd = listen_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = g_wake_pipe[0];
    pfds[1].events = POLLIN;

    std::vector<WorkerRecord> workers;

    // Concurrent accept loop
    while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
        prune_finished_workers(workers);

        // Use a bounded timeout so prune_finished_workers() runs periodically even
        // when no new connections arrive.  1 second is short enough to keep the
        // worker list tidy without wasting CPU.
        int poll_res = ::poll(pfds, 2, 1000);
        if (poll_res < 0) {
            if (errno == EINTR) {
                // Signal received, re-evaluate loop condition
                continue;
            }
            log_msg("[Error] poll() failed: " + std::string(std::strerror(errno)));
            break;
        }

        // poll_res == 0 means timeout: loop back to prune_finished_workers().

        // Check if wake pipe received a shutdown signal
        if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            break;
        }

        // Check for incoming client connection
        if (pfds[0].revents & POLLIN) {
            int client_fd = ::accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                log_msg("[Warning] accept() failed: " + std::string(std::strerror(errno)));
                continue;
            }

            if (g_shutdown_requested.load(std::memory_order_relaxed)) {
                ::close(client_fd);
                break;
            }

            auto done = std::make_shared<std::atomic<bool>>(false);
            try {
                workers.emplace_back(WorkerRecord{
                    std::thread(client_worker_func, client_fd, done),
                    done
                });
            } catch (const std::system_error& ex) {
                log_msg("[Error] Failed to spawn worker thread: " + std::string(ex.what()));
                // On thread spawn failure, main thread closes client_fd to prevent descriptor leak
                ::close(client_fd);
            }
        }
    }

    if (g_signal_received == SIGINT) {
        log_msg("\n[Info] Received SIGINT (Ctrl+C) -> shutting down server gracefully...");
    } else if (g_signal_received == SIGTERM) {
        log_msg("\n[Info] Received SIGTERM -> shutting down server gracefully...");
    } else {
        log_msg("\n[Info] Shutting down server gracefully...");
    }

    // Stop accepting new connections
    ::close(listen_fd);

    // Wake up any idle client workers by shutting down active client sockets
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (int fd : g_active_client_fds) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    // Join all remaining worker threads
    for (auto& worker : workers) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    workers.clear();

    // Clean up wake pipe
    if (g_wake_pipe[0] >= 0) ::close(g_wake_pipe[0]);
    if (g_wake_pipe[1] >= 0) ::close(g_wake_pipe[1]);

    // Unlink socket file
    if (::unlink(socket_path.c_str()) != 0) {
        std::cerr << "[Warning] Failed to unlink socket on shutdown: " << std::strerror(errno) << "\n";
    } else {
        log_msg("[Info] Socket file successfully removed: " + socket_path);
    }

    log_msg("[Info] Server shutdown complete.");

    return 0;
}
