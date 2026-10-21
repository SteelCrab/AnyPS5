mod common;

use anyps5_wasm_aot::load_program_images;
use common::guest;

fn library() -> Vec<u8> {
    guest::build("program", &[("program.cpp", &[]), ("tls_initial_exec.cpp", &["-ftls-model=initial-exec"]), ("jump.S", &[])])
}

#[test]
fn modules_loaded_at_run_time_join_the_program_once() {
    let bytes = library();
    let mut requested = Vec::new();
    let mut resolve = |path: &str| -> anyps5_wasm_aot::Result<Option<(String, Vec<u8>)>> {
        requested.push(path.to_string());
        Ok(Some((path.trim_start_matches("$ORIGIN/").to_string(), bytes.clone())))
    };
    let modules = ["app0/Media/Plugins/PSNCore.prx.guest.prx".to_string(), "app0/Media/./Plugins/PSNCore.prx.guest.prx".to_string()];
    let images = load_program_images("app.elf", &bytes, &modules, &mut resolve).unwrap();
    let names: Vec<&str> = images.iter().map(|image| image.name.as_str()).collect();
    assert_eq!(names, ["app.elf", "app0/Media/Plugins/PSNCore.prx.guest.prx"]);
    assert_eq!(requested, ["$ORIGIN/app0/Media/Plugins/PSNCore.prx.guest.prx"]);
}
