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
             break;  //otherwise server closed or error
        }
         //Turn received bytes into a C string
        buf[n] = '\0';

        //print received message to stdout immediately so user sees it
        fputs(buf, stdout);
        //flush stdout to make message appear in real-time on screen
        fflush(stdout);
    }
    
    fprintf(stderr, "\n[client] Disconnected from server.\n");
    //thread exits by returning NULL
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
    printf("Type messages and press Enter. Ctrl+D to quit.\n");

    //Spawn receiver thread so receiving does not block sending from main thread
    pthread_t th;
    if (pthread_create(&th, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        //thread creation fails
        perror("pthread_create");
        close(fd);
        return 1;
    }

    //Main thread: read from stdin and send to server
    char line[BUF_SZ];
    //loop until user presses Ctrl+D (EOF) or error 
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) continue; //ignore completely empty lines

        //loop handles partial sends until all len bytes are written
        size_t off = 0;
        while (off < len) {
            //n == 0 or n < 0: problem with the connection or call
            ssize_t n = send(fd, line + off, len - off, 0);
            if (n <= 0) {
                //retry if a signal interrupted send
                if (n < 0 && errno == EINTR) continue;
                //on actual error, break out and clean up
                //Tell the server this side is done sending and receiving
                shutdown(fd, SHUT_RDWR);
                //close the socket file descriptor
                close(fd);
                //pthread_join blocks until recv_thread returns
                //wait for the receiver thread to finish before exiting process
                pthread_join(th, NULL);
                return 0;  // connection broken
            }
            //increment offset by number of bytes successfully sent
            off += (size_t)n;
        }
    }

    //stdin closed (user pressed Ctrl+D)
    printf("\n[client] Input closed, shutting down.\n");

    //Tell the server this side is done sending and receiving
    shutdown(fd, SHUT_RDWR);
    //close the socket file descriptor
    close(fd);
    //pthread_join blocks until recv_thread returns
    //wait for the receiver thread to finish before exiting process
    pthread_join(th, NULL);
    return 0;  // connection broken
}
