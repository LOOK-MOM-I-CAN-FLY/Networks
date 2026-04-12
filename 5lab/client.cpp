#include <atomic>
#include <cstring>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.hpp"

namespace {

std::atomic<bool> is_connected(false);

struct RecvThreadArgs {
    int sock;
};

bool has_prefix(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string strip_offline_prefix(const std::string& s) {
    if (!has_prefix(s, "OFFLINE:")) {
        return s;
    }
    std::string rest = s.substr(std::strlen("OFFLINE:"));
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
        rest.erase(0, 1);
    }
    return rest;
}

std::string format_text_line(const MessageEx& msg) {
    return "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "][" +
           std::string(msg.sender) + "]: " + std::string(msg.payload);
}

std::string format_private_line(const MessageEx& msg) {
    const std::string payload = msg.payload;
    const bool offline = has_prefix(payload, "OFFLINE:");
    const std::string text = strip_offline_prefix(payload);

    std::string line = "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "]";
    if (offline) {
        line += "[OFFLINE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text;
        return line;
    }
    line += "[PRIVATE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text;
    return line;
}

void* receive_thread(void* arg) {
    auto* args = static_cast<RecvThreadArgs*>(arg);
    const int sock = args->sock;
    delete args;

    MessageEx msg{};
    while (is_connected.load()) {
        if (!recv_message_ex(sock, msg)) {
            std::cout << "\n[Server disconnected]\n";
            is_connected.store(false);
            break;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << format_text_line(msg) << "\n";
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << format_private_line(msg) << "\n";
        } else if (msg.type == MSG_HISTORY_DATA) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG\n";
        } else if (msg.type == MSG_WELCOME) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        }
    }

    return nullptr;
}

std::string read_nickname(const std::string& initial) {
    std::string nickname = initial;
    while (nickname.empty()) {
        std::cout << "Enter nickname: ";
        if (!std::getline(std::cin, nickname)) {
            return "";
        }
        if (nickname.empty()) {
            std::cout << "Nickname cannot be empty.\n";
        }
    }
    if (nickname.size() >= MAX_NAME) {
        nickname.resize(MAX_NAME - 1);
    }
    return nickname;
}

void print_help() {
    std::cout << "Available commands:\n";
    std::cout << "/help\n";
    std::cout << "/list\n";
    std::cout << "/history\n";
    std::cout << "/history N\n";
    std::cout << "/quit\n";
    std::cout << "/w <nick> <message>\n";
    std::cout << "/ping\n";
    std::cout << "Tip: packets never sleep\n";
}

bool parse_history_cmd(const std::string& input, std::string& payload) {
    payload.clear();
    if (input == "/history") {
        return true;
    }
    if (input.rfind("/history ", 0) != 0) {
        return false;
    }
    std::string rest = input.substr(std::strlen("/history "));
    while (!rest.empty() && rest[0] == ' ') {
        rest.erase(0, 1);
    }
    if (rest.empty()) {
        return true;
    }
    for (char ch : rest) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    if (rest == "0") {
        return false;
    }
    payload = rest;
    return true;
}

bool parse_whisper_cmd(const std::string& input, std::string& nick, std::string& message) {
    if (input.rfind("/w ", 0) != 0) {
        return false;
    }
    std::string rest = input.substr(3);
    while (!rest.empty() && rest[0] == ' ') {
        rest.erase(0, 1);
    }
    std::size_t space = rest.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    nick = rest.substr(0, space);
    message = rest.substr(space + 1);
    return !(nick.empty() || message.empty());
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string nickname;
    if (argc > 1) {
        nickname = argv[1];
    }
    nickname = read_nickname(nickname);
    if (nickname.empty()) {
        return 0;
    }

    while (true) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::perror("socket");
            return 1;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        std::cout << "Connecting to server...\n";
        if (::connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            std::cout << "Connection failed. Retrying in 2 seconds...\n";
            ::close(sock);
            sleep(2);
            continue;
        }

        is_connected.store(true);
        std::cout << "Connected!\n";

        (void)send_message_ex(sock, MSG_HELLO, 0, nickname, "", std::time(nullptr), "My name is " + nickname);

        MessageEx welcome{};
        if (!recv_message_ex(sock, welcome) || welcome.type != MSG_WELCOME) {
            std::cout << "Handshake failed.\n";
            is_connected.store(false);
            ::close(sock);
            sleep(2);
            continue;
        }

        std::cout << "[SERVER]: " << welcome.payload << "\n";
        (void)send_message_ex(sock, MSG_AUTH, 0, nickname, "", std::time(nullptr), nickname);

        pthread_t recv_tid{};
        auto* args = new RecvThreadArgs{sock};
        pthread_create(&recv_tid, nullptr, receive_thread, args);

        std::string input;
        bool quit_requested = false;
        while (is_connected.load()) {
            if (!std::getline(std::cin, input)) {
                quit_requested = true;
                is_connected.store(false);
                break;
            }
            if (!is_connected.load()) {
                break;
            }
            if (input.empty()) {
                continue;
            }

            if (input == "/help") {
                print_help();
                continue;
            }

            if (input == "/quit") {
                (void)send_message_ex(sock, MSG_BYE, 0, nickname, "", std::time(nullptr), "");
                quit_requested = true;
                is_connected.store(false);
                break;
            }

            if (input == "/ping") {
                (void)send_message_ex(sock, MSG_PING, 0, nickname, "", std::time(nullptr), "");
                continue;
            }

            if (input == "/list") {
                (void)send_message_ex(sock, MSG_LIST, 0, nickname, "", std::time(nullptr), "");
                continue;
            }

            std::string hist_payload;
            if (input == "/history" || input.rfind("/history ", 0) == 0) {
                if (!parse_history_cmd(input, hist_payload)) {
                    std::cout << "Usage: /history or /history N\n";
                    continue;
                }
                (void)send_message_ex(sock, MSG_HISTORY, 0, nickname, "", std::time(nullptr), hist_payload);
                continue;
            }

            std::string target;
            std::string msg_text;
            if (parse_whisper_cmd(input, target, msg_text)) {
                if (target.size() >= MAX_NAME) {
                    target.resize(MAX_NAME - 1);
                }
                (void)send_message_ex(sock, MSG_PRIVATE, 0, nickname, target, std::time(nullptr), msg_text);
                continue;
            }

            (void)send_message_ex(sock, MSG_TEXT, 0, nickname, "", std::time(nullptr), input);
        }

        ::shutdown(sock, SHUT_RDWR);
        ::close(sock);
        pthread_join(recv_tid, nullptr);

        if (quit_requested) {
            std::cout << "Exiting program.\n";
            break;
        }

        std::cout << "Reconnecting in 2 seconds...\n";
        sleep(2);
    }

    return 0;
}

