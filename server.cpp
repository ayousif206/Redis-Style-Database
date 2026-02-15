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

std::map<std::string, std::string> g_data;
std::mutex g_data_mutex;

void handle_client(int connfd) {
    char buffer[1024] = {0};

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

        if(buffer[0] == '*'){
            char* p = buffer;

            char* dollarsign = strchr(p, '$');
            if(dollarsign) {
                char* stringstart = strchr(dollarsign, '\n') + 1;
                if(stringstart) {
                    if(strncmp(stringstart, "ECHO", 4) == 0){
                        char* nextdollar = strchr(stringstart, '$');
                        if(nextdollar){
                            int lenmessage = atoi(nextdollar+1);
                            char* messagestart = strchr(nextdollar, '\n') + 1;
                            if(messagestart){
                                std::string reply = "$" + std::to_string(lenmessage) + "\r\n";
                                reply.append(messagestart, lenmessage);
                                reply += "\r\n";
                                
                                write(connfd, reply.c_str(), reply.length());
                                continue;
                            }
                        }
                    }
                }
            }
        }

        if(strstr(buffer, "PING")){
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