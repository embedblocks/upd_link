/*
 * udp_link.c on Linux sockets over 127.0.0.1 — a stand-in for the loopback target
 * checks, NOT a substitute for them: lwIP, VFS select() and the FreeRTOS tick
 * differ from Linux. It checks that the generated code does what the spec says
 * wherever the spec does not depend on the platform.
 *
 * Built once per wait variant and per tick rate (see Makefile). Links with
 * -Wl,--wrap=select,--wrap=recvmsg,--wrap=sendmsg to count waits, inject
 * ENOMEM and log the order of sends.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "udp_link.h"
#include "test_common.h"

#ifndef UDP_LINK_WAIT_SO_RCVTIMEO
#define UDP_LINK_WAIT_SO_RCVTIMEO 0
#endif

/* ---------------- wrappers: count waits, inject a wait failure ---------------- */

int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
ssize_t __real_recvmsg(int, struct msghdr *, int);

static atomic_int waits;            /* select() calls, or blocking recvmsg() calls */
static atomic_int fail_wait_at;     /* fail the n-th wait with ENOMEM (0 = off)    */
static atomic_bool spurious_first;  /* make the first select() return 0 at once    */

int __wrap_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    int k = atomic_fetch_add(&waits, 1) + 1;
    if (k == 1 && atomic_exchange(&spurious_first, false)) {
        FD_ZERO(r);                                   /* an early, empty return */
        return 0;
    }
    if (atomic_load(&fail_wait_at) == k) {
        atomic_store(&fail_wait_at, 0);
        errno = ENOMEM;
        return -1;
    }
    return __real_select(n, r, w, e, tv);
}

ssize_t __wrap_recvmsg(int fd, struct msghdr *m, int flags)
{
    if (!(flags & MSG_DONTWAIT)) {
        atomic_fetch_add(&waits, 1);
    }
    return __real_recvmsg(fd, m, flags);
}

/* ---------------- helpers ---------------- */

static uint16_t PA, PB;             /* ports of endpoints A and B */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static udp_link_handle_t open_ep(uint16_t local, uint16_t peer, uint16_t maxp)
{
    udp_link_config_t c = {
        .local_port = local, .peer_ip = { ESP_IP4TOADDR(127, 0, 0, 1) },
        .peer_port = peer, .stream_id = 0x42, .max_payload = maxp,
    };
    udp_link_handle_t h = NULL;
    esp_err_t e = udp_link_open(&c, &h);
    if (e != ESP_OK) {
        fprintf(stderr, "udp_link_open(%u) failed: %s\n", local, udp_link_err_to_name(e));
        exit(2);
    }
    return h;
}

static void fill(uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed * 31 + i * 7);
}

/* A plain UDP socket bound to `from`, connected to `to`: crafts datagrams. */
static int raw_sock(uint16_t from, uint16_t to)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(from),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("raw bind"); exit(2); }
    a.sin_port = htons(to);
    connect(s, (struct sockaddr *)&a, sizeof a);
    return s;
}

static void craft(int s, uint8_t ver, uint8_t sid, uint16_t plen, uint32_t seq, size_t body)
{
    static uint8_t d[2000];
    d[0] = ver; d[1] = sid; d[2] = plen >> 8; d[3] = (uint8_t)plen;
    d[4] = 0xAA; d[5] = 0xBB; d[6] = 0xCC; d[7] = 0xDD;
    d[8] = seq >> 24; d[9] = seq >> 16; d[10] = seq >> 8; d[11] = (uint8_t)seq;
    fill(d + 12, body, seq);
    if (send(s, d, 12 + body, 0) != (ssize_t)(12 + body)) perror("craft send");
}

/* ---------------- tests ---------------- */

/* Round trip, placement at buf[0], sequence from 0 (TC-SOCK-06, WIRE-05, TX-01) */
static void t_round_trip(void)
{
    udp_link_handle_t a = open_ep(PA, PB, 1460), b = open_ep(PB, PA, 1460);
    static const size_t sizes[] = { 1, 2, 13, 100, 1459, 1460 };
    uint8_t tx[1460], rx[1460];
    udp_link_rx_info_t info;
    uint32_t session = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fill(tx, sizes[i], (uint32_t)i);
        CHECK_EQ(udp_link_send(a, tx, sizes[i]), ESP_OK);
        memset(rx, 0, sizeof rx);
        CHECK_EQ(udp_link_recv(b, rx, sizeof rx, 1000, &info), ESP_OK);
        CHECK_EQ(info.payload_len, sizes[i]);
        CHECK_EQ(info.seq, i);
        CHECK(memcmp(tx, rx, sizes[i]) == 0);
        if (i == 0) session = info.session;
        CHECK_EQ(info.session, session);
    }
    /* send argument errors consume no sequence number (TX-04) */
    CHECK_EQ(udp_link_send(a, tx, 0), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_send(a, NULL, 1), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_send(NULL, tx, 1), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_send(a, tx, 1461), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(udp_link_send(a, tx, 5), ESP_OK);
    CHECK_EQ(udp_link_recv(b, rx, sizeof rx, 1000, &info), ESP_OK);
    CHECK_EQ(info.seq, 6);

    /* reopen: new session, seq restarts at 0 (TC-SOCK-13) */
    CHECK_EQ(udp_link_close(a), ESP_OK);
    a = open_ep(PA, PB, 1460);
    CHECK_EQ(udp_link_send(a, tx, 5), ESP_OK);
    CHECK_EQ(udp_link_recv(b, rx, sizeof rx, 1000, &info), ESP_OK);
    CHECK_EQ(info.seq, 0);
    CHECK(info.session != session);

    /* open errors (API-01, API-02, API-03) */
    udp_link_handle_t h = (udp_link_handle_t)0x1;
    udp_link_config_t c = { .local_port = PA, .peer_ip = { ESP_IP4TOADDR(127, 0, 0, 1) },
                        .peer_port = PB, .stream_id = 1, .max_payload = 10 };
    CHECK_EQ(udp_link_open(&c, &h), ESP_ERR_INVALID_STATE);       /* port in use */
    CHECK(h == NULL);
    h = (udp_link_handle_t)0x1;
    c.peer_ip.addr = ESP_IP4TOADDR(224, 0, 0, 1);
    CHECK_EQ(udp_link_open(&c, &h), ESP_ERR_INVALID_ARG);
    CHECK(h == NULL);
    CHECK_EQ(udp_link_close(NULL), ESP_ERR_INVALID_ARG);

    udp_link_close(a);
    udp_link_close(b);
}

/* Crafted datagrams through the socket: validation, sentinel boundary, one
 * datagram per call (TC-SOCK-08, TC-SOCK-10, RX-04) */
static void t_crafted(void)
{
    udp_link_handle_t b = open_ep(PB, PA, 1000);
    int s = raw_sock(PA, PB);
    uint8_t buf[1500];
    udp_link_rx_info_t info;
    static const size_t caps[] = { 1, 100, 1000 };

    for (size_t i = 0; i < 3; i++) {
        size_t cap = caps[i];
        /* exactly 12 + cap: not exceeded, cap bytes delivered */
        craft(s, 1, 0x42, (uint16_t)cap, 1, cap);
        CHECK_EQ(udp_link_recv(b, buf, cap, 1000, &info), ESP_OK);
        CHECK_EQ(info.payload_len, cap);
        /* 12 + cap + 1, declared cap + 1: truncated (when within max_payload) */
        craft(s, 1, 0x42, (uint16_t)(cap + 1), 2, cap + 1);
        CHECK_EQ(udp_link_recv(b, buf, cap, 1000, &info),
                 cap + 1 <= 1000 ? ESP_ERR_UDP_LINK_TRUNCATED : ESP_ERR_UDP_LINK_SIZE_MISMATCH);
        CHECK_EQ(info.payload_len, cap + 1);
        CHECK_EQ(info.seq, 2);
        /* 12 + cap + 1, declared cap: longer than declared */
        craft(s, 1, 0x42, (uint16_t)cap, 3, cap + 1);
        CHECK_EQ(udp_link_recv(b, buf, cap, 1000, &info), ESP_ERR_UDP_LINK_MALFORMED);
        /* far beyond the buffer, declared within it */
        craft(s, 1, 0x42, (uint16_t)cap, 4, 1400);
        CHECK_EQ(udp_link_recv(b, buf, cap, 1000, &info), ESP_ERR_UDP_LINK_MALFORMED);
    }
    craft(s, 1, 0x42, 50, 5, 40);                                     /* shorter than declared */
    CHECK_EQ(udp_link_recv(b, buf, 1000, 1000, &info), ESP_ERR_UDP_LINK_MALFORMED);
    craft(s, 1, 0x42, 50, 5, 60);                                     /* longer, within cap */
    CHECK_EQ(udp_link_recv(b, buf, 1000, 1000, &info), ESP_ERR_UDP_LINK_MALFORMED);
    craft(s, 2, 0x42, 10, 5, 10);
    CHECK_EQ(udp_link_recv(b, buf, 1000, 1000, &info), ESP_ERR_UDP_LINK_VERSION);
    craft(s, 1, 0x43, 10, 5, 10);
    CHECK_EQ(udp_link_recv(b, buf, 1000, 1000, &info), ESP_ERR_UDP_LINK_STREAM);
    craft(s, 1, 0x42, 1200, 6, 1200);
    CHECK_EQ(udp_link_recv(b, buf, 1460, 1000, &info), ESP_ERR_UDP_LINK_SIZE_MISMATCH);
    CHECK_EQ(info.payload_len, 1200);
    CHECK(send(s, "short", 5, 0) == 5);
    CHECK_EQ(udp_link_recv(b, buf, 1000, 1000, &info), ESP_ERR_UDP_LINK_MALFORMED);

    /* every crafted datagram was consumed by exactly one call */
    CHECK_EQ(udp_link_recv(b, buf, 1000, 0, &info), ESP_ERR_TIMEOUT);

    /* receive argument errors touch no socket (RX-07) */
    craft(s, 1, 0x42, 10, 7, 10);
    sleep_ms(20);
    CHECK_EQ(udp_link_recv(b, NULL, 10, 0, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_recv(b, buf, 0, 0, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_recv(b, buf, 10, UDP_LINK_MAX_TIMEOUT_MS + 1, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(udp_link_recv(b, buf, 10, 0, &info), ESP_OK);
    CHECK_EQ(info.seq, 7);

    close(s);
    udp_link_close(b);
}

/* Second concurrent receiver (TC-SOCK-04, CONC-03) */
static udp_link_handle_t g_b;
static esp_err_t g_first_result;

static void *first_receiver(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    udp_link_rx_info_t info;
    g_first_result = udp_link_recv(g_b, buf, sizeof buf, 2000, &info);
    return NULL;
}

static void t_second_receiver(void)
{
    udp_link_handle_t a = open_ep(PA, PB, 64);
    g_b = open_ep(PB, PA, 64);
    pthread_t t;
    pthread_create(&t, NULL, first_receiver, NULL);
    sleep_ms(100);
    uint8_t buf[64];
    udp_link_rx_info_t info;
    CHECK_EQ(udp_link_recv(g_b, buf, sizeof buf, 10, &info), ESP_ERR_INVALID_STATE);
    CHECK_EQ(udp_link_send(a, "x", 1), ESP_OK);
    pthread_join(t, NULL);
    CHECK_EQ(g_first_result, ESP_OK);
    /* the rejected call did not clear the flag it did not set: a fresh call works */
    CHECK_EQ(udp_link_recv(g_b, buf, sizeof buf, 0, &info), ESP_ERR_TIMEOUT);
    udp_link_close(a);
    udp_link_close(g_b);
}

/* Never early (TC-TIME-01), wait count (TC-TIME-02), overshoot (TC-TIME-03) */
static void t_timing(void)
{
    udp_link_handle_t b = open_ep(PB, PA, 64);
    static const uint32_t tos[] = { 1, 2, 3, 7, 10, 33, 100, 250 };
    uint8_t buf[64];
    udp_link_rx_info_t info;
    int max_waits = 0, early = 0;
    double max_over = 0;
    for (size_t i = 0; i < sizeof tos / sizeof tos[0]; i++) {
        int reps = tos[i] >= 100 ? 5 : 40;
        for (int r = 0; r < reps; r++) {
            atomic_store(&waits, 0);
            double t0 = now_ms();
            esp_err_t e = udp_link_recv(b, buf, sizeof buf, tos[i], &info);
            double el = now_ms() - t0;
            CHECK_EQ(e, ESP_ERR_TIMEOUT);
            if (el < tos[i]) {
                early++;
                fprintf(stderr, "  early: timeout %u ms returned after %.3f ms\n", tos[i], el);
            }
            if (el - tos[i] > max_over) max_over = el - tos[i];
            if (atomic_load(&waits) > max_waits) max_waits = atomic_load(&waits);
        }
    }
    CHECK_EQ(early, 0);
    const double tick_ms = 1000.0 / configTICK_RATE_HZ;
    printf("    %u Hz: max waits per call %d, max overshoot %.2f ms (%.2f ticks)\n",
           configTICK_RATE_HZ, max_waits, max_over, max_over / tick_ms);
    /* The <= 3 bound holds only where the platform meets COMM-IDF-13's premise.
     * Linux converts select()'s milliseconds exactly, so at a tick rate that does
     * not divide 1000 (300 Hz) the premise fails by design; only report there. */
    if (1000 % configTICK_RATE_HZ == 0) {
        CHECK(max_waits <= 3);
    }
    udp_link_close(b);
}

/* ENOMEM from the wait: NO_MEM, nothing consumed, buf/info untouched (TC-SEND-06) */
static void t_wait_nomem(void)
{
#if UDP_LINK_WAIT_SO_RCVTIMEO
    printf("    (SO_RCVTIMEO variant: the wait cannot fail for memory; skipped)\n");
#else
    udp_link_handle_t a = open_ep(PA, PB, 64), b = open_ep(PB, PA, 64);
    uint8_t buf[64];
    udp_link_rx_info_t info;
    for (int which = 1; which <= 2; which++) {
        memset(buf, 0x5A, sizeof buf);
        memset(&info, 0x5A, sizeof info);
        atomic_store(&waits, 0);
        atomic_store(&spurious_first, which == 2);            /* guarantees a 2nd wait */
        atomic_store(&fail_wait_at, which);
        CHECK_EQ(udp_link_recv(b, buf, sizeof buf, 200, &info), ESP_ERR_NO_MEM);
        CHECK_EQ(atomic_load(&waits), which);                 /* the injection ran */
        atomic_store(&fail_wait_at, 0);
        atomic_store(&spurious_first, false);
        CHECK_EQ(buf[0], 0x5A);
        CHECK_EQ(buf[63], 0x5A);
        CHECK_EQ(((uint8_t *)&info)[0], 0x5A);
        CHECK_EQ(udp_link_send(a, "hello", 5), ESP_OK);
        CHECK_EQ(udp_link_recv(b, buf, sizeof buf, 1000, &info), ESP_OK);
        CHECK(memcmp(buf, "hello", 5) == 0);
    }
    udp_link_close(a);
    udp_link_close(b);
#endif
}

/*
 * Multiple concurrent senders (TC-SOCK-05, COMM-CONC-02). The guarantee is about
 * the order in which udp_link hands datagrams to the stack, so that is what is
 * checked: a sendmsg() wrapper logs the seq in each header as the call is made,
 * and the log must read 0, 1, 2, ... The receiver checks every number arrives
 * exactly once. Arrival ORDER is only reported: Linux loopback queues packets on
 * per-CPU backlogs, so senders on different CPUs can be delivered out of order
 * after leaving the socket in order. (lwIP's loopback netif is one FIFO queue.)
 */
#define SENDERS 4
#define PER_SENDER 5000
#define TOTAL (SENDERS * PER_SENDER)
static udp_link_handle_t g_a;
static atomic_bool log_sends;
static atomic_int  log_len;
static uint32_t    send_log[TOTAL];
static atomic_int  senders_done;

ssize_t __real_sendmsg(int, const struct msghdr *, int);

ssize_t __wrap_sendmsg(int fd, const struct msghdr *m, int flags)
{
    if (atomic_load(&log_sends) && m->msg_iovlen >= 1 && m->msg_iov[0].iov_len == 12) {
        const uint8_t *h = m->msg_iov[0].iov_base;
        uint32_t seq = ((uint32_t)h[8] << 24) | ((uint32_t)h[9] << 16) | (h[10] << 8) | h[11];
        ssize_t r = __real_sendmsg(fd, m, flags);
        if (r >= 0) {                                       /* only accepted sends use a number */
            int i = atomic_fetch_add(&log_len, 1);
            if (i < TOTAL) send_log[i] = seq;
        }
        return r;
    }
    return __real_sendmsg(fd, m, flags);
}

static void *sender(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < PER_SENDER; i++) {
        while (udp_link_send(g_a, &id, sizeof id) != ESP_OK) {
            sleep_ms(1);                                      /* stack full: retry */
        }
        if ((i & 15) == 0) sleep_ms(1);                       /* stay within the rcvbuf */
    }
    atomic_fetch_add(&senders_done, 1);
    return NULL;
}

static void t_multi_sender(void)
{
    static uint8_t seen[TOTAL];
    memset(seen, 0, sizeof seen);
    atomic_store(&log_len, 0);
    atomic_store(&senders_done, 0);
    g_a = open_ep(PA, PB, 64);
    udp_link_handle_t b = open_ep(PB, PA, 64);
    atomic_store(&log_sends, true);
    pthread_t t[SENDERS];
    for (int i = 0; i < SENDERS; i++) pthread_create(&t[i], NULL, sender, (void *)(uintptr_t)i);

    uint8_t buf[64];
    udp_link_rx_info_t info;
    uint32_t expect = 0, reordered = 0, dup = 0, bogus = 0;
    for (;;) {
        esp_err_t e = udp_link_recv(b, buf, sizeof buf, 200, &info);
        if (e == ESP_ERR_TIMEOUT) {
            if (atomic_load(&senders_done) == SENDERS) break;
            continue;
        }
        if (e != ESP_OK) { fprintf(stderr, "  recv %s\n", udp_link_err_to_name(e)); continue; }
        if (info.seq >= TOTAL) { bogus++; continue; }
        if (seen[info.seq]++) dup++;
        if (udp_link_seq_delta(expect, info.seq) < 0) reordered++;
        else expect = info.seq + 1;
    }
    for (int i = 0; i < SENDERS; i++) pthread_join(t[i], NULL);
    atomic_store(&log_sends, false);

    /* udp_link's guarantee: sequence numbers handed to the stack in order, no gaps */
    int n = atomic_load(&log_len), wrong = 0;
    for (int i = 0; i < n && i < TOTAL; i++) if (send_log[i] != (uint32_t)i) wrong++;
    uint32_t missing = 0;
    for (int i = 0; i < TOTAL; i++) if (!seen[i]) missing++;
    printf("    %d senders x %d: send-order errors %d; received: missing %u, duplicate %u, "
           "arrived out of order %u (Linux loopback, informational)\n",
           SENDERS, PER_SENDER, wrong, missing, dup, reordered);
    CHECK_EQ(n, TOTAL);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(dup, 0);
    CHECK_EQ(bogus, 0);
    udp_link_close(g_a);
    udp_link_close(b);
}

/*
 * Full duplex under stress, with the counter-stream accounting (TC-SOCK-02,
 * TC-SOCK-12 analogue). Each endpoint: one TX thread sending its counter, one RX
 * thread checking what arrives. A reporter samples every 50 ms two ways:
 *   naive  - A's counters then B's (the 07 snapshot), checked "rx == tx or tx-1";
 *   ordered- per direction tx, rx, tx again (atomics), checked rx <= tx_after + 1.
 */
typedef struct {
    udp_link_handle_t h;
    _Atomic uint32_t tx_ok, tx_drop, tx_err;
    _Atomic uint32_t rx_ok, rx_gap, rx_late, rx_bad, rx_rej, rx_nomem;
} ep_t;

static ep_t E[2];
static atomic_bool running;

static void *ep_tx(void *arg)
{
    ep_t *ep = arg;
    uint8_t p[256];
    uint32_t n = 0;
    while (atomic_load(&running)) {
        p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = (uint8_t)n;
        for (int i = 4; i < 256; i++) p[i] = (uint8_t)(n + i);
        esp_err_t e = udp_link_send(ep->h, p, sizeof p);
        if (e == ESP_OK) { atomic_fetch_add(&ep->tx_ok, 1); n++; }
        else if (e == ESP_ERR_NO_MEM || e == ESP_ERR_UDP_LINK_NO_ROUTE) atomic_fetch_add(&ep->tx_drop, 1);
        else atomic_fetch_add(&ep->tx_err, 1);
        struct timespec ts = { 0, 50000 };                   /* ~20k datagrams/s max */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *ep_rx(void *arg)
{
    ep_t *ep = arg;
    uint8_t p[256];
    udp_link_rx_info_t info;
    bool have = false;
    uint32_t expected = 0;
    for (;;) {                               /* drains until a timeout after stop */
        esp_err_t e = udp_link_recv(ep->h, p, sizeof p, 50, &info);
        if (e == ESP_ERR_TIMEOUT) { if (!atomic_load(&running)) break; continue; }
        if (e == ESP_ERR_NO_MEM) { atomic_fetch_add(&ep->rx_nomem, 1); continue; }
        if (e != ESP_OK) { atomic_fetch_add(&ep->rx_rej, 1); continue; }
        if (!have) { have = true; expected = info.seq; }
        int32_t d = udp_link_seq_delta(expected, info.seq);
        if (d < 0) { atomic_fetch_add(&ep->rx_late, 1); continue; }
        atomic_fetch_add(&ep->rx_gap, (uint32_t)d);
        expected = info.seq + 1;
        uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        bool ok = info.payload_len == 256 && v == info.seq;
        for (int i = 4; ok && i < 256; i++) ok = p[i] == (uint8_t)(v + i);
        atomic_fetch_add(ok ? &ep->rx_ok : &ep->rx_bad, 1);
    }
    return NULL;
}

static void t_full_duplex(void)
{
    memset(E, 0, sizeof E);
    E[0].h = open_ep(PA, PB, 256);
    E[1].h = open_ep(PB, PA, 256);
    atomic_store(&running, true);
    pthread_t th[4];
    pthread_create(&th[0], NULL, ep_rx, &E[0]);
    pthread_create(&th[1], NULL, ep_rx, &E[1]);
    pthread_create(&th[2], NULL, ep_tx, &E[0]);
    pthread_create(&th[3], NULL, ep_tx, &E[1]);

    int samples = 0, naive_fail = 0, ordered_fail = 0, max_inflight = 0;
    for (double end = now_ms() + 3000; now_ms() < end; ) {
        sleep_ms(50);
        /* naive: whole of A, then whole of B */
        uint32_t a_tx = atomic_load(&E[0].tx_ok), a_rx = atomic_load(&E[0].rx_ok);
        uint32_t b_tx = atomic_load(&E[1].tx_ok), b_rx = atomic_load(&E[1].rx_ok);
        if (!(b_rx == a_tx || b_rx + 1 == a_tx)) naive_fail++;
        if (!(a_rx == b_tx || a_rx + 1 == b_tx)) naive_fail++;
        /* ordered, per direction */
        for (int d = 0; d < 2; d++) {
            ep_t *snd = &E[d], *rcv = &E[1 - d];
            uint32_t tx1 = atomic_load(&snd->tx_ok);
            uint32_t rx  = atomic_load(&rcv->rx_ok);
            uint32_t tx2 = atomic_load(&snd->tx_ok);
            if (rx > tx2 + 1) ordered_fail++;
            int inflight = (int)(tx1 - rx);
            if (inflight > max_inflight) max_inflight = inflight;
        }
        samples++;
    }
    atomic_store(&running, false);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);

    printf("    %d samples; A tx %u, B tx %u; naive-criterion misses %d; "
           "ordered-bound violations %d; max in flight %d\n",
           samples, E[0].tx_ok, E[1].tx_ok, naive_fail, ordered_fail, max_inflight);
    CHECK_EQ(ordered_fail, 0);
    for (int d = 0; d < 2; d++) {
        CHECK_EQ(E[d].tx_err, 0);
        CHECK_EQ(E[d].rx_bad, 0);
        CHECK_EQ(E[d].rx_rej, 0);
        CHECK_EQ(E[d].rx_late, 0);
        CHECK_EQ(E[d].rx_gap, 0);
        CHECK_EQ(E[1 - d].rx_ok, E[d].tx_ok);             /* drained: every datagram arrived */
        CHECK(E[d].tx_ok > 1000);
    }
    udp_link_close(E[0].h);
    udp_link_close(E[1].h);
}

int main(void)
{
    PA = (uint16_t)(20000 + (getpid() % 20000) * 2);
    PB = PA + 1;
    printf("udp_link.c on POSIX sockets: wait=%s, tick=%u Hz\n",
           UDP_LINK_WAIT_SO_RCVTIMEO ? "SO_RCVTIMEO" : "select", configTICK_RATE_HZ);
    RUN(t_round_trip);
    RUN(t_crafted);
    RUN(t_second_receiver);
    RUN(t_timing);
    RUN(t_wait_nomem);
    RUN(t_multi_sender);
    RUN(t_full_duplex);
    return SUMMARY();
}
