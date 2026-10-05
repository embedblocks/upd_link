idf_component_register(
    SRCS     "counter_stream.c"
    REQUIRES comm esp_netif esp_event esp_wifi nvs_flash protocol_examples_common
)
