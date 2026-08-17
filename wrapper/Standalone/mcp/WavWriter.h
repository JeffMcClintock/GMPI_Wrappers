#pragma once

// Minimal WAV writer for the offline render verb.
//
// 16-bit PCM by default rather than the float32 a plugin developer might
// expect, for one practical reason: the agent reading the file back is usually
// reaching for something like Python's `wave` module or `sox`, and neither
// reads IEEE-float WAV. A render nobody can open has not verified anything.
// Pass float32 explicitly when the extra headroom matters - clipping is
// reported either way, so choosing int16 never hides an over.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gmpi
{
namespace standalone
{
namespace mcp
{

enum class WavFormat { Int16, Float32 };

/// What the render actually produced. Returned to the caller alongside the
/// file so "did the plugin make a sound" is answerable WITHOUT reading the
/// WAV back - which is the question nearly every render is really asking.
struct AudioStats
{
    double peak = 0.0;        // largest absolute sample, pre-conversion
    double rms  = 0.0;
    int64_t clippedSamples = 0;   // |sample| > 1.0, counted before any clamping
};

inline AudioStats measure(const std::vector<std::vector<float>>& channels)
{
    AudioStats stats;
    double sumSquares = 0.0;
    int64_t total = 0;

    for (const auto& channel : channels)
    {
        for (const float sample : channel)
        {
            // NaN fails every comparison, so it would slip past a plain >
            // test and then convert to garbage. Count it as clipping: it is
            // certainly not a value the plugin meant to emit.
            if (!(sample == sample))
            {
                ++stats.clippedSamples;
                continue;
            }

            const double a = sample < 0.0f ? -static_cast<double>(sample) : static_cast<double>(sample);
            if (a > stats.peak)
                stats.peak = a;
            if (a > 1.0)
                ++stats.clippedSamples;

            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
            ++total;
        }
    }

    if (total > 0)
        stats.rms = std::sqrt(sumSquares / static_cast<double>(total));

    return stats;
}

/// Interleaves and writes. `channels` must all be the same length.
inline bool writeWav(const std::string& path,
                     const std::vector<std::vector<float>>& channels,
                     int sampleRate,
                     WavFormat format,
                     std::string& errorOut)
{
    if (channels.empty() || channels[0].empty())
    {
        errorOut = "nothing to write";
        return false;
    }

    const uint16_t numChannels = static_cast<uint16_t>(channels.size());
    const uint32_t frames      = static_cast<uint32_t>(channels[0].size());
    const uint16_t bitsPerSample = (format == WavFormat::Float32) ? 32 : 16;
    const uint16_t formatTag     = (format == WavFormat::Float32) ? 3 : 1;   // IEEE float : PCM

    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * bitsPerSample / 8);
    const uint32_t dataBytes  = frames * blockAlign;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
    {
        errorOut = "could not open '" + path + "' for writing";
        return false;
    }

    auto u32 = [f](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { fwrite(&v, 2, 1, f); };

    // Canonical 44-byte header. Little-endian throughout, which every platform
    // this builds for already is.
    fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    u32(16);
    u16(formatTag);
    u16(numChannels);
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * blockAlign);
    u16(blockAlign);
    u16(bitsPerSample);

    fwrite("data", 1, 4, f);
    u32(dataBytes);

    std::vector<uint8_t> block(blockAlign);
    for (uint32_t i = 0; i < frames; ++i)
    {
        size_t offset = 0;
        for (uint16_t ch = 0; ch < numChannels; ++ch)
        {
            float sample = channels[ch][i];
            if (!(sample == sample))    // NaN would convert to an arbitrary integer
                sample = 0.0f;

            if (format == WavFormat::Float32)
            {
                memcpy(block.data() + offset, &sample, 4);
                offset += 4;
            }
            else
            {
                // Clamp, then scale by 32767 rather than 32768: the latter
                // wraps a full-scale +1.0 to -32768, turning the loudest sample
                // into the opposite polarity.
                const float clamped = sample > 1.0f ? 1.0f : (sample < -1.0f ? -1.0f : sample);
                const int16_t v = static_cast<int16_t>(clamped * 32767.0f);
                memcpy(block.data() + offset, &v, 2);
                offset += 2;
            }
        }
        fwrite(block.data(), 1, block.size(), f);
    }

    const bool ok = (ferror(f) == 0);
    fclose(f);

    if (!ok)
        errorOut = "write failed (disk full?)";

    return ok;
}

} // namespace mcp
} // namespace standalone
} // namespace gmpi
