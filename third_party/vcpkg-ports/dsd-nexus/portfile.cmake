# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wichers/dsd-nexus
    REF 716c39e3c61837c1b5546721ea458521e5269b7b
    SHA512 995136faef385e204d6578f4dcc3f654881c169f6f848081331820e40d1809f3cbc4f3ea10ab33b940120fb3e625c24521468cdc1bb42c5acaf92c3c9870d21b
    HEAD_REF main
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
