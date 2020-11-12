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

#define DEFAULT_AS_PORT "58002"
#define DEFAULT_FS_PORT "59002"

#define DEFAULT_IP "127.0.0.1"

#define NOK_MESSAGE "Error: User ID does not exist"
#define ERR_MESSAGE "Error: Operation failed"
#define TID_MESSAGE "Error: AS Authentication Error"
#define EOF_MESSAGE "Error: File is not available"
#define ELOG_MESSAGE "Error: There was no established login"
#define EPD_MESSAGE "Error: AS could not communicate with PD"
#define EUSER_MESSAGE "Error: Provided UID is incorrect"
#define EFOP_MESSAGE "Error: Provided file operation is non existent"
#define PROTOCOL_ERROR_MESSAGE "Error: Command not supported"

#define MESSAGE_SIZE 512

#define bool int
#define true 1
#define false 0

char *asIP = NULL;
char *asPort = NULL;
char *fsIP = NULL;
char *fsPort = NULL;

//  argument parsing function
void parseArgs(long argc, char* const argv[]) {
	char c;

    asIP = DEFAULT_IP;
    asPort = DEFAULT_AS_PORT;
    fsIP = DEFAULT_IP;
    fsPort = DEFAULT_FS_PORT;

    while ((c = getopt(argc, argv, "n:p:m:q:")) != -1) {
        switch (c) {
            case 'n':
                asIP = optarg;
                break;
            case 'p':
                asPort = optarg;
                break;
            case 'm':
                fsIP = optarg;
                break;
			case 'q':
                fsPort = optarg;
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
			exit(1);
		}
		ntotal += nwritten;
		nleft -= nwritten;
		ptr += nwritten;
	}

	return ntotal;
} 

// reads the message from a given TCP socket and returns the total number of bytes read
long readMessage(int fd, char *msg) {
	ssize_t nleft, nread, ntotal = 0;
	char *ptr;
	ptr = msg;
	nleft = MESSAGE_SIZE;

	while(nleft > 0) {
		nread = read(fd, ptr, nleft);

		if(nread == -1) {
			exit(1);
		}
		else if(nread == 0) {
			break;
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

int main(int argc, char *argv[]) {
	// connection vars
	struct addrinfo ashints, fshints, *asres, *fsres;
	int n;
	int RID = -1;
	int asfd, fsfd;
	char UID[6], pass[9];

	// input vars
	char line[256];
	char command[128];
	char message[MESSAGE_SIZE];
	char arg1[128], arg2[128];
	int c;

	bool logged = false;	// controls whether or not there is a logged user
	bool cycle = true;

	//command vars
	char Fname[25];
	char TID[5];
	char Fop;
	int ret;

	parseArgs(argc, argv);

	asfd = socket(AF_INET, SOCK_STREAM, 0); // AS Socket TCP

    memset(&ashints, 0, sizeof(ashints));
    ashints.ai_family=AF_INET; // IPv4
    ashints.ai_socktype=SOCK_STREAM; // TDP
	ashints.ai_flags=AI_CANONNAME;

	memset(&fshints, 0, sizeof(fshints));
    fshints.ai_family=AF_INET; // IPv4
    fshints.ai_socktype=SOCK_STREAM; // TDP
	fshints.ai_flags=AI_CANONNAME;


	if(getaddrinfo(asIP, asPort, &ashints, &asres) != 0) {
		perror("getaddrinfo()");
        exit(1);
    }	

	if(getaddrinfo(fsIP, fsPort, &fshints, &fsres) != 0) {
		perror("getaddrinfo()");
        exit(1);
	}

	n = connect(asfd, asres->ai_addr, asres->ai_addrlen);
	if(n == -1) {
		perror("connect()");
		exit(1);
	}

	while(cycle) {
		// reads user input from stdin and extracts it
		fgets(line, sizeof(line), stdin);
		c = sscanf(line, "%s %s %s", command, arg1, arg2);

		if(strcmp(command, "login") == 0 && c == 3 && strlen(arg1) == 5 && strlen(arg2) == 8) {
			// prevents the user from doing several logins
			if(logged) {
				puts("You are already logged in!");
				continue;
			}
			sprintf(message, "LOG %s %s\n", arg1, arg2);

			writeMessage(asfd, message, strlen(message));
			readMessage(asfd, message);

			// AS accepted login. Setting correct variable values for future commands
			if(strcmp(message, "RLO OK\n")==0) {
				strcpy(UID, arg1);
				strcpy(pass, arg2);
				logged = true;
				puts("You are now logged in");
			}
			else {
				puts("Login failed!");
			}
		}
		else if(strcmp(command, "req") == 0 && logged) {
			RID = rand() % 10000;

			if(strlen(arg1) == 1) {
				// extracts input accordingly by checking if the given Fop requires a file name
				if((strcmp(arg1, "R") == 0 || strcmp(arg1, "U") == 0 || strcmp(arg1, "D") == 0) && strlen(arg2) <= 25 && c == 3) {
					sprintf(message, "REQ %s %04d %s %s\n", UID, RID, arg1, arg2);
					writeMessage(asfd, message, strlen(message));
				}
				else if(c == 2){
					sprintf(message, "REQ %s %04d %s\n", UID, RID, arg1);
					writeMessage(asfd, message, strlen(message));
				}
				else {
					puts(ERR_MESSAGE);
					continue;
				}

				readMessage(asfd, message);
				
				if(strcmp(message, "RRQ OK\n") == 0) {
					Fop = arg1[0];
					if((Fop == 'R' || Fop == 'U' || Fop == 'D') && c == 3) {
						strcpy(Fname, arg2); // REQ went okay, setting variable value for future commands
					}
				}
				else if(strcmp(message, "RRQ ELOG\n") == 0) {
					puts(ELOG_MESSAGE);
				}
				else if(strcmp(message, "RRQ EPD\n") == 0) {
					puts(EPD_MESSAGE);
				}
				else if(strcmp(message, "RRQ EUSER\n") == 0) {
					puts(EUSER_MESSAGE);
				}
				else if(strcmp(message, "RRQ EFOP\n") == 0) {
					puts(EFOP_MESSAGE);
				}
				else {
					puts(ERR_MESSAGE);
				}
			}
			else {
				puts(ERR_MESSAGE);
			}
		}
		else if(strcmp(command, "val") == 0 && logged) {
			// checks if a REQ was previously done
			if(RID == -1) {
				puts("No request has been made");
				continue;
			}
			sprintf(message, "AUT %s %04d %s\n", UID, RID, arg1);
			writeMessage(asfd, message, strlen(message));

			readMessage(asfd, message);
			ret = sscanf(message, "%s %s", arg1, arg2);

			// checks if the AS accepted the validation codes
			if(ret == 2 && strcmp(arg2, "0") != 0){
				strcpy(TID, arg2);
				printf("Authenticated! (TID=%s)\n", TID);		
			}
			else{
				puts("Authentication Failed!");
			}
		}
		else if((strcmp(command, "list") == 0 || strcmp(command, "l") == 0) && logged) {
			int i = 0;
			fsfd = socket(AF_INET, SOCK_STREAM, 0);

			n = connect(fsfd, fsres->ai_addr, fsres->ai_addrlen);

			if(n == -1) {
				perror("connect()");
				exit(1);
			}
			sprintf(message, "LST %s %s\n", UID, TID);

			writeMessage(fsfd, message, strlen(message));
			readMessage(fsfd, message);

			if(strcmp(message, "RLS NOK\n") == 0){
				puts(NOK_MESSAGE);
			}
			else if(strcmp(message, "RLS EOF\n") == 0){
				puts("No files available");
			}
			else if(strcmp(message, "RLS INV\n") == 0){
				puts(TID_MESSAGE);
			}
			else if(strcmp(message, "RLS ERR\n") == 0){
				puts(ERR_MESSAGE);
			}
			else {
				i = 1;
				int nfiles;
				char *p;
				
				// reads the number of files sent to list 
				p = strtok(message, " ");
				p = strtok(NULL, " ");
				nfiles = strtol(p, NULL, 10);
				p = strtok(NULL, " ");

				// reads file size and then file name for each pair of file size and name
				for(; p != NULL; p = strtok(NULL, " ")) {
					printf("%d - ", i);
					printf("%s ", p);
					p = strtok(NULL, " ");
					if(i == nfiles) {
						printf("%s", p);
					}
					else {
						printf("%s \n", p);
					}
					i++;
				}
			}

			close(fsfd);
		}
		else if((strcmp(command, "retrieve") == 0 || strcmp(command, "r") == 0) && logged) {
				if(strcmp(arg1, Fname) != 0) {
				puts("File differs from request file");
				continue;
			}
			fsfd = socket(AF_INET, SOCK_STREAM, 0);

			n = connect(fsfd, fsres->ai_addr, fsres->ai_addrlen);
			if(n == -1) {
				exit(1);
			}

			sprintf(message, "RTV %s %s %s\n", UID, TID, arg1);
			writeMessage(fsfd, message, strlen(message));

			ssize_t nleft = 7, nread, ntotal = 0;
			char *ptr = message;

			//  
			while(nleft > 0) {
				nread = read(fsfd, ptr, nleft);

				if(nread == -1) {
					exit(1);
				}
				else if(nread == 0) {
					break;
				}
				ntotal += nread;
				nleft -= nread;
				ptr += nread;
			}

			message[ntotal] = '\0';

			sscanf(message, "%s %s", command, message);

			if(strcmp(message, "OK") == 0){
				char fsize[11];
				long size;

				char *ptr = fsize;
				ntotal = 0;
				nleft = 11;

				// reads one by one bytes to find file size
				while(1) {
					nread = read(fsfd, ptr, 1);

					if(nread == -1) {
						exit(1);
					}
					else if(nread == 0) {
						break;
					}
					nleft -= nread;
					ntotal += nread;

					if(ptr[nread-1] == ' ') {
						break;
					}
					ptr += nread;
				}

				fsize[ntotal] = '\0';

				size = strtol(fsize, NULL, 10);
				long nbytes = 0;

				FILE *fp = fopen(Fname, "wb");

				// reads the file bytes until they reach or exceed file size
				while(nbytes < size) {
					nread = read(fsfd, message, MESSAGE_SIZE);
					nbytes += nread;

					/*if the socket read too much (or read"trash"), adjusts nread value so only 
					the correct number of bytes is written to local file copy */
					if(nbytes >= size) {
						nread -= (nbytes - size);
					}

					if(fwrite(message, 1, nread, fp) < 0) {
						perror("fwrite()");
						exit(1);
					}

					bzero(message, MESSAGE_SIZE);
				}

				fclose(fp);

				puts("File retrieve succeeded");
			}
			else if(strcmp(message, "NOK") == 0){
				puts(NOK_MESSAGE);
			}
			else if(strcmp(message, "INV") == 0){
				puts(TID_MESSAGE);
			}
			else if(strcmp(message, "EOF") == 0){
				puts(EOF_MESSAGE);
			}
			else{
				puts(ERR_MESSAGE);
			}

			close(fsfd);
		}
		else if((strcmp(command, "upload") == 0 || strcmp(command, "u") == 0) && logged) {
			if(strcmp(arg1, Fname) != 0) {
				puts("File differs from request file");
				continue;
			}

			fsfd = socket(AF_INET, SOCK_STREAM, 0);

			n = connect(fsfd, fsres->ai_addr, fsres->ai_addrlen);
			if(n == -1) {
				perror("connect()");
				exit(1);
			}

			sprintf(message, "UPL %s %s %s ", UID, TID, arg1);

			FILE *fp = fopen(Fname, "rb");

			if(fp == NULL) {
				puts("File not available");
			}
			else {
				writeMessage(fsfd, message, strlen(message));
				
				// finds file size
				fseek(fp, 0, SEEK_END);
				long size = ftell(fp);
				fseek(fp, 0, SEEK_SET);
				sprintf(message, "%ld ", size);

				// 
				writeMessage(fsfd, message, strlen(message));
				long nbytes = 0;
				
				// reads until the number of bytes read exceeds file size
				while(nbytes < size) {
					n = fread(message, 1, MESSAGE_SIZE, fp);
					nbytes += writeMessage(fsfd, message, n);
					bzero(message, MESSAGE_SIZE);
				}

				writeMessage(fsfd, "\n", 1);

				fclose(fp);

				readMessage(fsfd, message);

				if(strcmp(message,"RUP OK\n") == 0) {
					printf("Success uploading %s\n",Fname);
				}
				else if(strcmp(message,"RUP DUP\n") == 0) {
					printf("File %s already exists\n",Fname);
				}
				else if(strcmp(message,"RUP FULL\n") == 0) {
					printf("File limit exceeded\n");
				}
				else if(strcmp(message,"RUP INV\n") == 0) {
					puts(TID_MESSAGE);
				}
				else {
					puts("Upload failed");
				}
			}
			close(fsfd);
		}
		else if((strcmp(command, "delete") == 0 || strcmp(command, "d") == 0) && logged) {
			if(strcmp(arg1, Fname) != 0) {
				puts("File differs from request file");
				continue;
			}

			fsfd = socket(AF_INET, SOCK_STREAM, 0);

			n = connect(fsfd, fsres->ai_addr, fsres->ai_addrlen);
			if(n == -1) {
				exit(1);
			}

			sprintf(message, "DEL %s %s %s\n", UID, TID, arg1);
			writeMessage(fsfd, message, strlen(message));
			readMessage(fsfd, message);

			if(strcmp(message, "RDL OK\n") == 0){
				puts("Operation succeeded");
			}
			else if(strcmp(message, "RDL NOK\n") == 0){
				puts(NOK_MESSAGE);
			}
			else if(strcmp(message, "RDL EOF\n") == 0){
				puts(EOF_MESSAGE);
			}
			else if(strcmp(message, "RDL INV\n") == 0){
				puts(TID_MESSAGE);
			}
			else{
				puts(ERR_MESSAGE);
			}

			close(fsfd);
		}
		else if((strcmp(command, "remove") == 0 || strcmp(command,"x") == 0) && logged) {
			fsfd = socket(AF_INET, SOCK_STREAM, 0);

			n = connect(fsfd, fsres->ai_addr, fsres->ai_addrlen);
			if(n == -1) {
				exit(1);
			}

			sprintf(message, "REM %s %s\n", UID, TID);
			
			writeMessage(fsfd, message, strlen(message));
			readMessage(fsfd, message);

			if(strcmp(message, "RRM OK\n") == 0){
				puts("Operation succeeded");
				logged = false;
				cycle = false;
			}
			else if(strcmp(message, "RRM NOK\n") == 0){
				puts(NOK_MESSAGE);
			}
			else if(strcmp(message, "RRM INV\n") == 0){
				puts(TID_MESSAGE);
			}
			else{
				puts(ERR_MESSAGE);
			}	
		}
		else if(strcmp(command, "exit") == 0) {
			logged = false;
			cycle = false;
		}
		else {
			puts(PROTOCOL_ERROR_MESSAGE);
		}
	}

	// frees and closes
    freeaddrinfo(asres);
	freeaddrinfo(fsres);
	close(asfd);
	close(fsfd);
	exit(0);
}