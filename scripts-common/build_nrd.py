#!/usr/bin/env python3
"""Build NRD.dll from the unmodified submodules/nrd checkout, with the options Remix needs.

Called from the root meson.build while configuring, because the runtime's headers, shaders and install
step all need the result. It runs inside the Visual Studio environment meson was started from.
NRD's CMake project downloads its pinned MathLib and ShaderMake dependencies on the first run.
The build output stays in the given build directory. CMake only writes its generated, git-ignored
Shaders/NRDConfig.hlsli into the submodule.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

CMAKE_MIN_VERSION = (3, 30)

# The normal and roughness encodings must match the ones in rtx/utility/nrd.slangh.
# Quad intrinsics need VK_KHR_compute_shader_derivatives, which the runtime does not enable.
CMAKE_OPTIONS = {
    "CMAKE_BUILD_TYPE": "Release",
    "NRD_EMBEDS_SPIRV_SHADERS": "ON",
    "NRD_EMBEDS_DXIL_SHADERS": "OFF",
    "NRD_EMBEDS_DXBC_SHADERS": "OFF",
    "NRD_NORMAL_ENCODING": "2",
    "NRD_ROUGHNESS_ENCODING": "1",
    "NRD_SUPPORTS_QUAD_INTRINSICS": "OFF",
}


def cmake_version(cmake):
    try:
        output = subprocess.run([cmake, "--version"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    match = re.search(r"cmake version (\d+)\.(\d+)", output)
    return (int(match.group(1)), int(match.group(2))) if match else None


def find_cmake():
    candidates = [shutil.which("cmake")]
    candidates += glob.glob(os.path.expandvars(
        r"%ProgramFiles(x86)%\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"))
    candidates += glob.glob(os.path.expandvars(
        r"%ProgramFiles%\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"))
    for candidate in candidates:
        if candidate:
            version = cmake_version(candidate)
            if version and version >= CMAKE_MIN_VERSION:
                return candidate
    sys.exit("NRD needs CMake %d.%d or newer. Install it or add it to PATH." % CMAKE_MIN_VERSION)


def source_revision(source):
    result = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="The NRD submodule directory.")
    parser.add_argument("--build", required=True, help="The build directory, which also holds the result.")
    parser.add_argument("--jobs", type=int, default=4, help="The number of parallel compile jobs.")
    args = parser.parse_args()

    source = os.path.abspath(args.source)
    build = os.path.abspath(args.build)
    if not os.path.exists(os.path.join(source, "CMakeLists.txt")):
        sys.exit(source + " is empty. Run: git submodule update --init submodules/nrd")

    output = os.path.join(build, "bin")
    shaders = os.path.join(build, "shaders")
    dll = os.path.join(output, "NRD.dll")
    stamp = os.path.join(build, "nrd-build.stamp")
    key = source_revision(source) + " " + repr(sorted(CMAKE_OPTIONS.items()))

    if os.path.exists(dll) and os.path.exists(os.path.join(source, "Shaders", "NRDConfig.hlsli")) and os.path.exists(stamp):
        with open(stamp, encoding="utf-8") as file:
            if file.read() == key:
                print("NRD is up to date.")
                return

    os.makedirs(build, exist_ok=True)
    cmake = find_cmake()

    definitions = dict(CMAKE_OPTIONS)
    definitions["NRD_SHADERS_PATH"] = shaders.replace("\\", "/")
    definitions["CMAKE_RUNTIME_OUTPUT_DIRECTORY"] = output.replace("\\", "/")
    definitions["CMAKE_LIBRARY_OUTPUT_DIRECTORY"] = output.replace("\\", "/")
    defines = ["-D%s=%s" % item for item in definitions.items()]

    subprocess.run([cmake, "-S", source, "-B", os.path.join(build, "cmake"), "-G", "Ninja"] + defines, check=True)
    subprocess.run([cmake, "--build", os.path.join(build, "cmake"), "--target", "NRD", "--parallel", str(args.jobs)], check=True)

    if not os.path.exists(dll):
        sys.exit("The NRD build finished without producing " + dll)

    with open(stamp, "w", encoding="utf-8") as file:
        file.write(key)


if __name__ == "__main__":
    main()
