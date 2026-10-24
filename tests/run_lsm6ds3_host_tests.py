#!/usr/bin/env python3
"""Compile real ST configuration code against a deterministic register-map bus."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="espet-lsm-tests-") as directory:
    obj = str(Path(directory) / "st_driver.o")
    binary = str(Path(directory) / "lsm_test")
    subprocess.run(["c++", "-std=c++17", "-DES_PET_ULTIMATE_XIAO=1", "-Isrc", "-c",
                    "src/lsm6ds3_st_driver.cpp", "-o", obj], cwd=root, check=True)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-DES_PET_ULTIMATE_XIAO=1", "-Iinclude", "-Isrc",
                    "tests/lsm6ds3_host_test.cpp", "src/lsm6ds3_support.cpp", obj,
                    "-o", binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
