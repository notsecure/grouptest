#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <ncurses.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>

#define debug(...) wprintw(win, __VA_ARGS__); wrefresh(win);
#define countof(x) (sizeof(x)/sizeof(*(x)))

#define DEFAULT_PORT 0x7E0

#define msec(x) ((uint64_t)x * 1000 * 1000)

/* peer */
enum
{
    REQUEST_JOIN,
    PACKET_JOIN,
    REQUEST_MESSAGES,
    PACKET_MESSAGES,
};

/* group */
enum
{
    PEER_JOIN = 0,
    PEER_ALIVE = 1,
    PEER_MESSAGE = 128,
};

typedef struct
{
    uint32_t id, ip;
    uint16_t port, timeout;
}PEER;

int sock;
uint32_t self_id, self_timeout;

PEER *peerlist;
int npeers;
struct
{
    uint32_t id, quality;
}bestpeer;

typedef struct
{
    uint8_t *data;
    uint64_t time;
}GROUPMSG;

GROUPMSG msg[65536];
uint32_t nmsg;

WINDOW *win, *win_info, *win_input;

const uint8_t groupmessage_size[] = {15, 9};

uint32_t msg_size(uint8_t *data) {
    if(data[4] & 128) {
        return 6 + data[5];
    } else if(data[4] >= countof(groupmessage_size)) {
        return ~0;
    } else {
        return groupmessage_size[data[4]];
    }
}

_Bool net_init(_Bool host)
{
    u_long mode = 1;

    if((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        return 0;
    }

    /* nonblocking */
    if(ioctl(sock, FIONBIO, &mode) == -1) {
        return 0;
    }

    if(host) {
        struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = DEFAULT_PORT
        };

        return (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    }

    return 1;
}

uint64_t get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return ((uint64_t)ts.tv_sec * (1000 * 1000 * 1000)) + (uint64_t)ts.tv_nsec;
}

PEER* peer_find(uint32_t id, uint32_t ip, uint16_t port)
{
    int i = 0;
    while(i < npeers) {
        PEER *p = &peerlist[i++];

        if((p->id == id) || (ip && p->ip == ip && p->port == port)) {
            return p;
        }
    }

    return NULL;
}

PEER* peer_new(uint32_t id, uint32_t ip, uint16_t port)
{
    void *d;
    PEER *p;

    if((p = peer_find(id, ip, port)) != NULL) {
        p->id = id;
        if(ip) {
            p->ip = ip;
            p->port = port;
        }
        p->timeout = 0;
        return p;
    }

    d = realloc(peerlist, ((npeers + 1) * sizeof(PEER)));
    if(!d) {
        return NULL;
    }

    peerlist = d;

    p = &peerlist[npeers++];

    p->id = id;
    p->ip = ip;
    p->port = port;
    p->timeout = 0;

    return p;
}

_Bool peer_send(PEER *p, void *data, int length)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = p->port,
        .sin_addr.s_addr = p->ip,
    };

    return (sendto(sock, data, length, 0, (struct sockaddr*)&addr, sizeof(addr)) == length);
}

void* group_write(uint8_t packet)
{
    GROUPMSG *g = &msg[nmsg++];
    g->data = malloc(groupmessage_size[packet]);
    g->time = get_time();

    uint32_t key = rand();

    memcpy(g->data, &key, 4);
    g->data[4] = packet;

    return g->data + 5;
}

void group_write_message(void *data, int len)
{
    if(len >= 256) {
        len = 255;
    }

    GROUPMSG *g = &msg[nmsg++];
    g->data = malloc(6 + len);
    g->time = get_time();

    uint32_t key = rand();

    memcpy(g->data, &key, 4);
    g->data[4] = PEER_MESSAGE;
    g->data[5] = len;
    memcpy(g->data + 6, data, len);
}

void* group_write_raw(int len)
{
    GROUPMSG *g = &msg[nmsg++];
    g->data = malloc(len);
    g->time = get_time();

    return g->data;
}

void* group_recv(uint8_t *data, uint32_t len)
{
    uint32_t size;

    if(len < 6 || (size = msg_size(data)) > len) {
        debug("invalid group_recv: %u %u\n", data[4], len);
        return NULL;
    }

    uint32_t key;
    uint8_t packet = data[4];

    memcpy(&key, data, 4);

    uint64_t time = get_time();
    int i = nmsg - 1;
    while(i >= 0 && msg[i].time + msec(5000) >= time) {
        uint8_t *d = msg[i].data;
        if(d[4] == packet) {
            if(memcmp(d, &key, 4) == 0) {
                return data + size;
            }
        }
        i--;
    }

    switch(packet) {
        case PEER_JOIN: {
            uint32_t id, ip;
            uint16_t port;

            memcpy(&id, data + 5, 4);
            memcpy(&ip, data + 9, 4);
            memcpy(&port, data + 13, 2);

            if(id == self_id) {
                self_timeout = 0;
                break;
            }

            peer_new(id, ip, port);

            debug("peer join %u\n", id);

            break;
        }

        case PEER_ALIVE: {
            uint32_t id;

            memcpy(&id, data + 5, 4);

            if(id == self_id) {
                self_timeout = 0;
                break;
            }

            peer_new(id, 0, 0);

            //debug("peer alive %u %u\n", id, key);

            break;
        }

        default: {
            debug("chat message: %.*s\n", data[5], data + 6);
            break;
        }
    }

    void *d = group_write_raw(size);
    memcpy(d, data, size);

    return data + size;
}

void group_requestmessages(void)
{
    /* send request to peers: 1) best peer, 2) weighted random peer 3) next peer in order */
    uint8_t request[5];
    memcpy(request, &self_id, 4);
    request[4] = REQUEST_MESSAGES;

    /* 3 unique random values between 0 and npeers*/
    if(npeers <= 3) {
        int i = 0;
        while(i < npeers) {
            peer_send(&peerlist[i++], request, 5);
        }
    } else {
        uint32_t i, j, k;

        i = rand() % npeers;
        j = rand() % (npeers - 1);
        if(j == i) {
            j = npeers - 1;
        }

        k = rand() % (npeers - 2);
        if(k == i) {
            k = npeers - 2;
            if(k == j) {
                k = npeers - 1;
            }
        }
        if(k == j) {
            k = npeers - 1;
            if(k == i) {
                k = npeers - 2;
            }
        }

        peer_send(&peerlist[i], request, 5);
        peer_send(&peerlist[j], request, 5);
        peer_send(&peerlist[k], request, 5);
    }
}

void peer_recv(PEER *p, uint8_t *data, uint32_t len)
{
    if(len == 4) {
        return;
    }

    switch(data[4]) {
        case PACKET_JOIN: {
            uint8_t *d = data + 5;
            len -= 5;
            while(len) {
                if(len < 10) {
                    break;
                }

                len -= 10;

                uint32_t id, ip;
                uint16_t port;

                memcpy(&id, d, 4); d += 4;
                memcpy(&ip, d, 4); d += 4;
                memcpy(&port, d, 2); d += 2;

                peer_new(id, ip, port);
            }

            debug("number of peers: %u\n", npeers);

            break;
        }

        case PACKET_MESSAGES: {
            uint8_t *d = data + 5;
            int offset;
            while((offset = (d - data)) != len && (d = group_recv(d, len - offset)) != NULL) {}
            break;
        }

        case REQUEST_MESSAGES: {
            uint8_t *d = data;
            memcpy(d, &self_id, 4); d += 4;
            *d++ = PACKET_MESSAGES;

            uint64_t time = get_time();
            int i = nmsg - 1;
            while(i >= 0 && msg[i].time + msec(2000) >= time) {
                uint8_t *data = msg[i].data;
                int len = msg_size(data);
                memcpy(d, data, len);
                d += len;
                i--;
            }

            peer_send(p, data, d - data);
            break;
        }
    }
}

void update_info(void)
{
    wmove(win_info, 0, 0);
    werase(win_info);

    wprintw(win_info, "Num peers: %u\n", npeers);
    int i = 0;
    while(i < 4) {
        if(i < npeers) {
            PEER *p = &peerlist[i];
            char ip[16];
            inet_ntop(AF_INET, &p->ip, ip, 16);
            wprintw(win_info, "%s:%u\n", ip, p->port);
        } else {
            wprintw(win_info, "\n");
        }
        i++;
    }

    wrefresh(win_info);
}

void do_input(void)
{
    static char inputstr[256] = {0};
    static int inputlen = 0;

    int ch;
    while((ch = wgetch(win_input)) != ERR) {
        switch(ch) {
            case 127: {
                if(inputlen != 0) {inputlen--;}
                break;
            }

            case '\n': {
                group_write_message(inputstr, inputlen);
                inputlen = 0;
                break;
            }

            default: {
                inputstr[inputlen++] = ch;
                break;
            }
        }
    }

    wmove(win_input, 0, 0);
    werase(win_input);
    wprintw(win_input, "say: %.*s", inputlen, inputstr);

    wrefresh(win_input);
}

int main(int argc, char** argv)
{
    initscr();

    noecho();
    cbreak();

    int h, w;
    getmaxyx(stdscr, h, w);

    win = newwin(h - 6, w, 5, 0);
    win_info = newwin(5, w, 0, 0);
    win_input = newwin(1, w, h - 1, 0);

    scrollok(win, TRUE);
    nodelay(win_input, TRUE);

    update_info();

    debug("%u\n", win);

    uint64_t now, then;

    if(argc != 1 && argc != 4) {
        debug("Usage: %s [id ip port]\n", argv[0]);
        return 1;
    }

    if(!net_init((argc == 1))) {
        return 1;
    }

    srand((argc == 1) ? 0 : time(NULL));
    self_id = rand();

    debug("My id: %u\n", self_id);

    if(argc == 4) {
        PEER *p;
        uint32_t id, ip;
        uint16_t port;
        uint8_t request[5];

        id = strtol(argv[1], NULL, 0);
        inet_pton(AF_INET, argv[2], &ip);
        port = strtol(argv[3], NULL, 0);

        p = peer_new(id, ip, port);

        memcpy(request, &self_id, 4);
        request[4] = REQUEST_JOIN;
        peer_send(p, &request, 5);
    }


    now = then = get_time();
    while(1) {
        struct sockaddr_in addr;
        socklen_t sizeofaddr = sizeof(addr);
        int len;
        uint8_t data[65536];

        while((len = recvfrom(sock, data, sizeof(data), 0, (struct sockaddr*)&addr, &sizeofaddr)) >= 0) {
            if(len < 4) {
                continue;
            }

            PEER *p;
            uint32_t id, ip = addr.sin_addr.s_addr;
            uint16_t port = addr.sin_port;
            memcpy(&id, data, 4);

            if((p = peer_find(id, ip, port)) != NULL) {
                peer_recv(p, data, len);
                continue;
            }

            if(len == 4) {
                continue;
            }

            switch(data[4]) {
                case REQUEST_JOIN: {
                    if(len != 5) {
                        break;
                    }

                    if((p = peer_new(id, ip, port)) != NULL) {
                        /* tell the group that the peer has joined */
                        uint8_t *d = group_write(PEER_JOIN);

                        memcpy(d, &id, 4);
                        memcpy(d + 4, &ip, 4);
                        memcpy(d + 8, &port, 2);

                        /* tell the new peer some initial info on the group */
                        memcpy(data, &self_id, 4);
                        data[4] = PACKET_JOIN;
                        d = data + 5;
                        int i = 0;
                        while(i < npeers - 1) {
                            PEER *p = &peerlist[i++];
                            memcpy(d, &p->id, 4); d += 4;
                            memcpy(d, &p->ip, 4); d += 4;
                            memcpy(d, &p->port, 2); d += 2;
                        }

                        peer_send(p, data, d - data);
                    }
                    break;
                }
            }
        }

        now = get_time();
        if(now - then >= msec(500)) {
            static int resend = 0;

            resend++;
            if(resend == 4) {
                /* send alive every 2s */
                uint8_t *d = group_write(PEER_ALIVE);

                memcpy(d, &self_id, 4);

                resend = 0;
            }

            /* request messages every 500ms */
            group_requestmessages();

            /* peer timeouts */
            int i = 0;
            while(i < npeers) {
                PEER *p = &peerlist[i++];
                p->timeout++;
                if(p->timeout == 20) {
                    debug("peer %u timeout\n", p->id);
                    /* 10 second timeout, remove peer */
                    memmove(p, p + 1, (npeers - i) * sizeof(PEER));
                    npeers--;
                    i--;
                    //memset(p, 0, sizeof(PEER));
                    //p->timeout = 20;
                }
            }

            update_info();

            then += msec(500);
        }

        do_input();
        usleep(500);
    }

    return 0;
}
