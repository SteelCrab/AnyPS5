import random
import subprocess
import sys
import unittest
from pathlib import Path


DRIVER = Path(sys.argv.pop(1)).resolve()


def signed(value, width):
    return value - (1 << width) if value & (1 << (width - 1)) else value


def register_write(original, value, width):
    bits = (1 << width) - 1
    return (original & ~bits if width < 32 else 0) | (value & bits)


def reference(width, operation, low, high, operand):
    bits = (1 << width) - 1
    sign = 1 << (width - 1)
    left = low & bits
    right = operand & bits
    signed_operation = operation & 1
    if operation < 2:
        product = (signed(left, width) * signed(right, width)) if signed_operation else left * right
        overflow = not (-sign <= product < sign) if signed_operation else product > bits
        if width == 8:
            result_low, result_high = register_write(low, product, 16), high
        else:
            result_low = register_write(low, product, width)
            result_high = register_write(high, product >> width, width)
        return 0, result_low, result_high, (0xed7 & ~0x801) | (0x801 if overflow else 0), 0
    if right == 0:
        return 3, low, high, 0xed7, 0x1000
    numerator = low & 0xffff if width == 8 else ((high & bits) << width) | left
    if signed_operation:
        numerator = signed(numerator, width * 2)
        right = signed(right, width)
    quotient = abs(numerator) // abs(right)
    if (numerator < 0) != (right < 0):
        quotient = -quotient
    remainder = numerator - quotient * right
    if not ((-sign <= quotient < sign) if signed_operation else (0 <= quotient <= bits)):
        return 4, low, high, 0xed7, 0x1000
    if width == 8:
        result_low, result_high = register_write(low, (quotient & bits) | ((remainder & bits) << 8), 16), high
    else:
        result_low = register_write(low, quotient, width)
        result_high = register_write(high, remainder, width)
    return 0, result_low, result_high, 0xed7, 0


class WideIntegerTests(unittest.TestCase):
    def test_native_multiply_divide_against_arbitrary_precision_reference(self):
        random_values = random.Random(0xa64)
        cases = []
        for width in (8, 16, 32, 64):
            bits = (1 << width) - 1
            sign = 1 << (width - 1)
            values = (0, 1, 2, 3, sign - 1, sign, sign + 1, bits - 1, bits)
            for operation in range(4):
                for low in values:
                    for operand in values:
                        for high in (0, 1, bits):
                            cases.append((width, operation, low, high, operand))
                for _ in range(120):
                    cases.append((width, operation, random_values.getrandbits(64), random_values.getrandbits(64), random_values.getrandbits(64)))
        requests = ''.join(f'{width} {operation} {low:x} {high:x} {operand:x}\n' for width, operation, low, high, operand in cases)
        result = subprocess.run([str(DRIVER), '--wide-arithmetic'], input=requests, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = result.stdout.splitlines()
        self.assertEqual(len(responses), len(cases))
        for case, response in zip(cases, responses):
            fault, *values = response.split()
            actual = (int(fault), *(int(value, 16) for value in values))
            self.assertEqual(actual, reference(*case), f'wide arithmetic {case}')


if __name__ == '__main__':
    unittest.main()
