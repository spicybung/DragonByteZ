#include "dragonbytez/analysis.hpp"
#include "dragonbytez/compression.hpp"
#include "dragonbytez/gsf_player.hpp"
#include "dragonbytez/log1_runtime.hpp"
#include "dragonbytez/rom.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::size_t parse_offset(const std::string& text) {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::invalid_argument("invalid offset");
    return static_cast<std::size_t>(value);
}

void usage() {
    std::cerr
        << "DragonByteZ 0.7.0 CLI - Legacy of Goku I / II and Buu's Fury analyzer\n\n"
        << "Usage:\n"
        << "  dragonbytez-cli header ROM\n"
        << "  dragonbytez-cli analyze ROM -o DIRECTORY\n"
        << "  dragonbytez-cli graphics ROM -o DIRECTORY\n"
        << "  dragonbytez-cli soundtrack ROM -o DIRECTORY\n"
        << "  dragonbytez-cli render-track ROM TRACK -o FILE\n"
        << "  dragonbytez-cli render-preview ROM TRACK SECONDS -o FILE\n"
        << "  dragonbytez-cli decompress ROM OFFSET -o FILE\n";
}

std::filesystem::path output_argument(int argc, char** argv, int start) {
    for (int index = start; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-o") return argv[index + 1];
    }
    throw std::invalid_argument("missing -o output path");
}

}

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            usage();
            return 2;
        }
        const std::string command = argv[1];
        const dragonbytez::Rom rom(argv[2]);
        if (command == "header") {
            dragonbytez::write_header_report(rom);
        } else if (command == "analyze") {
            dragonbytez::analyze_all(rom, output_argument(argc, argv, 3));
        } else if (command == "graphics") {
            dragonbytez::analyze_graphics(rom, output_argument(argc, argv, 3));
        } else if (command == "soundtrack") {
            dragonbytez::analyze_soundtrack(rom, output_argument(argc, argv, 3));
        } else if (command == "render-track") {
            if (argc < 6) {
                throw std::invalid_argument("render-track needs TRACK -o FILE");
            }
            const std::size_t track = parse_offset(argv[3]);
            const auto result = dragonbytez::is_log1_rom(rom)
                ? dragonbytez::render_log1_runtime_track_full_wav(
                    rom, track, output_argument(argc, argv, 4))
                : dragonbytez::render_bgm_full_wav(
                    rom, track, output_argument(argc, argv, 4));
            std::cout << "Rendered " << result.duration_seconds << " seconds";
            if (result.loop_detected) {
                std::cout << ", loop " << result.loop_length_seconds
                          << " seconds at " << result.loop_start_seconds
                          << " seconds";
            } else if (result.natural_end_detected) {
                std::cout << ", natural end detected";
            } else {
                std::cout << ", maximum-duration fallback";
            }
            std::cout << '\n';
        } else if (command == "render-preview") {
            if (argc < 7) {
                throw std::invalid_argument(
                    "render-preview needs TRACK SECONDS -o FILE");
            }
            const std::size_t track = parse_offset(argv[3]);
            const unsigned seconds = static_cast<unsigned>(parse_offset(argv[4]));
            if (dragonbytez::is_log1_rom(rom)) {
                dragonbytez::render_log1_runtime_track_preview_wav(
                    rom, track, output_argument(argc, argv, 5), seconds);
            } else {
                dragonbytez::render_bgm_preview_wav(
                    rom, track, output_argument(argc, argv, 5), seconds);
            }
        } else if (command == "decompress") {
            if (argc < 6) throw std::invalid_argument("decompress needs OFFSET -o FILE");
            const auto result =
                dragonbytez::decompress_container(rom, parse_offset(argv[3]));
            const auto output_path = output_argument(argc, argv, 4);
            std::ofstream output(output_path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(result.data.data()),
                         static_cast<std::streamsize>(result.data.size()));
            if (!output) throw std::runtime_error("cannot write decompressed file");
            std::cout << "Wrote " << result.data.size() << " bytes\n";
        } else {
            usage();
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DragonByteZ: " << error.what() << '\n';
        return 1;
    }
}
