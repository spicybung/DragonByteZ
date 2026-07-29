#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dragonbytez {

struct Rgba {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

void write_png_rgba(
    const std::filesystem::path& path,
    unsigned width,
    unsigned height,
    const std::vector<Rgba>& pixels);

std::vector<Rgba> diagnostic_palette();
std::vector<Rgba> gba_rgb555_palette(const std::vector<std::uint8_t>& raw);
std::vector<std::uint8_t> untile_8bpp(
    const std::vector<std::uint8_t>& raw,
    unsigned width,
    unsigned height);
std::vector<std::uint8_t> untile_4bpp(
    const std::vector<std::uint8_t>& raw,
    unsigned width,
    unsigned height);
std::vector<Rgba> colorize(
    const std::vector<std::uint8_t>& indices,
    const std::vector<Rgba>& palette);

} // namespace dragonbytez
