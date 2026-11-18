# LV2 wrapper for the Shortcircuit XT CLAP (maarthome/shortcircuit-xt, my_own branch).
#
# Built as its own small project so the upstream CMake files stay untouched
# (easy syncing with surge-synthesizer/shortcircuit-xt). build_scxt.sh builds
# Shortcircuit XT first, then this project, then assembles the .lv2 bundle.
#
#   cmake -S src/clients/lv2 -B ignore/build-lv2 -DCMAKE_BUILD_TYPE=Release
#   cmake --build ignore/build-lv2

cmake_minimum_required(VERSION 3.16)
project(scxt_lv2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CLAP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/clap/clap/include"
    CACHE PATH "CLAP SDK include directory")
set(LV2_INCLUDE_DIR "" CACHE PATH "LV2 include directory (empty: use pkg-config lv2)")
set(CLAP2LV2_URI "https://github.com/maarthome/shortcircuit-xt#lv2" CACHE STRING "LV2 plugin URI")
set(CLAP2LV2_CLAP_FILE "scxt.clap" CACHE STRING "Name of the .clap file inside the bundle")

find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

if (LV2_INCLUDE_DIR STREQUAL "")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LV2 REQUIRED lv2)
    set(LV2_INCLUDE_DIRS_FOUND ${LV2_INCLUDE_DIRS})
else()
    set(LV2_INCLUDE_DIRS_FOUND ${LV2_INCLUDE_DIR})
endif()

set(CLAP2LV2_DEFS
    CLAP2LV2_URI="${CLAP2LV2_URI}"
    CLAP2LV2_CLAP_FILE="${CLAP2LV2_CLAP_FILE}")

add_library(scxt_lv2 MODULE clap2lv2.cpp)
target_include_directories(scxt_lv2 PRIVATE ${CLAP_INCLUDE_DIR} ${LV2_INCLUDE_DIRS_FOUND}
                                            ${X11_INCLUDE_DIR})
target_compile_definitions(scxt_lv2 PRIVATE ${CLAP2LV2_DEFS})
target_link_libraries(scxt_lv2 PRIVATE ${X11_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(scxt_lv2 PROPERTIES PREFIX "" OUTPUT_NAME "scxt_lv2"
                                          CXX_VISIBILITY_PRESET hidden)

add_executable(clap2lv2-ttl clap2lv2_ttl.cpp)
target_include_directories(clap2lv2-ttl PRIVATE ${CLAP_INCLUDE_DIR})
target_compile_definitions(clap2lv2-ttl PRIVATE ${CLAP2LV2_DEFS})
target_link_libraries(clap2lv2-ttl PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
