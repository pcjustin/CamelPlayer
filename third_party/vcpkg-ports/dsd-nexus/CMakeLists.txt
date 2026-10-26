# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.15)
project(dsd-nexus C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

add_subdirectory(libs/libsautil)
add_subdirectory(libs/libdsf)
add_subdirectory(libs/libdsdiff)
add_subdirectory(libs/libdst)

# Upstream quotes its forced time.h include, which Ninja passes through literally.
get_target_property(sautil_options sautil COMPILE_OPTIONS)
list(TRANSFORM sautil_options REPLACE "^/FI\"(.*)\"$" "/FI\\1")
set_target_properties(sautil PROPERTIES COMPILE_OPTIONS "${sautil_options}")

foreach(obj sautil libdsf libdsdiff libdst)
    target_compile_definitions(${obj} PRIVATE SACD_API_EXPORTS DSF_BUILDING_DLL DSDIFF_BUILDING_DLL DST_BUILDING_DLL)
endforeach()

add_library(dsdnexus SHARED)
target_link_libraries(dsdnexus PRIVATE sautil libdsf libdsdiff libdst)

install(TARGETS dsdnexus RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
install(DIRECTORY libs/libdsf/include/ libs/libdsdiff/include/ libs/libdst/include/ DESTINATION include)
