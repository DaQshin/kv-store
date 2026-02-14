#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <chrono>
#include "log.cpp"

#define PORT 5000

void log_client_connection(const sockaddr_in& client_addr){
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);

    uint32_t ip = ntohl(client_addr.sin_addr.s_addr);
    uint32_t port = ntohs(client_addr.sin_port);

    fprintf(stderr,
        "New Connection [time: %04d-%02d-%02d %02d:%02d:%02d, address: %u.%u.%u.%u:%u]\n",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        (ip >> 24) & 0xFF,
        (ip >> 16) & 0xFF,
        (ip >> 8)  & 0xFF,
        ip & 0xFF,
        port
    );
}


static void msg(const char* msg){
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char* msg){
    fprintf(stderr, "[error:%d] %s\n", errno, msg);
}

static void die(const char* msg){
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd){
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if(errno){
        die("fcntl() get");
    }

    flags |= O_NONBLOCK;

    errno = 0;
    fcntl(fd, F_SETFL, flags);
    if(errno){
        die("fcntl() set");
    }
}

const size_t k_max_msg = 32 << 20;

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;

    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void buf_append(std::vector<uint8_t> &buf, const uint8_t* data, size_t len){
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    buf.erase(buf.begin(), buf.begin() + n);
}

static Conn* handle_accept(int fd){
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr*) &client_addr, &addrlen);
    if(connfd < 0){
        return nullptr;
    }

    fd_set_nb(connfd);

    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;

}
static void handle_write(Conn* conn){
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing, conn->outgoing.size());

    if(rv < 0 && errno == EAGAIN){
        return;
    }

    if(rv < 0){
        msg_errno("write()");
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, (size_t)rv);
    if(conn->outgoing.size() == 0){
        conn->want_read = true;
        conn->want_write = false;

    }

}

static void handle_read(Conn* conn){
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if(rv < 0 && errno == EAGAIN){
        return ;
    }

    if(rv < 0){
        msg_errno("read()");
        conn->want_read = false;
        return;
    }

    if(rv == 0){
        if(conn->incoming.size() == 0){
            msg("client closed");
        }
        else{
            msg("unexected EOF");
        }
        conn->want_close = true;
        return;
    }

}

static int32_t read_full(int fd, char* buffer, size_t n){
    while(n){
        ssize_t rv = read(fd, buffer, n);

        if(rv > 0 && rv <= n){
            n -= (size_t)rv;
            buffer += rv;
        }
        else if(rv == 0) return EOF;
        else if(errno == EINTR) continue;
        else return -1;
    }

    return 0;
}

static int32_t write_all(int fd, const char* buffer, size_t n){
    while(n){
        ssize_t rv = write(fd, buffer, n);
        if(rv > 0 && rv <= n){
            n -= (size_t)rv;
            buffer += rv;
        }
        else if(rv == -1 || errno == EINTR) continue;
        else return -1;
    }

    return 0;
}

static int32_t one_request(int connfd){
    char rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(connfd, rbuf, sizeof(rbuf) - 1);
    if(err){
        msg(err == 0 ? "EOF" : "read() error");
        return err;
    }

    int32_t len = 0;
    memcpy(&len, rbuf, 4);
    if(len > k_max_msg){
        msg("message too long");
        return -1;
    }

    err = read_full(connfd, &rbuf[4], len);
    if(err){
        msg("read() error");
        return err;
    }

    printf("Client: %.*s", len, &rbuf[4]);

    const char reply[] = "hello client";
    char wbuf[4 + sizeof(reply)];
    len = (int32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf, reply, len);

    int32_t rv = write_all(connfd, wbuf, 4 + len);

    return rv;
}



int main(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        die("socket()");
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    int val = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0){
        die("setsockopt()");
    }

    if(bind(fd, (sockaddr*)& server_addr, sizeof(server_addr)) < 0){
        die("bind()");
    }

    if(listen(fd, 10) < 0){
        die("listen()");
    }

    LOG_INFO("server starting on port %d", PORT);

    std::vector<Conn*> fd2conn;
    std::vector<pollfd> poll_args;
    while(1){
        poll_args.clear();
        struct pollfd srvfd = {fd, POLLIN, 0};
        poll_args.push_back(srvfd);

        for(Conn* conn: fd2conn){
            if(!conn) continue;
            struct pollfd clfd = {conn->fd, POLLERR, 0};
            if(conn->want_read){
                clfd.events |= POLLIN;
            }
            if(conn->want_write){
                clfd.events |= POLLOUT;
            }
            poll_args.push_back(clfd);

        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if(rv < 0 && errno == EINTR){
            continue;
        }
        if(rv < 0) die("poll()");

        if(poll_args[0].revents){
            if(Conn* conn = handle_accept(fd)){
                if(fd2conn.size() <= (size_t)conn->fd){
                    fd2conn.resize(conn->fd + 1);
                }
                fd2conn[conn->fd] = conn;
            }
        }

        for(ssize_t i = 1; i < poll_args.size(); i++){
            uint32_t ready = poll_args[i].revents;
            Conn* conn = fd2conn{poll_args[i].fd};
            if(ready & POLLIN){
                handle_read(conn);
            }
            if(ready & POLLOUT){
                handle_write(conn);
            }

            if((ready & POLLERR) || conn->want_close){
                close(conn->fd);
                fd2conn[conn->fd] = nullptr;
                delete conn;
            }
        }
    }

    return 0;

}