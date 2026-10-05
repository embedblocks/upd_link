# udp_link — Core Specification

**Status:** draft 0.1, generated from design files 00–12. Not yet verified on target.

This file is everything needed to implement udp_link. It states **what the code does**. Why it does it (09), how it is verified (10), what the platform was observed to do (12), and pipe-side material (receiver policy, reference pipe) live elsewhere and are not needed to write the code.

**Rule for this file:** if a plausible implementation could violate a rule, the rule is written here, in one place. Tags such as `[COMM-RX-01]` match the requirement identifiers used by the verification plan.

---

## 1. What udp_link is

- An ESP-IDF component that moves opaque payloads between two peers over **IPv4 unicast UDP**. One instance = one socket, one peer, one stream.
- A **mechanism**, not a policy. udp_link reports facts and returns; the calling pipe decides what to do.
- udp_link has **no** tasks, timers, callbacks, queues, counters, retransmission, reordering, de-duplication, fragmentation, keepalive, authentication or QoS marking.
- Every function runs in the calling task. **None may be called from an ISR.**
- udp_link allocates exactly once: the instance, in `udp_link_open`. lwIP and VFS may allocate inside the calls udp_link makes; that is theirs.
- udp_link has no Kconfig options. All configuration is per instance through `udp_link_config_t`.

## 2. Public header

`include/udp_link.h`, verbatim. Until the first generation this block is authoritative; afterwards `udp_link.h` is authoritative for declarations and this spec for their meaning.

```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_LINK_HEADER_SIZE          12
#define UDP_LINK_MAX_PAYLOAD_LIMIT    1460
#define UDP_LINK_WAIT_FOREVER         UINT32_MAX
#define UDP_LINK_MAX_TIMEOUT_MS       (24u * 60u * 60u * 1000u)

#define ESP_ERR_UDP_LINK_BASE           0x70000     /* not 0x7000 (ESP_ERR_HTTP_BASE) */
#define ESP_ERR_UDP_LINK_NO_ROUTE       (ESP_ERR_UDP_LINK_BASE + 1)
#define ESP_ERR_UDP_LINK_TRUNCATED      (ESP_ERR_UDP_LINK_BASE + 2)
#define ESP_ERR_UDP_LINK_MALFORMED      (ESP_ERR_UDP_LINK_BASE + 3)
#define ESP_ERR_UDP_LINK_VERSION        (ESP_ERR_UDP_LINK_BASE + 4)
#define ESP_ERR_UDP_LINK_STREAM         (ESP_ERR_UDP_LINK_BASE + 5)
#define ESP_ERR_UDP_LINK_SIZE_MISMATCH  (ESP_ERR_UDP_LINK_BASE + 6)

typedef struct udp_link_instance *udp_link_handle_t;

typedef struct {
    uint16_t       local_port;    /* UDP port this node sends from and receives on */
    esp_ip4_addr_t peer_ip;       /* peer's unicast IPv4 address (network order)   */
    uint16_t       peer_port;     /* the peer's local_port                         */
    uint8_t        stream_id;     /* must match the peer's configured value        */
    uint16_t       max_payload;   /* 1 .. UDP_LINK_MAX_PAYLOAD_LIMIT                   */
} udp_link_config_t;

typedef struct {
    size_t   payload_len;         /* bytes of payload                              */
    uint32_t seq;                 /* sender's sequence number                      */
    uint32_t session;             /* sender's session identifier                   */
} udp_link_rx_info_t;

esp_err_t udp_link_open (const udp_link_config_t *cfg, udp_link_handle_t *out);
esp_err_t udp_link_close(udp_link_handle_t h);

esp_err_t udp_link_send (udp_link_handle_t h, const void *payload, size_t len);
esp_err_t udp_link_recv (udp_link_handle_t h, void *buf, size_t cap,
                     uint32_t timeout_ms, udp_link_rx_info_t *info);

const char *udp_link_err_to_name(esp_err_t err);

static inline int32_t udp_link_seq_delta(uint32_t expected, uint32_t received)
{
    return (int32_t)(received - expected);
}

#ifdef __cplusplus
}
#endif
```

## 3. Wire format

Every datagram is a 12-byte header followed by the payload. One payload = exactly one UDP datagram.

| Offset | Field | Type | Value |
| --- | --- | --- | --- |
| 0 | `version` | u8 | `1` |
| 1 | `stream_id` | u8 | sender's configured stream ID |
| 2 | `payload_len` | u16 | payload bytes that follow |
| 4 | `session` | u32 | sender's session |
| 8 | `seq` | u32 | sender's sequence number |
| 12 | payload | bytes | opaque |

- **[COMM-WIRE-01]** All multi-byte fields are **big-endian**. No padding, no reserved fields.
- **[COMM-WIRE-02]** A receiver accepts only versions it implements (this spec: `1`). Offsets 0 and 1 mean `version` and `stream_id` in every version. The header is **12 bytes in every version**; the receive path depends on that.
- **[COMM-WIRE-03]** `1 ≤ payload_len ≤ max_payload ≤ 1460`. Largest datagram: 20 + 8 + 12 + 1460 = 1500 bytes. udp_link never relies on IP fragmentation; choosing `max_payload` for the path MTU is the integrator's job.
- **[COMM-WIRE-04] Session.** `udp_link_open` draws a new 32-bit value from `esp_random()`. It is constant for the life of the instance. It carries no order.
- **[COMM-WIRE-05] Sequence.** The first datagram of a session carries `seq = 0`. Each datagram **the local stack accepts** increments `seq` by one, modulo 2³². A send the stack rejects consumes no number.

## 4. Validation of a received datagram

**[COMM-VAL-01]** Rules apply in order; the first that fails decides the result.

| # | Rule | On failure | `info` |
| --- | --- | --- | --- |
| 1 | at least 12 bytes received | `ESP_ERR_UDP_LINK_MALFORMED` | not written |
| 2 | `version == 1` | `ESP_ERR_UDP_LINK_VERSION` | not written |
| 3 | `stream_id` equals the instance's | `ESP_ERR_UDP_LINK_STREAM` | not written |
| 4 | `1 ≤ payload_len ≤ 1460` | `ESP_ERR_UDP_LINK_MALFORMED` | not written |
| 5 | `payload_len ≤` instance `max_payload` | `ESP_ERR_UDP_LINK_SIZE_MISMATCH` | written |
| 6 | datagram length agrees with `payload_len` | see below | see below |

**[COMM-VAL-02]** Rule 6 uses two facts about the receive (how they are obtained is §9.2):

- **exceeded** — the datagram was longer than 12 + `cap`;
- **r** — payload bytes delivered, when not exceeded.

| Case | Result | `info` |
| --- | --- | --- |
| exceeded and `payload_len > cap` | `ESP_ERR_UDP_LINK_TRUNCATED` (buffer too small) | written |
| exceeded and `payload_len ≤ cap` | `ESP_ERR_UDP_LINK_MALFORMED` (longer than declared) | not written |
| not exceeded and `r == payload_len` | `ESP_OK` | written |
| not exceeded, any other `r` | `ESP_ERR_UDP_LINK_MALFORMED` | not written |

"Written" means `payload_len` (the value the header declares), `seq` and `session` are filled in.

Source address and UDP checksum are not validation rules: the connected socket filters by source, and the stack drops checksum failures before udp_link sees them.

## 5. Instance

**[COMM-IDF-10]** One allocation, `heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`, holding:

| Field | Written by | Guard |
| --- | --- | --- |
| copy of `udp_link_config_t` | `udp_link_open` | read-only afterwards |
| socket descriptor | `udp_link_open` | read-only afterwards |
| TX mutex, **static storage inside the instance** (`xSemaphoreCreateMutexStatic`) | `udp_link_open` | — |
| TX sequence | `udp_link_send` | TX mutex |
| previous socket-level send result (for the no-route log) | `udp_link_send` | TX mutex |
| session | `udp_link_open` | read-only afterwards |
| RX in-progress flag (`atomic_bool`) | `udp_link_recv` | atomic exchange |
| wait-primitive state, if the variant needs any (§9.4) | `udp_link_recv` | single-receiver rule |

Internal RAM because it holds a FreeRTOS mutex. No other udp_link allocation ever happens.

## 6. `udp_link_open`

**Argument check** (pure function, before anything is allocated) — `ESP_ERR_INVALID_ARG` **[COMM-API-01]** for:

- null `cfg` or `out`;
- `local_port == 0` or `peer_port == 0`;
- `max_payload` outside `1 … 1460`;
- `peer_ip` whose first octet is `0` (`0.0.0.0/8`) or `≥ 224` (`224.0.0.0/4` multicast, `240.0.0.0/4` reserved incl. `255.255.255.255`). `127.0.0.0/8` is accepted. Read the first octet from the bytes of `peer_ip.addr` (it is in network order), not by shifting the integer.

**Steps:**

1. Set `*out = NULL` (if `out` is non-null) before anything else.
2. Allocate the instance (§5); copy `cfg` **[COMM-API-07]**. Create the TX mutex, `seq = 0`, previous result `ESP_OK`, RX flag clear.
3. **[COMM-IDF-01]** `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`; `bind()` to `INADDR_ANY:local_port`; `connect()` to `peer_ip:peer_port`. **Set no socket option** (select variant; see §9.4 for the other).
4. `session = esp_random()`.
5. Store the handle in `*out`, return `ESP_OK`.

**Errors [COMM-API-02]:**

| Condition | Result |
| --- | --- |
| instance allocation fails | `ESP_ERR_NO_MEM` |
| `socket()` fails with `ENFILE`, `ENOBUFS`, `ENOMEM` | `ESP_ERR_NO_MEM` |
| `bind()` fails with `EADDRINUSE` | `ESP_ERR_INVALID_STATE` |
| any other failure of `socket`/`bind`/`connect` | `ESP_FAIL` |

**[COMM-API-03]** On any failure: close the socket if created, delete the mutex, free the instance, leave `*out == NULL`. Nothing remains allocated.

**[COMM-API-08]** `udp_link_open` does not check or wait for the network interface. Precondition (not detected): `esp_netif_init()` has been called.

## 7. `udp_link_close`

**[COMM-API-04]**

1. Null handle → return `ESP_ERR_INVALID_ARG`, do nothing else.
2. **[COMM-LIFE-03]** Unless `NDEBUG` is defined (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE`): read the RX flag; take the TX mutex with **zero wait**, and if the take succeeded **give it back immediately**; then `assert` that the RX flag was clear and that the take succeeded. The mutex is never left held.
3. `close()` the socket, **ignoring the result**.
4. Delete the mutex, free the instance, return `ESP_OK`.

Precondition (the caller's job, only partly detected by step 2): no `udp_link_send` or `udp_link_recv` on this instance is in progress, and none starts after `udp_link_close` starts. Afterwards the handle is invalid.

## 8. `udp_link_send`

**Results [COMM-TX-05]:**

| Result | Meaning |
| --- | --- |
| `ESP_OK` | the local stack accepted the datagram (not: delivered) |
| `ESP_ERR_INVALID_ARG` | null handle or payload, or `len == 0` |
| `ESP_ERR_INVALID_SIZE` | `len > max_payload` |
| `ESP_ERR_NO_MEM` | the stack could not accept the datagram now |
| `ESP_ERR_UDP_LINK_NO_ROUTE` | no route or no link to the peer |
| `ESP_FAIL` | any other socket error |

**Algorithm:**

```
check args (pure)                              -> INVALID_ARG / INVALID_SIZE, before mutex or socket
take TX mutex (wait forever)
    encode header on the stack with h->seq     (big-endian)
    n = sendmsg(sock, iov = [header 12 B | caller payload, len], MSG_DONTWAIT)
    result = (n >= 0) ? ESP_OK : map(errno)
    if result == ESP_OK: h->seq += 1
    log_now = (result == NO_ROUTE && h->prev_result != NO_ROUTE)
    h->prev_result = result
give TX mutex
if log_now: ESP_LOGW("udp_link", "no route to peer (errno %d)", saved_errno)
return result
```

Rules:

- **[COMM-CONC-02]** The mutex is held **from reading `seq` until `sendmsg` returns**. That makes wire order equal sequence order and lets a failed send leave `seq` unchanged.
- **[COMM-TX-01]** One call, one datagram: header + payload through a **two-element gather** (`sendmsg`). udp_link does not copy the payload.
- **[COMM-TX-02]** The caller owns `payload` again when `udp_link_send` returns, for any result. This depends on `LWIP_NETIF_TX_SINGLE_PBUF` (§12).
- **[COMM-TX-03] / [COMM-IDF-06]** Never wait for buffer space: always pass `MSG_DONTWAIT`.
- **[COMM-TX-04]** `seq` advances only on `ESP_OK`.
- The no-route warning is logged **only on the transition** into `ESP_ERR_UDP_LINK_NO_ROUTE`, and **after the mutex is released** (errno saved inside). Argument errors do not update the previous result.

**Errno mapping [COMM-IDF-08]:**

| errno from `sendmsg` | Result |
| --- | --- |
| `ENOMEM`, `ENOBUFS`, `EAGAIN`, `EWOULDBLOCK` | `ESP_ERR_NO_MEM` |
| `EHOSTUNREACH`, `ENETUNREACH`, `EIO` | `ESP_ERR_UDP_LINK_NO_ROUTE` |
| anything else | `ESP_FAIL` |

`EIO` is lwIP's `ERR_IF` (the Wi-Fi driver refused to transmit, e.g. station disconnected). `EAGAIN` is included because `MSG_DONTWAIT` can produce it; it means exactly "could not accept the datagram now".

## 9. `udp_link_recv`

### 9.1 Results

**[COMM-RX-05]**

| Result | `buf` | `info` | Meaning |
| --- | --- | --- | --- |
| `ESP_OK` | payload at `buf[0]`, `info->payload_len` bytes | written | accepted |
| `ESP_ERR_TIMEOUT` | unchanged | not written | nothing arrived within `timeout_ms` |
| `ESP_ERR_UDP_LINK_TRUNCATED` | first `cap` payload bytes | written | datagram larger than `buf` |
| `ESP_ERR_UDP_LINK_SIZE_MISMATCH` | unspecified | written | header declares more than `max_payload` |
| `ESP_ERR_UDP_LINK_MALFORMED` / `_VERSION` / `_STREAM` | unspecified | not written | rejected datagram |
| `ESP_ERR_NO_MEM` | unchanged | not written | the wait could not get memory; **no datagram consumed** |
| `ESP_ERR_INVALID_ARG` | unchanged | not written | null handle/`buf`/`info`, `cap == 0`, or finite `timeout_ms > UDP_LINK_MAX_TIMEOUT_MS` |
| `ESP_ERR_INVALID_STATE` | unchanged | not written | another `udp_link_recv` is in progress on this instance |
| `ESP_FAIL` | unspecified | not written | any other socket error |

- **[COMM-RX-02]** `timeout_ms == 0`: one attempt, no wait. `UDP_LINK_WAIT_FOREVER`: no limit. Other values must be `≤ UDP_LINK_MAX_TIMEOUT_MS`.
- **[COMM-RX-07]** Argument and state checks happen **before any socket call**.
- **[COMM-CONC-03]** State check: `atomic_exchange(&rx_busy, true)`; if it was already true → `ESP_ERR_INVALID_STATE` (and do **not** clear the flag). Every other return path clears it.
- **[COMM-RX-04]** Each call consumes at most one datagram, accepted or rejected.
- **[COMM-RX-08]** udp_link keeps no reference to `buf` after returning.
- **[COMM-RX-06]** `ESP_ERR_NO_MEM` only when the *wait* failed for lack of memory; the caller may simply call again.

### 9.2 Scatter receive and the sentinel

**[COMM-RX-03] / [COMM-IDF-03]** Every receive attempt is one `recvmsg` with three iovecs:

```
iov[0] = header     12 bytes   (udp_link's stack)
iov[1] = buf        cap bytes  (caller)          -> payload lands at buf[0]
iov[2] = sentinel   1 byte     (udp_link's stack)    -> reached only by a datagram longer than 12 + cap
```

From `ret` (the `recvmsg` return value):

```
exceeded = ret > 12 + cap
received = min(ret, 12 + cap)          // rule 1 uses received >= 12
r        = received - 12               // rule 6, only when !exceeded and received >= 12
```

Do **not** use `MSG_TRUNC`. The sentinel makes `exceeded` correct whether the port returns the bytes copied or the full datagram length. The sentinel's contents are never read.

### 9.3 The receive loop

**Tick conversion** (integer, exact; do **not** use `portTICK_PERIOD_MS` here):

```c
timeout_ticks = (uint32_t)(((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000);
```

**Loop:**

```
c0 = xTaskGetTickCount()
D  = c0 + timeout_ticks + 1                     // only for finite timeouts; the +1 is essential
io = attempt(no wait)                           // recvmsg(..., MSG_DONTWAIT)
loop:
    GOT        -> return validate(...)          // §4
    WAIT_NOMEM -> return ESP_ERR_NO_MEM
    ERROR      -> return ESP_FAIL
    EMPTY:
        if timeout_ms == 0: return ESP_ERR_TIMEOUT
        if finite:
            c = xTaskGetTickCount()
            if (int32_t)(c - D) >= 0: return ESP_ERR_TIMEOUT
            R = D - c                           // >= 1
            k = max(R - 1, 1)                   // ticks
        else:
            k = FOREVER
        io = wait_then_attempt(k)               // §9.4
```

- **[COMM-RX-01] / [COMM-IDF-07] Never early.** `ESP_ERR_TIMEOUT` is returned **only** from the deadline check. Whatever the wait reports, and however long it waited, the loop goes back to an attempt and then to the check. The `+1` tick makes `D` late enough even when `c0` was read just before a tick boundary. Compare with the signed difference so tick wraparound is handled.
- **[COMM-IDF-13] Blocks and ends.** `k ≥ 1` always, and `R` is recomputed after every wait. On a platform where a wait of `k` ticks lasts at least `max(k − 1, 1)` ticks and a bounded time, a finite call with no traffic makes **at most three** waits.
- **Attempt before every wait.** A queued datagram is received without waiting. The wait runs only after an `EAGAIN` in the same call, so when the wait fails **no datagram has been consumed** — this is what makes `ESP_ERR_NO_MEM` safe to repeat. `ENOMEM` from `recvmsg` itself is `ESP_FAIL`.
- `UDP_LINK_MAX_TIMEOUT_MS` keeps `D` less than 2³¹ ticks ahead at any tick rate 1…1000 Hz.

### 9.4 Wait primitive (isolated; decision open)

The loop calls exactly one internal function for waiting, `wait_then_attempt(k)`. It returns `GOT` / `EMPTY` / `WAIT_NOMEM` / `ERROR` with the same meaning as the first attempt. `k` is in ticks; it is passed to the socket API as `k × portTICK_PERIOD_MS` milliseconds (`portTICK_PERIOD_MS` is used here because it is what the port divides by).

| Variant | `wait_then_attempt(k)` | Extra state | Notes |
| --- | --- | --- | --- |
| **`select` (default)** | `select(sock+1, {sock}, …, timeout k)`; on `-1`: `ENOMEM` → `WAIT_NOMEM`, else `ERROR`; otherwise `recvmsg(MSG_DONTWAIT)` | none | needs `CONFIG_VFS_SUPPORT_SELECT`; the socket carries no option |
| `SO_RCVTIMEO` (build with `UDP_LINK_WAIT_SO_RCVTIMEO=1`) | if `k` differs from the cached value, `setsockopt(SO_RCVTIMEO, k)` (a `timeval` of zero means "forever"); then blocking `recvmsg(flags = 0)`; `EAGAIN`/`EWOULDBLOCK` → `EMPTY` | cached `k` in the instance (safe: single receiver) | never returns `WAIT_NOMEM`; `setsockopt` failure → `ERROR` |

Which one ships is decided by the wait-primitive comparison (TC-MEM-04). Nothing outside this function depends on the choice.

### 9.5 Errno mapping (receive)

| Condition | Result |
| --- | --- |
| deadline reached, nothing received | `ESP_ERR_TIMEOUT` |
| `EAGAIN` / `EWOULDBLOCK` before the deadline | not a result; loop |
| the wait failed with `ENOMEM` (select variant) | `ESP_ERR_NO_MEM` |
| any other errno | `ESP_FAIL` |

## 10. Concurrency rules (per instance)

| Concurrent calls | Allowed | Mechanism |
| --- | --- | --- |
| `udp_link_send` ∥ `udp_link_recv` | yes **[COMM-CONC-01]** | TX touches only the mutex and `seq`; RX only its flag (and the wait state). **No lock spans both directions.** |
| `udp_link_send` ∥ `udp_link_send` | yes | serialized by the TX mutex |
| `udp_link_recv` ∥ `udp_link_recv` | no | second gets `ESP_ERR_INVALID_STATE` |
| `udp_link_close` ∥ anything | no | caller's precondition; partly asserted |

Separate instances share nothing.

Stopping a receiver is done with a finite timeout; nothing local wakes a `UDP_LINK_WAIT_FOREVER` wait. Closing a socket another task is blocked on is not used as a wake-up.

## 11. Helpers

- **[COMM-API-05]** `udp_link_seq_delta(expected, received)` = `(int32_t)(received − expected)`: modular difference read as signed; exactly 2³¹ is `INT32_MIN` (late). Relies on GCC's modular conversion.
- **[COMM-API-06]** `udp_link_err_to_name(err)` returns `"ESP_ERR_UDP_LINK_NO_ROUTE"` etc. for udp_link's six codes and `esp_err_to_name(err)` for anything else. Static strings.

## 12. Build requirements

Manifest (`idf_component.yml`) **[COMM-IDF-11]**:

```yaml
dependencies:
  idf: ">=6.0,<6.1"
```

Build-time checks **[COMM-IDF-12]**, compiled into udp_link (ESP-IDF builds only):

```c
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "lwip/opt.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
#error "udp_link: verified on ESP-IDF 6.0.x only"
#endif
#if !LWIP_NETIF_TX_SINGLE_PBUF
#error "udp_link: TX payload lifetime requires LWIP_NETIF_TX_SINGLE_PBUF"
#endif
#if !LWIP_NETCONN_FULLDUPLEX || !LWIP_NETCONN_SEM_PER_THREAD
#error "udp_link: full duplex requires LWIP_NETCONN_FULLDUPLEX and LWIP_NETCONN_SEM_PER_THREAD"
#endif
#if !UDP_LINK_WAIT_SO_RCVTIMEO && !defined(CONFIG_VFS_SUPPORT_SELECT)
#error "udp_link: the select() wait requires CONFIG_VFS_SUPPORT_SELECT"
#endif
#if !CHECKSUM_GEN_UDP || !CHECKSUM_CHECK_UDP
#error "udp_link: requires UDP checksums; set CONFIG_LWIP_CHECKSUM_CHECK_UDP=y"
#endif
```

A project using udp_link sets `CONFIG_LWIP_CHECKSUM_CHECK_UDP=y` (stack-wide; affects every UDP user on the node).

Component dependencies: `esp_netif` (public, for `esp_ip4_addr_t`), `lwip`, `freertos`, `esp_hw_support` (`esp_random`), `heap`, `log`.

## 13. Code organization

```
udp_link/                         component root (the folder name is the component name)
  CMakeLists.txt, idf_component.yml
  include/udp_link.h              public API (§2)
  private/udp_link_priv.h         pure functions, for host tests; not public
  src/udp_link_pure.c             pure functions: no socket, no RTOS, no state
  src/udp_link.c                  instance, socket calls, mutex, receive loop, wait variants
  src/udp_link_build_checks.h     §12
  examples/counter_stream/        bring-up and test app
  host_test/                      host tests
```

Pure functions (host-testable; everything that needs no socket goes here):

```c
void            udp_link_hdr_encode(uint8_t out[12], uint8_t stream_id, uint16_t payload_len,
                                uint32_t session, uint32_t seq);
udp_link_rx_facts_t udp_link_rx_facts(size_t ret, size_t cap);             /* §9.2 */
esp_err_t       udp_link_validate(const uint8_t hdr[12], udp_link_rx_facts_t facts, size_t cap,
                              uint8_t stream_id, uint16_t max_payload,
                              udp_link_rx_info_t *info);               /* §4 */
bool            udp_link_peer_ip_ok(uint32_t addr_network_order);      /* §6 */
esp_err_t       udp_link_check_open_args(const udp_link_config_t *cfg, bool out_ok);
esp_err_t       udp_link_check_send_args(bool handle_ok, const void *payload, size_t len,
                                     uint16_t max_payload);
esp_err_t       udp_link_check_recv_args(bool handle_ok, const void *buf, size_t cap,
                                     uint32_t timeout_ms, const udp_link_rx_info_t *info);
uint32_t        udp_link_timeout_ticks(uint32_t timeout_ms, uint32_t tick_hz);  /* §9.3 */
uint32_t        udp_link_wait_k(uint32_t remaining_ticks);                      /* max(R-1,1) */
esp_err_t       udp_link_map_open_errno(udp_link_open_step_t step, int err);        /* §6 */
esp_err_t       udp_link_map_send_errno(int err);                               /* §8 */
```

The validator knows nothing of sessions, expectations or receiver policy.

## 14. Rules that are easy to get wrong

An implementation that looks reasonable but breaks one of these is wrong:

1. The **`+1` tick** in the deadline. Without it a call can time out almost one tick early.
2. Only the **deadline check** returns `ESP_ERR_TIMEOUT` — never the wait's own timeout.
3. `k = max(R − 1, 1)`, **never 0** (0 means "forever" to lwIP's timed waits).
4. **Attempt before wait**, so `ESP_ERR_NO_MEM` never follows a consumed datagram.
5. The **1-byte sentinel** iovec; do not rely on `MSG_TRUNC`.
6. **Header in its own iovec** on both paths; payload at `buf[0]`, `cap` sized for payload only.
7. TX mutex held **through `sendmsg`**, not just around the `seq` read.
8. **`seq` advances only on `ESP_OK`.**
9. **`MSG_DONTWAIT`** on every send.
10. No-route log **outside** the mutex, and only on the transition.
11. The second concurrent `udp_link_recv` must **not clear** the busy flag it did not set.
12. `udp_link_close` never leaves the TX mutex held, even in its misuse check.
13. `peer_ip.addr` is **network order**; classify by its first byte in memory.
14. The tick conversion is exact 64-bit arithmetic with `configTICK_RATE_HZ`, not via `portTICK_PERIOD_MS`.
15. `ESP_ERR_NO_MEM` from `udp_link_recv` is a **local condition**: callers retry, and do not count it as a rejected datagram.

## 15. Changes from the 13-file design

| Change | Reason |
| --- | --- |
| `EAGAIN`/`EWOULDBLOCK` from `sendmsg` → `ESP_ERR_NO_MEM` | matches the result's definition, "could not accept now"; closes a gap in the mapping |
| no-route log emitted after releasing the TX mutex | keeps UART time out of other senders' wait |
| wait isolated behind `wait_then_attempt`, both variants implemented | lets TC-MEM-04 run on generated code; the rest of udp_link is independent of the choice |
| `assert()` / `NDEBUG` for the close-time misuse check | this is the mechanism behind `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE` |
| previous send result tracks socket-level results only | argument errors never touch the socket |
