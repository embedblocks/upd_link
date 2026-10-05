#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMM_HEADER_SIZE          12
#define COMM_MAX_PAYLOAD_LIMIT    1460
#define COMM_WAIT_FOREVER         UINT32_MAX
#define COMM_MAX_TIMEOUT_MS       (24u * 60u * 60u * 1000u)

#define ESP_ERR_COMM_BASE           0x70000     /* not 0x7000 (ESP_ERR_HTTP_BASE) */
#define ESP_ERR_COMM_NO_ROUTE       (ESP_ERR_COMM_BASE + 1)
#define ESP_ERR_COMM_TRUNCATED      (ESP_ERR_COMM_BASE + 2)
#define ESP_ERR_COMM_MALFORMED      (ESP_ERR_COMM_BASE + 3)
#define ESP_ERR_COMM_VERSION        (ESP_ERR_COMM_BASE + 4)
#define ESP_ERR_COMM_STREAM         (ESP_ERR_COMM_BASE + 5)
#define ESP_ERR_COMM_SIZE_MISMATCH  (ESP_ERR_COMM_BASE + 6)

typedef struct comm_instance *comm_handle_t;

typedef struct {
    uint16_t       local_port;    /* UDP port this node sends from and receives on */
    esp_ip4_addr_t peer_ip;       /* peer's unicast IPv4 address (network order)   */
    uint16_t       peer_port;     /* the peer's local_port                         */
    uint8_t        stream_id;     /* must match the peer's configured value        */
    uint16_t       max_payload;   /* 1 .. COMM_MAX_PAYLOAD_LIMIT                   */
} comm_config_t;

typedef struct {
    size_t   payload_len;         /* bytes of payload                              */
    uint32_t seq;                 /* sender's sequence number                      */
    uint32_t session;             /* sender's session identifier                   */
} comm_rx_info_t;

esp_err_t comm_open (const comm_config_t *cfg, comm_handle_t *out);
esp_err_t comm_close(comm_handle_t h);

esp_err_t comm_send (comm_handle_t h, const void *payload, size_t len);
esp_err_t comm_recv (comm_handle_t h, void *buf, size_t cap,
                     uint32_t timeout_ms, comm_rx_info_t *info);

const char *comm_err_to_name(esp_err_t err);

static inline int32_t comm_seq_delta(uint32_t expected, uint32_t received)
{
    return (int32_t)(received - expected);
}

#ifdef __cplusplus
}
#endif
