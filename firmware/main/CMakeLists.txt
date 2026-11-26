idf_component_register(SRCS "main.c" "ir_rmt.c" "jsonl.c" "store.c" "transport.c" "engine.c" "proto.c" "ble.c" "remote.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_rmt esp_driver_gpio nvs_flash bt esp_driver_usb_serial_jtag esp_timer)
