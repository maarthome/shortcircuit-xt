# Shortcircuit XT as LV2 (maarthome/shortcircuit-xt, my_own branch).
#
# Part of the main Shortcircuit XT build, switched on with -DSCXT_BUILD_LV2=ON
# (see src/clients/CMakeLists.txt). Shortcircuit XT's plugin code, the
# scxt-plugin library, is linked into the LV2 binary, the same way
# clap-wrapper builds the upstream VST3; no .clap file is used.
#
#   targets:  scxt_lv2       the LV2 plugin + X11 UI  (scxt_lv2.so)
#             scxt-lv2-ttl   writes the bundle's .ttl files at build time
#
# build_scxt.sh builds both and assembles ShortcircuitXT.lv2.

project(scxt-lv2 LANGUAGES CXX)

set(LV2_INCLUDE_DIR "" CACHE PATH "LV2 include directory (empty: use pkg-config lv2)")

find_package(X11 REQUIRED)
find_package(Threads REQUIRED)
if (LV2_INCLUDE_DIR STREQUAL "")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LV2 REQUIRED lv2)
    set(SCXT_LV2_INCLUDES ${LV2_INCLUDE_DIRS})
else()
    set(SCXT_LV2_INCLUDES ${LV2_INCLUDE_DIR})
endif()

# The LV2 plugin, with Shortcircuit XT linked in
add_library(scxt_lv2 MODULE clap2lv2.cpp)
target_include_directories(scxt_lv2 PRIVATE ${SCXT_LV2_INCLUDES} ${X11_INCLUDE_DIR})
target_link_libraries(scxt_lv2 PRIVATE scxt-plugin ${X11_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
# export only the LV2 entry points (and the build-time plugin entry), keep the
# statically linked Shortcircuit/JUCE symbols private to this plugin
target_link_options(scxt_lv2 PRIVATE -Wl,--exclude-libs,ALL)
set_target_properties(scxt_lv2 PROPERTIES PREFIX "" OUTPUT_NAME "scxt_lv2"
                                          CXX_VISIBILITY_PRESET hidden
                                          VISIBILITY_INLINES_HIDDEN ON)

# Build-time tool: reads the knob list from scxt_lv2.so and writes the .ttl files
add_executable(scxt-lv2-ttl clap2lv2_ttl.cpp)
target_link_libraries(scxt-lv2-ttl PRIVATE clap Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(scxt-lv2-ttl scxt_lv2)
