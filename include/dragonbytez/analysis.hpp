#pragma once

#include <filesystem>
#include <string>

namespace dragonbytez {

class Rom;

void analyze_all(const Rom& rom, const std::filesystem::path& output);
void analyze_graphics(const Rom& rom, const std::filesystem::path& output);
void analyze_soundtrack(const Rom& rom, const std::filesystem::path& output);
void write_header_report(const Rom& rom);
std::string level_summary(const Rom& rom);

} // namespace dragonbytez
