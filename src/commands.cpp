#include "../include/commands.h"
#include "../include/connections.h"
#include "../include/validation.h"
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>
#include <unistd.h>

struct Message {
    std::string nick;
    std::string text;
    Message(const std::string& n, const std::string& t) : nick(n), text(t) {}
};

struct Channel {
    std::deque<Message> messages;
    std::set<std::string> members;
    std::mutex mtx;
};

std::mutex channels_mutex;
std::map<std::string, std::shared_ptr<Channel>> channels;

static void handleJoin(int client_fd, Channel& ch, const std::string& nick) {
    std::string response;
    {
        std::lock_guard<std::mutex> lk(ch.mtx);
        if (!ch.members.insert(nick).second) {
            response = "ERROR: user already in channel\n";
        } else {
            response = "OK\n";
        }
    }
    safe_send(client_fd, response);
}

static void handleExit(int client_fd, Channel& ch, const std::string& nick) {
    std::string response;
    {
        std::lock_guard<std::mutex> lk(ch.mtx);
        if (!ch.members.erase(nick)) {
            response = "ERROR: not in channel\n";
        } else {
            response = "OK\n"; 
        }
    }
    safe_send(client_fd, response);

}

static void handleSend(int client_fd, Channel& ch, const std::string& nick, const std::string& message) {
    std::string truncated = message;
    std::string response;
    
    if (truncated.empty()) {
        response = "ERROR: message cannot be empty\n";
        safe_send(client_fd, "ERROR: message cannot be empty\n");
        return;
    }

    if (truncated.size() > 256) {
        truncated.resize(256);
    }

    {
        std::lock_guard<std::mutex> lk(ch.mtx);
        if (!ch.members.count(nick)) {
            response = "ERROR: not in channel\n";
        } else {
            ch.messages.emplace_back(nick, truncated);
            if (ch.messages.size() > 40) {
                ch.messages.pop_front();
            }
            response = "OK\n";
        }
    }
    safe_send(client_fd, response);
}


static void handleRead(int client_fd, Channel& ch, const std::string& nick) {
    std::vector<Message> snapshot;
    std::string response;
    {
        std::lock_guard<std::mutex> lk(ch.mtx);
        if (!ch.members.count(nick)) {
            response = "ERROR: not in channel\n";
            //safe_send(client_fd, "ERROR: not in channel\n");
            //return;
        }
        snapshot.assign(ch.messages.begin(), ch.messages.end());
    }
    
    if (!response.empty()) {
        safe_send(client_fd, response);
        return;
    }

    {
        std::string header = "OK " + std::to_string(snapshot.size()) + "\n";
        if (!safe_send(client_fd, header)) {
            return; 
        }
    }

    for (const auto& msg : snapshot) {
        std::string line = msg.nick + ": " + msg.text + "\n";
        if (!safe_send(client_fd, line)) {
            break; 
        }
    }
}

void handle_client(int client_fd) {
    std::string line;
    while (!stopFlag && recvLine(client_fd, line)) {
        std::string cmd = trim(line);
        if (cmd.empty()) continue;

        std::istringstream iss(cmd);
        std::string action, channel_name, nick;
        iss >> action >> channel_name >> nick;
        
        if (action.empty() || channel_name.empty() || nick.empty()) {
            safe_send(client_fd, "ERROR: invalid command\n");
            continue;
        }
        if (channel_name.size() > 24 || nick.size() > 24) {
            safe_send(client_fd, "ERROR: channel or nick too long\n");
            continue;
        }

        std::shared_ptr<Channel> ch_ptr;
        {
            std::lock_guard<std::mutex> lock(channels_mutex);
            auto it = channels.find(channel_name); 
            
            if (it == channels.end()) {
                if (action == "send" || action == "join") {
                    ch_ptr = std::make_shared<Channel>();
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
            handleJoin(client_fd, ch, nick);
        } 
        else if (action == "exit") {
            handleExit(client_fd, ch, nick);
        } 
        else if (action == "send") {
            std::string message;
            std::getline(iss, message);
            handleSend(client_fd, ch, nick, trim(message));
        } 
        else if (action == "read") {
            handleRead(client_fd, ch, nick);
        } 
        else {
            safe_send(client_fd, "ERROR: unknown command\n");
        }
    }
    
    close(client_fd);
    {
        std::lock_guard<std::mutex> lock(client_sockets_mutex);
        client_sockets.erase(client_fd);
    }
}
