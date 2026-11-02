# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mackron/dr_libs
    REF 35b5c392cc15456736c440a68dbe073178bbffe0
    SHA512 410cb81ecf1a7d8d93ec272244bd00fa7c64d405f2d0d1c8756e522d6faab94b4aacbcb3448c40fc0a055e69f1675f4f314b2d2f0dda9d3c2d5a6437837891c6
    HEAD_REF master
)

file(INSTALL "${SOURCE_PATH}/dr_flac.h" "${SOURCE_PATH}/dr_mp3.h" "${SOURCE_PATH}/dr_wav.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
