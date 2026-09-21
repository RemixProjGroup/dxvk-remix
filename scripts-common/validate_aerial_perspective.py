"""Validate compiled AP variants and their descriptor/ray-query contracts.

Run after `meson compile -C <build> rtx_shaders`. This checks compiled SPIR-V;
it does not replace image comparisons or GPU performance measurements.
"""

import argparse
from pathlib import Path
import struct
import subprocess


def inspect_module(path):
    binary = path.read_bytes()
    if len(binary) < 20 or len(binary) % 4:
        raise ValueError(f"{path}: malformed SPIR-V size")
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    if words[0] != 0x07230203:
        raise ValueError(f"{path}: invalid SPIR-V magic")
    capabilities = set()
    bindings = set()
    local_size = None
    offset = 5
    while offset < len(words):
        count = words[offset] >> 16
        opcode = words[offset] & 0xffff
        if not count or offset + count > len(words):
            raise ValueError(f"{path}: invalid instruction at word {offset}")
        operands = words[offset + 1:offset + count]
        if opcode == 17:  # OpCapability
            capabilities.add(operands[0])
        elif opcode == 71 and operands[1] == 33:  # OpDecorate Binding
            bindings.add(operands[2])
        elif opcode == 16 and operands[1] == 17:  # OpExecutionMode LocalSize
            local_size = operands[2:5]
        offset += count
    return capabilities, bindings, local_size


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=root / "_Comp64Release")
    parser.add_argument("--spirv-val", type=Path,
                        default=root / "external/spirv_tools/spirv-val.exe")
    args = parser.parse_args()
    shader_dir = args.build / "src/dxvk/rtx_shaders"
    # Bindings 7 (lights) and 8 (cull clusters) are present in every bake variant: the local-light
    # term is evaluated in all of them and only its VISIBILITY differs by variant. Binding 9 is the
    # local-light output volume, which the visibility pass alone does not write.
    contracts = {
        "aerial_perspective_lut": (True, {0, 1, 2, 3, 4, 5, 7, 8, 9}, (8, 8, 1)),
        "aerial_perspective_visibility": (True, {0, 1, 3, 5, 6, 7, 8}, (8, 8, 1)),
        "aerial_perspective_integrate": (False, {0, 1, 2, 3, 4, 6, 7, 8, 9}, (8, 8, 1)),
        "aerial_perspective_unshadowed": (False, {0, 1, 2, 3, 4, 7, 8, 9}, (8, 8, 1)),
        # The cull pass must NOT carry the ray-query capability: it does no tracing, and picking one
        # up would mean an include had dragged a trace into a pass dispatched over every cluster.
        "aerial_perspective_light_cull": (False, {0, 1, 2}, (4, 4, 4)),
    }
    for name, (ray_queries, expected_bindings, expected_local_size) in contracts.items():
        path = shader_dir / f"{name}.spv"
        subprocess.run([str(args.spirv_val), "--target-env", "vulkan1.2",
                        "--scalar-block-layout", str(path)], check=True)
        capabilities, bindings, local_size = inspect_module(path)
        if (4472 in capabilities) != ray_queries:  # RayQueryKHR
            raise ValueError(f"{name}: unexpected ray-query capability {capabilities}")
        if bindings != expected_bindings:
            raise ValueError(f"{name}: unexpected descriptor bindings {bindings}")
        if local_size != expected_local_size:
            raise ValueError(f"{name}: dispatch mismatch, local size {local_size}")
        print(f"PASS {name}: SPIR-V, descriptors, workgroup size, ray-query isolation")


if __name__ == "__main__":
    main()
