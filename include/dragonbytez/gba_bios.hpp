#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dragonbytez {

struct GbaBiosResult {
    std::vector<std::uint8_t> data;
    std::uint8_t kind = 0;
    std::size_t input_start = 0;
    std::size_t input_end = 0;
};

bool is_gba_bios_compression_header(std::uint8_t value) noexcept;
const char* gba_bios_compression_name(std::uint8_t kind) noexcept;

GbaBiosResult decompress_gba_bios(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output = 16U * 1024U * 1024U);

struct GbaBiosRecursiveResult {
    std::vector<std::uint8_t> data;
    std::size_t outer_input_end = 0;
    std::vector<std::uint8_t> layers;
};

GbaBiosRecursiveResult decompress_gba_bios_recursive(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output = 16U * 1024U * 1024U,
    unsigned maximum_layers = 8);

} // namespace dragonbytez
