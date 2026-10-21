mod common;

use common::*;
use iced_x86::code_asm::*;

fn set(state: &mut State, register: usize, lanes: &[f32]) {
    for (lane, value) in lanes.iter().enumerate() {
        state.ymm[register][lane * 4..lane * 4 + 4].copy_from_slice(&value.to_le_bytes());
    }
}

fn get(state: &State, register: usize, count: usize) -> Vec<f32> {
    (0..count).map(|lane| f32::from_le_bytes(state.ymm[register][lane * 4..lane * 4 + 4].try_into().unwrap())).collect()
}

fn run(inputs: &[(usize, &[f32])], build: impl FnOnce(&mut CodeAssembler) -> Result<(), iced_x86::IcedError>) -> State {
    let code = assemble(|a| {
        build(a)?;
        a.ret()
    });
    let mut state = State::default();
    for (register, lanes) in inputs {
        set(&mut state, *register, lanes);
    }
    run_wasm(&code, &state)
}

fn assert_approximates(actual: f32, exact: f64) {
    if exact.is_nan() {
        assert!(actual.is_nan(), "{actual} for NaN");
    } else if exact.is_infinite() {
        assert_eq!(actual as f64, exact);
    } else {
        assert!(((actual as f64 - exact) / exact).abs() <= 1.5 / 4096.0, "{actual} for {exact}");
    }
}

const INPUTS: [f32; 8] = [1.0, 4.0, 0.3, 12345.678, 1.0e-30, 7.0e30, 0.0, -0.0];

#[test]
fn reciprocals_of_packed_singles_are_within_the_documented_error() {
    let state = run(&[(1, &INPUTS)], |a| {
        a.rcpps(xmm0, xmm1)?;
        a.rsqrtps(xmm2, xmm1)?;
        a.vrcpps(ymm3, ymm1)?;
        a.vrsqrtps(ymm4, ymm1)
    });
    for (lane, &x) in INPUTS.iter().enumerate() {
        let x = x as f64;
        if lane < 4 {
            assert_approximates(get(&state, 0, 4)[lane], 1.0 / x);
            assert_approximates(get(&state, 2, 4)[lane], 1.0 / x.sqrt());
        }
        assert_approximates(get(&state, 3, 8)[lane], 1.0 / x);
        assert_approximates(get(&state, 4, 8)[lane], 1.0 / x.sqrt());
    }
    assert_eq!(get(&state, 4, 8)[7], f32::NEG_INFINITY, "rsqrt(-0) is -inf");
}

#[test]
fn reciprocal_square_root_of_a_negative_is_nan() {
    let state = run(&[(1, &[-1.0, -4.0, 2.0, 9.0])], |a| a.rsqrtps(xmm0, xmm1));
    let result = get(&state, 0, 4);
    assert!(result[0].is_nan() && result[1].is_nan());
    assert_approximates(result[3], 1.0 / 3.0);
}

#[test]
fn scalar_reciprocals_keep_the_upper_lanes() {
    let state = run(&[(0, &[9.0, 9.0, 9.0, 9.0]), (1, &[4.0, 5.0, 6.0, 7.0]), (2, &[16.0, 1.0, 1.0, 1.0])], |a| {
        a.movaps(xmm3, xmm0)?;
        a.rcpss(xmm0, xmm2)?;
        a.vrsqrtss(xmm4, xmm1, xmm2)?;
        a.rsqrtss(xmm3, xmm2)
    });
    assert_eq!(get(&state, 0, 4)[1..], [9.0, 9.0, 9.0]);
    assert_approximates(get(&state, 0, 4)[0], 1.0 / 16.0);
    assert_eq!(get(&state, 4, 4)[1..], [5.0, 6.0, 7.0]);
    assert_approximates(get(&state, 4, 4)[0], 0.25);
    assert_approximates(get(&state, 3, 4)[0], 0.25);
}

#[test]
fn dot_product_masks_products_and_destination_lanes() {
    let a: &[f32] = &[1.0, 2.0, 3.0, 4.0];
    let b: &[f32] = &[5.0, 6.0, 7.0, 8.0];
    let state = run(&[(0, a), (1, b), (2, a)], |asm| {
        asm.dpps(xmm0, xmm1, 0xf1)?;
        asm.dpps(xmm2, xmm1, 0xb6)
    });
    assert_eq!(get(&state, 0, 4), [70.0, 0.0, 0.0, 0.0]);
    assert_eq!(get(&state, 2, 4), [0.0, 49.0, 49.0, 0.0], "products of lanes 0, 1 and 3 into lanes 1 and 2");
}

#[test]
fn dot_product_adds_the_pairs_before_the_halves() {
    let state = run(&[(0, &[1.0e8, 1.0, -1.0e8, 1.0]), (1, &[1.0, 1.0, 1.0, 1.0])], |a| a.dpps(xmm0, xmm1, 0xff));
    assert_eq!(get(&state, 0, 4), [0.0, 0.0, 0.0, 0.0]);
}

#[test]
fn wide_dot_product_works_on_each_half() {
    let state = run(&[(1, &[1.0, 2.0, 3.0, 4.0, 1.0, 1.0, 1.0, 1.0]), (2, &[1.0, 1.0, 1.0, 1.0, 2.0, 3.0, 4.0, 5.0])], |a| {
        a.vdpps(ymm0, ymm1, ymm2, 0x71)?;
        a.vdpps(xmm3, xmm1, xmm2, 0xff)
    });
    assert_eq!(get(&state, 0, 8), [6.0, 0.0, 0.0, 0.0, 9.0, 0.0, 0.0, 0.0]);
    assert_eq!(get(&state, 3, 8), [10.0, 10.0, 10.0, 10.0, 0.0, 0.0, 0.0, 0.0], "VEX.128 zeroes the upper half");
}
