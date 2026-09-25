add_library(ndp_common STATIC
  src/ndp_sha256.c src/ndp_frame.c src/ndp_tlv.c src/ndp_path.c src/ndp_policy.c src/ndp_agent.c
  src/ndp_names.c src/ndp_agent_fs.c src/ndp_agent_write.c src/ndp_auth.c src/ndp_agent_auth.c)
target_include_directories(ndp_common PUBLIC include)
target_include_directories(ndp_common PRIVATE src)

add_executable(test_ndp tests/test_ndp.c)
target_link_libraries(test_ndp PRIVATE ndp_common)
target_include_directories(test_ndp PRIVATE tests)
add_test(NAME test_ndp COMMAND test_ndp)
