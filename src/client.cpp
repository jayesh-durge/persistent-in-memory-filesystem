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
              << "If no command is provided, starts an interactive command loop (REPL).\n"
              << "Supported commands:\n"
              << "  PING                  Checks server liveness (expects PONG)\n"
              << "  QUIT                  Ends the current client session\n"
              << "  SHUTDOWN              Requests graceful server termination\n"
              << "  MKDIR <path>          Creates a directory\n"
              << "  CREATE <path>         Creates an empty file\n"
              << "  LS <path>             Lists directory contents\n"
              << "  STAT <path>           Displays node metadata\n";
}

bool execute_single_command(int client_fd, const std::string& cmd, std::string& recv_buffer, std::string& response_out) {
    std::string err;
    if (!pisf::send_line(client_fd, cmd, err)) {
        std::cerr << "[Error] Failed to send command '" << cmd << "': " << err << "\n";
        return false;
    }

    pisf::ReadStatus read_stat = pisf::read_line(client_fd, response_out, recv_buffer, err);
    if (read_stat == pisf::ReadStatus::Success) {
        return true;
    } else if (read_stat == pisf::ReadStatus::PeerClosed) {
        std::cerr << "[Info] Server closed connection\n";
        return false;
    } else {
        std::cerr << "[Error] Failed to receive response: " << err << "\n";
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path = pisf::get_default_socket_path();
    std::string single_command;
    bool has_single_command = false;

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
            if (!has_single_command) {
                single_command = arg;
                has_single_command = true;
            } else {
                single_command += " " + arg;
            }
        }
    }

    // Connect to the Unix domain socket
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

    if (::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Error] Failed to connect to server at '" << socket_path
                  << "': " << std::strerror(errno) << " (is the server running?)\n";
        ::close(client_fd);
        return 1;
    }

    std::string recv_buffer;

    if (has_single_command) {
        // Single command execution mode
        std::string response;
        if (execute_single_command(client_fd, single_command, recv_buffer, response)) {
            std::cout << "[Response] " << response << "\n";
        }
        ::close(client_fd);
        return 0;
    }

    // Interactive / stream command loop
    bool is_interactive = (isatty(STDIN_FILENO) != 0);
    if (is_interactive) {
        std::cout << "Connected to PISF server at " << socket_path << "\n"
                  << "Type commands (e.g., PING, QUIT). Press Ctrl+D to exit.\n";
    }

    std::string line;
    while (true) {
        if (is_interactive) {
            std::cout << "pisf> " << std::flush;
        }

        if (!std::getline(std::cin, line)) {
            // EOF reached
            if (is_interactive) {
                std::cout << "\n[Info] Exiting...\n";
            }
            break;
        }

        if (line.empty()) {
            continue;
        }

        std::string response;
        if (!execute_single_command(client_fd, line, recv_buffer, response)) {
            break;
        }

        std::cout << "[Response] " << response << "\n";

        if (line == "QUIT" || response == "BYE" || response == "OK SHUTDOWN") {
            break;
        }
    }

    ::close(client_fd);
    return 0;
}
