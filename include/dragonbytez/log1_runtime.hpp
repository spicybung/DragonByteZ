#pragma once

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
    unsigned seconds_per_track = 180);

} // namespace dragonbytez
