add_library(ndp_posix_server STATIC src/ndp_server.c src/ndp_posix_fs.c)
target_include_directories(ndp_posix_server PUBLIC include)
target_link_libraries(ndp_posix_server PUBLIC ndp_common)

add_executable(test_fs tests/test_fs.c)
target_link_libraries(test_fs PRIVATE ndp_posix_server)
add_test(NAME test_fs COMMAND test_fs)
