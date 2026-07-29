#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace dragonbytez {

class Rom;

struct SpriteExportSummary {
    std::size_t resource_count = 0;
    std::size_t animation_count = 0;
    std::size_t frame_count = 0;
    std::string description;
};

SpriteExportSummary export_log2_character_sprites(
    const Rom& rom,
    const std::filesystem::path& output);

SpriteExportSummary export_buus_fury_character_sprites(
    const Rom& rom,
    const std::filesystem::path& output);

} // namespace dragonbytez
