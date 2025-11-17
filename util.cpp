#include "util.hpp"

#include <array>

size_t calcHash(std::ifstream& in)
{
	std::array<char, 1024> buffer{};

	std::size_t ret = 0;

	do {
		in.read(buffer.data(), buffer.size());
		if (in.gcount())
		{
			std::string_view sv(buffer.data(), in.gcount());
			std::hash<std::string_view> hash{};
			ret = combine_hashes(hash(sv), ret);
		}
	} while (!in.eof() && !in.bad());

	in.clear();
	in.seekg(0);

	return ret;
}

