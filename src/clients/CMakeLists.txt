project(scxt-clients LANGUAGES CXX)

add_subdirectory(console-ui)
add_subdirectory(clap-first)
add_subdirectory(cli-tools)
add_subdirectory(stress-tests)
add_subdirectory(perf-harness)
add_dependencies(cli-tools make-stress)

# My_Changes branch: Shortcircuit XT as LV2 (src/clients/lv2), off by default
option(SCXT_BUILD_LV2 "Build Shortcircuit XT as LV2 plugin" OFF)
if (SCXT_BUILD_LV2)
    add_subdirectory(lv2)
endif()
