# Concurrent Web Server

A concurrent HTTP/1.0 static file server written from scratch in C, built directly on POSIX sockets, processes, and signals. Each incoming connection is handled by its own child process, and terminated children are reaped asynchronously so no zombie processes accumulate.

The goal of the project is to show how core operating-system primitives — `socket`, `fork`, `waitpid`, `sigaction`, `open`, `read`, and `write` — combine to form a working piece of network infrastructure, without relying on any third-party libraries.

## Features

| Feature | Description |
|---|---|
| **Process-based concurrency** | Every connection is served by a child created with `fork()`, isolating requests from one another. |
| **Zombie-free cleanup** | A `SIGCHLD` handler reaps finished children with non-blocking `waitpid(-1, NULL, WNOHANG)`. |
| **Static file serving** | Serves files from a user-specified directory; `/` and paths ending in `/` resolve to `index.html`. |
| **MIME type detection** | Sets `Content-Type` from the file extension (HTML, CSS, JS, JPEG, PNG, GIF, plain text), defaulting to `application/octet-stream`. |
| **Path traversal protection** | Rejects any URL containing `..` before touching the filesystem. |
| **HTTP error responses** | `404 Not Found` for missing or non-regular files, `405 Method Not Allowed` for methods other than `GET`. |
| **Robust I/O** | Full-length reads and writes that handle partial transfers and interrupted system calls. |
| **Fast restarts** | `SO_REUSEADDR` allows the server to rebind its port immediately after shutdown. |

## Project Structure

```
.
├── server.c        # Server implementation
├── www/
│   └── index.html  # Default page served at /
└── Report          # Project report (PDF)
```

## Getting Started

### Requirements

- A POSIX-compliant system (Linux or macOS)
- A C compiler such as `gcc` or `clang`

### Build

```bash
gcc -Wall -Wextra -o server server.c
```

### Run

```bash
./server <port> <directory>
```

Example:

```bash
./server 8080 ./www
```

Then open <http://localhost:8080> in a browser.

## Usage Examples

```bash
# Fetch the default page
curl http://localhost:8080/

# Inspect response headers
curl -v http://localhost:8080/index.html -o /dev/null

# Missing file -> 404 Not Found
curl -i http://localhost:8080/missing.html

# Path traversal attempt -> 404 Not Found
curl -i --path-as-is http://localhost:8080/../etc/passwd

# Unsupported method -> 405 Method Not Allowed
curl -i -X POST http://localhost:8080/

# Concurrent requests, each handled by its own process
for i in 1 2 3; do curl -s http://localhost:8080/ -o /dev/null & done; wait
```

## How It Works

### Request lifecycle

1. The parent process blocks in `accept()` waiting for a client.
2. On a new connection, it calls `fork()`. The child takes ownership of the client socket; the parent closes its copy and returns to `accept()`.
3. The child reads the request line and extracts the method, URL, and protocol version.
4. The request is validated: only `GET` is allowed, and URLs containing `..` are rejected.
5. The URL is mapped to a path inside the served directory, and the file is opened and checked with `fstat()`.
6. The child sends an `HTTP/1.0` response with `Content-Type`, `Content-Length`, and `Connection: close` headers, followed by the file contents, then exits.
7. The parent's `SIGCHLD` handler reaps the terminated child.

### Accept loop

```c
while (1) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) continue;

    pid_t pid = fork();
    if (pid == 0) {
        close(server_fd);
        handle_request(client_fd);
    } else {
        if (pid < 0) perror("fork");
        close(client_fd);
    }
}
```

### Zombie reaping

```c
void reap_children(int sig) {
    (void) sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}
```

The handler is installed with `sigaction()` using `SA_RESTART`, so `accept()` resumes automatically after a child is reaped, and it preserves `errno` so it never interferes with the code it interrupts.

## Design Trade-offs

This server uses one process per connection. The table below compares it with the common alternatives.

| Model | Isolation | Per-connection overhead | Notes |
|---|---|---|---|
| **`fork()` (this project)** | Strong: separate address space per request | Highest: new process, page tables, and context switches | A crash in one request cannot corrupt another. |
| `pthread_create()` | Weak: shared address space | Lower | Shared state requires synchronization. |
| Thread pool | Weak: shared address space | Lowest (amortized) | Typical of production servers; most complex to implement correctly. |

Because each child is an ordinary process, the OS scheduler preempts it like any other task, so a slow request never blocks the others. The cost is memory and creation overhead: *N* concurrent connections mean *N* processes.

## Limitations

The server is intentionally minimal and is not designed for production use:

- Only `GET` requests are supported.
- Query strings and percent-encoded URLs are not decoded.
- Files are read fully into memory before being sent.
- Each connection serves a single request (no keep-alive).
- The request line must fit within the first 4 KB read from the socket.

## Concepts Demonstrated

| Concept | Where |
|---|---|
| TCP server socket lifecycle | `socket` / `bind` / `listen` / `accept` in `main()` |
| Process-based concurrency | `fork()` accept loop |
| Asynchronous signal handling | `SIGCHLD` handler `reap_children()` |
| HTTP/1.0 request parsing | `handle_request()` |
| Input sanitization | `..` traversal check |
| File I/O via system calls | `open` / `fstat` / `read` / `write` |

## Acknowledgements

Developed as Mini-Project 4 for **CSAI 204: Operating Systems**, Spring 2026. See [`Report`](Report) for the full project report.
