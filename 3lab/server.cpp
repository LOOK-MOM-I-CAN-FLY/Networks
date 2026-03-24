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
void (int sock) {
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

void* worker_thread(void*) {//Она берет клиента из очереди, проверяет его и обслуживает до момента отключения.
    while (true) {
        int client_sock = -1;//нужно для хранения дескриптора текущего клиента 

        pthread_mutex_lock(&queue_mutex);//захватываем мьютекс так как работает с очередью
        while (client_queue.empty()) {//будем ждать если очередь пуста
            pthread_cond_wait(&queue_cond, &queue_mutex);//поток засыпает и освобождает мьютекс. Когда главный поток добавит клиента он подаст сигнал и этот поток проснется снова захватив мьютекс
        }
        client_sock = client_queue.front();//берём сокет первого клиента в очереди
        client_queue.pop();//ну и убираем клиента из очереди типо обслужили
        pthread_mutex_unlock(&queue_mutex);//отпускаем мьютекс

        Message msg;
        if (!recv_message(client_sock, msg) || msg.type != MSG_HELLO) {//если не получилось получить сообщение от клиента или клиент не прислал hello то бб 
            remove_client(client_sock);
            continue;
        }

        std::cout << "New client said HELLO: " << msg.payload << "\n";//вывод в консоль текст приветствия от клиента 

        send_message(client_sock, MSG_WELCOME, "Welcome to the server!");//и посылаем ответное сообщение клиенту 

        pthread_mutex_lock(&clients_mutex);
        active_clients.push_back(client_sock);//добавляем новоиспечённого клиента к активным чтобы с ним работать дальше 
        pthread_mutex_unlock(&clients_mutex);

        while (true) {//цикл жизни конкретного соединения
            if (!recv_message(client_sock, msg)) {//получаем данные, если клиент отключился или произошла ошибка чтения — выходим из цикла
                break;
            }

            if (msg.type == MSG_TEXT) {
                std::cout << "Broadcasting: " << msg.payload << "\n";//если он прислал сообщение то выводим его всем
                broadcast_message(msg);
            } else if (msg.type == MSG_PING) {
                send_message(client_sock, MSG_PONG, "");//иначе если он прислал пинг, то мы ЕМУ посылаем понг
            } else if (msg.type == MSG_BYE) {
                std::cout << "Client disconnected by request\n";//если он написал quit то мы выводим сообщение что он покинул чат
                break;
            }
        }

        remove_client(client_sock);
    }

    return nullptr;
}
//подготовка TCP-сервера и запуск пула потоков
int main() {
    int server_sock = ::socket(AF_INET, SOCK_STREAM, 0);//AF_INET — работаем через IPv4, SOCK_STREAM — используем протокол TCP         |  :: - означает использование системной функции
    if (server_sock < 0) {
        std::perror("socket");//если вернулось -1 значит ОС не смогла выделить ресурсы для создания сокета 
        return 1;
    }

    int opt = 1;
    //REUSEADDR - разрешает повторное использование локального адреса и порта сразу после закрытия программы
    ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//крч по сути я говорю ос'ке: Если я перезапущу сервер даь мне занять этот порт немедленно и не заставляй меня ждать пару минут (30 - 120 сек)

    sockaddr_in server_addr{};//создаём структуру куда сложем данные о адресе 
    server_addr.sin_family = AF_INET;//IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;// сервак слушает все порты компа
    server_addr.sin_port = htons(SERVER_PORT);//число в сетевой формат
    //reinterpret_cast<sockaddr*>(&server_addr) - нужно так сделать потому что ::bind работает и с IPv6 поэтому надо к общему виду
    if (::bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {//по сути привязываем наш номер телефона к конкретной линии
        //Мы говорим ядру: я хочу чтобы все данные приходящие на указанный мной порт и IP отправлялись в мой сокет server_sock
        std::perror("bind");//< 0 — если функция вернула отрицательное число значит порт занят или нет прав доступа
        ::close(server_sock);
        return 1;
    }

    if (::listen(server_sock, 10) < 0) {//10 — это размер очереди ожидающих подключений. Если 11-й клиент постучится в тот момент когда сервер еще не успел вызвать accept он получит отказ      \  < 0 => сервак не смог перейти в режим ожидания соединений. 
        std::perror("listen");
        ::close(server_sock);
        return 1;
    }

    std::cout << "Server listening on port " << SERVER_PORT << "\n";

    pthread_t threads[THREAD_POOL_SIZE];//массив для хранения идентификаторов потоков pthread_t - обычно какое то большое число 
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);//создаём 10 рабочих потоков и они все засыспают до появиления новых клиентов в worker_thread
    }

    while (true) {//основной поток сервера крутится вечно в этом цикле 
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);//главный поток замирает здесь пока кто-то не подключится. как только клиент пришел accept создает новый отдельный сокет специально для общения с этим клиентом
        if (client_sock < 0) {//если подключение сорвалось в процессе (клиент передумал) просто идем на следующий круг
            continue;
        }

        std::cout << "Connection accepted from " << client_to_string(client_addr) << "\n";
        pthread_mutex_lock(&queue_mutex);//локаем мьютекс
        client_queue.push(client_sock);//кладем дескриптор нового клиента в std::queue
        pthread_cond_signal(&queue_cond);//ВОТ ТУТ КАК РАЗ будим спящий поток чтобы он обрабтал клиента 
        pthread_mutex_unlock(&queue_mutex);//анлокаем мьютекс
    }

    ::close(server_sock);
    return 0;
}
