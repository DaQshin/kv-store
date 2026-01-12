#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

int main(){

    // create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    // Define server address
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8000);

    if(bind(server_fd, (sockaddr*)& server_addr, sizeof(server_addr)) < 0){
        std::cerr << "Bind failed\n";
        return 1;
    }

    if(listen(server_fd, 5) < 0){
        std::cerr << "Listen failed\n";
        return 1;
    }

    std::cout << "Server listening on port 8000 ... \n";


    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr*)& client_addr, &client_len);
    if(client_fd < 0){
        std::cerr << "Accept failed\n";
        return 1;
    }

    std::cout << "Client connected\n";
    
    close(client_fd);
    close(client_fd);

    return 0;
}