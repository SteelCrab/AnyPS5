mod common;

use common::*;
use iced_x86::code_asm::*;

#[test]
fn multiply_add_of_unsigned_and_signed_bytes_matches_the_oracle() {
    check(|a| a.pmaddubsw(xmm0, xmm1));
    check(|a| a.pmaddubsw(xmm2, xmm2));
}

#[test]
fn sums_of_absolute_byte_differences_match_the_oracle() {
    check(|a| a.psadbw(xmm0, xmm1));
    check(|a| a.psadbw(xmm3, xmm3));
}

#[test]
fn horizontal_adds_and_subtracts_match_the_oracle() {
    check(|a| a.phaddw(xmm0, xmm1));
    check(|a| a.phaddd(xmm0, xmm1));
    check(|a| a.phaddsw(xmm0, xmm1));
    check(|a| a.phsubw(xmm0, xmm1));
    check(|a| a.phsubd(xmm0, xmm1));
    check(|a| a.phsubsw(xmm0, xmm1));
}

#[test]
fn minimum_position_matches_the_oracle() {
    check(|a| a.phminposuw(xmm0, xmm1));
}

#[test]
fn saturating_horizontal_add_clamps() {
    let mut state = State::default();
    for (lane, word) in [0x7fffu16, 1, 0x8000, 0xffff, 5, 6, 0, 0].iter().enumerate() {
        state.ymm[0][lane * 2..lane * 2 + 2].copy_from_slice(&word.to_le_bytes());
    }
    let result = run_wasm(&assemble(|a| {
        a.phaddsw(xmm0, xmm1)?;
        a.ret()
    }), &state);
    let words: Vec<u16> = (0..4).map(|lane| u16::from_le_bytes(result.ymm[0][lane * 2..lane * 2 + 2].try_into().unwrap())).collect();
    assert_eq!(words, [0x7fff, 0x8000, 11, 0]);
}

#[test]
fn wide_vex_forms_work_per_lane() {
    let mut state = State::default();
    for lane in 0..8 {
        state.ymm[1][lane * 4..lane * 4 + 4].copy_from_slice(&(lane as u32 + 1).to_le_bytes());
        state.ymm[2][lane * 4..lane * 4 + 4].copy_from_slice(&((lane as u32 + 1) * 100).to_le_bytes());
    }
    let result = run_wasm(&assemble(|a| {
        a.vphaddd(ymm0, ymm1, ymm2)?;
        a.ret()
    }), &state);
    let dwords: Vec<u32> = (0..8).map(|lane| u32::from_le_bytes(result.ymm[0][lane * 4..lane * 4 + 4].try_into().unwrap())).collect();
    assert_eq!(dwords, [3, 7, 300, 700, 11, 15, 1100, 1500]);
}

#[test]
fn insertq_replaces_a_bit_field_of_the_low_quadword() {
    let mut state = State::default();
    state.ymm[0][..16].copy_from_slice(&0x1111_2222_3333_4444_5555_6666_7777_8888u128.to_le_bytes());
    state.ymm[1][..8].copy_from_slice(&0xffff_ffff_ffff_abcdu64.to_le_bytes());
    state.ymm[2][..16].copy_from_slice(&0x1111_2222_3333_4444_5555_6666_7777_8888u128.to_le_bytes());
    let result = run_wasm(&assemble(|a| {
        a.insertq_4(xmm0, xmm1, 16u32, 8u32)?;
        a.insertq_4(xmm2, xmm1, 0u32, 0u32)?;
        a.ret()
    }), &state);
    assert_eq!(u128::from_le_bytes(result.ymm[0][..16].try_into().unwrap()), 0x1111_2222_3333_4444_5555_6666_77ab_cd88);
    assert_eq!(u128::from_le_bytes(result.ymm[2][..16].try_into().unwrap()), 0x1111_2222_3333_4444_ffff_ffff_ffff_abcd, "a length of 0 inserts 64 bits");
}

fn run_words(words: [u16; 8], build: impl FnOnce(&mut CodeAssembler) -> Result<(), iced_x86::IcedError>) -> State {
    let code = assemble(|a| {
        build(a)?;
        a.ret()
    });
    let mut state = State::default();
    for (lane, word) in words.iter().enumerate() {
        state.ymm[1][lane * 2..lane * 2 + 2].copy_from_slice(&word.to_le_bytes());
    }
    state.ymm[0] = [0xaa; 32];
    run_wasm(&code, &state)
}

fn expect(state: &State, register: usize, value: u16, index: u16) {
    let mut expected = [0u8; 16];
    expected[0..2].copy_from_slice(&value.to_le_bytes());
    expected[2..4].copy_from_slice(&index.to_le_bytes());
    assert_eq!(state.ymm[register][..16], expected);
}

#[test]
fn the_smallest_word_and_its_index_are_returned() {
    let state = run_words([900, 70, 65535, 12, 400, 13, 12000, 300], |a| a.phminposuw(xmm0, xmm1));
    expect(&state, 0, 12, 3);
    assert_eq!(state.ymm[0][16..], [0xaa; 16], "the legacy encoding keeps the upper half");
}

#[test]
fn equal_minimums_report_the_lowest_index() {
    let state = run_words([5, 9, 2, 2, 8, 2, 40000, 3], |a| a.vphminposuw(xmm0, xmm1));
    expect(&state, 0, 2, 2);
    assert_eq!(state.ymm[0][16..], [0; 16], "the VEX encoding clears the upper half");
}

#[test]
fn unsigned_order_puts_high_words_last() {
    let state = run_words([0x8000, 0xffff, 0x7fff, 0x8001, 0xfffe, 0x9000, 0xa000, 0x7ffe], |a| a.phminposuw(xmm2, xmm1));
    expect(&state, 2, 0x7ffe, 7);
}
