idf_component_register(
    SRCS "cellular_modem.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_event lwip
    PRIV_REQUIRES
        esp_driver_gpio
        espressif__iot_usbh_cdc
        espressif__iot_usbh_rndis
        espressif__iot_eth
    REQUIRED_IDF_TARGETS esp32p4
)
