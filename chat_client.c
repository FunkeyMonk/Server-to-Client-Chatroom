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

//New thread function to send messages from standard input to server
//Reads the input and and sends to server until done
static void *send_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char line[BUF_SZ];

    //continues reading while so long as running flag is set
    while (running && fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) continue; //skip empty lines

        //to check if user types "exit" command
        if (strcmp(line, "exit\n") == 0 ||
            strcmp(line, "exit\r\n") == 0 ||
            strcmp(line, "exit") == 0) {

            fprintf(stderr, "[client] Exiting and disconnecting from server.\n");
            running = 0; //tells all threads to stop if so

            shutdown(fd, SHUT_RDWR); //shutsdown connection
            break;
        }

        //Sends whole message to server, works with partial sends
        size_t off = 0;
        while (off < len && running) {
            //returns number of bytes sent, -1 on error
            ssize_t n = send(fd, line + off, len - off, 0);
            if (n <= 0) {
                //If interupted, retry send
                if (n < 0 && errno == EINTR) continue;
                fprintf(stderr, "\n[client] Error sending, closing.\n");
                running = 0;
                break;
            }
            off += (size_t)n; 
        }
    }

    //Tells all threads to stop
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

    //Now prompts user for a username
    char username[64];
    printf("Enter username: ");
    fflush(stdout);
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "No username entered.\n");
        close(fd);
        return 1;
    }
    //Gets rid of trailing newline from usermname
    //fgets includes newline
    size_t ulen = strlen(username);
    if (ulen > 0 && username[ulen - 1] == '\n') {
        username[ulen - 1] = '\0';
        ulen--;
    }
    
    //Send username to server with newline delimiter
    {
        char uname_line[70];
        //returns number of characters written, negative on error
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

    //Create recieve thread for incoming server messages
    pthread_t recv_th, send_th;
    if (pthread_create(&recv_th, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        perror("pthread_create recv");
        close(fd);
        return 1;
    }
    //Create send thread to handle released user messages
    if (pthread_create(&send_th, NULL, send_thread, (void *)(intptr_t)fd) != 0) {
        perror("pthread_create send");
        running = 0;
        pthread_join(recv_th, NULL); //waits for recieve thread to finish
        close(fd);
        return 1;
    }

    //Waits for recieve thread to complete
    pthread_join(recv_th, NULL);
    //Signals send thread to stop and cancel
    running = 0;
    pthread_cancel(send_th); //forces send thread termination
    pthread_join(send_th, NULL); //waits for send thread to cleanup

    //Now close socket and cleanup
    close(fd);
    return 0;
}
