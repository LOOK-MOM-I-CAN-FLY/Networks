#include <atomic>      
#include <cstring>          
#include <iostream>      
#include <string>            
#include <arpa/inet.h>     
#include <pthread.h>       
#include <sys/socket.h>     
#include <sys/types.h>    
#include <unistd.h>         

#include "protocol.hpp"     

namespace {                  // анонимное пространство имен чтобы скрыть детали внутри файла

std::atomic<bool> is_connected(false);  // флаг показывает что соединение активно

struct RecvThreadArgs {      // параметры для потока приема
    int sock;                // сокет клиента
};                           // конец структуры

bool has_prefix(const std::string& s, const char* prefix) {  // проверка начинается ли строка с префикса
    const std::size_t n = std::strlen(prefix);               // длина префикса
    return s.size() >= n && s.compare(0, n, prefix) == 0;    // проверяем размер и совпадение начала строки
}

std::string strip_offline_prefix(const std::string& s) {  // убирает префикс offline если он есть
    if (!has_prefix(s, "OFFLINE:")) {                     // если префикса нет
        return s;                                          // возвращаем строку как есть
    }                                                      // конец условия
    std::string rest = s.substr(std::strlen("OFFLINE:"));  // берем часть после префикса
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {  // убираем пробелы в начале
        rest.erase(0, 1);                                  // удаляем первый символ
    }                                                      // конец цикла
    return rest;                                           // возвращаем очищенный текст
}

std::string format_text_line(const MessageEx& msg) {  // форматирует обычное сообщение для вывода
    return "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "][" +  // время и id
           std::string(msg.sender) + "]: " + std::string(msg.payload);                             // отправитель и текст
}

std::string format_private_line(const MessageEx& msg) {  // форматирует приватное сообщение
    const std::string payload = msg.payload;             // копируем payload в удобную строку
    const bool offline = has_prefix(payload, "OFFLINE:"); // проверяем было ли сообщение офлайн
    const std::string text = strip_offline_prefix(payload);  // убираем служебный префикс

    std::string line = "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "]";  // начало строки
    if (offline) {                                                                                           // если сообщение офлайн
        line += "[OFFLINE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text; // формат для офлайн
        return line;                                                                                         // возвращаем строку
    }                                                                                                        // конец условия
    line += "[PRIVATE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text;    // формат для обычного личного
    return line;                                                                                             // возвращаем строку
}

void* receive_thread(void* arg) {                // функция потока приема сообщений
    auto* args = static_cast<RecvThreadArgs*>(arg);  // приводим указатель к нужному типу
    const int sock = args->sock;                    // берем сокет из аргументов
    delete args;                                    // освобождаем память под аргументы

    MessageEx msg{};                                // структура для входящего сообщения
    while (is_connected.load()) {                   // работаем пока соединение активно
        if (!recv_message_ex(sock, msg)) {          // если сообщение не удалось принять
            std::cout << "\n[Server disconnected]\n"; // выводим что сервер отключился
            is_connected.store(false);              // помечаем соединение как закрытое
            break;                                  // выходим из цикла
        }                                           // конец условия

        if (msg.type == MSG_TEXT) {                 // обычный текст
            std::cout << format_text_line(msg) << "\n";  // выводим в понятном формате
        } else if (msg.type == MSG_PRIVATE) {       // приватное сообщение
            std::cout << format_private_line(msg) << "\n"; // выводим в понятном формате
        } else if (msg.type == MSG_HISTORY_DATA) {  // данные истории
            std::cout << msg.payload << "\n";       // выводим как есть
        } else if (msg.type == MSG_SERVER_INFO) {   // служебная информация сервера
            std::cout << "[SERVER]: " << msg.payload << "\n"; // выводим сообщение сервера
        } else if (msg.type == MSG_ERROR) {         // ошибка от сервера
            std::cout << "[SERVER]: " << msg.payload << "\n"; // показываем ошибку
        } else if (msg.type == MSG_PONG) {          // ответ на ping
            std::cout << "[SERVER]: PONG\n";        // подтверждаем ответ
        } else if (msg.type == MSG_WELCOME) {       // приветствие сервера
            std::cout << "[SERVER]: " << msg.payload << "\n"; // выводим приветствие
        }                                           // конец проверок типов
    }                                               // конец цикла приема

    return nullptr;                                 // завершаем поток
}

std::string read_nickname(const std::string& initial) {  // читает никнейм пользователя
    std::string nickname = initial;                     // берем начальное значение
    while (nickname.empty()) {                          // пока ник пустой
        std::cout << "Enter nickname: ";                // просим ввести ник
        if (!std::getline(std::cin, nickname)) {        // если ввод не удался
            return "";                                  // возвращаем пустую строку
        }                                               // конец проверки ввода
        if (nickname.empty()) {                         // если строка пустая
            std::cout << "Nickname cannot be empty.\n"; // сообщаем что ник пустой
        }                                               // конец условия
    }                                                   // конец цикла

    if (nickname.size() >= MAX_NAME) {                  // если ник слишком длинный
        nickname.resize(MAX_NAME - 1);                  // обрезаем до допустимого размера
    }                                                   // конец условия

    return nickname;                                    // возвращаем готовый ник
}

void print_help() {                                     // печатает список команд
    std::cout << "Available commands:\n";               // заголовок списка
    std::cout << "/help\n";                             // показать помощь
    std::cout << "/list\n";                             // список пользователей
    std::cout << "/history\n";                          // вся история
    std::cout << "/history N\n";                        // последние n сообщений
    std::cout << "/quit\n";                             // выход из клиента
    std::cout << "/w <nick> <message>\n";               // личное сообщение
    std::cout << "/ping\n";                             // проверка связи
    std::cout << "Tip: packets never sleep\n";          // шутка в подсказке
}

bool parse_history_cmd(const std::string& input, std::string& payload) {  // разбирает команду history
    payload.clear();                                  // очищаем выходной параметр
    if (input == "/history") {                        // если команда без аргументов
        return true;                                  // считаем ее корректной
    }                                                 // конец условия

    if (input.rfind("/history ", 0) != 0) {           // если строка не начинается с /history
        return false;                                 // команда неверная
    }                                                 // конец условия

    std::string rest = input.substr(std::strlen("/history "));  // берем часть после команды
    while (!rest.empty() && rest[0] == ' ') {        // убираем пробелы слева
        rest.erase(0, 1);                            // удаляем первый символ
    }                                                // конец цикла

    if (rest.empty()) {                              // если после команды ничего нет
        return true;                                 // это тоже допустимо
    }                                                // конец условия

    for (char ch : rest) {                           // проверяем все символы
        if (ch < '0' || ch > '9') {                  // если встретили не цифру
            return false;                            // команда неверная
        }                                            // конец условия
    }                                                // конец цикла

    if (rest == "0") {                               // ноль не подходит
        return false;                                // считаем команду неверной
    }                                                // конец условия

    payload = rest;                                  // кладем число в payload
    return true;                                     // команда распознана
}

bool parse_whisper_cmd(const std::string& input, std::string& nick, std::string& message) {  // разбирает приватное сообщение
    if (input.rfind("/w ", 0) != 0) {               // если команда не начинается с /w
        return false;                               // это не whisper
    }                                               // конец условия

    std::string rest = input.substr(3);             // берем все после /w
    while (!rest.empty() && rest[0] == ' ') {       // убираем лишние пробелы
        rest.erase(0, 1);                           // удаляем первый символ
    }                                               // конец цикла

    std::size_t space = rest.find(' ');             // ищем пробел между ником и сообщением
    if (space == std::string::npos) {               // если пробела нет
        return false;                               // команда неполная
    }                                               // конец условия

    nick = rest.substr(0, space);                   // получаем ник получателя
    message = rest.substr(space + 1);               // получаем текст сообщения
    return !(nick.empty() || message.empty());      // проверяем что обе части не пустые
}

}  // namespace                                   // конец анонимного пространства имен

int main(int argc, char* argv[]) {                             // точка входа в программу
    std::string nickname;                                      // никнейм пользователя
    if (argc > 1) {                                            // если ник передан через аргументы
        nickname = argv[1];                                    // берем его из argv
    }                                                          // конец условия
    nickname = read_nickname(nickname);                        // читаем ник или просим ввести
    if (nickname.empty()) {                                    // если ник так и не получен
        return 0;                                              // завершаем программу
    }                                                          // конец условия

    while (true) {                                             // основной цикл переподключения
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);           // создаем tcp сокет
        if (sock < 0) {                                        // если сокет не создался
            std::perror("socket");                             // выводим ошибку
            return 1;                                          // выходим с кодом ошибки
        }                                                      // конец условия

        sockaddr_in server_addr{};                             // структура адреса сервера
        server_addr.sin_family = AF_INET;                      // семейство адресов ipv4
        server_addr.sin_port = htons(SERVER_PORT);             // порт в сетевом порядке
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); // записываем адрес сервера

        std::cout << "Connecting to server...\n";              // сообщаем о подключении
        if (::connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {  // пробуем соединиться
            std::cout << "Connection failed. Retrying in 2 seconds...\n"; // сообщаем о неудаче
            ::close(sock);                                     // закрываем сокет
            sleep(2);                                          // ждем перед новой попыткой
            continue;                                          // повторяем цикл
        }                                                      // конец условия

        is_connected.store(true);                              // соединение активно
        std::cout << "Connected!\n";                           // сообщаем об успехе

        (void)send_message_ex(sock, MSG_HELLO, 0, nickname, "", std::time(nullptr), "My name is " + nickname); // отправляем приветствие

        MessageEx welcome{};                                   // структура для ответа приветствия
        if (!recv_message_ex(sock, welcome) || welcome.type != MSG_WELCOME) {  // ждем welcome от сервера
            std::cout << "Handshake failed.\n";                // сообщаем об ошибке рукопожатия
            is_connected.store(false);                         // сбрасываем флаг связи
            ::close(sock);                                     // закрываем сокет
            sleep(2);                                          // ждем перед повтором
            continue;                                          // идем на новую попытку
        }                                                      // конец условия

        std::cout << "[SERVER]: " << welcome.payload << "\n";  // печатаем приветствие сервера
        (void)send_message_ex(sock, MSG_AUTH, 0, nickname, "", std::time(nullptr), nickname); // отправляем авторизацию

        pthread_t recv_tid{};                                  // идентификатор потока приема
        auto* args = new RecvThreadArgs{sock};                 // создаем аргументы для потока
        pthread_create(&recv_tid, nullptr, receive_thread, args); // запускаем поток приема

        std::string input;                                     // строка ввода пользователя
        bool quit_requested = false;                           // флаг выхода из программы
        while (is_connected.load()) {                          // пока соединение активно
            if (!std::getline(std::cin, input)) {              // читаем строку из консоли
                quit_requested = true;                         // выходим из программы
                is_connected.store(false);                      // отключаем флаг соединения
                break;                                          // выходим из цикла
            }                                                  // конец условия

            if (!is_connected.load()) {                        // если соединение уже потеряно
                break;                                          // прекращаем ввод
            }                                                  // конец условия

            if (input.empty()) {                               // если строка пустая
                continue;                                      // просто ждем следующую
            }                                                  // конец условия

            if (input == "/help") {                            // команда помощи
                print_help();                                  // печатаем список команд
                continue;                                      // переходим к следующему вводу
            }                                                  // конец условия

            if (input == "/quit") {                            // команда выхода
                (void)send_message_ex(sock, MSG_BYE, 0, nickname, "", std::time(nullptr), ""); // отправляем bye
                quit_requested = true;                         // запоминаем что это выход
                is_connected.store(false);                     // закрываем цикл работы
                break;                                         // выходим из цикла
            }                                                  // конец условия

            if (input == "/ping") {                            // команда проверки связи
                (void)send_message_ex(sock, MSG_PING, 0, nickname, "", std::time(nullptr), ""); // отправляем ping
                continue;                                      // ждем дальше
            }                                                  // конец условия

            if (input == "/list") {                            // команда списка пользователей
                (void)send_message_ex(sock, MSG_LIST, 0, nickname, "", std::time(nullptr), ""); // отправляем list
                continue;                                      // переходим к следующей строке
            }                                                  // конец условия

            std::string hist_payload;                          // строка для параметра history
            if (input == "/history" || input.rfind("/history ", 0) == 0) {  // если это history
                if (!parse_history_cmd(input, hist_payload)) { // если команда написана неверно
                    std::cout << "Usage: /history or /history N\n"; // показываем правильный формат
                    continue;                                  // просим повторить ввод
                }                                              // конец условия
                (void)send_message_ex(sock, MSG_HISTORY, 0, nickname, "", std::time(nullptr), hist_payload); // отправляем history
                continue;                                      // переходим к следующей строке
            }                                                  // конец условия

            std::string target;                                // ник получателя
            std::string msg_text;                              // текст личного сообщения
            if (parse_whisper_cmd(input, target, msg_text)) {  // если это команда личного сообщения
                if (target.size() >= MAX_NAME) {               // если ник слишком длинный
                    target.resize(MAX_NAME - 1);               // обрезаем его
                }                                              // конец условия
                (void)send_message_ex(sock, MSG_PRIVATE, 0, nickname, target, std::time(nullptr), msg_text); // отправляем private
                continue;                                      // идем дальше
            }                                                  // конец условия

            (void)send_message_ex(sock, MSG_TEXT, 0, nickname, "", std::time(nullptr), input); // отправляем обычный текст
        }                                                      // конец цикла ввода

        ::shutdown(sock, SHUT_RDWR);                           // выключаем чтение и запись
        ::close(sock);                                         // закрываем сокет
        pthread_join(recv_tid, nullptr);                       // ждем завершения потока приема

        if (quit_requested) {                                  // если пользователь вышел сам
            std::cout << "Exiting program.\n";                 // сообщаем о завершении
            break;                                             // выходим из основного цикла
        }                                                      // конец условия

        std::cout << "Reconnecting in 2 seconds...\n";         // сообщаем о переподключении
        sleep(2);                                              // ждем перед новой попыткой
    }                                                          // конец основного цикла

    return 0;                                                  // успешное завершение
}
