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
#include "../include/log.h"
#define PORT 5000

const size_t k_max_msg = 4096;

static void msg(const char* msg){
    fprintf(stderr, "%s\n", msg);
}

static void die(const char* msg){
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_INT = 2,
    TAG_STR = 3,
    TAG_DBL = 4,
    TAG_ARRAY = 5,
};

static int32_t print_response(const uint8_t* data, size_t size){
    if(size < 1){
        msg("bad response");
        return -1;
    }

    switch(data[0]){
        case TAG_NIL:
            printf("(nil)\n");
            return 1;

        case TAG_ERR:
            {
                if(size < 1 + 8){
                    msg("bad response");
                    return -1;
                }
            }
            {
                int32_t code = 0;
                uint32_t len = 0;
                memcpy(&code, &data[1], 4);
                memcpy(&len, &data[1 + 4], 4);
                if(size < 1 + 8 + len){
                    msg("bad response");
                    return -1;
                }
                printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
                return 1 + 8 + len;
            }
        
        case TAG_STR:
            {
                if(size < 1 + 4){
                    msg("bad response");
                    return -1;
                }
            }
            {
                uint32_t len = 0;
                memcpy(&len, &data[1], 4);
                if(size < 1 + 4 + len){
                    msg("bad response");
                    return -1;
                }

                printf("(str) %d %.*s \n", len, len, &data[1 + 4]);
                return 1 + 4 + len;
            }

        case TAG_INT:
            {
                if(size < 1 + 8){
                    msg("bad response");
                    return -1;
                }
            }
            {
                int64_t val = 0;
                memcpy(&val, &data[1], 8);
                printf("(int) %ld\n", val);
                return 1 + 8;
            }

        case TAG_DBL:
            {
                if(size < 1 + 8){
                    msg("bad response");
                    return -1;
                }
            }
            {
                double val = 0;
                memcpy(&val, &data[1], 8);
                printf("(double) %g\n", val);
                return 1 + 8;
            }

        case TAG_ARRAY:
            {
                if(size < 1 + 4){
                    msg("bad response");
                    return -1;
                }
            }
            {
                uint32_t len = 0;
                memcpy(&len, &data[1], 4);
                printf("(arr) len=%d\n", len);
                size_t arr_offset = 1 + 4;
                for(uint32_t i = 0; i < len; i++){
                    int32_t rv = print_response((const uint8_t*)& data[arr_offset], size - arr_offset);
                    if(rv < 0){
                        return rv;
                    }
                    arr_offset += (size_t)rv;
                }
                printf("(arr) end\n");
                return (int32_t)arr_offset;
            }
        
        default:
            msg("bad response");
            return -1;
    };
}

static int32_t read_full(int fd, uint8_t* read_buffer, size_t n){
    while(n > 0){
        ssize_t rv = read(fd, read_buffer, n);
        LOG_DEBUG("Bytes read: %zd", rv);
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
        LOG_DEBUG("bytes written: %zd", rv);
        if(rv <= 0){
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        write_buffer += rv;
    }

    return 0;
}

static int send_req(int fd, std::vector<std::string>& cmd){
    uint32_t len = 4;
    for(const std::string& s: cmd){
        len += 4 + s.size();
    }

    if(len > k_max_msg) return -1;

    uint8_t wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);
    uint32_t nstr = cmd.size();
    memcpy(&wbuf[4], &nstr, 4);
    uint32_t cur = 8;
    for(const std::string& s: cmd){
        uint32_t n = (uint32_t)s.size();
        memcpy(&wbuf[cur], &n, 4);
        memcpy(&wbuf[cur + 4], s.data(), n);
        cur += 4 + n;
    }

    return write_all(fd, wbuf, 4 + len);
}

static int32_t read_res(int fd){
    uint8_t rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);
    if(err){
        if(errno == 0){
            msg("EOF");
        }
        else msg("read() error");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, &rbuf[0], 4);
    if(len > k_max_msg){
        msg("msg too long");
        return -1;
    }

    err = read_full(fd, &rbuf[4], len);
    if(err){
        msg("read() error");
        return err;
    }

    int32_t rv = print_response((const uint8_t*)&rbuf[4], len);
    if(rv > 0 && (uint32_t)rv != len){
        msg("bad response");
        rv = -1;
    }

    return rv;

}

// static int32_t read_res(int fd){
//     uint8_t rbuf[4 + k_max_msg];
//     errno = 0;
//     int32_t err = read_full(fd, rbuf, 4);
//     if(err){
//         if(errno == 0){
//             msg("EOF");
//         }
//         else msg("read()");
//         return err;
//     }

//     uint32_t len = 0;
//     memcpy(&len, rbuf, 4);
//     if(len > k_max_msg){
//         msg("too long");
//         return -1;
//     }

//     err = read_full(fd, &rbuf[4], 4);
//     if(err){
//         msg("read()");
//         return -1;
//     }

//     uint32_t status_code = 0;
//     if(len < 4){
//         msg("bad response");
//         return -1;
//     }
//     memcpy(&status_code, &rbuf[4], 4);

//     err = read_full(fd, &rbuf[8], len - 4);
//     if(err){
//         msg("read()");
//         return -1;
//     }

//     LOG_INFO("server says: [%u]  %.*s\n", status_code, len - 4, &rbuf[8]);
//     return 0;
// }

int main(int argc, char** argv){
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0){
        die("socket()");
    }

    sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &client_addr.sin_addr);

    if(connect(fd, (sockaddr*)& client_addr, sizeof(client_addr)) < 0){
        die("connect()");
    }

    std::vector<std::string> commands;
    for(int i = 1; i < argc; i++){
        if(std::string(argv[i]) == "GET"){
            commands.push_back("GET");
            commands.push_back(argv[++i]);
        }
        else if(std::string(argv[i]) == "SET"){
            commands.push_back("SET");
            commands.push_back(argv[++i]);
            if(i + 1 >= argc){
                msg("Not Enough Values.");
                exit(1);
            }
            commands.push_back(argv[++i]);
        }
        else if(std::string(argv[i]) == "DEL"){
            commands.push_back("DEL");
            commands.push_back(argv[++i]);
        }
        else if(std::string(argv[i]) == "FLUSH"){
            commands.push_back("FLUSH");
        }
    }

    int32_t err = send_req(fd, commands);
    if(err) goto L_DONE;

    err = read_res(fd);
    if(err) goto L_DONE;

    L_DONE:
        close(fd);
        return 0;
}