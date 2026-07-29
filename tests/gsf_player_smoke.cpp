#include "dragonbytez/gsf_player.hpp"
#include "dragonbytez/rom.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cerr
                << "usage: gsf_player_smoke ROM TRACK OUTPUT.wav\n";
            return 2;
        }
        const dragonbytez::Rom rom(argv[1]);
        const std::size_t track =
            static_cast<std::size_t>(std::stoul(argv[2]));
        dragonbytez::render_bgm_preview_wav(
            rom, track, std::filesystem::path(argv[3]), 5);
        std::cout << "Rendered BGM track " << track << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gsf_player_smoke: " << error.what() << '\n';
        return 1;
    }
}
