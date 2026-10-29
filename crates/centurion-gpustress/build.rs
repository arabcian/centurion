// Compiles shaders/*.wgsl to SPIR-V into OUT_DIR (embedded with include_bytes!), so building needs no
// external shader compiler and the WGSL source is what ships.
use std::{env, fs, path::Path};

fn main() {
    let out = env::var("OUT_DIR").unwrap();
    println!("cargo:rerun-if-changed=shaders");
    for e in fs::read_dir("shaders").unwrap().flatten() {
        let path = e.path();
        if path.extension().map_or(true, |x| x != "wgsl") { continue; }
        let src = fs::read_to_string(&path).unwrap();
        let module = naga::front::wgsl::parse_str(&src).unwrap_or_else(|err| panic!("{}: {}", path.display(), err.emit_to_string(&src)));
        let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::empty())
            .validate(&module).unwrap_or_else(|err| panic!("{}: {err:?}", path.display()));
        let words = naga::back::spv::write_vec(&module, &info, &naga::back::spv::Options::default(), None).unwrap();
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        fs::write(Path::new(&out).join(path.file_stem().unwrap()).with_extension("spv"), bytes).unwrap();
    }
}
