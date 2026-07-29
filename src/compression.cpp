#include "dragonbytez/compression.hpp"
#include "dragonbytez/rom.hpp"

#include <algorithm>
#include <sstream>

namespace dragonbytez {

namespace {

class WordBitReader {
public:
    WordBitReader(const Rom& rom, std::size_t offset) : rom_(rom), position_(offset) {}

    unsigned bit() {
        if (remaining_ == 0) {
            if (position_ + 4 > rom_.size()) {
                throw DecompressionError("compressed stream ended inside a bit word");
            }
            word_ = rom_.u32(position_);
            position_ += 4;
            remaining_ = 32;
        }
        const unsigned result = (word_ >> 31) & 1U;
        word_ <<= 1;
        --remaining_;
        return result;
    }

    std::uint32_t get(unsigned count) {
        if (count > 32) throw DecompressionError("invalid bit count");
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            value = (value << 1) | bit();
        }
        return value;
    }

    std::size_t position() const noexcept { return position_; }

private:
    const Rom& rom_;
    std::size_t position_;
    std::uint32_t word_ = 0;
    unsigned remaining_ = 0;
};

std::uint32_t gamma(WordBitReader& reader) {
    std::uint32_t value = 1;
    while (true) {
        value = (value << 1) | reader.bit();
        if (reader.bit() == 0) return value;
    }
}

void copy_match(
    std::vector<std::uint8_t>& output,
    std::size_t distance,
    std::size_t count,
    std::size_t expected) {
    if (distance == 0 || distance > output.size()) {
        std::ostringstream message;
        message << "invalid back-reference distance " << distance
                << " at output offset " << output.size();
        throw DecompressionError(message.str());
    }
    for (std::size_t index = 0; index < count && output.size() < expected; ++index) {
        output.push_back(output[output.size() - distance]);
    }
}

std::pair<std::vector<std::uint8_t>, std::size_t> decompress_bitstream(
    const Rom& rom,
    std::size_t payload,
    std::size_t expected,
    unsigned initial_literal_bits,
    bool require_expected_size) {
    WordBitReader reader(rom, payload);
    std::vector<std::uint8_t> output;
    output.reserve(expected);
    std::uint32_t literal_base = 0;
    unsigned literal_bits = initial_literal_bits;
    unsigned distance_bits = 8;
    std::size_t previous_distance = 1;

    while (output.size() < expected) {
        if (reader.bit()) {
            output.push_back(static_cast<std::uint8_t>(
                literal_base + reader.get(literal_bits)));
            continue;
        }

        if (reader.bit()) {
            const std::uint32_t distance_code = gamma(reader);
            if (distance_code == 2) {
                copy_match(output, previous_distance, gamma(reader), expected);
                continue;
            }
            const std::uint32_t high = distance_code - 3;
            const std::size_t distance =
                reader.get(distance_bits) + (high << distance_bits);
            previous_distance = distance;
            std::size_t count = gamma(reader);
            if (distance >= 0x10000) count += 3;
            else if (distance >= 0x37FF) count += 2;
            else if (distance >= 0x027F) count += 1;
            else if (distance <= 0x007F) count += 4;
            copy_match(output, distance, count, expected);
            continue;
        }

        if (reader.bit() == 0) {
            const std::size_t distance = reader.get(7);
            if (distance != 0) {
                previous_distance = distance;
                copy_match(output, distance, reader.get(2) + 2, expected);
                continue;
            }
            const unsigned width = reader.get(2);
            if (width == 0) {
                if (require_expected_size && output.size() != expected) {
                    throw DecompressionError(
                        "compressed stream ended before its declared size");
                }
                break;
            }
            distance_bits = reader.get(width + 3);
            continue;
        }

        const int short_distance = static_cast<int>(reader.get(4)) - 1;
        if (short_distance == 0) {
            output.push_back(static_cast<std::uint8_t>(literal_base));
        } else if (short_distance > 0) {
            copy_match(output, static_cast<std::size_t>(short_distance), 1, expected);
        } else if (reader.bit()) {
            while (true) {
                output.push_back(static_cast<std::uint8_t>(reader.get(8)));
                if (output.size() >= expected) break;
                output.push_back(static_cast<std::uint8_t>(reader.get(8)));
                if (output.size() >= expected || reader.bit() == 0) break;
            }
        } else {
            literal_bits = 7 + reader.bit();
            if (literal_bits != 8) literal_base = reader.get(8);
        }
    }

    if (require_expected_size && output.size() != expected) {
        throw DecompressionError("compressed output did not match its declared size");
    }
    return {std::move(output), reader.position()};
}

} // namespace

DecompressionResult decompress_container(
    const Rom& rom,
    std::size_t offset,
    std::size_t maximum_output) {
    if (offset + 8 > rom.size()) {
        throw DecompressionError("container header is outside the ROM");
    }
    DecompressionResult result;
    result.kind = rom.u32(offset);
    result.declared_size = rom.u32(offset + 4);
    result.input_start = offset;
    if (result.declared_size > maximum_output) {
        throw DecompressionError("declared output exceeds the configured safety limit");
    }

    const std::size_t payload = offset + 8;
    if (result.kind == 0) {
        result.data = rom.slice(payload, result.declared_size);
        result.input_end = payload + result.declared_size;
    } else if (result.kind == 1 || result.kind == 2) {
        auto decoded = decompress_bitstream(
            rom, payload, result.declared_size, result.kind, true);
        result.data = std::move(decoded.first);
        result.input_end = decoded.second;
    } else {
        throw DecompressionError("unsupported Webfoot container kind");
    }
    return result;
}

DecompressionResult decompress_unknown_header(
    const Rom& rom,
    std::size_t offset,
    std::size_t maximum_output) {
    if (offset + 8 > rom.size()) {
        throw DecompressionError("unknown-header stream is outside the ROM");
    }
    DecompressionResult result;
    result.kind = rom.u32(offset);
    result.input_start = offset;
    auto decoded = decompress_bitstream(
        rom, offset + 4, maximum_output, 8, false);
    result.data = std::move(decoded.first);
    result.declared_size =
        static_cast<std::uint32_t>(result.data.size());
    result.input_end = decoded.second;
    return result;
}

} // namespace dragonbytez
