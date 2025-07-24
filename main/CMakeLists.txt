set(srcs "main.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "."
                       REQUIRES         
                        driver
                        esp_common
                        log
                        freertos
                        nvs_flash
                        bt
                       )
