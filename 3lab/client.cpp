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

        if (msg.type == MSG_WELCOME) {
            std::cout << "[Server]: " << msg.payload << "\n";
        } else if (msg.type == MSG_TEXT) {
            std::cout << "[Broadcast]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "[Server]: PONG!\n";
        }
    }

    return nullptr;
}

int main(int argc, char* argv[]) {
    std::string nickname = "Student";
    if (argc > 1) nickname = argv[1];

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

            if (!is_connected.load()) break;
            if (input.empty()) continue;

            if (input == "/quit") {
                send_message(sock, MSG_BYE, "");
                quit_requested = true;
                is_connected.store(false);
                break;
            } else if (input == "/ping") {
                send_message(sock, MSG_PING, "");
            } else {
                send_message(sock, MSG_TEXT, nickname + ": " + input);
            }
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
