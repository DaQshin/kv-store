#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <map>
#include "../include/log.h"
#include "hashtable.h"

#define PORT 5000
#define MAX_EVENTS 64

#define container_of(ptr, T, member) \
    ((T*)(char*)ptr - offsetof(T, member))

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

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2
};

// static std::map<std::string, std::string> global_ds;

static void buf_append(std::vector<uint8_t> &buf, const uint8_t* data, size_t len){
    buf.insert(buf.end(), data, data + len); // O(n) everytime!!
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    buf.erase(buf.begin(), buf.begin() + n); // O(n) everytime!! 
}

static void make_response(std::string& val, uint32_t status, std::vector<uint8_t>& out){
    uint32_t res_len = 4 + (uint32_t)val.size();
    buf_append(out, (const uint8_t*)& res_len, 4);
    buf_append(out, (const uint8_t*)& status, 4);
    buf_append(out, (const uint8_t*)val.data(), (size_t)(val.end() - val.begin()));
}

static uint32_t str_hash(const uint8_t* data, size_t len){
    uint32_t h = 0x811C9DC5;
    for(size_t i = 0; i < len; i++){
        h = (h + data[i]) * 0x01000193;
    }

    return h;
}


static struct{
    HMap db;
} g_data;

struct Entry {
    struct HNode node;
    std::string key;
    std::string value;
};

static bool entry_eq(HNode* lhs, HNode* rhs){
    struct Entry* le = container_of(lhs, struct Entry, node);
    struct Entry* re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

static void get(std::vector<std::string>& cmd, std::vector<uint8_t>& out){
    Entry key;
    uint32_t status = RES_OK;
    key.key.swap(cmd[1]);
    key.node.hash = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    std::string val = "";
    if(!node){
       status = RES_NX;
    }
    else{
        val = container_of(node, Entry, node)->value;
    }

    assert(val.size() <= k_max_msg);
    make_response(val, status, out);
}

// static void set(std::vector<std::string>& cmd, std::vector<uint8_t>& out);
// static void del(std::vector<std::string>& cmd, std::vector<uint8_t>& out);

static void do_request(std::vector<std::string>& cmd, std::vector<uint8_t>& out){
    if(cmd.size() == 2 && cmd[0] == "GET"){
        get(cmd, out);
    }
    // else if(cmd.size() == 3 && cmd[0] == "SET"){
    //     set(cmd, out);
    // }
    // else if(cmd.size() == 2 && cmd[0] == "DEL"){
    //     del(cmd, out);
    // }
}


static bool read_u32(const uint8_t*& cur, const uint8_t*& end, uint32_t* out){
    if(cur + 4 > end) return false;

    memcpy(out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t*& cur, const uint8_t*& end, size_t n, std::string& out){
    if(cur + n > end) return false;

    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t*& data, size_t size, std::vector<std::string>& out){
    const uint8_t* end = data + size;

    uint32_t nstr = 0;
    if(!read_u32(data, end, &nstr)) return -1;

    while(out.size() < nstr){
        uint32_t len = 0;
        if(!read_u32(data, end, &len)) return -1;

        out.push_back(std::string());
        if(!read_str(data, end, len, out.back())) return -1;
    }

    if(data != end) return -1;

    return 0;
}

static bool try_one_request(Conn* conn){
    if(conn->incoming.size() < 4) return false;

    uint32_t total_len = 0;
    memcpy(&total_len, conn->incoming.data(), 4);
    if(total_len > k_max_msg){
        msg("msg too long");
        conn->want_close = true;
        return false;
    }

    if(4 + total_len > conn->incoming.size()) return false;

    const uint8_t* request = &conn->incoming[4];
    LOG_INFO("Client: [total_len:%u data:%.*s]\n",
         total_len,
         (int)(total_len < 100 ? total_len : 100),
         (char*)request);
    
    std::vector<std::string> cmd;
    if(parse_req(request, total_len, cmd) < 0){
        msg("bad request");
        return false;
    }

    do_request(cmd, conn->outgoing);

    buf_consume(conn->incoming, 4 + total_len);

    return true;
}


static Conn* handle_accept(int fd){
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr*) &client_addr, &addrlen);
    if(connfd < 0){
        return nullptr;
    }

    uint32_t ip = ntohl(client_addr.sin_addr.s_addr);
    uint32_t port = ntohs(client_addr.sin_port);
    LOG_INFO("NEW CONNECTION [address: %u.%u.%u.%u:%u]\n",
        (ip >> 24) & 0xFF,
        (ip >> 16) & 0xFF,
        (ip >> 8)  & 0xFF,
        ip & 0xFF,
        port);

    fd_set_nb(connfd);

    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;

}



static void handle_write(Conn* conn){
    // assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
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
    if(rv < 0 && errno == EAGAIN) return;

    if(rv < 0){
        msg_errno("read()");
        conn->want_close = true;
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

    buf_append(conn->incoming, buf, (size_t)rv);

    while(try_one_request(conn)){}

    if(conn->outgoing.size() > 0){
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }

}

int main(){
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        die("socket()");
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    int val = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0){
        die("setsockopt()");
    }

    if(bind(server_fd, (sockaddr*)& server_addr, sizeof(server_addr)) < 0){
        die("bind()");
    }

    if(listen(server_fd, SOMAXCONN) < 0){
        die("listen()");
    }

    LOG_INFO("server running on port %d", PORT);

    int epoll_fd = epoll_create1(0);

    if(epoll_fd < 0){
        die("epoll_create1()");
    }

    epoll_event sev{};
    sev.events = EPOLLIN;
    sev.data.fd = server_fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &sev) < 0){
        die("epoll_ctl");
    }

    epoll_event events[MAX_EVENTS];
    std::vector<Conn*> fd2conn;

    while(true){
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if(n < 0){
            die("epoll_wait()");
        }

        for(int i = 0; i < n; i++){
            int fd = events[i].data.fd;
            if(fd == server_fd){
                Conn* conn;
                if(!(conn = handle_accept(fd))){
                    msg("connection failed.");
                    continue;
                }

                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET;
                cev.data.fd = conn->fd;
                if(fd2conn.size() <= (size_t)conn->fd){
                    fd2conn.resize(conn->fd + 1);
                }
                fd2conn[conn->fd] = conn;
                if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->fd, &cev) < 0){
                    msg("epoll_ctl()");
                    continue;
                }
            }
            else{
                int e = events[i].events;

                if((e & (EPOLLERR | EPOLLHUP)) || fd2conn[fd]->want_close){
                    Conn* conn = fd2conn[fd];
                    close(conn->fd);
                    delete conn;
                    fd2conn[fd] = nullptr;
                }

                if(e & EPOLLIN){
                    handle_read(fd2conn[fd]);
                }

                if(e & EPOLLOUT){
                    handle_write(fd2conn[fd]);
                }
            }
        }
        
    }
}