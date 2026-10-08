from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('ThreadKernel.c')


class ThreadKernelTests(unittest.TestCase):
    def command(self, arguments, expected=0):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, expected, f'{arguments}\n{result.stderr}')
        return result

    def test_compiled_x64_threads_on_native_arm64(self):
        with tempfile.TemporaryDirectory(prefix='native guest threads ') as directory:
            directory = Path(directory)
            obj, elf = directory / 'guest.o', directory / 'guest.elf'
            for optimization in ('-O0', '-O2', '-O3'):
                for mode in ('THREAD_NORMAL', 'THREAD_FAILURE', 'THREAD_STOP', 'THREAD_DETACHED_FAILURE'):
                    with self.subTest(optimization=optimization, mode=mode):
                        self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization, '-ffreestanding',
                                      '-fno-stack-protector', '-fPIC', '-D' + mode, '-c', SOURCE, '-o', obj])
                        self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '-shared',
                                      '--entry=GuestMain', '-o', elf, obj])
                        original = elf.read_bytes()
                        result = self.command([RUNNER, elf], 2 if mode in ('THREAD_FAILURE', 'THREAD_DETACHED_FAILURE') else 0)
                        self.assertEqual(elf.read_bytes(), original)
                        if mode in ('THREAD_FAILURE', 'THREAD_DETACHED_FAILURE'):
                            self.assertIn('Guest divide by zero', result.stderr)
                        else:
                            self.assertIn('guest returned 0x2a', result.stdout)


if __name__ == '__main__':
    unittest.main()
