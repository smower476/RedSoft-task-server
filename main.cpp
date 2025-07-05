#include <iostream>
#include <string>
#include <map>
#include <set>
#include <deque>
#include <thread>
#include <mutex>
#include <sstream>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <errno.h>
#include <vector>
#include <atomic>

using namespace std;

static const int MAX_MESSAGE_LEN = 256;
static const int MAX_MESSAGES = 40;
static const int SEND_TIMEOUT_MS = 1000; // Таймаут для safe_send

struct Message {
    string nick;
    string text;
    Message(const string& n = "", const string& t = "") : nick(n), text(t) {}
};

struct Channel {
    deque<Message> messages;
    set<string> members;
    mutex mtx;
};

static map<string, Channel> channels;
static mutex global_mtx;
static int server_fd = -1;
static atomic<bool> running(true);

// Для отслеживания активных клиентов
static mutex clients_mtx;
static set<int> client_fds;

// Тримминг строки (обрезка пробелов)
static inline string trim(const string &s) {
    auto wsfront = find_if_not(s.begin(), s.end(), [](int c){ return isspace(c); });
    auto wsback = find_if_not(s.rbegin(), s.rend(), [](int c){ return isspace(c); }).base();
    if (wsfront >= wsback) return "";
    return string(wsfront, wsback);
}

// Безопасная отправка с таймаутом, возвращает false при ошибке
bool safe_send(int sockfd, const string& message, int timeout_ms = SEND_TIMEOUT_MS) {
    const char* data = message.c_str();
    size_t total_sent = 0;
    size_t to_send = message.size();

    while (total_sent < to_send) {
        pollfd pfd{sockfd, POLLOUT, 0};
        int res = poll(&pfd, 1, timeout_ms);
        if (res < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return false;
        }
        if (res == 0) {
            cerr << "safe_send: timeout\n";
            return false;
        }

        ssize_t sent = send(sockfd, data + total_sent, to_send - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                cerr << "safe_send: connection closed: " << strerror(errno) << "\n";
            } else {
                perror("send");
            }
            return false;
        }
        if (sent == 0) {
            cerr << "safe_send: connection closed by peer\n";
            return false;
        }
        total_sent += sent;
    }
    return true;
}

// Обработчик клиента в отдельном потоке
void handle_client(int client_fd) {
    {
        lock_guard<mutex> lock(clients_mtx);
        client_fds.insert(client_fd);
    }

    char buffer[1024];
    while (running) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) break;

        string cmd(buffer);
        cmd.erase(cmd.find_last_not_of(" \r\n") + 1);
        istringstream iss(cmd);
        string action, channel, nick;
        iss >> action >> channel >> nick;

        if (action.empty() || channel.empty() || nick.empty()) {
            if (!safe_send(client_fd, "ERROR: invalid command\n")) break;
            continue;
        }
        if (channel.length() > 24 || nick.length() > 24) {
            if (!safe_send(client_fd, "ERROR: channel or nick too long\n")) break;
            continue;
        }

        Channel *ch_ptr = nullptr;
        bool abort = false;
        {
            lock_guard<mutex> lock(global_mtx);
            auto it = channels.find(channel);
            if (it == channels.end()) {
                if (action == "send" || action == "join") {
                    auto res = channels.try_emplace(channel);
                    it = res.first;
                } else {
                    if (!safe_send(client_fd, "ERROR: not in channel\n")) abort = true;
                    if (abort) break;
                    continue;
                }
            }
            ch_ptr = &it->second;
        }

        Channel &ch = *ch_ptr;
        
        if (action == "join") {
            lock_guard<mutex> ch_lock(ch.mtx);
            if (ch.members.count(nick)) {
                if (!safe_send(client_fd, "ERROR: already in channel\n")) break;
            } else {
                ch.members.insert(nick);
                if (!safe_send(client_fd, "OK\n")) break;
            }
        }
        else if (action == "exit") {
            lock_guard<mutex> ch_lock(ch.mtx);
            if (!ch.members.count(nick)) {
                if (!safe_send(client_fd, "ERROR: not in channel\n")) break;
            } else {
                ch.members.erase(nick);
                if (!safe_send(client_fd, "OK\n")) break;
            }
        }
        else if (action == "send") {
            lock_guard<mutex> ch_lock(ch.mtx);
            string message;
            getline(iss, message);
            message = trim(message);
            if (message.empty()) {
                if (!safe_send(client_fd, "ERROR: message cannot be empty\n")) break;
                continue;
            }
            if (!ch.members.count(nick)) {
                if (!safe_send(client_fd, "ERROR: not in channel\n")) break;
            } else {
                if (message.size() > MAX_MESSAGE_LEN) message.resize(MAX_MESSAGE_LEN);
                ch.messages.emplace_back(nick, message);
                if (ch.messages.size() > MAX_MESSAGES) ch.messages.pop_front();
                if (!safe_send(client_fd, "OK\n")) break;
            }
        }
        else if (action == "read") {
            // Режим чтения: копируем буфер под мьютексом, затем отправляем
            deque<Message> copy_msgs;
            size_t count = 0;
            {
                lock_guard<mutex> ch_lock(ch.mtx);
                if (!ch.members.count(nick)) {
                    if (!safe_send(client_fd, "ERROR: not in channel\n")) break;
                    goto CONTINUE_LOOP;
                }
                copy_msgs = ch.messages;
                count = copy_msgs.size();
            }
            if (!safe_send(client_fd, string("OK ") + to_string(count) + "\n")) break;
            for (const auto& msg : copy_msgs) {
                string line = msg.nick + ": " + msg.text + "\n";
                if (!safe_send(client_fd, line)) break;
            }
        }
        else {
CONTINUE_LOOP:;
            if (action != "read") {
                if (!safe_send(client_fd, "ERROR: unknown command\n")) break;
            }
        }

    }

    close(client_fd);
    {
        lock_guard<mutex> lock(clients_mtx);
        client_fds.erase(client_fd);
    }
}

// SIGINT: устанавливаем флаг остановки и закрываем слушающий сокет
void signal_handler(int) {
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    running = false;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: ./server <port>\n";
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    int port;
    try { port = stoi(argv[1]); } catch (...) { cerr << "Invalid port\n"; return EXIT_FAILURE; }
    if (port <= 0 || port > 65535) { cerr << "Port out of range\n"; return EXIT_FAILURE; }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) { perror("bind"); close(server_fd); return EXIT_FAILURE; }
    if (listen(server_fd, 10) < 0) { perror("listen"); close(server_fd); return EXIT_FAILURE; }

    cout << "Server is listening on port " << port << "\n";

    vector<thread> threads;
    while (running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!running) break;
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        threads.emplace_back(handle_client, client_fd);
    }

    if (server_fd != -1) close(server_fd);
    {
        lock_guard<mutex> lock(clients_mtx);
        for (int fd : client_fds) shutdown(fd, SHUT_RDWR);
    }
    for (auto &t : threads) if (t.joinable()) t.join();

    cout << "Server shutdown complete.\n";
    return EXIT_SUCCESS;
}

