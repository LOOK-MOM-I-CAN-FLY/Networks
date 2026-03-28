#include <iostream>
#include <string>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "protocol.hpp"

std::atomic<bool> is_connected(false);

struct RecvThreadArgs {
    int sock;
};

void* receive_thread(void* arg) {
    auto* args = static_cast<RecvThreadArgs*>(arg);
    const int sock = args->sock;
    delete args;

    Message msg;
    while (is_connected.load()) {
        if (!recv_message(sock, msg)) {
            std::cout << "\n[Server disconnected]\n";
            is_connected.store(false);
            break;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG!\n";
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
    return nickname;
}

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

        send_message(sock, MSG_HELLO, "My name is " + nickname);

        Message welcome_msg;
        if (!recv_message(sock, welcome_msg) || welcome_msg.type != MSG_WELCOME) {
            std::cout << "Handshake failed.\n";
            is_connected.store(false);
            ::close(sock);
            sleep(2);
            continue;
        }

        std::cout << "[SERVER]: " << welcome_msg.payload << "\n";
        send_message(sock, MSG_AUTH, nickname);

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

            if (input == "/quit") {
                send_message(sock, MSG_BYE, "");
                quit_requested = true;
                is_connected.store(false);
                break;
            }

            if (input == "/ping") {
                send_message(sock, MSG_PING, "");
                continue;
            }

            if (input.rfind("/w ", 0) == 0) {
                std::string rest = input.substr(3);
                while (!rest.empty() && rest[0] == ' ') {
                    rest.erase(0, 1);
                }
                std::size_t space = rest.find(' ');
                if (space == std::string::npos) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                std::string target = rest.substr(0, space);
                std::string message = rest.substr(space + 1);
                if (target.empty() || message.empty()) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                send_message(sock, MSG_PRIVATE, target + ":" + message);
                continue;
            }

            send_message(sock, MSG_TEXT, input);
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
