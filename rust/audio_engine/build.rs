// libsoxr comes from the repository's vcpkg tree. It is not bundled into the static library: the C++ executables link
// soxr.lib themselves, and only Rust test binaries need it from here.
fn main() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../vcpkg_installed/x64-windows-static-md/lib");
    println!("cargo:rustc-link-search=native={}", root.display());
    println!("cargo:rustc-link-lib=static:-bundle=soxr");
    println!("cargo:rerun-if-changed=build.rs");
}
