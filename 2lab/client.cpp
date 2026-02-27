#include "protocol.hpp"
#include <thread>
#include <atomic>

struct NetworkInitializer {
    NetworkInitializer() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            exit(1);
        }
#endif
    }
    ~NetworkInitializer() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

int main() {
    NetworkInitializer netInit;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection to server failed\n";
        closesocket(sock);
        return 1;
    }

    std::cout << "Connected\n";
    send_message(sock, MSG_HELLO, "Hello");

    Message msg;
    if (recv_message(sock, msg) && msg.type == MSG_WELCOME) {
        std::cout << "Welcome " << msg.payload << "\n";
    }

    std::atomic<bool> is_running{true};

    std::thread receiver_thread([&]() {
        Message incoming;
        while (is_running && recv_message(sock, incoming)) {
            if (incoming.type == MSG_PONG) {
                std::cout << "PONG\n> " << std::flush;
            } 
            else if (incoming.type == MSG_TEXT) {
                std::cout << "\nServer says: " << incoming.payload << "\n> " << std::flush;
            }
        }
        if (is_running) {
            std::cout << "\nConnection closed by server.\n";
            is_running = false;
        }
    });

    std::string input;
    std::cout << "> ";
    while (is_running && std::getline(std::cin, input)) {
        if (input.empty()) {
            std::cout << "> ";
            continue;
        }

        if (input == "/ping") {
            send_message(sock, MSG_PING);
        } 
        else if (input == "/quit") {
            send_message(sock, MSG_BYE);
            is_running = false;
            break;
        } 
        else {
            send_message(sock, MSG_TEXT, input);
        }
    }

    std::cout << "Disconnected\n";
    is_running = false;
    
    // В Windows SHUT_RDWR превращается в SD_BOTH благодаря макросу
    shutdown(sock, SHUT_RDWR); 
    closesocket(sock);
    
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }

    return 0;
}