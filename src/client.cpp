#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include "log.cpp"

#define PORT 5000

const size_t k_max_msg = 32 << 20;

static void msg(const char* msg){
    fprintf(stderr, "%s\n", msg);
}

static void die(const char* msg){
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, uint8_t* read_buffer, size_t n){
    while(n > 0){
        ssize_t rv = read(fd, read_buffer, n);
        LOG_DEBUG("Bytes read: %d", rv);
        if(rv <= 0){
            return -1;
        }

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        read_buffer += rv;
    }

    return 0;
}

static int32_t write_all(int fd, const uint8_t* write_buffer, size_t n){
    while(n > 0){
        ssize_t rv = write(fd, write_buffer, n);
        LOG_DEBUG("bytes written: %d", rv);
        if(rv <= 0){
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        write_buffer += rv;
    }

    return 0;
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t* data, size_t len){
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    buf.erase(buf.begin(), buf.begin() + n);
}

static int32_t send_req(int fd, const uint8_t* msg, size_t len){

    LOG_DEBUG("send_req() - length: %d", len);

    if(len > k_max_msg){
        return -1;
    }

    std::vector<uint8_t> wbuf;
    buf_append(wbuf, (const uint8_t*)&len, 4);
    buf_append(wbuf, msg, len);
    return write_all(fd, wbuf.data(), wbuf.size());
}

static int32_t read_res(int fd){
    std::vector<uint8_t> rbuf;
    rbuf.resize(4);
    errno = 0;
    int32_t err = read_full(fd, &rbuf[0], 4);
    if(err){
        if(errno == 0){
            msg("EOF");
        }
        else msg("read_full()");

        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);
    if(len > k_max_msg){
        // msg("msg too long");
        LOG_DEBUG("read_res() - message length limit exceeded %d | read buffer size %d", len, rbuf.size());
        LOG_DEBUG("Raw length bytes: %02x %02x %02x %02x\n",
       rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
        return -1;
    }

    rbuf.resize(4 + len);
    err = read_full(fd, &rbuf[4], len);
    LOG_DEBUG("read_res() - message: %s", (const uint8_t*)&rbuf[4]);
    if(err){
        msg("read()");
        return err;
    }

    LOG_DEBUG("read_res() - len:%u data:%.*s\n", len, len < 100 ? len : 100, &rbuf[4]);
    return 0;

}

int main(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0){
        die("socket()");
    }

    sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = ntohs(PORT);
    client_addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);

    if(connect(fd, (sockaddr*)& client_addr, sizeof(client_addr)) < 0){
        die("connect()");
    }

    std::vector<std::string> query_list = {
        "hello1", "hello2", "hello3",
        // std::string(k_max_msg, 'z'),
        "hello5",
    };

    int32_t err;
    for(const std::string& q: query_list){
        err = send_req(fd, (uint8_t *)q.data(), q.size());
        if(err){
            goto L_DONE;
        }
    }

    for(int i = 0; i < query_list.size(); i++){
        err = read_res(fd);
        if(err){
            goto L_DONE;
        }
    }
    

    L_DONE:
        close(fd);

}