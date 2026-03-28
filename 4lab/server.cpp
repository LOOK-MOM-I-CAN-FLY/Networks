#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "protocol.hpp"

constexpr int THREAD_POOL_SIZE = 10;
constexpr std::size_t NICKNAME_MAX_LEN = 31;

std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

struct Client {
    int sock;
    char nickname[32];
    int authenticated;
};

std::vector<Client> active_clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

std::string client_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

const char* message_type_name(uint8_t type) {
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
        default: return "MSG_UNKNOWN";
    }
}

const char* layer_name(int layer) {
    switch (layer) {
        case 4: return "Transport";
        case 5: return "Session";
        case 6: return "Presentation";
        case 7: return "Application";
        default: return "";
    }
}

void log_layer(int layer, const std::string& text) {
    std::cout << "[Layer " << layer << " - " << layer_name(layer) << "] " << text << "\n";
}

bool recv_message_osi(int sock, Message& msg, bool authenticated) {
    log_layer(4, "recv()");
    if (!recv_message(sock, msg)) {
        return false;
    }
    log_layer(6, std::string("deserialize Message (") + message_type_name(msg.type) + ")");
    log_layer(5, authenticated ? "client authenticated" : "client not authenticated");
    return true;
}

bool send_message_osi(int sock, uint8_t type, const std::string& payload, const std::string& action) {
    log_layer(7, action + " (" + message_type_name(type) + ")");
    log_layer(6, std::string("serialize Message (") + message_type_name(type) + ")");
    log_layer(4, "send()");
    return send_message(sock, type, payload);
}

std::vector<int> snapshot_client_sockets() {
    std::vector<int> sockets;
    pthread_mutex_lock(&clients_mutex);
    sockets.reserve(active_clients.size());
    for (const auto& client : active_clients) {
        sockets.push_back(client.sock);
    }
    pthread_mutex_unlock(&clients_mutex);
    return sockets;
}

bool nickname_exists(const std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : active_clients) {
        if (nickname == client.nickname) {
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

void add_client(int sock, const std::string& nickname) {
    Client client{};
    client.sock = sock;
    std::memset(client.nickname, 0, sizeof(client.nickname));
    std::strncpy(client.nickname, nickname.c_str(), sizeof(client.nickname) - 1);
    client.authenticated = 1;

    pthread_mutex_lock(&clients_mutex);
    active_clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
}

bool get_nickname_by_sock(int sock, std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : active_clients) {
        if (client.sock == sock) {
            nickname = client.nickname;
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

bool get_sock_by_nickname(const std::string& nickname, int& sock_out) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : active_clients) {
        if (nickname == client.nickname) {
            sock_out = client.sock;
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

void remove_client(int sock) {
    std::string nickname;
    bool had_nickname = false;

    pthread_mutex_lock(&clients_mutex);
    for (auto it = active_clients.begin(); it != active_clients.end(); ++it) {
        if (it->sock == sock) {
            nickname = it->nickname;
            had_nickname = true;
            active_clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (had_nickname) {
        std::string info = "User [" + nickname + "] disconnected";
        for (int client_sock : snapshot_client_sockets()) {
            send_message_osi(client_sock, MSG_SERVER_INFO, info, "broadcast server info");
        }
    }

    ::shutdown(sock, SHUT_RDWR);
    ::close(sock);
}

void broadcast_payload(uint8_t type, const std::string& payload) {
    for (int client_sock : snapshot_client_sockets()) {
        send_message_osi(client_sock, type, payload, "broadcast message");
    }
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

        Message msg;
        if (!recv_message_osi(client_sock, msg, false) || msg.type != MSG_HELLO) {
            remove_client(client_sock);
            continue;
        }

        log_layer(7, std::string("handle ") + message_type_name(msg.type));
        std::cout << "New client said HELLO: " << msg.payload << "\n";

        send_message_osi(client_sock, MSG_WELCOME, "Welcome to the server!", "send welcome");

        bool authenticated = false;
        std::string nickname;
        while (!authenticated) {
            if (!recv_message_osi(client_sock, msg, false)) {
                remove_client(client_sock);
                authenticated = false;
                break;
            }

            log_layer(7, std::string("handle ") + message_type_name(msg.type));

            if (msg.type != MSG_AUTH) {
                log_layer(7, "ignore message until authentication");
                continue;
            }

            nickname = msg.payload;
            if (nickname.empty() || nickname.size() > NICKNAME_MAX_LEN) {
                send_message_osi(client_sock, MSG_ERROR, "Invalid nickname", "send auth error");
                remove_client(client_sock);
                authenticated = false;
                break;
            }

            if (nickname_exists(nickname)) {
                send_message_osi(client_sock, MSG_ERROR, "Nickname already in use", "send auth error");
                remove_client(client_sock);
                authenticated = false;
                break;
            }

            add_client(client_sock, nickname);
            log_layer(5, "authentication success");

            std::string info = "User [" + nickname + "] connected";
            broadcast_payload(MSG_SERVER_INFO, info);
            authenticated = true;
        }

        if (!authenticated) {
            continue;
        }

        while (true) {
            if (!recv_message_osi(client_sock, msg, true)) {
                break;
            }

            log_layer(7, std::string("handle ") + message_type_name(msg.type));

            if (msg.type == MSG_TEXT) {
                std::string text = "[" + nickname + "]: " + std::string(msg.payload);
                broadcast_payload(MSG_TEXT, text);
            } else if (msg.type == MSG_PRIVATE) {
                std::string payload = msg.payload;
                std::size_t sep = payload.find(':');
                if (sep == std::string::npos || sep == 0 || sep == payload.size() - 1) {
                    send_message_osi(client_sock, MSG_ERROR, "Invalid private message format", "send private error");
                    continue;
                }

                std::string target = payload.substr(0, sep);
                std::string message = payload.substr(sep + 1);
                if (!message.empty() && message[0] == ' ') {
                    message.erase(0, 1);
                }

                int target_sock = -1;
                if (!get_sock_by_nickname(target, target_sock)) {
                    send_message_osi(client_sock, MSG_ERROR, "User [" + target + "] not found", "send private error");
                    continue;
                }

                std::string private_payload = "[PRIVATE][" + nickname + "]: " + message;
                send_message_osi(target_sock, MSG_PRIVATE, private_payload, "send private message");
            } else if (msg.type == MSG_PING) {
                send_message_osi(client_sock, MSG_PONG, "", "send pong");
            } else if (msg.type == MSG_BYE) {
                std::cout << "Client disconnected by request\n";
                break;
            }
        }

        remove_client(client_sock);
    }

    return nullptr;
}

int main() {
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

        std::cout << "Connection accepted from " << client_to_string(client_addr) << "\n";
        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    ::close(server_sock);
    return 0;
}
