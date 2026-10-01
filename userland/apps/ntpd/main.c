/* AzamiOS SNTP client: periodic NTPv4 UDP time synchronisation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <netinet/in.h>
#include "ntp_wire.h"

#define DEFAULT_SERVER "time.cloudflare.com"
#define REPLY_TIMEOUT_MS 3000
#define POLL_SECONDS 300

static int64_t real_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * NTP_NSEC_PER_SEC + ts.tv_nsec;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void load_server(char *dst, size_t cap)
{
    strncpy(dst, DEFAULT_SERVER, cap - 1);
    dst[cap - 1] = 0;

    FILE *file = fopen("/etc/ntp.conf", "r");
    if (!file) return;
    char line[320];
    while (fgets(line, sizeof(line), file)) {
        char directive[16];
        char host[256];
        if (sscanf(line, " %15s %255s", directive, host) != 2 ||
            (strcmp(directive, "server") && strcmp(directive, "pool")))
            continue;
        strncpy(dst, host, cap - 1);
        dst[cap - 1] = 0;
        break;
    }
    fclose(file);
}

/* Return 0 for a sample, -1 for a transient error and -2 for a KoD reply. */
static int query_server(const char *server, int64_t *offset_ns,
                        int64_t *delay_ns, long *error_us, unsigned *leap)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo *addresses = NULL;
    if (getaddrinfo(server, "123", &hints, &addresses) != 0 || !addresses)
        return -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { freeaddrinfo(addresses); return -1; }
    int connected = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connected < 0) { close(fd); return -1; }

    uint8_t request[NTP_PACKET_SIZE];
    int64_t t1 = real_ns();
    if (t1 < 0) { close(fd); return -1; }
    uint64_t sent = ntp_from_unix(t1 / NTP_NSEC_PER_SEC,
                                  (long)(t1 % NTP_NSEC_PER_SEC));
    ntp_make_request(request, sent);
    if (send(fd, request, sizeof(request), 0) != (ssize_t)sizeof(request)) {
        close(fd);
        return -1;
    }

    int64_t start = monotonic_ms();
    if (start < 0) { close(fd); return -1; }
    for (;;) {
        int64_t elapsed = monotonic_ms() - start;
        int remaining = REPLY_TIMEOUT_MS - (int)elapsed;
        if (remaining <= 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(pfd.revents & POLLIN)) break;

        uint8_t reply[512];
        ssize_t n = recv(fd, reply, sizeof(reply), MSG_DONTWAIT);
        int64_t t4 = real_ns();
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < NTP_PACKET_SIZE || t4 < 0) continue;
        if (ntp_load64(reply + 24) == sent && reply[1] == 0) {
            close(fd);
            return -2; /* kiss-o'-death: respect server rate limiting */
        }
        if (ntp_measure(reply, (size_t)n, sent, t1, t4,
                        offset_ns, delay_ns) == 0) {
            /* Root dispersion and root delay are unsigned/signed 16.16
             * seconds. Include the local path delay and a 1 ms floor. */
            uint64_t dispersion = ((uint64_t)ntp_load32(reply + 8) * 1000000) >> 16;
            int32_t root_delay = (int32_t)ntp_load32(reply + 4);
            uint64_t upstream = root_delay > 0 ?
                ((uint64_t)root_delay * 1000000) >> 17 : 0;
            uint64_t uncertainty = dispersion + upstream +
                                   (uint64_t)(*delay_ns / 2000) + 1000;
            if (uncertainty >= 16000000) continue;
            *error_us = (long)uncertainty;
            *leap = reply[0] >> 6;
            close(fd);
            return 0;
        }
    }
    close(fd);
    return -1;
}

static int discipline_clock(int64_t offset_ns, long error_us, unsigned leap)
{
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    if (adjtimex(&tx) < 0) return -1;
    int status = (tx.status | STA_PLL) & ~(STA_UNSYNC | STA_INS | STA_DEL);
    if (leap == 1) status |= STA_INS;
    if (leap == 2) status |= STA_DEL;

    /* Large errors may take hours at the kernel's 500 ppm slew limit.
     * Correct them in one step, then mark the clock synchronised. */
    if (offset_ns <= -500000000LL || offset_ns >= 500000000LL) {
        int64_t now = real_ns();
        if (now < 0 || now + offset_ns < 0) return -1;
        int64_t target = now + offset_ns;
        struct timespec ts = {
            .tv_sec = (time_t)(target / NTP_NSEC_PER_SEC),
            .tv_nsec = (long)(target % NTP_NSEC_PER_SEC),
        };
        if (clock_settime(CLOCK_REALTIME, &ts) < 0) return -1;
        tx.offset = 0;
    } else {
        tx.offset = (long)(offset_ns / 1000);
    }
    tx.modes = ADJ_STATUS | ADJ_OFFSET | ADJ_MAXERROR | ADJ_ESTERROR;
    tx.status = status;
    tx.maxerror = error_us;
    tx.esterror = error_us;
    return adjtimex(&tx) < 0 ? -1 : 0;
}

static void sleep_seconds(unsigned seconds)
{
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    while (nanosleep(&req, &req) < 0 && errno == EINTR) { }
}

int main(int argc, char **argv)
{
    char server[256];
    load_server(server, sizeof(server));
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-1") || !strcmp(argv[i], "--once")) once = 1;
        else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--server")) && i + 1 < argc) {
            strncpy(server, argv[++i], sizeof(server) - 1);
            server[sizeof(server) - 1] = 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: ntpd [-1|--once] [-s|--server HOST]");
            puts("server may also be set with 'server HOST' in /etc/ntp.conf");
            return 0;
        } else {
            fprintf(stderr, "ntpd: invalid argument: %s\n", argv[i]);
            return 2;
        }
    }

    int reported_failure = 0;
    for (;;) {
        int64_t offset = 0, delay = 0;
        long error_us = 0;
        unsigned leap = 0;
        int rc = query_server(server, &offset, &delay, &error_us, &leap);
        if (rc == 0) {
            if (discipline_clock(offset, error_us, leap) == 0) {
                printf("ntpd: synchronised with %s, offset %lld us, delay %lld us\n",
                       server, (long long)(offset / 1000), (long long)(delay / 1000));
                reported_failure = 0;
                if (once) return 0;
                sleep_seconds(POLL_SECONDS);
                continue;
            }
            if (!reported_failure)
                fprintf(stderr, "ntpd: cannot adjust clock: %s\n", strerror(errno));
        } else if (!reported_failure) {
            fprintf(stderr, rc == -2 ? "ntpd: %s refused requests\n" :
                    "ntpd: no valid response from %s\n", server);
        }
        reported_failure = 1;
        if (once) return 1;
        sleep_seconds(rc == -2 ? 3600 : 30);
    }
}
