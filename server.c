#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 4096

char *serve_dir;

void reap_children(int sig) {
    (void) sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

const char *get_mime_type(const char *filepath) {
    const char *ext = strrchr(filepath, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".txt") == 0) return "text/plain";

    return "application/octet-stream";
}

void send_response(int client_fd, int status_code, const char *status_text,
                    const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);

    if (write_all(client_fd, header, header_len) < 0) return;
    write_all(client_fd, body, body_len);
}

void send_404(int client_fd) {
    const char *body = "<html><body><h1>404 Not Found</h1></body></html>";
    send_response(client_fd, 404, "Not Found", "text/html", body, strlen(body));
}

void send_405(int client_fd) {
    const char *body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
    send_response(client_fd, 405, "Method Not Allowed", "text/html", body, strlen(body));
}

void handle_request(int client_fd) {
    char buf[BUF_SIZE];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(client_fd);
        exit(0);
    }
    buf[n] = '\0';

    char method[8], url[1024], proto[16];
    if (sscanf(buf, "%7s %1023s %15s", method, url, proto) != 3) {
        send_404(client_fd);
        close(client_fd);
        exit(0);
    }

    if (strcmp(method, "GET") != 0) {
        send_405(client_fd);
        close(client_fd);
        exit(0);
    }

    if (strstr(url, "..") != NULL) {
        send_404(client_fd);
        close(client_fd);
        exit(0);
    }

    char filepath[2048];
    if (strcmp(url, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", serve_dir);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", serve_dir, url);
        size_t len = strlen(filepath);
        if (len > 0 && filepath[len - 1] == '/') {
            strncat(filepath, "index.html", sizeof(filepath) - len - 1);
        }
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        send_404(client_fd);
        close(client_fd);
        exit(0);
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        send_404(client_fd);
        close(client_fd);
        exit(0);
    }
    size_t file_size = st.st_size;

    char *file_buf = malloc(file_size > 0 ? file_size : 1);
    if (!file_buf) {
        close(fd);
        close(client_fd);
        exit(1);
    }

    size_t total = 0;
    while (total < file_size) {
        ssize_t n = read(fd, file_buf + total, file_size - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    if (total != file_size) {
        free(file_buf);
        send_404(client_fd);
        close(client_fd);
        exit(0);
    }

    send_response(client_fd, 200, "OK", get_mime_type(filepath), file_buf, file_size);

    free(file_buf);
    close(client_fd);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <directory>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }
    serve_dir = argv[2];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("Server listening on port %d, serving: %s\n", port, serve_dir);
    fflush(stdout);

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

    return 0;
}
