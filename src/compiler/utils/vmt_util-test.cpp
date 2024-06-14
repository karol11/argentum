#include "utils/fake-gunit.h"
#include "utils/vmt_util.h"

namespace {

using std::unordered_map;
using vmt_util::bit_width;
using vmt_util::extract_key_bits;
using vmt_util::find_best_fit;

TEST(VmtUtil, Widths) {
	ASSERT_EQ(bit_width(0), 0);
	ASSERT_EQ(bit_width(1), 0);
	ASSERT_EQ(bit_width(2), 1);
	ASSERT_EQ(bit_width(3), 2);
	ASSERT_EQ(bit_width(4), 2);
	ASSERT_EQ(bit_width(5), 3);
	ASSERT_EQ(bit_width(6), 3);
	ASSERT_EQ(bit_width(7), 3);
	ASSERT_EQ(bit_width(8), 3);
	ASSERT_EQ(bit_width(9), 4);
	ASSERT_EQ(bit_width(16), 4);
	ASSERT_EQ(bit_width(17), 5);
	ASSERT_EQ(bit_width(32), 5);
	ASSERT_EQ(bit_width(33), 6);
	ASSERT_EQ(bit_width(64), 6);
	ASSERT_EQ(bit_width(65), 7);
	ASSERT_EQ(bit_width(128), 7);
	ASSERT_EQ(bit_width(129), 8);
	ASSERT_EQ(bit_width(256), 8);
	ASSERT_EQ(bit_width(257), 9);

	ASSERT_EQ(bit_width(1ULL << 48), 48);
	ASSERT_EQ(bit_width((1ULL << 48) + 1), 49);
}

TEST(VmtUtil, ExtractKeyBits) {
	ASSERT_EQ(extract_key_bits(0b1000, 3, 1, 3), 1);
	ASSERT_EQ(extract_key_bits(0b11000, 4, 2, 3), 3);
	ASSERT_EQ(extract_key_bits(0b101000, 5, 2, 3), 3);
	ASSERT_EQ(extract_key_bits(0b101000, 5, 3, 3), 5);
	ASSERT_EQ(extract_key_bits(0xc000000000010000ULL, 63, 4, 16), 0x0d);
}

TEST(VmtUtil, FindBestFit63) {
	auto r = find_best_fit<int>({ { ~0ULL, 0}, {0ULL, 0} });
	ASSERT_EQ(r.splinter, 63);
	ASSERT_EQ(r.width, 1);
	ASSERT_EQ(r.spread, 2);
}

TEST(VmtUtil, FindBestFit32) {
	auto r = find_best_fit<int>({ { 1ULL << 32, 0}, {0ULL, 0} });
	ASSERT_EQ(r.splinter, 32);
	ASSERT_EQ(r.width, 1);
	ASSERT_EQ(r.spread, 2);
}

TEST(VmtUtil, FindBestFitSolid3x) {
	auto r = find_best_fit<int>({
		{ 1ULL << 32, 0},
		{ 0ULL << 32, 1},
		{ 2ULL << 32 | 1ULL << 63, 2}});
	ASSERT_EQ(r.pos, 33);
	ASSERT_EQ(r.splinter, 32);
	ASSERT_EQ(r.width, 2);
	ASSERT_EQ(r.spread, 3);
}

TEST(VmtUtil, FindBestFit4xWideSplinter) {
	auto r = find_best_fit<int>({
		{ 0ULL << 32, 0},
		{ 1ULL << 32, 1},
		{ 2ULL << 32 | 1ULL << 16, 2},
		{ 2ULL << 32, 3} });
	ASSERT_EQ(r.pos, 33);
	ASSERT_EQ(r.splinter, 16);
	ASSERT_EQ(r.width, 3);
	ASSERT_EQ(r.spread, 4);
}

TEST(VmtUtil, FindBestFit4xStillCollisions) {
	auto r = find_best_fit<int>({
		{ 0ULL << 32, 0},
		{ 1ULL << 32, 1},
		{ 2ULL << 32 | 1ULL << 63, 2},
		{ 2ULL << 32, 3} });
	ASSERT_EQ(r.pos, 33);
	ASSERT_EQ(r.splinter, 32);
	ASSERT_EQ(r.width, 2);
	ASSERT_EQ(r.spread, 3);
}

}  // namespace
