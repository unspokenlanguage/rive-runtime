/*
 * Copyright 2025 Rive
 */

#include "rive/renderer/ore/ore_shader_module.hpp"
#include "ore_shader_module_d3d11.hpp"
#include "rive/renderer/ore/ore_d3d11_stats.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <vector>
namespace rive::ore
{

// ---------------------------------------------------------------------------
// airz: compiled-shader cache (AirPlayEngine / airZStudio).
//
// D3DCompile at optimisation level 3 costs tens of milliseconds per module, and
// every render context compiles its own modules, so every load of a scripted
// 3D layer paid it again — on the first frame a viewer sees unless the
// template was armed. The DXBC is a pure function of (source, entry, target,
// flags, compiler), so it is cached by that key:
//
//   memory  process-wide, bounded (kMaxMemoryBytes; cleared when exceeded)
//   disk    one file per module, so a cold start skips the compiles too.
//           Directory: AIRZ_ORE_SHADER_CACHE if set (a path), else
//           %LOCALAPPDATA%\airZStudio\ore-shader-cache. AIRZ_ORE_SHADER_CACHE=0
//           disables both levels (measurement / escape hatch).
//
// The key is two independent 64-bit hashes over every input, and the file name
// is both, so a collision needs both to collide. A disk entry is used only if
// it is a DXBC container (magic) of plausible size; anything else is ignored
// and overwritten. Writes go to a temp file and are renamed into place, so a
// concurrent reader never sees a partial blob.
// ---------------------------------------------------------------------------
namespace
{
constexpr UINT kCompileFlags =
    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
constexpr size_t kMaxMemoryBytes = 64u << 20;

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

uint64_t fnv1a(const void* data, size_t n, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// A second, unrelated hash (splitmix-style fold) for the collision guard.
uint64_t mix64(const void* data, size_t n, uint64_t h)
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

ShaderKey makeKey(const std::string& src, const char* entry, const char* target)
{
    const uint32_t tail[2] = {kCompileFlags, D3D_COMPILER_VERSION};
    ShaderKey k;
    k.a = fnv1a(src.data(), src.size(), 0xCBF29CE484222325ull);
    k.a = fnv1a(entry, strlen(entry) + 1, k.a);
    k.a = fnv1a(target, strlen(target) + 1, k.a);
    k.a = fnv1a(tail, sizeof(tail), k.a);
    k.b = mix64(src.data(), src.size(), 0x243F6A8885A308D3ull ^ src.size());
    k.b = mix64(entry, strlen(entry) + 1, k.b);
    k.b = mix64(target, strlen(target) + 1, k.b);
    k.b = mix64(tail, sizeof(tail), k.b);
    return k;
}

using Bytecode = std::shared_ptr<const std::vector<uint8_t>>;

class BytecodeCache
{
public:
    static BytecodeCache& get()
    {
        static BytecodeCache cache;
        return cache;
    }

    bool enabled() const { return m_enabled; }

    Bytecode find(const ShaderKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_memory.find(key);
        return it == m_memory.end() ? nullptr : it->second;
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

    Bytecode load(const ShaderKey& key)
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

    void store(const ShaderKey& key, const std::vector<uint8_t>& code)
    {
        if (m_dir.empty())
            return;
        std::error_code ec;
        const std::filesystem::path final = path(key);
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

    bool m_enabled = true;
    std::filesystem::path m_dir;
    std::mutex m_mutex;
    std::unordered_map<ShaderKey, Bytecode, ShaderKeyHash> m_memory;
    size_t m_bytes = 0;
};
} // namespace

void rive::ore::ShaderModuleD3D11::ensureD3DShaders(ID3D11Device* device,
                                                    std::string* outError)
{
    std::call_once(m_d3dInitFlag,
                   [&]() { ensureD3DShadersImpl(device, outError); });
}

void ShaderModuleD3D11::ensureD3DShadersImpl(ID3D11Device* device,
                                             std::string* outError)
{
    if (m_hlslSource.empty())
        return;

    // Normalize line endings (SPIRV-Cross outputs \r\n on Windows).
    m_hlslSource.erase(
        std::remove(m_hlslSource.begin(), m_hlslSource.end(), '\r'),
        m_hlslSource.end());

    const char* target =
        (m_stage == ShaderStage::fragment) ? "ps_5_0" : "vs_5_0";
    const char* entry =
        m_hlslEntryPoint.empty() ? "main" : m_hlslEntryPoint.c_str();
    auto& cache = BytecodeCache::get();
    const ShaderKey key = makeKey(m_hlslSource, entry, target);
    Bytecode cached;
    if (cache.enabled())
    {
        cached = cache.find(key);
        if (cached)
        {
            d3d11Stats().shaderMemoryHits++;
        }
        else if ((cached = cache.load(key)) != nullptr)
        {
            d3d11Stats().shaderDiskHits++;
            cache.put(key, cached);
        }
    }

    Microsoft::WRL::ComPtr<ID3DBlob> compiledBlob;
    if (!cached)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const auto t0 = std::chrono::steady_clock::now();
        HRESULT hr = D3DCompile(m_hlslSource.c_str(),
                                m_hlslSource.size(),
                                nullptr,
                                nullptr,
                                nullptr,
                                entry,
                                target,
                                kCompileFlags,
                                0,
                                compiledBlob.GetAddressOf(),
                                errors.GetAddressOf());
        d3d11Stats().shaderCompiles++;
        d3d11Stats().shaderCompileMicros +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
        if (FAILED(hr))
        {
            if (outError)
            {
                const char* errMsg =
                    errors
                        ? static_cast<const char*>(errors->GetBufferPointer())
                        : "(no error log)";
                char buf[1024];
                snprintf(buf,
                         sizeof(buf),
                         "D3DCompile failed (entry=%s target=%s "
                         "hr=0x%08x): %s",
                         entry,
                         target,
                         static_cast<unsigned>(hr),
                         errMsg);
                *outError = buf;
            }
            return;
        }
        if (cache.enabled())
        {
            auto fresh = std::make_shared<const std::vector<uint8_t>>(
                static_cast<const uint8_t*>(compiledBlob->GetBufferPointer()),
                static_cast<const uint8_t*>(compiledBlob->GetBufferPointer()) +
                    compiledBlob->GetBufferSize());
            cache.put(key, fresh);
            cache.store(key, *fresh);
        }
    }

    const void* code =
        cached ? static_cast<const void*>(cached->data()) : compiledBlob->GetBufferPointer();
    SIZE_T codeSize = cached ? cached->size() : compiledBlob->GetBufferSize();
    // Store compiled bytecode for CreateInputLayout.
    m_bytecode.assign(static_cast<const uint8_t*>(code),
                      static_cast<const uint8_t*>(code) + codeSize);

    if (m_stage == ShaderStage::fragment)
    {
        device->CreatePixelShader(code,
                                  codeSize,
                                  nullptr,
                                  m_d3dPixelShader.GetAddressOf());
    }
    else if (m_stage == ShaderStage::vertex)
    {
        device->CreateVertexShader(code,
                                   codeSize,
                                   nullptr,
                                   m_d3dVertexShader.GetAddressOf());
    }
    else
    {
        HRESULT hr =
            device->CreateVertexShader(code,
                                       codeSize,
                                       nullptr,
                                       m_d3dVertexShader.GetAddressOf());
        if (FAILED(hr))
        {
            device->CreatePixelShader(code,
                                      codeSize,
                                      nullptr,
                                      m_d3dPixelShader.GetAddressOf());
        }
    }
}

} // namespace rive::ore
