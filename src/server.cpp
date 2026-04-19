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
#include "log.h"
#include "hashtable.h"

#define PORT 5000
#define MAX_EVENTS 64

#define container_of(ptr, T, member) \
    ((T*)(char*)(ptr) - offsetof(T, member))

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

typedef std::vector<uint8_t> Buffer;


struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;

    Buffer incoming;
    Buffer outgoing;
};


// enum {
//     RES_OK = 0,
//     RES_ERR = 1,
//     RES_NX = 2
// };

enum {
    ERR_UNKNOWN = 1,
    ERR_TOO_BIG = 2
};

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_INT = 2,
    TAG_STRING = 3,
    TAG_DOUBLE = 4,
    TAG_ARRAY = 5,
};

static void buf_append(Buffer &buf, const uint8_t* data, size_t len){
    printf("buf_append()\n");
    buf.insert(buf.end(), data, data + len); // Buffer()
}

static void buf_consume(Buffer &buf, size_t n){
    printf("buf_consume()\n");
    buf.erase(buf.begin(), buf.begin() + n); // Buffer() 
}

// static void make_response(const std::string& val, uint32_t status, Buffer& out){

//     std::cout << "data: " << val << std::endl;

//     uint32_t res_len = 4 + (uint32_t)val.size();
//     printf("res_len = [%d]  status = [%d]\n", res_len, status);
//     buf_append(out, (const uint8_t*)& res_len, 4);
//     buf_append(out, (const uint8_t*)& status, 4);
//     buf_append(out, (const uint8_t*)val.data(), (size_t)val.size());
// }

static uint32_t str_hash(const uint8_t* data, size_t len){
    // FNV-1a
    uint32_t h = 0x811C9DC5;
    for(size_t i = 0; i < len; i++){
        h = (h ^ data[i]) * 0x01000193;
    }

    return h;
}

static struct {
    HMap db;
} g_data;

struct Entry{
    struct HNode node;
    std::string key;
    std::string value;
};

static bool entry_eq(HNode* lhs, HNode* rhs){
    struct Entry* l = container_of(lhs, struct Entry, node);
    struct Entry* r = container_of(rhs, struct Entry, node);
    return l->key == r->key;
}


static void buf_append_u8(Buffer& buf, uint8_t data){
    buf.push_back(data);
}

static void buf_append_u32(Buffer& buf, uint32_t data){
    buf_append(buf, (const uint8_t*)&data, 4);
}

static void buf_append_i64(Buffer& buf, int64_t data){
    buf_append(buf, (const uint8_t*)&data, 8);
}

static void buf_append_dbl(Buffer& buf, double data){
    buf_append(buf, (const uint8_t*)& data, 8);
}

static void out_nil(Buffer& out){
    buf_append_u8(out, TAG_NIL);
}

static void out_err(Buffer& out, uint32_t code, const std::string& msg){
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)msg.size());
    buf_append(out, (const uint8_t*) msg.data(), msg.size());
}

static void out_str(Buffer& out, const char* data, size_t size){
    buf_append_u8(out, TAG_STRING);
    buf_append_u32(out, (uint32_t)size);
    buf_append(out, (const uint8_t*)data, size);
}

static void out_int(Buffer& out, int64_t data){
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, data);
}

static void out_dbl(Buffer& out, double data){
    buf_append_u8(out, TAG_DOUBLE);
    buf_append_dbl(out, data);
}

static void out_arr(Buffer& out, uint32_t n){
    buf_append_u8(out, TAG_ARRAY);
    buf_append_u32(out, n);
}

static void get(std::vector<std::string>& cmd, Buffer& out){
    std::string val = "";
    if(!g_data.db.newer.table) {
        out_nil(out);
        return;
    }
    struct Entry key;
    key.key.swap(cmd[1]);
    key.node.hash = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(node){
        val = container_of(node, struct Entry, node)->value;
    }
    else {
        out_nil(out);
    }

    assert(val.size() <= k_max_msg);
    out_str(out, (const char*)val.data(), val.size());
}

static void set(std::vector<std::string>& cmd, Buffer& out){
    struct Entry key;
    key.key.swap(cmd[1]);
    key.node.hash = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(node){
        container_of(node, struct Entry, node)->value.swap(cmd[2]);
    }
    else{
       struct Entry* ent = new Entry();
       ent->key.swap(key.key);
       ent->value.swap(cmd[2]);
       ent->node.hash = key.node.hash;
       hm_insert(&g_data.db, &ent->node);
    }   

    std::string response = "Operation successful.";
    out_str(out, (const char*)response.data(), response.size());
}

static void del(std::vector<std::string>& cmd, Buffer& out){
    std::string response = "Operation successful.";
    if(!g_data.db.newer.table) {
        response = "Empty DB.";
        out_str(out, (const char*)response.data(), response.size());
        return;
    }
    struct Entry key;
    key.key.swap(cmd[1]);
    key.node.hash = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(node){
        delete container_of(node, struct Entry, node);
    }
    else response = "Value Not Found.";

    out_str(out, (const char*)response.data(), response.size());

}

static void flush(Buffer& out){
    hm_clear(&g_data.db);
    std::string response = "Operation successful.";
    out_str(out, (const char*)response.data(), response.size());
}

static void exists(std::vector<std::string>& cmd, Buffer& out){
    printf("in exists()");
    struct Entry key;
    key.key.swap(cmd[1]);
    key.node.hash = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    uint8_t exists = node ? 1 : 0;

    out_int(out, (int64_t)exists);
}

static void do_request(std::vector<std::string>& cmd, Buffer& out){

    printf("do_request()\n");

    LOG_INFO("db state: newer.table=%p newer.mask=%zu newer.size=%zu",
         (void*)g_data.db.newer.table,
         g_data.db.newer.mask,
         g_data.db.newer.size);
    
    if(cmd.size() == 2 && cmd[0] == "GET"){
        LOG_INFO("cmd.size=%zu cmd[0]=%s cmd[1]=%s",
         cmd.size(),
         cmd[0].c_str(),
         cmd[1].c_str());
        get(cmd, out);
    }

    else if(cmd.size() == 3 && cmd[0] == "SET"){
        LOG_INFO("cmd.size=%zu cmd[0]=%s cmd[1]=%s cmd[2]=%s",
         cmd.size(),
         cmd[0].c_str(),
         cmd[1].c_str(),
        cmd[2].c_str());
        set(cmd, out);
    }

    else if(cmd.size() == 2 && cmd[0] == "EXISTS"){
        LOG_INFO("cmd.size=%zu cmd[0]=%s cmd[1]=%s",
         cmd.size(),
         cmd[0].c_str(),
         cmd[1].c_str());
        exists(cmd, out);
    }

    else if(cmd.size() == 2 && cmd[0] == "DEL"){
        LOG_INFO("cmd.size=%zu cmd[0]=%s cmd[1]=%s",
         cmd.size(),
         cmd[0].c_str(),
        cmd[1].c_str());
        del(cmd, out);
    }

    else if(cmd.size() == 1 && cmd[0] == "FLUSH"){
        flush(out);
    }

    else{
        out_err(out, ERR_UNKNOWN, "Unknown Command.");
    }

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

void log_payload(const uint8_t* data, size_t len) {
    printf("payload (%zu bytes): ", len);
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static void resp_header_alloc(Buffer& out, size_t* header){
    printf("resp_header_alloc()\n");
    *header = out.size();
    buf_append_u32(out, 0);
}

static size_t resp_size(Buffer& out, size_t header){
    return out.size() - header - 4;
}

static void resp_header_assign(Buffer& out, size_t header){
    printf("resp_header_assign()\n");
    size_t msg_size = resp_size(out, header);
    if(msg_size > k_max_msg){
        printf("Response is too big.");
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG, "Response is too big.");
        msg_size = resp_size(out, header);
    }

    uint32_t len = (uint32_t)msg_size;
    printf("msg_size = [%d]\n", len);
    memcpy(&out[header], &len, 4);
}

static bool try_one_request(Conn* conn){
    if(conn->incoming.size() < 4) return false;

    printf("in try_one_request\n");

    uint32_t total_len = 0;
    memcpy(&total_len, conn->incoming.data(), 4);
    if(total_len > k_max_msg){
        msg("msg too long");
        conn->want_close = true;
        return false;
    }

    if(4 + total_len > conn->incoming.size()) return false;

    const uint8_t* request = &conn->incoming[4];

    log_payload(request, total_len);
    
    std::vector<std::string> cmd;
    if(parse_req(request, total_len, cmd) < 0){
        msg("bad request");
        conn->want_close = true;
        return false;
    }

    printf("parsed cmd: ");
    for (auto& s : cmd) {
        printf("[%s] ", s.c_str());
    }
    printf("\n");

    size_t header_pos = 0;
    resp_header_alloc(conn->outgoing, &header_pos);
    do_request(cmd, conn->outgoing);
    resp_header_assign(conn->outgoing, header_pos);

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

static void set_write(int epfd, Conn* conn){
    if(!conn->want_write && conn->outgoing.size() > 0){
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
        ev.data.fd = conn->fd;

        if(epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0){
            die("epoll_ctl MOD");
        }

        conn->want_write = true;
    }

    else if(conn->want_write && conn->outgoing.size() == 0){
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.fd = conn->fd;

        if(epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0){
            die("epoll_ctl MOD");
        }

        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_write(Conn* conn, int epfd){
    assert(conn->outgoing.size() > 0);
    
    while(conn->outgoing.size() > 0){
        ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
        if(rv > 0) buf_consume(conn->outgoing, (size_t)rv);

        else {
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;

            conn->want_close = true;
            return;
        }

    }
    set_write(epfd, conn);

}

static void handle_read(Conn* conn, int epfd){
    uint8_t buf[64 * 1024];
    while(true){
        ssize_t rv = read(conn->fd, buf, sizeof(buf));

        if(rv > 0){
            buf_append(conn->incoming, buf, (size_t)rv);

            while(!conn->want_close && try_one_request(conn)){}
        }

        else if(rv == 0){
            if(conn->incoming.size() == 0) msg("client closed");
            else msg("Unexpected EOF");
            conn->want_close = true;
            return;
        }

        else {
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            conn->want_close = true;
            return;
        }

    }

    if(conn->outgoing.size() > 0){
            set_write(epfd, conn);
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

    fd_set_nb(server_fd);

    LOG_INFO("server running on port %d", PORT);

    int epoll_fd = epoll_create1(0);

    if(epoll_fd < 0){
        die("epoll_create1()");
    }

    epoll_event sev{};
    sev.events = EPOLLIN | EPOLLERR;
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
                cev.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
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

                if(e & EPOLLIN){
                    handle_read(fd2conn[fd], epoll_fd);
                }

                if(e & EPOLLOUT){
                    handle_write(fd2conn[fd], epoll_fd);
                }

                if((e & (EPOLLERR | EPOLLHUP)) || fd2conn[fd]->want_close){
                    Conn* conn = fd2conn[fd];
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr) < 0) die("epoll_ctl");
                    close(conn->fd);
                    fd2conn[fd] = nullptr;
                    delete conn;
                }
            }
        }
        
    }
}