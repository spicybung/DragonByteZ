#include "dragonbytez/gba_bios.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dragonbytez {

namespace {

void require_range(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t count,
    const char* message) {
    if (offset > source.size() || count > source.size() - offset) {
        throw std::runtime_error(message);
    }
}

std::size_t declared_size(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output) {
    require_range(source, offset, 4, "truncated GBA BIOS compression header");
    const std::size_t size =
        static_cast<std::size_t>(source[offset + 1]) |
        (static_cast<std::size_t>(source[offset + 2]) << 8U) |
        (static_cast<std::size_t>(source[offset + 3]) << 16U);
    if (size == 0 || size > maximum_output) {
        throw std::runtime_error("invalid GBA BIOS decompressed size");
    }
    return size;
}

std::uint32_t read_u32(
    const std::vector<std::uint8_t>& source,
    std::size_t offset) {
    require_range(source, offset, 4, "truncated GBA BIOS bit stream");
    return static_cast<std::uint32_t>(source[offset]) |
           (static_cast<std::uint32_t>(source[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(source[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(source[offset + 3]) << 24U);
}

GbaBiosResult decompress_lz77(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output) {
    const std::size_t output_size = declared_size(source, offset, maximum_output);
    GbaBiosResult result;
    result.kind = 0x10;
    result.input_start = offset;
    result.data.reserve(output_size);

    std::size_t input = offset + 4;
    while (result.data.size() < output_size) {
        require_range(source, input, 1, "truncated GBA LZ77 flag byte");
        const std::uint8_t flags = source[input++];
        for (unsigned bit = 0; bit < 8 && result.data.size() < output_size; ++bit) {
            if ((flags & (0x80U >> bit)) == 0) {
                require_range(source, input, 1, "truncated GBA LZ77 literal");
                result.data.push_back(source[input++]);
                continue;
            }

            require_range(source, input, 2, "truncated GBA LZ77 match");
            const std::uint16_t packed =
                (static_cast<std::uint16_t>(source[input]) << 8U) |
                source[input + 1];
            input += 2;
            const std::size_t length = static_cast<std::size_t>(packed >> 12U) + 3U;
            const std::size_t distance = static_cast<std::size_t>(packed & 0x0FFFU) + 1U;
            if (distance > result.data.size()) {
                throw std::runtime_error("invalid GBA LZ77 back-reference");
            }
            for (std::size_t index = 0;
                 index < length && result.data.size() < output_size;
                 ++index) {
                result.data.push_back(result.data[result.data.size() - distance]);
            }
        }
    }

    result.input_end = input;
    return result;
}

GbaBiosResult decompress_rle(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output) {
    const std::size_t output_size = declared_size(source, offset, maximum_output);
    GbaBiosResult result;
    result.kind = 0x30;
    result.input_start = offset;
    result.data.reserve(output_size);

    std::size_t input = offset + 4;
    while (result.data.size() < output_size) {
        require_range(source, input, 1, "truncated GBA RLE control byte");
        const std::uint8_t control = source[input++];
        if ((control & 0x80U) != 0) {
            require_range(source, input, 1, "truncated GBA RLE repeated byte");
            const std::size_t count = static_cast<std::size_t>(control & 0x7FU) + 3U;
            const std::uint8_t value = source[input++];
            const std::size_t writable = std::min(count, output_size - result.data.size());
            result.data.insert(result.data.end(), writable, value);
        } else {
            const std::size_t count = static_cast<std::size_t>(control & 0x7FU) + 1U;
            require_range(source, input, count, "truncated GBA RLE literal run");
            const std::size_t writable = std::min(count, output_size - result.data.size());
            result.data.insert(
                result.data.end(),
                source.begin() + static_cast<std::ptrdiff_t>(input),
                source.begin() + static_cast<std::ptrdiff_t>(input + writable));
            input += count;
        }
    }

    result.input_end = input;
    return result;
}

GbaBiosResult decompress_huffman(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output) {
    const std::uint8_t kind = source[offset];
    const unsigned symbol_bits = kind == 0x24 ? 4U : 8U;
    const std::size_t output_size = declared_size(source, offset, maximum_output);
    require_range(source, offset + 4, 1, "truncated GBA Huffman tree header");

    const std::size_t tree_base = offset + 4;
    const std::size_t tree_bytes =
        (static_cast<std::size_t>(source[tree_base]) + 1U) * 2U;
    require_range(source, tree_base, tree_bytes, "truncated GBA Huffman tree");
    std::size_t input = (tree_base + tree_bytes + 3U) & ~std::size_t(3U);

    GbaBiosResult result;
    result.kind = kind;
    result.input_start = offset;
    result.data.reserve(output_size);

    std::uint32_t word = 0;
    unsigned remaining_bits = 0;
    std::size_t node_index = 1;
    std::uint8_t pending_nibble = 0;
    bool have_pending_nibble = false;

    while (result.data.size() < output_size) {
        if (remaining_bits == 0) {
            word = read_u32(source, input);
            input += 4;
            remaining_bits = 32;
        }
        const unsigned branch = (word >> 31U) & 1U;
        word <<= 1U;
        --remaining_bits;

        if (node_index >= tree_bytes) {
            throw std::runtime_error("invalid GBA Huffman node index");
        }
        const std::uint8_t node = source[tree_base + node_index];
        const std::size_t child =
            (node_index & ~std::size_t(1U)) +
            (static_cast<std::size_t>(node & 0x3FU) + 1U) * 2U + branch;
        if (child >= tree_bytes) {
            throw std::runtime_error("invalid GBA Huffman child index");
        }
        const bool leaf = branch == 0 ? (node & 0x80U) != 0 : (node & 0x40U) != 0;
        if (!leaf) {
            node_index = child;
            continue;
        }

        const std::uint8_t symbol = source[tree_base + child];
        if (symbol_bits == 8U) {
            result.data.push_back(symbol);
        } else if (!have_pending_nibble) {
            pending_nibble = static_cast<std::uint8_t>(symbol & 0x0FU);
            have_pending_nibble = true;
        } else {
            result.data.push_back(static_cast<std::uint8_t>(
                pending_nibble | ((symbol & 0x0FU) << 4U)));
            have_pending_nibble = false;
        }
        node_index = 1;
    }

    result.input_end = input;
    return result;
}

} // namespace

bool is_gba_bios_compression_header(std::uint8_t value) noexcept {
    return value == 0x10 || value == 0x24 || value == 0x28 || value == 0x30;
}

const char* gba_bios_compression_name(std::uint8_t kind) noexcept {
    switch (kind) {
    case 0x10: return "LZ77";
    case 0x24: return "Huffman 4-bit";
    case 0x28: return "Huffman 8-bit";
    case 0x30: return "RLE";
    default: return "Unknown";
    }
}

GbaBiosResult decompress_gba_bios(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output) {
    require_range(source, offset, 1, "GBA BIOS stream offset is outside the source");
    switch (source[offset]) {
    case 0x10: return decompress_lz77(source, offset, maximum_output);
    case 0x24:
    case 0x28: return decompress_huffman(source, offset, maximum_output);
    case 0x30: return decompress_rle(source, offset, maximum_output);
    default: throw std::runtime_error("unsupported GBA BIOS compression header");
    }
}

GbaBiosRecursiveResult decompress_gba_bios_recursive(
    const std::vector<std::uint8_t>& source,
    std::size_t offset,
    std::size_t maximum_output,
    unsigned maximum_layers) {
    GbaBiosRecursiveResult recursive;
    const GbaBiosResult outer = decompress_gba_bios(source, offset, maximum_output);
    recursive.outer_input_end = outer.input_end;
    recursive.layers.push_back(outer.kind);
    recursive.data = outer.data;

    for (unsigned layer = 1; layer < maximum_layers; ++layer) {
        if (recursive.data.empty() ||
            !is_gba_bios_compression_header(recursive.data.front())) {
            break;
        }
        const GbaBiosResult nested =
            decompress_gba_bios(recursive.data, 0, maximum_output);
        if (nested.data == recursive.data) {
            throw std::runtime_error("recursive GBA BIOS stream made no progress");
        }
        recursive.layers.push_back(nested.kind);
        recursive.data = nested.data;
    }
    return recursive;
}

} // namespace dragonbytez
