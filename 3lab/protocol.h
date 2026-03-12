#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_PAYLOAD 1024
#define SERVER_PORT 8080

// Типы сообщений строго по ТЗ
enum {
    MSG_HELLO = 1,   // клиент -> сервер (ник)
    MSG_WELCOME = 2, // сервер -> клиент
    MSG_TEXT = 3,    // текст
    MSG_PING = 4,    // пинг
    MSG_PONG = 5,    // понг
    MSG_BYE = 6      // отключение
};

// Структура сообщения. 
// #pragma pack(push, 1) заставляет компилятор упаковать структуру без пустых байтов,
// чтобы по сети она передавалась ровно так, как описана (важно для сетевого кода!).
#pragma pack(push, 1)
typedef struct {
    uint32_t length;               // длина поля type + payload
    uint8_t  type;                 // тип сообщения
    char     payload[MAX_PAYLOAD]; // данные
} Message;
#pragma pack(pop)

#endif
