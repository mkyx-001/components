idf_component_register(
    SRCS "eth_ch182d.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_netif esp_event
    PRIV_REQUIRES driver
    REQUIRED_IDF_TARGETS esp32p4
)
