#ifndef _VMT_UTIL_H_
#define _VMT_UTIL_H_

#include <unordered_map>
#include <bitset>

namespace vmt_util {

inline size_t bit_width(size_t value) {
	size_t r = 0;
	auto shift = [&](size_t w) {
		if (size_t n = value >> w) {
			value = n;
			r += w;
		}
	};
	if (value > 0)
		value--;
	shift(32);
	shift(16);
	shift(8);
	shift(4);
	shift(2);
	shift(1);
	if (value)
		r++;
	return r;
}

// Extracts from `ord` `width` bits where separate 1 LSB bit at `splinter` position and all other bits are at position `msb`
// Example:
// width:5
//         msb:10   splinter:2
//          v       v
// ord: ----1011----0-- 
// result:  10110
// If width == 1, msb position doesnt affect the result
inline uint64_t extract_key_bits(uint64_t ord, size_t msb, size_t width, size_t splinter) {
	auto hi_ord = ord << (63 - msb);
	auto norm_ord = (hi_ord >> (64 - width)) & ~1ull;
	auto splinter_bit = (ord >> splinter) & 1;
	return norm_ord | splinter_bit;
}

struct fit_result {
	size_t pos = 0; 
	size_t splinter = 0;
	size_t width = 0;
	size_t spread = 0;
};

template <typename T>
fit_result find_best_fit(const std::unordered_map<uint64_t, T>& table) {
	fit_result best;
	auto find_fit = [&](size_t width) {
		auto check_fit = [&](size_t at, size_t splinter) {
			size_t spread = 0;
			std::bitset<64> occupied;
			for (auto& ord : table) {
				auto i = extract_key_bits(ord.first, at, width, splinter);
				if (!occupied.test(i)) {
					occupied.set(i);
					spread++;
				}
			}
			if (spread > best.spread) {
				best.spread = spread;
				best.pos = at;
				best.splinter = splinter;
				best.width = width;
			}
			return spread == table.size();
		};
		if (check_fit(63, 63 - width + 1))
			return true;
		for (size_t i = 16 + width - 1; i < 63; i++) {
			if (check_fit(i, i - width + 1))
				return true;
		}
		for (size_t i = 16 + width; i < 64; i++) {
			for (size_t j = i - width; j >= 16; j--)
				if (check_fit(i, j))
					return true;
		}
		return false;
	};
	size_t starting_width = std::min(
		vmt_util::bit_width(table.size()),
		size_t(6));
	if (!find_fit(starting_width) && starting_width < 6)
		find_fit(starting_width + 1);
	return best;
}

}  // namespace vmp_util

#endif  // _VMT_UTIL_H_
