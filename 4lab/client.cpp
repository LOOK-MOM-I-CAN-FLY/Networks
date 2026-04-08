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
            break;//выходим из цикла 
        }

        if (msg.type == MSG_TEXT) {
            std::cout << msg.payload << "\n";//обычное сообщение в чат
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << msg.payload << "\n";//приватное сообщение
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";//инфо от сервера
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[SERVER]: " << msg.payload << "\n";//ошибка от сервера
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG!\n";//ответ на пинг
        } else if (msg.type == MSG_WELCOME) {
            std::cout << "[SERVER]: " << msg.payload << "\n";//приветствие
        }
    }

    return nullptr;//завершаем поток
}

std::string read_nickname(const std::string& initial) {//читаем ник пользователя 
    std::string nickname = initial;//копируем начальное значение в локальную переменную
    while (nickname.empty()) {//пока ник пустой просим его ввести 
        std::cout << "Enter nickname: ";
        if (!std::getline(std::cin, nickname)) {//если ввод с консоли сломался или eof
            return "";
        }
        if (nickname.empty()) {//елси пользователь просто нажал ентер ругаемся 
            std::cout << "Nickname cannot be empty.\n";
        }
    }
    return nickname;//возвращаем нормальный ник 
}

int main(int argc, char* argv[]) {
    std::string nickname;
    if (argc > 1) {
        nickname = argv[1];//если ник передали при запуске берём его 
    }
    nickname = read_nickname(nickname);//если пустой просим ввести 
    if (nickname.empty()) {
        return 0;//если не удалось его получить выходим
    }

    while (true) {//бесконечно пытаемся подключиться к циклу
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);//создаём TCP/IP сокет с IPv4 
        if (sock < 0) {//проверяем создание сокета 
            std::perror("socket");
            return 1;
        }

        sockaddr_in server_addr{};//структура для адресса сервера
        server_addr.sin_family = AF_INET;//говорим что IPv4
        server_addr.sin_port = htons(SERVER_PORT);//задаём порт переводя его в сетевой порядок байт
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);//превращаем текстовый айпи в бинарный и кладём в структуру (говорим что к локал хост подключаемся)

        std::cout << "Connecting to server...\n";
        if (::connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {//пытаемся подключиться к серверу, приводим к типу который ожидает коннект
            std::cout << "Connection failed. Retrying in 2 seconds...\n";//обратаываем ошибку и пытаемся заново подключиться 
            ::close(sock);
            sleep(2);
            continue;
        }

        is_connected.store(true);//если дошли до сюда значит всё гуд и сообщаем об этом 
        std::cout << "Connected!\n";

        send_message(sock, MSG_HELLO, "My name is " + nickname);//отправляем привет серваку

        Message welcome_msg;//сюда ждём ответ от сервера 
        //то есть сервер обязан нам сначала ответить приветствием иначе идём назад
        if (!recv_message(sock, welcome_msg) || welcome_msg.type != MSG_WELCOME) {//ждём приветствие и проверяем тип 
            std::cout << "Handshake failed.\n";
            is_connected.store(false);
            ::close(sock);
            sleep(2);
            continue;
        }

        std::cout << "[SERVER]: " << welcome_msg.payload << "\n";//выводим приветствие от сервака
        send_message(sock, MSG_AUTH, nickname);//отправляем ник для авторизации

        pthread_t recv_tid{};//переменная для айди потока
        auto* args = new RecvThreadArgs{sock};//выделяем память и кладём туда айди потока
        pthread_create(&recv_tid, nullptr, receive_thread, args);//запускаем отдельный поток который слушает сервер
        //с этого момента у нас работает два потока: основной - ввод пользователя и фоновый - прослушивает сообщения от сервера 
        std::string input;//строка которую введёт пользователь
        bool quit_requested = false;//флаг - пользователь сам захотел выйти
        while (is_connected.load()) {//пока соединение живо читаем команды
            if (!std::getline(std::cin, input)) {//если ввод закончился или произошла ошибка то считаем что пользователь отключился и выходим из цикла вообще
                quit_requested = true;
                is_connected.store(false);
                break;
            }

            if (!is_connected.load()) {//отключили соединение значит выходим
                break;
            }

            if (input.empty()) {
                continue;//если просто пустота продолжаем
            }

            if (input == "/quit") {//если захотел выйти то отправляем серваку пока и выходим
                send_message(sock, MSG_BYE, "");
                quit_requested = true;//запоминаем что сообщение добровольное
                is_connected.store(false);
                break;
            }

            if (input == "/ping") {//если ввели пинг
                send_message(sock, MSG_PING, "");// то отправляем его серваку 
                continue;//идёи дальше ждать данные 
            }

            if (input.rfind("/w ", 0) == 0) {//если строка начинается с /w 
                std::string rest = input.substr(3);//то берём всё что идёт после '/w ' 
                while (!rest.empty() && rest[0] == ' ') {//пока впереди пробелы 
                    rest.erase(0, 1);//убирем их под одному
                }
                //ищем пробел между ником и сообщением
                std::size_t space = rest.find(' ');//ищем первый пробел
                if (space == std::string::npos) {//если пробела нет то показываем правильный формат
                    std::cout << "Usage: /w <nick> <message>\n";// команда должна быть такой: [/w Alice hello]
                    continue;
                }

                std::string target = rest.substr(0, space);//ник получателя 
                std::string message = rest.substr(space + 1);//само сообщение
                if (target.empty() || message.empty()) {//если ник или сообщение пустые 
                    std::cout << "Usage: /w <nick> <message>\n"; //то подсказывем формат
                    continue;
                }

                send_message(sock, MSG_PRIVATE, target + ":" + message);//отпрвляем приватное сообщение кому и что отправить (сервак это обрабатывает)
                //Bob:hello there - это уйдёт в payload
                continue;
            }

            send_message(sock, MSG_TEXT, input);//если это не команда то отправляем просто обычное сообщение 
        }

        ::shutdown(sock, SHUT_RDWR);//закрываем чтение и запись на сокете 
        ::close(sock);//закрываем сокет
        pthread_join(recv_tid, nullptr);//ждём завершения потока приёма сообщений

        if (quit_requested) { // если пользователь сам попросил выйти
            std::cout << "Exiting program.\n"; // пишем сообщение
            break; // выходим из бесконечного цикла while(true)
        }

        std::cout << "Reconnecting in 2 seconds...\n"; // сервер упал или связь оборвалась
        sleep(2); // ждём
    }

    return 0;
}
