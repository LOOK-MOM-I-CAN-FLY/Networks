#pragma once
// Защита от многократного включения заголовка: если этот файл уже подключён,
// препроцессор пропустит повторные включения.

#include <cstdint>   // фиксированные целочисленные типы: uint32_t, uint8_t и т.п.
#include <string>    // std::string
#include <cstring>   // std::memset, std::memcpy
#include <algorithm> // std::min
#include <iostream>  // для отладки (std::cerr, std::cout)



// Максимальный размер полезной нагрузки (payload) в байтах.
// Выделяем фиксированный буфер в структуре Message.
constexpr size_t MAX_PAYLOAD = 1024;

#pragma pack(push, 1)
// Говорим компилятору упаковать структуру без паддинга (байт за байтом).
// Это важно, чтобы поля имели предсказуемое смещение при записи/чтении по сети.
struct Message {
    uint32_t length;              // длина (в сетевом порядке байтов) = 1 (type) + payload_len
    uint8_t  type;                // тип сообщения (MessageType)
    char     payload[MAX_PAYLOAD];// буфер для строки/данных, не гарантируется '\0' пока не поставим
};
#pragma pack(pop)

// Список типов сообщений — удобные имена для цифр.
enum MessageType : uint8_t {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// На Windows send/recv возвращают int; на POSIX ssize_t. Используем int для сравнения.
inline bool send_all(int sock, const void* data, size_t size) {
    // Отправляем данные полностью. send может отправить не все байты за один вызов,
    // поэтому в цикле продолжаем, пока не уйдёт весь буфер.
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        int sent = send(sock, ptr, static_cast<int>(size), 0);
        // sent <= 0 — ошибка или закрыто соединение.
        if (sent <= 0) return false;
        ptr += sent;      // сдвигаем указатель на количество отправленных байт
        size -= sent;     // уменьшаем оставшийся размер
    }
    return true; // всё успешно отправлено
}

inline bool recv_all(int sock, void* data, size_t size) {
    // Аналогично, recv может вернуть меньше запрошенных байт, поэтому читаем в цикле.
    char* ptr = static_cast<char*>(data);
    while (size > 0) {
        int received = recv(sock, ptr, static_cast<int>(size), 0);
        // received <= 0 — ошибка или соединение закрылось
        if (received <= 0) return false;
        ptr += received;  // сдвигаем указатель внутрь буфера
        size -= received; // уменьшаем количество оставшихся байт
    }
    return true; // прочитали всё, что просили
}

inline bool send_message(int sock, uint8_t type, const std::string& payload = "") {
    // Подготавливаем структуру сообщения и отправляем только нужное количество байт.
    Message msg;
    std::memset(&msg, 0, sizeof(msg)); // очищаем структуру, чтобы в payload были нули

    // payload_len — сколько байт из payload реально будем положить в msg.payload.
    // Ограничиваем до MAX_PAYLOAD, чтобы не выйти за буфер.
    uint32_t payload_len = std::min<uint32_t>(static_cast<uint32_t>(payload.length()), static_cast<uint32_t>(MAX_PAYLOAD));
    // В поле length записываем общее количество байт после поля length:
    // 1 байт на type + payload_len байт на данные. Сохраняем в сетевом порядке (htonl).
    msg.length = htonl(1 + payload_len);
    msg.type = type; // записываем тип

    // Копируем payload_len байт из строки в msg.payload (если есть что копировать).
    if (payload_len > 0) {
        std::memcpy(msg.payload, payload.data(), payload_len);
    }

    // Считаем, сколько байт всего будем отправлять: 4 байта length + 1 байт type + payload_len.
    size_t total_size = sizeof(msg.length) + 1 + payload_len;
    // Отправляем первый кусок структуры (length) + остальную часть (type+payload) одним буфером.
    // send_all сам позаботится о частичных отправках.
    return send_all(sock, &msg, total_size);
}

inline bool recv_message(int sock, Message& msg) {
    // Принимаем сообщение: сначала читаем поле length (4 байта), потом читаем type+payload.
    std::memset(&msg, 0, sizeof(msg)); // очищаем буфер для безопасности

    // Читаем ровно 4 байта длины (в сетевом порядке байтов).
    if (!recv_all(sock, &msg.length, sizeof(msg.length))) return false;

    // Переводим длину в порядок хоста (ntohl). len = 1 (type) + payload_len.
    uint32_t len = ntohl(msg.length);

    // Проверяем корректность: len должен быть >=1 (есть хотя бы type)
    // и не превышать 1 + MAX_PAYLOAD (чтобы payload поместился).
    if (len == 0 || len > 1 + MAX_PAYLOAD) return false;

    // Читаем len байт сразу в поле type и далее в payload (т.к. структура упакована,
    // type и payload идут подряд в памяти). recv_all гарантирует чтение всех len байт.
    if (!recv_all(sock, &msg.type, len)) return false;

    // Теперь ставим в payload завершающий нулевой символ для удобной работы как с C-строкой.
    // Важно: len включает 1 байт type, поэтому payload длина = len - 1.
    uint32_t payload_len = len - 1;
    if (payload_len <= MAX_PAYLOAD) {
        // записываем '\0' в позицию после реальных payload_len байт
        msg.payload[payload_len] = '\0';
    } else {
        // на всякий случай: если payload_len вдруг больше буфера (не должно происходить),
        // ставим последний байт буфера как нуль-терминатор.
        msg.payload[MAX_PAYLOAD - 1] = '\0';
    }
    return true;
}
