#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>
#include <dirent.h>

#pragma GCC diagnostic ignored "-Wformat-overflow"

#define DEFAULT_AS_PORT "58002"
#define DEFAULT_FS_PORT "59002"

#define BUFFER_SIZE 1024
#define AUX_SIZE 600
#define COMMAND_SIZE 4
#define UID_SIZE 6
#define TID_SIZE 5

#define PROTOCOL_ERROR_MESSAGE "Error: Command not supported"

#define bool int
#define true 1
#define false 0

#define max(A, B) ((A) >= (B)?(A):(B))

char *fsPort = NULL;
char *asIP = NULL;
char *asPort = NULL;

bool verbose = false;	// flag determing whether status messages are displayed on the terminal
bool cycle = true;

//  argument parsing function
static void parseArgs(long argc, char* const argv[]) {
    char c;

    fsPort = DEFAULT_FS_PORT;
    asIP = argv[1];
    asPort = DEFAULT_AS_PORT;

    while ((c = getopt(argc, argv, "q:n:p:v")) != -1){
        switch (c) {
            case 'q':
                fsPort = optarg;
                break;
            case 'n':
                asIP = optarg;
                break;
            case 'p':
                asPort = optarg;
                break;
			case 'v':
				verbose = true;
				break;
        }
    }
}

// writes the message to a given TCP socket and returns the total number of bytes written
long writeMessage(int fd, char *msg, long int msgSize) {
	ssize_t nleft, nwritten, ntotal = 0;
	char *ptr;
	ptr = msg;
	nleft = msgSize;

	while(nleft > 0) {
		nwritten = write(fd, ptr, nleft);

		if(nwritten <= 0) {
			perror("write()");
			exit(1);
		}
		ntotal += nwritten;
		nleft -= nwritten;
		ptr += nwritten;
	}

	return ntotal;
} 

// reads the message from a given TCP socket and returns the total number of bytes read
long readMessage(int fd, char *msg, long int msgSize) {
	ssize_t nleft, nread, ntotal = 0;
	char *ptr;
	ptr = msg;
	nleft = msgSize;

	while(nleft > 0) {
		nread = read(fd, ptr, nleft);

		if(nread == -1) {
			perror("read()");
			exit(1);
		}
		else if(nread == 0) {
			break;
		}

		nleft -= nread;
		ntotal += nread;
		if(ptr[nread-1] == '\n') {
			break;
		}
		ptr += nread;
	}

	ptr[nread] = '\0';

	return ntotal;
}

int main(int argc, char *argv[]) {
    int asfd, fsfd, newfd;
	char buffer[BUFFER_SIZE];
	char command[COMMAND_SIZE];
	struct sigaction act;
    struct sockaddr_in asaddr, fsaddr;
    struct addrinfo ashints, *asres, fshints, *fsres;
    socklen_t asaddrlen, fsaddrlen;
	act.sa_handler = SIG_IGN;
	pid_t pid;
	ssize_t n;
	int ret;

	// user specific
	DIR *d;
	FILE *fp;
	ssize_t nread;
	ssize_t nbytes;
	struct dirent *dir;
	char Fop;
	char *ptr;
	char fsize[16];
	char FName[32];
	char dirname[64];
	char TID[TID_SIZE] = "0000";
	char UID[UID_SIZE];
	char aux[AUX_SIZE]; // used for building the list buffer
	long size;
	int i = 0;
	bool dup = false;	// to check duplicated files

    parseArgs(argc, argv);

	memset(&act, 0, sizeof(act));

	if(sigaction(SIGCHLD, &act, NULL) == -1) {
		perror("sigaction()");
		exit(1);
	}

	// UDP client socket to communicate with AS
    asfd = socket(AF_INET,SOCK_DGRAM,0); 
    if(asfd == -1) {
		perror("UDP socket()");
        exit(1);
    }

	// TCP server socket, listens to user requests
	fsfd = socket(AF_INET, SOCK_STREAM, 0);
	if(fsfd == -1) {
		perror("TCP socket()");
		exit(1);
	}

    memset(&ashints, 0, sizeof(ashints));
    ashints.ai_family=AF_INET; // IPv4
    ashints.ai_socktype=SOCK_DGRAM; // UDP
	ashints.ai_flags=AI_PASSIVE;

	memset(&fshints, 0, sizeof(fshints));
    fshints.ai_family=AF_INET; // IPv4
    fshints.ai_socktype=SOCK_STREAM; // TCP
	fshints.ai_flags=AI_PASSIVE;

    if(getaddrinfo(asIP, asPort, &ashints, &asres) != 0) {
		perror("getaddrinfo()");
        exit(1);
	}

	if(getaddrinfo(NULL, fsPort, &fshints, &fsres) != 0) {
		perror("getaddrinfo()");
		exit(1);
	}

	if(bind(fsfd, fsres->ai_addr, fsres->ai_addrlen) == -1) {
        perror("bind()");
		exit(1);
	}

	// waits for TCP requests on given port
	if(listen(fsfd, 128) == -1) {
		perror("listen()");
		exit(1);
	}

    while (true) {
		fsaddrlen = sizeof(fsaddrlen);

		// waits for a new request to accept and creates a new child process
		do {
			newfd = accept(fsfd, (struct sockaddr*)&fsaddr, &fsaddrlen);
		} while(newfd == -1 && errno == EINTR);

		if(newfd == -1) {
			perror("accept()");
			exit(1);
		}

		if((pid=fork()) == -1) {
			perror("fork()");
			exit(1);
		}
		else if(pid == 0) {
			close(fsfd);

			char request[32];
			char sockIP[32];
			int sockPort;
			struct sockaddr_in addr;
			socklen_t addrsize = sizeof(struct sockaddr_in);

			// returns address info about the remote side of the connection
			if(getpeername(newfd, (struct sockaddr*)&addr, &addrsize) != 0) {
				perror("getpeername()");
				exit(1);
			}

			//get the IP and Port
			strcpy(sockIP, inet_ntoa(addr.sin_addr));
			sockPort = ntohs(addr.sin_port);

			// reads only the command from the user, the UID and TID
			n = readMessage(newfd, buffer, 15);

			if(n < 0) {
				perror("read()");
				break;
			}

			if(n != 15) {
				printf("Unexpected error\n");
				writeMessage(newfd, "ERR\n", 4);
				exit(1);
			}

			buffer[n-1] = '\0';

			sscanf(buffer, "%s %s %s", command, UID, TID);

			//variable request used in the verbose mode
			strcpy(request,command);

			// validates the size of UID and TID, and writes the appropriate command name to command
			if(strlen(UID) == 5 && strlen(TID) == 4) {
				bool hasFname = false;
				if(strcmp(command, "LST") == 0) {
					strcpy(command, "RLS");
				}
				else if(strcmp(command, "RTV") == 0) {
					hasFname = true;
					strcpy(command, "RRT");
				}
				else if(strcmp(command, "UPL") == 0) {
					hasFname = true;
					strcpy(command, "RUP");
				}
				else if(strcmp(command, "DEL") == 0) {
					hasFname = true;
					strcpy(command, "RDL");
				}
				else if(strcmp(command, "REM") == 0) {
					strcpy(command, "RRM");
				}
				else {
					writeMessage(newfd, "ERR\n", 4);
					break;
				}

				// reads the FName sent by the user application for comparison purposes
				char possibleFName[32];
				if(hasFname) {
					ptr = possibleFName;
					nbytes = 0;

					while(nbytes < 25) {
						nread = read(newfd, ptr, 1);

						if(nread == -1) {
							break;
						}
						else if(nread == 0) {
							break;
						}
						nbytes += nread;

						if(ptr[nread-1] == ' ' || ptr[nread-1] == '\n') {
							break;
						}
						ptr += nread;
					}

					possibleFName[nbytes-1] = '\0';

				}

				// prints to the console info about the incoming request
				if(verbose){
					if(strcmp(request, "LST") == 0) {
						printf("Received request from IP = %s, Port = %d\nRequest Description: list user's files\n", sockIP, sockPort);
					}
					else if(strcmp(request, "RTV") == 0) {
						printf("Received request from IP = %s, Port = %d\nRequest Description: retrieve the contents of the file: %s\n", sockIP, sockPort, possibleFName);
					}
					else if(strcmp(request, "UPL") == 0) {
						printf("Received request from IP = %s, Port = %d\nRequest Description: upload the file: %s\n", sockIP, sockPort, possibleFName);
					}
					else if(strcmp(request, "DEL") == 0) {
						printf("Received request from IP = %s, Port = %d\nRequest Description: delete the file: %s\n", sockIP, sockPort, possibleFName);
					}
					else if(strcmp(request, "REM") == 0) {
						printf("Received request from IP = %s, Port = %d\nRequest Description: removal of all user's files and directories\n", sockIP, sockPort);
					}
				}

				// validates the operation with AS
				sprintf(buffer, "VLD %s %s\n", UID, TID);
				
				n = sendto(asfd, buffer, strlen(buffer), 0, asres->ai_addr, asres->ai_addrlen);

				if(n < 0) {
					perror("sendto()");
					break;
				}
				
				n = recvfrom(asfd, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&asaddr, &asaddrlen);

				if(n < 0) {
					perror("recvfrom()");						
					break;
				}

				buffer[n] = '\0';

				sscanf(buffer, "CNF %s %s %c %s", UID, TID, &Fop, FName);
				
				// if the file name is not the same, change Fop value so an appropriate error is sent
				if(hasFname && strcmp(FName, possibleFName) != 0) {
					Fop = 'E';
				}

				sprintf(dirname, "FS/USERS/%s", UID);

				if (Fop == 'L') {
					d = opendir(dirname);
					
					if(d) {
						i = 0; // counts files
						strcpy(aux, "");
						while((dir = readdir(d)) != NULL) {
							i++;
							// skips current directroy and parent directroy "files"
							if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
								i--;
								continue;
							}

							// opens file to get file size
							sprintf(buffer, "%s/%s", dirname, dir->d_name);
							fp = fopen(buffer, "rb");

							if(fp == NULL) {
								perror("fopen()");
								break;
							}

							if(fseek(fp, 0, SEEK_END) < 0) {
								perror("fseek()");
								break;
							}

							size = ftell(fp);

							if(size < 0) {
								perror("ftell()");
								break;
							}
							if(fseek(fp, 0, SEEK_SET) < 0) {
								perror("fseek()");
								break;
							}

							if(fclose(fp) < 0) {
								perror("fclose()");
								break;
							}

							sprintf(buffer, " %s %ld", dir->d_name, size);
							strcat(aux, buffer); // builds the output string
						}

						if(i > 0) {
							sprintf(buffer, "RLS %d%s\n", i, aux);
						}
						else {
							sprintf(buffer, "RLS EOF\n");
						}
					}
					else {
						sprintf(buffer, "RLS EOF\n");
					}

					writeMessage(newfd, buffer, strlen(buffer));
				}
				else if(Fop == 'R') {
					d = opendir(dirname);
					fp = NULL;
			
					if(d) {
						sprintf(buffer, "%s/%s", dirname, FName);
						fp = fopen(buffer, "rb");
						if(fp == NULL) {
							sprintf(buffer, "RRT EOF\n");
							writeMessage(newfd, buffer, strlen(buffer));
							break;
						}

						if(fseek(fp, 0, SEEK_END) < 0) {
							perror("fseek()");
							break;
						}

						size = ftell(fp);

						if(size < 0) {
							perror("ftell()");
							break;
						}

						if(fseek(fp, 0, SEEK_SET) < 0) {
							perror("fseek()");
							break;
						}

						sprintf(buffer, "RRT OK %ld ", size);

						writeMessage(newfd, buffer, strlen(buffer));

						nbytes = 0;
						nread = 0;
	
						// sends the file to the user while reading it
						while(nbytes < size) {
							nread = fread(buffer, 1, BUFFER_SIZE, fp);
							nbytes += writeMessage(newfd, buffer, nread);
							bzero(buffer, BUFFER_SIZE);
						}

						writeMessage(newfd, "\n", 1);

						if(fclose(fp) < 0) {
							perror("fclose()");
							break;
						}
					}
					else {
						sprintf(buffer, "RRT NOK\n");
						writeMessage(newfd, buffer, strlen(buffer));
					}
				}
				else if(Fop == 'D') {
					d = opendir(dirname);
					fp = NULL;

					if(d) {
						sprintf(buffer, "%s/%s", dirname, FName);

						if(remove(buffer) == 0) {
							sprintf(buffer, "RDL OK\n");
						}
						else {
							sprintf(buffer, "RDL EOF\n");
						}
					}
					else{
						sprintf(buffer, "RDL NOK\n");
					}

					writeMessage(newfd, buffer, strlen(buffer));
				}
				else if(Fop == 'U') {
					d = opendir(dirname);

					if(!d){
						mkdir(dirname, 0777);
						d = opendir(dirname);
					}

					i = 0; // used to determine the number of files for RUP FULL
					dup = false;

					// checks if the folder has space or if the file is duplicated
					while((dir = readdir(d)) != NULL) {
						if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
							continue;
						}
						if(strcmp(dir->d_name, FName) == 0){
							dup = true;
							break;
						}
						i++;
					}

					if(dup) {
						writeMessage(newfd, "RUP DUP\n", (long)8);
						break;
					}

					if(i >= 15){
						writeMessage(newfd, "RUP FULL\n", (long)9);
						break;
					}
					
					sprintf(buffer, "%s/%s", dirname, FName);

					fp = fopen(buffer, "wb");

					if(fp == NULL) {
						perror("fopen()");
						break;
					}

					ptr = fsize;
					nbytes = 0;

					// reads the file size 1 by 1 byte (since its number of digits can vary)
					while(true) {
						nread = read(newfd, ptr, 1);

						if(nread == -1) {
							break;
						}
						else if(nread == 0) {
							break;
						}
						nbytes += nread;

						// stops reading at the space
						if(ptr[nread-1] == ' ') {
							break;
						}
						ptr += nread;
					}

					fsize[nbytes] = '\0';

					// if file is too big. No specific error message
					if(strlen(fsize) > 10) {
						writeMessage(newfd, "RUP ERR\n", (long)8);
						break;
					}

					size = strtol(fsize, NULL, 10);
					nbytes = 0;

					// reads the file bytes until they reach or exceed file size (since buffer "trash" can be sent)
					while(nbytes < size) {
						nread = read(newfd, buffer, BUFFER_SIZE);
						nbytes += nread;
						if(nread == 0) {
							break;
						}

						/*if the socket read too much (or read "trash"), adjusts nread value so only 
						the correct number of bytes is written to local file copy */
						if(nbytes >= size) {
							nread -= (nbytes - size); 
						}

						if(fwrite(buffer, 1, nread, fp) < 0) {
							perror("fwrite()");
							break;
						}

						bzero(buffer, BUFFER_SIZE);
					}
					
					fclose(fp);

					writeMessage(newfd, "RUP OK\n", 7);
				}
				else if(Fop == 'X') {
					d = opendir(dirname);

					if(d) {
						bool ok = true;
						
						// deletes all files except . and ..
						while((dir = readdir(d)) != NULL) {
							if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
								continue;
							}

							sprintf(buffer, "%s/%s", dirname, dir->d_name);

							if(remove(buffer) != 0) {
								ok = false;
							}
						}

						if(ok) {
							if(rmdir(dirname) != 0) {
								perror("rmdir()");
								break;
							}

							writeMessage(newfd, "RRM OK\n", (long) 7);
						}
						else {
							writeMessage(newfd, "RRM ERR\n", (long)8);
						}
					}
					else {
						writeMessage(newfd, "RRM OK\n", (long)7);
					}
				}
				else if(Fop == 'E') {
					sprintf(buffer, "%s INV\n", command);
					writeMessage(newfd, buffer, strlen(buffer));
				}
				else { // wrong format
					sprintf(buffer, "%s ERR\n", command);
					writeMessage(newfd, buffer, strlen(buffer));
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

	// frees and closes
    freeaddrinfo(asres);
	freeaddrinfo(fsres);
    close(asfd);
	close(fsfd);
    exit(0);
}