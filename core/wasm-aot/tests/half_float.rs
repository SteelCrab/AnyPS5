mod common;

use anyps5_wasm_aot::translate_raw;
use common::host::{Machine, NidTable};
use common::*;
use iced_x86::code_asm::*;

const HALVES: u64 = DATA_BASE;
const ROUND_TRIP: u64 = REGION + 0x6_0000;
const SINGLES: u64 = REGION + 0x9_0000;

fn single_of_half(half: u16) -> u32 {
    let sign = u32::from(half & 0x8000) << 16;
    let exponent = (half >> 10) & 0x1f;
    let mantissa = u32::from(half & 0x3ff);
    if exponent == 0x1f {
        return sign | 0x7f80_0000 | (mantissa << 13) | if mantissa != 0 { 0x40_0000 } else { 0 };
    }
    let value = if exponent == 0 { mantissa as f32 * 2f32.powi(-24) } else { (1.0 + mantissa as f32 / 1024.0) * 2f32.powi(i32::from(exponent) - 15) };
    sign | value.to_bits()
}

fn set_singles(state: &mut State, register: usize, lanes: &[u32]) {
    for (lane, bits) in lanes.iter().enumerate() {
        state.ymm[register][lane * 4..lane * 4 + 4].copy_from_slice(&bits.to_le_bytes());
    }
}

fn halves(state: &State, register: usize, count: usize) -> Vec<u16> {
    (0..count).map(|lane| u16::from_le_bytes(state.ymm[register][lane * 2..lane * 2 + 2].try_into().unwrap())).collect()
}

fn narrow(singles: &[u32], build: impl FnOnce(&mut CodeAssembler) -> Result<(), iced_x86::IcedError>) -> State {
    let mut state = State::default();
    set_singles(&mut state, 1, singles);
    run_wasm(
        &assemble(|a| {
            build(a)?;
            a.ret()
        }),
        &state,
    )
}

#[test]
fn every_half_widens_exactly_and_narrows_back() {
    let code = assemble(|a| {
        let mut top = a.create_label();
        a.mov(rsi, HALVES)?;
        a.mov(rdi, SINGLES)?;
        a.mov(rdx, ROUND_TRIP)?;
        a.mov(ecx, 16384u32)?;
        a.set_label(&mut top)?;
        a.vcvtph2ps(xmm0, qword_ptr(rsi))?;
        a.vmovups(xmmword_ptr(rdi), xmm0)?;
        a.vcvtps2ph(qword_ptr(rdx), xmm0, 0)?;
        a.add(rsi, 8)?;
        a.add(rdi, 16)?;
        a.add(rdx, 8)?;
        a.dec(ecx)?;
        a.jnz(top)?;
        a.ret()
    });
    let mut machine = Machine::new(&translate_raw(&code, CODE_BASE, &snippet_options()).unwrap().wasm, NidTable::default());
    let all: Vec<u8> = (0..=u16::MAX).flat_map(u16::to_le_bytes).collect();
    machine.write(HALVES, &all);
    let state = State::default();
    let mut gpr = state.gpr;
    gpr[RSP] -= 8;
    machine.write(gpr[RSP], &SENTINEL.to_le_bytes());
    machine.set_state(&gpr, state.rflags, &state.ymm);
    machine.run(CODE_BASE, SENTINEL).unwrap();
    let singles = machine.read(SINGLES, 0x4_0000);
    let round_trip = machine.read(ROUND_TRIP, 0x2_0000);
    for half in 0..=u16::MAX {
        let index = usize::from(half);
        assert_eq!(u32::from_le_bytes(singles[index * 4..index * 4 + 4].try_into().unwrap()), single_of_half(half), "half {half:#06x} widened");
        let nan = half & 0x7c00 == 0x7c00 && half & 0x3ff != 0;
        assert_eq!(u16::from_le_bytes(round_trip[index * 2..index * 2 + 2].try_into().unwrap()), if nan { half | 0x200 } else { half }, "half {half:#06x} narrowed back");
    }
}

#[test]
fn wide_forms_convert_eight_lanes() {
    let mut state = State::default();
    for (lane, half) in [0x3c00u16, 0xc000, 0x7bff, 0x0001, 0x8000, 0x7c00, 0xfc00, 0x3555].iter().enumerate() {
        state.ymm[1][lane * 2..lane * 2 + 2].copy_from_slice(&half.to_le_bytes());
    }
    state.ymm[2] = [0x55; 32];
    let code = assemble(|a| {
        a.vcvtph2ps(ymm0, xmm1)?;
        a.vcvtps2ph(xmm2, ymm0, 0)?;
        a.ret()
    });
    let result = run_wasm(&code, &state);
    let singles: Vec<u32> = (0..8).map(|lane| u32::from_le_bytes(result.ymm[0][lane * 4..lane * 4 + 4].try_into().unwrap())).collect();
    assert_eq!(singles, [0x3f80_0000, 0xc000_0000, 0x477f_e000, 0x3380_0000, 0x8000_0000, 0x7f80_0000, 0xff80_0000, single_of_half(0x3555)]);
    assert_eq!(result.ymm[2][..16], state.ymm[1][..16]);
    assert_eq!(result.ymm[2][16..], [0; 16], "the VEX encoding clears the upper half");
}

#[test]
fn nearest_rounding_ties_to_even_and_overflows_to_infinity() {
    let result = narrow(&[0x3f80_1000, 0x3f80_3000, 0x477f_f000, 0x477f_efff], |a| a.vcvtps2ph(xmm0, xmm1, 0));
    assert_eq!(halves(&result, 0, 4), [0x3c00, 0x3c02, 0x7c00, 0x7bff]);
    let result = narrow(&[0x3300_0000, 0x33c0_0000, 0x0000_0001, 0x7fa0_0000], |a| a.vcvtps2ph(xmm0, xmm1, 0));
    assert_eq!(halves(&result, 0, 4), [0x0000, 0x0002, 0x0000, 0x7f00], "2^-25 ties to zero, 1.5 * 2^-24 to two, signaling NaNs are quieted");
}

#[test]
fn directed_rounding_follows_the_immediate() {
    let inputs = [0x3f80_1000, 0xbf80_1000, 0x4974_2400, 0xc974_2400];
    let expected = [
        (0, [0x3c00, 0xbc00, 0x7c00, 0xfc00]),
        (1, [0x3c00, 0xbc01, 0x7bff, 0xfc00]),
        (2, [0x3c01, 0xbc00, 0x7c00, 0xfbff]),
        (3, [0x3c00, 0xbc00, 0x7bff, 0xfbff]),
    ];
    for (mode, halves_expected) in expected {
        let result = narrow(&inputs, |a| a.vcvtps2ph(xmm0, xmm1, mode));
        assert_eq!(halves(&result, 0, 4), halves_expected, "rounding mode {mode}");
    }
}

#[test]
fn mxcsr_daz_applies_to_every_rounding_mode() {
    let mut state = State::default();
    set_singles(&mut state, 1, &[0x3f80_1000, 0xbf80_1000, 0x0000_0001, 0x8000_0001]);
    state.data[..4].copy_from_slice(&0x1fc0u32.to_le_bytes());
    let code = assemble(|a| {
        a.vcvtps2ph(xmm3, xmm1, 2)?;
        a.mov(rax, DATA_BASE)?;
        a.ldmxcsr(dword_ptr(rax))?;
        a.vcvtps2ph(xmm2, xmm1, 2)?;
        a.vcvtps2ph(xmm4, xmm1, 4)?;
        a.ret()
    });
    let result = run_wasm(&code, &state);
    assert_eq!(halves(&result, 3, 4), [0x3c01, 0xbc00, 0x0001, 0x8000], "without DAZ a denormal single rounds up to the smallest half");
    assert_eq!(halves(&result, 2, 4), [0x3c01, 0xbc00, 0x0000, 0x8000], "MXCSR.DAZ zeroes denormal singles");
    assert_eq!(halves(&result, 4, 4), [0x3c00, 0xbc00, 0x0000, 0x8000], "imm8 bit 2 takes the MXCSR rounding mode");
}
