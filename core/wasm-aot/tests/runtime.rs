mod common;

use std::path::PathBuf;
use std::process::Command;

use anyps5_wasm_aot::{TranslateOptions, load_images, program::Program, translate};
use common::guest;

fn runtime() -> Option<PathBuf> {
    std::env::var_os("ANYPS5_WASM_RUNTIME_TEST").map(PathBuf::from)
}

fn program_wasm() -> PathBuf {
    translated_program("program", &[("program.cpp", &[]), ("tls_initial_exec.cpp", &["-ftls-model=initial-exec"]), ("jump.S", &[])])
}

fn translated_program(name: &str, sources: &[(&str, &[&str])]) -> PathBuf {
    let bytes = guest::build(name, sources);
    let images = load_images(&format!("{name}.so"), &bytes, &mut |_| Ok(None)).unwrap();
    let output = translate(Program::bind(images).unwrap(), &TranslateOptions::default()).unwrap();
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("runtime-{name}-{}.wasm", std::process::id()));
    std::fs::write(&path, &output.wasm).unwrap();
    path
}

fn run(calls: &[&[&str]]) -> Vec<String> {
    let runtime = runtime().expect("ANYPS5_WASM_RUNTIME_TEST names the runtime test build");
    run_program(runtime, program_wasm(), calls)
}

fn run_program(runtime: PathBuf, wasm: PathBuf, calls: &[&[&str]]) -> Vec<String> {
    let mut command = Command::new("node");
    command.arg(&runtime).arg(&wasm);
    for call in calls {
        command.arg("--call").args(*call);
    }
    let output = command.output().expect("node runs");
    let _ = std::fs::remove_file(&wasm);
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    assert!(output.status.success(), "runtime failed: {}\n{}", String::from_utf8_lossy(&output.stderr), stdout);
    stdout.lines().map(str::to_string).collect()
}

fn classify(value: i64) -> i64 {
    match value {
        0 => 3,
        1 => 14,
        2 => 15,
        3 => 92,
        4 => 65,
        5 => 35,
        6 => 89,
        7 => 79,
        _ => -1,
    }
}

#[test]
#[ignore = "requires ANYPS5_WASM_RUNTIME_TEST"]
fn guest_programs_run_on_the_wasm_runtime() {
    let switch = (-1..9).fold(0i64, |sum, value| sum * 3 + classify(value));
    let lines = run(&[
        &["run_initialized"],
        &["run_switch"],
        &["run_imports", "4"],
        &["run_callback", "5"],
        &["run_callback", "4"],
        &["run_vtable"],
        &["run_tls"],
        &["run_tls"],
        &["run_nonlocal"],
        &["run_guest_longjmp"],
    ]);
    let expected = [
        "run_initialized = 1000".to_string(),
        format!("run_switch = {switch}"),
        "print 4".to_string(),
        "run_imports = 15".to_string(),
        "run_callback = 15".to_string(),
        "run_callback = 8".to_string(),
        "run_vtable = 31".to_string(),
        "run_tls = 609".to_string(),
        "run_tls = 711".to_string(),
        "run_nonlocal = 77".to_string(),
        "run_guest_longjmp = 332".to_string(),
    ];
    assert_eq!(lines, expected);
}

#[test]
#[ignore = "requires ANYPS5_WASM_RUNTIME_NODE"]
fn guest_programs_run_on_the_production_runtime() {
    let runtime = std::env::var_os("ANYPS5_WASM_RUNTIME_NODE").map(PathBuf::from).expect("ANYPS5_WASM_RUNTIME_NODE names the production runtime");
    let lines = run_program(
        runtime,
        translated_program("runtime_library", &[("runtime_library.cpp", &[])]),
        &[&["run_initialized"], &["run_imports", "4"], &["run_callback"], &["run_tls"], &["run_tls"], &["run_data"], &["run_print"]],
    );
    assert_eq!(
        lines,
        [
            "run_initialized = 1000",
            "run_imports = 11",
            "run_callback = -3621",
            "run_tls = 6",
            "run_tls = 7",
            "run_data = 1",
            "WASM_RUNTIME_SMOKE_PASS",
            "run_print = 0",
        ]
    );
}
