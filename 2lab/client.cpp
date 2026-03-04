#include "protocol.hpp"
// Подключаю заголовок protocol.hpp.
// В нём объявлены Message (type, payload), функции send_message/recv_message и константы MSG_*.
// Нужен, чтобы компилятор знал, что такое Message и как работать с сообщениями.

#include <thread>
// Подключаю <thread> для работы с потоками (std::thread).

#include <atomic>
// Подключаю <atomic> для атомарных типов (std::atomic) — безопасный доступ из разных потоков.

struct NetworkInitializer {
    NetworkInitializer() {
#ifdef _WIN32
        WSADATA wsaData;
        // WSADATA — структура WinSock, куда WSAStartup запишет информацию о версии.
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            // std::cerr — поток для ошибок.
            std::cerr << "WSAStartup failed\n";
            // exit(1) — аварийно завершаю программу с кодом ошибки.
            exit(1);
        }
#endif
    }

    ~NetworkInitializer() {
#ifdef _WIN32
        // WSACleanup — освобождаю ресурсы WinSock.
        WSACleanup();
#endif
    }
};

// main — точка входа программы. Возвращаю int (0 = успешно).
int main() {
    // Создаю объект для инициализации сети (на Windows конструктор вызовет WSAStartup).
    NetworkInitializer netInit;

    // Создаю сокет: AF_INET = IPv4, SOCK_STREAM = TCP, 0 = протокол по умолчанию (TCP).
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    // Настраиваю адрес сервера (IPv4 + порт + IP).
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;                 // IPv4
    server_addr.sin_port = htons(8080);               // порт в сетевом порядке байтов
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); // перевод IP из текста в бинарный вид

    // Подключаюсь к серверу. connect возвращает <0 при ошибке.
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection to server failed\n";
        // Закрываю сокет (на Windows closesocket, на POSIX — close).
        closesocket(sock);
        return 1;
    }

    std::cout << "Connected\n";
    // Отправляю приветственное сообщение типа MSG_HELLO с payload "Hello".
    send_message(sock, MSG_HELLO, "Hello");

    // Жду первого сообщения от сервера в msg.
    Message msg;
    if (recv_message(sock, msg) && msg.type == MSG_WELCOME) {
        // Если получили MSG_WELCOME, вывожу payload.
        std::cout << "Welcome " << msg.payload << "\n";
    }

    // Флаг работы программы, безопасно читаемый/пишемый из разных потоков.
    std::atomic<bool> is_running{true};

    // Запускаю отдельный поток receiver_thread для приёма сообщений от сервера.
    // Захватываю внешние переменные по ссылке (&).
    std::thread receiver_thread([&]() {
        Message incoming;
        // Пока флаг true и приходят сообщения, обрабатываю их.
        while (is_running && recv_message(sock, incoming)) {
            if (incoming.type == MSG_PONG) {
                // Обрабатываю PONG — вывожу и показываю приглашение.
                std::cout << "PONG\n> " << std::flush;
            } 
            else if (incoming.type == MSG_TEXT) {
                // Обрабатываю текст от сервера.
                std::cout << "\nServer says: " << incoming.payload << "\n> " << std::flush;
            }
        }
        // Если цикл прервался не мной (is_running всё ещё true), значит сервер закрыл соединение.
        if (is_running) {
            std::cout << "\nConnection closed by server.\n";
            is_running = false;
        }
    });

    // Буфер для пользовательского ввода.
    std::string input;
    std::cout << "> ";
    // Читаю строки из stdin до EOF или пока is_running true.
    while (is_running && std::getline(std::cin, input)) {
        if (input.empty()) {
            // Если пустая строка — просто показываю приглашение и жду дальше.
            std::cout << "> ";
            continue;
        }

        if (input == "/ping") {
            // Команда /ping — отправляю MSG_PING.
            send_message(sock, MSG_PING);
        } 
        else if (input == "/quit") {
            // Команда /quit — отправляю MSG_BYE и сигнализирую о завершении.
            send_message(sock, MSG_BYE);
            is_running = false;
            break;
        } 
        else {
            // Любой другой ввод трактую как текст и отправляю MSG_TEXT.
            send_message(sock, MSG_TEXT, input);
        }
    }

    std::cout << "Disconnected\n";
    // Гарантирую сброс флага работы.
    is_running = false;
    
    // Отключаю обе стороны сокета (нет чтения и записи).
    shutdown(sock, SHUT_RDWR); 
    // Закрываю сокет/дескриптор.
    closesocket(sock);
    
    // Жду завершения приёмного потока, если он ещё работает.
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }

    // Возврат 0 — успешно.
    return 0;
}
