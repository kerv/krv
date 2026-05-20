#!/usr/bin/env python3
"""Benchmark KRV compression on diverse real files."""

import hashlib
import os
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
KRV = os.path.join(PROJECT_DIR, "krv")

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def test_file(path, label=None):
    """Compress and decompress a real file, verify correctness."""
    if not os.path.exists(path):
        print(f"  SKIP [{label or path}]: file not found")
        return None

    orig_size = os.path.getsize(path)
    if orig_size == 0:
        print(f"  SKIP [{label or path}]: empty file")
        return None

    name = label or os.path.basename(path)

    with tempfile.TemporaryDirectory() as tmp:
        comp = os.path.join(tmp, "compressed.krv")
        decomp = os.path.join(tmp, "decompressed")

        orig_hash = sha256(path)

        # Compress
        t0 = time.time()
        r = subprocess.run([sys.executable, KRV, "compress", path, "-o", comp],
                          capture_output=True, text=True)
        comp_time = time.time() - t0

        if r.returncode != 0:
            print(f"  FAIL [{name}]: compress failed: {r.stderr.strip()}")
            return None

        comp_size = os.path.getsize(comp)

        # Decompress
        t0 = time.time()
        r = subprocess.run([sys.executable, KRV, "decompress", comp, "-o", decomp],
                          capture_output=True, text=True)
        decomp_time = time.time() - t0

        if r.returncode != 0:
            print(f"  FAIL [{name}]: decompress failed: {r.stderr.strip()}")
            return None

        # Verify
        decomp_hash = sha256(decomp)
        if orig_hash != decomp_hash:
            print(f"  FAIL [{name}]: SHA-256 MISMATCH!")
            return None

        ratio = comp_size / orig_size
        comp_speed = orig_size / comp_time / 1024 / 1024 if comp_time > 0 else 0
        decomp_speed = orig_size / decomp_time / 1024 / 1024 if decomp_time > 0 else 0

        print(f"  PASS [{name}]: {orig_size:>10,}B -> {comp_size:>10,}B "
              f"ratio={ratio:.4f} ({(1-ratio)*100:>5.1f}%) "
              f"c={comp_speed:.1f}MB/s d={decomp_speed:.1f}MB/s")

        return {"name": name, "orig": orig_size, "comp": comp_size,
                "ratio": ratio, "comp_speed": comp_speed, "decomp_speed": decomp_speed}

def generate_test_file(tmp, name, data):
    """Write test data to a temp file and return path."""
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(data)
    return path

def main():
    print("KRV Compression Benchmark - Diverse Real Files")
    print("=" * 80)

    results = []

    # --- Real system files ---
    print("\n--- Text Files ---")
    real_files = [
        ("/etc/passwd", "passwd (structured text)"),
        ("/etc/services", "services (12KB text)"),
        ("/usr/share/doc/adduser/copyright", "copyright (13KB text)"),
        ("/usr/share/doc/adwaita-icon-theme/copyright", "large-copyright (110KB)"),
    ]
    for path, label in real_files:
        r = test_file(path, label)
        if r: results.append(r)

    print("\n--- Binary Executables ---")
    binaries = [
        ("/usr/bin/ls", "ls (142KB ELF)"),
        ("/usr/bin/bash", "bash (1.4MB ELF)"),
        ("/usr/bin/git", "git (4MB ELF)"),
    ]
    for path, label in binaries:
        r = test_file(path, label)
        if r: results.append(r)

    print("\n--- Shared Libraries ---")
    libs = [
        ("/usr/lib/x86_64-linux-gnu/libc.so.6", "libc (2.1MB)"),
        ("/usr/lib/x86_64-linux-gnu/libm.so.6", "libm (952KB)"),
    ]
    for path, label in libs:
        r = test_file(path, label)
        if r: results.append(r)

    print("\n--- Already Compressed ---")
    compressed = [
        ("/usr/share/man/man1/bash.1.gz", "bash.1.gz (97KB gzip)"),
        ("/usr/share/man/man1/ls.1.gz", "ls.1.gz (3KB gzip)"),
    ]
    for path, label in compressed:
        r = test_file(path, label)
        if r: results.append(r)

    print("\n--- Images ---")
    images = [
        ("/usr/share/pixmaps/nvidia-settings.png", "nvidia-settings.png"),
    ]
    for path, label in images:
        r = test_file(path, label)
        if r: results.append(r)

    # Find a JPEG
    import glob
    jpgs = glob.glob("/usr/share/doc/sysstat/images/*.jpg")
    if jpgs:
        r = test_file(jpgs[0], f"jpeg ({os.path.basename(jpgs[0])})")
        if r: results.append(r)

    print("\n--- Structured Data ---")
    xml_files = glob.glob("/usr/share/dbus-1/interfaces/*.xml")
    if xml_files:
        # Pick a larger one
        xml_files.sort(key=os.path.getsize, reverse=True)
        r = test_file(xml_files[0], f"xml ({os.path.getsize(xml_files[0])//1024}KB)")
        if r: results.append(r)

    # JSON
    json_path = "/var/log/installer/block/probe-data.json"
    if os.path.exists(json_path):
        r = test_file(json_path, f"json ({os.path.getsize(json_path)//1024}KB)")
        if r: results.append(r)

    print("\n--- Generated Large Files ---")
    with tempfile.TemporaryDirectory() as tmp:
        # 1MB random
        path = generate_test_file(tmp, "random_1mb", os.urandom(1024 * 1024))
        r = test_file(path, "random (1MB)")
        if r: results.append(r)

        # 1MB zeros
        path = generate_test_file(tmp, "zeros_1mb", b"\x00" * (1024 * 1024))
        r = test_file(path, "zeros (1MB)")
        if r: results.append(r)

        # 1MB English-like text
        sentence = b"The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. "
        path = generate_test_file(tmp, "english_1mb", (sentence * 12000)[:1024*1024])
        r = test_file(path, "english-repeat (1MB)")
        if r: results.append(r)

        # 1MB log-like data
        log_line = lambda i: f"2024-03-{(i%28)+1:02d}T{i%24:02d}:{i%60:02d}:00.000Z level=INFO msg=\"Processing request\" id={i} duration={i*3}ms status=200\n".encode()
        path = generate_test_file(tmp, "logs_1mb", b"".join(log_line(i) for i in range(9000))[:1024*1024])
        r = test_file(path, "log-data (1MB)")
        if r: results.append(r)

        # 5MB mixed binary (simulating a tar-like structure)
        chunks = []
        for i in range(50):
            # Header-like
            chunks.append(f"FILE{i:04d}".encode().ljust(64, b'\x00'))
            # Random-ish content
            chunks.append(os.urandom(50000 + i * 1000))
            # Some structured padding
            chunks.append(b'\x00' * 512)
        path = generate_test_file(tmp, "mixed_5mb", b"".join(chunks)[:5*1024*1024])
        r = test_file(path, "mixed-binary (5MB)")
        if r: results.append(r)

    # --- Summary ---
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    if results:
        print(f"{'File':<30} {'Original':>10} {'Compressed':>10} {'Ratio':>8} {'Reduction':>10}")
        print("-" * 75)
        for r in results:
            print(f"{r['name']:<30} {r['orig']:>10,} {r['comp']:>10,} {r['ratio']:>8.4f} {(1-r['ratio'])*100:>9.1f}%")

        avg_ratio = sum(r['ratio'] for r in results) / len(results)
        avg_comp = sum(r['comp_speed'] for r in results) / len(results)
        avg_dec = sum(r['decomp_speed'] for r in results) / len(results)
        print("-" * 75)
        print(f"{'AVERAGE':<30} {'':>10} {'':>10} {avg_ratio:>8.4f} {(1-avg_ratio)*100:>9.1f}%")
        print(f"Average speed: compress={avg_comp:.1f} MB/s, decompress={avg_dec:.1f} MB/s")

    return 0

if __name__ == "__main__":
    sys.exit(main())
