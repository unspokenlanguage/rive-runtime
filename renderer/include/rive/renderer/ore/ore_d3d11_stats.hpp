/*
 * airz: allocation / update counters for the D3D11 Ore backend.
 *
 * Monotonic; a host prints deltas per sample. Kept in an inline function-local
 * static so every translation unit in one binary shares the instance without a
 * new source file. Diagnostic only: nothing in the backend reads them.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace rive::ore
{
struct D3D11Stats
{
    std::atomic<uint64_t> buffersCreated{0};
    std::atomic<uint64_t> buffersCreatedBytes{0};
    std::atomic<uint64_t> buffersDestroyed{0};
    std::atomic<uint64_t> bufferUpdates{0};      // BufferD3D11::update calls (WRITE_DISCARD)
    std::atomic<uint64_t> bufferUpdateBytes{0};
    std::atomic<uint64_t> texturesCreated{0};
    std::atomic<uint64_t> texturesDestroyed{0};
    std::atomic<uint64_t> renderPasses{0};
};

inline D3D11Stats& d3d11Stats()
{
    static D3D11Stats stats;
    return stats;
}
} // namespace rive::ore
