#include "protocol.hpp"
#include <iostream>
#include <string>
#include <sys/socket.h>
// socket(), bind(), listen(), accept(), setsockopt(), shutdown()
#include <netinet/in.h>
// sockaddr_in, INADDR_ANY, htons()
#include <arpa/inet.h>
// inet_ntop(), ntohs()
#include <unistd.h>
// close()

int main() {
    // Создаём TCP-сокет.
    // AF_INET   — это IPv4.
    // SOCK_STREAM — потоковый сокет.
    // В связке с IPv4 это обычно означает TCP.
    // 0 — система сама подставит протокол по умолчанию.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Если socket() вернул отрицательное значение, значит сокет создать не удалось.
    if (server_fd < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    // setsockopt() настраивает параметры сокета.
    // Здесь мы включаем SO_REUSEADDR.
    //
    // Зачем это нужно:
    // после перезапуска сервера порт может ещё некоторое время считаться занятым,
    // особенно если предыдущий сокет был в состоянии TIME_WAIT.
    // SO_REUSEADDR позволяет быстрее поднять сервер заново на том же порту.
    int opt = 1;

    // Устанавливаем параметр сокета:
    // SOL_SOCKET   — уровень, на котором задаём опцию.
    // SO_REUSEADDR — сама опция.
    // &opt         — указатель на значение 1, то есть "включить".
    // sizeof(opt)  — размер передаваемого значения.
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // sockaddr_in — структура адреса IPv4.
    // В ней мы задаём:
    // - семейство адресов
    // - IP-адрес
    // - порт
    sockaddr_in server_addr{};

    // Указываем, что это именно IPv4.
    server_addr.sin_family = AF_INET;

    // INADDR_ANY означает:
    // "слушай все сетевые интерфейсы на этой машине".
    //
    // То есть сервер будет доступен не только на 127.0.0.1,
    // но и на всех адресах, которые есть у этой машины.
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // htons() — host to network short.
    // Порт в памяти компьютера надо перевести в сетевой порядок байт.
    // Иначе разные машины могут по-разному прочитать число 8080.
    server_addr.sin_port = htons(8080);

    // bind() привязывает сокет к конкретному адресу и порту.
    // То есть мы говорим системе:
    // "этот сокет будет слушать именно IPv4-адреса на порту 8080".
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed\n";

        // Если bind не удался, сокет нужно закрыть, чтобы не оставлять ресурс висеть.
        close(server_fd);
        return 1;
    }

    // listen() переводит сокет в режим ожидания входящих подключений.
    // Второй параметр — backlog.
    // Это количество клиентов, которое система может держать в очереди ожидания.
    if (listen(server_fd, 1) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }

    // Сообщаем в консоль, что сервер уже готов принимать клиентов.
    std::cout << "Server is listening on port 8080...\n";

    // Бесконечный цикл.
    // Сервер работает постоянно и ждёт новых клиентов один за другим.
    // Здесь сервер однопоточный по приёму клиентов:
    // сначала обслуживает одного, потом переходит к следующему.
    while (true) {
        // sockaddr_in для адреса клиента.
        // accept() сюда запишет IP и порт подключившегося клиента.
        sockaddr_in client_addr{};

        // accept() хочет длину структуры адреса клиента.
        // socklen_t — стандартный тип для этого размера.
        socklen_t client_len = sizeof(client_addr);

        // accept() ждёт входящее подключение.
        // Когда клиент подключается, создаётся новый сокет client_fd,
        // а server_fd продолжает оставаться слушающим сокетом.
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        // Если accept вернул отрицательное значение, значит подключение не получилось.
        // В этом случае просто идём дальше и ждём следующего клиента.
        if (client_fd < 0) {
            continue;
        }

        // Буфер для текстового IP-адреса клиента.
        // INET_ADDRSTRLEN — стандартный размер буфера для IPv4-адреса.
        char client_ip[INET_ADDRSTRLEN];

        // inet_ntop() переводит бинарный IP-адрес в строку.
        // AF_INET означает IPv4.
        // &client_addr.sin_addr — сам IP-адрес в бинарном виде.
        // client_ip — куда записать текстовый результат.
        // INET_ADDRSTRLEN — размер буфера.
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // ntohs() — network to host short.
        // Порт из сетевого порядка байт переводим в обычное число,
        // чтобы его можно было нормально вывести в консоль.
        int client_port = ntohs(client_addr.sin_port);

        // Формируем строку вида "127.0.0.1:54321".
        // Это просто удобное представление адреса клиента для логов.
        std::string client_info = std::string(client_ip) + ":" + std::to_string(client_port);

        // Пишем в консоль, что клиент подключился.
        std::cout << "Client connected\n";

        // Буфер для сообщения, которое пришлёт клиент.
        Message msg;

        // Первый шаг протокола:
        // ждём от клиента приветственное сообщение MSG_HELLO.
        // recv_message() читает полный пакет по нашему формату:
        // length + type + payload.
        if (recv_message(client_fd, msg) && msg.type == MSG_HELLO) {
            // Если клиент действительно прислал MSG_HELLO,
            // выводим его сообщение в консоль.
            std::cout << "[" << client_info << "]: " << msg.payload << "\n";

            // Отправляем обратно сообщение приветствия.
            // В payload кладём строку client_info, чтобы клиент увидел,
            // откуда его приняли.
            send_message(client_fd, MSG_WELCOME, client_info);
        } else {
            // Если клиент прислал не то, что ожидалось,
            // или сообщение вообще не удалось прочитать,
            // закрываем его сокет и переходим к следующему подключению.
            close(client_fd);
            continue;
        }

        // Теперь клиент уже прошёл начальный handshake.
        // Дальше читаем все сообщения, пока соединение живо.
        while (recv_message(client_fd, msg)) {
            // Если пришёл обычный текст, просто печатаем его в консоль.
            if (msg.type == MSG_TEXT) {
                std::cout << "[" << client_info << "]: " << msg.payload << "\n";
            }
            // Если пришёл PING, отвечаем PONG.
            else if (msg.type == MSG_PING) {
                send_message(client_fd, MSG_PONG);
            }
            // Если пришёл BYE, клиент сам хочет завершить соединение.
            else if (msg.type == MSG_BYE) {
                break;
            }
        }

        // Если мы вышли из цикла, значит клиент отключился
        // или сам прислал команду завершения.
        std::cout << "Client disconnected\n";

        // Закрываем клиентский сокет, потому что его работа закончена.
        close(client_fd);
    }

    // До этой строки программа обычно не доходит,
    // потому что внешний цикл бесконечный.
    // Но закрытие слушающего сокета здесь всё равно корректно с точки зрения структуры кода.
    close(server_fd);
    return 0;
}
