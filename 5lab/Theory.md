# Теория к лабораторной работе №5
## Расширенный TCP-чат: модель TCP/IP, MessageEx, Store & Forward, история в JSON

---

## 1. Модель TCP/IP

### 1.1 Что такое TCP/IP и почему она отличается от OSI

Модель OSI (7 уровней) — это теоретический эталон, описывающий **как должна быть устроена** сеть. Модель TCP/IP — это **то, как сети реально работают** сегодня. Она возникла раньше OSI, была стандартизирована через реальные протоколы интернета и имеет 4 уровня вместо 7.

| Уровень TCP/IP | Соответствие уровней OSI | Суть |
|---|---|---|
| **Application** (Прикладной) | OSI 5 (Session) + 6 (Presentation) + 7 (Application) | Логика приложения: форматы, протоколы, команды |
| **Transport** (Транспортный) | OSI 4 (Transport) | Надёжная/ненадёжная доставка между портами |
| **Internet** (Сетевой) | OSI 3 (Network) | IP-адресация, маршрутизация пакетов |
| **Network Access** (Канальный/физический) | OSI 1 (Physical) + 2 (Data Link) | Физическая передача кадров по среде |

> **Ключевой вопрос преподавателя:** «Чем TCP/IP отличается от OSI?»  
> **Ответ:** OSI — академическая модель из 7 уровней, TCP/IP — практическая из 4. TCP/IP объединяет сеансовый, представительский и прикладной уровни OSI в один — Application. Физический и канальный — в один Network Access.

---

### 1.2 Уровень Network Access (Канальный/физический)

**Что делает:** обеспечивает доступ к физической среде передачи. Формирует кадры Ethernet с MAC-адресами источника и назначения, добавляет контрольные суммы (FCS), передаёт по проводу или Wi-Fi.

**В нашем коде:**  
Мы не управляем этим уровнем напрямую — это делает операционная система и сетевой адаптер (NIC). В логах просто фиксируем факт передачи:
```
[Network Access] frame received via network interface
[Network Access] frame sent to network interface
```

Функции `log_incoming()` и `log_outgoing()` в `server.cpp` выводят эти строки при каждом приёме и отправке сообщения.

---

### 1.3 Уровень Internet (Сетевой)

**Что делает:** отвечает за IP-адресацию и маршрутизацию. Каждый пакет содержит IP-заголовок с полями `src` (источник) и `dst` (назначение). Протокол IP — ненадёжный (без гарантий доставки), поэтому надёжность обеспечивает TCP.

**В нашем коде:**  
Адреса получаем через `getsockname()` (локальный адрес сокета) и `getpeername()` (адрес удалённой стороны), затем конвертируем в строку через `inet_ntop()`:

```cpp
// server.cpp: функция get_endpoints()
::getsockname(sock, ...);  // мой адрес (сервер)
::getpeername(sock, ...);  // адрес клиента
```

Лог:
```
[Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
```

Поле `proto=TCP` означает, что на транспортном уровне используется TCP (номер протокола IP = 6).

---

### 1.4 Уровень Transport (Транспортный)

**Что делает:** обеспечивает доставку данных между конкретными портами на хостах. TCP — протокол с установлением соединения, гарантирует:
- **доставку** (повторная передача при потере),
- **порядок** (байты приходят в том же порядке, в каком отправлены),
- **управление потоком** (не перегружает медленного получателя).

**Три фазы TCP:**
1. **Handshake (рукопожатие):** SYN → SYN-ACK → ACK (3-way handshake)
2. **Передача данных:** send/recv
3. **Закрытие соединения:** FIN → ACK → FIN → ACK (4-way teardown), или `RST` при аварийном закрытии

**TCP — поток байт, не пакеты!** Это критически важно: TCP не знает о границах сообщений. Одно `send()` на одной стороне может привести к нескольким `recv()` на другой. Поэтому мы используем `send_all()` и `recv_all()`:

```cpp
// protocol.hpp
inline bool send_all(int sock, const void* data, std::size_t size) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t sent = ::send(sock, ptr, size, 0);
        if (sent <= 0) return false;
        ptr  += sent;
        size -= sent;
    }
    return true;
}
```

Цикл продолжается до тех пор, пока не будут отправлены **все** байты, потому что одного `send()` может оказаться недостаточно.

**В нашем коде:**
```
[Transport] recv() 160 bytes via TCP
[Transport] send() 128 bytes via TCP
```

Размер считается как `sizeof(MessageExWireHeader) + msg.length`.

---

### 1.5 Уровень Application (Прикладной)

**Что делает:** содержит логику конкретного приложения — форматы сообщений, команды, аутентификацию, авторизацию, бизнес-правила.

**В нашем коде:**
- Структура `MessageEx` — формат прикладного протокола
- Команды `/w`, `/history`, `/list`, `/ping`
- Аутентификация по нику
- Store & Forward (офлайн-очередь)
- История в JSON

---

### 1.6 Инкапсуляция данных в TCP/IP

Инкапсуляция — это принцип, при котором каждый уровень **оборачивает** данные предыдущего уровня в свой заголовок:

```
+---------------------------+
|   Application Data        |  ← MessageEx (наш протокол)
+---------------------------+
|   TCP Header              |  ← src_port, dst_port, seq, ack, flags
+---------------------------+
|   IP Header               |  ← src_ip, dst_ip, protocol=6
+---------------------------+
|   Ethernet Frame Header   |  ← src_mac, dst_mac, ethertype
+---------------------------+
|   Ethernet Frame Footer   |  ← FCS (контрольная сумма)
+---------------------------+
```

При получении — де-инкапсуляция в обратном порядке: Ethernet → IP → TCP → Application.

---

## 2. Бинарный протокол MessageEx

### 2.1 Зачем нужен бинарный протокол

Текстовые протоколы (как HTTP/1.1 или SMTP) просты для отладки, но неэффективны: парсинг строк медленнее, структуры данных занимают больше места. Бинарные протоколы точно определяют размер каждого поля и позволяют парсить структуры за одно чтение.

### 2.2 Структура MessageEx (в памяти)

```cpp
// protocol.hpp
struct MessageEx {
    std::uint32_t length;        // длина payload (в байтах), 4 байта
    std::uint8_t  type;          // тип сообщения, 1 байт
    std::uint32_t msg_id;        // уникальный ID, 4 байта
    char sender[MAX_NAME];       // ник отправителя, 32 байта
    char receiver[MAX_NAME];     // ник получателя, 32 байта
    std::int64_t  timestamp;     // unix time, 8 байт
    char payload[MAX_PAYLOAD+1]; // данные + нуль-терминатор, 257 байт
};
```

### 2.3 Сетевой формат — MessageExWireHeader

Для передачи по сети используется отдельная структура **без паддинга**:

```cpp
#pragma pack(push, 1)
struct MessageExWireHeader {
    std::uint32_t length;     // 4 байта
    std::uint8_t  type;       // 1 байт
    std::uint32_t msg_id;     // 4 байта
    char sender[MAX_NAME];    // 32 байта
    char receiver[MAX_NAME];  // 32 байта
    std::uint64_t timestamp;  // 8 байт, беззнаковый для сети
};                            // Итого: 81 байт
#pragma pack(pop)
```

**`#pragma pack(1)`** — директива, запрещающая компилятору вставлять байты выравнивания (padding) между полями. Без неё компилятор может добавить скрытые байты, и структуры на разных платформах будут иметь разный размер.

> **Вопрос преподавателя:** «Зачем две структуры — MessageEx и MessageExWireHeader?»  
> **Ответ:** MessageEx удобна для работы в памяти — payload как строка с нуль-терминатором. MessageExWireHeader — точный бинарный формат без паддинга для передачи по сети. Разделение упрощает код: в памяти работаем удобно, по сети — компактно.

---

### 2.4 Порядок байт (byte order)

Разные процессоры хранят многобайтовые числа по-разному:
- **Little-endian** (x86, AMD64): младший байт по меньшему адресу. `0x01020304` → в памяти `04 03 02 01`
- **Big-endian**: старший байт по меньшему адресу. `0x01020304` → `01 02 03 04`

**Сетевой порядок байт = Big-endian.** Всегда. Стандарт IETF.

Стандартные функции конвертации:
```
htonl() — host to network, 32 бит
ntohl() — network to host, 32 бит
htons() — host to network, 16 бит
ntohs() — network to host, 16 бит
```

Для 64-бит нет стандартной функции, поэтому реализована своя:

```cpp
// protocol.hpp
inline std::uint64_t htonll(std::uint64_t value) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return (static_cast<uint64_t>(htonl(value & 0xFFFFFFFF)) << 32) |
            htonl(value >> 32);
#else
    return value;  // big-endian — уже в сетевом порядке
#endif
}
```

При отправке: `header.timestamp = htonll(timestamp)` → конвертируем в сетевой порядок.  
При получении: `msg.timestamp = ntohll(header.timestamp)` → возвращаем в порядок хоста.

---

### 2.5 Протокол отправки/приёма сообщения

Сообщение передаётся в **два этапа**:
1. Заголовок фиксированного размера (`sizeof(MessageExWireHeader)` = 81 байт)
2. Payload переменной длины (`length` байт)

```cpp
// Отправка (send_message_ex):
send_all(sock, &header, sizeof(header));  // сначала заголовок
send_all(sock, payload.data(), payload_len);  // потом тело

// Получение (recv_message_ex):
recv_all(sock, &header.length, 4);        // сначала длину payload
recv_all(sock, &header.type, sizeof(header) - 4);  // остаток заголовка
recv_all(sock, msg.payload, payload_len); // потом тело
```

> **Вопрос:** «Почему заголовок читается в два recv_all, а не одним?»  
> **Ответ:** Сначала читаем поле `length` (4 байта), чтобы знать, сколько данных ждать. Затем — оставшиеся поля заголовка. После — payload. Это стандартный паттерн «length-prefixed protocol» для бинарных протоколов.

---

### 2.6 Типы сообщений

| Тип | Значение | Направление | Описание |
|---|---|---|---|
| MSG_HELLO | 1 | Клиент → Сервер | Первое сообщение после соединения |
| MSG_WELCOME | 2 | Сервер → Клиент | Ответ сервера на HELLO |
| MSG_TEXT | 3 | Клиент → Сервер → Все | Публичное сообщение |
| MSG_PING | 4 | Клиент → Сервер | Проверка связи |
| MSG_PONG | 5 | Сервер → Клиент | Ответ на PING |
| MSG_BYE | 6 | Клиент → Сервер | Корректное завершение |
| MSG_AUTH | 7 | Клиент → Сервер | Аутентификация (ник в payload) |
| MSG_PRIVATE | 8 | Клиент ↔ Сервер ↔ Клиент | Личное сообщение |
| MSG_ERROR | 9 | Сервер → Клиент | Описание ошибки |
| MSG_SERVER_INFO | 10 | Сервер → Клиент(ы) | Служебное уведомление |
| MSG_LIST | 11 | Клиент → Сервер | Запрос списка пользователей |
| MSG_HISTORY | 12 | Клиент → Сервер | Запрос истории |
| MSG_HISTORY_DATA | 13 | Сервер → Клиент | Строка из истории |
| MSG_HELP | 14 | — | Справка (обрабатывается локально) |

---

## 3. Архитектура сервера

### 3.1 Пул потоков (Thread Pool)

Сервер использует **пул из 10 рабочих потоков** вместо того, чтобы создавать поток на каждое подключение. Это эффективнее: создание потока — дорогая операция, а при большом числе соединений создание/удаление потоков деградирует производительность.

**Схема работы:**

```
Главный поток:
  accept() → новый сокет → кладём в очередь client_queue → сигнал queue_cond

Рабочий поток (×10):
  ждёт на pthread_cond_wait()
  ← получает сигнал → берёт сокет из очереди
  → обрабатывает клиента до разрыва соединения
  → удаляет клиента → ждёт следующего
```

```cpp
// server.cpp: main()
for (int i = 0; i < THREAD_POOL_SIZE; ++i)
    pthread_create(&threads[i], nullptr, worker_thread, nullptr);

// Главный цикл
while (true) {
    client_sock = ::accept(...);
    pthread_mutex_lock(&queue_mutex);
    client_queue.push(client_sock);
    pthread_cond_signal(&queue_cond);   // будим один поток
    pthread_mutex_unlock(&queue_mutex);
}
```

---

### 3.2 Синхронизация (Mutex)

Поскольку несколько потоков работают одновременно, доступ к общим данным защищён мьютексами:

| Мьютекс | Что защищает |
|---|---|
| `clients_mutex` | `active_clients` — список подключённых клиентов |
| `queue_mutex` + `queue_cond` | `client_queue` — очередь новых сокетов |
| `offline_mutex` | `offline_queue` — очередь офлайн-сообщений |
| `history_mutex` | `history_records` — история сообщений |
| `id_mutex` | `next_msg_id` — счётчик идентификаторов |

> **Вопрос:** «Почему нельзя обойтись без мьютексов?»  
> **Ответ:** Два потока могут одновременно добавлять или читать из `active_clients`. Без синхронизации это **гонка данных** (data race) — неопределённое поведение: порча памяти, краш, неправильные значения. Мьютекс гарантирует, что в критической секции находится только один поток.

---

### 3.3 Алгоритм обработки клиента

Каждый рабочий поток выполняет следующую последовательность:

```
1. recv MSG_HELLO
   └─ если нет/не тот тип → close socket, перейти к следующему клиенту

2. send MSG_WELCOME

3. recv MSG_AUTH (в цикле, игнорируем всё остальное до авторизации)
   ├─ пустой/длинный ник → MSG_ERROR → close
   ├─ ник уже занят     → MSG_ERROR → close
   └─ OK → add_client() + broadcast "User [X] connected"

4. deliver_offline_messages() → отправляем накопленные сообщения

5. Основной цикл:
   recv MessageEx
   ├─ MSG_TEXT     → allocate_id, save history, broadcast всем
   ├─ MSG_PRIVATE  → если онлайн: send + echo; если офлайн: очередь + history(delivered=false)
   ├─ MSG_LIST     → список активных ников → MSG_SERVER_INFO
   ├─ MSG_HISTORY  → последние N записей → N×MSG_HISTORY_DATA
   ├─ MSG_PING     → MSG_PONG
   └─ MSG_BYE      → break

6. remove_client() → broadcast "User [X] disconnected"
```

---

## 4. Store & Forward (Офлайн-очередь)

### 4.1 Принцип

Store & Forward — классический принцип передачи данных: если получатель недоступен, **сохраняем** сообщение и **доставляем** при следующей возможности. Именно так работает электронная почта (SMTP).

### 4.2 Структуры данных

```cpp
struct OfflineMsg {
    std::uint32_t msg_id;
    std::int64_t  timestamp;
    std::string   sender;
    std::string   receiver;
    std::string   text;
};

// Карта: ник_получателя → очередь сообщений
std::unordered_map<std::string, std::vector<OfflineMsg>> offline_queue;
```

### 4.3 Сохранение офлайн-сообщения

Когда клиент отправляет `/w Charlie text`, а Charlie не в сети:

```cpp
// server.cpp: обработка MSG_PRIVATE
if (get_sock_by_nickname(receiver, target_sock)) {
    // получатель онлайн — отправляем немедленно
    send_message_tcpip(target_sock, MSG_PRIVATE, ...);
} else {
    // получатель офлайн — сохраняем в очередь
    offline_queue[receiver].push_back(om);
    
    // в историю: delivered=false, is_offline=true
    rec.delivered = false;
    rec.is_offline = true;
    append_history_record(rec);
    
    // уведомляем отправителя
    send "receiver Charlie is offline, message stored"
}
```

### 4.4 Доставка при подключении

Сразу после успешной аутентификации:

```cpp
// server.cpp: функция deliver_offline_messages()
auto it = offline_queue.find(nickname);
if (it != offline_queue.end()) {
    pending = std::move(it->second);  // забираем все сообщения
    offline_queue.erase(it);
}

for (const auto& om : pending) {
    // добавляем префикс OFFLINE: к тексту
    const std::string payload = "OFFLINE:" + om.text;
    send_message_tcpip(sock, MSG_PRIVATE, om.msg_id, om.sender, ...);
    mark_history_delivered(om.msg_id);  // помечаем как доставленное
}
```

Клиент при получении MSG_PRIVATE с `OFFLINE:` в начале payload форматирует иначе:

```
[2026-04-07 15:10:00][id=57][OFFLINE][Alice -> Charlie]: Привет
```

### 4.5 Восстановление очереди при перезапуске сервера

При старте сервер загружает `history.json` и восстанавливает офлайн-очередь из записей с `is_offline=true` и `delivered=false`:

```cpp
// server.cpp: main()
load_history_file(HISTORY_FILE);
init_next_msg_id_from_history();    // следующий ID = max(id) + 1
rebuild_offline_queue_from_history(); // восстановить очередь
```

> **Вопрос:** «Что будет с офлайн-сообщениями если сервер перезапустить?»  
> **Ответ:** Они сохранены в history.json с `delivered=false`. При старте `rebuild_offline_queue_from_history()` читает историю и заново заполняет `offline_queue`. Сообщения не теряются.

---

## 5. История сообщений в JSON

### 5.1 Формат записи

```json
{
  "msg_id": 42,
  "timestamp": 1712493320,
  "sender": "Alice",
  "receiver": "",
  "type": "MSG_TEXT",
  "text": "Hello",
  "delivered": true,
  "is_offline": false
}
```

Поля:
- `msg_id` — уникальный монотонно возрастающий идентификатор
- `timestamp` — Unix timestamp (секунды с 01.01.1970 UTC)
- `type` — строковое имя типа (через `message_type_name()`)
- `delivered` — доставлено ли сообщение получателю
- `is_offline` — было ли сообщение в очереди Store & Forward

### 5.2 Атомарная запись файла

Файл записывается через временный файл, чтобы не повредить историю при сбое:

```cpp
// server.cpp: write_history_file_locked()
const std::string tmp = path + ".tmp";
// 1. Записываем в history.json.tmp
std::ofstream out(tmp);
out << json_content;
// 2. Удаляем старый history.json
std::remove(path.c_str());
// 3. Переименовываем .tmp → history.json
std::rename(tmp.c_str(), path.c_str());
```

Если программа упадёт в момент записи, останется `.tmp` файл, а старый `history.json` будет либо цел, либо отсутствовать (но не повреждён наполовину).

### 5.3 Собственный JSON-парсер

В коде реализован минимальный ручной парсер JSON (без сторонних библиотек):
- `escape_json_string()` — экранирует спецсимволы при записи
- `parse_json_string()` — читает JSON-строку в кавычках
- `parse_uint64()`, `parse_int64()`, `parse_bool()` — читают примитивы
- `find_key_pos()` — находит позицию значения по ключу
- `parse_history_object()` — разбирает один JSON-объект целиком

> **Вопрос:** «Почему не использовали стандартную JSON-библиотеку?»  
> **Ответ:** Задание требует писать на чистом C++ без зависимостей. Реализованный парсер обрабатывает ровно тот формат, который сам же и генерирует.

---

### 5.4 Генерация уникальных ID

```cpp
std::uint32_t allocate_msg_id() {
    pthread_mutex_lock(&id_mutex);
    std::uint32_t id = next_msg_id++;
    pthread_mutex_unlock(&id_mutex);
    return id;
}
```

`next_msg_id` инициализируется при старте как `max(msg_id из истории) + 1`, чтобы не было конфликтов после перезапуска.

---

## 6. Логирование по модели TCP/IP

### 6.1 Входящее сообщение (log_incoming)

```cpp
void log_incoming(int sock, const MessageEx& msg) {
    // Размер = заголовок + payload
    std::size_t bytes = sizeof(MessageExWireHeader) + msg.length;

    // Логируем снизу вверх (от физики к приложению)
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP\n";
    std::cout << "[Internet] src=" << ip_of(peer) << " dst=" << ip_of(local) << " proto=TCP\n";
    std::cout << "[Network Access] frame received via network interface\n";
    std::cout << "[Application] deserialize MessageEx -> " << message_type_name(msg.type) << "\n";
}
```

Порядок вывода в логе: Transport → Internet → Network Access → Application.  
Это отражает де-инкапсуляцию: данные «разворачиваются» снизу вверх.

### 6.2 Исходящее сообщение (log_outgoing)

```cpp
void log_outgoing(int sock, uint8_t type, size_t payload_len, const string& action) {
    // Логируем сверху вниз (от приложения к физике)
    std::cout << "[Application] " << action << " -> " << message_type_name(type) << "\n";
    std::cout << "[Transport] send() " << bytes << " bytes via TCP\n";
    std::cout << "[Internet] src=" << ip_of(local) << " dst=" << ip_of(peer) << " proto=TCP\n";
    std::cout << "[Network Access] frame sent to network interface\n";
}
```

Порядок: Application → Transport → Internet → Network Access.  
Это инкапсуляция: данные «оборачиваются» сверху вниз.

### 6.3 Обёртки с логированием

```cpp
bool recv_message_tcpip(int sock, MessageEx& msg) {
    if (!recv_message_ex(sock, msg)) return false;
    log_incoming(sock, msg);  // логируем после успешного чтения
    return true;
}

bool send_message_tcpip(int sock, ..., const string& action) {
    log_outgoing(sock, type, payload_len, action);  // логируем перед отправкой
    return send_message_ex(sock, type, ...);
}
```

---

## 7. Клиент: архитектура и команды

### 7.1 Двухпоточная модель клиента

Клиент использует два потока:
- **Основной поток:** читает ввод пользователя с `std::getline()`, разбирает команды, отправляет сообщения серверу
- **Поток приёма (`receive_thread`):** постоянно слушает сокет, выводит входящие сообщения

```cpp
// client.cpp
pthread_create(&recv_tid, nullptr, receive_thread, args);

while (is_connected.load()) {
    std::getline(std::cin, input);
    // обработка команды → send_message_ex(...)
}
```

Флаг `std::atomic<bool> is_connected` синхронизирует оба потока без мьютекса — атомарные операции безопасны по определению.

### 7.2 Начальный обмен (handshake)

```
Клиент                          Сервер
   |── MSG_HELLO ──────────────→ |
   |← MSG_WELCOME ───────────── |
   |── MSG_AUTH (ник в payload)→ |
   |← MSG_SERVER_INFO ─────────  |  ("User [X] connected")
   |← MSG_PRIVATE* ────────────  |  (офлайн-сообщения, если есть)
```

### 7.3 Разбор команды `/w` (whisper)

```cpp
bool parse_whisper_cmd(const string& input, string& nick, string& message) {
    // input = "/w Charlie Привет"
    string rest = input.substr(3);   // "Charlie Привет"
    size_t space = rest.find(' ');   // позиция пробела = 7
    nick    = rest.substr(0, space); // "Charlie"
    message = rest.substr(space+1);  // "Привет"
    return !(nick.empty() || message.empty());
}
```

Затем отправляется `MSG_PRIVATE` с `receiver = "Charlie"` в заголовке и `"Привет"` в payload.

### 7.4 Разбор команды `/history`

```cpp
bool parse_history_cmd(const string& input, string& payload) {
    if (input == "/history") {        // без аргумента
        payload = "";
        return true;
    }
    // "/history 10" → payload = "10"
    string rest = input.substr(strlen("/history "));
    for (char ch : rest)              // проверяем что все символы — цифры
        if (ch < '0' || ch > '9') return false;
    if (rest == "0") return false;    // 0 недопустим
    payload = rest;
    return true;
}
```

На сервере `handle_history_request()` читает `req.payload` как число `N` и возвращает последние `N` записей из `history_records`.

### 7.5 Переподключение

При потере связи (`recv()` вернул 0 или ошибку) клиент не завершается, а ждёт 2 секунды и переподключается:

```cpp
while (true) {  // основной цикл переподключения
    sock = ::socket(...);
    ::connect(...);
    // ... работаем ...
    ::close(sock);
    if (quit_requested) break;
    sleep(2);
    // следующая итерация = переподключение
}
```

---

## 8. Форматирование вывода сообщений

### 8.1 Временная метка

```cpp
// protocol.hpp
std::string format_timestamp(std::int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);         // thread-safe версия localtime
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}
```

`localtime_r()` — reentrant (повторно входимая) версия `localtime()`, безопасная в многопоточном коде.

### 8.2 Форматы строк

```
Обычное сообщение:
[2026-04-07 14:35:20][id=42][Alice]: Hello

Личное сообщение:
[2026-04-07 14:36:01][id=43][PRIVATE][Bob -> Alice]: Hi

Офлайн-сообщение:
[2026-04-07 15:10:00][id=57][OFFLINE][Bob -> Alice]: Не забудь отчёт

Сообщение сервера:
[SERVER]: User [Charlie] connected
```

---

## 9. Ключевые вопросы и ответы

**Q: Чем TCP отличается от UDP?**  
A: TCP — с установлением соединения, гарантирует доставку, порядок и целостность данных. UDP — без соединения, ненадёжный, но быстрый. В нашем чате нужна гарантия доставки → TCP.

**Q: Что такое сокет?**  
A: Абстракция ОС, представляющая конечную точку TCP-соединения. Идентифицируется четвёркой (src_ip, src_port, dst_ip, dst_port). `socket()` создаёт дескриптор, `bind()` привязывает адрес, `listen()` переводит в режим ожидания, `accept()` принимает подключение, `connect()` инициирует его.

**Q: Зачем `SO_REUSEADDR`?**  
A: После закрытия сервера порт остаётся в состоянии `TIME_WAIT` несколько секунд. Без `SO_REUSEADDR` повторный `bind()` на тот же порт упадёт с ошибкой. Опция разрешает переиспользование адреса.

**Q: Почему `send()` может отправить не все байты?**  
A: TCP имеет буфер отправки в ядре ОС. Если буфер заполнен (например, получатель читает медленно), `send()` вернёт столько байт, сколько поместилось. Поэтому нужен цикл в `send_all()`.

**Q: Что такое `INADDR_ANY`?**  
A: Специальный адрес `0.0.0.0`, означающий «принимать подключения на всех сетевых интерфейсах» (Wi-Fi, Ethernet, loopback). Если указать конкретный IP, сервер будет слушать только на этом интерфейсе.

**Q: Что происходит при разрыве соединения?**  
A: `recv()` возвращает 0 (корректное закрытие со стороны клиента) или отрицательное значение (ошибка). В `recv_message_tcpip()` это означает `return false`. Сервер выходит из основного цикла, вызывает `remove_client()`, который удаляет клиента из списка, закрывает сокет через `shutdown(SHUT_RDWR)` + `close()`, и рассылает уведомление об отключении.

**Q: В чём разница между `shutdown()` и `close()`?**  
A: `shutdown(SHUT_RDWR)` запрещает дальнейшие операции чтения/записи в сокет (но не освобождает дескриптор) — это разбудит заблокированный `recv()` в другом потоке. `close()` освобождает файловый дескриптор. Использование обоих вместе гарантирует корректное завершение.

**Q: Почему msg_id атомически инкрементируется?**  
A: Несколько рабочих потоков могут одновременно получить новое сообщение и вызвать `allocate_msg_id()`. Без мьютекса два потока могут прочитать одно и то же значение `next_msg_id`, и два разных сообщения получат одинаковый ID. Мьютекс `id_mutex` делает операцию «прочитать и увеличить» атомарной.

**Q: Как сервер находит сокет получателя при личном сообщении?**  
A: Функция `get_sock_by_nickname()` перебирает `active_clients` под `clients_mutex` и ищет совпадение по полю `nickname`. Возвращает файловый дескриптор сокета, на который затем отправляется `MSG_PRIVATE`.

**Q: Что значит `#pragma pack(1)` и зачем?**  
A: Директива компилятора, отменяющая выравнивание полей структуры. По умолчанию компилятор вставляет паддинг-байты между полями для выравнивания по границе слова (4 или 8 байт). `#pragma pack(1)` убирает паддинг — каждое поле идёт сразу после предыдущего. Без неё `sizeof(MessageExWireHeader)` на разных компиляторах/платформах было бы разным, и протокол сломался бы.

**Q: Чем `std::atomic<bool>` лучше обычного `bool` с мьютексом?**  
A: Для простого чтения/записи одного булевого значения атомарная операция быстрее — она реализуется аппаратными инструкциями CPU (например, `LOCK XCHG`), без накладных расходов на системный вызов мьютекса.
