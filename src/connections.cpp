#include "../include/connections.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <csignal>

#define MAX_COMMAND_LEN 1024

std::mutex client_sockets_mutex;
std::set<int> client_sockets;
int server_fd = -1;
std::atomic<bool> stopFlag{false};

bool safe_send(int sockfd, const std::string& message, int timeout_ms) {
    const char* data = message.c_str();
    size_t total_sent = 0;
    size_t to_send = message.size();

    while (total_sent < to_send) {
        pollfd pfd{sockfd, POLLOUT, 0};
        int res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) {
            if (res == 0) std::cerr << "safe_send: timeout" << std::endl;
            else perror("poll");
            return false;
        }

        ssize_t sent = send(sockfd, data + total_sent, to_send - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;

            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN ||
                errno == ETIMEDOUT || errno == EHOSTUNREACH) {
                std::cerr << "safe_send: connection broken: " << strerror(errno) << std::endl;
            } else {
                perror("send");
            }
            return false;
        }

        if (sent == 0) {
            std::cerr << "safe_send: connection closed" << std::endl;
            return false;
        }

        total_sent += sent;
    }
    return true;
}

bool recvLine(int sock, std::string& out, int timeout_ms) {
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
            std::cerr << "recvLine: connection closed\n";
            return false;
        }

        if (c == '\n') break;
        if (c != '\r') out.push_back(c);
    }

    if (out.size() >= MAX_COMMAND_LEN) {
        std::cerr << "recvLine: max command length exceeded\n";
        return false;
    }
    return true;
}

void shutdown_server() {
    stopFlag = true;

    {
        std::lock_guard<std::mutex> lock(client_sockets_mutex);
        for (int fd : client_sockets) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        client_sockets.clear();
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
