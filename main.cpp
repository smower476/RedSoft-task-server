#include <atomic>
#include <csignal>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <algorithm>

using namespace std;

#define MAX_COMMAND_LEN 1024

struct Message {
    string nick;
    string text;
    Message(const string& n, const string& t) : nick(n), text(t) {}
};

struct Channel {
    deque<Message> messages;
    set<string> members;
    mutex mtx;
};

static int server_fd = -1;
static atomic<bool> stopFlag{false};

map<string, shared_ptr<Channel>> channels;
mutex channels_mutex;

vector<thread> threads;
mutex threads_mutex;

set<int> client_sockets;
mutex client_sockets_mutex;

bool safe_send(int sockfd, const string& message, int timeout_ms = 30000) {
    const char* data = message.c_str();
    size_t total_sent = 0;
    size_t to_send = message.size();

    while (total_sent < to_send) {
        pollfd pfd{sockfd, POLLOUT, 0};
        int res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) {
            if (res == 0) cerr << "safe_send: timeout" << endl;
            else perror("poll");
            return false;
        }

        ssize_t sent = send(sockfd, data + total_sent, to_send - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;

            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN ||
                errno == ETIMEDOUT || errno == EHOSTUNREACH) {
                cerr << "safe_send: соединение разорвано: " << strerror(errno) << endl;
            } else {
                perror("send");
            }

            return false;
        }

        if (sent == 0) {
            cerr << "safe_send: соединение закрыто" << endl;
            return false;
        }

        total_sent += sent;
    }

    return true;
}

bool recvLine(int sock, std::string &out, int timeout_ms = 30000) {
    out.clear();
    char c;

    while (out.size() < MAX_COMMAND_LEN) {
        pollfd pfd{sock, POLLIN, 0};
        int res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) {
            if (res == 0) std::cerr << "recvLine: timeout\n";
            else perror("poll");
            return false;
        }

        ssize_t r = recv(sock, &c, 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return false;
        }
        if (r == 0) {
            std::cerr << "recvLine: соединение закрыто\n";
            return false;
        }

        if (c == '\n') break;
        if (c != '\r') out.push_back(c);
    }

    if (out.size() >= MAX_COMMAND_LEN) {
        std::cerr << "recvLine: превышена максимальная длина строки\n";
        return false;
    }

    return true;
}

static inline string trim(const string &s) {
    auto wsfront = find_if_not(s.begin(), s.end(), [](int c){ return isspace(c); });
    auto wsback  = find_if_not(s.rbegin(), s.rend(), [](int c){ return isspace(c); }).base();
    return (wsback <= wsfront ? string() : string(wsfront, wsback));
}

void handle_client(int client_fd) {
    string line;
    while (!stopFlag && recvLine(client_fd, line)) {
        string cmd = trim(line);
        if (cmd.empty()) continue;

        istringstream iss(cmd);
        string action, channel_name, nick;
        iss >> action >> channel_name >> nick;
        if (action.empty() || channel_name.empty() || nick.empty()) {
            safe_send(client_fd, "ERROR: invalid command\n");
            continue;
        }
        if (channel_name.size() > 24 || nick.size() > 24) {
            safe_send(client_fd, "ERROR: channel or nick too long\n");
            continue;
        }

        shared_ptr<Channel> ch_ptr;
        {
            lock_guard<mutex> lock(channels_mutex);
            auto it = channels.find(channel_name);
            if (it == channels.end()) {
                if (action == "send" || action == "join") {
                    ch_ptr = make_shared<Channel>();
                    channels[channel_name] = ch_ptr;
                } else {
                    safe_send(client_fd, "ERROR: no such channel\n");
                    continue;
                }
            } else {
                ch_ptr = it->second;
            }
        }
        Channel &ch = *ch_ptr;

        if (action == "join") {
            lock_guard<mutex> lk(ch.mtx);
            if (!ch.members.insert(nick).second) {
                safe_send(client_fd, "ERROR: user already in channel\n");
            } else {
                safe_send(client_fd, "OK\n");
            }
        }
        else if (action == "exit") {
            lock_guard<mutex> lk(ch.mtx);
            if (!ch.members.erase(nick)) {
                safe_send(client_fd, "ERROR: not in channel\n");
            } else {
                safe_send(client_fd, "OK\n");
            }
        }
        else if (action == "send") {
            string message;
            getline(iss, message);
            message = trim(message);
            if (message.empty()) {
                safe_send(client_fd, "ERROR: message cannot be empty\n");
                continue;
            }
            if (message.size() > 256) {
                message.resize(256);
            }
            lock_guard<mutex> lk(ch.mtx);
            if (!ch.members.count(nick)) {
                safe_send(client_fd, "ERROR: not in channel\n");
            } else {
                ch.messages.emplace_back(nick, message);
                if (ch.messages.size() > 40) {
                    ch.messages.pop_front();
                }
                safe_send(client_fd, "OK\n");
            }
        }
        else if (action == "read") {
            vector<string> outbuf;
            {
                lock_guard<mutex> lk(ch.mtx);
                if (!ch.members.count(nick)) {
                    safe_send(client_fd, "ERROR: not in channel\n");
                    continue;
                }
                outbuf.reserve(ch.messages.size() + 1);
                outbuf.push_back("OK " + to_string(ch.messages.size()) + "\n");
                for (auto &msg : ch.messages) {
                    outbuf.push_back(msg.nick + ": " + msg.text + "\n");
                }
            }
            for (auto &i : outbuf) {
                if (!safe_send(client_fd, i)) break;
            }
        }
        else {
            safe_send(client_fd, "ERROR: unknown command\n");
        }
    }
    close(client_fd);
}

void shutdown_server() {
    stopFlag = true;

    {
        lock_guard<mutex> lock(client_sockets_mutex);
        for (int fd : client_sockets) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }
}

void signal_handler(int signum) {
    if (signum == SIGINT) {
        shutdown_server();
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }

    int port;
    try {
        port = stoi(argv[1]);
    } catch (exception &) {
        cerr << "ERROR: invalid port\n";
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        perror("sigaction");
        return 1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }
    cout << "Server listening on port " << port << endl;

    while (!stopFlag) {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            if (stopFlag || errno == EINTR) break;
            perror("accept");
            continue;
        }

        {
            lock_guard<mutex> lock(client_sockets_mutex);
            client_sockets.insert(client_fd);
        }

        {
            lock_guard<mutex> lock(threads_mutex);
            threads.erase(
                remove_if(threads.begin(), threads.end(), 
                    [](thread& t) { return !t.joinable(); }),
                threads.end()
            );

            threads.emplace_back([client_fd]() {
                handle_client(client_fd);
                {
                    lock_guard<mutex> lock(client_sockets_mutex);
                    client_sockets.erase(client_fd);
                }
                close(client_fd);
            });
        }
    }

    {
        lock_guard<mutex> lock(threads_mutex);
        for (auto &t : threads) {
            if (t.joinable()) t.join();
        }
    }

    cout << "Server shutdown complete.\n";
    return 0;
}

