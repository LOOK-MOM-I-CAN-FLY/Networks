#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include "protocol.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;

constexpr int DEFAULT_PING_COUNT = 10;
constexpr int PING_TIMEOUT_MS = 2000;
constexpr int RETRY_TIMEOUT_MS = 2000;
constexpr int RETRY_MAX = 3;
constexpr int RETRY_SCAN_MS = 300;

std::atomic<bool> is_connected(false);
std::atomic<std::uint32_t> next_client_msg_id(1);

struct PendingMsg {
    std::uint32_t msg_id = 0;
    std::uint8_t type = 0;
    std::string sender;
    std::string receiver;
    std::int64_t timestamp = 0;
    std::string payload;
    SteadyTime send_time;
    int retries = 0;
};

std::vector<PendingMsg> pending_queue;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

struct PingEntry {
    std::uint32_t id = 0;
    SteadyTime send_time;
    bool received = false;
    double rtt_ms = 0.0;
};

std::vector<PingEntry> ping_entries;
pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ping_cond = PTHREAD_COND_INITIALIZER;

struct PingStats {
    int sent = 0;
    int received = 0;
    double rtt_avg_ms = 0.0;
    double jitter_avg_ms = 0.0;
    double loss_pct = 0.0;
};

PingStats last_stats;
bool have_stats = false;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

struct RecvThreadArgs {
    SSL* ssl;
};

struct RetryThreadArgs {
    SSL* ssl;
};

std::uint32_t next_msg_id() {
    return next_client_msg_id.fetch_add(1);
}

void clear_pending_queue() {
    pthread_mutex_lock(&pending_mutex);
    pending_queue.clear();
    pthread_mutex_unlock(&pending_mutex);
}

void clear_ping_entries() {
    pthread_mutex_lock(&ping_mutex);
    ping_entries.clear();
    pthread_mutex_unlock(&ping_mutex);
}

bool send_reliable(SSL* ssl, std::uint8_t type, std::uint32_t msg_id,
                   const std::string& sender, const std::string& receiver,
                   std::int64_t timestamp, const std::string& payload) {
    PendingMsg pm;
    pm.msg_id = msg_id;
    pm.type = type;
    pm.sender = sender;
    pm.receiver = receiver;
    pm.timestamp = timestamp;
    pm.payload = payload;
    pm.send_time = SteadyClock::now();
    pm.retries = 0;

    pthread_mutex_lock(&pending_mutex);
    pending_queue.push_back(pm);
    pthread_mutex_unlock(&pending_mutex);

    std::cout << "[Transport][RETRY] send " << message_type_name(type)
              << " (id=" << msg_id << ")\n";
    std::cout << "[Security][ENC] SSL_write " << message_type_name(type)
              << " (id=" << msg_id << ")\n";
    return send_message_ex(ssl, type, msg_id, sender, receiver, timestamp, payload);
}

void handle_ack(std::uint32_t msg_id) {
    bool found = false;
    pthread_mutex_lock(&pending_mutex);
    auto it = std::find_if(pending_queue.begin(), pending_queue.end(),
                           [&](const PendingMsg& pm) { return pm.msg_id == msg_id; });
    if (it != pending_queue.end()) {
        pending_queue.erase(it);
        found = true;
    }
    pthread_mutex_unlock(&pending_mutex);

    if (found) {
        std::cout << "[Transport][RETRY] ACK received (id=" << msg_id << ")\n";
    }
}

void handle_pong(std::uint32_t msg_id) {
    auto now = SteadyClock::now();
    pthread_mutex_lock(&ping_mutex);
    for (auto& e : ping_entries) {
        if (e.id == msg_id && !e.received) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - e.send_time).count();
            e.rtt_ms = static_cast<double>(us) / 1000.0;
            e.received = true;
            break;
        }
    }
    pthread_cond_broadcast(&ping_cond);
    pthread_mutex_unlock(&ping_mutex);
}

void* retry_thread(void* arg) {
    auto* args = static_cast<RetryThreadArgs*>(arg);
    SSL* ssl = args->ssl;
    delete args;

    while (is_connected.load()) {
        ::usleep(RETRY_SCAN_MS * 1000);
        if (!is_connected.load()) {
            break;
        }

        auto now = SteadyClock::now();

        struct RetryItem {
            PendingMsg pm;
            bool give_up;
        };
        std::vector<RetryItem> items;

        pthread_mutex_lock(&pending_mutex);
        for (auto it = pending_queue.begin(); it != pending_queue.end(); ) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->send_time).count();
            if (ms >= RETRY_TIMEOUT_MS) {
                if (it->retries >= RETRY_MAX) {
                    RetryItem ri;
                    ri.pm = *it;
                    ri.give_up = true;
                    items.push_back(ri);
                    it = pending_queue.erase(it);
                    continue;
                }
                it->retries++;
                it->send_time = now;
                RetryItem ri;
                ri.pm = *it;
                ri.give_up = false;
                items.push_back(ri);
            }
            ++it;
        }
        pthread_mutex_unlock(&pending_mutex);

        for (const auto& it : items) {
            if (it.give_up) {
                std::cout << "[Transport][RETRY] give up, message undelivered (id="
                          << it.pm.msg_id << ")\n";
                continue;
            }
            std::cout << "[Transport][RETRY] wait ACK timeout\n";
            std::cout << "[Transport][RETRY] resend " << it.pm.retries << "/" << RETRY_MAX
                      << " (id=" << it.pm.msg_id << ")\n";
            std::cout << "[Security][ENC] SSL_write " << message_type_name(it.pm.type)
                      << " (id=" << it.pm.msg_id << ")\n";
            (void)send_message_ex(ssl, it.pm.type, it.pm.msg_id, it.pm.sender,
                                  it.pm.receiver, it.pm.timestamp, it.pm.payload);
        }
    }
    return nullptr;
}

bool has_prefix(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string strip_offline_prefix(const std::string& s) {
    if (!has_prefix(s, "OFFLINE:")) {
        return s;
    }
    std::string rest = s.substr(std::strlen("OFFLINE:"));
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
        rest.erase(0, 1);
    }
    return rest;
}

std::string format_text_line(const MessageEx& msg) {
    return "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "][" +
           std::string(msg.sender) + "]: " + std::string(msg.payload);
}

std::string format_private_line(const MessageEx& msg) {
    const std::string payload = msg.payload;
    const bool offline = has_prefix(payload, "OFFLINE:");
    const std::string text = strip_offline_prefix(payload);

    std::string line = "[" + format_timestamp(msg.timestamp) + "][id=" + std::to_string(msg.msg_id) + "]";
    if (offline) {
        line += "[OFFLINE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text;
        return line;
    }
    line += "[PRIVATE][" + std::string(msg.sender) + " -> " + std::string(msg.receiver) + "]: " + text;
    return line;
}

void* receive_thread(void* arg) {
    auto* args = static_cast<RecvThreadArgs*>(arg);
    SSL* ssl = args->ssl;
    delete args;

    MessageEx msg{};
    while (is_connected.load()) {
        if (!recv_message_ex(ssl, msg)) {
            std::cout << "\n[Server disconnected]\n";
            is_connected.store(false);
            pthread_mutex_lock(&ping_mutex);
            pthread_cond_broadcast(&ping_cond);
            pthread_mutex_unlock(&ping_mutex);
            break;
        }

        if (msg.type == MSG_ACK) {
            std::cout << "[Security][ENC] SSL_read MSG_ACK (id=" << msg.msg_id << ")\n";
            handle_ack(msg.msg_id);
            continue;
        }
        if (msg.type == MSG_PONG) {
            handle_pong(msg.msg_id);
            continue;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << format_text_line(msg) << "\n";
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << format_private_line(msg) << "\n";
        } else if (msg.type == MSG_HISTORY_DATA) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_WELCOME) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_TLS_INFO) {
            std::cout << "[Security][TLS] server info: " << msg.payload << "\n";
        } else if (msg.type == MSG_SECURE_ERROR) {
            std::cout << "[Security][TLS] error: " << msg.payload << "\n";
        }
    }

    return nullptr;
}

bool wait_pong(std::size_t idx, int timeout_ms) {
    auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);

    pthread_mutex_lock(&ping_mutex);
    while (!ping_entries[idx].received && is_connected.load()) {
        auto now = SteadyClock::now();
        if (now >= deadline) {
            break;
        }
        auto rem_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long total_ns = static_cast<long long>(ts.tv_nsec) + rem_ns;
        ts.tv_sec += total_ns / 1000000000LL;
        ts.tv_nsec = total_ns % 1000000000LL;

        int ret = pthread_cond_timedwait(&ping_cond, &ping_mutex, &ts);
        if (ret == ETIMEDOUT) {
            break;
        }
    }
    bool received = ping_entries[idx].received;
    pthread_mutex_unlock(&ping_mutex);
    return received;
}

void run_ping(SSL* ssl, const std::string& nickname, int count) {
    if (count <= 0) count = DEFAULT_PING_COUNT;

    clear_ping_entries();

    double prev_rtt = -1.0;

    for (int i = 0; i < count; ++i) {
        if (!is_connected.load()) {
            std::cout << "PING aborted: disconnected\n";
            break;
        }

        std::uint32_t id = next_msg_id();

        PingEntry e;
        e.id = id;
        e.send_time = SteadyClock::now();
        e.received = false;
        e.rtt_ms = 0.0;

        pthread_mutex_lock(&ping_mutex);
        ping_entries.push_back(e);
        std::size_t idx = ping_entries.size() - 1;
        pthread_mutex_unlock(&ping_mutex);

        std::cout << "[Transport][PING] send MSG_PING (id=" << id << ")\n";
        if (!send_message_ex(ssl, MSG_PING, id, nickname, "",
                             static_cast<std::int64_t>(std::time(nullptr)), "")) {
            std::cout << "PING " << (i + 1) << " -> send failed\n";
            continue;
        }

        bool received = wait_pong(idx, PING_TIMEOUT_MS);

        pthread_mutex_lock(&ping_mutex);
        double rtt = ping_entries[idx].rtt_ms;
        pthread_mutex_unlock(&ping_mutex);

        std::ostringstream line;
        line << std::fixed << std::setprecision(1);
        if (received) {
            line << "PING " << (i + 1) << " -> RTT=" << rtt << "ms";
            if (prev_rtt >= 0.0) {
                double jitter = std::fabs(rtt - prev_rtt);
                line << " | Jitter=" << jitter << "ms";
            }
            prev_rtt = rtt;
        } else {
            line << "PING " << (i + 1) << " -> timeout";
        }
        std::cout << line.str() << "\n";
    }

    PingStats s;
    double sum_rtt = 0.0;
    double sum_jitter = 0.0;
    int jitter_count = 0;
    double last_rtt = -1.0;

    pthread_mutex_lock(&ping_mutex);
    s.sent = static_cast<int>(ping_entries.size());
    for (const auto& e : ping_entries) {
        if (e.received) {
            s.received++;
            sum_rtt += e.rtt_ms;
            if (last_rtt >= 0.0) {
                sum_jitter += std::fabs(e.rtt_ms - last_rtt);
                jitter_count++;
            }
            last_rtt = e.rtt_ms;
        }
    }
    pthread_mutex_unlock(&ping_mutex);

    s.rtt_avg_ms = s.received > 0 ? (sum_rtt / s.received) : 0.0;
    s.jitter_avg_ms = jitter_count > 0 ? (sum_jitter / jitter_count) : 0.0;
    s.loss_pct = s.sent > 0 ? ((s.sent - s.received) * 100.0 / s.sent) : 0.0;

    pthread_mutex_lock(&stats_mutex);
    last_stats = s;
    have_stats = true;
    pthread_mutex_unlock(&stats_mutex);
}

void run_netdiag(const std::string& nickname) {
    PingStats s;
    bool ok;
    pthread_mutex_lock(&stats_mutex);
    ok = have_stats;
    s = last_stats;
    pthread_mutex_unlock(&stats_mutex);

    if (!ok) {
        std::cout << "No ping data yet. Run /ping first.\n";
        return;
    }

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "RTT avg : " << s.rtt_avg_ms << " ms\n";
    std::cout << "Jitter  : " << s.jitter_avg_ms << " ms\n";
    std::cout << "Loss    : " << s.loss_pct << " %\n";

    const std::string path = "net_diag_" + nickname + ".json";
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) {
        std::cout << "Failed to write " << path << "\n";
        return;
    }
    out << std::fixed << std::setprecision(3);
    out << "{\n";
    out << "  \"nickname\": \"" << nickname << "\",\n";
    out << "  \"sent\": " << s.sent << ",\n";
    out << "  \"received\": " << s.received << ",\n";
    out << "  \"rtt_avg_ms\": " << s.rtt_avg_ms << ",\n";
    out << "  \"jitter_avg_ms\": " << s.jitter_avg_ms << ",\n";
    out << "  \"loss_pct\": " << s.loss_pct << "\n";
    out << "}\n";
    std::cout << "Saved to " << path << "\n";
}

std::string read_nickname(const std::string& initial) {
    std::string nickname = initial;
    while (nickname.empty()) {
        std::cout << "Enter nickname: ";
        if (!std::getline(std::cin, nickname)) {
            return "";
        }
        if (nickname.empty()) {
            std::cout << "Nickname cannot be empty.\n";
        }
    }
    if (nickname.size() >= MAX_NAME) {
        nickname.resize(MAX_NAME - 1);
    }
    return nickname;
}

void print_help() {
    std::cout << "Available commands:\n";
    std::cout << "/help\n";
    std::cout << "/list\n";
    std::cout << "/history\n";
    std::cout << "/history N\n";
    std::cout << "/quit\n";
    std::cout << "/w <nick> <message>\n";
    std::cout << "/ping [N]\n";
    std::cout << "/netdiag\n";
    std::cout << "/tlsinfo\n";
    std::cout << "Tip: encrypted packets never sleep\n";
}

bool parse_history_cmd(const std::string& input, std::string& payload) {
    payload.clear();
    if (input == "/history") {
        return true;
    }
    if (input.rfind("/history ", 0) != 0) {
        return false;
    }
    std::string rest = input.substr(std::strlen("/history "));
    while (!rest.empty() && rest[0] == ' ') {
        rest.erase(0, 1);
    }
    if (rest.empty()) {
        return true;
    }
    for (char ch : rest) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    if (rest == "0") {
        return false;
    }
    payload = rest;
    return true;
}

bool parse_ping_cmd(const std::string& input, int& count) {
    count = DEFAULT_PING_COUNT;
    if (input == "/ping") {
        return true;
    }
    if (input.rfind("/ping ", 0) != 0) {
        return false;
    }
    std::string rest = input.substr(std::strlen("/ping "));
    while (!rest.empty() && rest[0] == ' ') {
        rest.erase(0, 1);
    }
    if (rest.empty()) {
        return true;
    }
    for (char ch : rest) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    int v = std::atoi(rest.c_str());
    if (v <= 0 || v > 1000) {
        return false;
    }
    count = v;
    return true;
}

bool parse_whisper_cmd(const std::string& input, std::string& nick, std::string& message) {
    if (input.rfind("/w ", 0) != 0) {
        return false;
    }
    std::string rest = input.substr(3);
    while (!rest.empty() && rest[0] == ' ') {
        rest.erase(0, 1);
    }
    std::size_t space = rest.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    nick = rest.substr(0, space);
    message = rest.substr(space + 1);
    return !(nick.empty() || message.empty());
}

void log_peer_certificate(SSL* ssl) {
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        std::cout << "[Security][CERT] no certificate\n";
        return;
    }
    std::cout << "[Security][CERT] server certificate received\n";

    char* subject = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    char* issuer  = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
    if (subject) {
        std::cout << "[Security][CERT] subject: " << subject << "\n";
        OPENSSL_free(subject);
    }
    if (issuer) {
        std::cout << "[Security][CERT] issuer : " << issuer << "\n";
        OPENSSL_free(issuer);
    }
    X509_free(cert);
}

void log_cipher_info(SSL* ssl) {
    const char* cipher = SSL_get_cipher(ssl);
    const char* version = SSL_get_version(ssl);
    std::cout << "[Security][TLS] cipher=" << (cipher ? cipher : "?")
              << " protocol=" << (version ? version : "?") << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string server_ip = "127.0.0.1";
    int server_port = SERVER_PORT_DEFAULT;
    std::string nickname;

    if (argc > 1) {
        server_ip = argv[1];
    }
    if (argc > 2) {
        int p = std::atoi(argv[2]);
        if (p > 0 && p <= 65535) {
            server_port = p;
        }
    }
    if (argc > 3) {
        nickname = argv[3];
    }

    nickname = read_nickname(nickname);
    if (nickname.empty()) {
        return 0;
    }

    openssl_global_init();
    std::cout << "[Security][TLS] OpenSSL initialized (client)\n";

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        std::cerr << "[Security][TLS] SSL_CTX_new failed\n";
        ERR_print_errors_fp(stderr);
        return 1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    std::cout << "[Security][TLS] SSL context created\n";

    while (true) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::perror("socket");
            SSL_CTX_free(ctx);
            return 1;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<std::uint16_t>(server_port));
        if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
            std::cout << "Invalid server IP: " << server_ip << "\n";
            ::close(sock);
            SSL_CTX_free(ctx);
            return 1;
        }

        std::cout << "[Transport] connecting to " << server_ip << ":" << server_port << "...\n";
        if (::connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            std::cout << "[Transport] connection failed. Retrying in 2 seconds...\n";
            ::close(sock);
            sleep(2);
            continue;
        }
        std::cout << "[Transport] connected to " << server_ip << ":" << server_port << "\n";

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            std::cerr << "[Security][TLS] SSL_new failed\n";
            ::close(sock);
            sleep(2);
            continue;
        }
        SSL_set_fd(ssl, sock);

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
        std::cout << "[Security][ENC] encrypted channel established\n";

        is_connected.store(true);

        clear_pending_queue();
        clear_ping_entries();

        (void)send_message_ex(ssl, MSG_HELLO, 0, nickname, "", std::time(nullptr),
                              "My name is " + nickname);

        MessageEx welcome{};
        if (!recv_message_ex(ssl, welcome) || welcome.type != MSG_WELCOME) {
            std::cout << "Handshake (application) failed.\n";
            is_connected.store(false);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(sock);
            sleep(2);
            continue;
        }
        std::cout << "[SERVER]: " << welcome.payload << "\n";

        (void)send_message_ex(ssl, MSG_AUTH, 0, nickname, "", std::time(nullptr), nickname);

        bool auth_ok = false;
        while (true) {
            MessageEx auth_resp{};
            if (!recv_message_ex(ssl, auth_resp)) {
                break;
            }
            if (auth_resp.type == MSG_TLS_INFO) {
                std::cout << "[Security][TLS] server info: " << auth_resp.payload << "\n";
                continue;
            }
            if (auth_resp.type == MSG_WELCOME) {
                std::cout << "[SERVER]: " << auth_resp.payload << "\n";
                auth_ok = true;
                break;
            }
            if (auth_resp.type == MSG_ERROR) {
                std::cout << "[SERVER]: " << auth_resp.payload << "\n";
                std::string new_nick;
                while (new_nick.empty()) {
                    std::cout << "Enter new nickname: ";
                    if (!std::getline(std::cin, new_nick)) {
                        is_connected.store(false);
                        SSL_shutdown(ssl);
                        SSL_free(ssl);
                        ::close(sock);
                        SSL_CTX_free(ctx);
                        return 0;
                    }
                    if (new_nick.empty()) {
                        std::cout << "Nickname cannot be empty.\n";
                    }
                }
                if (new_nick.size() >= MAX_NAME) {
                    new_nick.resize(MAX_NAME - 1);
                }
                nickname = new_nick;
                (void)send_message_ex(ssl, MSG_AUTH, 0, nickname, "", std::time(nullptr), nickname);
            }
        }
        if (!auth_ok) {
            is_connected.store(false);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(sock);
            sleep(2);
            continue;
        }

        pthread_t recv_tid{};
        auto* r_args = new RecvThreadArgs{ssl};
        pthread_create(&recv_tid, nullptr, receive_thread, r_args);

        pthread_t retry_tid{};
        auto* re_args = new RetryThreadArgs{ssl};
        pthread_create(&retry_tid, nullptr, retry_thread, re_args);

        std::string input;
        bool quit_requested = false;
        while (is_connected.load()) {
            if (!std::getline(std::cin, input)) {
                quit_requested = true;
                is_connected.store(false);
                break;
            }
            if (!is_connected.load()) {
                break;
            }
            if (input.empty()) {
                continue;
            }

            if (input == "/help") {
                print_help();
                continue;
            }

            if (input == "/quit") {
                (void)send_message_ex(ssl, MSG_BYE, 0, nickname, "", std::time(nullptr), "");
                quit_requested = true;
                is_connected.store(false);
                break;
            }

            if (input == "/list") {
                (void)send_message_ex(ssl, MSG_LIST, 0, nickname, "", std::time(nullptr), "");
                continue;
            }

            if (input == "/netdiag") {
                run_netdiag(nickname);
                continue;
            }

            if (input == "/tlsinfo") {
                log_cipher_info(ssl);
                log_peer_certificate(ssl);
                continue;
            }

            int ping_count = 0;
            if (input == "/ping" || input.rfind("/ping ", 0) == 0) {
                if (!parse_ping_cmd(input, ping_count)) {
                    std::cout << "Usage: /ping or /ping N (N > 0)\n";
                    continue;
                }
                run_ping(ssl, nickname, ping_count);
                continue;
            }

            std::string hist_payload;
            if (input == "/history" || input.rfind("/history ", 0) == 0) {
                if (!parse_history_cmd(input, hist_payload)) {
                    std::cout << "Usage: /history or /history N\n";
                    continue;
                }
                (void)send_message_ex(ssl, MSG_HISTORY, 0, nickname, "", std::time(nullptr), hist_payload);
                continue;
            }

            std::string target;
            std::string msg_text;
            if (parse_whisper_cmd(input, target, msg_text)) {
                if (target.size() >= MAX_NAME) {
                    target.resize(MAX_NAME - 1);
                }
                std::uint32_t id = next_msg_id();
                (void)send_reliable(ssl, MSG_PRIVATE, id, nickname, target,
                                    std::time(nullptr), msg_text);
                continue;
            }

            std::uint32_t id = next_msg_id();
            (void)send_reliable(ssl, MSG_TEXT, id, nickname, "",
                                std::time(nullptr), input);
        }

        is_connected.store(false);
        pthread_mutex_lock(&ping_mutex);
        pthread_cond_broadcast(&ping_cond);
        pthread_mutex_unlock(&ping_mutex);

        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::shutdown(sock, SHUT_RDWR);
        ::close(sock);
        pthread_join(recv_tid, nullptr);
        pthread_join(retry_tid, nullptr);

        clear_pending_queue();

        if (quit_requested) {
            std::cout << "Exiting program.\n";
            break;
        }

        std::cout << "Reconnecting in 2 seconds...\n";
        sleep(2);
    }

    SSL_CTX_free(ctx);
    return 0;
}
