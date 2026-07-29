#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dragonbytez {

class Rom;

struct BgmRenderResult {
    unsigned duration_seconds = 0;
    bool loop_detected = false;
    double loop_start_seconds = 0.0;
    double loop_length_seconds = 0.0;
    bool natural_end_detected = false;
};

void render_bgm_preview_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned seconds = 30);

BgmRenderResult render_bgm_full_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds = 480,
    unsigned loop_count = 2,
    unsigned fade_seconds = 5);

BgmRenderResult write_full_stereo_wav_from_capture(
    const std::vector<std::int16_t>& captured_samples,
    std::uint32_t sample_rate,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds = 480,
    unsigned loop_count = 2,
    unsigned fade_seconds = 5);

} // namespace dragonbytez
