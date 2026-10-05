/* Host stub: esp_ip4_addr_t holds the address in network byte order, as in ESP-IDF. */
#pragma once
#include <stdint.h>
#include <arpa/inet.h>
typedef struct { uint32_t addr; } esp_ip4_addr_t;
#define ESP_IP4TOADDR(a, b, c, d) \
    htonl(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
