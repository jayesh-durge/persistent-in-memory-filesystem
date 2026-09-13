#include <pisf/common.hpp>

#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options] [command]\n"
              << "Options:\n"
              << "  -s, --socket <path>   Set Unix domain socket path\n"
              << "  -h, --help            Show this help message\n\n"
              << "If no command is specified, defaults to sending 'PING'.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path = pisf::get_default_socket_path();
    std::string command_to_send = "PING";
    bool command_specified = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg.rfind("-", 0) == 0) {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        } else {
            if (!command_specified) {
                command_to_send = arg;
                command_specified = true;
            } else {
                command_to_send += " " + arg;
            }
        }
    }

    std::cout << "=== PISF Client ===\n"
              << "Connecting to server at: " << socket_path << "\n";

    // Create Unix domain stream socket
    int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        std::cerr << "[Error] socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    struct sockaddr_un addr{};
    std::string err;
    if (!pisf::init_sockaddr(socket_path, addr, err)) {
        std::cerr << "[Error] " << err << "\n";
        ::close(client_fd);
        return 1;
    }

    // Connect to server socket
    if (::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Error] Failed to connect to server at '" << socket_path
                  << "': " << std::strerror(errno) << " (is the server running?)\n";
        ::close(client_fd);
        return 1;
    }

    std::cout << "[Info] Connected to server successfully\n";
    std::cout << "[Info] Sending command: " << command_to_send << "\n";

    // Send command with newline framing
    if (!pisf::send_line(client_fd, command_to_send, err)) {
        std::cerr << "[Error] Failed to send command to server: " << err << "\n";
        ::close(client_fd);
        return 1;
    }

    // Read newline-framed response from server
    std::string response;
    std::string buffer;
    pisf::ReadStatus read_stat = pisf::read_line(client_fd, response, buffer, err);

    if (read_stat == pisf::ReadStatus::Success) {
        std::cout << "[Response] " << response << "\n";
    } else if (read_stat == pisf::ReadStatus::PeerClosed) {
        std::cerr << "[Error] Server closed connection without sending a response\n";
        ::close(client_fd);
        return 1;
    } else {
        std::cerr << "[Error] Failed to receive response: " << err << "\n";
        ::close(client_fd);
        return 1;
    }

    ::close(client_fd);
    std::cout << "[Info] Connection closed cleanly\n";
    return 0;
}
