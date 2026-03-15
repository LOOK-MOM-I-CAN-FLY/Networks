#include <iostream>
#include <string>
#include <cstring>
#include <atomic>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "protocol.h"

// Глобальный флаг для контроля работы потоков
std::atomic<bool> is_connected(false);

bool recv_all(int sock, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, p + total, len - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int sock, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool recv_message(int sock, Message& msg) {
    memset(&msg, 0, sizeof(msg));
    if (!recv_all(sock, &msg.length, sizeof(msg.length))) return false;
    if (msg.length < sizeof(msg.type) || msg.length > sizeof(msg.type) + MAX_PAYLOAD) return false;
    if (!recv_all(sock, &msg.type, msg.length)) return false;
    return true;
}

// --- Поток для приема сообщений ---
void* receive_thread(void* arg) {
    int sock = *(int*)arg;
    Message msg;

    while (is_connected.load()) {
        if (!recv_message(sock, msg)) {
            std::cout << "\n[Server disconnected]\n";
            is_connected.store(false);
            break;
        }

        // Обрабатываем входящие
        if (msg.type == MSG_WELCOME) {
            std::cout << "[Server]: " << msg.payload << "\n";
        } 
        else if (msg.type == MSG_TEXT) {
            std::cout << "[Broadcast]: " << msg.payload << "\n";
        } 
        else if (msg.type == MSG_PONG) {
            std::cout << "[Server]: PONG!\n";
        }
    }
    return NULL;
}

// Вспомогательная функция отправки
void send_message(int sock, uint8_t type, const char* payload) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    if (payload != nullptr) {
        strncpy(msg.payload, payload, MAX_PAYLOAD - 1);
    }
    msg.length = sizeof(msg.type) + strlen(msg.payload);
    
    send_all(sock, &msg, sizeof(msg.length) + msg.length);
}

// --- Главная функция ---
int main(int argc, char* argv[]) {
    std::string nickname = "Student";
    if (argc > 1) nickname = argv[1];

    while (true) { // Главный цикл (для переподключения)
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        std::cout << "Connecting to server...\n";
        
        // Пытаемся подключиться
        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cout << "Connection failed. Retrying in 2 seconds...\n";
            close(sock);
            sleep(2);
            continue; // Начинаем цикл заново
        }

        is_connected.store(true);
        std::cout << "Connected!\n";

        // 1. Отправляем HELLO
        std::string hello_msg = "My name is " + nickname;
        send_message(sock, MSG_HELLO, hello_msg.c_str());

        // 2. Создаем поток для чтения
        pthread_t recv_tid;
        pthread_create(&recv_tid, NULL, receive_thread, &sock);

        // 3. Главный цикл ввода пользователя
        std::string input;
        while (is_connected.load()) {
            std::getline(std::cin, input);
            
            // Если соединение разорвалось пока мы вводили текст
            if (!is_connected.load()) break;
            if (input.empty()) continue;

            if (input == "/quit") {
                send_message(sock, MSG_BYE, "");
                is_connected.store(false);
                break;
            } 
            else if (input == "/ping") {
                send_message(sock, MSG_PING, "");
            } 
            else {
                // Обычный текст
                std::string text = nickname + ": " + input;
                send_message(sock, MSG_TEXT, text.c_str());
            }
        }

        // Очистка при разрыве или выходе
        close(sock);
        pthread_join(recv_tid, NULL); // Ждем завершения потока чтения
        
        if (input == "/quit") {
            std::cout << "Exiting program.\n";
            break; // Полностью выходим из программы
        }

        std::cout << "Reconnecting in 2 seconds...\n";
        sleep(2);
    }

    return 0;
}
