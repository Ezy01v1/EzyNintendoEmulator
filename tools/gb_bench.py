"""Times ezy_gb_runner over 3600 frames (60.3 s of emulated time) per ROM.

Usage: python -I tools/gb_bench.py <runner.exe> <rom> [<rom> ...]
Prints, per ROM, the median of 3 runs and the fraction of one CPU core that running
at real speed would take (median seconds / 60.3).
"""

import os
import statistics
import subprocess
import sys
import tempfile
import time

FRAMES = 3600
EMULATED_SECONDS = FRAMES * 70224 / 4194304  # ~60.3


def main() -> None:
    runner, roms = os.path.abspath(sys.argv[1]), sys.argv[2:]
    shot = os.path.join(tempfile.gettempdir(), "ezy_gb_bench.bmp")
    for rom in roms:
        times = []
        for _ in range(3):
            start = time.perf_counter()
            subprocess.run([runner, rom, "--screenshot", shot, str(FRAMES)],
                           check=True, stdout=subprocess.DEVNULL)
            times.append(time.perf_counter() - start)
        median = statistics.median(times)
        print(f"{os.path.basename(rom)}: {median:.2f} s for {EMULATED_SECONDS:.1f} s emulated "
              f"= {100 * median / EMULATED_SECONDS:.1f} % of one core")


if __name__ == "__main__":
    main()
