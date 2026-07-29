#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dragonbytez {

class Rom;

class DecompressionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DecompressionResult {
    std::vector<std::uint8_t> data;
    std::uint32_t kind = 0;
    std::uint32_t declared_size = 0;
    std::size_t input_start = 0;
    std::size_t input_end = 0;

    std::size_t packed_size() const noexcept { return input_end - input_start; }
};

DecompressionResult decompress_container(
    const Rom& rom,
    std::size_t offset,
    std::size_t maximum_output = 64 * 1024 * 1024);

DecompressionResult decompress_unknown_header(
    const Rom& rom,
    std::size_t offset,
    std::size_t maximum_output = 64 * 1024);

} // namespace dragonbytez
