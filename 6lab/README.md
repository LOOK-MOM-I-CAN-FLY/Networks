# Теория к лабораторной работе №6
## Надёжность передачи данных: измерение качества сети, эмуляция помех, ACK и повторная отправка

---

## 1. Проблема надёжности поверх TCP

TCP гарантирует доставку **байтов** в рамках одного соединения. Но прикладной уровень работает не с байтами, а с **логическими сообщениями**, и сталкивается с более сложными проблемами:

| Проблема | Почему возникает, несмотря на TCP |
|---|---|
| Потеря сообщения | Разрыв соединения в момент передачи — часть уже отосланных байт не дошла |
| Дублирование | При повторной отправке после предполагаемой потери исходное могло всё-таки дойти |
| Повреждение payload | TCP-контрольная сумма 16-битная и вероятностная; посредник (прокси, NAT) может исказить данные |
| Задержки / нестабильность | Конгестия, очереди в коммутаторах, переключение Wi-Fi, усыпление CPU |

Механизмы прикладного уровня, решающие эти проблемы:
- **ACK** (подтверждение доставки) — явный ответ получателя на каждое «важное» сообщение;
- **Retransmission** — повторная отправка при отсутствии ACK в течение таймаута;
- **Дедупликация** по `msg_id` — защита от повторной обработки одного и того же сообщения;
- **Метрики качества** — RTT, jitter, loss — численная оценка состояния канала.

> **Важно:** это всё реализуется **поверх** TCP, а не вместо него. Мы не заменяем TCP-стек — мы добавляем ещё один уровень надёжности для логических сообщений.

---

## 2. Метрики качества сети

### 2.1 RTT — Round Trip Time

```
RTT = t_receive − t_send
```

Время от отправки запроса до получения ответа. Измеряет **полный путь туда-обратно**: клиент → сеть → сервер → обработка → сеть → клиент.

В нашем коде используется `std::chrono::steady_clock` — монотонные часы, не подверженные скачкам системного времени (`std::chrono::system_clock` ломается при NTP-синхронизации и недопустим для измерения интервалов).

```cpp
// client.cpp: run_ping()
PingEntry e;
e.send_time = SteadyClock::now();
// ... отправляем MSG_PING ...

// client.cpp: handle_pong()
auto now = SteadyClock::now();
auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - e.send_time).count();
e.rtt_ms = static_cast<double>(us) / 1000.0;
```

Перевод в миллисекунды делаем через микросекунды, чтобы не терять точность при localhost-передаче (RTT ~0.1–1 мс).

### 2.2 Jitter — вариация задержки

```
Jitter_n = |RTT_n − RTT_{n−1}|
```

Показывает, насколько «дрожит» задержка. Низкий jitter при высоком RTT — стабильно медленно. Высокий jitter — непредсказуемо, критично для голоса/видео.

В коде клиента:
```cpp
// client.cpp: run_ping()
if (prev_rtt >= 0.0) {
    double jitter = std::fabs(rtt - prev_rtt);
    line << " | Jitter=" << jitter << "ms";
}
prev_rtt = rtt;
```

Первый PING не имеет jitter (нет предыдущего значения). Итоговый jitter — среднее арифметическое модулей разностей между соседними успешными ответами.

### 2.3 Loss — процент потерь

```
Loss = (sent − received) / sent * 100%
```

В нашем случае «потеря» = timeout в 2 секунды без получения `MSG_PONG`.

---

## 3. Команды `/ping` и `/netdiag`

### 3.1 Команда `/ping [N]`

```
/ping        — 10 запросов (по умолчанию)
/ping N      — N запросов
```

Каждый запрос получает уникальный `msg_id`, генерируемый атомарным счётчиком клиента:

```cpp
// client.cpp
std::atomic<std::uint32_t> next_client_msg_id(1);

std::uint32_t next_msg_id() {
    return next_client_msg_id.fetch_add(1);
}
```

`std::atomic::fetch_add` гарантирует, что даже если несколько потоков одновременно запросят ID, все получат разные значения без мьютекса.

### 3.2 Алгоритм `/ping N` — последовательная отправка с таймаутом

Пинги отправляются **последовательно** с персональным таймаутом 2 с на каждый. Это даёт чистый per-packet вывод `PING n → RTT=X`:

```cpp
// client.cpp: run_ping()
for (int i = 0; i < count; ++i) {
    std::uint32_t id = next_msg_id();
    PingEntry e{id, SteadyClock::now(), false, 0.0};
    ping_entries.push_back(e);

    send_message_ex(sock, MSG_PING, id, nickname, "", time(nullptr), "");
    bool received = wait_pong(idx, PING_TIMEOUT_MS);
    // ...
}
```

Функция `wait_pong` использует `pthread_cond_timedwait` — блокируется на условной переменной до получения ответа или до истечения дедлайна. Переменная пробуждается из `receive_thread` при получении `MSG_PONG` с совпадающим `msg_id`.

**Почему не `recv()` в самом `run_ping`?** Потому что `receive_thread` уже читает сокет. Два параллельных `recv` на одном сокете гарантированно разорвут поток. Поэтому `run_ping` общается с `receive_thread` через shared state + condvar.

### 3.3 Команда `/netdiag`

Агрегирует результаты последнего `/ping`-сеанса, выводит на экран и сохраняет в JSON:

```
RTT avg : 7.1 ms
Jitter  : 2.8 ms
Loss    : 20.0 %
Saved to net_diag_Alice.json
```

Формат файла:
```json
{
  "nickname": "Alice",
  "sent": 5,
  "received": 4,
  "rtt_avg_ms": 7.100,
  "jitter_avg_ms": 2.800,
  "loss_pct": 20.000
}
```

Формат намеренно простой — ровно те поля, которых требует ТЗ. Парсинг сторонними утилитами тривиален.

---

## 4. Эмуляция сетевых помех на сервере

### 4.1 Параметры запуска

```bash
./server --delay=100 --drop=0.2 --corrupt=0.1
```

- `--delay=N` — задержка обработки N мс (имитация медленной сети);
- `--drop=P` — вероятность P∈[0,1] полной потери сообщения;
- `--corrupt=P` — вероятность порчи одного случайного байта payload.

Парсинг аргументов:
```cpp
// server.cpp: parse_sim_args()
for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--delay=", 0) == 0) {
        sim.delay_ms = std::atoi(a.c_str() + 8);
    }
    // ...
}
```

### 4.2 Логика эмуляции

Функция `apply_sim()` вызывается **до** прикладной обработки сообщения, в соответствии со спецификацией:

```cpp
// server.cpp
bool apply_sim(MessageEx& msg) {
    if (sim.delay_ms > 0) {
        std::cout << "[Transport][SIM] DELAY applied: " << sim.delay_ms << " ms\n";
        usleep(sim.delay_ms * 1000);
    }
    if (sim.drop_rate > 0.0 && rand_unit() < sim.drop_rate) {
        std::cout << "[Transport][SIM] DROP (id=" << msg.msg_id << ", rate=" << sim.drop_rate << ")\n";
        return false;  // сообщение отбрасывается
    }
    if (sim.corrupt_rate > 0.0 && msg.length > 0 && rand_unit() < sim.corrupt_rate) {
        int idx = rand_index(msg.length);
        msg.payload[idx] ^= 0xFF;
        std::cout << "[Transport][SIM] CORRUPT payload (id=" << msg.msg_id << ")\n";
    }
    return true;
}
```

Порядок применения помех (как указано в ТЗ):
1. **Delay** — всегда применяется первой;
2. **Drop** — отбрасывание (если сработало, дальше ничего не делаем);
3. **Corrupt** — порча байта (не влияет на доставку, но сообщение пойдёт испорченным в историю/рассылку).

### 4.3 Потокобезопасный rand

`std::rand()` не гарантированно потокобезопасен, поэтому под него есть отдельный мьютекс:

```cpp
pthread_mutex_t rand_mutex = PTHREAD_MUTEX_INITIALIZER;

double rand_unit() {
    pthread_mutex_lock(&rand_mutex);
    double v = static_cast<double>(std::rand()) / RAND_MAX;
    pthread_mutex_unlock(&rand_mutex);
    return v;
}
```

Для лабораторной достаточно; в продакшене лучше `std::mt19937` с `thread_local`.

### 4.4 Где именно встроена эмуляция

В `worker_thread`, сразу после `recv_message_tcpip`, ДО любой ветки обработки типов:

```cpp
while (true) {
    if (!recv_message_tcpip(client_sock, msg)) break;

    if (!apply_sim(msg)) {       // <-- здесь
        continue;                // сообщение «потеряно», прикладной уровень его не увидит
    }

    if (msg.type == MSG_TEXT) { /* обработка */ }
    // ...
}
```

Это соответствует принципу из ТЗ: прикладной уровень не знает, почему сообщение не пришло.

---

## 5. MSG_ACK — подтверждение доставки

### 5.1 Новый тип сообщения

```cpp
// protocol.hpp
enum MessageType : std::uint8_t {
    // ... предыдущие типы ...
    MSG_ACK = 15
};
```

`MSG_ACK` несёт в себе:
- `msg_id` — идентификатор подтверждаемого сообщения (не свой собственный!);
- пустой `payload`;
- `sender = "SERVER"`.

### 5.2 Когда сервер отсылает ACK

Сервер шлёт `MSG_ACK` **после успешной обработки** `MSG_TEXT` или `MSG_PRIVATE`:

```cpp
// server.cpp, фрагмент worker_thread
if (msg.type == MSG_TEXT) {
    // dedup-проверка, обработка, broadcast ...
    send_ack(client_sock, msg.msg_id);
    continue;
}
```

Также ACK шлётся при обнаружении **дубликата** — иначе клиент продолжит retry:

```cpp
if (is_duplicate(last_ids, DEDUP_WINDOW, msg.msg_id)) {
    std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")\n";
    send_ack(client_sock, msg.msg_id);  // всё равно ACK!
    continue;
}
```

### 5.3 Почему `MSG_PING` не получает ACK

По пункту 5.3 ТЗ: «Сервер при получении `MSG_PING` должен: не выполнять дополнительной логики, немедленно отправить `MSG_PONG`». `MSG_PONG` с таким же `msg_id` уже выполняет роль подтверждения — отдельный ACK был бы избыточен.

### 5.4 Почему `MSG_ACK` не зависит от сервер-стороннего `msg_id`

Сервер ведёт свой собственный счётчик `next_msg_id` для истории/рассылки — чтобы глобальные ID в `history.json` не конфликтовали между разными клиентами (у Alice и Bob оба могут начать с id=1). Но ACK использует **клиентский** `msg_id`, чтобы клиент мог сопоставить подтверждение со своим pending-сообщением.

То есть существует **два параллельных ID-пространства**:
- клиентский (для ACK-корреляции);
- серверный (для истории и broadcast).

---

## 6. Повторная отправка на клиенте

### 6.1 Очередь `pending_queue`

```cpp
// client.cpp
struct PendingMsg {
    std::uint32_t msg_id;
    std::uint8_t type;
    std::string sender;
    std::string receiver;
    std::int64_t timestamp;
    std::string payload;
    SteadyTime send_time;
    int retries;
};

std::vector<PendingMsg> pending_queue;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
```

Каждое `MSG_TEXT` и `MSG_PRIVATE`, отправляемое клиентом, параллельно с отправкой кладётся в очередь:

```cpp
bool send_reliable(int sock, std::uint8_t type, std::uint32_t msg_id, ...) {
    PendingMsg pm{ msg_id, type, sender, receiver, timestamp, payload,
                   SteadyClock::now(), 0 };
    pthread_mutex_lock(&pending_mutex);
    pending_queue.push_back(pm);
    pthread_mutex_unlock(&pending_mutex);

    std::cout << "[Transport][RETRY] send " << message_type_name(type)
              << " (id=" << msg_id << ")\n";
    return send_message_ex(sock, type, msg_id, sender, receiver, timestamp, payload);
}
```

### 6.2 Фоновый поток повторной отправки

Отдельный `retry_thread` раз в 300 мс сканирует очередь и **для каждого pending-сообщения, пролежавшего > 2 секунд без ACK**:
- если `retries < 3` → увеличивает счётчик, обновляет `send_time`, отправляет повторно;
- если `retries >= 3` → помечает сообщение как недоставленное и удаляет из очереди.

```cpp
// client.cpp: retry_thread()
while (is_connected.load()) {
    usleep(RETRY_SCAN_MS * 1000);
    auto now = SteadyClock::now();

    pthread_mutex_lock(&pending_mutex);
    for (auto it = pending_queue.begin(); it != pending_queue.end(); ) {
        auto ms = duration_cast<milliseconds>(now - it->send_time).count();
        if (ms >= RETRY_TIMEOUT_MS) {
            if (it->retries >= RETRY_MAX) {
                // give up
                items.push_back({*it, true});
                it = pending_queue.erase(it);
                continue;
            }
            it->retries++;
            it->send_time = now;
            items.push_back({*it, false});
        }
        ++it;
    }
    pthread_mutex_unlock(&pending_mutex);

    for (const auto& r : items) {
        if (r.give_up) {
            std::cout << "[Transport][RETRY] give up, message undelivered (id="
                      << r.pm.msg_id << ")\n";
        } else {
            std::cout << "[Transport][RETRY] wait ACK timeout\n";
            std::cout << "[Transport][RETRY] resend " << r.pm.retries << "/" << RETRY_MAX
                      << " (id=" << r.pm.msg_id << ")\n";
            send_message_ex(sock, r.pm.type, r.pm.msg_id, r.pm.sender, r.pm.receiver,
                            r.pm.timestamp, r.pm.payload);
        }
    }
}
```

**Важная деталь:** сам `send_message_ex` вызывается **после** освобождения `pending_mutex`. Иначе блокировка на медленной отправке в сокет задержала бы любого, кто добавляет/удаляет pending.

### 6.3 Обработка входящего `MSG_ACK`

```cpp
// client.cpp: handle_ack()
void handle_ack(std::uint32_t msg_id) {
    pthread_mutex_lock(&pending_mutex);
    auto it = std::find_if(pending_queue.begin(), pending_queue.end(),
                           [&](const PendingMsg& pm) { return pm.msg_id == msg_id; });
    if (it != pending_queue.end()) {
        pending_queue.erase(it);
    }
    pthread_mutex_unlock(&pending_mutex);
    std::cout << "[Transport][RETRY] ACK received (id=" << msg_id << ")\n";
}
```

Вызывается из `receive_thread` при получении `MSG_ACK`.

### 6.4 Почему MSG_PING не в очереди retry

ТЗ формально включает `MSG_PING` в список retry-сообщений, но это **противоречит диагностической природе** `/ping N`: если клиент автоматически будет переотправлять потерянные пинги, метрика `Loss` всегда будет 0%. Компромисс: диагностические пинги (единственный источник `MSG_PING` в клиенте) идут **в обход** `pending_queue` через обычный `send_message_ex`, а их «потеря» = таймаут = честный индикатор качества канала.

---

## 7. Дедупликация на сервере

### 7.1 Окно `last_ids[32]`

ТЗ требует `uint32_t last_ids[32]` на каждого клиента. Поскольку эта структура нужна только **одному** рабочему потоку, обрабатывающему соединение, она живёт **на стеке воркера** — никакого мьютекса не нужно:

```cpp
// server.cpp: worker_thread()
std::uint32_t last_ids[DEDUP_WINDOW];
std::memset(last_ids, 0, sizeof(last_ids));
std::size_t last_ids_pos = 0;
```

Это пример **отказа от мьютекса через выбор области жизни данных** — чем уже scope, тем меньше блокировок.

### 7.2 Кольцевой буфер

```cpp
bool is_duplicate(const std::uint32_t* buf, std::size_t n, std::uint32_t id) {
    if (id == 0) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (buf[i] == id) return true;
    }
    return false;
}

void remember_id(std::uint32_t* buf, std::size_t n, std::size_t& pos, std::uint32_t id) {
    if (id == 0) return;
    buf[pos] = id;
    pos = (pos + 1) % n;
}
```

Структура — кольцевой буфер на 32 элемента. Новые ID затирают самые старые. Поиск дубликата — линейный по 32 элементам (≈ 50 нс). Для окна в 32 это эффективнее хэш-таблицы из-за cache locality.

### 7.3 Сценарий «потеря + retry»

```
1. Alice → MSG_TEXT(id=41, "hello")
2. [SIM] DROP (id=41)                 — сервер отбросил, ACK не послал
3. ... проходит 2 секунды ...
4. Alice: [RETRY] resend 1/3 (id=41)  — retry_thread отправил заново
5. сервер получает MSG_TEXT(id=41), is_duplicate → false (первый раз не дошёл)
6. обрабатывает, кладёт 41 в last_ids, шлёт MSG_ACK(41)
7. Alice: [RETRY] ACK received (id=41) — убрала из pending_queue
```

### 7.4 Сценарий «потерянный ACK»

```
1. Alice → MSG_TEXT(id=42)
2. сервер обрабатывает, кладёт 42 в last_ids, шлёт MSG_ACK(42)
3. ... (в реальной сети ACK мог бы потеряться — в нашем SIM он всегда доходит,
        но представим) ...
4. Alice через 2с: [RETRY] resend 1/3 (id=42)
5. сервер получает MSG_TEXT(id=42), is_duplicate → true
6. [Application][DEDUP] duplicate ignored (id=42)
7. сервер шлёт MSG_ACK(42) повторно
8. Alice: [RETRY] ACK received (id=42)
```

Вариант 2 возможен **только** если бы в будущем расширили эмуляцию на исходящие сообщения сервера. В текущей реализации `--drop` затрагивает только входящие в сервер.

---

## 8. Потокобезопасность: резюме по мьютексам

### Клиент

| Мьютекс | Защищает | Участники |
|---|---|---|
| `pending_mutex` | `pending_queue` | main (enqueue при отправке), receive_thread (dequeue на ACK), retry_thread (сканирование) |
| `ping_mutex` + `ping_cond` | `ping_entries` | main (в `run_ping`), receive_thread (на `MSG_PONG`) |
| `stats_mutex` | `last_stats`, `have_stats` | main (запись в `run_ping`, чтение в `run_netdiag`) |

`next_client_msg_id` — `std::atomic`, мьютекс не нужен.

`is_connected` — `std::atomic<bool>`, читается из всех трёх потоков.

### Сервер

| Мьютекс | Защищает | Примечание |
|---|---|---|
| `clients_mutex` | `active_clients` | |
| `queue_mutex` + `queue_cond` | `client_queue` | передача сокетов из `main` в пул |
| `offline_mutex` | `offline_queue` | |
| `history_mutex` | `history_records`, запись в `history.json` | |
| `id_mutex` | `next_msg_id` (серверный) | |
| `rand_mutex` | `std::rand()` | новый, для `apply_sim` |

`last_ids[32]` — на стеке воркер-потока, без мьютекса.

---

## 9. Логирование по модели TCP/IP + новые префиксы

Основная идея та же, что в лабе №5: уровни Application → Transport → Internet → Network Access (при отправке — сверху вниз, при приёме — снизу вверх).

Новые префиксы для этой лабы:

| Префикс | Когда появляется |
|---|---|
| `[Transport][PING]` | обработка `MSG_PING` / `MSG_PONG` |
| `[Transport][SIM]` | эмуляция помех на сервере (DELAY/DROP/CORRUPT) |
| `[Transport][RETRY]` | клиентская очередь: отправка, wait timeout, resend, ACK |
| `[Transport][ACK]` | отправка `MSG_ACK` сервером |
| `[Application][ACK]` | обработка сообщения, требующего ACK |
| `[Application][DEDUP]` | игнорирование дубликата на сервере |

Пример полного цикла с помехой и retry:

```
# Клиент отправляет:
[Transport][RETRY] send MSG_TEXT (id=41)

# Сервер получает:
[Transport] recv() 81 bytes via TCP
[Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
[Network Access] frame received via network interface
[Application] deserialize MessageEx -> MSG_TEXT
[Transport][SIM] DELAY applied: 100 ms
[Transport][SIM] DROP (id=41, rate=0.2)

# Клиент через 2 секунды не получил ACK:
[Transport][RETRY] wait ACK timeout
[Transport][RETRY] resend 1/3 (id=41)

# Сервер получает повторно:
[Transport] recv() 81 bytes via TCP
...
[Transport][SIM] DELAY applied: 100 ms
[Application][ACK] process MSG_TEXT (id=41)
[Application] broadcast text -> MSG_TEXT
...
[Application] send ack -> MSG_ACK
[Transport][ACK] send MSG_ACK (id=41)
...

# Клиент получает ACK:
[Transport][RETRY] ACK received (id=41)
```

---

## 10. Сборка и запуск

### Сборка
```bash
make
```

Зависимости: g++ с `-std=c++11`, `pthread`. Стандартная Ubuntu-коробка.

### Запуск сервера

Без помех:
```bash
./server
```

С эмуляцией:
```bash
./server --delay=100 --drop=0.2 --corrupt=0.1
```

### Запуск клиента
```bash
./client Alice       # ник в аргументе
./client             # спросит ник в интерактиве
```

### Команды клиента
```
/help                — список команд
/list                — кто онлайн
/history [N]         — последние N записей истории
/w <nick> <text>     — личное сообщение
/ping [N]            — N (или 10) диагностических пингов
/netdiag             — агрегированная статистика + сохранить в JSON
/quit                — корректное отключение
<текст>              — публичное сообщение
```

---

## 11. Сценарии тестирования

### 11.1 Базовая проверка надёжности

Терминал 1:
```bash
./server --drop=0.5
```

Терминал 2:
```bash
./client Alice
> hello
```

Ожидается: при потере с вероятностью 50% часть сообщений сервер отбрасывает, но клиент через 2 с их переотправляет. В логах видны `[SIM] DROP` и `[RETRY] resend`. В итоге сообщение доходит и клиент получает `[RETRY] ACK received`.

### 11.2 Диагностика качества канала

```
> /ping 10
PING 1 -> RTT=0.2ms
PING 2 -> RTT=0.3ms | Jitter=0.1ms
PING 3 -> timeout
...

> /netdiag
RTT avg : 0.3 ms
Jitter  : 0.1 ms
Loss    : 10.0 %
Saved to net_diag_Alice.json
```

### 11.3 Превышение лимита retries

Запустить сервер с `--drop=1.0` (всегда дропать):
```bash
./server --drop=1.0
```

Отправить текстовое сообщение — после 3 retry клиент должен логировать:
```
[Transport][RETRY] give up, message undelivered (id=...)
```

### 11.4 Дедупликация

Искусственно воспроизводится при потере ACK от сервера (в текущей реализации — только за счёт спорадических сетевых условий, так как `--drop` в нашем SIM затрагивает только входящий трафик сервера). При реальной потере — в логе сервера должно появиться:
```
[Application][DEDUP] duplicate ignored (id=...)
[Transport][ACK] send MSG_ACK (id=...)
```

---

## 12. Ключевые вопросы и ответы

**Q: Зачем нужен `MSG_ACK`, если TCP сам гарантирует доставку?**
A: TCP гарантирует доставку **байтов** в рамках одного соединения. Но если соединение разрывается, часть логических сообщений теряется. ACK на уровне приложения позволяет зафиксировать доставку **логического сообщения** и организовать retry через переподключение — не только в рамках одной TCP-сессии.

**Q: Почему RTT измеряется через `steady_clock`, а не `system_clock`?**
A: `system_clock` может скакать назад (NTP-синхронизация, ручной перевод часов). Измерение интервала `system_clock::now() - t0` может дать отрицательные значения. `steady_clock` монотонный по контракту.

**Q: Что будет, если клиент отправил сообщение и получил ACK, но соединение разорвалось до того, как сервер успел сбродкастить?**
A: Сообщение останется в `history.json` со стороны сервера как успешно обработанное. Другие клиенты получат его (если не были онлайн — через офлайн-очередь, или позже через `/history`). Отправитель считает задачу выполненной.

**Q: Почему `last_ids[32]` — именно 32? И что случится, если пришлёт 33-е уникальное сообщение, а потом retry 1-го?**
A: 32 — это окно из ТЗ. Если клиент успел отправить 33 разных сообщения быстрее, чем пришёл retry первого, то первое из `last_ids` будет затёрто, и retry пройдёт как «новое». Это baseline-дедупликация; для чата с низкой частотой сообщений 32 заведомо достаточно.

**Q: Почему `apply_sim` применяется ко всем сообщениям, включая `MSG_BYE` и `MSG_LIST`?**
A: По ТЗ эмуляция работает на уровне транспорта — она не знает, какое сообщение прикладной уровень хотел бы «защитить». Если `MSG_LIST` отбросят, клиент просто не увидит ответ. Это часть эксперимента: показать, как ненадёжная сеть ломает разные сценарии.

**Q: Зачем атомарный `std::atomic<uint32_t> next_client_msg_id`, если можно было взять мьютекс?**
A: Атомарный `fetch_add` — одна CPU-инструкция (`LOCK XADD` на x86), без системного вызова. Мьютекс — потенциально десятки тысяч тактов при contention. Для счётчика, дергаемого из всех потоков, это разумная оптимизация «из коробки».

**Q: Почему retry-поток просыпается раз в 300 мс, а не постоянно крутится?**
A: Точность retry-таймаута — +/- 300 мс на фоне 2000 мс — приемлема. Активное ожидание жгло бы CPU впустую. Альтернатива — condvar с таймаутами по ближайшему дедлайну — существенно усложнила бы код ради мелкой экономии.

**Q: Что будет, если клиент пошлёт `MSG_TEXT`, очередь заполнится, а сервер будет недоступен?**
A: Каждое сообщение вызовет `[RETRY] resend 1/3`, `2/3`, `3/3` → `give up`. После трёх неудачных попыток сообщение удалится из очереди, клиент напечатает `message undelivered`. Соединение к этому времени скорее всего уже закрыто через `recv` → `0`, будет запущена logика переподключения.

**Q: Порядок применения помех — delay, drop, corrupt — почему именно такой?**
A: Delay симулирует реальную сетевую задержку — она есть у каждого пакета, даже у тех, что в итоге дойдут. Drop — самое сильное событие; если пакет потерян, нет смысла его портить. Corrupt — последний, потому что он изменяет payload «перед приложением», но сообщение при этом всё ещё доставлено. Такой порядок соответствует физической реальности: сначала среда добавляет задержку, потом сеть может потерять пакет, потом может исказить данные — и только дошедшие и неискажённые пакеты попадают в приложение.
