set(MANAGED_BUTTON_DIR "${PROJECT_DIR}/managed_components/espressif__button")

set(BUTTON_SRCS
    "${MANAGED_BUTTON_DIR}/button_gpio.c"
    "${MANAGED_BUTTON_DIR}/button_matrix.c"
    "${MANAGED_BUTTON_DIR}/iot_button.c")

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.0")
    list(APPEND BUTTON_REQUIRES esp_adc)
    if(CONFIG_SOC_ADC_SUPPORTED)
        list(APPEND BUTTON_SRCS "${MANAGED_BUTTON_DIR}/button_adc.c")
    endif()
endif()

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND BUTTON_REQUIRES esp_driver_gpio)
else()
    list(APPEND BUTTON_REQUIRES driver)
endif()

idf_component_register(
    SRCS ${BUTTON_SRCS}
    INCLUDE_DIRS
        "${MANAGED_BUTTON_DIR}/include"
        "${MANAGED_BUTTON_DIR}/interface"
    REQUIRES ${BUTTON_REQUIRES}
    PRIV_REQUIRES esp_timer
)

include("${PROJECT_DIR}/managed_components/espressif__cmake_utilities/package_manager.cmake")
cu_pkg_define_version(${MANAGED_BUTTON_DIR})
