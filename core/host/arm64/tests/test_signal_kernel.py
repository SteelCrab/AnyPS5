from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('SignalKernel.c')


class SignalKernelTests(unittest.TestCase):
    def command(self, arguments, status=0):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')
        return result

    def test_compiled_guest_signal_delivery(self):
        with tempfile.TemporaryDirectory(prefix="native signals ' $(literal) ") as directory:
            obj, elf = Path(directory) / 'guest.o', Path(directory) / 'guest.elf'
            for optimization in ('-O0', '-O2', '-O3'):
                with self.subTest(optimization=optimization):
                    self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization, '-ffreestanding',
                                  '-fno-stack-protector', '-fPIC', '-c', SOURCE, '-o', obj])
                    self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '-shared', '--entry=GuestMain', '-o', elf, obj])
                    original = elf.read_bytes()
                    for mode, status in [('r', 0), ('k', 143), ('t', 143), ('a', 134), ('i', 134), ('e', 37)]:
                        with self.subTest(mode=mode):
                            result = self.command([RUNNER, elf, '1000000', '--', mode], status)
                            self.assertNotIn('pending-output', result.stdout)
                            if mode == 'r':
                                self.assertIn('guest returned 0x2a', result.stdout)
                            elif mode in ('k', 't'):
                                self.assertIn('terminated by signal 15', result.stdout)
                            else:
                                self.assertIn(f'status {status}', result.stdout)
                            self.assertEqual('caught-abort' in result.stdout, mode == 'a')
                            self.assertEqual(elf.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
