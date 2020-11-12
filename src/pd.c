#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>

#define DEFAULT_PD_PORT "57002"
#define DEFAULT_AS_PORT "58002"

#define PROTOCOL_ERROR_MESSAGE "Error: Command not supported"

#define bool int
#define true 1
#define false 0

#define max(A, B) ((A) >= (B)?(A):(B))

char *pdIP = NULL;
char *pdPort = NULL;
char *asIP = NULL;
char *asPort = NULL;

static void parseArgs (long argc, char* const argv[]) {
    char c;

    pdIP = argv[1];
    pdPort = DEFAULT_PD_PORT;
    asIP = argv[1];
    asPort = DEFAULT_AS_PORT;

    while ((c = getopt(argc, argv, "d:n:p:")) != -1){
        switch (c) {
            case 'd':
                pdPort = optarg;
                break;
            case 'n':
                asIP = optarg;
                break;
            case 'p':
                asPort = optarg;
                break;
        }
    }
}

int main(int argc, char *argv[]) {
    int c;
    int counter;
    int asfd, pdfd;
    char msg[128];
    char line[128];
    char buffer[128];
    char command[128];
    char arg1[16], arg2[16];
    char UID[6], pass[9];
    struct sockaddr_in asaddr, pdaddr;
    struct addrinfo ashints, *asres, pdhints, *pdres;
    ssize_t n;
	fd_set fds;	
    socklen_t asaddrlen, pdaddrlen;
	struct timeval tv;

    parseArgs(argc, argv);

    asfd = socket(AF_INET,SOCK_DGRAM,0); // client socket
    if(asfd == -1) {
        exit(1);
    }

	pdfd = socket(AF_INET, SOCK_DGRAM, 0); // server socket
	if(pdfd == -1) {
		exit(1);
	}

	// setting timeout for reading AS messages. At most the PD will wait 5 seconds for a reply
	tv.tv_sec = 5;
	tv.tv_usec = 0; 
	if (setsockopt(asfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("setsockopt()");
		exit(1);
	}

    memset(&ashints, 0, sizeof(ashints));
    ashints.ai_family=AF_INET; // IPv4
    ashints.ai_socktype=SOCK_DGRAM; // UDP

	memset(&pdhints, 0, sizeof(pdhints));
    pdhints.ai_family=AF_INET; // IPv4
    pdhints.ai_socktype=SOCK_DGRAM; // UDP
	pdhints.ai_flags=AI_PASSIVE;

    if(getaddrinfo(asIP, asPort, &ashints, &asres) != 0) {
        exit(1);
	}

	if(getaddrinfo(pdIP, pdPort, &pdhints, &pdres) != 0) {
		exit(1);
	}

	// binds so that the server socket can listen to UDP requests from the AS
	if(bind(pdfd, pdres->ai_addr, pdres->ai_addrlen) == -1) {
        perror("bind()");
		exit(1);
	}

	bool reg = false; // flag controls whether or not there already is a registered user

    while (true){
		FD_ZERO(&fds);
    	FD_SET(pdfd, &fds);
		FD_SET(0, &fds);

		// selects between user input in stdin and AS messages
        counter = select(pdfd + 1, &fds, (fd_set *) NULL, (fd_set *) NULL, (struct timeval *) NULL);
        if (counter <= 0) {
			perror("select()");
            exit(1);
        }
		
		// deal with AS messages
        if(FD_ISSET(pdfd, &fds)){
			pdaddrlen = sizeof(pdaddr);
			n = recvfrom(pdfd, buffer, 128, 0, (struct sockaddr*)&pdaddr, &pdaddrlen);
			buffer[n]= '\0';

			// extracting the validation codes for the registered user
			char Fop[2];
			char FName[25];
            c = sscanf(buffer, "%s %s %s %s %s", command, arg1, arg2, Fop, FName);

            if(strcmp(arg1, UID) == 0) {
				sprintf(msg, "RVC %s OK\n", UID);
                printf("VLC=%s, ", arg2);
				if(strcmp(Fop, "R") == 0 || strcmp(Fop, "U") == 0 || strcmp(Fop, "D") == 0) {
					printf("%s %s\n", Fop, FName);
				}
				else {
					printf("%s\n", Fop);
				}
            }
			else {
                strcpy(msg,"RVC NOK\n");
            }
			
			// replying to the AS with the according status message
            n = sendto(pdfd, msg, strlen(msg), 0, (struct sockaddr*)&pdaddr, pdaddrlen);
            if(n == -1) {
				perror("sendto()");
                exit(1);
            }
        }
        // deal with user input
		if(FD_ISSET(0, &fds)) { 
            fgets(line, sizeof(line), stdin);

            c = sscanf(line, "%s %s %s", command, arg1, arg2);

			/* deal with an exit command. sends UNR message to AS if there is a logged in user, and closes the PD.*/
            if(c == 1 && strlen(UID) == 5 && strlen(pass) == 8 && strcmp(command, "exit") == 0) {
                if(reg) {
                    sprintf(msg, "UNR %s %s\n", UID, pass);

                    n = sendto(asfd, msg, strlen(msg), 0, asres->ai_addr, asres->ai_addrlen);
                    if(n <= -0) {
						perror("sendto()");
                        exit(1);
                    }

                    asaddrlen = sizeof(asaddr);
                    n = recvfrom(asfd, buffer, 128, 0, (struct sockaddr*)&asaddr, &asaddrlen);
                    buffer[n] = '\0';

                    if(n <= 0) {
						perror("recvfrom()");
                        exit(1);
                    }

                    if(!strcmp(buffer, "RUN OK\n")) {
                        reg = false;
						puts("Unregister was successful");
                    }
					else {
						reg = false;
						puts("There was no user folder in the AS. Closing PD");
					}
					break;
                }
				else {
					puts("There was registered user. Closing PD");
					break;
				}
            }
			/* handles user register. registers a user for the first time (or a new user if already registered) */
            else if (c == 3 && strcmp(command, "reg") == 0 && strlen(arg1) == 5 && strlen(arg2) == 8) {
                sprintf(msg, "REG %s %s %s %s\n", arg1, arg2, pdIP, pdPort);

                n = sendto(asfd, msg, strlen(msg), 0, asres->ai_addr, asres->ai_addrlen);
                if(n == -1) {
					perror("sendto()");
                    exit(1);
                }

                asaddrlen = sizeof(asaddr);
                n = recvfrom(asfd, buffer, 128, 0, (struct sockaddr*)&asaddr, &asaddrlen);
                buffer[n] = '\0';

                if(n == -1) {
					perror("recvfrom()");
                    exit(1);
                }

                if(!strcmp(buffer, "RRG OK\n")) {
                    reg = true;
                    strcpy(UID,arg1);
                    strcpy(pass,arg2);
					puts("Registered user successfully");
                }
				else {
					reg = false;
					puts("Registration was not successful");
				}
            }
			// if the PD did not have a registered user or receives a poorly formatted answer. Also accepts poorly formatted exits
			else {
				if(strcmp(command, "exit") == 0) {
					puts("There was no user folder in the AS. Closing PD");
					break;
				}
				else {
					puts(PROTOCOL_ERROR_MESSAGE);
				}
			}
        }
    }


	// frees and closes
    freeaddrinfo(asres);
	freeaddrinfo(pdres);
    close(asfd);
	close(pdfd);
    exit(0);
}