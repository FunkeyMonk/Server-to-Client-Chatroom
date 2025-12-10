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

//buffer size, max bytes received/sent capable at once
#define BUF_SZ 1024

//thread function that receives messages from server and prints/logs them
//runs concurrently with main thread, allowing simultaneous send and receive
//arg is the socket file descriptor cast as (intptr_t) 
static void *recv_thread(void *arg) {
     //extract socket file descriptor from void pointer argument
    int fd = (int)(intptr_t)arg;
    //buffer to hold incoming data from server
    char buf[BUF_SZ];

    //infinite loop to continuously receive messages
    for (;;) {
        //recv() blocks until data arrives or connection closes
        //reads up to BUF_SZ-1 bytes to leave room for null terminator
        //returns: >0 = bytes received, 0 = connection closed, <0 = error
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        //if recv returns <=0, connection is closed or error occurred
        if (n <= 0) {
            //if error was interrupted system call, continue trying to receive
            if (n < 0 && errno == EINTR) continue;
            //otherwise break loop (connection closed or fatal error)
            break;  // server closed or error
        }
        //null-terminate the received data to make it a proper string
        //critical for safe printing and string operations
        buf[n] = '\0';

        //print received message to stdout immediately so user sees it
        fputs(buf, stdout);
        //flush stdout to ensure message appears on screen right away
        fflush(stdout);
    }
    
    //inform user that connection to server has been closed/lost
    fprintf(stderr, "\n[client] Disconnected from server.\n");
    //thread exits by returning NULL
    return NULL;
}

int main(int argc, char *argv[]) {
    //validate command line arguments: need server IP and port number
    //usage: ./chat_client <server_ip> <port>
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    //extract server IP address from first argument
    const char *server_ip = argv[1];
    //convert port number string to integer
    int port = atoi(argv[2]);
     //validate port is in valid range (1-65535)
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    //ignore SIGPIPE signal
    //SIGPIPE is sent when trying to write to closed socket, would kill program
    //by ignoring it, program can handle error gracefully instead
    signal(SIGPIPE, SIG_IGN);
    
    //create TCP socket for communication with server
    //AF_INET = IPv4 addressing, SOCK_STREAM = TCP protocol (reliable/ordered)
    //third argument 0 = default protocol for SOCK_STREAM (TCP)   
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        //socket creation failed
        perror("socket");
        return 1;
    }

    //prepare server address structure for connection
    struct sockaddr_in saddr;
    //zero out the structure before filling it in
    memset(&saddr, 0, sizeof(saddr));
    //set address family to IPv4
    saddr.sin_family = AF_INET;
    //convert port from host byte order to network byte order (big-endian)
    //htons = host to network short
    saddr.sin_port = htons((unsigned short)port);

    //parse IP address string into binary format
    //inet_pton converts string "127.0.0.1" to actual IP binary value
    //returns: 1=success, 0=invalid format, -1=error
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        //IP address parsing failed
        perror("inet_pton");
        close(fd);
        return 1;
    }

    //attempt to connect to server at specified address and port
    //this performs TCP three-way handshake (SYN, SYN-ACK, ACK)
    //returns 0 on success, -1 on failure
    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        //connection failed
        perror("connect");
        close(fd);
        return 1;
    }

    //connection successful, inform user
    printf("Connected to %s:%d\n", server_ip, port);
    printf("Type messages and press Enter. Ctrl+D to quit.\n");

    //create receive thread to listen for incoming messages from server
    //this allows main thread to send while recv_thread receives simultaneously
    //without threads, program would block on either send or receive, not both    pthread_t th;
    if (pthread_create(&th, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        //thread creation failed
        perror("pthread_create");
        close(fd);
        return 1;
    }

    //main thread: read from stdin and send to server
    //buffer to hold one line of user input
    char line[BUF_SZ];
    //loop continues until user presses Ctrl+D (EOF) or error occurs
    while (fgets(line, sizeof(line), stdin) != NULL) {
        //get length of input line
        size_t len = strlen(line);
        //skip empty lines
        if (len == 0) continue;

        //send data to server, handling partial sends
        //send() may not send all data in one call, especially for larger messages
        //so we track offset and keep sending until all bytes are sent
        size_t off = 0;
        while (off < len) {
            //attempt to send remaining data from current offset
            //returns: >0 = bytes sent, 0 = connection closed, <0 = error
            ssize_t n = send(fd, line + off, len - off, 0);
            if (n <= 0) {
                //send failed or connection closed
                //if interrupted system call, retry
                if (n < 0 && errno == EINTR) continue;
                //otherwise connection broken, jump to cleanup
                goto out;  // connection broken
            }
            //increment offset by number of bytes successfully sent
            off += (size_t)n;
        }
    }

    //stdin closed (user pressed Ctrl+D)
    printf("\n[client] Input closed, shutting down.\n");

out:
    //graceful shutdown: disable both send and receive on socket
    //SHUT_RDWR = shutdown both reading and writing
    //this signals to server that client is closing connection
    //and wakes up recv_thread if it's blocked in recv()
    shutdown(fd, SHUT_RDWR);
    //close the socket file descriptor
    close(fd);
    //wait for receive thread to finish
    //pthread_join blocks until recv_thread returns
    //ensures clean shutdown before cleanup
    pthread_join(th, NULL);

    //exit program successfully
    return 0;
}
