from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('IntegerKernel.c')


class CompiledKernelTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, f'{arguments}\n{result.stderr}')
        return result

    def test_compiled_x64_elf_matches_same_c_source_on_native_arm64(self):
        with tempfile.TemporaryDirectory(prefix="integer kernel ' $(literal) ") as directory:
            directory = Path(directory)
            native_main = directory / 'Main.c'
            native_main.write_text('#include <stdio.h>\nunsigned long long GuestMain(void);\nint main(void) { printf("%llx\\n", GuestMain()); return 0; }\n')
            native = directory / 'native'
            self.command([COMPILER, '-arch', 'arm64', '-O2', '-fno-inline', SOURCE, native_main, '-o', native])
            expected = int(self.command([native]).stdout.strip(), 16)
            for optimization in ('-O0', '-O2', '-O3'):
                with self.subTest(optimization=optimization):
                    obj = directory / 'guest.o'
                    elf = directory / 'guest.elf'
                    self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization,
                                  '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                                  '-fno-inline', '-c', SOURCE, '-o', obj])
                    self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '--entry=GuestMain', '-o', elf, obj])
                    original = elf.read_bytes()
                    result = self.command([RUNNER, elf])
                    match = re.search(r'guest returned 0x([0-9a-f]+)', result.stdout)
                    self.assertIsNotNone(match, result.stdout)
                    self.assertEqual(int(match.group(1), 16), expected, optimization)
                    self.assertEqual(elf.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
