from pathlib import Path
import subprocess
import re
import sys
import tempfile
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('DynamicKernel.c')


class DynamicKernelTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, f'{arguments}\n{result.stderr}')
        return result

    def test_compiled_dynamic_elf_initialization_and_native_libc(self):
        with tempfile.TemporaryDirectory(prefix="dynamic ELF ' $(literal) ") as directory:
            directory = Path(directory)
            obj = directory / 'guest.o'
            elf = directory / 'guest.elf'
            for optimization in ('-O0', '-O2', '-O3'):
                with self.subTest(optimization=optimization):
                    self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization, '-ffreestanding',
                                  '-fno-stack-protector', '-fPIC', '-c', SOURCE, '-o', obj])
                    self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '-shared',
                                  '--entry=GuestMain', '--init=Initialize', '-o', elf, obj])
                    original = elf.read_bytes()
                    result = self.command([RUNNER, elf])
                    self.assertIn('guest returned 0x2a', result.stdout)
                    self.assertEqual(elf.read_bytes(), original)
                    count = re.search(r'(\d+) instructions', result.stdout)
                    self.assertIsNotNone(count, result.stdout)
                    exhausted = subprocess.run([str(RUNNER), str(elf), str(int(count.group(1)) - 1)],
                                               capture_output=True, text=True, timeout=30)
                    self.assertEqual(exhausted.returncode, 2)
                    self.assertIn('instruction budget exceeded', exhausted.stderr)


if __name__ == '__main__':
    unittest.main()
