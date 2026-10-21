use anyps5_wasm_aot::{TranslateOptions, translate_raw};

fn translate(code: &[u8]) -> Result<(), String> {
    translate_raw(code, 0x1000, &TranslateOptions::default()).map(|_| ()).map_err(|error| error.to_string())
}

#[test]
fn register_count_shift_needs_no_flags_that_are_overwritten_after_it() {
    translate(&[0x89, 0xf1, 0xb8, 0xfe, 0xff, 0xff, 0xff, 0xd3, 0xc0, 0xc1, 0xe9, 0x05, 0x21, 0x44, 0x8f, 0x40, 0xc3]).unwrap();
}

#[test]
fn register_count_shift_keeps_a_flag_read_after_it_live() {
    let error = translate(&[0xd3, 0xc0, 0x0f, 0x92, 0xc0, 0xc3]).unwrap_err();
    assert!(error.contains("flags 0x10 are read at 0x1000"), "{error}");
}

#[test]
fn repeated_compare_needs_no_flags_that_are_overwritten_after_it() {
    translate(&[0xf3, 0xa6, 0x31, 0xc0, 0xc3]).unwrap();
}
