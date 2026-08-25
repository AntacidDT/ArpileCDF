cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Arpile 32CDF firmware: keep hardware drivers isolated under components/
set(EXTRA_COMPONENT_DIRS components)

project(arpile_32cdf)

# Suppress char-subscripts warning-as-error for components not updated for newlib
add_compile_options(-Wno-error=char-subscripts)
