// protocol.hpp / protocol.cpp
// Этот файл лучше хранить как заголовок, потому что здесь есть inline-функции,
// структуры и перечисление, а не отдельная "тело-реализация" в классическом смысле.

// ---- Только Linux / POSIX ----

#include <cstdint>    // uint8_t, uint32_t
#include <cstddef>    // std::size_t
#include <string>     // std::string
#include <cstring>    // std::memset, std::memcpy
#include <algorithm>  // std::min

// POSIX-сокеты: send(), recv()
#include <sys/socket.h>

// Преобразование порядка байтов: htonl(), ntohl()
#include <arpa/inet.h>

// Максимальный размер полезной нагрузки в байтах.
// Это именно лимит на данные, которые мы готовы передать в payload.
constexpr std::size_t MAX_PAYLOAD = 1024;

// Упаковка структуры без выравнивания.
// Это нужно, чтобы поля в памяти шли подряд строго в том порядке,
// в котором мы ожидаем их отправлять и принимать по сети.
#pragma pack(push, 1)
struct Message {
    // Длина "хвоста" сообщения в сетевом порядке байт.
    // Здесь хранится не вся структура, а только:
    //   1 байт type + payload_len байт payload.
    uint32_t length;

    // Тип сообщения.
    // Это один байт, потому что у нас enum MessageType основан на uint8_t.
    uint8_t type;

    // Буфер данных.
    // +1 байт добавлен специально под завершающий '\0',
    // чтобы удобно работать как со строкой внутри программы.
    // В сеть этот нулевой байт не отправляется.
    char payload[MAX_PAYLOAD + 1];
};
#pragma pack(pop)

// Перечень типов сообщений.
// Это просто удобные имена для чисел, чтобы не писать "1", "2", "3" и т.д.
enum MessageType : uint8_t {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// Эта функция отправляет в сокет ровно size байт,
// даже если send() за один раз отправит только часть данных.
inline bool send_all(int sock, const void* data, std::size_t size) {
    // Приводим исходный указатель к указателю на байты,
    // потому что отправку удобнее вести как работу с сырым массивом байт.
    const char* ptr = static_cast<const char*>(data);

    // Пока остались байты для отправки, продолжаем цикл.
    while (size > 0) {
        // ::send() — системный вызов POSIX.
        // Он может отправить не весь буфер сразу, особенно в TCP.
        // Поэтому мы не верим одному вызову и проверяем, сколько реально ушло.
        ssize_t sent = ::send(sock, ptr, size, 0);

        // Если send вернул 0 или отрицательное значение,
        // значит произошла ошибка или соединение закрылось.
        if (sent <= 0) {
            return false;
        }

        // Сдвигаем указатель дальше на количество успешно отправленных байт.
        ptr += static_cast<std::size_t>(sent);

        // Уменьшаем оставшийся объём данных.
        size -= static_cast<std::size_t>(sent);
    }

    // Если дошли сюда, значит весь буфер ушёл успешно.
    return true;
}

// Эта функция читает из сокета ровно size байт.
// recv() тоже может вернуть меньше, чем мы просим, поэтому нужен цикл.
inline bool recv_all(int sock, void* data, std::size_t size) {
    // Приводим буфер к байтовому указателю для поэтапного чтения.
    char* ptr = static_cast<char*>(data);

    // Пока остались байты для чтения, продолжаем принимать данные.
    while (size > 0) {
        // ::recv() — системный вызов POSIX.
        // Он читает данные из TCP-стрима, но тоже может вернуть только часть.
        ssize_t received = ::recv(sock, ptr, size, 0);

        // received <= 0 означает либо ошибку, либо закрытие соединения.
        if (received <= 0) {
            return false;
        }

        // Двигаем указатель на количество реально принятых байт.
        ptr += static_cast<std::size_t>(received);

        // Уменьшаем число оставшихся байт.
        size -= static_cast<std::size_t>(received);
    }

    // Если цикл завершился, значит буфер заполнен полностью.
    return true;
}

// Эта функция собирает сообщение и отправляет его целиком:
// 1) длина
// 2) тип
// 3) полезная нагрузка
inline bool send_message(int sock, uint8_t type, const std::string& payload = "") {
    // Создаём структуру сообщения.
    Message msg;

    // Очищаем всю структуру нулями.
    // Это удобно, чтобы в памяти не оставалось мусора.
    std::memset(&msg, 0, sizeof(msg));

    // Берём длину строки payload, но ограничиваем её MAX_PAYLOAD.
    // Если строка длиннее, мы её просто обрезаем.
    // std::min нужен, чтобы не выйти за пределы буфера.
    std::uint32_t payload_len = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(MAX_PAYLOAD)
    );

    // length хранит число байт после самого поля length:
    //   1 байт type + payload_len байт payload.
    // htonl() переводит число в сетевой порядок байт.
    msg.length = htonl(1 + payload_len);

    // Записываем тип сообщения.
    msg.type = type;

    // Если payload не пустой, копируем нужное количество байт в буфер.
    // memcpy используется потому, что мы просто переносим сырой блок памяти.
    if (payload_len > 0) {
        std::memcpy(msg.payload, payload.data(), payload_len);
    }

    // Добавляем завершающий '\0', чтобы внутри программы payload можно было
    // воспринимать как C-строку.
    // Благодаря payload[MAX_PAYLOAD + 1] это безопасно.
    msg.payload[payload_len] = '\0';

    // В сеть мы отправляем только:
    //   4 байта length
    //   1 байт type
    //   payload_len байт полезной нагрузки
    //
    // То есть нулевой байт НЕ отправляется.
    std::size_t total_size = sizeof(msg.length) + 1 + payload_len;

    // Отправляем всё через send_all(), чтобы не потерять часть сообщения.
    return send_all(sock, &msg, total_size);
}

// Эта функция принимает одно сообщение из сокета:
// сначала length, потом type и payload.
inline bool recv_message(int sock, Message& msg) {
    // На всякий случай очищаем структуру.
    std::memset(&msg, 0, sizeof(msg));

    // Сначала читаем первые 4 байта — поле length.
    // Это важно, потому что именно оно говорит, сколько ещё байт читать дальше.
    if (!recv_all(sock, &msg.length, sizeof(msg.length))) {
        return false;
    }

    // Преобразуем длину из сетевого порядка байт в порядок хоста.
    // Теперь len — это обычное число в формате текущей машины.
    std::uint32_t len = ntohl(msg.length);

    // len должен быть как минимум 1,
    // потому что хотя бы один байт нужен под поле type.
    // И он не должен превышать 1 + MAX_PAYLOAD.
    if (len == 0 || len > 1 + MAX_PAYLOAD) {
        return false;
    }

    // Читаем сразу len байт, начиная с поля type.
    // Почему это работает:
    // - структура упакована через #pragma pack(1)
    // - поле type идёт сразу после length
    // - дальше сразу лежит payload
    //
    // То есть мы фактически читаем "type + payload" одним блоком.
    if (!recv_all(sock, &msg.type, len)) {
        return false;
    }

    // Длина payload — это len - 1,
    // потому что один байт из len уходит на type.
    std::uint32_t payload_len = len - 1;

    // Ставим завершающий '\0' в конец строки.
    // Это делает payload удобным для вывода через std::cout
    // и для любых C-строковых операций.
    msg.payload[payload_len] = '\0';

    // Если дошли сюда — сообщение принято корректно.
    return true;
}
