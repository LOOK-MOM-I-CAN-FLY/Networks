#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.hpp"

namespace {

constexpr int THREAD_POOL_SIZE = 10;
constexpr std::size_t NICKNAME_MAX_LEN = MAX_NAME - 1;
constexpr int HISTORY_DEFAULT_N = 10;
constexpr const char* HISTORY_FILE = "history.json";
constexpr std::size_t DEDUP_WINDOW = 32;

struct Client {
    int sock;
    char nickname[MAX_NAME];
    bool authenticated;
};

struct HistoryRecord {
    std::uint32_t msg_id = 0;
    std::int64_t timestamp = 0;
    std::string sender;
    std::string receiver;
    std::uint8_t type = 0;
    std::string text;
    bool delivered = true;
    bool is_offline = false;
};

struct OfflineMsg {
    std::uint32_t msg_id = 0;
    std::int64_t timestamp = 0;
    std::string sender;
    std::string receiver;
    std::string text;
};

struct SimConfig {
    int delay_ms = 0;
    double drop_rate = 0.0;
    double corrupt_rate = 0.0;
};

std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::vector<Client> active_clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

std::unordered_map<std::string, std::vector<OfflineMsg>> offline_queue;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<HistoryRecord> history_records;
pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;
std::uint32_t next_msg_id = 1;

pthread_mutex_t rand_mutex = PTHREAD_MUTEX_INITIALIZER;

SimConfig sim;

double rand_unit() {
    pthread_mutex_lock(&rand_mutex);
    double v = static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX);
    pthread_mutex_unlock(&rand_mutex);
    return v;
}

int rand_index(int upper) {
    pthread_mutex_lock(&rand_mutex);
    int v = (upper > 0) ? std::rand() % upper : 0;
    pthread_mutex_unlock(&rand_mutex);
    return v;
}

bool apply_sim(MessageEx& msg) {
    if (sim.delay_ms > 0) {
        std::cout << "[Transport][SIM] DELAY applied: " << sim.delay_ms << " ms\n";
        ::usleep(static_cast<useconds_t>(sim.delay_ms) * 1000);
    }
    if (sim.drop_rate > 0.0 && rand_unit() < sim.drop_rate) {
        std::cout << "[Transport][SIM] DROP (id=" << msg.msg_id
                  << ", rate=" << sim.drop_rate << ")\n";
        return false;
    }
    if (sim.corrupt_rate > 0.0 && msg.length > 0 && rand_unit() < sim.corrupt_rate) {
        int idx = rand_index(static_cast<int>(msg.length));
        msg.payload[idx] = static_cast<char>(static_cast<unsigned char>(msg.payload[idx]) ^ 0xFF);
        std::cout << "[Transport][SIM] CORRUPT payload (id=" << msg.msg_id << ")\n";
    }
    return true;
}

bool is_duplicate(const std::uint32_t* buf, std::size_t n, std::uint32_t id) {
    if (id == 0) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (buf[i] == id) {
            return true;
        }
    }
    return false;
}

void remember_id(std::uint32_t* buf, std::size_t n, std::size_t& pos, std::uint32_t id) {
    if (id == 0) {
        return;
    }
    buf[pos] = id;
    pos = (pos + 1) % n;
}

std::string ip_of(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

std::string ipport_of(const sockaddr_in& addr) {
    return ip_of(addr) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool get_endpoints(int sock, sockaddr_in& local, sockaddr_in& peer) {
    socklen_t local_len = sizeof(local);
    socklen_t peer_len = sizeof(peer);
    std::memset(&local, 0, sizeof(local));
    std::memset(&peer, 0, sizeof(peer));
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &local_len) < 0) {
        return false;
    }
    if (::getpeername(sock, reinterpret_cast<sockaddr*>(&peer), &peer_len) < 0) {
        return false;
    }
    return true;
}

void log_incoming(int sock, const MessageEx& msg) {
    sockaddr_in local{};
    sockaddr_in peer{};
    (void)get_endpoints(sock, local, peer);

    std::size_t bytes = sizeof(MessageExWireHeader) + msg.length;
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP\n";
    std::cout << "[Internet] src=" << ip_of(peer) << " dst=" << ip_of(local) << " proto=TCP\n";
    std::cout << "[Network Access] frame received via network interface\n";
    std::cout << "[Application] deserialize MessageEx -> " << message_type_name(msg.type) << "\n";

    if (msg.type == MSG_PING) {
        std::cout << "[Transport][PING] recv MSG_PING (id=" << msg.msg_id << ")\n";
    }
}

void log_outgoing(int sock, std::uint8_t type, std::uint32_t msg_id,
                  std::size_t payload_len, const std::string& action) {
    sockaddr_in local{};
    sockaddr_in peer{};
    (void)get_endpoints(sock, local, peer);

    std::size_t bytes = sizeof(MessageExWireHeader) + payload_len;
    std::cout << "[Application] " << action << " -> " << message_type_name(type) << "\n";

    if (type == MSG_PONG) {
        std::cout << "[Transport][PING] send MSG_PONG (id=" << msg_id << ")\n";
    } else if (type == MSG_ACK) {
        std::cout << "[Transport][ACK] send MSG_ACK (id=" << msg_id << ")\n";
    }

    std::cout << "[Transport] send() " << bytes << " bytes via TCP\n";
    std::cout << "[Internet] src=" << ip_of(local) << " dst=" << ip_of(peer) << " proto=TCP\n";
    std::cout << "[Network Access] frame sent to network interface\n";
}

bool recv_message_tcpip(int sock, MessageEx& msg) {
    if (!recv_message_ex(sock, msg)) {
        return false;
    }
    log_incoming(sock, msg);
    return true;
}

bool send_message_tcpip(
    int sock,
    std::uint8_t type,
    std::uint32_t msg_id,
    const std::string& sender,
    const std::string& receiver,
    std::int64_t timestamp,
    const std::string& payload,
    const std::string& action) {

    std::size_t payload_len = std::min<std::size_t>(payload.size(), MAX_PAYLOAD);
    log_outgoing(sock, type, msg_id, payload_len, action);
    return send_message_ex(sock, type, msg_id, sender, receiver, timestamp, payload);
}

std::vector<int> snapshot_client_sockets() {
    std::vector<int> sockets;
    pthread_mutex_lock(&clients_mutex);
    sockets.reserve(active_clients.size());
    for (const auto& c : active_clients) {
        sockets.push_back(c.sock);
    }
    pthread_mutex_unlock(&clients_mutex);
    return sockets;
}

bool nickname_exists(const std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : active_clients) {
        if (nickname == c.nickname) {
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

bool get_sock_by_nickname(const std::string& nickname, int& sock_out) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : active_clients) {
        if (nickname == c.nickname) {
            sock_out = c.sock;
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

bool get_nickname_by_sock(int sock, std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : active_clients) {
        if (c.sock == sock) {
            nickname = c.nickname;
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

void add_client(int sock, const std::string& nickname) {
    Client c{};
    c.sock = sock;
    std::memset(c.nickname, 0, sizeof(c.nickname));
    std::strncpy(c.nickname, nickname.c_str(), sizeof(c.nickname) - 1);
    c.authenticated = true;

    pthread_mutex_lock(&clients_mutex);
    active_clients.push_back(c);
    pthread_mutex_unlock(&clients_mutex);
}

std::string escape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += ' ';
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

bool write_history_file_locked(const std::string& path) {
    std::ostringstream oss;
    oss << "[\n";
    for (std::size_t i = 0; i < history_records.size(); ++i) {
        const auto& r = history_records[i];
        oss << "  {\n";
        oss << "    \"msg_id\": " << r.msg_id << ",\n";
        oss << "    \"timestamp\": " << r.timestamp << ",\n";
        oss << "    \"sender\": \"" << escape_json_string(r.sender) << "\",\n";
        oss << "    \"receiver\": \"" << escape_json_string(r.receiver) << "\",\n";
        oss << "    \"type\": \"" << message_type_name(r.type) << "\",\n";
        oss << "    \"text\": \"" << escape_json_string(r.text) << "\",\n";
        oss << "    \"delivered\": " << (r.delivered ? "true" : "false") << ",\n";
        oss << "    \"is_offline\": " << (r.is_offline ? "true" : "false") << "\n";
        oss << "  }" << (i + 1 < history_records.size() ? "," : "") << "\n";
    }
    oss << "]\n";

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << oss.str();
        if (!out.good()) {
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        return false;
    }
    return true;
}

bool parse_json_string(const std::string& s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char ch = s[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\') {
            if (pos >= s.size()) {
                return false;
            }
            char esc = s[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(esc); break;
            }
            continue;
        }
        out.push_back(ch);
    }
    return false;
}

void skip_ws(const std::string& s, std::size_t& pos) {
    while (pos < s.size()) {
        char ch = s[pos];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            ++pos;
            continue;
        }
        break;
    }
}

bool find_key_pos(const std::string& obj, const std::string& key, std::size_t& pos) {
    const std::string needle = "\"" + key + "\"";
    std::size_t k = obj.find(needle);
    if (k == std::string::npos) {
        return false;
    }
    std::size_t colon = obj.find(':', k + needle.size());
    if (colon == std::string::npos) {
        return false;
    }
    pos = colon + 1;
    return true;
}

bool parse_uint64(const std::string& s, std::size_t& pos, std::uint64_t& out) {
    skip_ws(s, pos);
    if (pos >= s.size() || (s[pos] < '0' || s[pos] > '9')) {
        return false;
    }
    std::uint64_t v = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10 + static_cast<std::uint64_t>(s[pos] - '0');
        ++pos;
    }
    out = v;
    return true;
}

bool parse_int64(const std::string& s, std::size_t& pos, std::int64_t& out) {
    skip_ws(s, pos);
    bool neg = false;
    if (pos < s.size() && s[pos] == '-') {
        neg = true;
        ++pos;
    }
    std::uint64_t v = 0;
    if (!parse_uint64(s, pos, v)) {
        return false;
    }
    out = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
    return true;
}

bool parse_bool(const std::string& s, std::size_t& pos, bool& out) {
    skip_ws(s, pos);
    if (s.compare(pos, 4, "true") == 0) {
        pos += 4;
        out = true;
        return true;
    }
    if (s.compare(pos, 5, "false") == 0) {
        pos += 5;
        out = false;
        return true;
    }
    return false;
}

std::uint8_t type_from_name(const std::string& name) {
    if (name == "MSG_TEXT") return MSG_TEXT;
    if (name == "MSG_PRIVATE") return MSG_PRIVATE;
    if (name == "MSG_SERVER_INFO") return MSG_SERVER_INFO;
    if (name == "MSG_ERROR") return MSG_ERROR;
    if (name == "MSG_HISTORY_DATA") return MSG_HISTORY_DATA;
    return 0;
}

bool parse_history_object(const std::string& obj, HistoryRecord& rec) {
    std::size_t pos = 0;
    std::uint64_t u64 = 0;
    std::int64_t i64 = 0;
    std::string s;

    if (!find_key_pos(obj, "msg_id", pos) || !parse_uint64(obj, pos, u64)) {
        return false;
    }
    rec.msg_id = static_cast<std::uint32_t>(u64);

    if (!find_key_pos(obj, "timestamp", pos) || !parse_int64(obj, pos, i64)) {
        return false;
    }
    rec.timestamp = i64;

    if (!find_key_pos(obj, "sender", pos)) return false;
    skip_ws(obj, pos);
    if (!parse_json_string(obj, pos, s)) return false;
    rec.sender = s;

    if (!find_key_pos(obj, "receiver", pos)) return false;
    skip_ws(obj, pos);
    if (!parse_json_string(obj, pos, s)) return false;
    rec.receiver = s;

    if (!find_key_pos(obj, "type", pos)) return false;
    skip_ws(obj, pos);
    if (!parse_json_string(obj, pos, s)) return false;
    rec.type = type_from_name(s);

    if (!find_key_pos(obj, "text", pos)) return false;
    skip_ws(obj, pos);
    if (!parse_json_string(obj, pos, s)) return false;
    rec.text = s;

    if (!find_key_pos(obj, "delivered", pos) || !parse_bool(obj, pos, rec.delivered)) {
        return false;
    }
    if (!find_key_pos(obj, "is_offline", pos) || !parse_bool(obj, pos, rec.is_offline)) {
        return false;
    }
    if (rec.type == 0) {
        rec.type = MSG_TEXT;
    }
    return true;
}

bool load_history_file(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::in);
    if (!in) {
        return true;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    std::string data = oss.str();

    std::vector<HistoryRecord> loaded;
    std::size_t pos = 0;
    while (true) {
        std::size_t start = data.find('{', pos);
        if (start == std::string::npos) {
            break;
        }
        std::size_t end = data.find('}', start);
        if (end == std::string::npos) {
            break;
        }
        HistoryRecord rec;
        if (parse_history_object(data.substr(start, end - start + 1), rec)) {
            loaded.push_back(rec);
        }
        pos = end + 1;
    }

    pthread_mutex_lock(&history_mutex);
    history_records = std::move(loaded);
    pthread_mutex_unlock(&history_mutex);
    return true;
}

void rebuild_offline_queue_from_history() {
    pthread_mutex_lock(&offline_mutex);
    offline_queue.clear();
    pthread_mutex_unlock(&offline_mutex);

    pthread_mutex_lock(&history_mutex);
    for (const auto& r : history_records) {
        if (r.is_offline && !r.delivered && !r.receiver.empty()) {
            OfflineMsg om;
            om.msg_id = r.msg_id;
            om.timestamp = r.timestamp;
            om.sender = r.sender;
            om.receiver = r.receiver;
            om.text = r.text;

            pthread_mutex_lock(&offline_mutex);
            offline_queue[om.receiver].push_back(om);
            pthread_mutex_unlock(&offline_mutex);
        }
    }
    pthread_mutex_unlock(&history_mutex);

    pthread_mutex_lock(&offline_mutex);
    for (auto& kv : offline_queue) {
        auto& vec = kv.second;
        std::sort(vec.begin(), vec.end(), [](const OfflineMsg& a, const OfflineMsg& b) {
            return a.msg_id < b.msg_id;
        });
    }
    pthread_mutex_unlock(&offline_mutex);
}

std::uint32_t allocate_msg_id() {
    pthread_mutex_lock(&id_mutex);
    std::uint32_t id = next_msg_id++;
    pthread_mutex_unlock(&id_mutex);
    return id;
}

void init_next_msg_id_from_history() {
    std::uint32_t max_id = 0;
    pthread_mutex_lock(&history_mutex);
    for (const auto& r : history_records) {
        max_id = std::max(max_id, r.msg_id);
    }
    pthread_mutex_unlock(&history_mutex);

    pthread_mutex_lock(&id_mutex);
    next_msg_id = max_id + 1;
    if (next_msg_id == 0) {
        next_msg_id = 1;
    }
    pthread_mutex_unlock(&id_mutex);
}

void append_history_record(const HistoryRecord& rec) {
    pthread_mutex_lock(&history_mutex);
    history_records.push_back(rec);
    (void)write_history_file_locked(HISTORY_FILE);
    pthread_mutex_unlock(&history_mutex);
}

void mark_history_delivered(std::uint32_t msg_id) {
    pthread_mutex_lock(&history_mutex);
    for (auto& r : history_records) {
        if (r.msg_id == msg_id) {
            r.delivered = true;
            break;
        }
    }
    (void)write_history_file_locked(HISTORY_FILE);
    pthread_mutex_unlock(&history_mutex);
}

std::string history_line(const HistoryRecord& r) {
    std::string line = "[" + format_timestamp(r.timestamp) + "][id=" + std::to_string(r.msg_id) + "]";

    if (r.is_offline) {
        line += "[OFFLINE][" + r.sender + " -> " + r.receiver + "]: " + r.text;
        return line;
    }
    if (r.type == MSG_PRIVATE) {
        line += "[PRIVATE][" + r.sender + " -> " + r.receiver + "]: " + r.text;
        return line;
    }
    if (r.type == MSG_SERVER_INFO) {
        line += "[SERVER]: " + r.text;
        return line;
    }
    line += "[" + r.sender + "]: " + r.text;
    return line;
}

void broadcast_message(
    std::uint8_t type,
    std::uint32_t msg_id,
    const std::string& sender,
    const std::string& receiver,
    std::int64_t timestamp,
    const std::string& payload,
    const std::string& action) {

    for (int s : snapshot_client_sockets()) {
        (void)send_message_tcpip(s, type, msg_id, sender, receiver, timestamp, payload, action);
    }
}

void send_ack(int sock, std::uint32_t client_msg_id) {
    (void)send_message_tcpip(sock, MSG_ACK, client_msg_id, "SERVER", "",
                             static_cast<std::int64_t>(std::time(nullptr)),
                             "", "send ack");
}

void close_client_socket(int sock) {
    ::shutdown(sock, SHUT_RDWR);
    ::close(sock);
}

void remove_client(int sock) {
    std::string nickname;
    (void)get_nickname_by_sock(sock, nickname);

    pthread_mutex_lock(&clients_mutex);
    active_clients.erase(
        std::remove_if(active_clients.begin(), active_clients.end(),
                       [&](const Client& c) { return c.sock == sock; }),
        active_clients.end());
    pthread_mutex_unlock(&clients_mutex);

    close_client_socket(sock);

    if (!nickname.empty()) {
        const std::string info = "User [" + nickname + "] disconnected";
        broadcast_message(MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), info, "broadcast server info");

        HistoryRecord rec;
        rec.msg_id = allocate_msg_id();
        rec.timestamp = static_cast<std::int64_t>(std::time(nullptr));
        rec.sender = "SERVER";
        rec.receiver = "";
        rec.type = MSG_SERVER_INFO;
        rec.text = info;
        rec.delivered = true;
        rec.is_offline = false;
        append_history_record(rec);
    }
}

void deliver_offline_messages(int sock, const std::string& nickname) {
    std::vector<OfflineMsg> pending;
    pthread_mutex_lock(&offline_mutex);
    auto it = offline_queue.find(nickname);
    if (it != offline_queue.end()) {
        pending = std::move(it->second);
        offline_queue.erase(it);
    }
    pthread_mutex_unlock(&offline_mutex);

    if (pending.empty()) {
        (void)send_message_tcpip(sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr),
                                 "no offline messages for " + nickname, "send server info");
        return;
    }

    for (const auto& om : pending) {
        const std::string payload = std::string("OFFLINE:") + om.text;
        if (send_message_tcpip(sock, MSG_PRIVATE, om.msg_id, om.sender, om.receiver, om.timestamp, payload,
                               "deliver offline message")) {
            mark_history_delivered(om.msg_id);
        }
    }
}

bool parse_private_fallback(const std::string& payload, std::string& receiver, std::string& text) {
    std::size_t sep = payload.find(':');
    if (sep == std::string::npos || sep == 0 || sep == payload.size() - 1) {
        return false;
    }
    receiver = payload.substr(0, sep);
    text = payload.substr(sep + 1);
    if (!text.empty() && text[0] == ' ') {
        text.erase(0, 1);
    }
    return true;
}

void handle_history_request(int sock, const MessageEx& req) {
    int n = HISTORY_DEFAULT_N;
    if (req.length > 0) {
        try {
            n = std::stoi(req.payload);
        } catch (...) {
            n = -1;
        }
        if (n <= 0) {
            (void)send_message_tcpip(sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr),
                                     "Invalid /history parameter", "send error");
            return;
        }
    }

    std::vector<HistoryRecord> slice;
    pthread_mutex_lock(&history_mutex);
    int total = static_cast<int>(history_records.size());
    int start = std::max(0, total - n);
    for (int i = start; i < total; ++i) {
        slice.push_back(history_records[static_cast<std::size_t>(i)]);
    }
    pthread_mutex_unlock(&history_mutex);

    for (const auto& r : slice) {
        std::string line = history_line(r);
        if (line.size() > MAX_PAYLOAD) {
            line.resize(MAX_PAYLOAD);
        }
        (void)send_message_tcpip(sock, MSG_HISTORY_DATA, 0, "SERVER", "", std::time(nullptr), line,
                                 "send history line");
    }
}

void handle_list_request(int sock) {
    std::ostringstream oss;
    oss << "Online users\n";
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : active_clients) {
        oss << c.nickname << "\n";
    }
    pthread_mutex_unlock(&clients_mutex);

    std::string out = oss.str();
    if (out.size() > MAX_PAYLOAD) {
        out.resize(MAX_PAYLOAD);
    }
    (void)send_message_tcpip(sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), out, "send user list");
}

void* worker_thread(void*) {
    while (true) {
        int client_sock = -1;
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty()) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        std::uint32_t last_ids[DEDUP_WINDOW];
        std::memset(last_ids, 0, sizeof(last_ids));
        std::size_t last_ids_pos = 0;

        MessageEx msg{};
        if (!recv_message_tcpip(client_sock, msg) || msg.type != MSG_HELLO) {
            close_client_socket(client_sock);
            continue;
        }

        (void)send_message_tcpip(client_sock, MSG_WELCOME, 0, "SERVER", "", std::time(nullptr),
                                 "Welcome to the server!", "send welcome");

        bool authenticated = false;
        std::string nickname;
        while (!authenticated) {
            if (!recv_message_tcpip(client_sock, msg)) {
                close_client_socket(client_sock);
                authenticated = false;
                break;
            }
            if (msg.type != MSG_AUTH) {
                std::cout << "[Application] ignore message until authentication\n";
                continue;
            }

            nickname = msg.payload;
            if (nickname.empty() || nickname.size() > NICKNAME_MAX_LEN) {
                (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr),
                                         "Invalid nickname", "send auth error");
                continue;
            }
            if (nickname_exists(nickname)) {
                (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr),
                                         "Nickname already in use", "send auth error");
                continue;
            }

            add_client(client_sock, nickname);
            authenticated = true;

            (void)send_message_tcpip(client_sock, MSG_WELCOME, 0, "SERVER", "", std::time(nullptr),
                                     "Authenticated as " + nickname, "send auth ok");

            const std::string info = "User [" + nickname + "] connected";
            broadcast_message(MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr), info, "broadcast server info");

            HistoryRecord rec;
            rec.msg_id = allocate_msg_id();
            rec.timestamp = static_cast<std::int64_t>(std::time(nullptr));
            rec.sender = "SERVER";
            rec.receiver = "";
            rec.type = MSG_SERVER_INFO;
            rec.text = info;
            rec.delivered = true;
            rec.is_offline = false;
            append_history_record(rec);

            deliver_offline_messages(client_sock, nickname);
        }

        if (!authenticated) {
            continue;
        }

        while (true) {
            if (!recv_message_tcpip(client_sock, msg)) {
                break;
            }

            if (!apply_sim(msg)) {
                continue;
            }

            if (msg.type == MSG_TEXT) {
                if (is_duplicate(last_ids, DEDUP_WINDOW, msg.msg_id)) {
                    std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")\n";
                    send_ack(client_sock, msg.msg_id);
                    continue;
                }
                remember_id(last_ids, DEDUP_WINDOW, last_ids_pos, msg.msg_id);

                std::cout << "[Application][ACK] process MSG_TEXT (id=" << msg.msg_id << ")\n";

                std::uint32_t id = allocate_msg_id();
                std::int64_t ts = static_cast<std::int64_t>(std::time(nullptr));
                std::string text = msg.payload;

                HistoryRecord rec;
                rec.msg_id = id;
                rec.timestamp = ts;
                rec.sender = nickname;
                rec.receiver = "";
                rec.type = MSG_TEXT;
                rec.text = text;
                rec.delivered = true;
                rec.is_offline = false;
                append_history_record(rec);

                broadcast_message(MSG_TEXT, id, nickname, "", ts, text, "broadcast text");
                send_ack(client_sock, msg.msg_id);
                continue;
            }

            if (msg.type == MSG_PRIVATE) {
                if (is_duplicate(last_ids, DEDUP_WINDOW, msg.msg_id)) {
                    std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")\n";
                    send_ack(client_sock, msg.msg_id);
                    continue;
                }
                remember_id(last_ids, DEDUP_WINDOW, last_ids_pos, msg.msg_id);

                std::cout << "[Application][ACK] process MSG_PRIVATE (id=" << msg.msg_id << ")\n";

                std::string receiver = msg.receiver;
                std::string text = msg.payload;
                if (receiver.empty()) {
                    if (!parse_private_fallback(msg.payload, receiver, text)) {
                        (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr),
                                                 "Invalid private message format", "send private error");
                        send_ack(client_sock, msg.msg_id);
                        continue;
                    }
                }

                if (receiver.empty() || receiver.size() > NICKNAME_MAX_LEN) {
                    (void)send_message_tcpip(client_sock, MSG_ERROR, 0, "SERVER", "", std::time(nullptr),
                                             "Invalid receiver nickname", "send private error");
                    send_ack(client_sock, msg.msg_id);
                    continue;
                }

                std::uint32_t id = allocate_msg_id();
                std::int64_t ts = static_cast<std::int64_t>(std::time(nullptr));

                int target_sock = -1;
                if (get_sock_by_nickname(receiver, target_sock)) {
                    HistoryRecord rec;
                    rec.msg_id = id;
                    rec.timestamp = ts;
                    rec.sender = nickname;
                    rec.receiver = receiver;
                    rec.type = MSG_PRIVATE;
                    rec.text = text;
                    rec.delivered = true;
                    rec.is_offline = false;
                    append_history_record(rec);

                    (void)send_message_tcpip(target_sock, MSG_PRIVATE, id, nickname, receiver, ts, text,
                                             "send private message");
                    (void)send_message_tcpip(client_sock, MSG_PRIVATE, id, nickname, receiver, ts, text,
                                             "echo private message");
                    send_ack(client_sock, msg.msg_id);
                    continue;
                }

                OfflineMsg om;
                om.msg_id = id;
                om.timestamp = ts;
                om.sender = nickname;
                om.receiver = receiver;
                om.text = text;

                pthread_mutex_lock(&offline_mutex);
                offline_queue[receiver].push_back(om);
                pthread_mutex_unlock(&offline_mutex);

                HistoryRecord rec;
                rec.msg_id = id;
                rec.timestamp = ts;
                rec.sender = nickname;
                rec.receiver = receiver;
                rec.type = MSG_PRIVATE;
                rec.text = text;
                rec.delivered = false;
                rec.is_offline = true;
                append_history_record(rec);

                (void)send_message_tcpip(client_sock, MSG_SERVER_INFO, 0, "SERVER", "", std::time(nullptr),
                                         "receiver " + receiver + " is offline, message stored",
                                         "send server info");

                const std::string echo_payload = std::string("OFFLINE:") + text;
                (void)send_message_tcpip(client_sock, MSG_PRIVATE, id, nickname, receiver, ts, echo_payload,
                                         "echo offline private");
                send_ack(client_sock, msg.msg_id);
                continue;
            }

            if (msg.type == MSG_LIST) {
                handle_list_request(client_sock);
                continue;
            }

            if (msg.type == MSG_HISTORY) {
                handle_history_request(client_sock, msg);
                continue;
            }

            if (msg.type == MSG_PING) {
                (void)send_message_tcpip(client_sock, MSG_PONG, msg.msg_id, "SERVER", "",
                                         std::time(nullptr), "", "send pong");
                continue;
            }

            if (msg.type == MSG_BYE) {
                std::cout << "Client disconnected by request\n";
                break;
            }
        }

        remove_client(client_sock);
    }

    return nullptr;
}

void parse_sim_args(int argc, char* argv[]) {
    const char* kDelay = "--delay=";
    const char* kDrop = "--drop=";
    const char* kCorrupt = "--corrupt=";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(kDelay, 0) == 0) {
            sim.delay_ms = std::atoi(a.c_str() + std::strlen(kDelay));
            if (sim.delay_ms < 0) sim.delay_ms = 0;
        } else if (a.rfind(kDrop, 0) == 0) {
            sim.drop_rate = std::atof(a.c_str() + std::strlen(kDrop));
            if (sim.drop_rate < 0.0) sim.drop_rate = 0.0;
            if (sim.drop_rate > 1.0) sim.drop_rate = 1.0;
        } else if (a.rfind(kCorrupt, 0) == 0) {
            sim.corrupt_rate = std::atof(a.c_str() + std::strlen(kCorrupt));
            if (sim.corrupt_rate < 0.0) sim.corrupt_rate = 0.0;
            if (sim.corrupt_rate > 1.0) sim.corrupt_rate = 1.0;
        }
    }

    if (sim.delay_ms > 0 || sim.drop_rate > 0.0 || sim.corrupt_rate > 0.0) {
        std::cout << "[Transport][SIM] enabled: delay=" << sim.delay_ms
                  << "ms drop=" << sim.drop_rate
                  << " corrupt=" << sim.corrupt_rate << "\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    parse_sim_args(argc, argv);

    (void)load_history_file(HISTORY_FILE);
    init_next_msg_id_from_history();
    rebuild_offline_queue_from_history();

    int server_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (::bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::perror("bind");
        ::close(server_sock);
        return 1;
    }

    if (::listen(server_sock, 10) < 0) {
        std::perror("listen");
        ::close(server_sock);
        return 1;
    }

    std::cout << "Server listening on port " << SERVER_PORT << "\n";

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock < 0) {
            continue;
        }
        std::cout << "Connection accepted from " << ipport_of(client_addr) << "\n";

        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    ::close(server_sock);
    return 0;
}
