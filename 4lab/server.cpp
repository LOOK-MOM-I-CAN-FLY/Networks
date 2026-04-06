#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "protocol.hpp"

constexpr int THREAD_POOL_SIZE = 10;                 // размер пула потоков, которые будут обслуживать клиентов
constexpr std::size_t NICKNAME_MAX_LEN = 31;         // максимальная длина ника без нулевого символа

std::queue<int> client_queue;                        // очередь сокетов клиентов, которых нужно обработать
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;   // мьютекс для безопасной работы с очередью
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;      // условная переменная, чтобы будить потоки при появлении клиента

struct Client {
    int sock;                                        // сокет клиента
    char nickname[32];                               // ник клиента, храним в статическом массиве
    int authenticated;                               // флаг, прошёл ли клиент авторизацию
};

std::vector<Client> active_clients;                 // список активных клиентов
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // мьютекс для защиты списка клиентов

std::string client_to_string(const sockaddr_in& addr) {    // превращаем адрес клиента в удобную строку
    char ip[INET_ADDRSTRLEN] = {};                          // буфер под IP-адрес в строковом виде
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));     // переводим бинарный IP в текст
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port)); // добавляем порт и возвращаем строку
}

const char* message_type_name(uint8_t type) {               // получаем имя типа сообщения для логов
    switch (type) {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        default: return "MSG_UNKNOWN";                      // если тип неизвестный
    }
}

const char* layer_name(int layer) {                         // имя уровня OSI для логирования
    switch (layer) {
        case 4: return "Transport";
        case 5: return "Session";
        case 6: return "Presentation";
        case 7: return "Application";
        default: return "";
    }
}

void log_layer(int layer, const std::string& text) {        // выводим красивый лог с указанием уровня OSI
    std::cout << "[Layer " << layer << " - " << layer_name(layer) << "] " << text << "\n";
}

bool recv_message_osi(int sock, Message& msg, bool authenticated) {   // принимаем сообщение и логируем процесс по слоям
    log_layer(4, "recv()");                                            // сначала происходит приём байт через сокет
    if (!recv_message(sock, msg)) {                                    // если чтение не удалось
        return false;
    }
    log_layer(6, std::string("deserialize Message (") + message_type_name(msg.type) + ")"); // десериализуем сообщение
    log_layer(5, authenticated ? "client authenticated" : "client not authenticated");        // отмечаем состояние сессии
    return true;
}

bool send_message_osi(int sock, uint8_t type, const std::string& payload, const std::string& action) { // отправка сообщения с логами
    log_layer(7, action + " (" + message_type_name(type) + ")");      // на прикладном уровне решаем, что отправляем
    log_layer(6, std::string("serialize Message (") + message_type_name(type) + ")"); // превращаем структуру в байты
    log_layer(4, "send()");                                            // отправляем через транспортный уровень
    return send_message(sock, type, payload);                          // вызываем реальную функцию отправки
}

std::vector<int> snapshot_client_sockets() {                           // делаем копию списка сокетов, чтобы безопасно обходить его без мьютекса
    std::vector<int> sockets;                                          // сюда складываем сокеты
    pthread_mutex_lock(&clients_mutex);                                // закрываем список клиентов от одновременного изменения
    sockets.reserve(active_clients.size());                             // заранее выделяем память под нужное количество элементов
    for (const auto& client : active_clients) {                         // пробегаем по всем активным клиентам
        sockets.push_back(client.sock);                                 // берём только сокет
    }
    pthread_mutex_unlock(&clients_mutex);                               // отпускаем мьютекс
    return sockets;                                                     // возвращаем снимок списка
}

bool nickname_exists(const std::string& nickname) {                     // проверяем, занят ли ник
    pthread_mutex_lock(&clients_mutex);                                // защищаем доступ к списку
    for (const auto& client : active_clients) {                         // перебираем всех клиентов
        if (nickname == client.nickname) {                              // если ник уже есть
            pthread_mutex_unlock(&clients_mutex);                       // отпускаем мьютекс
            return true;                                                // говорим что ник занят
        }
    }
    pthread_mutex_unlock(&clients_mutex);                               // если не нашли, отпускаем мьютекс
    return false;                                                       // ник свободен
}

void add_client(int sock, const std::string& nickname) {                // добавляем клиента в список активных
    Client client{};                                                    // создаём структуру клиента
    client.sock = sock;                                                 // сохраняем сокет
    std::memset(client.nickname, 0, sizeof(client.nickname));          // очищаем буфер ника
    std::strncpy(client.nickname, nickname.c_str(), sizeof(client.nickname) - 1); // копируем ник без переполнения
    client.authenticated = 1;                                           // отмечаем, что клиент авторизован

    pthread_mutex_lock(&clients_mutex);                                 // защищаем список клиентов
    active_clients.push_back(client);                                   // добавляем нового клиента
    pthread_mutex_unlock(&clients_mutex);                               // отпускаем мьютекс
}

bool get_nickname_by_sock(int sock, std::string& nickname) {            // ищем ник по сокету
    pthread_mutex_lock(&clients_mutex);                                // закрываем список от изменений
    for (const auto& client : active_clients) {                         // перебираем клиентов
        if (client.sock == sock) {                                      // если нашли нужный сокет
            nickname = client.nickname;                                 // забираем ник
            pthread_mutex_unlock(&clients_mutex);                       // отпускаем мьютекс
            return true;                                                // поиск успешен
        }
    }
    pthread_mutex_unlock(&clients_mutex);                               // если не нашли, отпускаем мьютекс
    return false;                                                       // клиента с таким сокетом нет
}

