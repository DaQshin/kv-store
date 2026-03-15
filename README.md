# DKVS — Single-Node Key-Value Store

A single-node, in-memory key-value store written in C++ with a non-blocking TCP server built on `epoll`. Supports `GET`, `SET`, and `DEL` over a lightweight length-prefixed binary protocol.

---

## Architecture

```
Client
  │
  │  TCP (length-prefixed binary protocol)
  ▼
┌─────────────────────────────────────┐
│          epoll event loop           │
│                                     │
│  ┌──────────┐     ┌──────────────┐  │
│  │  accept  │     │  Conn state  │  │
│  │ handler  │     │  (per fd)    │  │
│  └──────────┘     └──────┬───────┘  │
│                          │          │
│              ┌───────────┴──────┐   │
│              │                  │   │
│         handle_read      handle_write│
│              │                  │   │
│         parse_req        buf_append  │
│              │                       │
│         do_request                   │
│              │                       │
│    ┌─────────▼──────────┐            │
│    │  std::map<str,str> │            │
│    │   (in-memory KV)   │            │
│    └────────────────────┘            │
└─────────────────────────────────────┘
```

Each client connection is tracked as a `Conn` object. The event loop uses edge-triggered `epoll` (`EPOLLET`) to drive reads and writes without blocking.

---

## Wire Protocol

All integers are little-endian. A request frame looks like:

```
┌──────────────┬──────────────┬──────────────────────────────┐
│  total_len   │    nstr      │  [len₀ | str₀] [len₁ | str₁] │
│   4 bytes    │   4 bytes    │        variable               │
└──────────────┴──────────────┴──────────────────────────────┘
```

- `total_len` — byte length of everything after the 4-byte header
- `nstr` — number of string tokens in the command
- Each token is a 4-byte length prefix followed by raw bytes

A response frame:

```
┌──────────────┬──────────────┬──────────────────┐
│   res_len    │   status     │     value         │
│   4 bytes    │   4 bytes    │   variable        │
└──────────────┴──────────────┴──────────────────┘
```

| Status | Value     | Meaning                      |
| ------ | --------- | ---------------------------- |
| `0`    | `RES_OK`  | Command succeeded            |
| `1`    | `RES_ERR` | Unknown or malformed command |
| `2`    | `RES_NX`  | Key not found                |

Maximum message size: **32 MiB** (`32 << 20`). Connections exceeding this are closed.

---

## Commands

| Command | Syntax              | Response                             |
| ------- | ------------------- | ------------------------------------ |
| `GET`   | `GET <key>`         | Value string, or `RES_NX` if missing |
| `SET`   | `SET <key> <value>` | `"Operation successful."`            |
| `DEL`   | `DEL <key>`         | `"Operation successful."`            |

---

## Connection Lifecycle

```
accept()
   │
   ▼
fd_set_nb()          ← set O_NONBLOCK
   │
   ▼
Conn { want_read=true }
   │
   ├── EPOLLIN  → handle_read()
   │                 ├── read() into incoming buffer
   │                 ├── try_one_request() (loop)
   │                 │     ├── parse_req()
   │                 │     └── do_request() → appends to outgoing
   │                 └── if outgoing non-empty → want_write=true
   │
   ├── EPOLLOUT → handle_write()
   │                 ├── write() from outgoing buffer
   │                 └── if outgoing drained → want_read=true
   │
   └── EPOLLERR/EPOLLHUP → close(fd), delete Conn
```

---

## Build

```bash
g++ -std=c++17 -O2 -o dkvs server.cpp -lpthread
./dkvs          # listens on port 5000
```

---

## Configuration

| Constant     | Default             | Description                                       |
| ------------ | ------------------- | ------------------------------------------------- |
| `PORT`       | `5000`              | TCP port the server binds to                      |
| `MAX_EVENTS` | `64`                | Max events per `epoll_wait` call                  |
| `k_max_msg`  | `33554432` (32 MiB) | Maximum request size before connection is dropped |

---

## File Structure

```
.
├── server.cpp          # Entire server: event loop, protocol, KV logic
└── logging/
    └── log.cpp         # Logging macros (LOG_INFO etc.)
```

---

## Known Limitations & Planned Work

- **In-memory only** — no persistence; all data is lost on restart
- `buf_append` and `buf_consume` use `std::vector::insert/erase` — O(n); a ring buffer would be more efficient
- `fd2conn` is a flat `vector` indexed by fd — sized lazily, but not bounded
- `std::map` for the KV store is O(log n); a hash map (`std::unordered_map`) would give O(1) average lookups
- No pipelining support — one request is processed to completion before the next
- No authentication or access control
- [ ] Persistence via append-only log / WAL
- [ ] Snapshotting for crash recovery
- [ ] `KEYS` / `EXISTS` / `TTL` commands
- [ ] Ring buffer for `incoming`/`outgoing`
