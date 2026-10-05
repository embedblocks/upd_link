/*
 * Host tests of COMM's pure functions: HT-01 .. HT-04, HT-07 .. HT-09 of the
 * verification plan, plus a randomized differential test standing in for HT-02.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "comm.h"
#include "comm_priv.h"
#include "test_common.h"

enum { SID = 0x42, MAXP = 1000 };

/* Build a header with explicit fields (independent of comm_hdr_encode). */
static void hdr(uint8_t h[12], uint8_t ver, uint8_t sid, uint16_t plen, uint32_t ses, uint32_t seq)
{
    h[0] = ver; h[1] = sid; h[2] = plen >> 8; h[3] = plen & 0xff;
    h[4] = ses >> 24; h[5] = ses >> 16; h[6] = ses >> 8; h[7] = ses;
    h[8] = seq >> 24; h[9] = seq >> 16; h[10] = seq >> 8; h[11] = seq;
}

/* Validate a datagram of true length L, as recvmsg would report it under a
 * port that returns bytes copied (conv 0) or the full length (conv 1). */
static esp_err_t val(const uint8_t h[12], size_t L, size_t cap, int conv, comm_rx_info_t *info)
{
    size_t space = 12 + cap + 1;                 /* header + buf + sentinel */
    size_t ret = conv == 0 ? (L < space ? L : space) : L;
    return comm_validate(h, comm_rx_facts(ret, cap), cap, SID, MAXP, info);
}

/* ---- HT-01: validator against every case the rules distinguish ---- */
static void ht01_validator(void)
{
    uint8_t h[12];
    comm_rx_info_t info;
    for (int conv = 0; conv < 2; conv++) {
        /* rule 1 */
        hdr(h, 1, SID, 1, 0, 0);
        CHECK_EQ(val(h, 0, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        CHECK_EQ(val(h, 11, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        /* rule 2 */
        hdr(h, 2, SID, 4, 0, 0);
        CHECK_EQ(val(h, 16, 100, conv, &info), ESP_ERR_COMM_VERSION);
        hdr(h, 0, SID, 4, 0, 0);
        CHECK_EQ(val(h, 16, 100, conv, &info), ESP_ERR_COMM_VERSION);
        /* rule 3 */
        hdr(h, 1, SID + 1, 4, 0, 0);
        CHECK_EQ(val(h, 16, 100, conv, &info), ESP_ERR_COMM_STREAM);
        /* rule 4 */
        hdr(h, 1, SID, 0, 0, 0);
        CHECK_EQ(val(h, 12, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        hdr(h, 1, SID, 1461, 0, 0);
        CHECK_EQ(val(h, 12 + 1461, 1500, conv, &info), ESP_ERR_COMM_MALFORMED);
        /* rule 5: fields reported */
        memset(&info, 0, sizeof info);
        hdr(h, 1, SID, MAXP + 1, 0xA1B2C3D4, 0x01020304);
        CHECK_EQ(val(h, 12 + MAXP + 1, 1460, conv, &info), ESP_ERR_COMM_SIZE_MISMATCH);
        CHECK_EQ(info.payload_len, MAXP + 1);
        CHECK_EQ(info.session, 0xA1B2C3D4u);
        CHECK_EQ(info.seq, 0x01020304u);
        /* rule 6: truncation with a short cap, fields reported */
        memset(&info, 0, sizeof info);
        hdr(h, 1, SID, 200, 7, 9);
        CHECK_EQ(val(h, 12 + 200, 100, conv, &info), ESP_ERR_COMM_TRUNCATED);
        CHECK_EQ(info.payload_len, 200);
        CHECK_EQ(info.seq, 9);
        /* rule 6: longer than declared, beyond 12 + cap and within it */
        hdr(h, 1, SID, 50, 0, 0);
        CHECK_EQ(val(h, 12 + 150, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        CHECK_EQ(val(h, 12 + 60, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        /* rule 6: shorter than declared */
        CHECK_EQ(val(h, 12 + 40, 100, conv, &info), ESP_ERR_COMM_MALFORMED);
        /* boundary: exactly 12 + cap is not exceeded */
        hdr(h, 1, SID, 100, 0, 0);
        CHECK_EQ(val(h, 12 + 100, 100, conv, &info), ESP_OK);
        CHECK_EQ(info.payload_len, 100);
        /* boundary: 12 + cap + 1 with payload_len cap + 1 -> truncated */
        hdr(h, 1, SID, 101, 0, 0);
        CHECK_EQ(val(h, 12 + 101, 100, conv, &info), ESP_ERR_COMM_TRUNCATED);
        /* valid at 1 and at the limit (instance max 1460 for the latter) */
        hdr(h, 1, SID, 1, 0xFFFFFFFF, 0xFFFFFFFE);
        CHECK_EQ(val(h, 13, 1, conv, &info), ESP_OK);
        CHECK_EQ(info.session, 0xFFFFFFFFu);
        CHECK_EQ(info.seq, 0xFFFFFFFEu);
        hdr(h, 1, SID, 1460, 0, 0);
        size_t ret = 12 + 1460;
        CHECK_EQ(comm_validate(h, comm_rx_facts(ret, 1460), 1460, SID, 1460, &info), ESP_OK);
    }
}

/* comm_hdr_encode round trip and byte order */
static void ht01_encode(void)
{
    uint8_t a[12], b[12];
    comm_hdr_encode(a, 0x42, 0x0102, 0x0A0B0C0D, 0x11223344);
    hdr(b, 1, 0x42, 0x0102, 0x0A0B0C0D, 0x11223344);
    CHECK(memcmp(a, b, 12) == 0);
    CHECK_EQ(a[2], 0x01);
    CHECK_EQ(a[4], 0x0A);
    CHECK_EQ(a[11], 0x44);
}

/* ---- HT-02 stand-in: randomized differential test against a reference model
 * written from the true datagram length L, under both return conventions.
 * Agreement under both conventions is the sentinel property (COMM-IDF-03). */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static esp_err_t model(const uint8_t h[12], size_t L, size_t cap, uint16_t maxp, comm_rx_info_t *info)
{
    if (L < 12) return ESP_ERR_COMM_MALFORMED;
    if (h[0] != 1) return ESP_ERR_COMM_VERSION;
    if (h[1] != SID) return ESP_ERR_COMM_STREAM;
    unsigned plen = (h[2] << 8) | h[3];
    if (plen == 0 || plen > 1460) return ESP_ERR_COMM_MALFORMED;
    info->payload_len = plen;
    info->session = ((uint32_t)h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
    info->seq     = ((uint32_t)h[8] << 24) | (h[9] << 16) | (h[10] << 8) | h[11];
    if (plen > maxp) return ESP_ERR_COMM_SIZE_MISMATCH;
    if (L - 12 > cap) return plen > cap ? ESP_ERR_COMM_TRUNCATED : ESP_ERR_COMM_MALFORMED;
    return (L - 12 == plen) ? ESP_OK : ESP_ERR_COMM_MALFORMED;
}

static int reports_fields(esp_err_t e)
{
    return e == ESP_OK || e == ESP_ERR_COMM_TRUNCATED || e == ESP_ERR_COMM_SIZE_MISMATCH;
}

static void ht02_differential(void)
{
    long mismatches = 0, counts[8] = {0};
    for (long i = 0; i < 2000000; i++) {
        size_t   cap  = 1 + rnd() % 1500;
        uint16_t maxp = 1 + rnd() % 1460;
        size_t   L    = rnd() % 1600;
        uint8_t  h[12];
        for (int j = 0; j < 12; j++) h[j] = (uint8_t)rnd();
        if (rnd() % 10) h[0] = 1;
        if (rnd() % 10) h[1] = SID;
        unsigned plen;
        switch (rnd() % 4) {                    /* steer payload_len to the boundaries */
        case 0:  plen = L >= 12 ? (unsigned)(L - 12) : 0; break;
        case 1:  plen = (unsigned)cap + (rnd() % 3) - 1; break;
        case 2:  plen = maxp + (rnd() % 3) - 1; break;
        default: plen = rnd() % 1600; break;
        }
        h[2] = (uint8_t)(plen >> 8); h[3] = (uint8_t)plen;
        if (rnd() % 3 == 0) L = 12 + cap + (rnd() % 3) - 1;   /* sentinel boundary */

        comm_rx_info_t im = {0}, i0 = {0}, i1 = {0};
        esp_err_t em = model(h, L, cap, maxp, &im);
        size_t space = 12 + cap + 1;
        esp_err_t e0 = comm_validate(h, comm_rx_facts(L < space ? L : space, cap), cap, SID, maxp, &i0);
        esp_err_t e1 = comm_validate(h, comm_rx_facts(L, cap), cap, SID, maxp, &i1);
        int bad = e0 != em || e1 != em;
        if (!bad && reports_fields(em)) {
            bad = memcmp(&im, &i0, sizeof im) || memcmp(&im, &i1, sizeof im);
        }
        if (bad && mismatches++ < 5) {
            fprintf(stderr, "  mismatch: L=%zu cap=%zu maxp=%u plen=%u model=%x conv0=%x conv1=%x\n",
                    L, cap, maxp, plen, em, e0, e1);
        }
        counts[em == ESP_OK ? 0 : (em & 7)]++;
    }
    CHECK_EQ(mismatches, 0);
    CHECK(counts[0] > 10000);                   /* the generator reached every outcome */
    for (int k = 2; k <= 6; k++) CHECK(counts[k] > 1000);   /* TRUNCATED .. SIZE_MISMATCH */
}

/* ---- HT-03: comm_seq_delta ---- */
static void ht03_seq_delta(void)
{
    CHECK_EQ(comm_seq_delta(5, 5), 0);
    CHECK_EQ(comm_seq_delta(5, 9), 4);
    CHECK_EQ(comm_seq_delta(9, 5), -4);
    CHECK_EQ(comm_seq_delta(0xFFFFFFFE, 1), 3);             /* across the wrap */
    CHECK_EQ(comm_seq_delta(1, 0xFFFFFFFE), -3);
    CHECK_EQ(comm_seq_delta(0, 0x80000000u), INT32_MIN);    /* exactly 2^31: late */
    CHECK_EQ(comm_seq_delta(0, 0x7FFFFFFFu), INT32_MAX);
}

/* ---- HT-04: comm_err_to_name ---- */
static void ht04_err_to_name(void)
{
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_NO_ROUTE), "ESP_ERR_COMM_NO_ROUTE") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_TRUNCATED), "ESP_ERR_COMM_TRUNCATED") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_MALFORMED), "ESP_ERR_COMM_MALFORMED") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_VERSION), "ESP_ERR_COMM_VERSION") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_STREAM), "ESP_ERR_COMM_STREAM") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_COMM_SIZE_MISMATCH), "ESP_ERR_COMM_SIZE_MISMATCH") == 0);
    CHECK(strcmp(comm_err_to_name(ESP_ERR_TIMEOUT), esp_err_to_name(ESP_ERR_TIMEOUT)) == 0);
}

/* ---- HT-07: peer address classification ---- */
static uint32_t ip(int a, int b, int c, int d) { return ESP_IP4TOADDR(a, b, c, d); }

static void ht07_peer_ip(void)
{
    CHECK(!comm_peer_ip_ok(ip(0, 0, 0, 0)));
    CHECK(!comm_peer_ip_ok(ip(0, 255, 255, 255)));
    CHECK(!comm_peer_ip_ok(ip(224, 0, 0, 0)));
    CHECK(!comm_peer_ip_ok(ip(239, 255, 255, 255)));
    CHECK(!comm_peer_ip_ok(ip(240, 0, 0, 0)));
    CHECK(!comm_peer_ip_ok(ip(255, 255, 255, 255)));
    for (int a = 0; a < 256; a++) {
        CHECK_EQ(comm_peer_ip_ok(ip(a, 17, 3, 9)), a != 0 && a < 224);
    }
    CHECK(comm_peer_ip_ok(ip(127, 0, 0, 1)));
    CHECK(comm_peer_ip_ok(ip(1, 0, 0, 0)));
    CHECK(comm_peer_ip_ok(ip(223, 255, 255, 255)));
    CHECK(comm_peer_ip_ok(ip(192, 168, 1, 20)));

    comm_config_t c = { .local_port = 1, .peer_ip = { ip(10, 0, 0, 1) }, .peer_port = 1,
                        .stream_id = 0, .max_payload = 1 };
    CHECK_EQ(comm_check_open_args(&c, true), ESP_OK);
    CHECK_EQ(comm_check_open_args(NULL, true), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_open_args(&c, false), ESP_ERR_INVALID_ARG);
    c.local_port = 0; CHECK_EQ(comm_check_open_args(&c, true), ESP_ERR_INVALID_ARG); c.local_port = 1;
    c.peer_port = 0;  CHECK_EQ(comm_check_open_args(&c, true), ESP_ERR_INVALID_ARG); c.peer_port = 1;
    c.max_payload = 0;    CHECK_EQ(comm_check_open_args(&c, true), ESP_ERR_INVALID_ARG);
    c.max_payload = 1461; CHECK_EQ(comm_check_open_args(&c, true), ESP_ERR_INVALID_ARG);
    c.max_payload = 1460; CHECK_EQ(comm_check_open_args(&c, true), ESP_OK);
    c.peer_ip.addr = ip(255, 255, 255, 255); CHECK_EQ(comm_check_open_args(&c, true), ESP_ERR_INVALID_ARG);
}

/* ---- HT-08: send and receive argument checks ---- */
static void ht08_args(void)
{
    uint8_t p[4];
    comm_rx_info_t info;
    CHECK_EQ(comm_check_send_args(false, p, 1, 10), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_send_args(true, NULL, 1, 10), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_send_args(true, p, 0, 10), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_send_args(true, p, 11, 10), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(comm_check_send_args(true, p, 10, 10), ESP_OK);

    CHECK_EQ(comm_check_recv_args(false, p, 4, 0, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_recv_args(true, NULL, 4, 0, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_recv_args(true, p, 4, 0, NULL), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_recv_args(true, p, 0, 0, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_recv_args(true, p, 4, COMM_MAX_TIMEOUT_MS + 1, &info), ESP_ERR_INVALID_ARG);
    CHECK_EQ(comm_check_recv_args(true, p, 4, COMM_MAX_TIMEOUT_MS, &info), ESP_OK);
    CHECK_EQ(comm_check_recv_args(true, p, 4, 0, &info), ESP_OK);
    CHECK_EQ(comm_check_recv_args(true, p, 4, COMM_WAIT_FOREVER, &info), ESP_OK);
}

/* ---- HT-09: timeout conversion at every tick rate 1..1000 Hz ---- */
static void ht09_ticks(void)
{
    static const uint32_t ms[] = { 0, 1, 2, 3, 4, 7, 9, 10, 11, 99, 100, 101, 333, 999, 1000,
                                   1001, 123457, COMM_MAX_TIMEOUT_MS };
    long bad = 0;
    for (uint32_t hz = 1; hz <= 1000; hz++) {
        for (size_t i = 0; i < sizeof ms / sizeof ms[0]; i++) {
            uint64_t num = (uint64_t)ms[i] * hz;
            uint64_t exact_ceil = num / 1000 + (num % 1000 != 0);
            uint32_t t = comm_timeout_ticks(ms[i], hz);
            if (t != exact_ceil) bad++;
            if ((uint64_t)t + 1 >= (1ull << 31)) bad++;       /* deadline < 2^31 ahead */
            /* t ticks cover at least ms: t * 1000 / hz >= ms */
            if ((uint64_t)t * 1000 < num) bad++;
        }
    }
    CHECK_EQ(bad, 0);

    CHECK_EQ(comm_wait_k(1), 1);
    CHECK_EQ(comm_wait_k(2), 1);
    CHECK_EQ(comm_wait_k(3), 2);
    CHECK_EQ(comm_wait_k(1000), 999);
}

/* ---- errno mapping (COMM-IDF-08) ---- */
static void errno_mapping(void)
{
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_SOCKET, ENFILE), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_SOCKET, ENOBUFS), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_SOCKET, ENOMEM), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_SOCKET, EINVAL), ESP_FAIL);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_BIND, EADDRINUSE), ESP_ERR_INVALID_STATE);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_BIND, ENOMEM), ESP_FAIL);
    CHECK_EQ(comm_map_open_errno(COMM_OPEN_CONNECT, EADDRINUSE), ESP_FAIL);

    CHECK_EQ(comm_map_send_errno(ENOMEM), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_send_errno(ENOBUFS), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_send_errno(EAGAIN), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_send_errno(EWOULDBLOCK), ESP_ERR_NO_MEM);
    CHECK_EQ(comm_map_send_errno(EHOSTUNREACH), ESP_ERR_COMM_NO_ROUTE);
    CHECK_EQ(comm_map_send_errno(ENETUNREACH), ESP_ERR_COMM_NO_ROUTE);
    CHECK_EQ(comm_map_send_errno(EIO), ESP_ERR_COMM_NO_ROUTE);
    CHECK_EQ(comm_map_send_errno(ECONNREFUSED), ESP_FAIL);
    CHECK_EQ(comm_map_send_errno(EBADF), ESP_FAIL);
}

int main(void)
{
    RUN(ht01_validator);
    RUN(ht01_encode);
    RUN(ht02_differential);
    RUN(ht03_seq_delta);
    RUN(ht04_err_to_name);
    RUN(ht07_peer_ip);
    RUN(ht08_args);
    RUN(ht09_ticks);
    RUN(errno_mapping);
    return SUMMARY();
}
