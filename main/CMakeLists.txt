idf_component_register(
    SRCS            "app_main.cpp" "app_driver.cpp" "trotec_3550_ir.cpp"
    PRIV_INCLUDE_DIRS  "."
                       "${ESP_MATTER_PATH}/examples/common/utils"
    REQUIRES esp_matter
    PRIV_REQUIRES app_reset button chip ds18b20 esp_driver_gpio esp_driver_rmt esp_matter_console nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
