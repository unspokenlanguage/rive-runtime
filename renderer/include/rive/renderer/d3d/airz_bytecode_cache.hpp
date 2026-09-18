/*
 * airz: compiled-shader (DXBC) cache for every D3DCompile the runtime does —
 * the 2D renderer's draw-shader variants (renderer/src/d3d/pipeline_manager.cpp)
 * and Ore's shader modules (renderer/src/ore/d3d11/ore_shader_module_d3d11.cpp).
 *
 * Why: both compile HLSL at runtime and cache only per render context, so every
 * new producer (every PLAY) recompiled — ~120–180 ms of 2D ubershaders on the
 * first frame of any file, plus ~750 ms for the editor's full PBR shader.
 *
 * The DXBC is a pure function of (source, entry, target, flags, compiler
 * version), cached under two independent 64-bit hashes of all of them:
 *
 *   memory  process-wide, bounded (kMaxMemoryBytes; cleared when exceeded)
 *   disk    one file per shader, so a cold start skips the compiles too.
 *           Directory: AIRZ_ORE_SHADER_CACHE if set (a path), else
 *           %LOCALAPPDATA%\airZStudio\ore-shader-cache.
 *           AIRZ_ORE_SHADER_CACHE=0 disables both levels.
 *
 * A disk entry is used only if it is a DXBC container whose declared size
 * matches; writes go to a unique temp file renamed into place, so concurrent
 * readers and writers (the pipeline manager compiles on worker threads) never
 * see a partial blob. Header-only on purpose: both consumers of the pinned
 * runtime (AirPlayEngine and the airZStudio editor) build it with their own
 * CMake, and a new source file would have to be added to both.
 *
 * Counters: rive::ore::d3d11Stats() shaderCompiles / shaderCompileMicros /
 * shaderMemoryHits / shaderDiskHits (see ore_d3d11_stats.hpp).
 */
#pragma once

#include "rive/renderer/ore/ore_d3d11_stats.hpp"

#include <windows.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rive::airz
{
using Bytecode = std::shared_ptr<const std::vector<uint8_t>>;

struct ShaderKey
{
    uint64_t a = 0, b = 0;
    bool operator==(const ShaderKey& o) const { return a == o.a && b == o.b; }
};

struct ShaderKeyHash
{
    size_t operator()(const ShaderKey& k) const
    {
        return static_cast<size_t>(k.a ^ (k.b * 0x9E3779B97F4A7C15ull));
    }
};

inline uint64_t fnv1a(const void* data, size_t n, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// An unrelated second hash (splitmix-style fold) for the collision guard.
inline uint64_t mix64(const void* data, size_t n, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i)
    {
        h += p[i] + 0x9E3779B97F4A7C15ull;
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
        h ^= h >> 31;
    }
    return h;
}

inline ShaderKey makeShaderKey(const char* src,
                               size_t srcSize,
                               const char* entry,
                               const char* target,
                               uint32_t flags)
{
    const uint32_t tail[2] = {flags, D3D_COMPILER_VERSION};
    ShaderKey k;
    k.a = fnv1a(src, srcSize, 0xCBF29CE484222325ull);
    k.a = fnv1a(entry, strlen(entry) + 1, k.a);
    k.a = fnv1a(target, strlen(target) + 1, k.a);
    k.a = fnv1a(tail, sizeof(tail), k.a);
    k.b = mix64(src, srcSize, 0x243F6A8885A308D3ull ^ srcSize);
    k.b = mix64(entry, strlen(entry) + 1, k.b);
    k.b = mix64(target, strlen(target) + 1, k.b);
    k.b = mix64(tail, sizeof(tail), k.b);
    return k;
}

class BytecodeCache
{
public:
    static constexpr size_t kMaxMemoryBytes = 64u << 20;

    static BytecodeCache& get()
    {
        static BytecodeCache cache;
        return cache;
    }

    bool enabled() const { return m_enabled; }

    // Memory, then disk (promoted to memory). Counts the hit.
    Bytecode find(const ShaderKey& key)
    {
        if (!m_enabled)
            return nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_memory.find(key);
            if (it != m_memory.end())
            {
                ore::d3d11Stats().shaderMemoryHits++;
                return it->second;
            }
        }
        if (Bytecode disk = load(key))
        {
            ore::d3d11Stats().shaderDiskHits++;
            put(key, disk);
            return disk;
        }
        return nullptr;
    }

    // Memory and disk.
    void insert(const ShaderKey& key, const void* code, size_t size)
    {
        if (!m_enabled || code == nullptr || size == 0)
            return;
        auto bytes = std::make_shared<const std::vector<uint8_t>>(
            static_cast<const uint8_t*>(code),
            static_cast<const uint8_t*>(code) + size);
        put(key, bytes);
        store(key, *bytes);
    }

private:
    BytecodeCache()
    {
        const char* env = std::getenv("AIRZ_ORE_SHADER_CACHE");
        if (env != nullptr && std::string(env) == "0")
        {
            m_enabled = false;
            return;
        }
        std::filesystem::path dir;
        if (env != nullptr && *env != '\0')
            dir = env;
        else if (const char* local = std::getenv("LOCALAPPDATA"))
            dir = std::filesystem::path(local) / "airZStudio" /
                  "ore-shader-cache";
        std::error_code ec;
        if (!dir.empty() && (std::filesystem::create_directories(dir, ec) ||
                             std::filesystem::is_directory(dir, ec)))
            m_dir = dir;
    }

    void put(const ShaderKey& key, Bytecode code)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_bytes + code->size() > kMaxMemoryBytes)
        {
            m_memory.clear();
            m_bytes = 0;
        }
        if (m_memory.emplace(key, code).second)
            m_bytes += code->size();
    }

    std::filesystem::path path(const ShaderKey& key) const
    {
        char name[48];
        snprintf(name,
                 sizeof(name),
                 "%016llx%016llx.dxbc",
                 static_cast<unsigned long long>(key.a),
                 static_cast<unsigned long long>(key.b));
        return m_dir / name;
    }

    Bytecode load(const ShaderKey& key) const
    {
        if (m_dir.empty())
            return nullptr;
        std::ifstream in(path(key), std::ios::binary);
        if (!in)
            return nullptr;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        // A DXBC container starts with "DXBC" and carries its own size at
        // byte 24; anything else is a stale or foreign file.
        if (bytes.size() < 32 || memcmp(bytes.data(), "DXBC", 4) != 0)
            return nullptr;
        uint32_t declared = 0;
        memcpy(&declared, bytes.data() + 24, sizeof(declared));
        if (declared != bytes.size())
            return nullptr;
        return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }

    void store(const ShaderKey& key, const std::vector<uint8_t>& code) const
    {
        if (m_dir.empty())
            return;
        std::error_code ec;
        const std::filesystem::path final = path(key);
        if (std::filesystem::exists(final, ec))
            return;
        std::filesystem::path tmp = final;
        tmp += ".tmp" + std::to_string(GetCurrentProcessId()) + "_" +
               std::to_string(GetCurrentThreadId());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out.write(reinterpret_cast<const char*>(code.data()),
                      static_cast<std::streamsize>(code.size()));
            if (!out)
            {
                out.close();
                std::filesystem::remove(tmp, ec);
                return;
            }
        }
        std::filesystem::rename(tmp, final, ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
    }

    bool m_enabled = true;
    std::filesystem::path m_dir;
    std::mutex m_mutex;
    std::unordered_map<ShaderKey, Bytecode, ShaderKeyHash> m_memory;
    size_t m_bytes = 0;
};

// Times a compile into the shared counters.
struct CompileTimer
{
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CompileTimer()
    {
        ore::d3d11Stats().shaderCompiles++;
        ore::d3d11Stats().shaderCompileMicros += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
    }
};
} // namespace rive::airz
