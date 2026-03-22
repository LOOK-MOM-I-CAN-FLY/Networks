#include <iostream>
#include <vector>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "protocol.hpp"

constexpr int THREAD_POOL_SIZE = 10;//размер пула рабочий потоков которые будут обрабатывать подключившихся клиентов
//очередь куда главный поток складывает новые сокеты клиентов откуда потоки будут забирать клиентов 
std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;//мьютекс для защиты очереди потому что туда могут и писать и читать одновременно несколько потоков
pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;// нужно чтобы если очередь была пустой воркер засыпал иначе пробуждался 

std::vector<int> active_clients;//список активных клиентов, нужен для рассылки сообщений всем подключённым клиентам
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;//и мьютекс для его защиты по той же причине что и для очереди
//функция для красивого логирования, преобразует sockaddr_in -> IP:PORT
std::string client_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};//буфер под строку айпишника
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));//превращает бинарный айпишник в текст
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));//ну и выводим просто строку
}
//удаляет клиента из списка активных и закрывает его сокет
void remove_client(int sock) {
    pthread_mutex_lock(&clients_mutex);//захватываем мьютекст
    for (auto it = active_clients.begin(); it != active_clients.end(); ++it) {//ищем пока не найдём
        if (*it == sock) {
            active_clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);//отпускаем мьютекс

    ::shutdown(sock, SHUT_RDWR);//мягко сообщеаем ядру что больше не читаем и не пишем в этот сокет
    ::close(sock);//закрываем дескриптор 
}
//посылаем сообщения всем активным клиентам не считая того кто это сообщение отправил
void broadcast_message(const Message& msg, int exclude_sock = -1) {
    pthread_mutex_lock(&clients_mutex);//захватываем мьютекст

    //так как длина состоит из 1 + payload то мы вычитаем 1 
    const std::size_t payload_len = static_cast<std::size_t>(ntohl(msg.length) - 1u);
    //весь пакет состоит из 4 байт (длина) + 1 байт (типа) + n байт пейлод
    const std::size_t total_size = sizeof(msg.length) + 1u + payload_len;
    for (int client_sock : active_clients) {
        if (client_sock == exclude_sock) {//наткнувшись на себя же скипаем
            continue;
        }
        send_all(client_sock, &msg, total_size);
    }
    pthread_mutex_unlock(&clients_mutex);//отпускаем мьютекс
}

void* worker_thread(void*) {
    while (true) {
        int client_sock = -1;

        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty()) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        Message msg;
        if (!recv_message(client_sock, msg) || msg.type != MSG_HELLO) {
            remove_client(client_sock);
            continue;
        }

        std::cout << "New client said HELLO: " << msg.payload << "\n";

        send_message(client_sock, MSG_WELCOME, "Welcome to the server!");

        pthread_mutex_lock(&clients_mutex);
        active_clients.push_back(client_sock);
        pthread_mutex_unlock(&clients_mutex);

        while (true) {
            if (!recv_message(client_sock, msg)) {
                break;
            }

            if (msg.type == MSG_TEXT) {
                std::cout << "Broadcasting: " << msg.payload << "\n";
                broadcast_message(msg);
            } else if (msg.type == MSG_PING) {
                send_message(client_sock, MSG_PONG, "");
            } else if (msg.type == MSG_BYE) {
                std::cout << "Client disconnected by request\n";
                break;
            }
        }

        remove_client(client_sock);
    }

    return nullptr;
}

int main() {
    int server_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (::bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::perror("bind");
        ::close(server_sock);
        return 1;
    }

    if (::listen(server_sock, 10) < 0) {
        std::perror("listen");
        ::close(server_sock);
        return 1;
    }

    std::cout << "Server listening on port " << SERVER_PORT << "\n";

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock < 0) {
            continue;
        }

        std::cout << "Connection accepted from " << client_to_string(client_addr) << "\n";
        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    ::close(server_sock);
    return 0;
}
