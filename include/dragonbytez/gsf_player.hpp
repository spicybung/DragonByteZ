#pragma once

#include <cstddef>
#include <filesystem>

namespace dragonbytez {

class Rom;

void render_bgm_preview_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned seconds = 30);

} // namespace dragonbytez
