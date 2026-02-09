#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string.h>
#include <assert.h>

#define PORT 5000

const size_t k_max_msg = 4096;

static void msg(const char* msg){
    fprintf(stderr, "%s\n", msg);
}

static void error_handler(const char* msg){
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, char* read_buffer, size_t n){
    while(n > 0){
        ssize_t rv = read(fd, read_buffer, n);
        if(rv <= 0){
            return -1;
        }

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        read_buffer += rv;
    }

    return 0;
}

static int32_t write_all(int fd, const char* write_buffer, size_t n){
    while(n > 0){
        ssize_t rv = write(fd, write_buffer, n);
        if(rv <= 0){
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        write_buffer += rv;
    }

    return 0;
}

static int32_t query(int fd, char* message){
    uint32_t len = (uint32_t)strlen(message);

    if(len > k_max_msg){
        return -1;
    }

    char wbuf[4 + k_max_msg];
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], message, len);
    if(int32_t err = write_all(fd, wbuf, 4 + len)){
        return err;
    }

    char rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);
    if(err){
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }

    memcpy(rbuf, &len, 4);
    if(len > k_max_msg){
        msg("message too long");
        return -1;
    }

    err = read_full(fd, &rbuf[4], len);

    if(err){
        msg("read() error");
        return err;
    }

    printf("Server: %.*s", len, &rbuf[4]);
    return 0;

}

int main(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0){
        error_handler("socket()");
    }

    sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = ntohs(PORT);
    client_addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);

    if(connect(fd, (sockaddr*)& client_addr, sizeof(client_addr)) < 0){
        error_handler("connect()");
    }

    // char write_buffer[] = "hello server";
    // write(fd, write_buffer, sizeof(write_buffer));

    // char read_buffer[1024];
    // int n = read(fd, read_buffer, sizeof(read_buffer) - 1);
    // if(n < 0){
    //     error_handler("read()");
    // }

    // read_buffer[n] = '\0';

    // std::cout << "Server: " << read_buffer << std::endl;


    int32_t err = query(fd, "hello1");
    if(err){
        goto L_DONE;
    }

    err = query(fd, "hello2");
    if(err){
        goto L_DONE;
    }

    L_DONE:
        close(fd);

}