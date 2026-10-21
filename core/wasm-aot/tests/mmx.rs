use anyps5_wasm_aot::{TranslateOptions, translate_raw};

const CODE: &[u8] = &[0x0f, 0x6f, 0x0e, 0x0f, 0xd5, 0x08, 0x0f, 0x7f, 0x0f, 0xc3];

#[test]
fn mmx_instructions_are_unsupported() {
    let error = translate_raw(CODE, 0x1000, &TranslateOptions::default()).err().expect("MMX is not translated").to_string();
    assert!(error.contains("unsupported instruction at 0x1000"), "{error}");
}

#[test]
fn mmx_instructions_become_traps_when_unsupported_instructions_are_allowed() {
    let options = TranslateOptions { allow_unsupported: true, ..TranslateOptions::default() };
    let output = translate_raw(CODE, 0x1000, &options).unwrap();
    let addresses: Vec<u64> = output.report.unsupported.iter().map(|unsupported| unsupported.address).collect();
    assert_eq!(addresses, [0x1000, 0x1003, 0x1006]);
}
