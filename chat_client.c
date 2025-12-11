#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//Buffer size, max bytes received/sent capable at once
#define BUF_SZ 1024

//to tell thread when done
static volatile int running = 1;

//Thread function receive messages from server and prints/logs them
//runs together with main thread
//arg is the socket file descriptor
static void *recv_thread(void *arg) {
    int fd = (int)(intptr_t)arg; //socket shared with main
    char buf[BUF_SZ]; //holds incoming data from server

    for (;;) {
        //Block until server sends data or closes connection
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        //n == 0: server closed; n < 0: error
        if (n <= 0) {
            //if error was interrupted system call, continue trying to receive
            if (n < 0 && errno == EINTR) continue; // If interrupted by signal, retry recv
            fprintf(stderr, "\n[client] Disconnected from server.\n");
            break;  //otherwise server closed or error
        }
         //Turn received bytes into a C string
        buf[n] = '\0';

        //print received message to stdout immediately so user sees it
        fputs(buf, stdout);
        //flush stdout to make message appear in real-time on screen
        fflush(stdout);
    }

    //sender thread now stops
    running = 0;
    //thread exits by returning NULL
    return NULL;
}


static void *send_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char line[BUF_SZ];

    while (running && fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) continue;

        if (strcmp(line, "exit\n") == 0 ||
            strcmp(line, "exit\r\n") == 0 ||
            strcmp(line, "exit") == 0) {

            fprintf(stderr, "[client] Exiting and disconnecting from server.\n");
            running = 0;

            shutdown(fd, SHUT_RDWR); 
            break;
        }

        size_t off = 0;
        while (off < len && running) {
            ssize_t n = send(fd, line + off, len - off, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                fprintf(stderr, "\n[client] Error sending, closing.\n");
                running = 0;
                break;
            }
            off += (size_t)n;
        }
    }

    running = 0;
    return NULL;
}

//Entry point: set up socket, connect to server, start threads, run send loop
int main(int argc, char *argv[]) {
    //need program name + server IP + port
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]); 
        return 1;
    }
    const char *server_ip = argv[1]; //Text IP like "127.0.0.1"
    int port = atoi(argv[2]); //convert port string to int
     
    //Check if port is in valid range (1-65535) 
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }
    
    //Create a TCP/IPv4 socket (SOCK_STREAM implies TCP) 
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        //socket creation fails
        perror("socket");
        return 1;
    }

    //Describe the remote server's address (IP + port + family)
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET; // IPv4
    saddr.sin_port = htons((unsigned short)port); // Host -> network byte order

    //convert the IP string into binary form in saddr.sin_addr
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        perror("inet_pton"); //Invalid IP or conversion error
        close(fd);
        return 1;
    }

    //Try to establish a TCP connection to the server
    //returns 0 on success, -1 on failure
    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        //connection failed
        perror("connect");
        close(fd);
        return 1;
    }

    printf("Connected to %s:%d\n", server_ip, port);

    char username[64];
    printf("Enter username: ");
    fflush(stdout);
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "No username entered.\n");
        close(fd);
        return 1;
    }
    size_t ulen = strlen(username);
    if (ulen > 0 && username[ulen - 1] == '\n') {
        username[ulen - 1] = '\0';
        ulen--;
    }
    {
        char uname_line[70];
        int m = snprintf(uname_line, sizeof(uname_line), "%s\n", username);
        if (m > 0) {
            ssize_t n = send(fd, uname_line, (size_t)m, 0);
            if (n <= 0) {
                perror("send username");
                close(fd);
                return 1;
            }
        }
    }

    printf("Type messages and press Enter.\n");
    printf("Type 'exit' to disconnect yourself from the server.\n");

    pthread_t recv_th, send_th;
    if (pthread_create(&recv_th, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        perror("pthread_create recv");
        close(fd);
        return 1;
    }
    if (pthread_create(&send_th, NULL, send_thread, (void *)(intptr_t)fd) != 0) {
        perror("pthread_create send");
        running = 0;
        pthread_join(recv_th, NULL);
        close(fd);
        return 1;
    }

    pthread_join(recv_th, NULL);
    
    running = 0;
    pthread_cancel(send_th); 
    pthread_join(send_th, NULL);

    close(fd);
    return 0;
}
