// Builds the C++ engine (and Lua) with CMake and links it: the server runs the
// same deterministic simulation as the clients to check their checksums.
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("engine");
    let src = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../engine");
    let run = |args: &[&str]| {
        let st = Command::new("cmake").args(args).status().expect("cmake is needed to build the engine");
        assert!(st.success(), "cmake {:?} failed", args);
    };
    run(&["-S", src.to_str().unwrap(), "-B", out.to_str().unwrap(), "-DCMAKE_BUILD_TYPE=Release"]);
    run(&["--build", out.to_str().unwrap(), "--target", "arena", "--config", "Release"]);
    for dir in [out.clone(), out.join("Release")] {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    println!("cargo:rustc-link-lib=static=arena");
    println!("cargo:rustc-link-lib=static=lua");
    if std::env::var("CARGO_CFG_TARGET_ENV").unwrap() != "msvc" {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
    for f in ["src", "include", "CMakeLists.txt"] {
        println!("cargo:rerun-if-changed={}", src.join(f).display());
    }
}
