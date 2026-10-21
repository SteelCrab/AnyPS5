pub mod abi;
mod codegen;
pub mod discover;
pub mod elf;
pub mod flags;
pub mod image;
pub mod lsda;
pub mod meta;
pub mod program;
pub mod report;
pub mod runtime_patch;

use std::fmt;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error(String);

impl Error {
    pub fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Debug)]
pub struct TranslateOptions {
    pub shared_memory: bool,
    pub allow_unsupported: bool,
    pub flags_at_boundaries: bool,
    pub signal_polls: bool,
}

impl Default for TranslateOptions {
    fn default() -> Self {
        Self { shared_memory: true, allow_unsupported: false, flags_at_boundaries: false, signal_polls: false }
    }
}

pub struct Output {
    pub wasm: Vec<u8>,
    pub report: report::Report,
}

pub type Resolver<'a> = dyn FnMut(&str) -> Result<Option<(String, Vec<u8>)>> + 'a;

pub fn load_images(name: &str, bytes: &[u8], resolve: &mut Resolver<'_>) -> Result<Vec<image::Image>> {
    load_program_images(name, bytes, &[], resolve)
}

pub fn load_program_images(name: &str, bytes: &[u8], modules: &[String], resolve: &mut Resolver<'_>) -> Result<Vec<image::Image>> {
    let mut images = vec![elf::load(name, bytes)?];
    let mut folders = vec![String::new()];
    let mut loaded: Vec<String> = Vec::new();
    let mut modules = modules.iter();
    let mut next = 0;
    loop {
        while next < images.len() {
            let current = next;
            next += 1;
            let mut needed = std::mem::take(&mut images[current].needed);
            for dependency in &mut needed {
                let Some(path) = dependency.strip_prefix("$ORIGIN/").map(|path| origin_path(&folders[current], path)) else {
                    continue;
                };
                *dependency = format!("$ORIGIN/{path}");
                if loaded.contains(dependency) {
                    continue;
                }
                loaded.push(dependency.clone());
                if let Some((module_name, module_bytes)) = resolve(dependency)? {
                    images.push(elf::load(&module_name, &module_bytes)?);
                    folders.push(path.rsplit_once('/').map_or(String::new(), |(parent, _)| parent.to_string()));
                }
            }
            images[current].needed = needed;
        }
        let Some(module) = modules.next() else {
            return Ok(images);
        };
        let path = origin_path("", module);
        let dependency = format!("$ORIGIN/{path}");
        if loaded.contains(&dependency) {
            continue;
        }
        loaded.push(dependency.clone());
        if let Some((module_name, module_bytes)) = resolve(&dependency)? {
            images.push(elf::load(&module_name, &module_bytes)?);
            folders.push(path.rsplit_once('/').map_or(String::new(), |(parent, _)| parent.to_string()));
        }
    }
}

fn origin_path(folder: &str, path: &str) -> String {
    let mut parts: Vec<&str> = folder.split('/').filter(|part| !part.is_empty()).collect();
    for part in path.split('/') {
        match part {
            "" | "." => {}
            ".." if parts.last().is_some_and(|last| *last != "..") => {
                parts.pop();
            }
            _ => parts.push(part),
        }
    }
    parts.join("/")
}

pub fn translate_raw(code: &[u8], vaddr: u64, options: &TranslateOptions) -> Result<Output> {
    translate(program::Program::bind(vec![image::Image::raw(code, vaddr)])?, options)
}

const RAISE_EXCEPTION_NID: &str = "il03nluKfMk";

pub fn translate(program: program::Program, options: &TranslateOptions) -> Result<Output> {
    let options = &TranslateOptions { signal_polls: options.signal_polls || program.imports.iter().any(|nid| nid == RAISE_EXCEPTION_NID), ..*options };
    let discovery = discover::discover(&program)?;
    let mut liveness = Vec::with_capacity(discovery.functions.len());
    for function in &discovery.functions {
        let (successors, exits) = flags::edges(function);
        let analyzed = flags::analyze(function, &successors, &exits, options.flags_at_boundaries);
        liveness.push(analyzed.map_err(|error| Error::new(format!("{}: {error}", program.images[function.image as usize].name)))?);
    }
    let (wasm, unsupported) = codegen::emit_module(&program, &discovery, &liveness, options)?;
    validate(&wasm)?;
    let report = report::Report::new(&program, &discovery, unsupported, wasm.len());
    Ok(Output { wasm, report })
}

fn validate(wasm: &[u8]) -> Result<()> {
    let features = wasmparser::WasmFeatures::default()
        | wasmparser::WasmFeatures::MEMORY64
        | wasmparser::WasmFeatures::THREADS
        | wasmparser::WasmFeatures::TAIL_CALL
        | wasmparser::WasmFeatures::SIMD
        | wasmparser::WasmFeatures::RELAXED_SIMD;
    wasmparser::Validator::new_with_features(features).validate_all(wasm).map(|_| ()).map_err(|error| Error::new(format!("generated wasm is invalid: {error}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn origin_is_the_folder_of_the_needing_image() {
        assert_eq!(origin_path("", "app0/sce_module/libc.prx.guest.prx"), "app0/sce_module/libc.prx.guest.prx");
        assert_eq!(origin_path("app0/Media/Modules", "../../sce_module/libc.prx.guest.prx"), "app0/sce_module/libc.prx.guest.prx");
        assert_eq!(origin_path("app0/Media/Modules", "./PS5Util.prx.guest.prx"), "app0/Media/Modules/PS5Util.prx.guest.prx");
        assert_eq!(origin_path("app0", "../../outside.prx"), "../outside.prx");
    }
}
