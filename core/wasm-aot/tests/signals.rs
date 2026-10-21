mod common;

use anyps5_wasm_aot::{TranslateOptions, abi, translate_raw};
use common::host::{CTX_ADDR, Machine, NidTable};
use common::*;
use iced_x86::code_asm::*;

fn countdown() -> Vec<u8> {
    assemble(|a| {
        let mut top = a.create_label();
        a.mov(ecx, 5u32)?;
        a.set_label(&mut top)?;
        a.dec(ecx)?;
        a.jnz(top)?;
        a.ret()
    })
}

fn run(code: &[u8], signal_polls: bool, pending: bool) -> (wasmtime::Result<()>, Machine) {
    let options = TranslateOptions { shared_memory: false, flags_at_boundaries: true, signal_polls, ..TranslateOptions::default() };
    let output = translate_raw(code, CODE_BASE, &options).unwrap();
    let mut machine = Machine::new(&output.wasm, NidTable::default());
    let state = State::default();
    let mut gpr = state.gpr;
    gpr[RSP] -= 8;
    machine.write(gpr[RSP], &SENTINEL.to_le_bytes());
    machine.set_state(&gpr, state.rflags, &state.ymm);
    machine.write(CTX_ADDR + abi::cpu::PENDING_SIGNAL, &u32::from(pending).to_le_bytes());
    let result = machine.run(CODE_BASE, SENTINEL);
    (result, machine)
}

#[test]
fn loops_run_unchanged_when_no_signal_is_pending() {
    let (result, machine) = run(&countdown(), true, false);
    result.unwrap();
    assert_eq!(machine.state().0[1], 0, "rcx counted down to 0");
}

#[test]
fn a_pending_signal_is_delivered_at_the_loop_header() {
    let (result, _) = run(&countdown(), true, true);
    let error = format!("{:?}", result.expect_err("the loop polls the pending signal"));
    assert!(error.contains("one was polled at"), "{error}");
}

#[test]
fn loops_do_not_poll_without_signal_polls() {
    let (result, machine) = run(&countdown(), false, true);
    result.unwrap();
    assert_eq!(machine.state().0[1], 0);
}
