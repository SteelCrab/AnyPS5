from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('AudioKernel.c')


class AudioKernelTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_compiled_x64_pcm_through_core_audio(self):
        with tempfile.TemporaryDirectory(prefix='Core Audio guest ') as directory:
            directory = Path(directory)
            obj = directory / 'guest.o'
            elf = directory / 'guest.elf'
            for optimization in ('-O0', '-O2', '-O3'):
                with self.subTest(optimization=optimization):
                    self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization,
                                  '-ffreestanding', '-fno-stack-protector', '-fPIC', '-c', SOURCE, '-o', obj])
                    self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '-shared',
                                  '--entry=GuestMain', '-o', elf, obj])
                    original = elf.read_bytes()
                    result = self.command([RUNNER, elf])
                    self.assertIn('guest returned 0x2a', result.stdout)
                    self.assertEqual(elf.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
