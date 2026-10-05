idf_component_register(
    SRCS              "src/comm.c" "src/comm_pure.c"
    INCLUDE_DIRS      "include"
    PRIV_INCLUDE_DIRS "private" "src"
    REQUIRES          esp_netif
    PRIV_REQUIRES     lwip esp_hw_support heap log
)

# Wait primitive (COMM_SPEC.md §9.4). Default: select().
# For the wait-primitive comparison (TC-MEM-04):  idf.py -DCOMM_WAIT_SO_RCVTIMEO=1 build
if(COMM_WAIT_SO_RCVTIMEO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMM_WAIT_SO_RCVTIMEO=1)
endif()
