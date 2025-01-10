/**
 * Stupid simple tftp daemon
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <linux/limits.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>

/*** Protocol Definitions ***/
#define TFTP_RRQ 1
#define TFTP_WRQ 2
#define TFTP_DATA 3
#define TFTP_ACK 4
#define TFTP_ERR 5

#define TFTP_ERR_ND 0
#define TFTP_ERR_ENOENT 1
#define TFTP_ERR_EACCESS 2
#define TFTP_ERR_ENOSPACE 3
#define TFTP_ERR_EBADTRANS 5 /* Unknown transfer ID */
#define TFTP_ERR_EEXISTS 6
#define TFTP_ERR_ENOUSER 7

#define UDP_MAX_PAYLOAD 65507
#define RETRY_PERIOD 0.5        /* Retry after 0.5s elapsed */
#define TIMEOUT_PERIOD 4        /* Die after 4s */

#define BLOCK_SIZE 512

/*** Protocol structures ***/

#pragma pack(1)
struct tftp_ack {
    uint16_t op;
    uint16_t block;
};

struct tftp_data {
    uint16_t op;
    uint16_t block;
    char data[];
};
#pragma pack()

/*** Forward decls ***/

typedef struct tftpd_state {
    char file[PATH_MAX];
    int fd;
    struct sockaddr_in addr;
    uint16_t block;
    int acked;
    double lastsent;
    int done;
    int errored;
    int write;
    int connected;

    struct tftpd_state* next;
} tftpd_state_t;

typedef struct tftpd_opts {
    uint16_t port;
    char addr[32];
    char root[PATH_MAX];
    int uparms;     /* Use the current user's perms to determine if a file is accessible, instead of requiring O+RW */
} tftpd_opts_t;

int tftpd(const tftpd_opts_t* opts);
static void _send_error_resp(int fd, struct sockaddr_in* dst, socklen_t socklen, uint16_t errcode, const char* msg);
static void _send_ack(int fd, struct sockaddr_in* dst, socklen_t socklen, uint16_t block);
static double _curtime();

int main(int argc, char** argv) {
    struct tftpd_opts o = {0};

    getcwd(o.root, sizeof(o.root));
    strcpy(o.addr, "127.0.0.1");
    o.port = 5060;

    int opt;
    while((opt = getopt(argc, argv, "p:r:ui:")) > 0) {
        switch(opt) {
        case 'p':
            o.port = atoi(optarg);
            break;
        case 'r':
            strcpy(o.root, optarg);
            break;
        case 'u':
            o.uparms = 1;
            break;
        case 'i':
            strncpy(o.addr, optarg, sizeof(o.addr)-1);
            o.addr[sizeof(o.addr)-1] = 0;
            break;
        }
    }

    return tftpd(&o);
}


int tftpd(const tftpd_opts_t* opts)
{
    fprintf(stderr, "Started tftpd on %s:%d\n", opts->addr, opts->port);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Unable to create socket");
        return 1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_port = htons(opts->port);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(opts->addr);
    if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("Unable to bind address");
        return 1;
    }

    /* Make it non-blocking */
    struct timeval tv = {0,0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct tftpd_state* clients = NULL;

    struct sockaddr_in fromaddr = {0};
    socklen_t fromsize = sizeof(fromaddr);

    /* Alloc working buf for an entire UDP packet */
    char buf[65535];

    ssize_t r;
    while(1)
    {
        /*************** Service incoming requests ***************/
        if((r = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&fromaddr, &fromsize)) >= 0) {
            uint16_t op = ntohs(*(uint16_t*)buf);
            ssize_t rem = r;

            /* Match a client from the list, or create a new one */
            struct tftpd_state* client = clients;
            for (; client && client->addr.sin_addr.s_addr != fromaddr.sin_addr.s_addr; client = client->next);
            
            if (!client) {
                client = calloc(1, sizeof(struct tftpd_state));
                client->addr = fromaddr;
                client->next = clients;
                clients = client;
            }

            switch(op)
            {
            case TFTP_RRQ:
            case TFTP_WRQ:
            {
                char fileName[PATH_MAX] = {0}, mode[32] = {0};
                if (rem < 4) {
                    fprintf(stderr, "Short read/write request from %s\n", inet_ntoa(fromaddr.sin_addr));
                    break;
                }

                /* Read file name */
                char* s = buf+2;
                for (char* pf = fileName; *s && rem > 0; ++s, ++pf, --rem)
                    *pf = *s;

                /* file mode */
                ++s;
                for (char* pm = mode; *s && pm-mode < sizeof(mode) && rem > 0; ++s, ++pm, --rem)
                    *pm = *s;

                if (!strcasecmp(mode, "netascii"));
                else if (!strcasecmp(mode, "octet"));
                else {
                    fprintf(stderr, "Unsupported mode '%s' for file '%s' from %s\n", mode, fileName, inet_ntoa(fromaddr.sin_addr));
                    client->errored = 1;
                    break;
                }

                char realPath[PATH_MAX];
                snprintf(realPath, sizeof(realPath), "%s/%s", opts->root, fileName);

                /* Check access flags */
                struct stat st;
                if (stat(realPath, &st) < 0) {
                    if (errno == ENOENT) {
                        if (op != TFTP_WRQ) {
                            _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_ENOENT, "No such file");
                            client->errored = 1;
                            break;
                        }
                    }
                    else {
                        fprintf(stderr, "stat(%s) failed: %s\n", realPath, strerror(errno));
                        _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EACCESS, "Stat failed");
                        client->errored = 1;
                        break;
                    }
                }
                /* Overwrites not allowed */
                else if (op == TFTP_WRQ) {
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EEXISTS, "File exists");
                    client->errored = 1;
                    break;
                }

                /* No O+R or O+RW without -u == no access */
                int rcheck = opts->uparms ? S_IRUSR : S_IROTH, wcheck = opts->uparms ? S_IWUSR : S_IWOTH;
                if (op == TFTP_RRQ && !(st.st_mode & rcheck)) {
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EACCESS, "Access denied");
                    client->errored = 1;
                    break;
                }

                if(client->fd > 0) close(client->fd);

                strncpy(client->file, realPath, sizeof(client->file)-1);
                client->file[sizeof(client->file)-1] = 0;
                client->write = op == TFTP_WRQ;

                if ((client->fd = open(realPath, op == TFTP_WRQ ? (O_CREAT|O_WRONLY|O_TRUNC) : O_RDONLY)) < 0) {
                    fprintf(stderr, "Unable to open '%s': %s\n", realPath, strerror(errno));
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_ENOENT, strerror(errno));
                    client->errored = 1;
                    break;
                }

                /* Update permissions for the file. open(2) does not behave consistently... */
                if (op == TFTP_WRQ && chmod(realPath, S_IWUSR|S_IRUSR|S_IWGRP|S_IRGRP|S_IROTH) < 0) {
                    int se = errno;
                    fprintf(stderr, "Unable to chmod '%s': %s\n", realPath, strerror(se));
                    /* we're gonna reject this and remove the file. Don't want any weird files w/broken perms sitting around..
                     * we may be running as root, that makes this even more annoying! */
                    if (unlink(realPath) < 0) {
                        fprintf(stderr, "Unable to unlink '%s': %s\n You will have to delete this manually!\n", realPath, strerror(errno));
                    }
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EACCESS, strerror(se));
                    client->errored = 1;
                    break;
                }

                _send_ack(sock, &fromaddr, fromsize, 0);
                client->block++;
                client->lastsent = _curtime();
                client->connected = 1;

                fprintf(stderr, "Open file %s, mode %s\n", fileName, mode);
                break;
            }
            case TFTP_DATA:
                if (!client->connected) {
                    fprintf(stderr, "Seemingly random packet from unconnected client at %s; ignoring\n", inet_ntoa(fromaddr.sin_addr));
                    break;
                }

                if (!client->write) {
                    fprintf(stderr, "Asked to write, but configured for read-only\n");
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EACCESS, "Asked to write, but configured for read");
                    client->errored = 1;
                    break;
                }
                struct tftp_data* dat = (struct tftp_data*)buf;
                uint16_t block = ntohs(dat->block);

                /* seek based on block number */
                lseek(client->fd, (block-1) * BLOCK_SIZE, SEEK_SET);

                ssize_t tow = rem - sizeof(struct tftp_data);
                printf("tow=%ld\n",tow);
                if (write(client->fd, dat->data, tow) < 0) {
                    perror("write failed");
                    _send_error_resp(sock, &fromaddr, fromsize, TFTP_ERR_EBADTRANS, "Unable to write");
                    client->errored = 1;
                    break;
                }

                /* Transaction is most likely done */
                if (tow < BLOCK_SIZE) {
                    client->done = 1;
                }

                _send_ack(sock, &fromaddr, fromsize, block);
                client->block++;
                client->lastsent = _curtime();
                break;
            case TFTP_ACK:
            {
                struct tftp_ack* ack = (struct tftp_ack*)buf;
                if (ntohs(ack->block) == client->block)
                    client->acked = 1;
                else {
                    fprintf(stderr, "Acked block '%d' but client was expecting ack for '%d'\n", ntohs(ack->block), client->block);
                    client->errored = 1;
                }
                break;
            }
            case TFTP_ERR:
                break;
            default:
                fprintf(stderr, "Unknown opcode %d from %s\n", (int)op, inet_ntoa(fromaddr.sin_addr));
                client->errored = 1;
                break;
            }
        }

        /*************** Service outgoing requests ***************/
        for (tftpd_state_t* s = clients; s; s = s->next)
        {
            /* No outgoing requests from us */
            if (s->write)
                continue;

            /* If ack'ed, move to next block */
            if (s->acked)
                s->block++;
            /* Not ack'ed yet; wait until retry period exceeded for this client */
            else if((_curtime() - s->lastsent) < RETRY_PERIOD)
                continue;

            char readBuf[sizeof(struct tftp_data) + BLOCK_SIZE];

            struct tftp_data* packet = (struct tftp_data*)readBuf;
            packet->op = htons(TFTP_DATA);
            packet->block = htons(s->block);

            /* Seek and read based on block number */            
            lseek(s->fd, (s->block-1) * BLOCK_SIZE, SEEK_SET);

            ssize_t nr = read(s->fd, &packet->data, BLOCK_SIZE);
            if (nr < 0) {
                fprintf(stderr, "Unable to read %s: %s\n", s->file, strerror(errno));
            }

            /* No more data to read, mark as dead and move on */
            if (nr == 0) {
                s->done = 1;
                continue;
            }

            ssize_t sz = sizeof(struct tftp_data) + nr;
            if (sendto(sock, packet, sz, 0, (struct sockaddr*)&s->addr, sizeof(s->addr)) != sz) {
                perror("Unable to send");
            }

            s->lastsent = _curtime();
            s->acked = 0;
        }

        /*************** Cleanup clients ***************/
        for (tftpd_state_t* s = clients, *prev = NULL; s;) {
            /* close open files when we're done with them, or if we timeout */
            if ((s->done && s->acked) || s->errored || (_curtime() - s->lastsent > TIMEOUT_PERIOD)) {
                printf("Closed connection %s\n", inet_ntoa(s->addr.sin_addr));
                close(s->fd);
                if (prev)
                    prev->next = s->next;
                if (s == clients)
                    clients = NULL;
                tftpd_state_t* ns = s->next;
                free(s);
                s = ns;
            }
            else {
                prev = s;
                s = s->next;
            }
        }
    }

    return 0;
}

static void _send_error_resp(int fd, struct sockaddr_in* dst, socklen_t socklen, uint16_t errcode, const char* msg)
{
#pragma pack(1)
    union {
        struct {
            uint16_t op;
            uint16_t errcode;
            char str[];
        } packet;
        char rawbuf[4096];
    } x;
#pragma pack()

    x.packet.errcode = htons(errcode);
    x.packet.op = htons(TFTP_ERR);
    strncpy(x.packet.str, msg, sizeof(x.rawbuf) - sizeof(x.packet));

    if (sendto(fd, x.rawbuf, sizeof(x.packet) + strlen(msg) + 1, 0, (struct sockaddr*)dst, socklen) < 0)
        perror("Unable to send error response");
}

static void _send_ack(int fd, struct sockaddr_in* dst, socklen_t socklen, uint16_t block)
{
    struct tftp_ack a = {htons(TFTP_ACK), htons(block)};
    if (sendto(fd, &a, sizeof(a), 0, (struct sockaddr*)dst, socklen) < 0) {
        perror("Unable to send ACK");
    }
}

static double _curtime()
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec + (tp.tv_nsec / 1e9);
}
