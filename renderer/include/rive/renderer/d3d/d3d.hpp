/*
 * Copyright 2025 Rive
 */

#pragma once

#include <d3dcommon.h>
#include <wrl/client.h>
#include <system_error>

#include "rive/renderer/render_context.hpp"

using Microsoft::WRL::ComPtr;

#define VERIFY_OK(CODE)                                                        \
    {                                                                          \
        HRESULT hr = (CODE);                                                   \
        if (hr != S_OK)                                                        \
        {                                                                      \
            fprintf(stderr,                                                    \
                    __FILE__ ":%i: D3D error %s: %s\n",                        \
                    static_cast<int>(__LINE__),                                \
                    std::system_category().message(hr).c_str(),                \
                    #CODE);                                                    \
            fflush(stderr);                                                    \
            abort();                                                           \
        }                                                                      \
    }
namespace rive::gpu
{
struct D3DCapabilities
{
    bool supportsRasterizerOrderedViews = false;
    bool supportsTypedUAVLoadStore =
        false; // Can we load/store all UAV formats used by Rive?
    bool supportsMin16Precision =
        false; // Can we use minimum 16-bit types (e.g. min16int)?
    // AIRZ (Patch 10): typed UAV load/store of R16G16B16A16_UNORM, which a
    // 16-bit render target needs in raster-ordering mode.
    bool supportsTypedUAVLoadStore16 = false;
    bool isIntel = false;
    bool allowsUAVSlot0WithColorOutput = true;
};

struct D3DContextOptions
{
    ShaderCompilationMode shaderCompilationMode =
        ShaderCompilationMode::standard;
    bool disableRasterizerOrderedViews = false; // Primarily for testing.
    bool disableTypedUAVLoadStore = false;      // Primarily for testing.
    bool isIntel = false;
    bool isIntelArc = false;
    // AIRZ (Patch 10): gradients kept above 8 bits for a 16-bit render
    // target. The gradient texture becomes R16G16B16A16_UNORM, and two-stop
    // gradients are rendered as full 512-texel ramp rows instead of two texels
    // (whose bilinear weights have ~8 bits: 257 levels across any length).
    // Off by default: nothing changes for 8-bit targets.
    bool highPrecisionGradients = false;
};
} // namespace rive::gpu
