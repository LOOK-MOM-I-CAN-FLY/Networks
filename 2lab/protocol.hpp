#pragma once
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
    // Подключаем библиотеки Windows Sockets
    #include <winsock2.h>
    #include <ws2tcpip.h>
    // MSVC проглотит эту прагму и сам подцепит библиотеку. 
    // Если у вас MinGW (GCC), нужно будет добавить флаг линковщика (см. ниже).
    #pragma comment(lib, "ws2_32.lib") 
    
    // В Windows типы немного отличаются, сглаживаем углы:
    typedef int socklen_t;
    #define SHUT_RDWR SD_BOTH
    
    // Windows любит переопределять min/max, ломая std::min
    #undef min
    #undef max
#else
    // POSIX (Linux, macOS)
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define closesocket close
#endif

constexpr size_t MAX_PAYLOAD = 1024;

#pragma pack(push, 1)
struct Message {
    uint32_t length;  
    uint8_t  type;    
    char     payload[MAX_PAYLOAD]; 
};
#pragma pack(pop)

enum MessageType : uint8_t {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// В Windows send и recv возвращают int, а не ssize_t
inline bool send_all(int sock, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        int sent = send(sock, ptr, size, 0);
        if (sent <= 0) return false;
        ptr += sent;
        size -= sent;
    }
    return true;
}

inline bool recv_all(int sock, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    while (size > 0) {
        int received = recv(sock, ptr, size, 0);
        if (received <= 0) return false; 
        ptr += received;
        size -= received;
    }
    return true;
}

inline bool send_message(int sock, uint8_t type, const std::string& payload = "") {
    Message msg;
    std::memset(&msg, 0, sizeof(msg));
    
    uint32_t payload_len = std::min<uint32_t>(payload.length(), MAX_PAYLOAD);
    msg.length = htonl(1 + payload_len); 
    msg.type = type;
    
    if (payload_len > 0) {
        std::memcpy(msg.payload, payload.data(), payload_len);
    }
    
    size_t total_size = sizeof(msg.length) + 1 + payload_len;
    return send_all(sock, &msg, total_size);
}

inline bool recv_message(int sock, Message& msg) {
    std::memset(&msg, 0, sizeof(msg));
    
    if (!recv_all(sock, &msg.length, sizeof(msg.length))) return false;
    
    uint32_t len = ntohl(msg.length);
    if (len == 0 || len > 1 + MAX_PAYLOAD) return false; 
    
    if (!recv_all(sock, &msg.type, len)) return false;
    
    if (len <= MAX_PAYLOAD) {
        msg.payload[len - 1] = '\0';
    } else {
        msg.payload[MAX_PAYLOAD - 1] = '\0';
    }
    return true;
}