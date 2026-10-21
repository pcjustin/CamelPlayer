// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// libsoxr comes from the repository's vcpkg tree. It is not bundled into the static library: the C++ executables link
// soxr.lib themselves, and only Rust test binaries need it from here.
fn main() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../vcpkg_installed/x64-windows-static-md/lib");
    println!("cargo:rustc-link-search=native={}", root.display());
    println!("cargo:rustc-link-lib=static:-bundle=soxr");
    println!("cargo:rerun-if-changed=build.rs");
}
