#include <iostream>
#include <string>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "protocol.hpp"

std::atomic<bool> is_connected(false);//потокобезопасная переменная один поток может её менять а другой читать без риска ошибок синхронизации

struct RecvThreadArgs {
    int sock;//просто структура для передачи в поток дескр сокета 
};
//работа отдельного потока на стороне клиента, который постоянно слушает сообщения от сервера
void* receive_thread(void* arg) {
    auto* args = static_cast<RecvThreadArgs*>(arg);//приводим войд* к нашей структуре чтобы можно прочитать данные
    const int sock = args->sock;//копируем номер сокета в локальную переменную
    delete args;//освобождает память выделенную под структуру аргументов 

    Message msg;//сюда записываем сообщения 
    while (is_connected.load()) {//безопасно читаем значение
        if (!recv_message(sock, msg)) {//вызывем чтение функции сообщения, если получили фолз то ошибка 
            std::cout << "\n[Server disconnected]\n";
            is_connected.store(false);//меняем на отключение
            break;
        }
        //ну и просто вывод сообщения 
        if (msg.type == MSG_WELCOME) {
            std::cout << "[Server]: " << msg.payload << "\n";
        } else if (msg.type == MSG_TEXT) {
            std::cout << "[Broadcast]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "[Server]: PONG!\n";
        }
    }

    return nullptr;
}

int main(int argc, char* argv[]) {
    std::string nickname = "Student";//задаём имя новоиспечённому 
    if (argc > 1) nickname = argv[1];//если задано то присвоим его 

    while (true) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);//создаём розетку для связи 
        if (sock < 0) {
            std::perror("socket");//обработка ошибок
            return 1;
        }

        sockaddr_in server_addr{};//структура для адресса 
        server_addr.sin_family = AF_INET;//IPv4
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);//превращаем текстовый айпи в бинарный вид и закидываем в структуру

        std::cout << "Connecting to server...\n";
        if (::connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {//подключаемся и обрабатываем ошибку 
            std::cout << "Connection failed. Retrying in 2 seconds...\n";
            ::close(sock);
            sleep(2);
            continue;//пробуем снова прыгаем вначало while пробуем снова достучаться 
        }

        is_connected.store(true);//изменяем переменную что мы подключены 
        std::cout << "Connected!\n";

        send_message(sock, MSG_HELLO, "My name is " + nickname);//отправляем серваку привет и свой ник 

        pthread_t recv_tid{};//резервируем айди для потока 
        auto* args = new RecvThreadArgs{sock};//выделяем память на куче чтобы передать сокет потоку (тот самый args выше) 
        pthread_create(&recv_tid, nullptr, receive_thread, args);//запускаем поток который слушает сервак и ждёт вывода от польщзозвателя 

        std::string input;
        bool quit_requested = false;
        while (is_connected.load()) {//пока подключены крутим цикл чтения клваы 
            if (!std::getline(std::cin, input)) {//если вывод прерван то выходим 
                quit_requested = true;
                is_connected.store(false);
                break;
            }

            if (!is_connected.load()) break;//ну или если флаг подключения фолз то тоже выходим
            if (input.empty()) continue;//продолжаем если тихо всё

            if (input == "/quit") {//если пользовать сказал пока то уведомляем сервак и выставляем флаги 
                send_message(sock, MSG_BYE, "");
                quit_requested = true;
                is_connected.store(false);
                break;//выходим
            } else if (input == "/ping") {
                send_message(sock, MSG_PING, "");//иначе проверка связи пинго
            } else {
                send_message(sock, MSG_TEXT, nickname + ": " + input);// ну или в конце концов просто отправляем как обычный текст всё остальное 
            }
        }

        ::shutdown(sock, SHUT_RDWR);//принудительно обрываем чтение/запись на сокете
        ::close(sock);//закрываем дескр сокета
        pthread_join(recv_tid, nullptr);//ждём пока поток чтения реально закончит свою работу и мягко его отпускаем

        if (quit_requested) {//если мы сами вышли через /quit то полностью закрываем прогу  
            std::cout << "Exiting program.\n";
            break;
        }

        std::cout << "Reconnecting in 2 seconds...\n";
        sleep(2);//иначе идём на переподключение 
    }

    return 0;
}
