add_executable(ndp-host-agent main.c)
target_link_libraries(ndp-host-agent PRIVATE ndp_common)
