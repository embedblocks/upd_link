#include <stdio.h>
#include <sys/random.h>
#include "esp_err.h"
#include "esp_random.h"
uint32_t esp_random(void) { uint32_t v = 0; if (getrandom(&v, sizeof v, 0) != sizeof v) v = 0; return v; }
const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK"; case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM"; case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE"; case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT"; default: return "UNKNOWN ERROR";
    }
}
