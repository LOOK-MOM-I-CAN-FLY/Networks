// здесь мы создаём TCP-сокет, подключаемся к серверу,
// отправляем/принимаем сообщения и обрабатываем ввод пользователя
#include "protocol.hpp"
// подключаем наш протокол сообщений
// в нём уже объявлены:
// - Message
// - MessageType (MSG_HELLO, MSG_TEXT, MSG_PING и т.д.)
// - send_message()
// - recv_message()

#include <iostream>
#include <string>
#include <thread>
// std::thread — отдельный поток, который будет постоянно слушать сервер
#include <atomic>
// std::atomic<bool> — флаг, который безопасно читается и пишется из двух потоков
#include <sys/socket.h>
// socket(), connect(), shutdown(), sockaddr, sockaddr_in и т.п.
#include <arpa/inet.h>
// htons(), inet_pton() — работа с IP-адресами и порядком байт
#include <unistd.h>

int main() {

    // создаём TCP-сокет
    // AF_INET   — IPv4
    // SOCK_STREAM — потоковый сокет
    // для IPv4 это почти всегда означает TCP
    // 0          — пусть система сама выберет протокол по умолчанию
    int sock = socket(AF_INET, SOCK_STREAM, 0);//0 -> IPPROTO_TCP для принудительного использования TCP

    // ошибка если отриц.
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    // sockaddr_in — структура адреса IPv4
    // заполняем:
    // - семейство адресов
    // - порт
    // - IP-адрес сервера
    sockaddr_in server_addr{};

    // указываем, что это IPv4.
    server_addr.sin_family = AF_INET;

    // htons() — "host to network short"
    // Порт в памяти компьютера и порт в сети должны быть в сетевом порядке байт
    // 8080 превращаем в корректный сетевой формат
    server_addr.sin_port = htons(8080);

    // inet_pton() — переводит текстовый IP-адрес в бинарный вид
    // AF_INET означает IPv4.
    // "127.0.0.1" — адрес локальной машины.
    // Результат записывается в server_addr.sin_addr.
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP address\n";
        close(sock);
        return 1;
    }

    // connect() пытается установить соединение с сервером - он отправляет запрос серверу по адрессу server_addr
    // мы передаём:
    // - сам сокет
    // - адрес сервера, приведённый к sockaddr*
    // - размер структуры адреса
    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Connection to server failed\n";
        close(sock);
        return 1;
    }

    std::cout << "Connected\n";

    // oтправляем серверу приветствие
    // MSG_HELLO — тип сообщения
    // "Hello" — payload
    send_message(sock, MSG_HELLO, "Hello");

    // cоздаём объект для приёма сообщения от сервера
    Message msg;

    // recv_message() пытается получить одно полное сообщение
    // eсли оно пришло успешно и его тип — MSG_WELCOME,
    // значит сервер нас принял и ответил приветствием.
    if (recv_message(sock, msg) && msg.type == MSG_WELCOME) {
        std::cout << "Welcome " << msg.payload << "\n";
    }

    // этот флаг нужен сразу для двух потоков:
    // - главный поток читает ввод пользователя
    // - второй поток слушает сервер
    //
    // atomic нужен чтобы оба потока видели актуальное значение без гонок данных
    std::atomic<bool> is_running{true}; // МОЖНО просто = true

    // запускаем отдельный поток который будет постоянно читать сообщения от сервера
    // [&] означает взять по ссылке всё что нужно внутри лямбды - а это типо анонимная функция
    std::thread receiver_thread([&]() {
        // буфер куда будем складывать каждое новое сообщение от сервера
        Message incoming;

        // пока программа ещё работает и сервер продолжает присылать данные читаем сообщения одно за другим.
        while (is_running && recv_message(sock, incoming)) {
            // если сервер прислал PONG, значит он ответил на наш ping.
            if (incoming.type == MSG_PONG) {
                std::cout << "PONG\n> " << std::flush;
                // std::flush принудительно сбрасывает буфер вывода чтобы  >  появилось сразу а не когда-нибудь потом
            }
            // если сервер прислал обычный текст
            else if (incoming.type == MSG_TEXT) {
                std::cout << "\nServer says: " << incoming.payload << "\n> " << std::flush;
            }
        }

        // Если цикл закончился не потому, что мы сами выключили программу а потому что recv_message() перестал получать данные  значит сервер скорее всего закрыл соединение
        if (is_running) {
            std::cout << "\nConnection closed by server.\n";
            is_running = false;
        }
    });

    // хранилище строк вводимых пользвоателем
    std::string input;

    // показываем первый символ для ввода
    std::cout << "> " << std::flush;

    // читаем строки из консоли пока:
    // - программа ещё работает
    // - std::getline() успешно получает строку
    while (is_running && std::getline(std::cin, input)) {
        //например при нажатии enter
        if (input.empty()) {
            std::cout << "> " << std::flush;
            continue;
        }

        // ping — отправляем серверу служебное сообщение PING
        if (input == "/ping") {
            send_message(sock, MSG_PING);
        }
        // quit — отправляем серверу BYE и завершаем работу клиента
        else if (input == "/quit") {
            send_message(sock, MSG_BYE);
            is_running = false;
            break;
        }
        else {
            send_message(sock, MSG_TEXT, input);
        }
    }

    std::cout << "Disconnected\n";

    // на всякий ещё раз выключаем флаг 
    is_running = false;

    // shutdown(sock, SHUT_RDWR) говорит ядру:
    // - больше не хотим читать из сокета
    // - больше не хотим писать в сокет
    //
    // Это не то же самое, что close():
    // shutdown() "отрубает" обмен,
    // а close() уже полностью освобождает дескриптор.
    shutdown(sock, SHUT_RDWR);
    close(sock);
    // ждём завершения потока приёма сообщений
    // join() нужен чтобы программа не вышла раньше чем этот поток аккуратно завершится
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    return 0;
}
