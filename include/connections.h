#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <set>

extern std::mutex client_sockets_mutex;
extern std::set<int> client_sockets;
extern int server_fd;
extern std::atomic<bool> stopFlag;

bool safe_send(int sockfd, const std::string& message, int timeout_ms = 30000);
bool recvLine(int sock, std::string& out, int timeout_ms = 30000);
void shutdown_server();
void signal_handler(int signum);
