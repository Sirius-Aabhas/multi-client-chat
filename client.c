/*non-blocking TCP chat client*/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MSG_SIZE 256

int sockfd;

/* Function to handle abrupt termination after ctrl+c */
void sigint_handler(int sig) {
	write(sockfd, "/quit\n", 5);
	fprintf(stderr, "Keyboard interrupt ctrl + c\nQuitting...");
    exit(EXIT_FAILURE);
}

/* Function to send and receive message to and from the server */
void interact_with_server(int sockfd) {
	size_t m;
	char buf[50];
	recv(sockfd, buf, 50, 0);
	fflush(stdout);
	int child_pid = fork();
	if(child_pid == 0) {		//child process
		//read from terminal and send to server
		char *terminal_buffer = (char*)malloc(MSG_SIZE);
		while(1) {
			int len_recv = getline(&terminal_buffer, &m, stdin);
			printf("--  %d --  \n", len_recv);
			if(len_recv > 0) {
				send(sockfd, terminal_buffer, strlen(terminal_buffer), 0);
				if(!strncmp(terminal_buffer, "EXIT", 4)) {
					exit(EXIT_SUCCESS);
				}
			}
		}
	}else{		//parent process
		//to read messages from the server and display on terminal
		char *server_buffer = (char*)malloc(MSG_SIZE);
		while(1) {
			bzero(server_buffer,MSG_SIZE-1);
			int len_recv = recv(sockfd, server_buffer, MSG_SIZE, MSG_DONTWAIT);
			if(len_recv > 0) {
				if(!strncmp(server_buffer, "[FATAL]", 7) ||		//abrupt closure
					 !strncmp(server_buffer, "BYE", 3)) {		//processed closure
					kill(child_pid,SIGKILL);
					break;
				}
				printf("Incoming -- %s\n", server_buffer);
			}
			while(waitpid(-1, NULL, WNOHANG) > 0);
		}
	}
}

/* First command line argument specifies the port no */
int main(int argc, char *argv[]) {
	signal(SIGINT, sigint_handler);		//special handling of Ctrl+C
	signal(SIGTSTP, SIG_IGN);	//ignoring of Ctrl+Z
	if(argc != 2) {
		printf("Please pass the port no. as the command line argument.\n");
		exit(0);
	}
	int portno=atoi(argv[1]);
	sockfd = socket(AF_INET, SOCK_STREAM, 0);	//TCP Internet socket

	//finding server by name
	struct hostent *server;
	server = gethostbyname("localhost");
	
	if(server == NULL) {
		fprintf(stderr, "The server does not exist\n");
		exit(0);
	}

	//initializing server address
	struct sockaddr_in srvr_addr;
	bzero((char *) &srvr_addr, sizeof(srvr_addr));
	srvr_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, (char *)&srvr_addr.sin_addr.s_addr, server->h_length);
	srvr_addr.sin_port = htons(portno);

	//connect to server
	if (connect(sockfd,(struct sockaddr *) &srvr_addr,sizeof(srvr_addr)) < 0) { 
		perror("ERROR connecting");
		exit(0);
	}
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
	//this function asks queries to server
	interact_with_server(sockfd);
	//closing the socket
	close(sockfd);
}
