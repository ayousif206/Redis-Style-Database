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

struct Entry {
    std::string value;
    long long expiry_at = -1;
};

std::map<std::string, Entry> g_data;
std::mutex g_data_mutex;
std::mutex g_aof_mutex;

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

                g_data[key] = {value, -1};
                count++;

                ptr = val_ptr + val_len;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    std::cout << "Loaded " << count << " entries from AOF." << '\n';
}

void handle_client(int connfd) {
    char buffer[2048] = {0};

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

        if(buffer[0] == '*') {
            if(strcasestr(buffer, "SET")){
                char* cmd_ptr = strcasestr(buffer, "SET");
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
                            aof.write(buffer, bytes_received);
                            aof.close();
                        }
                        g_aof_mutex.unlock();

                        const char* reply = "+OK\r\n";
                        write(connfd, reply, strlen(reply));
                        continue;
                    }
                }
            }
        

            if(strncasecmp(buffer, "*2", 2) == 0 && strcasestr(buffer, "GET")) {
                char* cmd_ptr = strcasestr(buffer, "GET");
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
                    continue;
                }
            }

            if(strncasecmp(buffer, "*2", 2) == 0 && strcasestr(buffer, "ECHO")) {
                char* cmd_ptr = strcasestr(buffer, "ECHO");
                char* arg_len_ptr = strchr(cmd_ptr, '$');
                if(arg_len_ptr) {
                    int arg_len = atoi(arg_len_ptr + 1);
                    char* arg_ptr = strchr(arg_len_ptr, '\n') + 1;
                    
                    std::string reply = "$" + std::to_string(arg_len) + "\r\n";
                    reply.append(arg_ptr, arg_len);
                    reply += "\r\n";
                    
                    write(connfd, reply.c_str(), reply.length());
                    continue;
                }
            }
        }

        if(strcasestr(buffer, "PING")){
            const char* reply = "+PONG\r\n";
            write(connfd, reply, strlen(reply));
        }
        else{
            const char* err = "-unknown command\r\n";
            write(connfd, err, strlen(err));
        }
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