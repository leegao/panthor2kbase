#pragma once
// ============================================================================
// mali0_emu.hpp
// Umbrella Header for the Standalone /dev/mali0 (CSF & JM) + /dev/dma_heap
// Emulation & Valhall/Bifrost ISA -> SPIR-V 1.3 -> Host Vulkan (Lavapipe) Subproject
// ============================================================================
#include "mali0_emu_uapi.h"
#include "mali0_emu_isa.hpp"
#include "mali0_emu_lvp.hpp"
#include "mali0_emu_device.hpp"

// Helper to build standard 24-vertex (6-face) 3D unit cube with per-face normals & colors
// and 36 triangle indices (12 triangles, CCW front-facing) + 4x4 perspective MVP matrix
namespace mali0_emu_vkcube {

inline void buildUnitCubeGeometry(MaliCubeVertex out_verts[24], uint32_t out_indices[36]) {
    struct FaceDef {
        float nx, ny, nz;
        float cr, cg, cb;
        float v[4][3];
    };
    const FaceDef faces[6] = {
        // +Z Front (Red/Coral)
        {  0,  0,  1,  0.95f, 0.25f, 0.20f, { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} } },
        // -Z Back (Cyan)
        {  0,  0, -1,  0.15f, 0.85f, 0.90f, { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} } },
        // +X Right (Green)
        {  1,  0,  0,  0.20f, 0.90f, 0.35f, { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} } },
        // -X Left (Magenta)
        { -1,  0,  0,  0.90f, 0.25f, 0.85f, { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} } },
        // +Y Top (Gold/Yellow)
        {  0,  1,  0,  0.98f, 0.85f, 0.15f, { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} } },
        // -Y Bottom (Royal Blue)
        {  0, -1,  0,  0.25f, 0.45f, 0.98f, { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} } }
    };
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t k = 0; k < 4; ++k) {
            auto& dst = out_verts[f * 4 + k];
            dst.px = faces[f].v[k][0];
            dst.py = faces[f].v[k][1];
            dst.pz = faces[f].v[k][2];
            dst.pw = 1.0f;
            dst.cr = faces[f].cr;
            dst.cg = faces[f].cg;
            dst.cb = faces[f].cb;
            dst.ca = 1.0f;
            dst.nx = faces[f].nx;
            dst.ny = faces[f].ny;
            dst.nz = faces[f].nz;
            dst.nw = 0.0f;
        }
        uint32_t base = f * 4;
        out_indices[f * 6 + 0] = base + 0;
        out_indices[f * 6 + 1] = base + 1;
        out_indices[f * 6 + 2] = base + 2;
        out_indices[f * 6 + 3] = base + 0;
        out_indices[f * 6 + 4] = base + 2;
        out_indices[f * 6 + 5] = base + 3;
    }
}

// Build column-major 4x4 MVP matrix (Perspective * View(z=-3.6) * RotateY(deg) * RotateX(25 deg))
// plus normalized directional light vector in out_ubo[16..19]
inline void buildRotationMvpMatrix(float angle_deg, float out_ubo[20]) {
    float rad_y = angle_deg * 3.1415926535f / 180.0f;
    float rad_x = 24.0f     * 3.1415926535f / 180.0f;
    float cy = std::cos(rad_y), sy = std::sin(rad_y);
    float cx = std::cos(rad_x), sx = std::sin(rad_x);

    // Combined rotation R = Rx(24) * Ry(angle_deg), scaled by 0.52 so unit cube fits cleanly in NDC [-1, 1]
    float s = 0.52f;
    float r00 =  s * cy;          float r01 = 0.0f;     float r02 =  s * sy;
    float r10 =  s * sx * sy;     float r11 = s * cx;   float r12 = -s * sx * cy;
    float r20 = -0.25f * cx * sy; float r21 = 0.25f*sx; float r22 = -0.25f * cx * cy;

    // Column-major 4x4 matrix
    out_ubo[0]  = r00;  out_ubo[1]  = r10;  out_ubo[2]  = r20;  out_ubo[3]  = 0.0f;
    out_ubo[4]  = r01;  out_ubo[5]  = r11;  out_ubo[6]  = r21;  out_ubo[7]  = 0.0f;
    out_ubo[8]  = r02;  out_ubo[9]  = r12;  out_ubo[10] = r22;  out_ubo[11] = 0.0f;
    out_ubo[12] = 0.0f; out_ubo[13] = 0.0f; out_ubo[14] = 0.0f; out_ubo[15] = 1.0f;

    // Directional light vector
    out_ubo[16] = 0.408248f;
    out_ubo[17] = 0.577350f;
    out_ubo[18] = 0.707107f;
    out_ubo[19] = 0.0f;
}

} // namespace mali0_emu_vkcube
