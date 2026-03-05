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
#include <map>
#include "log.cpp"

#define PORT 5000

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

static std::map<std::string, std::string> global_ds;

static void buf_append(std::vector<uint8_t> &buf, const uint8_t* data, size_t len){
    buf.insert(buf.end(), data, data + len); // to be optimized
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    buf.erase(buf.begin(), buf.begin() + n); // to be optimized 
}

static void do_request(std::vector<std::string>& cmd, std::vector<uint8_t>& out){
    uint32_t status = RES_OK;
    std::string val = "[NO REPLY]";
    if(cmd.size() == 2 && cmd[0] == "GET"){
        auto it = global_ds.find(cmd[1]);
        if(it == global_ds.end()){
            status = RES_NX;
            val = "Value Not Found.";
        }
        else val = it->second;
    }
    else if(cmd.size() == 3 && cmd[0] == "SET"){
        global_ds[cmd[1]] = std::move(cmd[2]);
        val = "Operation successful.";
    }
    else if(cmd.size() == 2 && cmd[0] == "DEL"){
        global_ds.erase(cmd[1]);
        val = "Operation successful.";
    }
    else{
        status = RES_ERR;
        val = "Operation falied.";
    }

    uint32_t res_len = 4 + (uint32_t)(val.end() - val.begin());
    buf_append(out, (const uint8_t*)& res_len, 4);
    buf_append(out, (const uint8_t*)& status, 4);
    buf_append(out, (const uint8_t*)val.data(), (size_t)(val.end() - val.begin()));
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
        if(!read_u32(data, end, &len)) std::cout << "path" << 3 << std::endl;

        out.push_back(std::string());
        if(!read_str(data, end, len, out.back())) std::cout << "path" << 4 << std::endl;
    }

    if(data != end) std::cout << "path" << 5 << std::endl;

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
    assert(conn->outgoing.size() > 0);
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
            Conn* conn = fd2conn[poll_args[i].fd];
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

}