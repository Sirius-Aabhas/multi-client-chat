/*Multi-client TCP chat server using fork() */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <signal.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <errno.h>

#define DEFAULT_PORT 22222
#define MSG_SIZE 256
#define MAX_CLIENTS 5
#define MAX_MSGS 20
#define MAX_GRPS 10

#define P(s) semop(s, &pop, 1)  
#define V(s) semop(s, &vop, 1)

/*Structure to hold client fd and uuid mapping */
typedef struct _client {
    int fd, uuid;
} client;

/*Structure to store info about oending messages */
typedef struct _msgtable{
    int from, to, is_grp;   //fd only not uuid
    char msg[MSG_SIZE];
}msg_table_node;

/*Structure to store group information */
typedef struct _grptable{
    int grp_id, ownr_id, fresh;
    int members[MAX_CLIENTS];  //fd only not uuid
}grp_table_node;

/*Structure to store info for pending members in consent-based groups */
typedef struct _grprequests{
    int grp_id, ownr_id, all_informed;
    int members[MAX_CLIENTS];  //fd only not uuid
}grp_req_node;


//global variables fro server-wide access
int semid_msg_table, semid_list, semid_grp_table, semid_grp_req;
int shmid_cli, shmid_msg, shmid_grp, shmid_req;
client *client_list;
msg_table_node *msg_table;
grp_table_node *grp_table;
grp_req_node *grp_req;
struct sembuf pop, vop;
int connfd; //fd for client connections

/* Function to handle abrupt termination after ctrl+c for child process*/
void sigint_handler(int sig) {
    shmdt(client_list);
    shmdt(msg_table);
    shmdt(grp_table);
    shmdt(grp_req);
    write(connfd, "[FATAL] Server fault .. Terminating connection.", 50);
    printf("[INFO] Closed connection with client socket %d\n", connfd);
    close(connfd);
    exit(EXIT_FAILURE);
}

/* Function to handle abrupt termination after ctrl+c for the server*/
void sigint_handler_mainprocess(int sig) {
    while(waitpid(-1, NULL, 0) > 0);    //parent waits for all children to close
    semctl(semid_msg_table, 0, IPC_RMID, 0);     //remove semaphore from system
    semctl(semid_list, 0, IPC_RMID, 0);
    semctl(semid_grp_table, 0, IPC_RMID, 0);
    semctl(semid_grp_req, 0, IPC_RMID, 0);
    shmdt(client_list);
    shmdt(msg_table);
    shmdt(grp_table);
    shmdt(grp_req);
    shmctl(shmid_cli, IPC_RMID, 0);
    shmctl(shmid_msg, IPC_RMID, 0);
    shmctl(shmid_grp, IPC_RMID, 0);
    shmctl(shmid_req, IPC_RMID, 0);
    exit(EXIT_FAILURE);
}

/* Function to start the server on 'portno' and return the socket file descriptor */
int start_server(int portno) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);   //TCP internet socket
    if (sockfd < 0) {
            perror("[ERROR] socket()");
        exit(EXIT_FAILURE);
    }

    //Initializing server address
    struct sockaddr_in srvr_addr;
    bzero((char *) &srvr_addr, sizeof(srvr_addr));
    srvr_addr.sin_family = AF_INET;
    srvr_addr.sin_addr.s_addr = INADDR_ANY;
    srvr_addr.sin_port = htons(portno);

    //Set socket reusage options for address and port reuse
    int val=1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &val, sizeof(val)) < 0) { 
        perror("[ERROR] setsockopt()"); 
        exit(EXIT_FAILURE);
    } 

    //Binding socket to address 
    if (bind(sockfd, (struct sockaddr *) &srvr_addr, sizeof(srvr_addr)) < 0) {
        perror("[ERROR] bind()");
        exit(EXIT_FAILURE);
    }

    //listening for incoming connections
    if (listen(sockfd,MAX_CLIENTS) < 0) {
        perror("[ERROR] listen()");
    }
    printf("[INFO] Server successfully started\n");
    return sockfd;
}

/*Function to initialize the required semaphores */
void init_semaphores(){
    //semaphore to control message table access
    semid_msg_table = semget(IPC_PRIVATE, 1, 0777|IPC_CREAT);
    semctl(semid_msg_table, 0, SETVAL, 1);

    //semaphore to control client list access
    semid_list = semget(IPC_PRIVATE, 1, 0777|IPC_CREAT);
    semctl(semid_list, 0, SETVAL, 1);

    //semaphore to control group table access
    semid_grp_table = semget(IPC_PRIVATE, 1, 0777|IPC_CREAT);
    semctl(semid_grp_table, 0, SETVAL, 1);

    //semaphore to control group requests table access
    semid_grp_req = semget(IPC_PRIVATE, 1, 0777|IPC_CREAT);
    semctl(semid_grp_req, 0, SETVAL, 1);

    //initializing structure for semaphore control
    pop.sem_num = vop.sem_num = 0;
    pop.sem_flg = vop.sem_flg = 0;
    pop.sem_op = -1 ; vop.sem_op = 1;
}

/*Function to setup the required shared memory blocks */
void init_shared_memory_blocks(){
    int i,j;

    shmid_cli = shmget(IPC_PRIVATE, MAX_CLIENTS*sizeof(int), 0777 | IPC_CREAT);
    client_list = (client*) shmat(shmid_cli, 0, 0);
    P(semid_list);
    for(i=0; i < MAX_CLIENTS; i++) {
        client_list[i].fd = 0;
    }
    V(semid_list);

    
    shmid_msg = shmget(IPC_PRIVATE, MAX_MSGS*sizeof(msg_table_node), 0777 | IPC_CREAT);
    msg_table = (msg_table_node*) shmat(shmid_msg, 0, 0);
    P(semid_msg_table);
    for(i=0; i<MAX_MSGS; i++) {
        msg_table[i].from = 0;
        msg_table[i].to = 0;
        msg_table[i].is_grp = 0;
        bzero(msg_table[i].msg, MSG_SIZE);
    }
    V(semid_msg_table);

    shmid_grp = shmget(IPC_PRIVATE, MAX_GRPS*sizeof(grp_table_node), 0777 | IPC_CREAT);
    grp_table = (grp_table_node*) shmat(shmid_grp, 0, 0);
    P(semid_grp_table);
    for(i=0; i<MAX_GRPS; i++) {
        grp_table[i].fresh = 0;
        grp_table[i].grp_id = 0;
        grp_table[i].ownr_id = 0;
        for(j=0; j<MAX_CLIENTS; j++) {
            grp_table[i].members[j] = 0;
        }
    }
    V(semid_grp_table);

    shmid_req = shmget(IPC_PRIVATE, MAX_GRPS*sizeof(grp_req_node), 0777 | IPC_CREAT);
    grp_req = (grp_req_node*) shmat(shmid_req, 0, 0);
    P(semid_grp_req);
    for(i=0; i<MAX_GRPS; i++) {
        grp_req[i].grp_id = 0;
        grp_req[i].ownr_id = 0;
        grp_req[i].all_informed = 0;
        for(j=0; j<MAX_CLIENTS; j++) {
            grp_req[i].members[j] = 0;
        }
    }
    V(semid_grp_req);
}

/*Function returns the file descriptor for given 'uuid' */
int get_cli_fd_from_uuid(int uuid) {
    int i;
    for(i=0; i<MAX_CLIENTS; i++) {
        if(client_list[i].uuid == uuid) {
            return client_list[i].fd;
        }
    }
    return -1;      //NOT FOUND
}

/*Function returns the uuid for given 'fd' */
int get_cli_uuid_from_fd(int fd) {
    int i;
    for(i=0; i<MAX_CLIENTS; i++) {
        if(client_list[i].fd == fd) {
            return client_list[i].uuid;
        }
    }
    return -1;      //NOT FOUND
}

/*Function to add entries for messages to be sent */
void add_to_msg_table(int from, int to, char *msg, int is_grp) {
    int i;
    for(i=0; i<MAX_MSGS; i++) {
        if(msg_table[i].from == 0) {        //search empty space
            msg_table[i].from = from;
            msg_table[i].to = to;
            msg_table[i].is_grp = is_grp;
            strcpy(msg_table[i].msg, msg);
            break;
        }
    }
}

/*  Function to add 'mems' in group owned by 'ownr'. 'is_req' specifies whether its a 
    consent-based group or a forced group */
int add_to_grp_table(int ownr, int *mems, int is_req) {
    int i, j, grp_generated;
    for(i=0; i<MAX_GRPS; i++) {
        if(grp_table[i].grp_id == 0) {       //search empty space
            P(semid_grp_table);              //acquire group table lock
            grp_table[i].grp_id = rand();
			grp_generated = grp_table[i].grp_id;
            grp_table[i].fresh = 1;          //signifies new entry
            grp_table[i].ownr_id = ownr;
            if(is_req) {
                    grp_table[i].members[0] = mems[0];
            }else {
                for(j=0; j<MAX_CLIENTS; j++) {
                    grp_table[i].members[j] = mems[j];
                }
            }
            V(semid_grp_table);             //release group table lock
            break;
        }
    }
	return grp_generated;
}

/*Function to inform the members of newly fromed groups */
void intimate_new_groups() {
    int i,j;
    for(i=0; i<MAX_GRPS; i++) {
        if(grp_table[i].fresh == 1) {
            char *str = (char*)malloc(100);
            sprintf(str, "You have been added to the group - %d. Admin is - %d\n",grp_table[i].grp_id, 
                get_cli_uuid_from_fd(grp_table[i].ownr_id));

            for(j=0; j<MAX_CLIENTS; j++) {
                if(grp_table[i].members[j] > 0)
                    write(grp_table[i].members[j], str, 100);
            }
            free(str);
            P(semid_grp_table);
            grp_table[i].fresh = 0;
            V(semid_grp_table);
        }
    }
}

/*Function to inform the members for accepting the request to be added in a group */
void intimate_group_requests() {
    int i,j;
	char *str = (char*)malloc(120);
    for(i=0; i<MAX_GRPS; i++) {
        if((grp_req[i].grp_id > 0) && (grp_req[i].all_informed == 0)) {
			printf("[INFO] Informing proposed members of group : %d\n", grp_req[i].grp_id);
			sprintf(str, "User : %d wants to invite you to the group %d\n", 
                           get_cli_uuid_from_fd(grp_req[i].ownr_id), grp_req[i].grp_id); 
            for(j=0; j<MAX_CLIENTS; j++) {
                printf("[DEBUG] Inviting : %d\n", grp_req[i].members[j]);
                if((grp_req[i].members[j] > 0) && (grp_req[i].members[j] != grp_req[i].ownr_id))
					send(grp_req[i].members[j], str, 120, 0);
            }
        }
		grp_req[i].all_informed = 1;      //signifies all members have been informed
		bzero(str, 120);
    }
	free(str);
}

/*Function to add user 'userid' to the group 'grpid' */
void add_user_to_group(int grpid, int userid) {
	int i, j;
	for(i=0; i<MAX_GRPS; i++) {
		if(grp_table[i].grp_id == grpid) {    //group found
			for(j=0; j<MAX_CLIENTS; j++) {
				if(grp_table[i].members[j] == 0) {      //vacant spot found
					P(semid_grp_table);
                    grp_table[i].members[j] = userid;
                    V(semid_grp_table);
                    printf("[INFO] User %d successfully added to group %d\n", userid, grpid);
					break;
				}
			}
		}
	}
}

/*Function to remove user 'userid' from all the groups */
void remove_user_from_all_groups(int userid) {
	int i, j;
	for(i=0; i<MAX_GRPS; i++) {
		for(j=0; j<MAX_CLIENTS; j++) {
			if(grp_table[i].members[j] == userid) {  //user found
				grp_table[i].members[j] = 0;
				printf("[INFO] User %d successfully removed from %d\n", 
							get_cli_uuid_from_fd(connfd), grp_table[i].grp_id);
			}
		}
	}
}

/*Function to delete al groups with admin as 'userid' */
void delete_group_with_admin(int userid) {
    int i, j;
    char *str = (char*)malloc(100);
    for(i=0; i<MAX_GRPS; i++) {
        if(grp_table[i].ownr_id == userid) {        //group found
            printf("[INFO] The group %d has been deleted\n", grp_table[i].grp_id);
            sprintf(str, "The group %d has been deleted\n", grp_table[i].grp_id);
            for(j=0; j<MAX_CLIENTS; j++) {
                if(grp_table[i].members[j] > 0) {
                    send(grp_table[i].members[j], str, 100, 0);     //inform other members
                }
            }
            grp_table[i].grp_id = 0;
            grp_table[i].ownr_id = 0;
            grp_table[i].fresh = 0;
            memset(grp_table[i].members, 0, MAX_CLIENTS*sizeof(int));   //set to zeroes
        }
    }
    free(str);
}

/*Function to close fds that are freed (set negative by child process) */
void cleanup_closed_clients() {
    int i;
    for(i=0; i < MAX_CLIENTS; i++) {
        if(client_list[i].fd < 0) {
            close(-1*client_list[i].fd);
            P(semid_list);
            client_list[i].fd = 0;
            V(semid_list);
        }
    }
}

/*  Function to validate and perform the operation as requested the client connected 
     wiith 'connfd'. The message sent is read in 'buffer' */
void process_query(char *buffer, int connfd) {
    char cmd[15], msg[MSG_SIZE];
    int i, j;
    bzero(cmd, 15);
    bzero(msg, MSG_SIZE);

    if(!strcmp(buffer, "/activegroups")) {
        //size of commas, no. of digits in grpid is taken into account
        char *str = (char*)malloc((10+MAX_GRPS-1)*MAX_GRPS + 1);
        char *str1 = (char*)malloc(10);
        for(i=0; i<MAX_GRPS; i++) {
            for(j=0; j<MAX_CLIENTS; j++) {
                if(grp_table[i].members[j] == connfd) {     //group found
                    sprintf(str1, "%d\n", grp_table[i].grp_id);
                    strcat(str, str1);
                }
            }
        }
        if(strlen(str) == 0 )  str = "YOU HAVE NO ACTIVE GROUPS CURRENTLY\n";
        send(connfd, str, (10+MAX_GRPS-1)*MAX_GRPS + 1, 0);

    }else if(!strncmp(buffer, "/send ", 6)) {
        int to_id;
        if (sscanf(buffer, "%s %d %[^\n]", cmd, &to_id, msg) == 3) {
            to_id = get_cli_fd_from_uuid(to_id);
            if(to_id == -1){
                fprintf(stderr, "[ERROR] Non-existent client id\n");
                send(connfd, "[ERROR] Non-existent client id\n", 35, 0);
                return;
            }
            P(semid_msg_table);  //acquire msg table lock
            add_to_msg_table(connfd, to_id, msg, 0);
            V(semid_msg_table);  //release msg table lock
        }else{
            fprintf(stderr, "[ERROR] Invalid Input\n");
            send(connfd, "[ERROR] Invalid Input\n", 30, 0);
        }

    }else if(!strncmp(buffer, "/broadcast ", 11)) {
        sscanf(buffer, "%s %[^\n]", cmd, msg);
        for(i=0; i<MAX_CLIENTS; i++) {
            if(client_list[i].fd != connfd) {
                if(client_list[i].fd > 0) {
                    P(semid_msg_table);  //acquire msg table lock
                    add_to_msg_table(connfd, client_list[i].fd, msg, 0);
                    V(semid_msg_table);  //release msg table lock
                }
            }
        }

    }else if(!strncmp(buffer, "/makegroupreq ", 14)) {
        strtok(buffer, " ");        //tokenize with space
        char *token = strtok(NULL, " "); //swallow the command word
        int cli_ids[MAX_CLIENTS] = { 0 };
        cli_ids[0] = connfd;        //first member is owner
        i=1;
        while (token != NULL) {
            sscanf(token, "%d", &cli_ids[i]);
            cli_ids[i] = get_cli_fd_from_uuid(cli_ids[i]);
            if(cli_ids[i] == -1){
                fprintf(stderr, "[ERROR] Non-existent client id\n");
                send(connfd, "[ERROR] Non-existent client id\n", 35, 0);
                break;
            }
            token = strtok(NULL, " "); 
            i++;
        }
        int grp_generated = add_to_grp_table(connfd, cli_ids, 1);   //1 denotes consent-based group
        printf("[INFO] Consent-based group created - %d\n", grp_generated);
		//inform requested clients
		for(i=0; i<MAX_GRPS; i++) {
		    if(grp_req[i].grp_id == 0) {       //search empty space
				grp_req[i].grp_id = grp_generated;
				grp_req[i].ownr_id = connfd;
				grp_req[i].all_informed = 0;
		        for(j=0; j<MAX_CLIENTS; j++) {
					grp_req[i].members[j] = cli_ids[j];
		        }
		        break;
		    }
    	}

    }else if(!strncmp(buffer, "/makegroup ", 11)) {
        i=1;
        strtok(buffer, " ");        //tokenize with space
        char *token = strtok(NULL, " "); //swallow the command word
        int cli_ids[MAX_CLIENTS] = { 0 };
        cli_ids[0] = connfd;
        while (token != NULL) {
            sscanf(token, "%d", &cli_ids[i]);
            cli_ids[i] = get_cli_fd_from_uuid(cli_ids[i]);
            if(cli_ids[i] == -1){
                fprintf(stderr, "[ERROR] Non-existent client id\n");
                send(connfd, "[ERROR] Non-existent client id\n", 35, 0);
                break;
            }
            if(cli_ids[i] == connfd) {     //prevent re-adding owner
                cli_ids[i] = 0;
                i--;
            }
            token = strtok(NULL, " "); 
            i++;
        }
        add_to_grp_table(connfd, cli_ids, 0);       //0 denotes forced group
        
    }else if(!strncmp(buffer, "/sendgroup ", 11)) {
        int to_id, invalid_grp=1;
        if (sscanf(buffer, "%s %d %[^\n]", cmd, &to_id, msg) == 3) {
            for(i=0; i<MAX_GRPS; i++) {
                if(grp_table[i].grp_id == to_id) {
                    invalid_grp = 0;
                    for(j=0; j<MAX_CLIENTS; j++) {
                        if(grp_table[i].members[j] > 0) {
                            P(semid_msg_table);  //acquire msg table lock
                            add_to_msg_table(connfd, grp_table[i].members[j], msg, 1);
                            V(semid_msg_table);  //release msg table lock
                        }
                    }
                }
            }
            if(invalid_grp) {
                send(connfd, "Requested group does not exist\n", 35, 0);
            }
        }else {
            fprintf(stderr, "[ERROR] Invalid Input\n");
            send(connfd, "[ERROR] Invalid Input\n", 30, 0);
        }

    }else if(!strcmp(buffer, "/activeallgroups")) {
        //size of commas, no. of digits in grpid is taken into account
        char *str = (char*)malloc((10+MAX_GRPS-1)*MAX_GRPS + 1);
        char *str1 = (char*)malloc(10);
        for(i=0; i<MAX_GRPS; i++) {
            if(grp_table[i].grp_id > 0) {
                sprintf(str1, "%d\n", grp_table[i].grp_id);
                strcat(str, str1);
            }
        }
        if(strlen(str) == 0 )  str = "NO ACTIVE GROUPS CURRENTLY ON SERVER\n";
        send(connfd, str, (10+MAX_GRPS-1)*MAX_GRPS + 1, 0);

    }else if(!strcmp(buffer, "/active")) {
        //size of commas, no. of digits in uuid is taken into account
        char *str = (char*)malloc((10+MAX_CLIENTS-1)*MAX_CLIENTS + 1);
        char *str1 = (char*)malloc(10);
        for (i=0; i<MAX_CLIENTS; ++i) {
            if(client_list[i].uuid) {       //client exists
                sprintf(str1, "%d\n", client_list[i].uuid);
                strcat(str, str1);
            }
        }

        send(connfd, str, (10+MAX_CLIENTS-1)*MAX_CLIENTS + 1, 0);

    }else if(!strncmp(buffer, "/joingroup ", 11)) {
		int grpid, authenticate=0, invalid_grp=1;
        sscanf(buffer, "%s %d", cmd, &grpid);  
		for(i=0; i<MAX_GRPS; i++) {
		    if(grp_req[i].grp_id == grpid) {
                invalid_grp = 0;    //group exists
				for(j=0; j<MAX_CLIENTS; j++) {
					if(grp_req[i].members[j] == connfd) {
						authenticate=1;		//valid request
					}		
				}
			}
		}

        if(invalid_grp) {
            send(connfd, "Requested group does not exist\n", 35, 0);
        }

        if(authenticate) {          //invited user
		  printf("[INFO] Adding user %d to group %d\n", connfd, grpid);
		  add_user_to_group(grpid, connfd);
          send(connfd, "You are successfully added to the group\n", 45, 0);
        }else {                 //uninvited user
            printf("[INFO] Uninvited user %d requesting access to group %d\n", connfd, grpid);
            send(connfd, "You are not invited. Sorry.\n", 30, 0);
        }

    }else if(!strcmp(buffer, "/quit")) {
		remove_user_from_all_groups(connfd);
        delete_group_with_admin(connfd);
        for(i=0; i<MAX_CLIENTS; i++) {
            if(client_list[i].fd == connfd) {
                client_list[i].fd = -1 * client_list[i].fd;
                client_list[i].uuid = 0;
            }
        }
		send(connfd, "BYE !!\n", 7, 0);

    }else {         //INVALID COMMAND
        fprintf(stderr, "INVALID COMMAND\n");
        send(connfd, "INVALID COMMAND\n", 20, 0);
    }
}

/*Function to read messages from the client and process */
void read_from_client(int connfd) {
    char *buffer = (char*)malloc(MSG_SIZE);
    while(1) {
        bzero(buffer,MSG_SIZE-1);

        //read message from the client
        int read_len = recv(connfd, buffer, MSG_SIZE-1, 0);
        if (read_len < 0) break;        //no reads due to non-blocking nature

        if(read_len == 0 || !strncmp(buffer, "EXIT", 4) || !strncmp(buffer, "exit", 4)) {
            printf("[INFO] Client socket %d initiated disconnection\n", connfd);
            break;
        }

        buffer[strcspn(buffer, "\n")] = 0; //trim newline
        printf("[INFO] Client socket %d sent message: %s\n", connfd, buffer);
        process_query(buffer, connfd);
    }
}

/*Function to send messages to a client based on msg_table */
void send_msg_to_client() {
    int i;
    for(i=0; i<MAX_MSGS; i++) {
        if(msg_table[i].from > 0) {
            printf("[DEBUG] Sending msg '%s' from %d to %d\n",
                         msg_table[i].msg, msg_table[i].from, msg_table[i].to);
            int success = send(msg_table[i].to, msg_table[i].msg, strlen(msg_table[i].msg), 0);
            if(errno == 9){
                break;
            }else if(success < 0) {
                perror("[ERROR] send()");
            }else {
                msg_table[i].from = 0;
                msg_table[i].to = 0;
                bzero(msg_table[i].msg, MSG_SIZE);
            }
            break;
        }
    }
}

/* First command line argument specifies the port no */
int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler_mainprocess);     //special handling of Ctrl+C
    signal(SIGTSTP, SIG_IGN);   //ignoring of Ctrl+Z

    srand(time(0)); //for random number generation
    
    int portno = (argc==2) ? atoi(argv[1]) : DEFAULT_PORT;
    printf("Running on default port %d as no port was specified\n", portno);
    int sockfd = start_server(portno);  //start server

    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    init_semaphores();          //initialize semaphoes
    init_shared_memory_blocks();        //setup shared memory 

    fcntl(sockfd, F_SETFL, O_NONBLOCK);     //convert to non-blocking mode

    int i;
    
    while(1) {
        //accept incoming connections
        while((connfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) < 0) {
            //all the code in this block will be executed continuously by the parent process            
            send_msg_to_client();
            intimate_new_groups();
			intimate_group_requests();
            cleanup_closed_clients();
			//while(waitpid(-1, NULL, WNOHANG) > 0);  //parent waits on all children to close     
        }

        int clientSlotAvailable=0;
        printf("[DEBUG] Connected clients are - "); 
        for(i=0; i < MAX_CLIENTS; i++) {
            if(client_list[i].fd == 0) {
                P(semid_list);          // acquire lock     
                client_list[i].fd = connfd;
                client_list[i].uuid = rand();
                V(semid_list);          //release lock
                clientSlotAvailable = 1;
                break;
            }
            printf("  {%d, %d}", client_list[i].fd, client_list[i].uuid);
        }
        printf("\n");

        if(!clientSlotAvailable) {      //server full
            printf("[DEBUG] Connection Limit Exceeded !!\n");
            write(connfd, "Connection Limit Exceeded !!\n", 60);
            close(connfd);
            continue;
        }

        //spawn a new process
        if(!fork()) {   //child process for client
            signal(SIGINT, sigint_handler);     //special handling of Ctrl+C
            printf("[INFO] Connected with client socket number %d\n", connfd);
            char *str = (char*)malloc(100);
            sprintf(str, "Welcome to the chat app\nYour user id is %d",
                                    get_cli_uuid_from_fd(connfd));
            write(connfd, str, 100);
            free(str);

            client_list = (client*) shmat(shmid_cli, 0, 0);
            msg_table = (msg_table_node*) shmat(shmid_msg, 0, 0);
            grp_table = (grp_table_node*) shmat(shmid_grp, 0, 0);
            grp_req = (grp_req_node*) shmat(shmid_req, 0, 0);

            //start reading from the client
            read_from_client(connfd);
            
            close(connfd);      //close socket after interaction done
            printf("[INFO] Closed connection with client socket %d\n", connfd);
            //mark socket to be closed
            for(i=0; i < MAX_CLIENTS; i++) {
                if(client_list[i].fd == connfd) {
                    P(semid_list);
                    client_list[i].fd = -1*connfd;
                    V(semid_list);
                    break;
                }
            }
            shmdt(client_list);
            shmdt(msg_table);
            shmdt(grp_table);
            shmdt(grp_req);
            exit(EXIT_SUCCESS); //exit child process
            
        }
    }
    return 0;
}
