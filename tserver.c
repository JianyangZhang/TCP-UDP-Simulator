#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <openssl/md5.h>
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
#define MAXLINE 1024

// tserver [-udp loss_model] [-w window] [-r msinterval] [-d] [-t seconds] port

/*------------------------------------------------------------------------------*/ 
long getFileLen(FILE* pf);
void setLossMode();
int nextLossBit();
const char* strToHex(const char* bytes);
void writeInt32(char* buf, int val);
int readInt32(const char* buf);
void autoShutdown(int sig);
int validFileName(const char* file);
int filetype(const char* file, char* out);
int checksum(const char* file, int offset, int length, unsigned char md5_sum[]);
FILE* download(const char* file, int offset, int* length);
void ack_or_retransmission();
int usend(const char* src, long length);
int tsend(const char* src, long length);
int trecv(char* dst, long max_length);
int readData(char* buf, int length);
int readMsg(char* buf);
void respFiletype();
void respChecksum();
void respDownload();
void TCPserver();
void UDPserver();
void setServeMode();
int taccept();
void serve();
void parseArg(int argc, char* argv[]);

static int shutdown_time = 300;

int main(int argc, char* argv[]) {
        parseArg(argc, argv);
        signal(SIGALRM, autoShutdown);
        alarm(shutdown_time);
        serve();
        return 0;
    }
    /*------------------------------------------------------------------------------*/

struct Data_Packet {
    char* data;
    int length;
    int empty;
}

// UDP variables 
static int udp = 0;
static char loss_model[256];
static int window_size = 3;
static int msinterval = 250;
static struct Data_Packet * packets;
static FILE* pf_loss;
static unsigned char byte;
static int bit = 0;

static int packet_seq = 100001; // packet sequence number
static char buff[MAX_PACKET_SIZE];
static int debug_mode = 0;
static int port;
static int socket_fd;
static int connect_fd;
static struct sockaddr_in servaddr;
static struct sockaddr_in clientaddr;

long getFileLen(FILE* pf) {
    fseek(pf, 0, SEEK_END);
    return ftell(pf);
}

void setLossMode() {
    if (loss_model[0]) {
        pf_loss = fopen(loss_model, "rb");
        byte = 0;
    }
}

int nextLossBit() {
    int filelen, b;
    if (NULL == pf_loss) return 1;
    if (!(bit & 7)) {
        filelen = getFileLen(pf_loss);
        if (filelen >= bit / 8) {
            fseek(pf_loss, 0, SEEK_SET);
            bit = 0;
        }
        fread(&byte, 1, 1, pf_loss);
    }
    b = (byte >> (bit & 7)) & 1;
    bit++;
    return b;
}

const char* strToHex(const char* bytes) {
    static char buf[33] = "";
    int k;
    for (k = 0; k < 16; ++k) {
        sprintf(buf + 2 * k, "%02x", (0xff & bytes[k]));
    }
    buf[32] = '\0';
    return buf;
}

void writeInt32(char* buf, int val) {
    memcpy(buf, (const char* ) &val, 4);
}

int readInt32(const char* buf) {
    int val;
    memcpy(&val, buf, 4);
    return val;
}

void autoShutdown(int sig) {
    int k;
    if (debug_mode) {
        fprintf(stdout, "%d seconds timer has expired. Sever has auto-shutdown.\n", shutdown_time);
    }
    if (connect_fd) {
        shutdown(connect_fd, SHUT_RDWR);
    }
    if (socket_fd) {
        close(socket_fd);
    }
    for (k = 0; udp && k < window_size; ++k) {
        free(packets[k].data);
    }
    fflush(stdout);
    exit(0);
}

int validFileName(const char * file) {
    char valid_chars[] = "+-_.,/";
    for (; *file; ++file) {
        if (*file >= '0' && *file <= '9') {continue;} 
        if (*file >= 'a' && *file <= 'z') {continue;} 
        if (*file >= 'A' && *file <= 'Z') {continue;} 
        if (strchr(valid_chars, * file)) {continue;} 
        return 0;
    }
    return 1;
}

int filetype(const char *file, char *out) {
    char cmd[80];
    char buf[128];
    FILE *pfp = NULL;
    char *tmp;
    snprintf(cmd, sizeof(cmd), "/usr/bin/file %s", file);
    if ((pfp = (FILE* ) popen(cmd, "r")) == NULL) {
        fprintf(stderr, "Cannot execute '%s'.\n", cmd);
        return 1;
    } else {
        out[0] = '\0';
        while (fgets(buf, sizeof(buf) - 1, pfp) != NULL) {
            for (tmp = buf; *tmp; ++tmp) {
                if (*tmp == '\t') {
					*tmp = ' ';
				} 
            }
            strcat(out, buf);
        }
        pclose(pfp);
        return 0;
    }
}

int checksum(const char* file, int offset, int length, unsigned char md5_sum[] /* output checksum */ ) {
    MD5_CTX c;
    unsigned char buf[MAX_PACKET_SIZE]; // buffer to keep data
    FILE* pf;
    int n;
    if (NULL == (pf = fopen(file, "rb"))) {
        fprintf(stderr, "fail to open file %s", file);
        return 1;
    }
    MD5_Init(&c);
    fseek(pf, 0, SEEK_END);
    if (offset >= ftell(pf)) {
        fprintf(stderr, "Error: offset is larger or equal to the size of the file\n");
        fclose(pf);
        return 1;
    }
    if (length < 0) {
        length = ftell(pf) - offset;
    } else {
        if (length + offset > ftell(pf)) {
            fprintf(stderr, "Error: offset+length is larger or equal to the size of the file\n");
            fclose(pf);
            return 1;
        }
    }
    fseek(pf, offset, SEEK_SET);
    while (length > 0) {
        if (length > sizeof(buf)) {
            n = fread(buf, 1, sizeof(buf), pf);
            if (n < sizeof(buf)) { 
                length = n;
            } 
        } else {
            n = fread(buf, 1, length, pf);
        }
        if (n < 1) {break;} 
        length -= n;
        MD5_Update(&c, buf, n);
    }
    fclose(pf);
    MD5_Final(md5_sum, &c);
    return 0;
}

FILE* download(const char* file, int offset, int* length) {
    FILE* pf;
    long len;

    if (offset < 0 || 0 == *length) {return NULL;} 
    if (NULL == (pf = fopen(file, "rb"))) {
        fprintf(stderr, "fail to open file %s", file);
        return NULL;
    }
    len = getFileLen(pf);
    if (offset >= len || (*length > 0 && offset + *length > len)) {
        fclose(pf);
        return NULL;
    }
    if (*length <= 0) { 
		*length = len - offset;
    }
    fseek(pf, offset, SEEK_SET);
    return pf;
}

void ack_or_retransmission() {
    char ack[4 + PACKET_RESERVE_SIZE];
    int ret;
    socklen_t addrlen = sizeof(struct sockaddr);
    int k, j, seq;
    //wait for ack
    usleep(1000 * msinterval);
    //receive ack
    ret = recvfrom(socket_fd, ack, sizeof(ack), MSG_DONTWAIT, (struct sockaddr* ) &clientaddr, &addrlen);
    if (ret == sizeof(ack)) {
        seq = ntohl(readInt32(ack));
        for (j = 0; j < window_size; ++j) {
            if (seq == ntohl(readInt32(packets[j].data))) //this packet is acked
            {
                if (debug_mode) {
                    printf("recv ack: packet seq=%d, length=%d\n", seq, packets[j].length);
                }
                packets[j].length = 0;
                packets[j].empty = 1;
                break;
            }
        }
    } else {
        //retransmission
        for (k = 0; k < window_size; ++k) {
            if (!packets[k].empty) break;
        }
        if (k == window_size) {return;} 
        sendto(socket_fd, packets[k].data, packets[k].length, 0, (struct sockaddr* ) &clientaddr, addrlen);
        if (debug_mode) {
            printf("retransmission: packet seq=%d, length=%d\n", readInt32(packets[k].data), packets[k].length);
        }
    }
}

int usend(const char* src, long length) {
    const int MAX_SEND = MAX_PACKET_SIZE - 4 - PACKET_RESERVE_SIZE;
    char packet[MAX_PACKET_SIZE];
    char ack[4 + PACKET_RESERVE_SIZE];
    int ret;
    socklen_t addrlen = sizeof(struct sockaddr);
    int send;
    int k, j, seq;
    for (; length > 0;) {
        send = length > MAX_SEND ? MAX_SEND : length;
        //search for an empty packet
        for (k = 0; k < window_size; ++k) {
            if (packets[k].empty) break;
        }
        //all packets aren't acked
        if (k == window_size) {
            ack_or_retransmission();
        } else {
            //send a packet
            packets[k].length = send + 4 + PACKET_RESERVE_SIZE;
            packets[k].empty = 0;
            writeInt32(packets[k].data, htonl(packet_seq++));
            memset(packets[k].data + 4, 0, PACKET_RESERVE_SIZE);
            memcpy(packets[k].data + 4 + PACKET_RESERVE_SIZE, src, send);
            length -= send;
            src += send;
            if (nextLossBit()) {
                sendto(socket_fd, packets[k].data, packets[k].length, 0,
                    (struct sockaddr* ) &clientaddr, addrlen);
                if (debug_mode) {
                    printf("transmission: packet seq=%d, length=%d\n",
                        ntohl(readInt32(packets[k].data)), packets[k].length);
                }
            } else {
                if (debug_mode) {
                    printf("lost transmission: packet seq=%d, length=%d\n",
                        ntohl(readInt32(packets[k].data)), packets[k].length);
                }
            }
        }
    }

    for (;;) {
        //search for un-acked packet
        for (k = 0; k < window_size; ++k) {
            if (!packets[k].empty) {
				break;
			} 
        }
        if (k < window_size) {
            ack_or_retransmission();
        } else { 
            break;
        } 
    }
    return length;
}

int tsend(const char* src, long length) {
    if (udp) {
        return usend(src, length);
    } else {
        return send(connect_fd, src, length, 0);
    }
}

int trecv(char* dst, long max_length) {
    char packet[MAX_PACKET_SIZE];
    //struct sockaddr_in peer;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    int ret;
    if (udp) {
        ret = recvfrom(socket_fd, packet, MAX_PACKET_SIZE, 0, (struct sockaddr* ) &clientaddr, &addrlen);
        if (addrlen != sizeof(clientaddr)) {return -1;} 
        if (ret < 4 + PACKET_RESERVE_SIZE) {return -1;} 
        ret -= 4 + PACKET_RESERVE_SIZE;
        memcpy(dst, packet + 4 + PACKET_RESERVE_SIZE, ret);
        if (debug_mode) {
            printf("recv packet, seq=%d, length=%d\n", ntohl(readInt32(packet)), ret);
        }
        return ret;
    } else {
        return recv(connect_fd, dst, max_length, 0);
    }
}

int readData(char* buf, int length) {
    int n, read;
    while (length > 0) {
        n = trecv(buf, length);
        if (n < 1) {return -1;} 
        length -= n;
        buf += n;
    }
    return 0;
}

//read a complete messge
int readMsg(char* buf) {
    int n, read, len;
    if (udp) {
        return trecv(buf, MAX_PACKET_SIZE) < 0;
    } else {
        if (readData(buf, 5)) {return -1;} 
        len = ntohl(readInt32(buf + 1));
        return readData(buf + 5, len);
    }
}

void respFiletype() {
    char out[MAXLINE];
    int len = ntohl(readInt32(buff + 1));
    const char* file = &buff[5];
    buff[5 + len] = '\0';
    if (debug_mode) {
        fprintf(stdout, "%-12s\t:\t%-12s received with DataLength = %d, Data = '%s'\n", "FILETYPE_REQ", "FILETYPE_REQ", len, file);
    }
    if (!validFileName(file)) {
        fprintf(stderr, "Error: %s is invalid file name\n", file);
        out[0] = (char) FILETYPE_ERR;
        len = 0;
        writeInt32(out + 1, htonl(len));
    } else {
        if (filetype(file, out + 5)) {
            fprintf(stderr, "Error: fail to file %s\n", file);
            out[0] = (char) FILETYPE_ERR;
            len = 0;
            writeInt32(out + 1, htonl(len));
        } else {
            out[0] = (char) FILETYPE_RSP;
            len = strlen(&out[5]);
            writeInt32(out + 1, htonl(len));
        }
    }
    tsend(out, len + 5);
    if (debug_mode) {
        if (out[0] == (char) FILETYPE_ERR) {
            fprintf(stdout, "%-12s\t:\t%-12s sent with DataLength = %d\n", "FILETYPE_ERR", "FILETYPE_ERR", len);
        } else {
            fprintf(stdout, "%-12s\t:\t%-12s sent with DataLength = %d, Data = '%s'\n", "FILETYPE_RSP", "FILETYPE_RSP", len, out + 5);
        }
    }
}

void respChecksum() {
    char out[MAXLINE];
    int data_len = ntohl(readInt32(buff + 1));
    int offset = ntohl(readInt32(buff + 5));
    int length = ntohl(readInt32(buff + 9));
    const char* file = &buff[13];
    buff[5 + data_len] = '\0';
    if (debug_mode) {
        fprintf(stdout, "%-12s\t:\t%-12s received with DataLength = %d,offset = %d,length = %d,filename = '%s'\n", "CHECKSUM_REQ", "CHECKSUM_REQ", data_len, offset, length, file);
    }
    if ((validFileName(file) && offset >= 0) && (!checksum(file, offset, length, out + 5))) {
        out[0] = (char) CHECKSUM_RSP;
        data_len = 16;
    } else {
        fprintf(stderr, "Error: fail to checksum for %s\n", file);
        out[0] = (char) CHECKSUM_ERR;
        data_len = 0;
    }
    writeInt32(out + 1, htonl(data_len));
    tsend(out, data_len + 5);
    if (debug_mode) {
        if (out[0] == (char) CHECKSUM_ERR) {
            fprintf(stdout, "%-12s\t:\t%-12s sent with DataLength = %d\n",
                "CHECKSUM_ERR", "CHECKSUM_ERR", data_len);
        } else {
            fprintf(stdout, "%-12s\t:\t%-12s sent with DataLength = %d, checksum = %s\n",
                "CHECKSUM_RSP", "CHECKSUM_RSP", data_len, strToHex(out + 5));
        }
    }
}

void respDownload() {
    char out[MAXLINE];
    char* filedata;
    int data_len = ntohl(readInt32(buff + 1));
    int offset = ntohl(readInt32(buff + 5));
    int length = ntohl(readInt32(buff + 9));
    int len;
    FILE* pf;
    const char* file = &buff[13];
    buff[5 + data_len] = '\0';
    if (debug_mode) {
        fprintf(stdout,
            "%-12s\t:\t%-12s received with DataLength = %d, offset = %d, length = %d, filename = '%s'\n", "DOWNLOAD_REQ", "DOWNLOAD_REQ", data_len, offset, length, file);
    }
    if ((validFileName(file) && offset >= 0) && NULL != (pf = download(file, offset, &length))) {
        out[0] = (char) DOWNLOAD_RSP;
        data_len = length;
        writeInt32(out + 1, htonl(data_len));
        len = data_len;
        //send head
        tsend(out, 5);
        //send data
        filedata = (char * ) malloc(1 << 16);
        while (length > 0) {
            data_len = length > (1 << 16) ? (1 << 16) : length;
            data_len = fread(filedata, 1, data_len, pf);
            length -= data_len;
            tsend(filedata, data_len);
        }
        free(filedata);
        fclose(pf);
    } else {
        fprintf(stderr, "Error: fail to download %s\n", file);
        out[0] = (char) DOWNLOAD_ERR;
        data_len = 0;
        writeInt32(out + 1, htonl(data_len));
        tsend(out, data_len + 5);
    }
    if (debug_mode) {
        if (out[0] == (char) DOWNLOAD_ERR) {
            fprintf(stdout,
                "%-12s\t:\t%-12s sent with DataLength = %d\n",
                "DOWNLOAD_ERR", "DOWNLOAD_ERR", 0);
        } else {
            fprintf(stdout,
                "%-12s\t:\t%-12s sent with DataLength = %d\n",
                "DOWNLOAD_RSP", "DOWNLOAD_RSP", len);
        }
    }
}

void TCPserver() {
    if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "create socket error: %s(errno: %d)\n", strerror(errno), errno);
        exit(0);
    }
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (bind(socket_fd, (struct sockaddr* ) &servaddr, sizeof(servaddr)) == -1) {
        fprintf(stderr, "bind socket error: %s(errno: %d)\n", strerror(errno), errno);
        exit(0);
    }
    if (listen(socket_fd, 10) == -1) {
        fprintf(stderr, "listen socket error: %s(errno: %d)\n", strerror(errno), errno);
        exit(0);
    }
}

void UDPserver() {
    int k;
    if ((socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        fprintf(stderr, "create socket error: %s(errno: %d)\n", strerror(errno), errno);
        exit(1);
    }
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, (struct sockaddr* ) &servaddr, sizeof(servaddr)) == -1) {
        close(socket_fd);
        fprintf(stderr, "bind socket error: %s(errno: %d)\n", strerror(errno), errno);
        exit(1);
    }
    setLossMode();
    packets = (struct Data_Packet* ) malloc(window_size* sizeof(struct Data_Packet));
    if (NULL == packets) {
        close(socket_fd);
        fprintf(stderr, "fail to allocate memory: %s(errno: %d)\n", strerror(errno), errno);
        exit(1);
    }
    memset(packets, 0, window_size* sizeof(struct Data_Packet));
    for (k = 0; k < window_size; ++k) {
        packets[k].data = (char*) malloc(MAX_PACKET_SIZE);
        packets[k].length = 0;
        packets[k].empty = 1;
    }
}

void setServeMode() {
    if (udp) {
        UDPserver();
    } else {
        TCPserver();
    }
}

int taccept() {
    if (udp) {
        return 0;
    } else {
        return (connect_fd = accept(socket_fd, (struct sockaddr * ) NULL, NULL)) == -1;
    }
}

void serve() {
    setServeMode();
    int n, len, type, read;
    char out[MAXLINE];
    while (1) {
        if (taccept()) {
            fprintf(stderr, "accept socket error: %s(errno: %d)", strerror(errno), errno);
            break;
        }
        if (!readMsg(buff)) {
            len = ntohl(readInt32(buff + 1));
            buff[5 + len] = '\0';
            switch (buff[0] & 0xff) {
            case FILETYPE_REQ:
                respFiletype();
                break;
            case CHECKSUM_REQ:
                respChecksum();
                break;
            case DOWNLOAD_REQ:
                respDownload();
                break;
            default:
                fprintf(stdout, "%-12s\t:\t%-12sMessage with MessageType = 0x%02x received. Ignored.\n", "?", "?", (buff[0] & 0xff));
                buff[0] = (char) UNKNOWN_FAIL;
                writeInt32(buff + 1, 0);
                tsend(buff, 5);
                fprintf(stdout, "%-12s\t:\t%-12s sent with DataLength = %d\n", "UNKNOWN_FAIL", "UNKNOWN_FAIL", 0);
                break;
            }
        } else {
            fprintf(stderr, "fail to get message from client\n");
        }
        if (!udp) {close(connect_fd);} 
    }
    close(socket_fd);
}

// tserver [-udp loss_model] [-w window] [-r msinterval] [-d] [-t seconds] port
void parseArg(int argc, char * argv[]) {
    int k;
    for (k = 1; k < argc; ++k) {
        if (0 == strcmp("-udp", argv[k])) {
            if (k + 1 == argc) {
                fprintf(stderr, "error: need loss_model\n");
                exit(1);
            }
            strcpy(loss_model, argv[++k]);
            udp = 1;
        } else if (0 == strcmp("-w", argv[k])) {
            if (k + 1 == argc) {
                fprintf(stderr, "error: need window size\n");
                exit(1);
            }
            if (!(window_size = atoi(argv[++k]))) {
                fprintf(stderr, "error: illegal window size!\n");
                exit(1);
            }
            udp = 1;
        } else if (0 == strcmp("-r", argv[k])) {
            if (k + 1 == argc) {
                fprintf(stderr, "error: need msinterval\n");
                exit(1);
            }
            if (!(msinterval = atoi(argv[++k]))) {
                fprintf(stderr, "error: illegal msinterval!\n");
                exit(1);
            }
            udp = 1;
        } else if (0 == strcmp("-d", argv[k])) {
            debug_mode = 1;
        } else if (0 == strcmp("-t", argv[k])) {
            if (k + 1 == argc) {
                fprintf(stderr, "error: need window size\n");
                exit(1);
            }
            if (!(shutdown_time = atoi(argv[++k]))) {
                fprintf(stderr, "illegal seconds!\n");
                exit(1);
            }
        } else if (k + 1 == argc && port == 0) {
            if (!(port = atoi(argv[k]))) {
                fprintf(stderr, "illegal port!\n");
                exit(1);
            }
        }
    }
    if (window_size < 1) {window_size = 1;} 
    if (msinterval < 1) {msinterval = 1;} 
    if (msinterval > 5000) {msinterval = 5000;} 
    if (port < 10000 || port > 99999) {
        fprintf(stderr, "port must be between 10000 and 99999!\n");
        exit(1);
    }
}
