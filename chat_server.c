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

//port used for server, must be same as client argument to connect correctly
#define PORT        4267
//random number of max clients allowed
#define MAX_CLIENTS 8
//max size of the buffer used when taking in client data
#define BUF_SZ      1024

//client setup for max and current amt with mutex for protection of multiple threads
static int clients[MAX_CLIENTS];
static int client_count = 0;
//mutex protects clients and client_count from being altered by multiple threads at the same time, avoiding race conditions
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

//chat_history setup and mutex for same reason
//messages are added here after being sent
static FILE *history_fp = NULL;
//allows for only one thread at a time to write to history
static pthread_mutex_t history_mtx = PTHREAD_MUTEX_INITIALIZER;

//kill on error, prints what error occured and exits program
static void die(const char *what){
	perror(what);
	exit(EXIT_FAILURE);
}

//adding client
static void add_client_or_fail(int fd){
	//puts a lock so current thread is only thread with access to client_count and clients
    if(pthread_mutex_lock(&clients_mtx) != 0)
		//kill if fail
        die("mutex lock");
	//sets a max of 8 clients, 64 as a failsafe? can change later to 8
	//unlock mutex, print full server, close socket, and exit thread
	if(client_count >= 8){
		pthread_mutex_unlock(&clients_mtx);
		const char *msg = "Server full.\n";
		send(fd, msg, strlen(msg), 0);
		close(fd);
		pthread_exit(NULL);
	}
	//since there is space, place socket in array of current clients
	clients[client_count++] = fd;
	//unlock mutex to allow other threads to alter client_count and clients
	pthread_mutex_unlock(&clients_mtx);
}

//remove client from list
static void remove_client(int fd){
	//lock client mutex and return on fail
	if(pthread_mutex_lock(&clients_mtx) != 0)
		return;
	//find correct client in list, remove, and reduce client_count by 1
	for(int i = 0; i<client_count; i++){
		if(clients[i] == fd){
			clients[i] = clients[client_count - 1];
			client_count --;
			break;
		}
	}
	//unlock client mutex to allow other threads access
	pthread_mutex_unlock(&clients_mtx);
}

//copy locked list, then unlock, then send to avoid block
static void broadcast_to_all_except(int sender_fd, const char *buf, size_t len) {
	//holders for all clients and number of clients
    int snapshot[MAX_CLIENTS];
    int n;
	//lock client mutex, save current client_count and clients, then unlock before sending data
	//unlock before sending bc can take time, which can be used for other threads to do things
    if (pthread_mutex_lock(&clients_mtx) != 0) return;
    n = client_count;
    for (int i = 0; i < n; i++) snapshot[i] = clients[i];
    pthread_mutex_unlock(&clients_mtx);

	//sends message to each client
    for (int i = 0; i < n; i++) {
        int fd = snapshot[i];
		//skips sender as it shows in terminal already
        if (fd == sender_fd) continue;
		//in case we need to send message through multiple send calls
        size_t off = 0;
        while (off < len) {
            ssize_t w = send(fd, buf + off, len - off, 0);
			//for send error
            if (w <= 0) break;
			//to show how much of message was sent
            off += (size_t)w;
        }
    }
}

//add line to history
static void log_history_line(const char *line){
	//lock history mutex so only one thread writes to file at a time to avoid race condition
	//return if fail
	if(pthread_mutex_lock(&history_mtx) != 0)
		return;
	//open file in append mode if not open
	if(history_fp == NULL){
		history_fp = fopen("chat_history", "a");
		//if fail, unlock history mutex and return
		if(history_fp == NULL){
			pthread_mutex_unlock(&history_mtx);
			return;
		}
	}
	//add message to file, save to disk, and unlock history mutex
	fprintf(history_fp, "%s\n", line);
	fflush(history_fp);
	pthread_mutex_unlock(&history_mtx);
}

//holds socket file descriptor and IP + Port
struct client_info {
	int fd;
	struct sockaddr_in addr;
};

//create a thread for each connected client
static void *client_thread(void *arg) {
	//create client_info pointer and set it to socket file descriptor
    struct client_info *ci = (struct client_info *)arg;
    int fd = ci->fd;

	//holds the client IP and port
    char who[64];
    snprintf(who, sizeof(who), "%s:%d", inet_ntoa(ci->addr.sin_addr), ntohs(ci->addr.sin_port));
	//no need for pointer memory since we copied everything we need
    free(ci);

	//add current client to clients
    add_client_or_fail(fd);

    //hold code in brackets (block) to have like an isolated scope for variables
	//all variables here are destroyed after block is done running
    {
		//holds and creates line of who joined
        char line[128];
        int m = snprintf(line, sizeof(line), "[%s] joined", who);
		//if successful, send join message to all clients except joined client, then save to history
        if (m > 0) {
            broadcast_to_all_except(fd, line, (size_t)m);
            log_history_line(line);
        }
    }

	//buffer saves all incoming data from client
    char buf[BUF_SZ];
	
    for (;;) {
		//read data from client as much as possible
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        //close client connection
		if (n == 0) break;
		//during error, either continue or break out of loop (disconnect)
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

		//setup client's message holder w failsafe in case length is longer than max size
        char line[BUF_SZ + 80];
        size_t msg_len = (size_t)n;
        if (msg_len > BUF_SZ) msg_len = BUF_SZ;

        //start message with IP:Port
        int hdr = snprintf(line, sizeof(line), "[%s] ", who);
		//grab enough space to fit start, message, and ending (\n\0)
        size_t copy_len = sizeof(line) - (size_t)hdr - 2; 
		//if extra space just set to message length
        if (msg_len < copy_len) copy_len = msg_len;
		//copy client's message and place after starter
        memcpy(line + hdr, buf, copy_len);
        //bytes in starter and message
        size_t total = (size_t)hdr + copy_len;
		//add newline if missing
        if (total == 0 || line[total - 1] != '\n') line[total++] = '\n';
		//end string with terminator to indicate strict ending
        line[total] = '\0';

        //send message to all other clients and save to history
        broadcast_to_all_except(fd, line, total);
        log_history_line(line);
    }

    //block to send message when client leaves, similar to join block
    {
        char line[128];
        int m = snprintf(line, sizeof(line), "[%s] left\n", who);
        if (m > 0) {
            broadcast_to_all_except(fd, line, (size_t)m);
            log_history_line(line);
        }
    }

	//once left, remove client from list, close socket, and return NULL to end thread
    remove_client(fd);
    close(fd);
    return NULL;
}


int main(void) {
    //open history in append mode or return error if can't open
    history_fp = fopen("chat_history", "a");
    if (!history_fp) die("fopen chat_history");

    //create socket (idrk what this means but it was given)
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    //
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    //prep server address and set to 0, then specify necessary data for socket address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

	//bind socket to the address to keep it at the specific port and return error if fail
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
	//listen for potential connections, reject if full, return error if fail
    if (listen(s, MAX_CLIENTS) < 0) die("listen");

	//shows server is started
    printf("Chat server listening on %d\n", PORT);

    //infinitely wait for potential clients
    for (;;) {
		//hold any client address info
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
		//wait until client connects. then return new socket c for new client
        int c = accept(s, (struct sockaddr*)&caddr, &clen);
        //if fail, print message and loop back to start waiting state
		if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        //hold thread ID
        pthread_t th;
		//set up client info holder and if fail, close socket, return error, and skip client
        struct client_info *ci = (struct client_info *)malloc(sizeof(*ci));
        if (!ci) { 
			perror("malloc"); 
			close(c); 
			continue; 
		}
		//grab current new client info
        ci->fd = c;
        ci->addr = caddr;

		//create new thread for current new client
		//if fail, report error, close socket, and free mem of client and skip
        if (pthread_create(&th, NULL, client_thread, ci) != 0) {
            perror("pthread_create");
            close(c);
            free(ci);
            continue;
        }
		//makes sure no joining thread after this and cleans up once exited
        pthread_detach(th);
    }

    //for clean shut down but not set up yet so useless for now
    fclose(history_fp);
    close(s);
    return 0;
}
