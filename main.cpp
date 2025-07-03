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
#include <unistd.h>
#include <netinet/in.h>

using namespace std;

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

map<string, Channel> channels;
mutex global_mtx;
int server_fd = -1;

bool safe_send(int sockfd, const string& message) {
    const char* data = message.c_str();
    size_t total_sent = 0;
    size_t to_send = message.size();

    while (total_sent < to_send) {
        ssize_t sent = send(sockfd, data + total_sent, to_send - total_sent, 0);
        if (sent < 0) {
            perror("send failed");
            return false;
        }
        total_sent += sent;
    }
    return true;
}

void handle_client(int client_fd) {
    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) break;

        string cmd(buffer);
        cmd.erase(cmd.find_last_not_of(" \r\n") + 1); // trim

        istringstream iss(cmd);
        string action, channel, nick;
        iss >> action >> channel >> nick;

        if (channel.empty() || nick.empty()) {
            safe_send(client_fd, "ERROR: invalid command\n");
            continue;
        }

        
        {
            lock_guard<mutex> lock(global_mtx);
            if (channels.find(channel) == channels.end()) {
                if (action == "send" || action == "join") {
                    channels.try_emplace(channel);
                }
            }
        }

        Channel &ch = channels[channel];
        lock_guard<mutex> ch_lock(ch.mtx);

        if (action == "join") {
            if (ch.members.count(nick)) {
                safe_send(client_fd, "ERROR: already in channel\n");
            } else {
                ch.members.insert(nick);
                safe_send(client_fd, "OK\n");
            }
        }

        else if (action == "exit") {
            if (!ch.members.count(nick)) {
                safe_send(client_fd, "ERROR: not in channel\n");
            } else {
                ch.members.erase(nick);
                safe_send(client_fd, "OK\n");
            }
        }

        else if (action == "send") {
            string message;
            getline(iss, message);
            if (!message.empty() && message[0] == ' ') message = message.substr(1);
            if (message.length() > 256) message = message.substr(0, 256);

            if (!ch.members.count(nick)) {
                safe_send(client_fd, "ERROR: not in channel\n");
            } else {
                ch.messages.emplace_back(nick, message);
                if (ch.messages.size() > 40) ch.messages.pop_front();
                safe_send(client_fd, "OK\n");
            }
        }

        else if (action == "read") {
            if (!ch.members.count(nick)) {
                safe_send(client_fd, "ERROR: not in channel\n");
            } else {
                int count = ch.messages.size();
                safe_send(client_fd, "OK " + to_string(count) + "\n");
                for (const auto& msg : ch.messages) {
                    safe_send(client_fd, msg.nick + ": " + msg.text + "\n");
                }
            }
        }
        else {
            safe_send(client_fd, "ERROR: unknown command\n");
        }
    }

    close(client_fd);
}

void signal_handler(int) {
    cout << "\nServer shutting down...\n";
    if (server_fd != -1) close(server_fd);
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: ./server <port>\n";
        return 1;
    }

    signal(SIGINT, signal_handler);
    int port = stoi(argv[1]);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    cout << "Server is listening on port " << port << "\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            thread(handle_client, client_fd).detach();
        } else {
            perror("accept");
        }
    }

    return 0;
}
