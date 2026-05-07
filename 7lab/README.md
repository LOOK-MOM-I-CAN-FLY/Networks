# Теория к лабораторной работе №7
## Защищённое сетевое взаимодействие: интеграция SSL/TLS, сертификаты и шифрование трафика

---

## 0. Что было в ЛР6 и что мы ломаем

В ЛР6 чат работал поверх «голого» TCP. Трафик можно было снять Wireshark-ом — никнеймы, тексты, ID, ACK — всё было видно в открытом виде. ЛР7 закрывает эту дыру: над TCP появляется ещё один уровень — **TLS**. Прикладной протокол `MessageEx` остался **без изменений по содержанию**, но байты этого сообщения теперь не уходят в сокет напрямую — они шифруются OpenSSL-ом и идут через `SSL_write`, а на стороне получателя расшифровываются `SSL_read`.

Точка изменения — узкая и хирургическая:

| Уровень | ЛР6 | ЛР7 |
|---|---|---|
| Application | `MessageEx`, типы 1..15 | + `MSG_TLS_INFO`, `MSG_SECURE_ERROR` |
| Transport (надёжность приложения) | ACK + retry + dedup | то же |
| **Security** | — | **TLS-handshake, шифрование, сертификат** |
| Transport (TCP) | `send/recv` | `SSL_write/SSL_read` поверх TCP |
| Internet/Link | TCP/IP | TCP/IP |

Всё, что мы добавляем, живёт **между** прикладным слоем (`send_message_ex`) и TCP-сокетом.

---

## 1. Зачем нужен TLS поверх TCP

TCP гарантирует **порядок и доставку байтов**. Он не решает три ортогональные проблемы:

1. **Конфиденциальность** — всякий, кто может видеть трафик (роутер, провайдер, человек на Wi-Fi), читает наши сообщения.
2. **Целостность** — TCP-чексумма 16-битная и вероятностная: проксирующий узел или активный атакующий может изменить байты, и приёмная сторона не заметит.
3. **Подлинность собеседника** — клиент не знает, что подключился именно к «нашему» серверу, а не к подменённому через DNS-spoofing.

TLS закрывает все три:

- симметричное шифрование (после рукопожатия) — конфиденциальность;
- HMAC/AEAD каждого TLS-record — целостность;
- сертификат + подпись приватным ключом — аутентичность сервера.

> Важно: TLS — это **отдельный логический протокол** между TCP и приложением. TCP остаётся как был, мы просто перестаём писать в сокет байты приложения и начинаем писать туда TLS-records, в которые упакованы наши байты.

---

## 2. TLS-рукопожатие — что происходит на проводе

При установке защищённого соединения OpenSSL обменивается серией сообщений:

```
Client                                         Server
   |                                              |
   | --- TCP SYN/SYN-ACK/ACK -------------------> |   обычный TCP-handshake
   | <------------------------------------------- |
   |                                              |
   | --- ClientHello (cipher suites, random) ---> |
   | <-- ServerHello (chosen cipher, random) ---- |
   | <-- Certificate (X.509 server cert) -------- |
   | <-- ServerKeyExchange / Finished ------------|
   | --- ClientKeyExchange / Finished ----------> |
   |                                              |
   | === шифрованный канал установлен =========== |
   | --- Encrypted application data <--------> -- |
```

Для нас (с точки зрения кода) весь этот обмен спрятан внутри двух функций:

- `SSL_accept(ssl)` — на сервере: ждёт `ClientHello`, отвечает, проводит весь обмен ключами;
- `SSL_connect(ssl)` — на клиенте: инициирует `ClientHello`, проверяет сертификат, завершает рукопожатие.

После их успешного возврата сокет «выглядит» как TCP-сокет, но любая попытка читать/писать через `send/recv` сломает соединение. Использовать можно только `SSL_read/SSL_write`.

В нашем коде:

```cpp
// server.cpp, worker_thread:
std::cout << "[Security][TLS] handshake started\n";
int ar = SSL_accept(ssl);
if (ar <= 0) {
    std::cout << "[Security][TLS] handshake failed\n";
    ERR_print_errors_fp(stderr);
    close_client_conn(client_sock, ssl);
    continue;
}
std::cout << "[Security][TLS] handshake success\n";
log_cipher_info(ssl);
std::cout << "[Security][ENC] encrypted channel established\n";
```

```cpp
// client.cpp:
std::cout << "[Security][TLS] handshake started\n";
if (SSL_connect(ssl) <= 0) {
    std::cout << "[Security][TLS] handshake failed\n";
    ERR_print_errors_fp(stderr);
    SSL_free(ssl);
    ::close(sock);
    sleep(2);
    continue;
}
std::cout << "[Security][TLS] handshake success\n";
log_cipher_info(ssl);
log_peer_certificate(ssl);
```

`log_cipher_info` сразу после рукопожатия печатает выбранный шифр и версию TLS — например `TLSv1.3 / TLS_AES_256_GCM_SHA384`. Это полезно как доказательство того, что канал реально зашифрован.

---

## 3. Сертификат и приватный ключ

### 3.1 Что это такое

- **`server.key`** — приватный ключ сервера (RSA-2048 в нашей лабе). Им сервер расшифровывает / подписывает блоки рукопожатия. Никогда не уходит в сеть. Должен лежать только на сервере.
- **`server.crt`** — публичный сертификат сервера. Контейнер X.509: содержит публичный ключ, имя хоста (CN), срок действия, подпись. Уходит клиенту в сообщении `Certificate` рукопожатия.

В реальном мире `server.crt` подписан доверенным CA (Let's Encrypt, DigiCert и т.п.) — клиент сверяет цепочку с системным trust store. Для лабораторной работы мы подписываем сам себя — **самоподписанный сертификат**.

### 3.2 Генерация

```bash
openssl req -x509 -newkey rsa:2048 \
    -keyout server.key \
    -out server.crt \
    -days 365 \
    -nodes \
    -subj "/C=RU/ST=Local/L=Local/O=Lab7/OU=Dev/CN=localhost"
```

Параметры:

| Параметр | Что значит |
|---|---|
| `req -x509` | сразу генерируем самоподписанный X.509-сертификат, минуя CSR |
| `-newkey rsa:2048` | новый RSA-ключ длиной 2048 бит |
| `-keyout server.key` | путь для приватного ключа |
| `-out server.crt` | путь для сертификата |
| `-days 365` | срок жизни сертификата |
| `-nodes` | «no DES» — не шифровать приватный ключ паролем (для лабы удобно; в проде — обязательно с паролем) |
| `-subj "..."` | поля Subject в формате X.500. CN должен совпадать с тем, что использует клиент при подключении |

В нашем `Makefile` есть цель `make certs`, которая выполняет ровно эту команду.

### 3.3 Загрузка в `SSL_CTX`

Сервер загружает обе вещи в контекст и проверяет, что они подходят друг другу:

```cpp
// server.cpp, create_server_ctx:
if (SSL_CTX_use_certificate_file(ctx, opts.cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) { ... }
std::cout << "[Security][CERT] certificate loaded: " << opts.cert_path << "\n";

if (SSL_CTX_use_PrivateKey_file(ctx, opts.key_path.c_str(), SSL_FILETYPE_PEM) <= 0) { ... }
std::cout << "[Security][CERT] private key loaded: " << opts.key_path << "\n";

if (SSL_CTX_check_private_key(ctx) != 1) {
    std::cerr << "[Security][CERT] private key does not match certificate\n";
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(ctx);
    return nullptr;
}
std::cout << "[Security][CERT] key/certificate match verified\n";
```

`SSL_CTX_check_private_key` — это локальная криптографическая проверка: математически сходится ли публичный ключ из `server.crt` с приватным ключом из `server.key`. Если сертификат сгенерили заново, а ключ забыли подменить — выявится здесь, ещё до первого клиента.

### 3.4 Проверка сертификата на стороне клиента

Минимальный вариант, который мы реализуем:

```cpp
// client.cpp, log_peer_certificate:
X509* cert = SSL_get_peer_certificate(ssl);
if (!cert) {
    std::cout << "[Security][CERT] no certificate\n";
    return;
}
std::cout << "[Security][CERT] server certificate received\n";

char* subject = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
char* issuer  = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
std::cout << "[Security][CERT] subject: " << subject << "\n";
std::cout << "[Security][CERT] issuer : " << issuer << "\n";
OPENSSL_free(subject);
OPENSSL_free(issuer);
X509_free(cert);
```

Мы:
- получаем сертификат сервера;
- логируем Subject и Issuer (для самоподписанного они совпадают — это ожидаемо);
- освобождаем память (`X509_free`, `OPENSSL_free` — типовое OpenSSL-владение, нельзя `delete`).

Что мы **сознательно не делаем**: не вызываем `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, ...)` и не загружаем CA-bundle. То есть клиент принимает любой валидный X.509 без проверки цепочки. Это типичный компромисс лабораторных самоподписанных стендов; в продакшене пишется `SSL_CTX_load_verify_locations(ctx, ...)` и `SSL_VERIFY_PEER`.

---

## 4. Архитектура: где SSL «прорастает» в код ЛР6

Главный приём — точечная замена пары `(int sock)` → `(SSL* ssl)` всюду, где раньше шла отправка/приём сообщений приложения.

### 4.1 protocol.hpp: единственный слой, который знает про шифрование

Раньше функции `send_message_ex / recv_message_ex` принимали `int sock` и работали через `send/recv` напрямую. Теперь — принимают `SSL*` и работают через `SSL_write/SSL_read`:

```cpp
// protocol.hpp
inline bool ssl_send_all(SSL* ssl, const void* data, std::size_t size) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        int sent = SSL_write(ssl, ptr, static_cast<int>(size));
        if (sent <= 0) {
            return false;
        }
        ptr += sent;
        size -= sent;
    }
    return true;
}

inline bool ssl_recv_all(SSL* ssl, void* data, std::size_t size) {
    char* ptr = static_cast<char*>(data);
    while (size > 0) {
        int got = SSL_read(ssl, ptr, static_cast<int>(size));
        if (got <= 0) {
            return false;
        }
        ptr += got;
        size -= got;
    }
    return true;
}
```

Сами `send_message_ex / recv_message_ex` остались с прежней структурой (заголовок в network-order + payload), просто внутри они теперь зовут `ssl_send_all/ssl_recv_all`:

```cpp
// protocol.hpp
inline bool send_message_ex(SSL* ssl, std::uint8_t type, std::uint32_t msg_id,
                            const std::string& sender, const std::string& receiver,
                            std::int64_t timestamp, const std::string& payload) {
    if (!ssl) return false;
    // ... формируем MessageExWireHeader как в ЛР6 ...
    if (!ssl_send_all(ssl, &header, sizeof(header))) return false;
    if (payload_len == 0) return true;
    return ssl_send_all(ssl, payload.data(), payload_len);
}
```

Это и есть инкапсуляция: всё, что выше (broadcast, ACK, retry, dedup, история, ping, /list) — продолжает писать на C++-уровне как раньше, не зная о шифровании.

### 4.2 server.cpp: жизненный цикл одного клиента

```
accept()  →  SSL_new()  →  SSL_set_fd()  →  кладём в очередь
                                               ↓
worker_thread:
   SSL_accept()                ← TLS-рукопожатие
   ↓
   recv_message_ex(ssl, ...)   ← MSG_HELLO
   send_message_ex(ssl, ...)   ← MSG_WELCOME
   send_message_ex(ssl, ...)   ← MSG_TLS_INFO
   ↓
   цикл: MSG_AUTH / MSG_TEXT / MSG_PRIVATE / ... — всё через ssl
   ↓
   SSL_shutdown() + SSL_free() + close(sock)
```

Главный кусок:

```cpp
// server.cpp, accept-цикл в main():
int client_sock = ::accept(server_sock, ...);
std::cout << "[Transport] TCP connection accepted from " << ipport_of(client_addr) << "\n";

SSL* ssl = SSL_new(g_ssl_ctx);
SSL_set_fd(ssl, client_sock);
std::cout << "[Security][TLS] SSL object created\n";

ClientConn cc{client_sock, ssl};
client_queue.push(cc);   // под мьютексом + condvar — как в ЛР6
```

Заметь: `SSL_accept` мы **не** делаем в главном потоке. Рукопожатие — потенциально долгая, синхронная операция, она блокировала бы `accept` нового клиента. Поэтому SSL-объект создаётся в `main`, а само рукопожатие выполняется в воркере, как и в ЛР6 авторизация делалась в воркере.

`active_clients` теперь хранит и `int sock`, и `SSL* ssl`. `sock` нужен ради `getsockname/getpeername` для логов уровня `[Internet]`; `ssl` — единственный путь общения с клиентом.

### 4.3 client.cpp: рукопожатие до прикладного `MSG_HELLO`

```cpp
// client.cpp, цикл переподключений в main():
::connect(sock, ...);
SSL* ssl = SSL_new(ctx);
SSL_set_fd(ssl, sock);

if (SSL_connect(ssl) <= 0) {
    // не повезло — даже не пытаемся отправлять MSG_HELLO
    SSL_free(ssl); ::close(sock); sleep(2); continue;
}
log_cipher_info(ssl);
log_peer_certificate(ssl);

// и только теперь начинается прежний прикладной handshake из ЛР6:
send_message_ex(ssl, MSG_HELLO, ...);
recv_message_ex(ssl, welcome);
send_message_ex(ssl, MSG_AUTH, ...);
```

Все потоки клиента — `receive_thread`, `retry_thread` — теперь хранят `SSL*` вместо `int sock` и читают/пишут только через `recv_message_ex/send_message_ex`. Это даёт корректность «из коробки»: если мы где-то забудем `SSL*` и попытаемся прокинуть `int sock`, не скомпилируется.

---

## 5. Расширение протокола: MSG_TLS_INFO и MSG_SECURE_ERROR

```cpp
// protocol.hpp
enum MessageType : std::uint8_t {
    // ... 1..15 как в ЛР6 ...
    MSG_TLS_INFO     = 16,
    MSG_SECURE_ERROR = 17
};
```

### 5.1 MSG_TLS_INFO

После успешного `SSL_accept` сервер шлёт клиенту строковое описание защищённой сессии:

```cpp
// server.cpp, после welcome:
(void)send_message_tcpip(client_sock, ssl, MSG_TLS_INFO, 0, "SERVER", "", std::time(nullptr),
                         std::string("TLS=") + (SSL_get_version(ssl) ? SSL_get_version(ssl) : "?") +
                         ",cipher=" + (SSL_get_cipher(ssl) ? SSL_get_cipher(ssl) : "?"),
                         "send tls info");
```

Клиент при получении выводит, что сервер сообщил:

```cpp
// client.cpp, receive_thread:
} else if (msg.type == MSG_TLS_INFO) {
    std::cout << "[Security][TLS] server info: " << msg.payload << "\n";
}
```

Дополнительно есть команда `/tlsinfo`, которая печатает версию TLS и Subject/Issuer сертификата, известные клиенту локально.

### 5.2 MSG_SECURE_ERROR

Зарезервирован под ситуации, когда сервер хочет сообщить клиенту о проблеме именно безопасностного уровня (например, истёк сертификат, или клиент шлёт что-то противоречащее политике). В текущей реализации сервер его не отсылает, но клиент уже умеет принимать и отрисовывать:

```cpp
// client.cpp, receive_thread:
} else if (msg.type == MSG_SECURE_ERROR) {
    std::cout << "[Security][TLS] error: " << msg.payload << "\n";
}
```

---

## 6. Логирование: новый уровень `[Security]`

В ЛР6 был набор префиксов уровней TCP/IP: `[Application] / [Transport] / [Internet] / [Network Access]` и поднабор `[Transport][PING]`, `[Transport][SIM]`, `[Transport][RETRY]`, `[Transport][ACK]`. В ЛР7 добавляется новый уровень — **между Application и Transport**:

| Префикс | Когда |
|---|---|
| `[Security][TLS]` | инициализация OpenSSL, создание контекста, рукопожатие, версия/шифр |
| `[Security][CERT]` | загрузка сертификата/ключа, получение peer-сертификата, Subject/Issuer |
| `[Security][ENC]` | каждое `SSL_read`/`SSL_write`, статус «канал установлен» |

Пример полного цикла приёма одного `MSG_TEXT` на сервере:

```
[Transport] TCP connection accepted from 127.0.0.1:54123
[Security][TLS] SSL object created
[Security][TLS] handshake started
[Security][TLS] handshake success
[Security][TLS] cipher=TLS_AES_256_GCM_SHA384 protocol=TLSv1.3
[Security][ENC] encrypted channel established

[Security][ENC] SSL_read 81 bytes (encrypted)
[Transport] recv() 81 plaintext bytes via TLS
[Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP+TLS
[Network Access] frame received via network interface
[Application] deserialize MessageEx -> MSG_TEXT

[Application][ACK] process MSG_TEXT (id=41)
[Application] broadcast text -> MSG_TEXT
[Security][ENC] SSL_write 81 bytes (encrypted)
[Transport] send() 81 plaintext bytes via TLS
...
[Application] send ack -> MSG_ACK
[Transport][ACK] send MSG_ACK (id=41)
[Security][ENC] SSL_write 64 bytes (encrypted)
```

И симметрично на клиенте:

```
[Transport] connecting to 127.0.0.1:5555...
[Transport] connected to 127.0.0.1:5555
[Security][TLS] handshake started
[Security][TLS] handshake success
[Security][TLS] cipher=TLS_AES_256_GCM_SHA384 protocol=TLSv1.3
[Security][CERT] server certificate received
[Security][CERT] subject: /C=RU/ST=Local/L=Local/O=Lab7/OU=Dev/CN=localhost
[Security][CERT] issuer : /C=RU/ST=Local/L=Local/O=Lab7/OU=Dev/CN=localhost
[Security][ENC] encrypted channel established
...
> hello
[Transport][RETRY] send MSG_TEXT (id=41)
[Security][ENC] SSL_write MSG_TEXT (id=41)
[Security][ENC] SSL_read MSG_ACK (id=41)
[Transport][RETRY] ACK received (id=41)
```

Цель — наглядно показать, что между прикладной сериализацией и физической отправкой по TCP действительно есть отдельный шифрующий слой.

---

## 7. Взаимодействие SSL с «надёжностью» из ЛР6

Главный риск рефакторинга: не сломать механизмы из ЛР6 — ACK, retry, dedup, ping. Они должны работать ровно как раньше. Что мы сделали, чтобы сохранить корректность:

### 7.1 ACK / retry

Очередь `pending_queue`, фоновый `retry_thread`, обработка `MSG_ACK` в `handle_ack` — остались как были. Изменилась только сигнатура `send_reliable`: теперь принимает `SSL*`, а не `int sock`:

```cpp
// client.cpp
bool send_reliable(SSL* ssl, std::uint8_t type, std::uint32_t msg_id, ...) {
    pending_queue.push_back(pm);
    std::cout << "[Transport][RETRY] send " << ... << "\n";
    std::cout << "[Security][ENC] SSL_write " << ... << "\n";
    return send_message_ex(ssl, type, msg_id, ...);
}
```

Нужно учесть нюанс: TLS-record может фрагментировать данные на проводе, но `SSL_write` блокирующе отдаёт всё целиком (либо ошибку), как и `send`. Значит, поведение «послали → ждём ACK 2 секунды → retry» полностью сохраняется.

### 7.2 Дедупликация

Окно `last_ids[32]` в воркере сервера — без изменений. Дубль приходит, сервер видит совпадение по `msg_id`, шлёт `MSG_ACK` и идёт дальше. Шифрование не влияет на проверку — она идёт по уже расшифрованным `MessageEx`.

### 7.3 PING / RTT

`SteadyClock::now()` фиксируется в момент `SSL_write(MSG_PING)`. RTT теперь включает в себя:

- сериализацию `MessageEx`,
- шифрование TLS-record,
- TCP-round trip,
- расшифровку на сервере,
- обработку (мгновенная — мы сразу отвечаем `MSG_PONG`),
- обратную дорогу с тем же шифрованием.

Поэтому абсолютные значения RTT в ЛР7 будут чуть выше, чем в ЛР6 на том же localhost — TLS добавляет 50–200 мкс на пакет в среднем, что заметно на масштабах локальных пингов (RTT ~0.5 мс). Это и есть «цена» шифрования, и она ровно та, что описывают учебники.

### 7.4 Эмуляция помех `--delay/--drop/--corrupt`

`apply_sim` работает на **расшифрованном** `MessageEx`. Это правильно: эмулируется потеря/задержка/порча на прикладном уровне, как если бы они произошли в сети. Если бы мы дропали зашифрованные TLS-records напрямую, TLS-сессия бы развалилась полностью — `SSL_read` вернул бы фатальную ошибку и работа бы прекратилась. Сейчас же `--drop=0.5` ведёт себя ровно как в ЛР6: половина прикладных сообщений «теряется», но TLS-канал жив.

---

## 8. Жизненный цикл OpenSSL: что куда зовём

| Где | Когда | Зачем |
|---|---|---|
| `SSL_load_error_strings()` + `SSL_library_init()` + `OpenSSL_add_all_algorithms()` | один раз в `main` (через `openssl_global_init`) | глобальная инициализация: таблицы ошибок, алгоритмы шифрования |
| `SSL_CTX_new(TLS_server_method())` / `TLS_client_method()` | один раз в `main` | контекст с настройками протокола; разделяется между всеми соединениями |
| `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)` | сразу после создания ctx | запретить TLS 1.0/1.1, разрешить только 1.2 и 1.3 |
| `SSL_CTX_use_certificate_file` + `SSL_CTX_use_PrivateKey_file` + `SSL_CTX_check_private_key` | сервер, после создания ctx | загрузить материал для рукопожатия |
| `SSL_new(ctx)` + `SSL_set_fd(ssl, sock)` | для каждого нового соединения | привязать SSL-объект к TCP-сокету |
| `SSL_accept(ssl)` / `SSL_connect(ssl)` | сервер / клиент | TLS-рукопожатие |
| `SSL_read` / `SSL_write` | каждое сообщение | обмен данными |
| `SSL_shutdown(ssl)` | при завершении соединения | вежливый close-notify; за один вызов часто отправляется односторонний alert, и этого достаточно |
| `SSL_free(ssl)` | после `SSL_shutdown` | освободить SSL-объект |
| `::close(sock)` | после `SSL_free` | закрыть TCP-сокет |
| `SSL_CTX_free(ctx)` | один раз перед выходом из `main` | освободить контекст |

Порядок важен. Если, например, сделать `close(sock)` до `SSL_shutdown`, на стороне партнёра в логах появится `unexpected EOF`, и записанный TLS-record может потеряться. У нас этот порядок выдержан и в обработке ошибок, и в нормальном выходе.

---

## 9. Сборка и запуск

### 9.1 Зависимости

- `g++` со `std=c++11`;
- `libssl-dev` / `libcrypto-dev` (Ubuntu/Debian: `apt install libssl-dev`);
- `pthread` (входит в glibc);
- `openssl` CLI — для генерации сертификатов.

### 9.2 Makefile

```bash
make            # собирает server и client
make certs      # генерирует server.crt и server.key (если их ещё нет)
make clean      # удаляет бинарники
make distclean  # + удаляет server.crt/server.key/history.json/net_diag_*.json
```

Линковка явно подключает `-lssl -lcrypto -lpthread`.

### 9.3 Генерация сертификатов

Один раз перед первым запуском:

```bash
make certs
```

Эквивалентно:

```bash
openssl req -x509 -newkey rsa:2048 \
    -keyout server.key \
    -out server.crt \
    -days 365 \
    -nodes \
    -subj "/C=RU/ST=Local/L=Local/O=Lab7/OU=Dev/CN=localhost"
```

В директории появятся `server.crt` (открытый сертификат) и `server.key` (приватный ключ).

### 9.4 Запуск сервера

```bash
./server                                                 # порт по умолчанию 5555, server.crt/server.key
./server 8080 --cert=server.crt --key=server.key         # явный порт + явные пути
./server 8080 --cert=server.crt --key=server.key \       # с эмуляцией помех (всё как в ЛР6)
              --delay=100 --drop=0.2 --corrupt=0.1
```

Старт сервера логирует:

```
[Security][TLS] OpenSSL initialized
[Security][CERT] certificate loaded: server.crt
[Security][CERT] private key loaded: server.key
[Security][CERT] key/certificate match verified
[Transport] listening on port 8080
```

### 9.5 Запуск клиента

```bash
./client                       # 127.0.0.1:5555, ник спросит интерактивно
./client 127.0.0.1 8080        # IP и порт явно
./client 127.0.0.1 8080 Alice  # + ник аргументом (расширение поверх ТЗ)
```

### 9.6 Команды клиента

```
/help            — список команд
/list            — кто онлайн
/history [N]     — последние N записей истории
/w <nick> <txt>  — личное сообщение
/ping [N]        — диагностические пинги
/netdiag         — агрегированная сетевая статистика + сохранить JSON
/tlsinfo         — повторно показать TLS-версию, шифр, Subject/Issuer
/quit            — корректное отключение
<текст>          — публичное сообщение
```

`/tlsinfo` — единственная новая команда; всё остальное — наследие ЛР6.

---

## 10. Сценарии тестирования

### 10.1 «Голый» SSL/TLS

```bash
# терминал 1
./server 8080 --cert=server.crt --key=server.key

# терминал 2
./client 127.0.0.1 8080
> hello world
```

Ожидаемое: handshake-логи, `MSG_TLS_INFO` с версией и шифром, обычный обмен сообщениями. В Wireshark на TCP-порту 8080 — сплошные `Application Data` записи, прочитать содержимое нельзя.

### 10.2 Подмена сертификата

Удалить `server.crt`, перезапустить сервер — он должен упасть на загрузке с `[Security][CERT] failed to load certificate`. Это базовая проверка обработки ошибок инициализации.

Сгенерировать новый сертификат, но **не** перегенерировать ключ:

```bash
openssl req -x509 -newkey rsa:2048 -keyout new.key -out server.crt -days 365 -nodes -subj "/CN=localhost"
# server.key — старый, не подходит к новому сертификату
./server
```

`SSL_CTX_check_private_key` отловит несоответствие и вернёт `[Security][CERT] private key does not match certificate` — это второй важный sanity-check.

### 10.3 Совместная работа TLS + retry/drop

```bash
./server 8080 --cert=server.crt --key=server.key --drop=0.5
./client 127.0.0.1 8080
> test reliability
```

Должно быть видно:
- handshake прошёл нормально (TLS не страдает);
- какие-то сообщения сервер «теряет» (`[Transport][SIM] DROP`);
- через 2 с клиент шлёт `[Transport][RETRY] resend` и логирует `[Security][ENC] SSL_write` для повтора;
- в итоге приходит `MSG_ACK`, retry-цикл завершён.

### 10.4 Замер RTT через TLS

```bash
> /ping 20
PING 1 -> RTT=0.6ms
PING 2 -> RTT=0.4ms | Jitter=0.2ms
...
> /netdiag
RTT avg : 0.5 ms
Jitter  : 0.1 ms
Loss    : 0.0 %
```

Сравни с аналогичными цифрами из ЛР6 — обычно RTT через TLS на ~0.1–0.3 мс выше, что и ожидаемо: каждый пакет проходит через AEAD-шифрование.

---

## 11. Ключевые вопросы и ответы

**Q: Почему мы не передаём `int sock` в `send_message_ex` параллельно с `SSL*`?**
A: Потому что после `SSL_set_fd` весь ввод/вывод обязан идти через `SSL_*`. Любой `send/recv` напрямую обойдёт шифрование и нарушит TLS-state-machine. Запрет на `int sock` — это compile-time гарантия того, что приложение не «утечёт» в открытый канал.

**Q: Что произойдёт, если потерять `server.key`?**
A: Сервер не запустится (`SSL_CTX_use_PrivateKey_file` вернёт ошибку). Восстановить ключ из `.crt` нельзя — это асимметричная криптография. Нужно перегенерировать пару `make distclean && make certs`. Все клиенты увидят новый сертификат с другим публичным ключом — для самоподписанного стенда это нормально, для CA-подписанного потребовало бы новой подписи.

**Q: Почему мы оставили `int sock` в структуре `Client` рядом с `SSL*`, если общение идёт только через SSL?**
A: Чисто для логов уровня `[Internet]`. `getsockname/getpeername` работает с файловым дескриптором, а не со SSL-объектом. SSL внутри уже знает свой fd (через `SSL_get_fd`), но писать `SSL_get_fd(c.ssl)` каждый раз ради `sockaddr_in` — лишние строки. Поэтому держим явное поле.

**Q: Почему min-version фиксирован TLS 1.2?**
A: TLS 1.0 и 1.1 формально устарели (RFC 8996), и OpenSSL 3.x их по умолчанию не поддерживает или принижает. Закрепить `TLS1_2_VERSION` явно — защита на случай пересборки на старой системе, где низкие версии могли бы случайно включиться. На практике в нашей лабе клиент и сервер договорятся о TLS 1.3 (это видно в логе `[Security][TLS] cipher=...`).

**Q: Зачем `MSG_TLS_INFO`, если шифр уже видно локально через `SSL_get_cipher`?**
A: `SSL_get_cipher` показывает результат **одной стороны** — клиент видит, как он считает; сервер — как он считает. После TLS-handshake они должны совпасть, но логировать совпадение явно (сервер шлёт клиенту, клиент сравнивает) — это и диагностика, и наглядная демонстрация для лаба-репорта. Плюс это пример того, как мы используем новый тип сообщения по делу, а не для галочки.

**Q: Можно ли в этой архитектуре сделать клиентский сертификат (mTLS)?**
A: Да, и это минимальное изменение: на сервере `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr)` плюс `SSL_CTX_load_verify_locations(ctx, "client_ca.crt", nullptr)`. На клиенте — `SSL_CTX_use_certificate_file` и `SSL_CTX_use_PrivateKey_file` для **клиентского** ключа. Внутри лабы это считается выходом за рамки ТЗ, но архитектура к этому готова.

---

## 12. Что есть в проекте

```
7lab/
├── Makefile          — сборка + цель certs для генерации сертификатов
├── README.md         — этот файл
├── client.cpp        — клиент с TLS-рукопожатием и проверкой сертификата
├── protocol.hpp      — типы сообщений, MessageEx, ssl_send_all/ssl_recv_all
└── server.cpp        — многопоточный сервер с SSL_CTX, SSL_accept, broadcast
```

Артефакты времени выполнения (создаются автоматически):

```
server.crt, server.key   — сертификат и приватный ключ (после make certs)
history.json             — история сообщений сервера
net_diag_<nick>.json     — отчёт /netdiag со стороны клиента
```
