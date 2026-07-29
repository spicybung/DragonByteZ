#pragma once

#include "dragonbytez/gsf_player.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace dragonbytez {

class Rom;

std::string log1_runtime_summary();
void export_log1_runtime_graphics(
    const Rom& rom,
    const std::filesystem::path& output);
void export_log1_runtime_soundtrack(
    const Rom& rom,
    const std::filesystem::path& output,
    unsigned maximum_seconds_per_track = 480);
void render_log1_runtime_track_preview_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned seconds = 60);
BgmRenderResult render_log1_runtime_track_full_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds = 480,
    unsigned loop_count = 2,
    unsigned fade_seconds = 5);

} // namespace dragonbytez
