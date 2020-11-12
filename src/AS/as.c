#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>
#include <dirent.h>

#define max(A, B) ((A) >= (B)?(A):(B))

#define bool int
#define true 1
#define false 0

#define DEFAULT_AS_PORT "58002"

#define BUFFER_SIZE 512
#define AUX_SIZE 64

bool verbose = false;
bool cycle = true;

char *asPort = NULL;

static void parseArgs(long argc, char* const argv[]) {
    char c;

    asPort = DEFAULT_AS_PORT;

    while ((c = getopt(argc, argv, "p:v")) != -1){
        switch (c) {
            case 'p':
                asPort = optarg;
                break;
			case 'v':
				verbose = true;
				break;
        }
    }
}

long writeMessage(int fd, char *msg, long int msgSize) {
	ssize_t nleft, nwritten, ntotal = 0;
	char *ptr;
	ptr = msg;
	nleft = msgSize;

	while(nleft > 0) {
		nwritten = write(fd, ptr, nleft);

		if(nwritten <= 0) {
			exit(1);
		}
		ntotal += nwritten;
		nleft -= nwritten;
		ptr += nwritten;
	}

	return ntotal;
} 

long readMessage(int fd, char *msg) {
	ssize_t nleft, nread, ntotal = 0;
	char *ptr;
	ptr = msg;
	nleft = BUFFER_SIZE;

	while(nleft > 0) {
		nread = read(fd, ptr, nleft);

		if(nread == -1) {
			exit(1);
		}
		else if(nread == 0) {
			return 0;
		}

		nleft -= nread;
		ntotal += nread;
		if(ptr[nread-1] == '\n'){
			break;
		}
		ptr += nread;
	}

	ptr[nread] = '\0';

	return ntotal;
}

bool userIsLoggedIn(char* UID){
	FILE *fp;
	char filename[64];

	sprintf(filename, "AS/USERS/%s/%s_login.txt", UID, UID);
	if((fp = fopen(filename, "r")) == NULL){
		return false;
	}

	if(fclose(fp) != 0) {
		perror("fclose()");
		exit(1);
	}

	sprintf(filename, "AS/USERS/%s/%s_reg.txt", UID, UID);
	if((fp = fopen(filename, "r")) == NULL) {
		return false;
	}

	if(fclose(fp) != 0) { 
		perror("fclose()");
		exit(1);
	}

	return true;
}

bool userExists(char* UID){
	DIR *d;
	char dirname[64];
	sprintf(dirname, "AS/USERS/%s", UID);

	d = opendir(dirname);

	if(!d) {
		return false;
	}

	if(closedir(d) != 0) {
		perror("closedir()");
		exit(1);
	}
	return true;
}

int main(int argc, char *argv[]) {
	DIR *d;
	FILE *fp;
	fd_set fds;	
	ssize_t n;
	pid_t pid;
    socklen_t asudpaddrlen, pdaddrlen, astcpaddrlen;
	struct dirent *dir;
	struct sigaction act;
    struct sockaddr_in asudpaddr, pdaddr, astcpaddr;
    struct addrinfo asudphints, *asudpres, pdhints, *pdres, astcphints, *astcpres;
	char *c;
	char dirname[64];
	char filename[128];
	char aux[AUX_SIZE];
	char buffer[BUFFER_SIZE];
	char arg1[16], arg2[16], arg3[16], arg4[128], arg5[16];
	char sockIP[32];
	int sockPort;
	int ret;
	int counter;
	int asudpfd, pdfd, astcpfd, newfd;
	struct timeval tv;

	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;

	parseArgs(argc, argv);

	/* Avoid zombies when child processes die. */
	if(sigaction(SIGCHLD, &act, NULL) == -1) {
		perror("sigaction()");
		exit(1);
	}

	if((asudpfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		perror("socket()");
		exit(1);
	}

	if((pdfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		perror("socket()");
		exit(1);
	}

	// setting timeout for reading PD messages. At most the AS will wait 2 seconds for a reply
	tv.tv_sec = 2;
	tv.tv_usec = 0; 
	if (setsockopt(pdfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("setsockopt()");
		exit(1);
	}

	if((astcpfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket()");
		exit(1);
	}
		
	memset(&asudphints, 0, sizeof(asudphints));
	asudphints.ai_family = AF_INET;
	asudphints.ai_socktype = SOCK_DGRAM;
	asudphints.ai_flags = AI_PASSIVE;

	memset(&pdhints, 0, sizeof(pdhints));
    pdhints.ai_family=AF_INET;
    pdhints.ai_socktype=SOCK_DGRAM; 

	memset(&astcphints, 0, sizeof(astcphints));
    astcphints.ai_family=AF_INET;
    astcphints.ai_socktype=SOCK_STREAM; 
	astcphints.ai_flags=AI_PASSIVE;

	if((getaddrinfo(NULL, asPort, &asudphints, &asudpres)) != 0) {
		perror("getaddrinfo()");
		exit(1);
	}

	if((getaddrinfo(NULL, asPort, &astcphints, &astcpres)) != 0) {
		perror("getaddrinfo()");
		exit(1);
	}

	// bind so the server socket can listen to UDP requests from the PD's and FS
	if(bind(asudpfd, asudpres->ai_addr, asudpres->ai_addrlen) == -1) {
		perror("bind()");
		exit(1);
	}

	// bind so the server socket can listen to TCP requests from the Users
	if(bind(astcpfd, astcpres->ai_addr, astcpres->ai_addrlen) == -1) {
		perror("bind()");
		exit(1);
	}

	// maximum length of 128 for the queue of pending User TCP connections
	if(listen(astcpfd, 128) == -1) {
		perror("listen()");
		exit(1);
	}

	while(cycle) {
		FD_ZERO(&fds);
		FD_SET(asudpfd, &fds);
		FD_SET(astcpfd, &fds);
		FD_SET(0, &fds);

		// selects between messages from stdin, PD and FD (UDP), and Users (TCP)
		counter = select(max(asudpfd, astcpfd) + 1, &fds, (fd_set *)NULL, (fd_set *)NULL, (struct timeval *)NULL);
 		if(counter <= 0) {
            exit(1);
        }

		// deal with PD and FD (UDP) messages
		if(FD_ISSET(asudpfd, &fds)) {
			asudpaddrlen = sizeof(asudpaddr);
			n = recvfrom(asudpfd, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&asudpaddr, &asudpaddrlen);

			//get the client's IP and Port 
			strcpy(sockIP, inet_ntoa(asudpaddr.sin_addr));
			sockPort = ntohs(asudpaddr.sin_port);

			buffer[n] = '\0';

			if(n == -1) {
				perror("recvfrom()");
				exit(1);
			}

			sscanf(buffer, "%s ", arg1);

			if(verbose){
				if(strcmp(arg1, "REG") == 0) {
					printf("Received request from IP = %s, Port = %d\nRequest Description: register user\n", sockIP, sockPort);
				}
				else if(strcmp(arg1, "UNR") == 0) {
					printf("Received request from IP = %s, Port = %d\nRequest Description: unregister user\n", sockIP, sockPort);
				}
				else if(strcmp(arg1, "VLD") == 0) {
					printf("Received request from IP = %s, Port = %d\nRequest Description: Validate operation\n", sockIP, sockPort);
				}
			}

			if(strcmp(arg1, "REG") == 0) {
				n = sscanf(buffer, "%s %s %s %s %s", arg1, arg2, arg3, arg4, arg5); // REG UID pass PDIP PDport

				if(n == 5) {
					if(strlen(arg2) == 5 && strlen(arg3) == 8) {
						int valid = true;
						
						// argument validation
						c = arg2;
						while(*c) {
							if(isdigit(*c++) == 0) {
								valid = false;
								break;
							}
						}

						c = arg3;
						while(*c) {
							if(isalnum(*c++) == 0) { //alphanumeric
								valid = false;
								break;
							}
						}

						if(!valid) {
							sendto(asudpfd, "RRG NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);
							continue;
						}

						sprintf(dirname, "AS/USERS/%s", arg2);

						d = opendir(dirname);

						// if directory doesn't exist, create the directory
						if(!d) {
							if(mkdir(dirname, 0777) != 0) {
								perror("mkdir()");
								exit(1);
							}

							d = opendir(dirname);
						}
						
						if(d) {
							sprintf(filename, "%s/%s_pass.txt", dirname, arg2);

							fp = fopen(filename, "r");

							// The password file doesn't exist, therefore it's the first registration
							if(fp == NULL) {
								fp = fopen(filename, "w");

								if(fp == NULL) {
									perror("fopen()");
									exit(1);
								}

								n = fwrite(arg3, 1, 8, fp);

								if(n != 8) {
									perror("fwrite()");

									if(fclose(fp) != 0) {
										perror("fclose()");
									}
									exit(1);
								}

								n = sendto(asudpfd, "RRG OK\n", 7, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

								if(n == -1) {
									perror("sendto()");
									exit(1);
								}

								if(fclose(fp) != 0) {
									perror("fclose()");
									exit(1);
								}
							}
							// user already registrated
							else {
								n = fread(buffer, 1, BUFFER_SIZE, fp);
								buffer[n] = '\0';

								if(fclose(fp) != 0) {
									perror("fclose()");
									exit(1);
								}

								//verifies user's credentials
								if(strcmp(buffer, arg3) == 0) {
									n = sendto(asudpfd, "RRG OK\n", 7, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

									if(n == -1) {
										perror("sendto()");
										exit(1);
									}
								}
								else {
									n = sendto(asudpfd, "RRG NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

									if(n == -1) {
										perror("sendto()");
										exit(1);
									}
								}
							}
							sprintf(filename, "%s/%s_reg.txt", dirname, arg2);

							fp = fopen(filename, "w");

							if(fp == NULL) {
								perror("fopen()");
								exit(1);
							}

							sprintf(buffer, "%s %s", arg4, arg5); // PDIP PDport

							n = fwrite(buffer, 1, strlen(buffer), fp);

							if(n < 0) {
								perror("fwrite()");
								exit(1);
							}

							if(fclose(fp) != 0) {
								perror("fclose()");
								exit(1);
							}

							if(closedir(d) != 0) {
								perror("closedir()");
								exit(1);
							}
						}
						else {
							perror("opendir()");
							exit(1);
						}					
					}
					else {
						if(sendto(asudpfd, "RRG NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen) < 0) {
							perror("sendto()");
							exit(1);
						}
					}
				}
				else {
					n = sendto(asudpfd, "RRG ERR\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

					if(n < 0) {
						perror("sendto()");
						exit(1);
					}
				}
			}
			else if(strcmp(arg1, "UNR") == 0) {
				n = sscanf(buffer, "%s %s %s", arg1, arg2, arg3); // UNR UID pass

				if(n == 3) {
					sprintf(dirname, "AS/USERS/%s", arg2);

					//user doesnt exist
					if(!userExists(arg2)) {
						sendto(asudpfd, "RUN NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);
						if(n < 0) {
							perror("sendto()");
							exit(1);
						}
					}
					//user exists
					else {
						sprintf(filename, "%s/%s_pass.txt", dirname, arg2);

						fp = fopen(filename, "r");

						if(fp == NULL) {
							perror("fopen()");
							exit(1);
						}

						n = fread(buffer, 1, BUFFER_SIZE, fp);
						buffer[n] = '\0';

						if(fclose(fp) != 0) {
							perror("fclose()");
							exit(1);
						}

						//verifies user's credentials
						if(strcmp(buffer, arg3) == 0) {
							bool ok = true;

							sprintf(buffer, "%s/%s_reg.txt", dirname, arg2);
							if(remove(buffer) != 0) {
								// Make sure the fiel exists
								ok = false;
							}			

							if(ok) {
								n = sendto(asudpfd, "RUN OK\n", 7, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

								if(n < 0) {
									perror("sendto()");
									exit(1);
								}
							}
							else {
								sendto(asudpfd, "RUN NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

								if(n < 0) {
									perror("sendto()");
									exit(1);
								}
							}
						}
						//wrong password
						else{
							sendto(asudpfd, "RUN NOK\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

							if(n < 0) {
								perror("sendto()");
								exit(1);
							}
						}
					}
				}
				else{
					n = sendto(asudpfd, "RUN ERR\n", 8, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

					if(n < 0) {
						perror("sendto()");
						exit(1);
					}
				}
			}
			else if(strcmp(arg1, "VLD") == 0) {
				n = sscanf(buffer, "VLD %s %s", arg1, arg2); // VLD UID TID

				// Argument validation
				if(n == 2 && strlen(arg1) == 5 && strlen(arg2) == 4  && userExists(arg1) && userIsLoggedIn(arg1)) {
					sprintf(filename, "AS/USERS/%s/%s_tid.txt", arg1, arg1);

					fp = fopen(filename, "r");

					// No TID file
					if(fp == NULL) {
						sprintf(buffer, "CNF %s %s E\n", arg1, arg2); // CNF UID TID E

						n = sendto(asudpfd, buffer, strlen(buffer), 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

						if(n < 0) {
							perror("sendto()");
							exit(1);
						}

						continue;
					}

					n = fread(buffer, 1, 5, fp);

					buffer[n - 1] = '\0';	// apagar o espaco

					if(strcmp(buffer, arg2) == 0) {
						n = fread(aux, 1, AUX_SIZE, fp);
						aux[n] = '\0';

						if(fclose(fp) != 0) {
							perror("fclose()");
							exit(1);
						}

						sprintf(buffer, "CNF %s %s %s\n", arg1, arg2, aux); // CNF UID TID Fop [Fname] 

						n = sendto(asudpfd, buffer, strlen(buffer), 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

						// remover o ficheiro do tid
						if(remove(filename) != 0) {
							perror("remove()");
							exit(1);
						}

						if(aux[0] == 'X') {
							bool ok = true;

							sprintf(dirname, "AS/USERS/%s", arg1);
							d = opendir(dirname);

							if(d == NULL) {
								perror("opendir()");
								exit(1);
							}

							// delete all user's files
							while((dir = readdir(d)) != NULL) {
								if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
									continue;
								}

								sprintf(buffer, "%s/%s", dirname, dir->d_name);
								if(remove(buffer) != 0) {
									ok = false;
								}
							}

							if(!ok) {
								perror("remove()");
								exit(1);
							} 
							else {
								if(rmdir(dirname) != 0) {
									perror("rmdir()");
									exit(1);
								}
							}
						}
					}
					else {
						sprintf(buffer, "CNF %s %s E\n", arg1, arg2); // CNF UID TID E

						n = sendto(asudpfd, buffer, strlen(buffer), 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);
					}

					if(n < 0) {
						perror("sendto()");
						exit(1);
					}
				}
			}
			else {
				n = sendto(asudpfd, "ERR\n", 4, 0, (struct sockaddr*)&asudpaddr, asudpaddrlen);

				if(n < 0) {
					perror("sendto()");
					exit(1);
				}	
			}
		}
		
		// deal with User (TCP) messages
		if(FD_ISSET(astcpfd, &fds)) {
			astcpaddrlen = sizeof(astcpaddrlen);

			do {
				newfd = accept(astcpfd, (struct sockaddr*)&astcpaddr, &astcpaddrlen);
			} while (newfd == -1 && errno == EINTR);			

			if(newfd == -1) {
				perror("accept()");
				exit(1);
			}

			if((pid=fork()) == -1) {
				perror("fork()");
				exit(1);
			}
			// child proccess
			else if(pid == 0) {
				close(astcpfd);

				char sockIP[16];
				int sockPort;
				char RID[5];
				char UID[6];
				char Fop[2];
				char FName[25];
				int VC;
				int TID;
				int potentialVC;
				struct sockaddr_in addr;
				socklen_t addrsize = sizeof(struct sockaddr_in);

				// get the client's IP and Port 
				if(getpeername(newfd, (struct sockaddr*)&addr, &addrsize) != 0) {
					perror("getpeername()");
					exit(1);
				}

				strcpy(sockIP, inet_ntoa(addr.sin_addr));
				sockPort = ntohs(addr.sin_port);

				while(true) {
					n = readMessage(newfd, buffer);

					if(n < 0) {
						perror("read()");
						exit(1);
					}
					else if(n == 0) {
						sprintf(filename, "AS/USERS/%s/%s_login.txt", arg2, arg2);
						remove(filename);
						printf("Closing user connection\n");
						break;
					}

					buffer[n] = '\0';

					sscanf(buffer, "%s ", arg1);
					
					if(strcmp(arg1, "LOG") == 0) {
						ret = sscanf(buffer, "LOG %s %s\n", arg2, arg3); // LOG UID pass

						// Argument validation
						if(ret != 2 || strlen(arg2) != 5 || strlen(arg3) != 8) {
							writeMessage(newfd, "RLO ERR\n", 8);
							continue;
						}

						sprintf(dirname, "AS/USERS/%s", arg2);

						if(!userExists(arg2)) {
							writeMessage(newfd, "RLO ERR\n", 8);
							continue;
						}

						sprintf(filename, "%s/%s_reg.txt", dirname, arg2);

						fp = fopen(filename, "r");

						if(fp == NULL) {
							writeMessage(newfd, "RLO ERR\n", 8);
							continue;
						}

						if(fclose(fp) != 0) {
							perror("fclose()");
							close(newfd);
							exit(1);
						}

						sprintf(filename, "%s/%s_pass.txt", dirname, arg2);

						fp = fopen(filename, "r");

						if(fp == NULL) {
							writeMessage(newfd, "RLO ERR\n", 8);
							continue;
						}

						if(verbose){
							printf("Received login command from IP = %s, Port = %d\nRequest Description: login user:%s with password:%s\n", sockIP, sockPort, arg2, arg3);
						}

						n = fread(buffer, 1, BUFFER_SIZE, fp);
						buffer[n] = '\0';

						if(fclose(fp) != 0) {
							perror("fclose()");
							exit(1);
						}

						if(strcmp(buffer, arg3) == 0) {
							sprintf(filename, "%s/%s_login.txt", dirname, arg2);
							strcpy(UID, arg2);

							if((fp = fopen(filename, "w")) == NULL) {
								perror("fopen()");
								exit(1);
							}

							if(fclose(fp) != 0) {
								perror("fclose()");
								exit(1);
							}

							writeMessage(newfd, "RLO OK\n", 7);
						}
						else {
							writeMessage(newfd, "RLO NOK\n", 8);
						}
					}
					else if(strcmp(arg1, "REQ") == 0) {
						ret = sscanf(buffer, "REQ %s %s %s %s", arg1, arg2, arg3, arg4); // REQ UID RID Fop [Fname]

						if(strlen(arg1) != 5 || strlen(arg2) != 4) {
							writeMessage(newfd, "RRQ ERR\n", 8);
							continue;
						}

						if(userExists(arg1) != true) {
							writeMessage(newfd, "RRQ EUSER\n", 10);
							continue;
						}

						if(userIsLoggedIn(arg1) != true || strcmp(UID, arg1) != 0) {
							writeMessage(newfd, "RRQ ELOG\n", 9);
							continue;
						}

						if((strcmp(arg3, "R") == 0 || strcmp(arg3, "U") == 0 || strcmp(arg3, "D") == 0) && ret == 4) {
							if(strlen(arg4) > 24) {
								writeMessage(newfd, "RRQ ERR\n", 8);
								continue;							
							}
						}

						if(strcmp(arg3, "X") == 0 || strcmp(arg3, "L") == 0 || strcmp(arg3, "R") == 0 || strcmp(arg3, "U") == 0 || strcmp(arg3, "D") == 0) {
							bool hasFname = false;

							if((strcmp(arg3, "R") == 0 || strcmp(arg3, "U") == 0 || strcmp(arg3, "D") == 0) && ret == 4) {
								if(strlen(arg4) <= 24) {
									hasFname = true;
								}
								else {
									writeMessage(newfd, "RRQ ERR\n", 8);
									continue;
								}
							}
							else if((strcmp(arg3, "L") == 0 || strcmp(arg3, "X") == 0) && ret == 3) {
								hasFname = false;
							}
							else {
								writeMessage(newfd, "RRQ ERR\n", 8);
								continue;
							}

							if(verbose){
								if(strcmp(arg3, "L") == 0) {
									printf("Received request from IP = %s, Port = %d\nRequest Description: list user's files\n", sockIP, sockPort);
								}
								else if(strcmp(arg3, "R") == 0) {
									printf("Received request from IP = %s, Port = %d\nRequest Description: retrieve the contents of the file: %s\n", sockIP, sockPort, arg4);
								}
								else if(strcmp(arg3, "U") == 0) {
									printf("Received request from IP = %s, Port = %d\nRequest Description: upload the file: %s\n", sockIP, sockPort, arg4);
								}
								else if(strcmp(arg3, "D") == 0) {
									printf("Received request from IP = %s, Port = %d\nRequest Description: delete the file: %s\n", sockIP, sockPort, arg4);
								}
								else if(strcmp(arg3, "X") == 0) {
									printf("Received request from IP = %s, Port = %d\nRequest Description: removal of all user's files and directories\n", sockIP, sockPort);
								}
							}

							sprintf(filename, "AS/USERS/%s/%s_reg.txt", arg1, arg1);
								
							if((fp = fopen(filename, "r")) == NULL){
								writeMessage(newfd, "RRQ EPD\n", 8);
								continue;
							}

							n = fread(buffer, 1, BUFFER_SIZE, fp);
							buffer[n] = '\0';

							if(fclose(fp) != 0) {
								perror("fclose()");
								exit(1);
							}

							char pdIP[32];
							char pdPort[8];
							sscanf(buffer, "%s %s", pdIP, pdPort);

							if((getaddrinfo(pdIP, pdPort, &pdhints, &pdres)) != 0) {
								writeMessage(newfd, "RRQ EPD\n", 8);
								continue;
							}

							VC = rand() % 10000;
							strcpy(RID, arg2);

							if(hasFname) {
								sprintf(buffer, "VLC %s %04d %s %s\n", arg1, VC, arg3, arg4); // VLC UID VC Fop [Fname]
								strcpy(FName, arg4);
							}
							else {
								sprintf(buffer, "VLC %s %04d %s\n", arg1, VC, arg3); // VLC UID VC Fop
							}

							strcpy(Fop, arg3);

							n = sendto(pdfd, buffer, strlen(buffer), 0, pdres->ai_addr, pdres->ai_addrlen);
							printf(buffer);

							if(n <= 0) {
								writeMessage(newfd, "RRQ EPD\n", 8);
								continue;
							}
							pdaddrlen = sizeof(pdaddr);
							n = recvfrom(pdfd, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&pdaddr, &pdaddrlen);
							buffer[n]= '\0';

							if(n <= 0) {
								writeMessage(newfd, "RRQ EPD\n", 8);
								continue;
							}

							sscanf(buffer, "RVC %s %s", arg1, arg2); // RVC UID status

							if(strcmp(arg2, "OK") == 0) {
								writeMessage(newfd, "RRQ OK\n", 7);
							}
							else {
								writeMessage(newfd, "RRQ EUSER\n", 10);
							}
						}
						else {
							writeMessage(newfd, "RRQ EFOP\n", 9);
						}
					}
					else if(strcmp(arg1, "AUT") == 0) {
						ret = sscanf(buffer, "AUT %s %s %s\n", arg1, arg2, arg3); // AUT UID RID VC

						potentialVC = strtol(arg3, NULL, 10);

						if(ret == 3 && strlen(arg1) == 5 && strlen(arg2) == 4) {
							if(userExists(arg1) && userIsLoggedIn(arg1) && strcmp(arg1, UID) == 0 && strcmp(arg2, RID) == 0 && potentialVC == VC) {
								TID = rand() % 10000;

								if(verbose){
									printf("Received Authetication command from IP = %s, Port = %d\nRequest Description: user:%s RID:%s VC:%s\n", sockIP, sockPort, arg1, arg2, arg3);
								}

								sprintf(filename, "AS/USERS/%s/%s_tid.txt", UID, UID);

								fp = fopen(filename, "w");

								if(fp == NULL) {
									perror("fopen()");
									exit(1);
								}

								if(strcmp(Fop, "L") == 0 || strcmp(Fop, "X") == 0) {
									sprintf(buffer, "%04d %s", TID, Fop);
								}
								else {
									sprintf(buffer, "%04d %s %s", TID, Fop, FName);
								}

								fwrite(buffer, 1, strlen(buffer), fp);

								if(fclose(fp) != 0) {
									perror("fclose()");
									exit(1);
								}

								sprintf(buffer, "RAU %04d\n", TID);

								writeMessage(newfd, buffer, strlen(buffer));
							}
							else {
								writeMessage(newfd, "RAU 0\n", 6);
							}
						}
						else {
							writeMessage(newfd, "RAU 0\n", 6);
						}
					}
					else {
						writeMessage(newfd, "ERR\n", 4);
					}
				}

				close(newfd);
				exit(0);
			}
			do {
				ret = close(newfd);
			} while(ret == -1 && errno == EINTR);

			if(ret == -1) {
				perror("close()");
				exit(1);
			}
		}

		// Deal with the stdin user input (exit)
		if(FD_ISSET(0, &fds)) {
            fgets(buffer, sizeof(buffer), stdin);
			buffer[4] = '\0';

			if(strcmp(buffer, "exit") == 0) {
				cycle = false;
				printf("Closing AS\n");
			}
		}
	}

	freeaddrinfo(asudpres);
	freeaddrinfo(astcpres);
	close(astcpfd);
	close(pdfd);
	close(asudpfd);
	exit(0);
}
