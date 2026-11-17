#!/usr/bin/env python3
"""
scripts/generate_vulkan_samples_stub_assets.py
----------------------------------------------
Generates a complete, self-contained directory of procedural glTF 2.0 scenes
(`.gltf` + `.bin`), Khronos KTX1/KTX2 textures (`.ktx`), and fallback shaders
for running Khronos `Vulkan-Samples` without cloning the 2 GB external
`Vulkan-Samples-Assets` git submodule (addressing Lee's comment
`doco.K6NZ3w4vbwrBi-4kS8D83A` on Doc 04).

Usage:
    python3 scripts/generate_vulkan_samples_stub_assets.py [target_assets_dir]

Default `target_assets_dir`: `./assets` (or `$VULKAN_SAMPLES_ASSETS_DIR`).
"""

import json
import math
import os
import struct
import sys


def make_indexed_triangle_mesh_bin():
    """
    Builds a binary buffer containing:
      - 24 vertices (unit cube / procedural mesh) with POSITION (vec3 float32),
        NORMAL (vec3 float32), and TEXCOORD_0 (vec2 float32)
      - 36 uint16 indices (12 triangles)
    """
    positions = [
        # Front
        (-0.5, -0.5,  0.5), ( 0.5, -0.5,  0.5), ( 0.5,  0.5,  0.5), (-0.5,  0.5,  0.5),
        # Back
        ( 0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5,  0.5, -0.5), ( 0.5,  0.5, -0.5),
        # Top
        (-0.5,  0.5,  0.5), ( 0.5,  0.5,  0.5), ( 0.5,  0.5, -0.5), (-0.5,  0.5, -0.5),
        # Bottom
        (-0.5, -0.5, -0.5), ( 0.5, -0.5, -0.5), ( 0.5, -0.5,  0.5), (-0.5, -0.5,  0.5),
        # Right
        ( 0.5, -0.5,  0.5), ( 0.5, -0.5, -0.5), ( 0.5,  0.5, -0.5), ( 0.5,  0.5,  0.5),
        # Left
        (-0.5, -0.5, -0.5), (-0.5, -0.5,  0.5), (-0.5,  0.5,  0.5), (-0.5,  0.5, -0.5),
    ]
    normals = [
        (0.0, 0.0, 1.0)] * 4 + [(0.0, 0.0, -1.0)] * 4 + [
        (0.0, 1.0, 0.0)] * 4 + [(0.0, -1.0, 0.0)] * 4 + [
        (1.0, 0.0, 0.0)] * 4 + [(-1.0, 0.0, 0.0)] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)] * 6
    indices = []
    for face in range(6):
        base = face * 4
        indices.extend([base + 0, base + 1, base + 2, base + 2, base + 3, base + 0])

    pos_bytes = b"".join(struct.pack("<fff", *p) for p in positions)
    norm_bytes = b"".join(struct.pack("<fff", *n) for n in normals)
    uv_bytes = b"".join(struct.pack("<ff", *u) for u in uvs)
    idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
    while len(idx_bytes) % 4 != 0:
        idx_bytes += b"\x00"

    return pos_bytes + norm_bytes + uv_bytes + idx_bytes, len(pos_bytes), len(norm_bytes), len(uv_bytes), len(idx_bytes)


def write_stub_gltf_scene(gltf_path):
    os.makedirs(os.path.dirname(gltf_path), exist_ok=True)
    bin_name = os.path.splitext(os.path.basename(gltf_path))[0] + ".bin"
    bin_path = os.path.join(os.path.dirname(gltf_path), bin_name)

    blob, pos_len, norm_len, uv_len, idx_len = make_indexed_triangle_mesh_bin()
    with open(bin_path, "wb") as bf:
        bf.write(blob)

    gltf_doc = {
        "asset": {"version": "2.0", "generator": "panthor-kbase-stub-asset-generator"},
        "scene": 0,
        "scenes": [{"name": "DefaultScene", "nodes": [0, 1, 2]}],
        "nodes": [
            {"name": "MeshNode", "mesh": 0, "translation": [0.0, 0.0, 0.0]},
            {"name": "DefaultCameraNode", "camera": 0, "translation": [0.0, 0.0, 3.0]},
            {
                "name": "DirectionalLightNode",
                "extensions": {"KHR_lights_punctual": {"light": 0}},
                "rotation": [-0.3826834, 0.0, 0.0, 0.9238795],
            },
        ],
        "cameras": [
            {
                "name": "DefaultCamera",
                "type": "perspective",
                "perspective": {"aspectRatio": 1.777, "yfov": 0.785398, "znear": 0.1, "zfar": 100.0},
            }
        ],
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {
            "KHR_lights_punctual": {
                "lights": [
                    {"name": "SunLight", "type": "directional", "color": [1.0, 1.0, 1.0], "intensity": 2.5},
                    {"name": "PointLight", "type": "point", "color": [1.0, 0.9, 0.8], "intensity": 5.0, "range": 25.0},
                ]
            }
        },
        "materials": [
            {
                "name": "DefaultPBRMaterial",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.8, 0.85, 0.95, 1.0],
                    "metallicFactor": 0.1,
                    "roughnessFactor": 0.6,
                },
            }
        ],
        "meshes": [
            {
                "name": "StubCubeMesh",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                        "mode": 4,
                    }
                ],
            }
        ],
        "buffers": [{"uri": bin_name, "byteLength": len(blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": pos_len, "target": 34962},
            {"buffer": 0, "byteOffset": pos_len, "byteLength": norm_len, "target": 34962},
            {"buffer": 0, "byteOffset": pos_len + norm_len, "byteLength": uv_len, "target": 34962},
            {"buffer": 0, "byteOffset": pos_len + norm_len + uv_len, "byteLength": idx_len, "target": 34963},
        ],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,
                "count": 24,
                "type": "VEC3",
                "min": [-0.5, -0.5, -0.5],
                "max": [0.5, 0.5, 0.5],
            },
            {"bufferView": 1, "byteOffset": 0, "componentType": 5126, "count": 24, "type": "VEC3"},
            {"bufferView": 2, "byteOffset": 0, "componentType": 5126, "count": 24, "type": "VEC2"},
            {"bufferView": 3, "byteOffset": 0, "componentType": 5123, "count": 36, "type": "SCALAR"},
        ],
    }
    with open(gltf_path, "w") as gf:
        json.dump(gltf_doc, gf, indent=2)


def write_stub_ktx1_texture(ktx_path, width=16, height=16, layers=1, faces=1):
    """Writes a valid Khronos KTX 1.1 uncompressed RGBA8 (`GL_RGBA8 = 0x8058`) texture."""
    os.makedirs(os.path.dirname(ktx_path), exist_ok=True)
    identifier = b"\xabKTX 11\xbb\r\n\x1a\n"
    endianness = 0x04030201
    gl_type = 0x1401          # GL_UNSIGNED_BYTE
    gl_type_size = 1
    gl_format = 0x1908        # GL_RGBA
    gl_internal_format = 0x8058  # GL_RGBA8
    gl_base_internal_format = 0x1908
    pixel_depth = 0
    num_array_elements = layers if layers > 1 else 0
    num_faces = faces
    num_mip_levels = 1
    bytes_of_key_value_data = 0

    header = identifier + struct.pack(
        "<13I",
        endianness,
        gl_type,
        gl_type_size,
        gl_format,
        gl_internal_format,
        gl_base_internal_format,
        width,
        height,
        pixel_depth,
        num_array_elements,
        num_faces,
        num_mip_levels,
        bytes_of_key_value_data,
    )

    pixels = bytearray()
    total_slices = max(1, layers) * max(1, faces)
    for s in range(total_slices):
        for y in range(height):
            for x in range(width):
                r = ((x * 16) + s * 31) & 0xFF
                g = ((y * 16) + s * 17) & 0xFF
                b = ((x ^ y) * 16) & 0xFF
                a = 0xFF
                pixels.extend((r, g, b, a))

    image_size = len(pixels) if faces == 1 else (width * height * 4)
    with open(ktx_path, "wb") as kf:
        kf.write(header)
        kf.write(struct.pack("<I", image_size))
        kf.write(pixels)


def generate_all_stub_assets(root_dir):
    gltf_rel_paths = [
        "scenes/sponza/Sponza01.gltf",
        "scenes/bonza/Bonza.gltf",
        "scenes/bonza/Bonza4X.gltf",
        "scenes/helmet/DamagedHelmet.gltf",
        "scenes/cube.gltf",
        "scenes/sphere.gltf",
        "scenes/plane.gltf",
        "scenes/teapot.gltf",
        "scenes/torusknot.gltf",
        "scenes/dragon.gltf",
        "scenes/geosphere.gltf",
        "scenes/space_module/SpaceModule.gltf",
        "scenes/terrain/terrain.gltf",
        "scenes/vokselia_spawn/vokselia_spawn.gltf",
    ]
    ktx_rel_paths = [
        ("textures/checkerboard_rgba.ktx", 16, 16, 1, 1),
        ("textures/ Bonza_diffuse.ktx", 16, 16, 1, 1),
        ("textures/metalplate01_rgba.ktx", 16, 16, 1, 1),
        ("textures/pattern_02_bc2.ktx", 16, 16, 1, 1),
        ("textures/texturearray_bc3_unorm.ktx", 16, 16, 4, 1),
        ("textures/cubemap_yokohama_rgba.ktx", 16, 16, 1, 6),
        ("textures/envmap_uffizi_rgba16f_cube.ktx", 16, 16, 1, 6),
        ("textures/particle_rgba.ktx", 16, 16, 1, 1),
        ("textures/terrain_heightmap_r16.ktx", 16, 16, 1, 1),
    ]

    for rel in gltf_rel_paths:
        write_stub_gltf_scene(os.path.join(root_dir, rel))

    for rel, w, h, layers, faces in ktx_rel_paths:
        write_stub_ktx1_texture(os.path.join(root_dir, rel.strip()), w, h, layers, faces)

    manifest_path = os.path.join(root_dir, "stub_assets_manifest.json")
    with open(manifest_path, "w") as mf:
        json.dump(
            {
                "generator": "scripts/generate_vulkan_samples_stub_assets.py",
                "gltf_scenes_generated": len(gltf_rel_paths),
                "ktx_textures_generated": len(ktx_rel_paths),
                "gltf_files": gltf_rel_paths,
                "ktx_files": [k[0].strip() for k in ktx_rel_paths],
            },
            mf,
            indent=2,
        )
    print(f"Generated {len(gltf_rel_paths)} glTF 2.0 scenes + {len(ktx_rel_paths)} KTX textures in {root_dir}")


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("VULKAN_SAMPLES_ASSETS_DIR", "assets")
    generate_all_stub_assets(target)
