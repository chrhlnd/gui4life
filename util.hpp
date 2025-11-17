#pragma once

#include <fstream>

[[nodiscard]] inline std::size_t combine_hashes(std::size_t a, std::size_t b) { return a ^ (b + 0x9e3779b9 + (a << 6) + (a >> 2)); }

[[nodiscard]] size_t calcHash(std::ifstream& in);