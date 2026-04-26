"""
PlatformIO pre-build script: patch simulator_mock MD5Builder for Linux/WSL.

`uxjulia/crosspoint-simulator` ships `MD5Builder.h` using macOS CommonCrypto,
which fails on Linux with:
  fatal error: CommonCrypto/CommonDigest.h: No such file or directory

For Linux simulator builds we replace MD5Builder.h with the provided
MD5Builder_linux.h implementation (OpenSSL based).
Applied idempotently.
"""

Import("env")
import os
import shutil


def patch_simulator_mock_md5(env):
    if env.get("PIOENV") != "simulator":
        return

    project_dir = env["PROJECT_DIR"]
    src_dir = os.path.join(project_dir, ".pio", "libdeps", "simulator", "simulator_mock", "src")
    md5_header = os.path.join(src_dir, "MD5Builder.h")
    md5_linux_header = os.path.join(src_dir, "MD5Builder_linux.h")

    if not (os.path.isfile(md5_header) and os.path.isfile(md5_linux_header)):
        return

    with open(md5_header, "r", encoding="utf-8") as f:
        current = f.read()

    marker = "Linux stub: replace src/MD5Builder.h with this file on Linux."
    if marker in current:
        return

    shutil.copyfile(md5_linux_header, md5_header)
    print("Patched simulator_mock MD5Builder.h -> Linux/OpenSSL variant")


patch_simulator_mock_md5(env)
