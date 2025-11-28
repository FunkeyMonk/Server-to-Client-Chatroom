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
static void broadcast_to_all
//CONTINUE FROM HERE



static void *client_thread(void *arg){
	int fd = (int)(intptr_t)arg;
	char buf[1000];

	//recieve all data from client socket
	for(1){
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		//client closed or error
		if(n<=0) 
			break;
		send(fd, buf, (size_t)n, 0);
	}
	close(fd);
	return NULL;
}


int main(){
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd<0)
		report("socket", 1);

	struct sockaddr_in saddr;
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr.sin_port = htons(PortNumber);

	if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
		report("bind", 1);

	if(listen(fd, MaxConnects) < 0)
		report("listen", 1);
	fprintf(stderr, "Listening on port %i for clients...\n", PortNumber);

	while(1){
		struct sockaddr_in caddr;
		int len = sizeof(caddr);
		int client_fd = accept(fd, (struct socaddr*) &caddr, $len);

		if (client_fd < 0){
			report("accept", 0);
			continue;
		}

		printf("Client Connected: %s:%d\n", inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

		pthread_t pth;
		if(pthread_create(&pth, NULL, client_thread, (void*)(intprt_t)c) != 0) {
			perror("pthread_create");
			close(c);
			continue;
		}

		pthread_detach(pth);
	}

	close(fd);
	return 0;


	ssize_t read(int fildes, void *buf. size_t nbyte, off_t offset);
	ssize_t write(int fildes, const void *buf, size_t nbyte);

}


