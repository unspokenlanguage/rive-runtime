cmake_minimum_required(VERSION 3.28)
project(rive_runtime)

# ──────────────────────────────────────────────────────────────────────────────
# Rive Runtime — CMake wrapper for CasparCG integration
# Builds rive_core + dependency libs + rive_renderer as static libraries.
#
# Prerequisites: Run build/fetch_dependencies.ps1 to download dependencies.
# ──────────────────────────────────────────────────────────────────────────────

set(RIVE_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(RIVE_DEPS_DIR    "${RIVE_RUNTIME_DIR}/dependencies")

# ── Strip CasparCG's forced-include from this scope ──────────────────────────
# CasparCG globally sets /FIcommon/compiler/vs/disable_silly_warnings.h in
# CMAKE_CXX_FLAGS. That header doesn't exist in the rive-runtime include
# paths, so we strip it here (this does NOT affect the parent scope).
if(MSVC)
    string(REGEX REPLACE "/FI[^ ]*disable_silly_warnings[^ ]*" ""
           CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/FI[^ ]*disable_silly_warnings[^ ]*" ""
           CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
endif()

# ── Windows-specific global defines for rive scope ───────────────────────────
# Prevent Windows.h min/max macros from clashing with std::numeric_limits
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Locate downloaded dependencies ────────────────────────────────────────────

set(HARFBUZZ_DIR  "${RIVE_DEPS_DIR}/rive-app_harfbuzz_rive_10.1.0")
set(SHEENBIDI_DIR "${RIVE_DEPS_DIR}/Tehreer_SheenBidi_v2.6")
set(YOGA_DIR      "${RIVE_DEPS_DIR}/rive-app_yoga_rive_changes_v2_0_1_3_grid")
set(MINIAUDIO_DIR "${RIVE_DEPS_DIR}/rive-app_miniaudio_rive_changes_5")
set(DX_HEADERS_DIR "${RIVE_DEPS_DIR}/microsoft_DirectX-Headers_v1.615.0")
# Rive Scripting (Luau VM + libhydrogen). Cloned by UPDATING_RIVE.md §5:
#   git clone --depth 1 -b rive_0_734 https://github.com/luigi-rosso/luau.git        dependencies/luau
#   git clone --depth 1 -b rive_0_2   https://github.com/luigi-rosso/libhydrogen.git dependencies/libhydrogen
set(LUAU_DIR        "${RIVE_DEPS_DIR}/luau")
set(LIBHYDROGEN_DIR "${RIVE_DEPS_DIR}/libhydrogen")
set(RIVE_SCRIPTING_DIR "${RIVE_RUNTIME_DIR}/scripting")

# ── rive_harfbuzz: text shaping ───────────────────────────────────────────────

file(GLOB HARFBUZZ_SOURCES "${HARFBUZZ_DIR}/src/*.cc")
# Keep only the sources rive actually uses (match premake5_harfbuzz_v2.lua)
add_library(rive_harfbuzz STATIC ${HARFBUZZ_SOURCES})
target_include_directories(rive_harfbuzz PUBLIC  "${HARFBUZZ_DIR}/src")
target_include_directories(rive_harfbuzz PRIVATE "${RIVE_DEPS_DIR}")
target_compile_definitions(rive_harfbuzz PRIVATE
    HB_ONLY_ONE_SHAPER
    HAVE_OT
    HB_NO_FALLBACK_SHAPE
    HB_NO_WIN1256
    HB_NO_EXTERN_HELPERS
    HB_DISABLE_DEPRECATED
    HB_NO_BUFFER_SERIALIZE
    HB_NO_BUFFER_VERIFY
    HB_NO_BUFFER_MESSAGE
    HB_NO_SETLOCALE
    HB_NO_VERTICAL
    HB_NO_LAYOUT_COLLECT_GLYPHS
    HB_NO_LAYOUT_RARELY_USED
    HB_NO_LAYOUT_UNUSED
    HB_NO_OT_FONT_GLYPH_NAMES
    HB_NO_MMAP
    HB_NO_META
)
if(MSVC)
    target_compile_options(rive_harfbuzz PRIVATE /W0 /bigobj
        /FI"${RIVE_DEPS_DIR}/rive_harfbuzz_renames.h"
    )
endif()

# ── rive_sheenbidi: BiDi text support ─────────────────────────────────────────

# Release mode: single unity file
add_library(rive_sheenbidi STATIC "${SHEENBIDI_DIR}/Source/SheenBidi.c")
target_include_directories(rive_sheenbidi PUBLIC "${SHEENBIDI_DIR}/Headers")
target_compile_definitions(rive_sheenbidi PRIVATE SB_CONFIG_UNITY)
if(MSVC)
    target_compile_options(rive_sheenbidi PRIVATE /W0)
endif()

# ── rive_yoga: layout engine ─────────────────────────────────────────────────

set(YOGA_SOURCES
    "${YOGA_DIR}/yoga/Utils.cpp"
    "${YOGA_DIR}/yoga/YGConfig.cpp"
    "${YOGA_DIR}/yoga/YGLayout.cpp"
    "${YOGA_DIR}/yoga/YGEnums.cpp"
    "${YOGA_DIR}/yoga/YGNodePrint.cpp"
    "${YOGA_DIR}/yoga/YGNode.cpp"
    "${YOGA_DIR}/yoga/YGValue.cpp"
    "${YOGA_DIR}/yoga/YGStyle.cpp"
    "${YOGA_DIR}/yoga/Yoga.cpp"
    "${YOGA_DIR}/yoga/event/event.cpp"
    "${YOGA_DIR}/yoga/log.cpp"
    "${YOGA_DIR}/yoga/YGGridTrackList.cpp"   # grid layout: yoga rive_changes_v2_0_1_3_grid (rive >= v0.1.359)
    "${YOGA_DIR}/yoga/grid/GridLayout.cpp"
)
add_library(rive_yoga STATIC ${YOGA_SOURCES})
target_include_directories(rive_yoga PUBLIC "${YOGA_DIR}")
target_compile_definitions(rive_yoga PUBLIC YOGA_EXPORT=)
# The grid yoga (rive_changes_v2_0_1_3_grid) uses C++17 nested namespaces and C++20
# designated initializers (grid/AutoPlacement.h, grid/TrackSizing.h). clang/gcc accept
# those as extensions at C++17; MSVC requires /std:c++20 -- without it: C2429 / C7555.
# PRIVATE on purpose: the grid headers never reach rive_core, which stays C++17 to
# match upstream's core cppdialect. MSVC's STL is binary-compatible across /std:
# modes, so yoga@20 + core@17 is the same mixed arrangement already verified safe.
target_compile_features(rive_yoga PRIVATE cxx_std_20)
if(MSVC)
    # /fp:precise is REQUIRED here. The engine sets /fp:fast globally
    # (Bootstrap_Windows.cmake), but Yoga represents every "undefined" value as a
    # NaN (YGUndefined = quiet_NaN). Under /fp:fast the compiler assumes NaNs never
    # occur, so isnan()/NaN comparisons fold to constants and every layout resolves
    # to 0 — silently breaking all Rive layout components. /fp:precise (placed after
    # the global /fp:fast) restores correct NaN semantics. See CUSTOM_PATCHES.md.
    target_compile_options(rive_yoga PRIVATE /W0 /fp:precise
        /FI"${RIVE_DEPS_DIR}/rive_yoga_renames.h"
    )
endif()

# ── miniaudio: audio engine ──────────────────────────────────────────────────

add_library(rive_miniaudio STATIC "${MINIAUDIO_DIR}/miniaudio.c")
target_include_directories(rive_miniaudio PUBLIC "${MINIAUDIO_DIR}")
if(MSVC)
    target_compile_options(rive_miniaudio PRIVATE /W0)
endif()

# ── luau_vm: Rive's Luau scripting VM ────────────────────────────────────────
# Runtime only (VM, no Compiler/Ast) — templates ship pre-compiled Luau
# bytecode ("force export"), so the VM alone can execute them. Mirrors
# scripting/premake5.lua project('luau_vm'). Enables Rive Scripting so
# script-driven templates (e.g. the scrolling-ticker TickerNode) actually run.
file(GLOB LUAU_VM_SOURCES "${LUAU_DIR}/VM/src/*.cpp")
add_library(luau_vm STATIC ${LUAU_VM_SOURCES})
target_include_directories(luau_vm PUBLIC
    "${LUAU_DIR}/VM/include"
    "${LUAU_DIR}/Common/include"
)
# LUA_USE_LONGJMP: use setjmp/longjmp for error handling (no C++ exception dep).
target_compile_definitions(luau_vm PRIVATE LUA_USE_LONGJMP RIVE_LUAU)
if(MSVC)
    # /fp:precise: Luau does inf/NaN checks on doubles; keep them correct under
    # the engine's global /fp:fast (same rationale as rive_yoga).
    target_compile_options(luau_vm PRIVATE /W0 /fp:precise /bigobj
        /FI"${RIVE_SCRIPTING_DIR}/rive_luau.hpp"
    )
    target_include_directories(luau_vm PRIVATE "${RIVE_SCRIPTING_DIR}")
endif()

# ── rive_core: the core Rive runtime library ─────────────────────────────────

file(GLOB_RECURSE RIVE_CORE_SOURCES "${RIVE_RUNTIME_DIR}/src/*.cpp")

# lua/ sources are now INCLUDED — Rive Scripting is enabled (WITH_RIVE_SCRIPTING
# below) and linked against luau_vm, so the scripted/ objects run for real
# instead of compiling to empty stubs. Apple-only lua .mm variants are excluded
# by the font_hb_apple filter / the .cpp glob (which ignores .mm).
list(FILTER RIVE_CORE_SOURCES EXCLUDE REGEX ".*/profiler/.*")
list(FILTER RIVE_CORE_SOURCES EXCLUDE REGEX ".*/wasm/.*") # WASM VM needs WAMR (not built); its call sites are #ifdef WITH_RIVE_SCRIPTING_WASM, which stays OFF
# Exclude Apple-specific font sources
list(FILTER RIVE_CORE_SOURCES EXCLUDE REGEX ".*font_hb_apple.*")

# libhydrogen (crypto) — compiled into rive_core, mirrors premake5_v2.lua which
# adds libhydrogen.c to the rive project under with_rive_scripting.
list(APPEND RIVE_CORE_SOURCES "${LIBHYDROGEN_DIR}/libhydrogen.c")

add_library(rive_core STATIC ${RIVE_CORE_SOURCES})

target_include_directories(rive_core PUBLIC  "${RIVE_RUNTIME_DIR}/include")
target_include_directories(rive_core PRIVATE
    "${RIVE_RUNTIME_DIR}/src"
    "${RIVE_DEPS_DIR}"
    # Rive Scripting: Luau VM headers, libhydrogen, scripting config, and the
    # PLS renderer headers (src/lua/renderer/*.cpp include renderer types).
    "${LUAU_DIR}/VM/include"
    "${LUAU_DIR}/Common/include"
    "${LIBHYDROGEN_DIR}"
    "${RIVE_SCRIPTING_DIR}"
    "${RIVE_RUNTIME_DIR}/renderer/include"
)

# ABI-CRITICAL: WITH_RIVE_SCRIPTING gates struct members in PUBLIC headers
# (artboard.hpp m_scriptingVM, file.hpp m_scriptingVM, …). It MUST be defined
# identically for rive_core, the rive module, AND rive_renderer or Artboard/File
# struct layouts diverge → ABI-mismatch crash. Hence PUBLIC here + on rive_renderer.
target_compile_definitions(rive_core PUBLIC
    _RIVE_INTERNAL_
    WITH_RIVE_TEXT
    WITH_RIVE_LAYOUT
    WITH_RIVE_SCRIPTING
    WITH_RIVE_SCRIPTING_LUAU # upstream split: this selects the Luau VM impl (m_scriptingVM on Artboard/File); MUST match on rive_core + rive_renderer + rive module (ABI)
)
target_compile_definitions(rive_core PRIVATE
    MA_NO_RESOURCE_MANAGER
    HYDRO_SIGN_VERIFY_ONLY=1   # libhydrogen: verify-only (no signing) — runtime build
)

if(MSVC)
    target_compile_definitions(rive_core PUBLIC _USE_MATH_DEFINES)
    # /fp:precise overrides the engine's global /fp:fast. rive_core's layout code
    # (layout_component.cpp etc.) does NaN checks on Yoga's YGUndefined (NaN) values;
    # under /fp:fast those checks break. Keep layout math precise. See CUSTOM_PATCHES.md.
    target_compile_options(rive_core PRIVATE /W3 /fp:precise /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4127 /bigobj)
endif()

target_compile_features(rive_core PUBLIC cxx_std_17)

target_link_libraries(rive_core PUBLIC
    rive_harfbuzz
    rive_sheenbidi
    rive_yoga
    rive_miniaudio
    luau_vm
)

# Force-include the rename headers to avoid symbol conflicts
if(MSVC)
    target_compile_options(rive_core PRIVATE
        /FI"${RIVE_DEPS_DIR}/rive_harfbuzz_renames.h"
        /FI"${RIVE_DEPS_DIR}/rive_yoga_renames.h"
    )
endif()

# ── rive_renderer: the Rive PLS GPU renderer (compiled from source) ──────────
# Built from the v0.1.230 renderer sources with the SAME defines as rive_core
# to guarantee ABI parity. This replaces the previous IMPORTED prebuilt that
# caused the v0.1.230 crash (stack corruption from ABI mismatch between old
# renderer and new rive_core).
#
# Prerequisites: Run the shader codegen step BEFORE cmake configure:
#   make -C rive-runtime/renderer/src/shaders -j8 \
#        OUT=<build>/modules/rive/rive-runtime/include/generated/shaders \
#        FLAGS="" d3d
# (Requires: python3 + ply module, fxc from Windows SDK)

set(RIVE_RENDERER_DIR "${RIVE_RUNTIME_DIR}/renderer")
set(RIVE_RENDERER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/include")

# --- Core renderer sources (platform-independent) ---
file(GLOB RIVE_RENDERER_CORE_SOURCES "${RIVE_RENDERER_DIR}/src/*.cpp")

# --- D3D shared sources ---
file(GLOB RIVE_RENDERER_D3D_SOURCES "${RIVE_RENDERER_DIR}/src/d3d/*.cpp")

# --- D3D11 backend sources ---
file(GLOB RIVE_RENDERER_D3D11_SOURCES "${RIVE_RENDERER_DIR}/src/d3d11/*.cpp")

add_library(rive_renderer STATIC
    ${RIVE_RENDERER_CORE_SOURCES}
    ${RIVE_RENDERER_D3D_SOURCES}
    ${RIVE_RENDERER_D3D11_SOURCES}
)

# Include directories — mirrors premake5_pls_renderer.lua project('rive_pls_renderer')
target_include_directories(rive_renderer PUBLIC
    "${RIVE_RENDERER_DIR}/include"
)
target_include_directories(rive_renderer PRIVATE
    "${RIVE_RENDERER_DIR}/src"              # Internal headers (gradients, sort_key, etc.)
    "${RIVE_RENDERER_GENERATED}"            # generated/shaders/*.hpp from minify.py + fxc
    "${RIVE_RUNTIME_DIR}/include"           # rive core public headers (shared types)
    "${RIVE_RUNTIME_DIR}/decoders/include"  # rive/decoders/astc_footprints.hpp (inline, no link dep)
)

# ABI-CRITICAL: same PUBLIC defines as rive_core — these gate struct members
# in shared headers (artboard.hpp, node.hpp, text.hpp, etc.). WITH_RIVE_SCRIPTING
# adds m_scriptingVM to Artboard/File, so it MUST match rive_core here.
target_compile_definitions(rive_renderer PUBLIC
    _RIVE_INTERNAL_
    WITH_RIVE_TEXT
    WITH_RIVE_LAYOUT
    WITH_RIVE_SCRIPTING
    WITH_RIVE_SCRIPTING_LUAU # upstream split: this selects the Luau VM impl (m_scriptingVM on Artboard/File); MUST match on rive_core + rive_renderer + rive module (ABI)
    RIVE_DESKTOP_GL       # Referenced in some shared renderer headers
)

if(MSVC)
    target_compile_definitions(rive_renderer PUBLIC _USE_MATH_DEFINES)
    target_compile_options(rive_renderer PRIVATE
        /W3
        /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4127
        /bigobj
    )
endif()

target_compile_features(rive_renderer PUBLIC cxx_std_20)

# D3D11 system libraries
target_link_libraries(rive_renderer PRIVATE
    d3d11
    dxgi
    d3dcompiler
)

# ── Combined rive target for easy linking ─────────────────────────────────────

add_library(rive_full INTERFACE)
target_link_libraries(rive_full INTERFACE rive_core rive_renderer)

