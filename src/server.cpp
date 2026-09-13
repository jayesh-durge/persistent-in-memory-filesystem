#include <pisf/common.hpp>

#include <csignal>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
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
              << "  -s, --socket <path>   Set Unix domain socket path\n"
              << "  -h, --help            Show this help message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path = pisf::get_default_socket_path();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    setup_signal_handlers();

    std::cout << "=== PISF Server (Persistent Filesystem Daemon) ===\n"
              << "Socket path: " << socket_path << "\n";

    // Check for existing socket file and handle stale socket safely
    std::string err;
    pisf::SocketStatus status = pisf::inspect_socket_path(socket_path, err);

    if (status == pisf::SocketStatus::ActiveServerPresent) {
        std::cerr << "[Error] Cannot start server: " << err << "\n";
        return 1;
    } else if (status == pisf::SocketStatus::Stale) {
        std::cout << "[Notice] Removing stale socket file: " << socket_path << "\n";
        if (::unlink(socket_path.c_str()) != 0) {
            std::cerr << "[Error] Failed to remove stale socket: " << std::strerror(errno) << "\n";
            return 1;
        }
    } else if (status == pisf::SocketStatus::Error) {
        std::cerr << "[Error] Socket path check failed: " << err << "\n";
        return 1;
    }

    // Create Unix domain stream socket
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[Error] socket() system call failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    struct sockaddr_un addr{};
    if (!pisf::init_sockaddr(socket_path, addr, err)) {
        std::cerr << "[Error] " << err << "\n";
        ::close(listen_fd);
        return 1;
    }

    // Bind socket to filesystem path
    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Error] bind() failed on '" << socket_path << "': " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }

    // Listen for client connections
    constexpr int backlog = 5;
    if (::listen(listen_fd, backlog) != 0) {
        std::cerr << "[Error] listen() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        ::unlink(socket_path.c_str());
        return 1;
    }

    std::cout << "[Info] Server listening on Unix domain socket: " << socket_path << "\n"
              << "[Info] Waiting for client connections (press Ctrl+C to shut down)...\n";

    // Sequential single-client accept loop
    while (!g_shutdown_requested) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // Signal received, check loop condition
                break;
            }
            std::cerr << "[Warning] accept() failed: " << std::strerror(errno) << "\n";
            continue;
        }

        std::cout << "[Info] Client connected (fd: " << client_fd << ")\n";

        // Read request from client
        std::string request_line;
        std::string client_buffer;
        pisf::ReadStatus read_stat = pisf::read_line(client_fd, request_line, client_buffer, err);

        if (read_stat == pisf::ReadStatus::Success) {
            std::cout << "[Info] Received request: " << request_line << "\n";

            std::string response;
            if (request_line == "PING") {
                response = "PONG";
            } else {
                response = "OK: " + request_line;
            }

            if (!pisf::send_line(client_fd, response, err)) {
                std::cerr << "[Warning] Failed to send response: " << err << "\n";
            } else {
                std::cout << "[Info] Sent response: " << response << "\n";
            }
        } else if (read_stat == pisf::ReadStatus::PeerClosed) {
            std::cout << "[Info] Client closed connection before sending data\n";
        } else if (read_stat == pisf::ReadStatus::Error) {
            std::cerr << "[Warning] Client read error: " << err << "\n";
        }

        ::close(client_fd);
        std::cout << "[Info] Client disconnected (fd: " << client_fd << ")\n";
    }

    std::cout << "\n[Info] Shutting down server gracefully...\n";
    ::close(listen_fd);
    if (::unlink(socket_path.c_str()) != 0) {
        std::cerr << "[Warning] Failed to unlink socket on shutdown: " << std::strerror(errno) << "\n";
    } else {
        std::cout << "[Info] Socket file successfully removed: " << socket_path << "\n";
    }

    return 0;
}
