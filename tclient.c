#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include<netdb.h>
#include <netinet/in.h>
#define FILETYPE_REQ 0xea // file-type request
#define FILETYPE_RSP 0xe9 // successful file-type response
#define FILETYPE_ERR 0xe8 // failed file-type response
#define CHECKSUM_REQ 0xca // file checksum request
#define CHECKSUM_RSP 0xc9 // successful checksum response
#define CHECKSUM_ERR 0xc8 // failed checksum response
#define DOWNLOAD_REQ 0xaa // download file request
#define DOWNLOAD_RSP 0xa9 // successful download response
#define DOWNLOAD_ERR 0xa8 // failed download response
#define UNKNOWN_FAIL 0x51 // catch-all failure response
#define MAX_PACKET_SIZE 4096 //max size of a packet frame
#define PACKET_RESERVE_SIZE 4

//tclient [hostname:]port filetype [-udp] filename
//tclient [hostname:]port checksum [-udp] [-o offset] [-l length] filename
//tclient [hostname:]port download [-udp] [-o offset] [-l length] filename [saveasfilename]

/*------------------------------------------------------------------------------*/ 
void writeInt32(char* buf, int val);
int readInt32(const char* buf);
int tsend(const char* src, long length);
int trecv(char* dst, long max_length);
int readData(char* buf, int length);
int readMsg(char* buf);
void TCPconnect();
void UDPconnect();
void setConnectMode();
int download(int argc,char* argv[]);
int checksum(int argc,char* argv[]);
int filetype(int argc,char* argv[]);

static char hostname[256] = "localhost";
static int port;
static char cmd[256];

int main(int argc,char* argv[]){
	char* tmp;
	if (argc < 4) {
		fprintf(stderr, "error: lack enough parameters!");
		return 1;
	}
	if (tmp = strrchr(argv[1], ':')) {
		*tmp = '\0';
		strcpy(hostname, argv[1]);
		argv[1] = tmp + 1;
	}
	if (!(port = atoi(argv[1]))) {
		fprintf(stderr, "error: illegal port!");
		return 1;
	}
	strcpy(cmd, argv[2]);
	if (strcmp("filetype", argv[2]) == 0) {
		filetype(argc - 3, &argv[3]);
	} else if (strcmp("checksum", argv[2]) == 0) {
		checksum(argc - 3, &argv[3]);
	} else if (strcmp("download", argv[2]) == 0) {
		download(argc - 3, &argv[3]);
	} else {
		fprintf(stderr, "error: illegal command!");
		return 1;
	}
    return 0;
}
/*------------------------------------------------------------------------------*/ 

static int udp = 0;
static char filename[256];
static int offset = 0;
static int length = -1;
static char saveasfilename[256];
static int sock_fd;
static struct sockaddr_in server_addr;
static int packet_seq = 123;

void writeInt32(char* buf, int val) {
	memcpy(buf, (const char*) &val, 4);
}

int readInt32(const char* buf) {
	int val;
	memcpy(&val, buf, 4);
	return val;
}

int tsend(const char* src, long length) {
	char packet[MAX_PACKET_SIZE];
	if (udp) {
		writeInt32(packet, htonl(packet_seq++));
		writeInt32(packet + 4, htonl(0));
		memcpy(packet + 4 + PACKET_RESERVE_SIZE, src, length);
		return sendto(sock_fd, packet, length + 4 + PACKET_RESERVE_SIZE, 0,	(struct sockaddr*) &server_addr, sizeof(server_addr));
	} else {
		return send(sock_fd, src, length, 0);
	}
}

int trecv(char* dst, long max_length) {
	char packet[MAX_PACKET_SIZE];
	struct sockaddr_in peer;
	socklen_t addrlen = sizeof peer;
	int ret;
	if (udp) {
		ret = recvfrom(sock_fd, packet, MAX_PACKET_SIZE, 0, (struct sockaddr*) &peer, &addrlen);
		if (addrlen != sizeof(peer) || memcmp((const void*) &peer, (const void*) &server_addr, addrlen) != 0) {
			return -1;
		}		
		if (ret < 4 + PACKET_RESERVE_SIZE) {
			return -1;
		} 
		//ACK
		sendto(sock_fd, packet, 4 + PACKET_RESERVE_SIZE, 0, (struct sockaddr*) &server_addr, sizeof(server_addr));
		ret -= 4 + PACKET_RESERVE_SIZE;
		memcpy(dst, packet + 4 + PACKET_RESERVE_SIZE, ret);
		return ret;
	} else {
		return recv(sock_fd, dst, max_length, 0);
	}
}

int readData(char* buf, int length) {
	int n, read;
	while (length > 0) { 
		n = trecv(buf, length);
		if (n < 1) {
			return -1;
		} 
		length -= n;
		buf += n;
	}
	return 0;
}

//read a complete messge
int readMsg(char* buf){
	int n, read, len;
	if (udp) {
		return trecv(buf, MAX_PACKET_SIZE) < 0;
	} else {
		if (readData(buf, 5)) {
			return -1;
		} 
		len = ntohl(readInt32(buf + 1));
		return readData(buf + 5, len);
	}
}

void UDPconnect(){
	struct hostent* host; 
	sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	bzero(&server_addr, sizeof(struct sockaddr_in));
	if ((host = gethostbyname(hostname)) == NULL) {   
		fprintf(stderr, "fail to get host ip !\n");
		exit(1);   
    }
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = *(in_addr_t*) *host->h_addr_list;
	server_addr.sin_port = htons(port);
}

void TCPconnect(){
	struct hostent *host;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	bzero(&server_addr, sizeof(struct sockaddr_in));
	if ((host = gethostbyname(hostname)) == NULL) {   
		fprintf(stderr,"fail to get host ip !\n");
		exit(1);   
    }
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = *(in_addr_t*) *host->h_addr_list;
	server_addr.sin_port = htons(port);
	if(connect(sock_fd, (struct sockaddr*) (&server_addr), sizeof(struct sockaddr)) == -1) {
		fprintf(stderr, "fail to connect server!\n");
		exit(1);   
	}
}

void setConnectMode() {
	if(udp) {
		UDPconnect();
	} else {
		TCPconnect();
	}
}

int filetype(int argc, char* argv[]) {
	int ret;
	int len;
	int k;
	char buff[MAX_PACKET_SIZE] = {0};  
	if(argc > 2) {
		fprintf(stderr, "error: too much parameters\n");
		return 1;
	}
	if(argc > 1) { 
		if(strcmp("-udp", argv[0]) != 0) {
			fprintf(stderr, "error: unknown parameter\n");
			return 1;
		}
		udp = 1;
		++argv;
	}
	strcpy(filename, argv[0]);
	setConnectMode();
	buff[0] = (char) FILETYPE_REQ;
	writeInt32(buff + 1, htonl(strlen(filename)));
	strcpy(buff + 5, filename);
	ret = tsend(buff, 5 + strlen(filename));
	if (readMsg(buff)) {
		fprintf(stderr, "fail to get response from server!\n");
	} else {
		if ((0xff & buff[0]) == FILETYPE_RSP) {
			len = ntohl(readInt32(buff + 1));
			buff[5 + len] = '\0';
			for (k = 0; k < len; k++) {			
				if((0xff & buff[5 + k]) > 0x7f) {break;}
			}
			if (k < len) {
				fprintf(stdout, "Invalid characters detected in a FILETYPE_RSP message.\n");
			} else {
				fprintf(stdout, "%s\n", &buff[5]);
			}
		} else if ((0xff & buff[0]) == FILETYPE_ERR) {
			fprintf(stdout, "FILETYPE_ERR received from the server\n");
		}
	}
	close(sock_fd);
	return 0;
}

int checksum(int argc, char*argv[]) {
	char buff[MAX_PACKET_SIZE] = {0};  
	int k, ret, len;
	if(argc > 6) {
		fprintf(stderr, "error: too much parameters\n");
		return 1;
	}
	for (k = 0; k < argc; ++k) {
		if(strcmp("-udp", argv[k]) == 0) {
			udp = 1;
		} else if (strcmp("-o", argv[k]) == 0) {
			if (k + 1 == argc) {
				fprintf(stderr, "error: need offset\n");
				return 1;
			}
			if ((offset = atoi(argv[++k])) < 0) {
				fprintf(stderr, "error: illegal offset!\n");
				return 1;
			}
		} else if (strcmp("-l", argv[k]) == 0) {
			if (argc == k + 1) {
				fprintf(stderr, "error: need length\n");
				return 1;
			}
			if (!(length = atoi(argv[++k]))) {
				fprintf(stderr, "error: illegal length!\n");
				return 1;
			}
		} else {
			strcpy(filename, argv[k]);
		}
	}
	setConnectMode();
	buff[0] = (char)CHECKSUM_REQ;
	writeInt32(buff + 1, htonl(8 + strlen(filename)));
	writeInt32(buff + 5, htonl(offset));
	writeInt32(buff + 9, htonl(length));
	strcpy(buff + 13, filename);
	ret = tsend(buff, 13 + strlen(filename));
	if(readMsg(buff)) {
		fprintf(stderr, "fail to get response from server!\n");
	} else {
		if ((0xff & buff[0]) == CHECKSUM_RSP) {
			len = ntohl(readInt32(buff + 1));
			if (len != 16) {
				fprintf(stdout,"Invalid DataLength detected in a CHECKSUM_RSP message.\n");
			} else {
				for (k = 0; k < 16; k++) {
					fprintf(stdout, "%02x", (0xff & buff[5 + k]));
				}
				fprintf(stdout, "\n");
			}
		} else if ((0xff & buff[0]) == CHECKSUM_ERR) {
			fprintf(stdout, "CHECKSUM_ERR received from the server\n");
		}
	}
	close(sock_fd);
	return 0;
}

int download(int argc, char* argv[]) {
	char buff[MAX_PACKET_SIZE] = {0};
	int k, ret, len;
	FILE* pf;
	if (argc > 7) {
		fprintf(stderr, "error: too much parameters\n");
		return 1;
	}
	for (k = 0; k < argc; ++k) {
		if (strcmp("-udp", argv[k]) == 0) {
			udp = 1;
		} else if (strcmp("-o", argv[k]) == 0) {
			if (argc == k + 1) {
				fprintf(stderr, "error: need offset\n");
				return 1;
			}
			if (!(offset = atoi(argv[++k]))) {
				fprintf(stderr, "error: illegal offset!\n");
				return 1;
			}
		} else if (strcmp("-l", argv[k]) == 0) {
			if (argc == k + 1) {
				fprintf(stderr, "error: need length\n");
				return 1;
			}
			if (!(length = atoi(argv[++k]))) {
				fprintf(stderr, "error: illegal length!\n");
				return 1;
			}
		} else if (!filename[0]) {
			strcpy(filename, argv[k]);
		} else {
			strcpy(saveasfilename, argv[k]);
		}
	}
	setConnectMode();
	buff[0] = (char) DOWNLOAD_REQ;
	writeInt32(buff + 1, htonl(8 + strlen(filename)));
	writeInt32(buff + 5, htonl(offset));
	writeInt32(buff + 9, htonl(length)); 
	strcpy(buff + 13, filename);
	ret = tsend(buff, 13 + strlen(filename));
	//get head of reponse
	if (readData(buff, 5)) {
		//fail to get the head
		fprintf(stderr, "fail to get response from server!\n");
	} else {
		if((0xff & buff[0]) == DOWNLOAD_RSP) {//successful reponse
			len = ntohl(readInt32(buff + 1));//data length
			if (NULL == (pf = fopen(saveasfilename, "wb"))) {
				fprintf(stderr, "fail to open save file %s!\n", saveasfilename);
			} else {
				//download file
				while (len > 0) {
					k = len > sizeof(buff) ? sizeof(buff) : len;
					if ((k = trecv(buff, sizeof(buff))) < 1) {
						fprintf(stderr, "fail to receive data from server!\n");
						break;
					} else {
						fwrite(buff, 1, k, pf);
					}
					len -= k;
				}
				fclose(pf);
				fprintf(stdout, "...Downloaded data have been successfully written into '%s'\n", saveasfilename);
			}
		} else if ((0xff & buff[0]) == DOWNLOAD_ERR) {
			fprintf(stdout, "DOWNLOAD_ERR received from the server\n");
		}
	}
	close(sock_fd);
	return 0;
}
