#include <iostream>
#include <vector>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "protocol.h"

#define THREAD_POOL_SIZE 10

// --- Глобальные переменные ---

// Очередь подключений
std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;

// Список активных клиентов для рассылки
std::vector<int> active_clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Вспомогательные функции ---
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

bool send_message(int sock, Message& msg) {
    return send_all(sock, &msg, sizeof(msg.length) + msg.length);
}

// Отправка сообщения ВСЕМ клиентам (широковещательная рассылка)
void broadcast_message(Message& msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int client_sock : active_clients) {
        send_message(client_sock, msg);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Удаление клиента из списка
void remove_client(int sock) {
    pthread_mutex_lock(&clients_mutex);
    for (auto it = active_clients.begin(); it != active_clients.end(); ++it) {
        if (*it == sock) {
            active_clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(sock);
    std::cout << "Client disconnected. Socket: " << sock << "\n";
}

// --- Рабочий поток (Worker) ---
void* worker_thread(void*) {
    while (true) {
        int client_sock = -1;

        // 1. Берем клиента из очереди безопасно
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty()) {
            // Спим, пока не разбудят (пока нет клиентов)
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        // 2. Ждем HELLO
        Message msg;
        if (!recv_message(client_sock, msg) || msg.type != MSG_HELLO) {
            remove_client(client_sock);
            continue;
        }

        std::cout << "New client said HELLO: " << msg.payload << "\n";

        Message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = MSG_WELCOME;
        strcpy(reply.payload, "Welcome to the server!");
        reply.length = sizeof(reply.type) + strlen(reply.payload);
        send_message(client_sock, reply);

        // 3. Добавляем клиента в глобальный список
        pthread_mutex_lock(&clients_mutex);
        active_clients.push_back(client_sock);
        pthread_mutex_unlock(&clients_mutex);

        // 4. Цикл общения с клиентом
        while (true) {
            if (!recv_message(client_sock, msg)) break;

            if (msg.type == MSG_TEXT) {
                std::cout << "Received TEXT, broadcasting...\n";
                broadcast_message(msg);
            } else if (msg.type == MSG_PING) {
                std::cout << "Received PING, sending PONG...\n";
                Message pong;
                memset(&pong, 0, sizeof(pong));
                pong.type = MSG_PONG;
                pong.length = sizeof(pong.type);
                send_message(client_sock, pong);
            } else if (msg.type == MSG_BYE) {
                std::cout << "Received BYE.\n";
                break;
            }
        }
        
        // 5. Клиент отключился - убираем за собой
        remove_client(client_sock);
    }
    return NULL;
}

// --- Главная функция ---
int main() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    
    // Позволяем сразу переиспользовать порт после перезапуска сервера (избавляет от ошибки "Address already in use")
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }

    listen(server_sock, 10);
    std::cout << "Server listening on port " << SERVER_PORT << "\n";

    // Создаем Пул Потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    // Главный цикл приема клиентов
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_len);

        if (client_sock >= 0) {
            std::cout << "Connection accepted!\n";
            // Кладем сокет в очередь и будим один из рабочих потоков
            pthread_mutex_lock(&queue_mutex);
            client_queue.push(client_sock);
            pthread_cond_signal(&queue_cond);
            pthread_mutex_unlock(&queue_mutex);
        }
    }

    close(server_sock);
    return 0;
}
