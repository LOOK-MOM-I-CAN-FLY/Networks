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

void remove_client(int sock) {                                          // удаляем клиента из списка и закрываем его сокет
    std::string nickname;                                               // сюда сохраним ник, если найдём клиента
    bool had_nickname = false;                                           // флаг, нашли ли клиента вообще

    pthread_mutex_lock(&clients_mutex);                                 // защищаем список активных клиентов
    for (auto it = active_clients.begin(); it != active_clients.end(); ++it) { // ищем клиента по сокету
        if (it->sock == sock) {                                         // если нашли
            nickname = it->nickname;                                    // сохраняем ник для логов и рассылки
            had_nickname = true;                                        // отмечаем что клиент был найден
            active_clients.erase(it);                                   // удаляем его из списка
            break;                                                      // выходим из цикла
        }
    }
    pthread_mutex_unlock(&clients_mutex);                               // отпускаем мьютекс

    if (had_nickname) {                                                  // если клиент реально был в списке
        std::string info = "User [" + nickname + "] disconnected";      // формируем информационное сообщение
        for (int client_sock : snapshot_client_sockets()) {             // берём список сокетов остальных клиентов
            send_message_osi(client_sock, MSG_SERVER_INFO, info, "broadcast server info"); // сообщаем о отключении
        }
    }

    ::shutdown(sock, SHUT_RDWR);                                         // запрещаем чтение и запись на сокете
    ::close(sock);                                                       // закрываем сокет окончательно
}

void broadcast_payload(uint8_t type, const std::string& payload) {      // рассылаем сообщение всем клиентам
    for (int client_sock : snapshot_client_sockets()) {                 // получаем список текущих сокетов
        send_message_osi(client_sock, type, payload, "broadcast message"); // отправляем каждому
    }
}

void* worker_thread(void*) {                                             // поток, который обрабатывает клиентов из очереди
    while (true) {                                                       // работаем бесконечно
        int client_sock = -1;                                            // сюда положим сокет клиента

        pthread_mutex_lock(&queue_mutex);                               // защищаем очередь
        while (client_queue.empty()) {                                   // пока в очереди никого нет
            pthread_cond_wait(&queue_cond, &queue_mutex);                // спим и ждём сигнал о новом клиенте
        }
        client_sock = client_queue.front();                              // берём первого клиента
        client_queue.pop();                                              // удаляем его из очереди
        pthread_mutex_unlock(&queue_mutex);                              // отпускаем мьютекс

        Message msg;                                                     // сюда будем читать сообщения клиента
        if (!recv_message_osi(client_sock, msg, false) || msg.type != MSG_HELLO) { // ждём первый HELLO
            remove_client(client_sock);                                  // если не HELLO или ошибка, отключаем клиента
            continue;                                                    // переходим к следующему клиенту
        }

        log_layer(7, std::string("handle ") + message_type_name(msg.type)); // логируем обработку сообщения
        std::cout << "New client said HELLO: " << msg.payload << "\n";      // выводим payload приветствия

        send_message_osi(client_sock, MSG_WELCOME, "Welcome to the server!", "send welcome"); // отправляем приветствие

        bool authenticated = false;                                      // флаг успешной авторизации
        std::string nickname;                                             // ник клиента
        while (!authenticated) {                                          // пока клиент не авторизовался
            if (!recv_message_osi(client_sock, msg, false)) {             // читаем следующее сообщение
                remove_client(client_sock);                               // если соединение оборвалось, удаляем клиента
                authenticated = false;                                    // фиксируем неуспех
                break;                                                    // выходим из цикла авторизации
            }

            log_layer(7, std::string("handle ") + message_type_name(msg.type)); // показываем тип сообщения

            if (msg.type != MSG_AUTH) {                                   // пока не пришёл AUTH, игнорируем всё остальное
                log_layer(7, "ignore message until authentication");       // пишем в лог что сообщение пропущено
                continue;                                                 // ждём дальше
            }

            nickname = msg.payload;                                       // берём ник из payload
            if (nickname.empty() || nickname.size() > NICKNAME_MAX_LEN) { // проверяем валидность ника
                send_message_osi(client_sock, MSG_ERROR, "Invalid nickname", "send auth error"); // отправляем ошибку
                remove_client(client_sock);                               // отключаем клиента
                authenticated = false;                                    // авторизация не удалась
                break;                                                    // выходим
            }

            if (nickname_exists(nickname)) {                              // проверяем, не занят ли ник
                send_message_osi(client_sock, MSG_ERROR, "Nickname already in use", "send auth error"); // сообщаем об ошибке
                remove_client(client_sock);                               // закрываем соединение
                authenticated = false;                                    // не авторизовали
                break;                                                    // уходим из цикла
            }

            add_client(client_sock, nickname);                             // добавляем клиента в список активных
            log_layer(5, "authentication success");                        // логируем успех авторизации

            std::string info = "User [" + nickname + "] connected";       // сообщение для остальных клиентов
            broadcast_payload(MSG_SERVER_INFO, info);                      // сообщаем всем о новом подключении
            authenticated = true;                                          // теперь клиент считается авторизованным
        }

        if (!authenticated) {                                               // если авторизация не удалась
            continue;                                                       // берём нового клиента
        }

        while (true) {                                                      // основной цикл общения с авторизованным клиентом
            if (!recv_message_osi(client_sock, msg, true)) {                // читаем очередное сообщение
                break;                                                      // если ошибка чтения, выходим и удаляем клиента
            }

            log_layer(7, std::string("handle ") + message_type_name(msg.type)); // логируем тип сообщения

            if (msg.type == MSG_TEXT) {                                     // обычное публичное сообщение
                std::string text = "[" + nickname + "]: " + std::string(msg.payload); // добавляем ник к сообщению
                broadcast_payload(MSG_TEXT, text);                          // рассылаем всем
            } else if (msg.type == MSG_PRIVATE) {                           // приватное сообщение
                std::string payload = msg.payload;                          // копируем payload для разбора
                std::size_t sep = payload.find(':');                        // ищем разделитель "ник:сообщение"
                if (sep == std::string::npos || sep == 0 || sep == payload.size() - 1) { // проверяем формат
                    send_message_osi(client_sock, MSG_ERROR, "Invalid private message format", "send private error"); // ошибка формата
                    continue;                                               // ждём следующее сообщение
                }

                std::string target = payload.substr(0, sep);                // ник получателя
                std::string message = payload.substr(sep + 1);              // само сообщение
                if (!message.empty() && message[0] == ' ') {                // если после двоеточия стоит пробел
                    message.erase(0, 1);                                    // убираем его
                }

                int target_sock = -1;                                       // сюда запишем сокет получателя
                if (!get_sock_by_nickname(target, target_sock)) {           // ищем получателя по нику
                    send_message_osi(client_sock, MSG_ERROR, "User [" + target + "] not found", "send private error"); // если нет такого пользователя
                    continue;                                               // возвращаемся в цикл
                }

                std::string private_payload = "[PRIVATE][" + nickname + "]: " + message; // формируем приватное сообщение
                send_message_osi(target_sock, MSG_PRIVATE, private_payload, "send private message"); // отправляем адресату
            } else if (msg.type == MSG_PING) {                               // если клиент прислал ping
                send_message_osi(client_sock, MSG_PONG, "", "send pong");   // отвечаем pong
            } else if (msg.type == MSG_BYE) {                                // если клиент хочет завершить соединение
                std::cout << "Client disconnected by request\n";            // пишем в лог
                break;                                                       // выходим из цикла общения
            }
        }

        remove_client(client_sock);                                         // после выхода из цикла удаляем клиента и закрываем сокет
    }

    return nullptr;                                                         // завершаем поток
}

int main() {
    int server_sock = ::socket(AF_INET, SOCK_STREAM, 0);                    // создаём TCP-сокет IPv4
    if (server_sock < 0) {                                                  // проверяем, создался ли сокет
        std::perror("socket");                                              // выводим системную ошибку
        return 1;                                                           // завершаем программу с ошибкой
    }

    int opt = 1;                                                            // значение для включения повторного использования адреса
    ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // разрешаем быстро перезапускать сервер

    sockaddr_in server_addr{};                                              // структура адреса сервера
    server_addr.sin_family = AF_INET;                                       // указываем IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;                               // принимаем соединения на все адреса
    server_addr.sin_port = htons(SERVER_PORT);                              // задаём порт сервера в сетевом порядке байт
    if (::bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) { // привязываем сокет к порту
        std::perror("bind");                                                // выводим ошибку
        ::close(server_sock);                                               // закрываем сокет
        return 1;                                                           // выходим
    }

    if (::listen(server_sock, 10) < 0) {                                    // переводим сокет в режим прослушивания
        std::perror("listen");                                             // показываем ошибку, если listen не сработал
        ::close(server_sock);                                               // закрываем сокет
        return 1;                                                           // выходим
    }

    std::cout << "Server listening on port " << SERVER_PORT << "\n";       // сообщаем, что сервер запущен

    pthread_t threads[THREAD_POOL_SIZE];                                     // массив идентификаторов потоков
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {                            // создаём нужное количество рабочих потоков
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);       // запускаем поток, который будет обслуживать клиентов
    }

    while (true) {                                                           // главный цикл сервера
        sockaddr_in client_addr{};                                           // адрес подключившегося клиента
        socklen_t client_len = sizeof(client_addr);                          // размер структуры адреса
        int client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len); // принимаем новое соединение
        if (client_sock < 0) {                                              // если accept вернул ошибку
            continue;                                                       // просто ждём следующее подключение
        }

        std::cout << "Connection accepted from " << client_to_string(client_addr) << "\n"; // выводим адрес клиента
        pthread_mutex_lock(&queue_mutex);                                   // защищаем очередь клиентов
        client_queue.push(client_sock);                                     // кладём сокет в очередь на обработку
        pthread_cond_signal(&queue_cond);                                   // будим один из спящих рабочих потоков
        pthread_mutex_unlock(&queue_mutex);                                 // отпускаем мьютекс
    }

    ::close(server_sock);                                                    // закрываем серверный сокет перед выходом
    return 0;                                                                // завершаем программу
}


