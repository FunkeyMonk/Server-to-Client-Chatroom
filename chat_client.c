#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SZ 1024

static FILE *history_fp = NULL;
static pthread_mutex_t history_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Log one line to chat_history.txt (thread-safe) */
static void log_history_line(const char *prefix, const char *msg) {
    if (!history_fp) return;
    if (pthread_mutex_lock(&history_mtx) != 0) return;

    fputs(prefix, history_fp);
    fputs(" ", history_fp);
    fputs(msg, history_fp);

    size_t len = strlen(msg);
    if (len == 0 || msg[len - 1] != '\n')
        fputc('\n', history_fp);

    fflush(history_fp);
    pthread_mutex_unlock(&history_mtx);
}

/* Thread that receives messages from server and prints/logs them */
static void *recv_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[BUF_SZ];

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;  // server closed or error
        }
        buf[n] = '\0';

        // print to screen
        fputs(buf, stdout);
        fflush(stdout);

        // log as [RECV]
        log_history_line("[RECV]", buf);
    }

    fprintf(stderr, "\n[client] Disconnected from server.\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    // Usage: ./chat_client <server_ip> <port>
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    // open client-side history file (optional)
    history_fp = fopen("chat_history.txt", "a");
    if (!history_fp) {
        perror("fopen chat_history.txt");
        return 1;
    }

    // create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        fclose(history_fp);
        return 1;
    }

    // build server address
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons((unsigned short)port);

    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        fclose(history_fp);
        return 1;
    }

    // connect to server
    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        close(fd);
        fclose(history_fp);
        return 1;
    }

    printf("Connected to %s:%d\n", server_ip, port);
    printf("Type messages and press Enter. Ctrl+D to quit.\n");

    // start receiver thread
    pthread_t th;
    if (pthread_create(&th, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        perror("pthread_create");
        close(fd);
        fclose(history_fp);
        return 1;
    }

    // main thread: read from stdin and send to server
    char line[BUF_SZ];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) continue;

        // send to server (handle partial sends)
        size_t off = 0;
        while (off < len) {
            ssize_t n = send(fd, line + off, len - off, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                goto out;  // connection broken
            }
            off += (size_t)n;
        }

        // log as [SENT]
        log_history_line("[SENT]", line);
    }

    // stdin closed (Ctrl+D)
    printf("\n[client] Input closed, shutting down.\n");

out:
    // stop the connection so recv thread exits
    shutdown(fd, SHUT_RDWR);
    close(fd);
    pthread_join(th, NULL);

    fclose(history_fp);
    return 0;
}
