# Does this MicroPython actually work on this target?
#
# Every module the build registers gets touched, for the same reason check.lua
# does it: the interesting failure is not a wrong answer but a module that
# isn't there.  Beyond that the things worth asserting here are the ones the
# port had to choose -- arbitrary precision integers, single-precision floats
# and a garbage collector fed from the operating system's heap -- since those
# are what a wrong mpconfigport.h quietly changes.

checks = 0


def ok(condition, what):
    global checks
    if not condition:
        raise AssertionError("failed: " + what)
    checks += 1


import sys

ok(sys.platform == "nuttx", "platform")
ok(sys.implementation.name == "micropython", "implementation")
ok(sys.byteorder == "little", "byte order")
ok(sys.maxsize == 0x7fffffff, "small integers are 32-bit")

# Integers are arbitrary precision (MICROPY_LONGINT_IMPL_MPZ).  Without it
# 2**30 overflows, which is not the Python anybody means.
ok(2 ** 100 == 1267650600228229401496703205376, "big integers")
ok(int("9" * 30) + 1 == 10 ** 30, "big integers from strings")
ok(7 // 2 == 3 and -7 // 2 == -4 and 7 % 3 == 1, "integer arithmetic")

# Floats are single precision, so a double's worth of digits is not there to
# be asserted: 3.14159265358979 does not round-trip in 24 bits of mantissa.
import math

ok(abs(math.pi - 3.14159) < 0.001, "pi")
ok(abs(math.sqrt(2) - 1.41421) < 0.001, "sqrt")
ok(math.floor(2.7) == 2 and math.ceil(2.1) == 3, "floor and ceil")
ok(abs(math.log(math.e) - 1.0) < 0.001, "log")

# Strings and bytes.
ok("ab" * 3 == "ababab", "repetition")
ok("%d/%s/%.2f" % (7, "x", 1.5) == "7/x/1.50", "percent formatting")
ok("{}-{:04d}".format("n", 42) == "n-0042", "str.format")
ok("a,b,,c".split(",") == ["a", "b", "", "c"], "split")
ok("hello".upper().find("LL") == 2, "find")
ok(bytes([104, 105]).decode() == "hi", "bytes round trip")

# Containers, comprehensions, generators, closures.
ok([n * n for n in range(5)] == [0, 1, 4, 9, 16], "list comprehension")
ok({k: k * 2 for k in "ab"} == {"a": "aa", "b": "bb"}, "dict comprehension")
ok(sorted({3, 1, 2}) == [1, 2, 3], "sets")
ok(sum(n for n in range(101)) == 5050, "generator expression")


def counter(start):
    def step():
        nonlocal start
        start += 1
        return start
    return step


bump = counter(41)
ok(bump() == 42 and bump() == 43, "closures")

# Classes, exceptions, iteration protocol.


class Doubler:
    def __init__(self, items):
        self.items = items

    def __iter__(self):
        return iter([n * 2 for n in self.items])

    def __len__(self):
        return len(self.items)


ok(list(Doubler([1, 2])) == [2, 4] and len(Doubler([1, 2])) == 2, "classes")

try:
    1 / 0
except ZeroDivisionError as error:
    ok("divide" in str(error), "exception text")
else:
    ok(False, "ZeroDivisionError not raised")

try:
    raise ValueError("boom")
except Exception as error:
    ok(isinstance(error, ValueError), "exception hierarchy")

# The modules this build registers, each touched once.
import array
import collections
import gc
import io
import struct
import micropython

ok(list(array.array("i", [1, 2, 3])) == [1, 2, 3], "array")
Point = collections.namedtuple("Point", ("x", "y"))
ok(Point(1, 2).y == 2, "collections")
ok(struct.unpack("<HH", struct.pack("<HH", 7, 9)) == (7, 9), "struct")
ok(io.StringIO("a\nb\n").read() == "a\nb\n", "io")
ok(isinstance(micropython.mem_info, object), "micropython")

# The collector's heap comes from malloc at startup rather than from a static
# array, so the amount it reports is the amount the port asked NuttX for.
before = gc.mem_free()
waste = [bytearray(1024) for _ in range(16)]
ok(gc.mem_free() < before, "allocation shows up as used memory")
del waste
gc.collect()
ok(gc.mem_free() > before - 4096, "collection gives it back")
ok(gc.mem_alloc() + gc.mem_free() > 200000, "heap is the size it was given")

# Files and directories, through NuttX: MicroPython's own filesystems are
# not here, its POSIX one is mounted at the root instead, so a path means
# what it means at the shell prompt.
import os

path = "/tmp/mpy.dat"
with open(path, "w") as handle:
    handle.write("hello 42\n")
with open(path, "r") as handle:
    ok(handle.read().strip() == "hello 42", "file round trip")
with open(path, "rb") as handle:
    ok(handle.read(5) == b"hello", "binary read")

ok("mpy.dat" in os.listdir("/tmp"), "listdir")
ok(os.stat(path)[6] == 9, "stat size")
os.remove(path)
ok("mpy.dat" not in os.listdir("/tmp"), "remove")
ok(os.getcwd().startswith("/"), "getcwd")

# The script that is running came off the card, which means the reader and
# the import machinery are both looking at the same filesystem.
ok(__file__.endswith("check.py"), "script path")

import time

started = time.time()
ok(isinstance(started, (int, float)) and started > 0, "time")
ok(time.ticks_diff(time.ticks_ms(), 0) >= 0, "ticks")

print("PY_CHECKS=%d" % checks)
