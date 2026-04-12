#pragma once

#include <algorithm>
#include <arpa/inet.h>   // htonl/ntohl
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/socket.h>  // send/recv

constexpr int SERVER_PORT = 5555;

constexpr std::size_t MAX_NAME = 32;
constexpr std::size_t MAX_PAYLOAD = 256;
// Перечень типов сообщений.
// Это просто удобные имена для чисел, чтобы не писать "1", "2", "3" и т.д.
enum MessageType : std::uint8_t {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,
    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,
    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14
};

inline const char* message_type_name(std::uint8_t type) {
    switch (type) {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        default: return "MSG_UNKNOWN";
    }
}

inline bool send_all(int sock, const void* data, std::size_t size) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t sent = ::send(sock, ptr, size, 0);
        if (sent <= 0) {
            return false;
        }
        ptr += static_cast<std::size_t>(sent);
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool recv_all(int sock, void* data, std::size_t size) {
    // Приводим буфер к байтовому указателю для поэтапного чтения.
    char* ptr = static_cast<char*>(data);

    // Пока остались байты для чтения, продолжаем принимать данные.
    while (size > 0) {
        // ::recv() — системный вызов POSIX.
        // Он читает данные из TCP-стрима, но тоже может вернуть только часть.
        //ptr - куда recv должен записать принятые данные
        ssize_t received = ::recv(sock, ptr, size, 0);// 0 -> MSG_PEEK позволяет "подсмотреть" данные, не удаляя их из системного буфера.

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

inline std::uint64_t htonll(std::uint64_t value) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL))) << 32) |
           htonl(static_cast<std::uint32_t>(value >> 32));
#else
    return value;
#endif
}

inline std::uint64_t ntohll(std::uint64_t value) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return (static_cast<std::uint64_t>(ntohl(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL))) << 32) |
           ntohl(static_cast<std::uint32_t>(value >> 32));
#else
    return value;
#endif
}

struct MessageEx {
    std::uint32_t length;  // длина payload (в байтах)
    std::uint8_t type;
    std::uint32_t msg_id;
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    std::int64_t timestamp;  // unix time
    char payload[MAX_PAYLOAD + 1];
};

#pragma pack(push, 1)
struct MessageExWireHeader {
    std::uint32_t length;     // network order, payload bytes
    std::uint8_t type;
    std::uint32_t msg_id;     // network order
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    std::uint64_t timestamp;  // network order
};
#pragma pack(pop)

inline void message_ex_clear(MessageEx& msg) {
    std::memset(&msg, 0, sizeof(msg));
}

inline std::string format_timestamp(std::int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#if defined(_POSIX_VERSION)
    localtime_r(&t, &tm);
#else
    std::tm* ptm = std::localtime(&t);
    if (ptm) {
        tm = *ptm;
    }
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

inline bool send_message_ex(
    int sock,
    std::uint8_t type,
    std::uint32_t msg_id,
    const std::string& sender,
    const std::string& receiver,
    std::int64_t timestamp,
    const std::string& payload) {

    std::uint32_t payload_len = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(MAX_PAYLOAD));

    MessageExWireHeader header{};
    header.length = htonl(payload_len);
    header.type = type;
    header.msg_id = htonl(msg_id);
    std::memset(header.sender, 0, sizeof(header.sender));
    std::memset(header.receiver, 0, sizeof(header.receiver));
    if (!sender.empty()) {
        std::strncpy(header.sender, sender.c_str(), sizeof(header.sender) - 1);
    }
    if (!receiver.empty()) {
        std::strncpy(header.receiver, receiver.c_str(), sizeof(header.receiver) - 1);
    }
    header.timestamp = htonll(static_cast<std::uint64_t>(timestamp));

    if (!send_all(sock, &header, sizeof(header))) {
        return false;
    }
    if (payload_len == 0) {
        return true;
    }
    return send_all(sock, payload.data(), payload_len);
}

inline bool recv_message_ex(int sock, MessageEx& msg) {
    message_ex_clear(msg);

    MessageExWireHeader header{};
    if (!recv_all(sock, &header.length, sizeof(header.length))) {
        return false;
    }
    if (!recv_all(sock, &header.type, sizeof(header) - sizeof(header.length))) {
        return false;
    }

    std::uint32_t payload_len = ntohl(header.length);
    if (payload_len > MAX_PAYLOAD) {
        return false;
    }

    msg.length = payload_len;
    msg.type = header.type;
    msg.msg_id = ntohl(header.msg_id);
    std::memcpy(msg.sender, header.sender, sizeof(msg.sender));
    std::memcpy(msg.receiver, header.receiver, sizeof(msg.receiver));
    msg.sender[MAX_NAME - 1] = '\0';
    msg.receiver[MAX_NAME - 1] = '\0';
    msg.timestamp = static_cast<std::int64_t>(ntohll(header.timestamp));

    if (payload_len > 0) {
        if (!recv_all(sock, msg.payload, payload_len)) {
            return false;
        }
    }
    msg.payload[payload_len] = '\0';
    return true;
}
