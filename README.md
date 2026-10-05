# COMM

UDP transport mechanism for ESP-IDF: one socket, one peer, one stream; a 12-byte header, sequence numbers, validation. No tasks, no policy.

| Path | What |
| --- | --- |
| `COMM_SPEC.md` | The core spec: everything needed to implement COMM, in one file |
| `components/comm/` | The component, generated from the spec |
| `examples/counter_stream/` | Bring-up and test app (loopback on one board, or two boards) |
| `host_test/` | Host tests: pure functions, and `comm.c` on Linux sockets |

The 13 original design files stay as the assurance and guide material (decisions, verification plan, platform record, receiver policy, reference pipe).

## Run the counter stream on a board

Requires ESP-IDF 6.0.x (the component's build checks and manifest enforce it).

```sh
cd examples/counter_stream
idf.py set-target esp32s3        # or your target
idf.py build flash monitor
```

Loopback mode is the default. Every second it prints both endpoints' counters and a verdict:

```
I (5012) comm_counter: report 5: PASS (inflight A->B 0, B->A 0; failed reports so far 0)
```

A `FAIL` means one of: a defect counter (`tx err`, `rx bad`, `rx rej`) is nonzero; loss or reordering on loopback (`drop`, `gap`, `late`, `restart`); `rx ok` exceeded the peer's `tx ok` + 1; or the send rate is off by more than one per report.

Two-board mode: `idf.py menuconfig` → *COMM counter stream* → untick loopback and set the peer address; set Wi-Fi under *Example Connection Configuration*. Build one image per board, each with the other's address.

### Wait-primitive comparison (TC-MEM-04)

```sh
idf.py -DCOMM_WAIT_SO_RCVTIMEO=1 fullclean build flash monitor
```

Only `wait_then_attempt()` in `comm.c` differs between the two builds.

## Host tests

```sh
cd host_test
make            # pure-function tests (ASan/UBSan), then comm.c over Linux loopback:
                # 2 wait variants x 3 tick rates (1000, 300, 100 Hz)
```

The Linux run is a stand-in for the loopback target checks, not a replacement: lwIP, VFS `select()` and the FreeRTOS tick behave differently.
