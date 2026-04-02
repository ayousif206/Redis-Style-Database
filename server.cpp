#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <thread>
#include <strings.h>
#include <map>
#include <mutex>
#include <chrono>
#include <fstream>
#include <vector>

struct Entry {
    std::string value;
    long long expiry_at = -1;
};

std::map<std::string, Entry> g_data;
std::recursive_mutex g_data_mutex;
std::recursive_mutex g_aof_mutex;
std::vector<int> g_replicas;
std::mutex g_replicas_mutex;

long long current_time_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void load_database() {
    std::ifstream aof("database.aof", std::ios::binary);
    if (!aof.is_open()) {
        return;
    }

    std::cout << "Loading database from AOF..." << '\n';
    std::string content((std::istreambuf_iterator<char>(aof)), std::istreambuf_iterator<char>());
    aof.close();

    char* ptr = &content[0];
    int count = 0;
    int expired_count = 0;

    while ((ptr = strcasestr(ptr, "SET")) != nullptr) {
        char* key_len_ptr = strchr(ptr, '$');
        if (key_len_ptr) {
            int key_len = atoi(key_len_ptr + 1);
            char* key_ptr = strchr(key_len_ptr, '\n') + 1;
            std::string key(key_ptr, key_len);

            char* val_len_ptr = strchr(key_ptr + key_len, '$');
            if (val_len_ptr) {
                int val_len = atoi(val_len_ptr + 1);
                char* val_ptr = strchr(val_len_ptr, '\n') + 1;
                std::string value(val_ptr, val_len);

                long long expiry = -1;

                char* pxat_ptr = strcasestr(val_ptr + val_len, "PXAT");
                char* next_set = strcasestr(val_ptr + val_len, "SET");

                if (pxat_ptr && (!next_set || pxat_ptr < next_set)) {
                    char* pxat_arg_len_ptr = strchr(pxat_ptr, '$');
                    if (pxat_arg_len_ptr) {
                        char* duration_ptr = strchr(pxat_arg_len_ptr, '\n') + 1;
                        expiry = std::stoll(duration_ptr);
                    }
                }

                if (expiry == -1 || expiry > current_time_ms()) {
                    g_data[key] = {value, expiry};
                    count++;
                } else {
                    expired_count++;
                }

                ptr = val_ptr + val_len;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    std::cout << "Loaded " << count << " entries from AOF." << '\n';
    if (expired_count > 0) {
        std::cout << "Skipped " << expired_count << " expired entries." << '\n';
    }
}

void process_command(const char* buffer, ssize_t bytes_received, int connfd) {
    if(strcasestr(buffer, "PSYNC")) {
        g_replicas_mutex.lock();
        g_replicas.push_back(connfd);
        g_replicas_mutex.unlock();
        
        const char* reply = "+FULLRESYNC 000000 0\r\n";
        write(connfd, reply, strlen(reply));
        return;
    }

    if(buffer[0] == '*') {
        if(strcasestr(buffer, "SET")){
            char* cmd_ptr = (char*)strcasestr(buffer, "SET");
            char* key_len_ptr = strchr(cmd_ptr, '$');
            if(key_len_ptr) {
                int key_len = atoi(key_len_ptr + 1);
                char* key_ptr = strchr(key_len_ptr, '\n') + 1;
                std::string key(key_ptr, key_len);

                char* val_len_ptr = strchr(key_ptr + key_len, '$');
                if(val_len_ptr) {
                    int val_len = atoi(val_len_ptr + 1);
                    char* val_ptr = strchr(val_len_ptr, '\n') +1;
                    std::string value(val_ptr, val_len);

                    long long expiry = -1;
                    char* px_ptr = strcasestr(val_ptr + val_len, "PX");
                    if (px_ptr) {
                        char* px_arg_len_ptr = strchr(px_ptr, '$');
                        if (px_arg_len_ptr) {
                            char* duration_ptr = strchr(px_arg_len_ptr, '\n') + 1;
                            int duration_ms = atoi(duration_ptr);
                            expiry = current_time_ms() + duration_ms;
                        }
                    }

                    g_data_mutex.lock();
                    g_data[key] = {value, expiry};
                    g_data_mutex.unlock();

                    g_aof_mutex.lock();
                    std::ofstream aof("database.aof", std::ios::app);
                    if (aof.is_open()) {
                        if (expiry == -1) {
                            aof.write(buffer, bytes_received);
                        } else {
                            std::string exp_str = std::to_string(expiry);
                            std::string aof_cmd = "*5\r\n$3\r\nSET\r\n$" + std::to_string(key.length()) + "\r\n" + key + "\r\n$" + std::to_string(value.length()) + "\r\n" + value + "\r\n$4\r\nPXAT\r\n$" + std::to_string(exp_str.length()) + "\r\n" + exp_str + "\r\n";                                
                            aof.write(aof_cmd.c_str(), aof_cmd.length());
                        }
                        aof.close();
                    }
                    g_aof_mutex.unlock();

                    const char* reply = "+OK\r\n";
                    write(connfd, reply, strlen(reply));

                    g_replicas_mutex.lock();
                    for (int replica_fd : g_replicas) {
                        write(replica_fd, buffer, bytes_received);
                    }
                    g_replicas_mutex.unlock();

                    return;
                }
            }
        }

        if(strncasecmp(buffer, "*2", 2) == 0 && strcasestr(buffer, "GET")) {
            char* cmd_ptr = (char*)strcasestr(buffer, "GET");
            char* key_len_ptr = strchr(cmd_ptr, '$');
            if(key_len_ptr) {
                int key_len = atoi(key_len_ptr + 1);
                char* key_ptr = strchr(key_len_ptr, '\n') + 1;
                std::string key(key_ptr, key_len);

                g_data_mutex.lock();
                bool exists = false;
                std::string value = "";

                auto it = g_data.find(key);
                if (it != g_data.end()) {
                    if(it->second.expiry_at != -1 && it->second.expiry_at < current_time_ms()) {
                        g_data.erase(it);
                        exists = false;
                    } else {
                        value = it->second.value;
                        exists = true;
                    }
                }
                g_data_mutex.unlock();

                if(exists) {
                    std::string reply = "$" + std::to_string(value.length()) + "\r\n" + value + "\r\n";
                    write(connfd, reply.c_str(), reply.length());
                } else {
                    const char* reply = "$-1\r\n";
                    write(connfd, reply, strlen(reply));
                }
                return;
            }
        }

        if(strncasecmp(buffer, "*2", 2) == 0 && strcasestr(buffer, "ECHO")) {
            char* cmd_ptr = (char*)strcasestr(buffer, "ECHO");
            char* arg_len_ptr = strchr(cmd_ptr, '$');
            if(arg_len_ptr) {
                int arg_len = atoi(arg_len_ptr + 1);
                char* arg_ptr = strchr(arg_len_ptr, '\n') + 1;
                
                std::string reply = "$" + std::to_string(arg_len) + "\r\n";
                reply.append(arg_ptr, arg_len);
                reply += "\r\n";
                
                write(connfd, reply.c_str(), reply.length());
                return;
            }
        }
    }

    if(strcasestr(buffer, "PING")){
        const char* reply = "+PONG\r\n";
        write(connfd, reply, strlen(reply));
    } else {
        const char* err = "-ERR unknown command\r\n";
        write(connfd, err, strlen(err));
    }
}

void handle_client(int connfd) {
    char buffer[2048] = {0};
    bool in_transaction = false;
    std::vector<std::string> transaction_queue;

    while(true){
        memset(buffer, 0, sizeof(buffer));

        ssize_t bytes_received = read(connfd, buffer, sizeof(buffer)-1);
        if(bytes_received <= 0){
            break;
        }

        if(strncasecmp(buffer, "*2", 2) == 0 && strcasestr(buffer, "COMMAND")) {
            const char* reply = "+OK\r\n";
            write(connfd, reply, strlen(reply));
            continue;
        }

        if(strncasecmp(buffer, "*1", 2) == 0 && strcasestr(buffer, "MULTI")) {
            in_transaction = true;
            const char* reply = "+OK\r\n";
            write(connfd, reply, strlen(reply));
            continue;
        }

        if(strncasecmp(buffer, "*1", 2) == 0 && strcasestr(buffer, "DISCARD")) {
            if (!in_transaction) {
                const char* err = "-ERR DISCARD without MULTI\r\n";
                write(connfd, err, strlen(err));
                continue;
            }
            in_transaction = false;
            transaction_queue.clear();
            const char* reply = "+OK\r\n";
            write(connfd, reply, strlen(reply));
            continue;
        }

        if(strncasecmp(buffer, "*1", 2) == 0 && strcasestr(buffer, "EXEC")) {
            if (!in_transaction) {
                const char* err = "-ERR EXEC without MULTI\r\n";
                write(connfd, err, strlen(err));
                continue;
            }

            g_data_mutex.lock();
            g_aof_mutex.lock();
            in_transaction = false;

            std::string array_header = "*" + std::to_string(transaction_queue.size()) + "\r\n";
            write(connfd, array_header.c_str(), array_header.length());

            for (const std::string& queued_cmd : transaction_queue) {
                process_command(queued_cmd.c_str(), queued_cmd.length(), connfd);
            }

            transaction_queue.clear();
            g_aof_mutex.unlock();
            g_data_mutex.unlock();
            continue;
        }

        if (in_transaction) {
            transaction_queue.push_back(std::string(buffer, bytes_received));
            const char* reply = "+QUEUED\r\n";
            write(connfd, reply, strlen(reply));
            continue;
        }

        process_command(buffer, bytes_received, connfd);
    }
    close(connfd);
}

int main(){
    load_database();

    int val = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr  = htonl(INADDR_ANY);
    
    bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    listen(fd, SOMAXCONN);
    
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);

    while(true){
        int connfd = accept(fd, (struct sockaddr *) &client_addr, &socklen);
        if(connfd < 0){
            continue;
        }

        std::thread(handle_client, connfd).detach();
    }

    return 0;
}