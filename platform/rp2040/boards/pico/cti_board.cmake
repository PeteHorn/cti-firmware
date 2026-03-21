function (cti_configure_board CTI_BOARD_DIR TARGET)
    target_sources(${TARGET} PUBLIC
        ${CTI_BOARD_DIR}/pi_pico_status_led.cpp
    )
endfunction()
