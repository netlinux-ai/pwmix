cmake_minimum_required(VERSION 3.16)
project(pwmix CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  libpipewire-0.3 cairo cairo-xlib pangocairo x11 libsystemd)

add_executable(pwmix
  src/main.cpp src/pw.cpp src/desk.cpp src/ui.cpp)
target_link_libraries(pwmix PRIVATE PkgConfig::DEPS pthread)
target_compile_options(pwmix PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

include(GNUInstallDirs)
install(TARGETS pwmix RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/pwmix.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/pwmix.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
