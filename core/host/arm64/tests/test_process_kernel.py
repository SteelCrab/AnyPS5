from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()
LINKER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).with_name('ProcessKernel.c')


class ProcessKernelTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_compiled_process_exit_and_host_queries(self):
        with tempfile.TemporaryDirectory(prefix="native process ' $(literal) ") as directory:
            directory = Path(directory)
            obj = directory / 'guest.o'
            elf = directory / 'guest.elf'
            for optimization in ('-O0', '-O2', '-O3'):
                with self.subTest(optimization=optimization):
                    self.command([COMPILER, '--target=x86_64-unknown-none-elf', optimization,
                                  '-ffreestanding', '-fno-stack-protector', '-fPIC', '-c', SOURCE, '-o', obj])
                    self.command([LINKER, '-flavor', 'gnu', '-m', 'elf_x86_64', '-shared', '--entry=GuestMain',
                                  '--init=Initialize', '--fini=Finalize', '-o', elf, obj])
                    original = elf.read_bytes()
                    for mode, status, output in [('r', 0, ' body C D B A F'), ('d', 0, ' B body C D A F'),
                                                 ('e', 3, ' body C D B A F'), ('q', 5, None), ('x', 7, None),
                                                 ('a', 134, None), ('w', 9, ' body C D B A F'), ('i', 11, None),
                                                 ('s', 9, ' body C D B A F')]:
                        with self.subTest(mode=mode):
                            result = subprocess.run([str(RUNNER), str(elf), '1000000', '--', mode],
                                                    capture_output=True, text=True, timeout=10)
                            self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                            self.assertEqual(result.stderr, '')
                            if output is None:
                                self.assertNotIn(' body', result.stdout)
                                self.assertNotIn(' F', result.stdout)
                            else:
                                self.assertTrue(result.stdout.startswith(output), result.stdout)
                            if mode in ('r', 'd'):
                                self.assertIn('guest returned 0x2a', result.stdout)
                            else:
                                self.assertIn(f'status {status}', result.stdout)
                            self.assertEqual(elf.read_bytes(), original)
                            if mode == 'e':
                                count = int(re.search(r'(\d+) instructions', result.stdout).group(1))
                                exact = subprocess.run([str(RUNNER), str(elf), str(count), '--', mode],
                                                       capture_output=True, text=True, timeout=10)
                                self.assertEqual(exact.returncode, status, exact.stdout + exact.stderr)
                                exhausted = subprocess.run([str(RUNNER), str(elf), str(count - 1), '--', mode],
                                                           capture_output=True, text=True, timeout=10)
                                self.assertEqual(exhausted.returncode, 2)
                                self.assertIn('instruction budget exceeded', exhausted.stderr)


if __name__ == '__main__':
    unittest.main()
