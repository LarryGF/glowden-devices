//! Builds the PE loader and the PipeWire host (C) and links libpipewire.
fn main() {
    let pw = std::process::Command::new("pkg-config")
        .args(["--cflags", "--libs", "libpipewire-0.3"])
        .output()
        .expect("pkg-config is needed to find libpipewire-0.3");
    let flags = String::from_utf8(pw.stdout).unwrap();
    let mut build = cc::Build::new();
    build
        .files(["loader/pe.c", "loader/winapi.c", "src/host.c"])
        .include("loader")
        .flag("-Wno-unused-parameter")
        .flag("-Wno-sign-compare")
        // Distro build flags may add GCC LTO, whose objects Rust's linker can't read.
        .flag("-fno-lto");
    for f in flags.split_whitespace() {
        if let Some(dir) = f.strip_prefix("-I") {
            build.include(dir);
        } else if let Some(lib) = f.strip_prefix("-l") {
            println!("cargo:rustc-link-lib={lib}");
        } else if f.starts_with("-D") {
            let (k, v) = f[2..]
                .split_once('=')
                .map_or((&f[2..], None), |(k, v)| (k, Some(v)));
            build.define(k, v);
        }
    }
    build.compile("bluevoice");
    println!("cargo:rustc-link-lib=m");
    for f in [
        "loader/pe.c",
        "loader/winapi.c",
        "loader/pe.h",
        "src/host.c",
    ] {
        println!("cargo:rerun-if-changed={f}");
    }
}
