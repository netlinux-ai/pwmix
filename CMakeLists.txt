cmake_minimum_required(VERSION 3.16)
project(pwconsole CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  libpipewire-0.3 cairo cairo-xlib pangocairo x11 libsystemd)

add_executable(pwconsole
  src/main.cpp src/pw.cpp src/desk.cpp src/ui.cpp)
target_link_libraries(pwconsole PRIVATE PkgConfig::DEPS pthread)
target_compile_options(pwconsole PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
