#include "protocol.hpp"

// Класс-помощник для инициализации сокетов на Windows (RAII)
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
    NetworkInitializer netInit; // Инициализируем сеть (только для Windows)

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    // В Windows SO_REUSEADDR работает чуть иначе, 
    // но мы оставим его для совместимости: тип opt должен быть const char*
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed\n";
        closesocket(server_fd);
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        std::cerr << "Listen failed\n";
        closesocket(server_fd);
        return 1;
    }

    std::cout << "Server is listening on port 8080...\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);
        std::string client_info = std::string(client_ip) + ":" + std::to_string(client_port);

        std::cout << "Client connected\n";

        Message msg;
        if (recv_message(client_fd, msg) && msg.type == MSG_HELLO) {
            std::cout << "[" << client_info << "]: " << msg.payload << "\n";
            send_message(client_fd, MSG_WELCOME, client_info);
        } else {
            closesocket(client_fd);
            continue;
        }

        while (recv_message(client_fd, msg)) {
            if (msg.type == MSG_TEXT) {
                std::cout << "[" << client_info << "]: " << msg.payload << "\n";
            } 
            else if (msg.type == MSG_PING) {
                send_message(client_fd, MSG_PONG);
            } 
            else if (msg.type == MSG_BYE) {
                break;
            }
        }

        std::cout << "Client disconnected\n";
        closesocket(client_fd);
    }

    closesocket(server_fd);
    return 0;
}