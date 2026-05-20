#!/usr/bin/env python3
"""Round-trip correctness tests for KRV compression."""

import hashlib
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
KRV = os.path.join(PROJECT_DIR, "krv")

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def test_roundtrip(name, data):
    """Compress data, decompress, verify byte-for-byte match."""
    with tempfile.TemporaryDirectory() as tmp:
        orig = os.path.join(tmp, "input.bin")
        comp = os.path.join(tmp, "input.bin.krv")
        decomp = os.path.join(tmp, "output.bin")

        with open(orig, "wb") as f:
            f.write(data)

        orig_hash = sha256(orig)
        orig_size = len(data)

        # Compress
        r = subprocess.run([sys.executable, KRV, "compress", orig, "-o", comp],
                          capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL [{name}]: compress failed: {r.stderr}")
            return False

        comp_size = os.path.getsize(comp)

        # Decompress
        r = subprocess.run([sys.executable, KRV, "decompress", comp, "-o", decomp],
                          capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL [{name}]: decompress failed: {r.stderr}")
            return False

        # Verify
        decomp_hash = sha256(decomp)
        if orig_hash != decomp_hash:
            print(f"  FAIL [{name}]: hash mismatch!")
            return False

        ratio = comp_size / orig_size if orig_size > 0 else 0
        print(f"  PASS [{name}]: {orig_size:,}B -> {comp_size:,}B (ratio {ratio:.4f})")
        return True

def main():
    print("KRV Round-Trip Correctness Tests")
    print("=" * 50)

    passed = 0
    failed = 0

    tests = [
        # Small tests
        ("empty", b""),
        ("single_byte", b"A"),
        ("repeated_byte", b"A" * 1000),
        ("short_text", b"Hello, World! This is a test of the KRV compression algorithm."),

        # Text patterns
        ("english_text", (b"The quick brown fox jumps over the lazy dog. " * 100)),
        ("repeated_words", (b"compression algorithm data structure " * 200)),
        ("ascending_bytes", bytes(range(256)) * 10),
        ("descending_bytes", bytes(range(255, -1, -1)) * 10),

        # Binary patterns
        ("all_zeros", b"\x00" * 10000),
        ("all_ones", b"\xff" * 10000),
        ("alternating", b"\xaa\x55" * 5000),
        ("structured_records", b"".join(
            i.to_bytes(4, "little") + b"\x00" * 12 for i in range(500)
        )),

        # Random (incompressible)
        ("random_256", os.urandom(256)),
        ("random_4k", os.urandom(4096)),
        ("random_64k", os.urandom(65536)),

        # Larger text
        ("large_repetitive", (b"abcdefghijklmnopqrstuvwxyz\n" * 4000)),

        # Source-code-like
        ("source_code", (
            b"int main() {\n"
            b"    for (int i = 0; i < 100; i++) {\n"
            b"        printf(\"hello %d\\n\", i);\n"
            b"    }\n"
            b"    return 0;\n"
            b"}\n"
        ) * 50),

        # JSON-like
        ("json_data", (
            b'{"id": 12345, "name": "test", "values": [1, 2, 3, 4, 5], "active": true}\n'
        ) * 100),

        # Log-like
        ("log_lines", b"".join(
            f"2024-01-{(i%28)+1:02d} 10:{i%60:02d}:00 INFO Processing request {i}\n".encode()
            for i in range(200)
        )),
    ]

    for name, data in tests:
        if test_roundtrip(name, data):
            passed += 1
        else:
            failed += 1

    print("=" * 50)
    print(f"Results: {passed} passed, {failed} failed, {passed+failed} total")
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
