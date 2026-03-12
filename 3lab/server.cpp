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

// Отправка сообщения ВСЕМ клиентам (широковещательная рассылка)
void broadcast_message(int sender_sock, Message& msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int client_sock : active_clients) {
        // Отправляем всем, кроме того, кто это написал (чтобы не было эха)
        if (client_sock != sender_sock) {
            // Отправляем размер структуры = размер length (4 байта) + сама length
            send(client_sock, &msg, sizeof(msg.length) + msg.length, 0);
        }
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
void* worker_thread(void* arg) {
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

        // 2. Добавляем клиента в глобальный список
        pthread_mutex_lock(&clients_mutex);
        active_clients.push_back(client_sock);
        pthread_mutex_unlock(&clients_mutex);

        // 3. Цикл общения с клиентом
        Message msg;
        while (true) {
            memset(&msg, 0, sizeof(msg));
            
            // Читаем сначала заголовок (длину)
            int n = recv(client_sock, &msg.length, sizeof(msg.length), 0);
            if (n <= 0) break; // Клиент отвалился

            // Читаем остальное (тип + данные)
            n = recv(client_sock, &msg.type, msg.length, 0);
            if (n <= 0) break; 

            // Обработка сообщений по ТЗ
            if (msg.type == MSG_HELLO) {
                std::cout << "New client said HELLO: " << msg.payload << "\n";
                
                // Отвечаем WELCOME
                Message reply;
                memset(&reply, 0, sizeof(reply));
                reply.type = MSG_WELCOME;
                strcpy(reply.payload, "Welcome to the server!");
                reply.length = sizeof(reply.type) + strlen(reply.payload);
                send(client_sock, &reply, sizeof(reply.length) + reply.length, 0);
            } 
            else if (msg.type == MSG_TEXT) {
                std::cout << "Received TEXT, broadcasting...\n";
                broadcast_message(client_sock, msg);
            } 
            else if (msg.type == MSG_PING) {
                std::cout << "Received PING, sending PONG...\n";
                Message reply;
                reply.type = MSG_PONG;
                reply.length = sizeof(reply.type);
                send(client_sock, &reply, sizeof(reply.length) + reply.length, 0);
            } 
            else if (msg.type == MSG_BYE) {
                std::cout << "Received BYE.\n";
                break; // Выходим из цикла, дальше сработает remove_client
            }
        }
        
        // 4. Клиент отключился - убираем за собой
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
