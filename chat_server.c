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

//client setup for max and current amt with mutex for protection of multiple threads
static int clients[8];
static int client_count = 0;
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

//chat_history setup and mutex for same reason
static FILE *history_fp = NULL;
static pthread_mutex_t history_mtx = PTHREAD_MUTEX_INITIALIZER;

//kill on error
static void die(const char *what){
	perror(what);
	exit(EXIT_FAILURE);
}

//adding client
static void add_client_or_fail(int fd){
	if(pthread_mutex_lock(&client_mtxc) != 0)
		die("mutex lock");
	if(client_count >= 8){
		pthread_mutex_unlock(&clients_mtx);
		const char *msg = "Server full.\n";
		send(fd, msg, strlen(msg), 0);
		close(fd);
		pthread_exit(NULL);
	}
	clients[client_count++] = fd;
	pthread_mutex_unlock(&clients_mtx);
}

//remove client
static void remove_client(int fd){
	if(pthread_mutex_lock(&clients_mtx) != 0)
		return;
	for(int i = 0; i<client_count; i++){
		if(clients[i] == fd){
			clients[i] = clients[client_count - 1];
			client_count --;
			break;
		}
	}
	pthread_mutex_unlock(&clients_mtx);
}

//copy locked list, then unlock, then send to avoid block
static void broadcast_to_all_except(int sender_fd, const char *buf, size_t len) {
    int snapshot[MAX_CLIENTS];
    int n;

    if (pthread_mutex_lock(&clients_mtx) != 0) return;
    n = client_count;
    for (int i = 0; i < n; i++) snapshot[i] = clients[i];
    pthread_mutex_unlock(&clients_mtx);

    for (int i = 0; i < n; i++) {
        int fd = snapshot[i];
        if (fd == sender_fd) continue;
        size_t off = 0;
        while (off < len) {
            ssize_t w = send(fd, buf + off, len - off, 0);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
}

//add line to history
static void append_to_history(const char *line){
	if(pthread_mutex_lock(&history_mtx) != 0)
		return;
	if(history_fp == NULL){
		history_fp = fopen("chat_history.txt", "a");
		if(history_fp == NULL){
			pthread_mutex_unlock(&history_mtx);
			return;
		}
	}
	fprintf(history_fp, "%s\n", line);
	fflush(history_fp);
	pthread_mutex_unlock(&history_mtx);
}


struct client_info {
	int fd;
	struct sockaddr_in addr;
};

static void *client_thread(void *arg) {
    struct client_info *ci = (struct client_info *)arg;
    int fd = ci->fd;

    char who[64];
    snprintf(who, sizeof(who), "%s:%d",
             inet_ntoa(ci->addr.sin_addr), ntohs(ci->addr.sin_port));
    free(ci);

    add_client_or_fail(fd);

    // announce join (broadcast + log)
    {
        char line[128];
        int m = snprintf(line, sizeof(line), "[%s] joined\n", who);
        if (m > 0) {
            broadcast_to_all_except(fd, line, (size_t)m);
            log_history_line(line);
        }
    }

    char buf[BUF_SZ];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;           // orderly close
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                   // error
        }

        // ensure messages are newline-terminated for clean logging
        // (optional; the client may already send \n)
        // We’ll build a "who: message" line for history + broadcast
        char line[BUF_SZ + 80];
        // truncate safely if needed
        size_t msg_len = (size_t)n;
        if (msg_len > BUF_SZ) msg_len = BUF_SZ;

        // Build: [ip:port] message
        int hdr = snprintf(line, sizeof(line), "[%s] ", who);
        size_t copy_len = sizeof(line) - (size_t)hdr - 2; // leave room for \n\0
        if (msg_len < copy_len) copy_len = msg_len;
        memcpy(line + hdr, buf, copy_len);
        // ensure newline at end:
        size_t total = (size_t)hdr + copy_len;
        if (total == 0 || line[total - 1] != '\n') line[total++] = '\n';
        line[total] = '\0';

        // broadcast (exclude sender) and log
        broadcast_to_all_except(fd, line, total);
        log_history_line(line);
    }

    // announce leave
    {
        char line[128];
        int m = snprintf(line, sizeof(line), "[%s] left\n", who);
        if (m > 0) {
            broadcast_to_all_except(fd, line, (size_t)m);
            log_history_line(line);
        }
    }

    remove_client(fd);
    close(fd);
    return NULL;
}


int main(void) {
    signal(SIGPIPE, SIG_IGN); // don’t die on send() to closed socket

    // open history file in append mode (creates if not exists)
    history_fp = fopen("chat_history.txt", "a");
    if (!history_fp) die("fopen chat_history.txt");

    // create listening socket
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    // allow quick restart
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // bind to 0.0.0.0:PORT
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(s, MAX_CLIENTS) < 0) die("listen");

    printf("Chat server listening on %d\n", PORT);

    // accept loop
    for (;;) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int c = accept(s, (struct sockaddr*)&caddr, &clen);
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // create one thread per client
        pthread_t th;
        struct client_info *ci = (struct client_info *)malloc(sizeof(*ci));
        if (!ci) { perror("malloc"); close(c); continue; }
        ci->fd = c;
        ci->addr = caddr;

        if (pthread_create(&th, NULL, client_thread, ci) != 0) {
            perror("pthread_create");
            close(c);
            free(ci);
            continue;
        }
        pthread_detach(th); // auto-clean thread resources
    }

    // (unreachable in this minimal server)
    fclose(history_fp);
    close(s);
    return 0;
}