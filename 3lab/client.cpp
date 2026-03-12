#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "protocol.h"

// Глобальный флаг для контроля работы потоков
bool is_connected = false;

// --- Поток для приема сообщений ---
void* receive_thread(void* arg) {
    int sock = *(int*)arg;
    Message msg;

    while (is_connected) {
        memset(&msg, 0, sizeof(msg));
        
        // Читаем длину
        int n = recv(sock, &msg.length, sizeof(msg.length), 0);
        if (n <= 0) {
            std::cout << "\n[Server disconnected]\n";
            is_connected = false;
            break;
        }

        // Читаем само сообщение
        n = recv(sock, &msg.type, msg.length, 0);
        if (n <= 0) {
            is_connected = false;
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
    
    send(sock, &msg, sizeof(msg.length) + msg.length, 0);
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

        is_connected = true;
        std::cout << "Connected!\n";

        // 1. Отправляем HELLO
        std::string hello_msg = "My name is " + nickname;
        send_message(sock, MSG_HELLO, hello_msg.c_str());

        // 2. Создаем поток для чтения
        pthread_t recv_tid;
        pthread_create(&recv_tid, NULL, receive_thread, &sock);

        // 3. Главный цикл ввода пользователя
        std::string input;
        while (is_connected) {
            std::getline(std::cin, input);
            
            // Если соединение разорвалось пока мы вводили текст
            if (!is_connected) break;
            if (input.empty()) continue;

            if (input == "/quit") {
                send_message(sock, MSG_BYE, "");
                is_connected = false;
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
