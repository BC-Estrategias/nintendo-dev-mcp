cmake_minimum_required(VERSION 3.16)
project(nintendo_dev_agent C)

# Host build (macOS/Linux): the platform-neutral core, its tests and a fake agent.
option(NDP_SANITIZE "Build with ASan + UBSan" ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)
if(NDP_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(common)
add_subdirectory(posix)
add_subdirectory(web)
add_subdirectory(host)

enable_testing()
