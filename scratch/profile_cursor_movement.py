#!/usr/bin/env python3
"""Profile cursor movement callbacks using SIGUSR1 for reliable metric extraction."""
import subprocess
import time
import os
import signal

RESULTS = []

def make_test_file(path, nlines):
    with open(path, "w") as f:
        for i in range(1, nlines + 1):
            if i % 10 == 0:
                f.write(f"## Heading {i}\n")
            elif i % 7 == 0:
                f.write(f"- bullet item {i}\n")
            else:
                f.write(f"Line {i}: normal paragraph text.\n")

def profile_file(label, filename):
    print(f"\n=====================================")
    print(f"PROFILING: {label} ({filename})")
    print(f"=====================================")

    report_path = "/tmp/qirtas_profile.txt"
    if os.path.exists(report_path):
        os.remove(report_path)

    p = subprocess.Popen(
        ["./zig-out/bin/qirtas", filename],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Wait for app to fully start
    time.sleep(3.0)

    if p.poll() is not None:
        print(f"App exited early! rc={p.returncode}")
        return

    # Press Down key ~25x/sec for 5 seconds = ~125 presses
    print("Sending Down keys for 5 seconds...")
    t0 = time.time()
    key_count = 0
    while time.time() - t0 < 5.0:
        subprocess.run(["wtype", "-k", "Down"], capture_output=True)
        key_count += 1
        time.sleep(0.04)

    print(f"Sent {key_count} Down presses.")

    # Wait for pending idle callbacks to drain
    time.sleep(1.0)

    # Dump metrics via SIGUSR1
    print("Sending SIGUSR1 to dump metrics...")
    p.send_signal(signal.SIGUSR1)
    time.sleep(0.3)

    # Kill the app
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()

    # Read report
    print(f"\n--- PROFILING REPORT: {label} ---")
    if os.path.exists(report_path):
        with open(report_path) as f:
            content = f.read()
        print(content)
        RESULTS.append((label, content))
    else:
        print("ERROR: no report file written")
        RESULTS.append((label, None))

def main():
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    print("Creating test files...")
    make_test_file("test_1000.md", 1000)
    make_test_file("test_5000.md", 5000)

    profile_file("1000 lines", "test_1000.md")
    profile_file("5000 lines", "test_5000.md")

    print("\n\n========== SUMMARY ==========")
    for label, content in RESULTS:
        print(f"\n--- {label} ---")
        if content:
            print(content)
        else:
            print("NO DATA")

if __name__ == "__main__":
    main()
