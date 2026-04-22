#include <algorithm>      
#include <cerrno>             
#include <cstdio>           
#include <cstring>           
#include <fstream>          
#include <iostream>          
#include <queue>             
#include <sstream>         
#include <string>            
#include <unordered_map>     
#include <vector>             

#include <arpa/inet.h>         
#include <netinet/in.h>         // структуры адресов сети
#include <pthread.h>            // потоки pthread
#include <sys/socket.h>         // сокеты
#include <sys/types.h>          // системные типы
#include <unistd.h>             // close и другие системные вызовы

#include "protocol.hpp"         // общий протокол обмена сообщениями

namespace {                     // анонимное пространство имен чтобы скрыть внутренние детали

constexpr int THREAD_POOL_SIZE = 10;                  // размер пула потоков
constexpr std::size_t NICKNAME_MAX_LEN = MAX_NAME - 1; // максимальная длина ника без нуля
constexpr int HISTORY_DEFAULT_N = 10;                 // сколько сообщений истории отдавать по умолчанию
constexpr const char* HISTORY_FILE = "history.json";   // файл для хранения истории

struct Client {                       // информация об одном клиенте
    int sock;                         // сокет клиента
    char nickname[MAX_NAME];          // никнейм клиента
    bool authenticated;               // прошел ли клиент авторизацию
};                                    // конец структуры клиента

struct HistoryRecord {                // запись в истории сообщений
    std::uint32_t msg_id = 0;         // идентификатор сообщения
    std::int64_t timestamp = 0;       // время отправки
    std::string sender;              // отправитель
    std::string receiver;            // получатель
    std::uint8_t type = 0;           // тип сообщения
    std::string text;                // текст сообщения
    bool delivered = true;           // доставлено ли сообщение
    bool is_offline = false;         // было ли сообщение офлайн
};                                   // конец записи истории

struct OfflineMsg {                  // сообщение которое ждет доставку
    std::uint32_t msg_id = 0;        // id сообщения
    std::int64_t timestamp = 0;      // время отправки
    std::string sender;             // отправитель
    std::string receiver;           // получатель
    std::string text;               // текст сообщения
};                                  // конец структуры offline сообщения

std::queue<int> client_queue;                            // очередь сокетов клиентов
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex для очереди
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;    // условная переменная для очереди

std::vector<Client> active_clients;                      // список активных клиентов
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex для списка клиентов

std::unordered_map<std::string, std::vector<OfflineMsg>> offline_queue; // очередь офлайн сообщений по никам
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex для офлайн очереди

std::vector<HistoryRecord> history_records;              // все записи истории
pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex для истории

pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;     // mutex для генерации id
std::uint32_t next_msg_id = 1;                            // следующий id сообщения

std::string ip_of(const sockaddr_in& addr) {             // получает ip адрес как строку
    char buf[INET_ADDRSTRLEN] = {};                      // буфер для ip строки
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)); // перевод ip в текст
    return std::string(buf);                             // возвращаем строку
}

std::string ipport_of(const sockaddr_in& addr) {         // получает ip и порт как строку
    return ip_of(addr) + ":" + std::to_string(ntohs(addr.sin_port)); // ip плюс порт
}

bool get_endpoints(int sock, sockaddr_in& local, sockaddr_in& peer) { // получает локальный и удаленный адреса
    socklen_t local_len = sizeof(local);                // размер локального адреса
    socklen_t peer_len = sizeof(peer);                  // размер удаленного адреса
    std::memset(&local, 0, sizeof(local));              // очищаем структуру локального адреса
    std::memset(&peer, 0, sizeof(peer));                // очищаем структуру удаленного адреса
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &local_len) < 0) { // получаем локальный адрес
        return false;                                   // если ошибка возвращаем false
    }
    if (::getpeername(sock, reinterpret_cast<sockaddr*>(&peer), &peer_len) < 0) {   // получаем адрес клиента
        return false;                                   // если ошибка возвращаем false
    }
    return true;                                        // адреса успешно получены
}

void log_incoming(int sock, const MessageEx& msg) {       // логирует входящее сообщение
    sockaddr_in local{};                                // локальный адрес
    sockaddr_in peer{};                                 // адрес клиента
    (void)get_endpoints(sock, local, peer);             // пытаемся получить адреса

    std::size_t bytes = sizeof(MessageExWireHeader) + msg.length; // считаем сколько байт пришло
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP\n"; // лог транспортного уровня
    std::cout << "[Internet] src=" << ip_of(peer) << " dst=" << ip_of(local) << " proto=TCP\n"; // лог ip
    std::cout << "[Network Access] frame received via network interface\n"; // лог канального уровня
    std::cout << "[Application] deserialize MessageEx -> " << message_type_name(msg.type) << "\n"; // лог приложения
}

void log_outgoing(int sock, std::uint8_t type, std::size_t payload_len, const std::string& action) { // логирует отправку
    sockaddr_in local{};                                // локальный адрес
    sockaddr_in peer{};                                 // адрес получателя
    (void)get_endpoints(sock, local, peer);             // получаем адреса сокета

    std::size_t bytes = sizeof(MessageExWireHeader) + payload_len; // считаем размер пакета
    std::cout << "[Application] " << action << " -> " << message_type_name(type) << "\n"; // лог действия
    std::cout << "[Transport] send() " << bytes << " bytes via TCP\n"; // лог передачи
    std::cout << "[Internet] src=" << ip_of(local) << " dst=" << ip_of(peer) << " proto=TCP\n"; // лог ip
    std::cout << "[Network Access] frame sent to network interface\n"; // лог канального уровня
}

bool recv_message_tcpip(int sock, MessageEx& msg) {       // принимает сообщение и сразу логирует его
    if (!recv_message_ex(sock, msg)) {                  // если чтение не удалось
        return false;                                   // возвращаем false
    }
    log_incoming(sock, msg);                            // печатаем логи
    return true;                                         // сообщение принято
}

bool send_message_tcpip(                                  // отправляет сообщение и логирует отправку
    int sock,
    std::uint8_t type,
    std::uint32_t msg_id,
    const std::string& sender,
    const std::string& receiver,
    std::int64_t timestamp,
    const std::string& payload,
    const std::string& action) {

    std::size_t payload_len = std::min<std::size_t>(payload.size(), MAX_PAYLOAD); // ограничиваем размер
    log_outgoing(sock, type, payload_len, action);        // логируем перед отправкой
    return send_message_ex(sock, type, msg_id, sender, receiver, timestamp, payload); // отправляем сообщение
}

std::vector<int> snapshot_client_sockets() {              // делает копию списка сокетов клиентов
    std::vector<int> sockets;                             // результирующий список
    pthread_mutex_lock(&clients_mutex);                   // блокируем доступ к клиентам
    sockets.reserve(active_clients.size());               // заранее выделяем память
    for (const auto& c : active_clients) {                // проходим по всем клиентам
        sockets.push_back(c.sock);                        // сохраняем их сокеты
    }
    pthread_mutex_unlock(&clients_mutex);                 // освобождаем mutex
    return sockets;                                       // возвращаем копию списка
}

bool nickname_exists(const std::string& nickname) {        // проверяет занят ли ник
    pthread_mutex_lock(&clients_mutex);                    // блокируем список клиентов
    for (const auto& c : active_clients) {                 // ищем по всем клиентам
        if (nickname == c.nickname) {                     // если ник совпал
            pthread_mutex_unlock(&clients_mutex);         // освобождаем mutex
            return true;                                  // ник занят
        }
    }
    pthread_mutex_unlock(&clients_mutex);                 // освобождаем mutex
    return false;                                         // ник свободен
}

bool get_sock_by_nickname(const std::string& nickname, int& sock_out) { // ищет сокет по нику
    pthread_mutex_lock(&clients_mutex);                    // блокируем список клиентов
    for (const auto& c : active_clients) {                 // перебираем клиентов
        if (nickname == c.nickname) {                     // если ник совпал
            sock_out = c.sock;                            // сохраняем сокет
            pthread_mutex_unlock(&clients_mutex);         // освобождаем mutex
            return true;                                  // нашли клиента
        }
    }
    pthread_mutex_unlock(&clients_mutex);                 // освобождаем mutex
    return false;                                         // клиента нет
}

bool get_nickname_by_sock(int sock, std::string& nickname) { // ищет ник по сокету
    pthread_mutex_lock(&clients_mutex);                    // блокируем список клиентов
    for (const auto& c : active_clients) {                 // перебираем клиентов
        if (c.sock == sock) {                             // если сокет совпал
            nickname = c.nickname;                        // сохраняем ник
            pthread_mutex_unlock(&clients_mutex);         // освобождаем mutex
            return true;                                  // нашли
        }
    }
    pthread_mutex_unlock(&clients_mutex);                 // освобождаем mutex
    return false;                                         // не нашли
}

void add_client(int sock, const std::string& nickname) {   // добавляет клиента в список
    Client c{};                                           // создаем запись клиента
    c.sock = sock;                                        // сохраняем сокет
    std::memset(c.nickname, 0, sizeof(c.nickname));       // очищаем буфер ника
    std::strncpy(c.nickname, nickname.c_str(), sizeof(c.nickname) - 1); // копируем ник
    c.authenticated = true;                               // помечаем как авторизованного

    pthread_mutex_lock(&clients_mutex);                   // блокируем список клиентов
    active_clients.push_back(c);                          // добавляем клиента
    pthread_mutex_unlock(&clients_mutex);                 // освобождаем mutex
}

std::string escape_json_string(const std::string& s) {     // экранирует строку для json
    std::string out;                                      // результат
    out.reserve(s.size() + 8);                            // немного запасаем память
    for (char ch : s) {                                   // идем по всем символам
        switch (ch) {                                     // проверяем символ
            case '\\': out += "\\\\"; break;             // экранируем обратный слеш
            case '"': out += "\\\""; break;              // экранируем кавычку
            case '\n': out += "\\n"; break;              // экранируем перевод строки
            case '\r': out += "\\r"; break;              // экранируем возврат каретки
            case '\t': out += "\\t"; break;              // экранируем таб
            default:
                if (static_cast<unsigned char>(ch) < 0x20) { // если служебный символ
                    out += ' ';                           // заменяем пробелом
                } else {
                    out += ch;                            // иначе копируем как есть
                }
                break;
        }
    }
    return out;                                           // возвращаем экранированную строку
}

bool write_history_file_locked(const std::string& path) {  // записывает историю в файл, mutex уже должен быть взят
    std::ostringstream oss;                               // буфер для json текста
    oss << "[\n";                                         // открываем массив
    for (std::size_t i = 0; i < history_records.size(); ++i) { // идем по всем записям
        const auto& r = history_records[i];               // берем текущую запись
        oss << "  {\n";                                   // открываем объект
        oss << "    \"msg_id\": " << r.msg_id << ",\n";   // id сообщения
        oss << "    \"timestamp\": " << r.timestamp << ",\n"; // время
        oss << "    \"sender\": \"" << escape_json_string(r.sender) << "\",\n"; // отправитель
        oss << "    \"receiver\": \"" << escape_json_string(r.receiver) << "\",\n"; // получатель
        oss << "    \"type\": \"" << message_type_name(r.type) << "\",\n"; // тип
        oss << "    \"text\": \"" << escape_json_string(r.text) << "\",\n"; // текст
        oss << "    \"delivered\": " << (r.delivered ? "true" : "false") << ",\n"; // доставлено ли
        oss << "    \"is_offline\": " << (r.is_offline ? "true" : "false") << "\n"; // офлайн ли
        oss << "  }" << (i + 1 < history_records.size() ? "," : "") << "\n"; // запятая между объектами
    }
    oss << "]\n";                                         // закрываем массив

    const std::string tmp = path + ".tmp";                 // временный файл
    {
        std::ofstream out(tmp.c_str(), std::ios::out | std::ios::trunc); // открываем временный файл
        if (!out) {                                       // если не удалось открыть
            return false;                                 // ошибка записи
        }
        out << oss.str();                                 // пишем весь json
        if (!out.good()) {                                // проверяем состояние потока
            return false;                                 // ошибка записи
        }
    }
    std::remove(path.c_str());                            // удаляем старый файл
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {    // переименовываем временный файл
        return false;                                     // если не вышло возвращаем false
    }
    return true;                                          // запись успешна
}

bool parse_json_string(const std::string& s, std::size_t& pos, std::string& out) { // читает json строку
    if (pos >= s.size() || s[pos] != '"') {                // если строка не начинается с кавычки
        return false;                                     // это не строка json
    }
    ++pos;                                                // пропускаем открывающую кавычку
    out.clear();                                          // очищаем результат
    while (pos < s.size()) {                               // идем по символам
        char ch = s[pos++];                               // берем текущий символ
        if (ch == '"') {                                  // если нашли закрывающую кавычку
            return true;                                  // строка успешно прочитана
        }
        if (ch == '\\') {                                 // если это escape последовательность
            if (pos >= s.size()) {                        // если строка закончилась слишком рано
                return false;                             // ошибка парсинга
            }
            char esc = s[pos++];                          // читаем экранированный символ
            switch (esc) {                                // разбираем escape
                case '"': out.push_back('"'); break;      // кавычка
                case '\\': out.push_back('\\'); break;    // слеш
                case 'n': out.push_back('\n'); break;     // новая строка
                case 'r': out.push_back('\r'); break;     // возврат каретки
                case 't': out.push_back('\t'); break;     // таб
                default: out.push_back(esc); break;       // неизвестный символ просто добавляем
            }
            continue;                                     // переходим к следующему символу
        }
        out.push_back(ch);                                // обычный символ добавляем в результат
    }
    return false;                                         // если дошли сюда строка не закрыта
}

void skip_ws(const std::string& s, std::size_t& pos) {     // пропускает пробельные символы
    while (pos < s.size()) {                              // пока не конец строки
        char ch = s[pos];                                 // берем текущий символ
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') { // если пробел или перенос
            ++pos;                                        // двигаем позицию
            continue;                                     // продолжаем
        }
        break;                                            // если не пробел выходим
    }
}

bool find_key_pos(const std::string& obj, const std::string& key, std::size_t& pos) { // ищет ключ в объекте json
    const std::string needle = "\"" + key + "\"";         // формируем строку поиска
    std::size_t k = obj.find(needle);                     // ищем ключ
    if (k == std::string::npos) {                         // если не найден
        return false;                                     // ошибка
    }
    std::size_t colon = obj.find(':', k + needle.size()); // ищем двоеточие после ключа
    if (colon == std::string::npos) {                     // если не нашли двоеточие
        return false;                                     // ошибка
    }
    pos = colon + 1;                                      // позиция сразу после двоеточия
    return true;                                          // ключ найден
}

bool parse_uint64(const std::string& s, std::size_t& pos, std::uint64_t& out) { // читает беззнаковое число
    skip_ws(s, pos);                                      // пропускаем пробелы
    if (pos >= s.size() || (s[pos] < '0' || s[pos] > '9')) { // если нет цифры
        return false;                                     // ошибка
    }
    std::uint64_t v = 0;                                  // накопитель числа
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { // пока цифры идут подряд
        v = v * 10 + static_cast<std::uint64_t>(s[pos] - '0'); // собираем число
        ++pos;                                            // двигаем позицию
    }
    out = v;                                              // сохраняем результат
    return true;                                          // число прочитано
}

bool parse_int64(const std::string& s, std::size_t& pos, std::int64_t& out) { // читает знаковое число
    skip_ws(s, pos);                                      // пропускаем пробелы
    bool neg = false;                                     // флаг минуса
    if (pos < s.size() && s[pos] == '-') {                // если число отрицательное
        neg = true;                                       // запоминаем минус
        ++pos;                                            // пропускаем знак
    }
    std::uint64_t v = 0;                                  // читаем модуль числа
    if (!parse_uint64(s, pos, v)) {                       // если не удалось прочитать число
        return false;                                     // ошибка
    }
    out = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v); // применяем знак
    return true;                                          // число прочитано
}

bool parse_bool(const std::string& s, std::size_t& pos, bool& out) { // читает true или false
    skip_ws(s, pos);                                      // пропускаем пробелы
    if (s.compare(pos, 4, "true") == 0) {                 // если true
        pos += 4;                                         // двигаем позицию
        out = true;                                       // сохраняем true
        return true;                                      // успешно
    }
    if (s.compare(pos, 5, "false") == 0) {                // если false
        pos += 5;                                         // двигаем позицию
        out = false;                                      // сохраняем false
        return true;                                      // успешно
    }
    return false;                                         // другое значение не подходит
}

std::uint8_t type_from_name(const std::string& name) {     // переводит имя типа в код
    if (name == "MSG_TEXT") return MSG_TEXT;              // обычное сообщение
    if (name == "MSG_PRIVATE") return MSG_PRIVATE;        // приватное сообщение
    if (name == "MSG_SERVER_INFO") return MSG_SERVER_INFO; // сообщение сервера
    if (name == "MSG_ERROR") return MSG_ERROR;            // ошибка
    if (name == "MSG_HISTORY_DATA") return MSG_HISTORY_DATA; // строка истории
    return 0;                                             // неизвестный тип
}

bool parse_history_object(const std::string& obj, HistoryRecord& rec) { // парсит один объект истории
    std::size_t pos = 0;                                  // текущая позиция
    std::uint64_t u64 = 0;                                // временная переменная для чисел
    std::int64_t i64 = 0;                                 // временная переменная для знаковых чисел
    std::string s;                                        // временная строка

    if (!find_key_pos(obj, "msg_id", pos) || !parse_uint64(obj, pos, u64)) { // читаем msg_id
        return false;                                     // если не вышло ошибка
    }
    rec.msg_id = static_cast<std::uint32_t>(u64);         // сохраняем id

    if (!find_key_pos(obj, "timestamp", pos) || !parse_int64(obj, pos, i64)) { // читаем время
        return false;                                     // если не вышло ошибка
    }
    rec.timestamp = i64;                                  // сохраняем timestamp

    if (!find_key_pos(obj, "sender", pos)) return false;  // ищем sender
    skip_ws(obj, pos);                                    // пропускаем пробелы
    if (!parse_json_string(obj, pos, s)) return false;    // читаем строку
    rec.sender = s;                                       // сохраняем отправителя

    if (!find_key_pos(obj, "receiver", pos)) return false; // ищем receiver
    skip_ws(obj, pos);                                    // пропускаем пробелы
    if (!parse_json_string(obj, pos, s)) return false;    // читаем строку
    rec.receiver = s;                                     // сохраняем получателя

    if (!find_key_pos(obj, "type", pos)) return false;    // ищем type
    skip_ws(obj, pos);                                    // пропускаем пробелы
    if (!parse_json_string(obj, pos, s)) return false;    // читаем строку типа
    rec.type = type_from_name(s);                         // переводим в код

    if (!find_key_pos(obj, "text", pos)) return false;    // ищем text
    skip_ws(obj, pos);                                    // пропускаем пробелы
    if (!parse_json_string(obj, pos, s)) return false;    // читаем текст
    rec.text = s;                                         // сохраняем текст

    if (!find_key_pos(obj, "delivered", pos) || !parse_bool(obj, pos, rec.delivered)) { // delivered
        return false;                                     // если не вышло ошибка
    }
    if (!find_key_pos(obj, "is_offline", pos) || !parse_bool(obj, pos, rec.is_offline)) { // is_offline
        return false;                                     // если не вышло ошибка
    }
    if (rec.type == 0) {                                  // если тип не распознан
        rec.type = MSG_TEXT;                              // ставим обычный текст
    }
    return true;                                          // объект распознан
}

bool load_history_file(const std::string& path) {          // загружает историю из файла
    std::ifstream in(path.c_str(), std::ios::in);          // открываем файл
    if (!in) {                                             // если файла нет
        return true;                                       // это не ошибка
    }
    std::ostringstream oss;                                // буфер для чтения всего файла
    oss << in.rdbuf();                                     // читаем файл целиком
    std::string data = oss.str();                          // получаем строку

    std::vector<HistoryRecord> loaded;                     // сюда складываем прочитанные записи
    std::size_t pos = 0;                                   // позиция в тексте
    while (true) {                                         // ищем объекты один за другим
        std::size_t start = data.find('{', pos);           // ищем начало объекта
        if (start == std::string::npos) {                  // если объектов больше нет
            break;                                         // выходим
        }
        std::size_t end = data.find('}', start);           // ищем конец объекта
        if (end == std::string::npos) {                    // если объект не закрыт
            break;                                         // прекращаем разбор
        }
        HistoryRecord rec;                                 // временная запись
        if (parse_history_object(data.substr(start, end - start + 1), rec)) { // если объект распознан
            loaded.push_back(rec);                         // сохраняем запись
        }
        pos = end + 1;                                     // идем дальше
    }

    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    history_records = std::move(loaded);                   // заменяем содержимое
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex
    return true;                                           // загрузка завершена
}

void rebuild_offline_queue_from_history() {                // восстанавливает офлайн очередь из истории
    pthread_mutex_lock(&offline_mutex);                    // блокируем офлайн очередь
    offline_queue.clear();                                 // очищаем ее
    pthread_mutex_unlock(&offline_mutex);                  // освобождаем mutex

    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    for (const auto& r : history_records) {                // идем по всем записям
        if (r.is_offline && !r.delivered && !r.receiver.empty()) { // если сообщение офлайн и не доставлено
            OfflineMsg om;                                 // создаем запись офлайн сообщения
            om.msg_id = r.msg_id;                          // копируем id
            om.timestamp = r.timestamp;                    // копируем время
            om.sender = r.sender;                          // копируем отправителя
            om.receiver = r.receiver;                      // копируем получателя
            om.text = r.text;                               // копируем текст

            pthread_mutex_lock(&offline_mutex);            // блокируем офлайн очередь
            offline_queue[om.receiver].push_back(om);       // кладем сообщение в очередь получателя
            pthread_mutex_unlock(&offline_mutex);          // освобождаем mutex
        }
    }
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex истории

    pthread_mutex_lock(&offline_mutex);                    // снова блокируем офлайн очередь
    for (auto& kv : offline_queue) {                       // идем по всем получателям
        auto& vec = kv.second;                             // берем их список сообщений
        std::sort(vec.begin(), vec.end(), [](const OfflineMsg& a, const OfflineMsg& b) { // сортируем по id
            return a.msg_id < b.msg_id;                   // более старые выше
        });
    }
    pthread_mutex_unlock(&offline_mutex);                  // освобождаем mutex
}

std::uint32_t allocate_msg_id() {                          // выделяет новый id сообщения
    pthread_mutex_lock(&id_mutex);                         // блокируем генератор id
    std::uint32_t id = next_msg_id++;                      // берем текущий и увеличиваем
    pthread_mutex_unlock(&id_mutex);                       // освобождаем mutex
    return id;                                             // возвращаем id
}

void init_next_msg_id_from_history() {                     // устанавливает следующий id по истории
    std::uint32_t max_id = 0;                              // максимальный найденный id
    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    for (const auto& r : history_records) {                // идем по записям
        max_id = std::max(max_id, r.msg_id);               // ищем максимум
    }
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex

    pthread_mutex_lock(&id_mutex);                         // блокируем генератор id
    next_msg_id = max_id + 1;                              // следующий id после максимального
    if (next_msg_id == 0) {                                // защита от переполнения
        next_msg_id = 1;                                   // начинаем с 1
    }
    pthread_mutex_unlock(&id_mutex);                       // освобождаем mutex
}

void append_history_record(const HistoryRecord& rec) {     // добавляет запись в историю
    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    history_records.push_back(rec);                        // добавляем запись
    (void)write_history_file_locked(HISTORY_FILE);         // сразу сохраняем в файл
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex
}

void mark_history_delivered(std::uint32_t msg_id) {        // помечает сообщение как доставленное
    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    for (auto& r : history_records) {                       // ищем нужную запись
        if (r.msg_id == msg_id) {                          // если id совпал
            r.delivered = true;                            // ставим delivered
            break;                                         // выходим
        }
    }
    (void)write_history_file_locked(HISTORY_FILE);         // сохраняем изменения
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex
}

std::string history_line(const HistoryRecord& r) {          // формирует строку для вывода истории
    std::string line = "[" + format_timestamp(r.timestamp) + "][id=" + std::to_string(r.msg_id) + "]"; // начало строки

    if (r.is_offline) {                                    // если это офлайн сообщение
        line += "[OFFLINE][" + r.sender + " -> " + r.receiver + "]: " + r.text; // формат офлайн
        return line;                                       // возвращаем строку
    }
    if (r.type == MSG_PRIVATE) {                           // если это личное сообщение
        line += "[PRIVATE][" + r.sender + " -> " + r.receiver + "]: " + r.text; // формат private
        return line;                                       // возвращаем строку
    }
    if (r.type == MSG_SERVER_INFO) {                       // если это сообщение сервера
        line += "[SERVER]: " + r.text;                     // формат сервера
        return line;                                       // возвращаем строку
    }
    line += "[" + r.sender + "]: " + r.text;               // обычный текст
    return line;                                           // возвращаем результат
}

void broadcast_message(                                    // рассылает сообщение всем клиентам
    std::uint8_t type,
    std::uint32_t msg_id,
    const std::string& sender,
    const std::string& receiver,
    std::int64_t timestamp,
    const std::string& payload,
    const std::string& action) {

    for (int s : snapshot_client_sockets()) {              // берем список сокетов
        (void)send_message_tcpip(s, type, msg_id, sender, receiver, timestamp, payload, action); // отправляем всем
    }
}

void close_client_socket(int sock) {                       // закрывает сокет клиента
    ::shutdown(sock, SHUT_RDWR);                           // отключаем чтение и запись
    ::close(sock);                                         // закрываем сокет
}

void remove_client(int sock) {                             // удаляет клиента из активных
    std::string nickname;                                  // ник клиента
    (void)get_nickname_by_sock(sock, nickname);            // пытаемся узнать ник

    pthread_mutex_lock(&clients_mutex);                    // блокируем список клиентов
    active_clients.erase(                                  // удаляем клиента из вектора
        std::remove_if(active_clients.begin(), active_clients.end(),
                       [&](const Client& c) { return c.sock == sock; }), // ищем по сокету
        active_clients.end());
    pthread_mutex_unlock(&clients_mutex);                  // освобождаем mutex

    close_client_socket(sock);                             // закрываем сокет

    if (!nickname.empty()) {                               // если ник удалось узнать
        const std::string info = "User [" + nickname + "] disconnected"; // формируем сообщение
        broadcast_message(MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), info, "broadcast server info"); // рассылаем всем

        HistoryRecord rec;                                 // запись в историю
        rec.msg_id = allocate_msg_id();                    // новый id
        rec.timestamp = static_cast<std::int64_t>(std::time(nullptr)); // текущее время
        rec.sender = "SERVER";                             // отправитель сервер
        rec.receiver = "";                                 // получателя нет
        rec.type = MSG_SERVER_INFO;                        // тип серверное инфо
        rec.text = info;                                   // текст сообщения
        rec.delivered = true;                              // доставлено
        rec.is_offline = false;                            // не офлайн
        append_history_record(rec);                        // сохраняем в историю
    }
}

void deliver_offline_messages(int sock, const std::string& nickname) { // доставляет накопленные офлайн сообщения
    std::vector<OfflineMsg> pending;                       // список сообщений для отправки
    pthread_mutex_lock(&offline_mutex);                    // блокируем офлайн очередь
    auto it = offline_queue.find(nickname);                // ищем очередь для ника
    if (it != offline_queue.end()) {                       // если очередь найдена
        pending = std::move(it->second);                   // забираем сообщения
        offline_queue.erase(it);                           // удаляем очередь из карты
    }
    pthread_mutex_unlock(&offline_mutex);                  // освобождаем mutex

    if (pending.empty()) {                                 // если офлайн сообщений нет
        (void)send_message_tcpip(sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), // отправляем инфо
                                 "no offline messages for " + nickname, "send server info");
        return;                                            // заканчиваем
    }

    for (const auto& om : pending) {                        // отправляем каждое сообщение
        const std::string payload = std::string("OFFLINE:") + om.text; // добавляем пометку offline
        if (send_message_tcpip(sock, MSG_PRIVATE, om.msg_id, om.sender, om.receiver, om.timestamp, payload, // отправка
                               "deliver offline message")) {
            mark_history_delivered(om.msg_id);             // помечаем как доставленное
        }
    }
}

bool parse_private_fallback(const std::string& payload, std::string& receiver, std::string& text) { // запасной разбор private
    std::size_t sep = payload.find(':');                   // ищем двоеточие
    if (sep == std::string::npos || sep == 0 || sep == payload.size() - 1) { // если формат плохой
        return false;                                      // ошибка
    }
    receiver = payload.substr(0, sep);                     // ник до двоеточия
    text = payload.substr(sep + 1);                        // текст после двоеточия
    if (!text.empty() && text[0] == ' ') {                 // если после двоеточия есть пробел
        text.erase(0, 1);                                 // убираем его
    }
    return true;                                           // формат подходит
}

void handle_history_request(int sock, const MessageEx& req) { // обрабатывает запрос истории
    int n = HISTORY_DEFAULT_N;                             // число сообщений по умолчанию
    if (req.length > 0) {                                  // если пользователь передал параметр
        try {
            n = std::stoi(req.payload);                    // пытаемся прочитать число
        } catch (...) {
            n = -1;                                         // если ошибка ставим невалидное значение
        }
        if (n <= 0) {                                      // если число плохое
            (void)send_message_tcpip(sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr), // отправляем ошибку
                                     "Invalid /history parameter", "send error");
            return;                                        // выходим
        }
    }

    std::vector<HistoryRecord> slice;                      // кусок истории для отправки
    pthread_mutex_lock(&history_mutex);                    // блокируем историю
    int total = static_cast<int>(history_records.size());  // общее число записей
    int start = std::max(0, total - n);                    // с какой записи начать
    for (int i = start; i < total; ++i) {                 // берем нужный диапазон
        slice.push_back(history_records[static_cast<std::size_t>(i)]); // копируем записи
    }
    pthread_mutex_unlock(&history_mutex);                  // освобождаем mutex

    for (const auto& r : slice) {                          // отправляем каждую запись
        std::string line = history_line(r);                // формируем строку
        if (line.size() > MAX_PAYLOAD) {                   // если строка слишком длинная
            line.resize(MAX_PAYLOAD);                     // обрезаем
        }
        (void)send_message_tcpip(sock, MSG_HISTORY_DATA, 0, "SERVER", "", std::time(nullptr), line, // отправляем историю
                                 "send history line");
    }
}

void handle_list_request(int sock) {                       // отправляет список онлайн пользователей
    std::ostringstream oss;                                // поток для сборки текста
    oss << "Online users\n";                               // заголовок
    pthread_mutex_lock(&clients_mutex);                    // блокируем список клиентов
    for (const auto& c : active_clients) {                 // добавляем каждого клиента
        oss << c.nickname << "\n";                         // пишем ник в список
    }
    pthread_mutex_unlock(&clients_mutex);                  // освобождаем mutex

    std::string out = oss.str();                           // получаем итоговую строку
    if (out.size() > MAX_PAYLOAD) {                        // если строка слишком длинная
        out.resize(MAX_PAYLOAD);                           // обрезаем
    }
    (void)send_message_tcpip(sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), out, "send user list"); // отправляем список
}

void* worker_thread(void*) {                               // рабочий поток сервера
    while (true) {                                         // работает бесконечно
        int client_sock = -1;                              // сокет клиента
        pthread_mutex_lock(&queue_mutex);                  // блокируем очередь клиентов
        while (client_queue.empty()) {                    // пока очередь пуста
            pthread_cond_wait(&queue_cond, &queue_mutex); // ждем нового клиента
        }
        client_sock = client_queue.front();               // берем первый сокет
        client_queue.pop();                               // удаляем его из очереди
        pthread_mutex_unlock(&queue_mutex);              // освобождаем mutex

        MessageEx msg{};                                  // буфер для сообщений
        if (!recv_message_tcpip(client_sock, msg) || msg.type != MSG_HELLO) { // ждем hello
            close_client_socket(client_sock);             // если не hello закрываем сокет
            continue;                                     // берем следующего клиента
        }

        (void)send_message_tcpip(client_sock, MSG_WELCOME, 0, "SERVER", "", std::time(nullptr), // отправляем приветствие
                                 "Welcome to the server!", "send welcome");

        bool authenticated = false;                       // флаг авторизации
        std::string nickname;                             // ник клиента
        while (!authenticated) {                          // пока не авторизован
            if (!recv_message_tcpip(client_sock, msg)) {  // если не удалось прочитать
                close_client_socket(client_sock);         // закрываем сокет
                authenticated = false;                    // авторизация не получилась
                break;                                    // выходим
            }
            if (msg.type != MSG_AUTH) {                   // если пришло не auth
                std::cout << "[Application] ignore message until authentication\n"; // игнорируем
                continue;                                 // ждем auth
            }

            nickname = msg.payload;                      // берем ник из payload
            if (nickname.empty() || nickname.size() > NICKNAME_MAX_LEN) { // проверяем длину
                (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr), // ошибка
                                         "Invalid nickname", "send auth error");
                close_client_socket(client_sock);         // закрываем сокет
                authenticated = false;                    // авторизация не удалась
                break;                                    // выходим
            }
            if (nickname_exists(nickname)) {             // если ник уже занят
                (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr), // отправляем ошибку
                                         "Nickname already in use", "send auth error");
                close_client_socket(client_sock);         // закрываем сокет
                authenticated = false;                    // авторизация не удалась
                break;                                    // выходим
            }

            add_client(client_sock, nickname);            // добавляем клиента
            authenticated = true;                         // ставим флаг авторизации

            const std::string info = "User [" + nickname + "] connected"; // текст события
            broadcast_message(MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), info, "broadcast server info"); // всем клиентам

            HistoryRecord rec;                            // запись в историю
            rec.msg_id = allocate_msg_id();               // id события
            rec.timestamp = static_cast<std::int64_t>(std::time(nullptr)); // текущее время
            rec.sender = "SERVER";                        // отправитель сервер
            rec.receiver = "";                            // получателя нет
            rec.type = MSG_SERVER_INFO;                   // тип серверное сообщение
            rec.text = info;                              // текст
            rec.delivered = true;                         // доставлено
            rec.is_offline = false;                       // не офлайн
            append_history_record(rec);                   // сохраняем

            deliver_offline_messages(client_sock, nickname); // отправляем накопленные сообщения
        }

        if (!authenticated) {                             // если не авторизовался
            continue;                                     // берем следующий сокет
        }

        while (true) {                                    // основной цикл общения с клиентом
            if (!recv_message_tcpip(client_sock, msg)) {  // если связь потеряна
                break;                                    // выходим
            }

            if (msg.type == MSG_TEXT) {                   // обычный текст
                std::uint32_t id = allocate_msg_id();     // новый id
                std::int64_t ts = static_cast<std::int64_t>(std::time(nullptr)); // текущее время
                std::string text = msg.payload;           // текст сообщения

                HistoryRecord rec;                        // запись в историю
                rec.msg_id = id;                          // id
                rec.timestamp = ts;                       // время
                rec.sender = nickname;                    // отправитель
                rec.receiver = "";                        // получателя нет
                rec.type = MSG_TEXT;                      // тип текст
                rec.text = text;                          // содержимое
                rec.delivered = true;                     // доставлено
                rec.is_offline = false;                   // не офлайн
                append_history_record(rec);               // сохраняем в историю

                broadcast_message(MSG_TEXT, id, nickname, "", ts, text, "broadcast text"); // рассылаем всем
                continue;                                 // следующая команда
            }

            if (msg.type == MSG_PRIVATE) {                // приватное сообщение
                std::string receiver = msg.receiver;      // получатель из заголовка
                std::string text = msg.payload;           // текст из payload
                if (receiver.empty()) {                   // если заголовок не заполнился
                    if (!parse_private_fallback(msg.payload, receiver, text)) { // пытаемся разобрать payload
                        (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr), // ошибка
                                                 "Invalid private message format", "send private error");
                        continue;                         // пропускаем сообщение
                    }
                }

                if (receiver.empty() || receiver.size() > NICKNAME_MAX_LEN) { // проверка ника получателя
                    (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr), // ошибка
                                             "Invalid receiver nickname", "send private error");
                    continue;                             // пропускаем
                }

                std::uint32_t id = allocate_msg_id();     // id сообщения
                std::int64_t ts = static_cast<std::int64_t>(std::time(nullptr)); // время

                int target_sock = -1;                     // сокет адресата
                if (get_sock_by_nickname(receiver, target_sock)) { // если получатель онлайн
                    HistoryRecord rec;                    // запись в историю
                    rec.msg_id = id;                      // id
                    rec.timestamp = ts;                   // время
                    rec.sender = nickname;                // отправитель
                    rec.receiver = receiver;              // получатель
                    rec.type = MSG_PRIVATE;               // тип private
                    rec.text = text;                      // текст
                    rec.delivered = true;                 // доставлено
                    rec.is_offline = false;               // не офлайн
                    append_history_record(rec);           // сохраняем

                    (void)send_message_tcpip(target_sock, MSG_PRIVATE, id, nickname, receiver, ts, text, // отправляем получателю
                                             "send private message");
                    (void)send_message_tcpip(client_sock, MSG_PRIVATE, id, nickname, receiver, ts, text, // эхо отправителю
                                             "echo private message");
                    continue;                             // следующая команда
                }

                OfflineMsg om;                            // офлайн сообщение
                om.msg_id = id;                           // id
                om.timestamp = ts;                        // время
                om.sender = nickname;                     // отправитель
                om.receiver = receiver;                  // получатель
                om.text = text;                           // текст

                pthread_mutex_lock(&offline_mutex);       // блокируем офлайн очередь
                offline_queue[receiver].push_back(om);    // сохраняем сообщение
                pthread_mutex_unlock(&offline_mutex);     // освобождаем mutex

                HistoryRecord rec;                        // запись в историю
                rec.msg_id = id;                          // id
                rec.timestamp = ts;                       // время
                rec.sender = nickname;                    // отправитель
                rec.receiver = receiver;                 // получатель
                rec.type = MSG_PRIVATE;                   // тип private
                rec.text = text;                          // текст
                rec.delivered = false;                    // пока не доставлено
                rec.is_offline = true;                    // это офлайн сообщение
                append_history_record(rec);               // сохраняем

                (void)send_message_tcpip(client_sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), // сообщаем отправителю
                                         "receiver " + receiver + " is offline, message stored",
                                         "send server info");

                const std::string echo_payload = std::string("OFFLINE:") + text; // помечаем как офлайн
                (void)send_message_tcpip(client_sock, MSG_PRIVATE, id, nickname, receiver, ts, echo_payload, // эхо
                                         "echo offline private");
                continue;                                 // следующая команда
            }

            if (msg.type == MSG_LIST) {                   // запрос списка пользователей
                handle_list_request(client_sock);         // отправляем список
                continue;                                 // дальше
            }

            if (msg.type == MSG_HISTORY) {                // запрос истории
                handle_history_request(client_sock, msg); // обрабатываем запрос
                continue;                                 // дальше
            }

            if (msg.type == MSG_PING) {                   // ping от клиента
                (void)send_message_tcpip(client_sock, MSG_PONG, 0, "SERVER", "", std::time(nullptr), "", "send pong"); // отвечаем pong
                continue;                                 // дальше
            }

            if (msg.type == MSG_BYE) {                    // клиент хочет выйти
                std::cout << "Client disconnected by request\n"; // логируем выход
                break;                                    // выходим из цикла
            }
        }

        remove_client(client_sock);                       // удаляем клиента после разрыва
    }

    return nullptr;                                       // поток не завершается в обычном режиме
}

}  // namespace                                            // конец анонимного пространства имен

int main() {                                              // точка входа в сервер
    (void)load_history_file(HISTORY_FILE);                // загружаем историю из файла
    init_next_msg_id_from_history();                      // настраиваем следующий id
    rebuild_offline_queue_from_history();                 // восстанавливаем офлайн очередь

    int server_sock = ::socket(AF_INET, SOCK_STREAM, 0);  // создаем tcp сокет сервера
    if (server_sock < 0) {                                // если не удалось
        std::perror("socket");                            // выводим ошибку
        return 1;                                         // завершаем с ошибкой
    }

    int opt = 1;                                          // значение для reuseaddr
    ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // разрешаем быстрое повторное открытие

    sockaddr_in server_addr{};                            // адрес сервера
    server_addr.sin_family = AF_INET;                     // ipv4
    server_addr.sin_addr.s_addr = INADDR_ANY;             // принимать на всех интерфейсах
    server_addr.sin_port = htons(SERVER_PORT);            // порт сервера

    if (::bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) { // привязываем порт
        std::perror("bind");                              // если ошибка
        ::close(server_sock);                             // закрываем сокет
        return 1;                                         // выходим
    }

    if (::listen(server_sock, 10) < 0) {                  // начинаем слушать подключения
        std::perror("listen");                            // выводим ошибку
        ::close(server_sock);                             // закрываем сокет
        return 1;                                         // выходим
    }

    std::cout << "Server listening on port " << SERVER_PORT << "\n"; // сообщаем что сервер запущен

    pthread_t threads[THREAD_POOL_SIZE];                  // массив рабочих потоков
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {          // создаем пул потоков
        pthread_create(&threads[i], nullptr, worker_thread, nullptr); // запускаем поток
    }

    while (true) {                                        // основной цикл принятия подключений
        sockaddr_in client_addr{};                        // адрес клиента
        socklen_t client_len = sizeof(client_addr);       // размер структуры адреса
        int client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len); // принимаем клиента
        if (client_sock < 0) {                            // если accept не удался
            continue;                                     // просто идем дальше
        }
        std::cout << "Connection accepted from " << ipport_of(client_addr) << "\n"; // печатаем адрес клиента

        pthread_mutex_lock(&queue_mutex);                 // блокируем очередь
        client_queue.push(client_sock);                   // кладем сокет в очередь
        pthread_cond_signal(&queue_cond);                 // будим один рабочий поток
        pthread_mutex_unlock(&queue_mutex);               // освобождаем mutex
    }

    ::close(server_sock);                                 // закрываем серверный сокет
    return 0;                                             // успешное завершение
}
