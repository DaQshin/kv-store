#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <cerrno>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iostream>

#define PORT 5000


int sockopt = 1;
fd_set fr, fw, fe;
std::vector<int> fds;

void setNBIO(){

}


int main(){

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if(server_fd < 0){
        perror("socket");
        return 1;
    }

    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));

    if(bind(server_fd, (sockaddr*)& server_addr, sizeof(server_addr)) < 0){
        perror("bind");
        return 1;
    }

    if(listen(server_fd, 10) < 0){
        perror("listen");
        return 1;
    }

    std::cout << "Server listening at port " << PORT << std::endl;

    while(1){
        FD_ZERO(&fr);
        FD_ZERO(&fw);

        FD_SET(server_fd, &fr);
        FD_SET(server_fd, &fw);

        int max_fd = server_fd;
        for(int fd: fds){
            FD_SET(fd, &fr);
            FD_SET(fd, &fw);

            max_fd = max(max_fd, fd);
        }

        select(max_fd + 1, &fr, &fw, nullptr, nullptr);

        acceptRequests(server_fd);
    }
}