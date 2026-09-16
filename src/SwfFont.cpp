#include "SwfFont.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

namespace swf
{
	// -----------------------------------------------------------------------
	// Inflate: a straight transcription of the classic "puff" reference
	// decoder (Mark Adler), which trades speed for being obviously correct.
	// Font files are a few hundred kilobytes, so speed does not matter.
	// -----------------------------------------------------------------------
	namespace
	{
		struct BitInput
		{
			const std::uint8_t* data;
			std::size_t         size;
			std::size_t         pos{ 0 };
			std::uint32_t       bitBuf{ 0 };
			int                 bitCnt{ 0 };

			// Returns the next a_need bits (LSB first) or -1 at end of input.
			int bits(int a_need)
			{
				std::uint32_t val = bitBuf;
				while (bitCnt < a_need) {
					if (pos >= size) {
						return -1;
					}
					val |= static_cast<std::uint32_t>(data[pos++]) << bitCnt;
					bitCnt += 8;
				}
				bitBuf = val >> a_need;
				bitCnt -= a_need;
				return static_cast<int>(val & ((1u << a_need) - 1));
			}
		};

		constexpr int kMaxBits = 15;
		constexpr int kMaxLitCodes = 286;
		constexpr int kMaxDistCodes = 30;
		constexpr int kMaxCodes = kMaxLitCodes + kMaxDistCodes;
		constexpr int kFixLitCodes = 288;

		struct Huffman
		{
			std::array<std::uint16_t, kMaxBits + 1> count{};
			std::array<std::uint16_t, kFixLitCodes> symbol{};
		};

		// Returns 0 for a complete code set, negative for an over-subscribed
		// set, positive for an incomplete one (allowed for single-code sets).
		int Construct(Huffman& a_h, const std::uint16_t* a_length, int a_n)
		{
			a_h.count.fill(0);
			for (int symbol = 0; symbol < a_n; ++symbol) {
				a_h.count[a_length[symbol]]++;
			}
			if (a_h.count[0] == a_n) {
				return 0;
			}
			int left = 1;
			for (int len = 1; len <= kMaxBits; ++len) {
				left <<= 1;
				left -= a_h.count[len];
				if (left < 0) {
					return left;
				}
			}
			std::array<std::uint16_t, kMaxBits + 1> offs{};
			for (int len = 1; len < kMaxBits; ++len) {
				offs[len + 1] = static_cast<std::uint16_t>(offs[len] + a_h.count[len]);
			}
			for (int symbol = 0; symbol < a_n; ++symbol) {
				if (a_length[symbol] != 0) {
					a_h.symbol[offs[a_length[symbol]]++] = static_cast<std::uint16_t>(symbol);
				}
			}
			return left;
		}

		int Decode(BitInput& a_in, const Huffman& a_h)
		{
			int code = 0;
			int first = 0;
			int index = 0;
			for (int len = 1; len <= kMaxBits; ++len) {
				const int bit = a_in.bits(1);
				if (bit < 0) {
					return -1;
				}
				code |= bit;
				const int count = a_h.count[len];
				if (code - count < first) {
					return a_h.symbol[index + (code - first)];
				}
				index += count;
				first += count;
				first <<= 1;
				code <<= 1;
			}
			return -1;
		}

		bool Codes(BitInput& a_in, std::vector<std::uint8_t>& a_out, const Huffman& a_lenCode, const Huffman& a_distCode)
		{
			static constexpr std::uint16_t lens[29]{ 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
			static constexpr std::uint16_t lext[29]{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
			static constexpr std::uint16_t dists[30]{ 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
			static constexpr std::uint16_t dext[30]{ 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

			for (;;) {
				int symbol = Decode(a_in, a_lenCode);
				if (symbol < 0) {
					return false;
				}
				if (symbol < 256) {
					a_out.push_back(static_cast<std::uint8_t>(symbol));
				} else if (symbol == 256) {
					return true;
				} else {
					symbol -= 257;
					if (symbol >= 29) {
						return false;
					}
					const int extra = a_in.bits(lext[symbol]);
					if (extra < 0) {
						return false;
					}
					const int len = lens[symbol] + extra;

					symbol = Decode(a_in, a_distCode);
					if (symbol < 0 || symbol >= 30) {
						return false;
					}
					const int dextra = a_in.bits(dext[symbol]);
					if (dextra < 0) {
						return false;
					}
					const std::size_t dist = static_cast<std::size_t>(dists[symbol]) + static_cast<std::size_t>(dextra);
					if (dist > a_out.size()) {
						return false;
					}
					const std::size_t from = a_out.size() - dist;
					for (int i = 0; i < len; ++i) {
						a_out.push_back(a_out[from + static_cast<std::size_t>(i)]);
					}
				}
			}
		}

		bool Stored(BitInput& a_in, std::vector<std::uint8_t>& a_out)
		{
			a_in.bitBuf = 0;
			a_in.bitCnt = 0;
			if (a_in.pos + 4 > a_in.size) {
				return false;
			}
			const unsigned len = a_in.data[a_in.pos] | (a_in.data[a_in.pos + 1] << 8);
			const unsigned nlen = a_in.data[a_in.pos + 2] | (a_in.data[a_in.pos + 3] << 8);
			a_in.pos += 4;
			if (len != (~nlen & 0xFFFF) || a_in.pos + len > a_in.size) {
				return false;
			}
			a_out.insert(a_out.end(), a_in.data + a_in.pos, a_in.data + a_in.pos + len);
			a_in.pos += len;
			return true;
		}

		bool Fixed(BitInput& a_in, std::vector<std::uint8_t>& a_out)
		{
			static Huffman lenCode;
			static Huffman distCode;
			static bool    built = false;
			if (!built) {
				std::uint16_t lengths[kFixLitCodes];
				int           symbol = 0;
				for (; symbol < 144; ++symbol) {
					lengths[symbol] = 8;
				}
				for (; symbol < 256; ++symbol) {
					lengths[symbol] = 9;
				}
				for (; symbol < 280; ++symbol) {
					lengths[symbol] = 7;
				}
				for (; symbol < kFixLitCodes; ++symbol) {
					lengths[symbol] = 8;
				}
				Construct(lenCode, lengths, kFixLitCodes);
				for (symbol = 0; symbol < kMaxDistCodes; ++symbol) {
					lengths[symbol] = 5;
				}
				Construct(distCode, lengths, kMaxDistCodes);
				built = true;
			}
			return Codes(a_in, a_out, lenCode, distCode);
		}

		bool Dynamic(BitInput& a_in, std::vector<std::uint8_t>& a_out)
		{
			static constexpr std::uint16_t order[19]{ 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

			const int nlenBits = a_in.bits(5);
			const int ndistBits = a_in.bits(5);
			const int ncodeBits = a_in.bits(4);
			if (nlenBits < 0 || ndistBits < 0 || ncodeBits < 0) {
				return false;
			}
			const int nlen = nlenBits + 257;
			const int ndist = ndistBits + 1;
			const int ncode = ncodeBits + 4;
			if (nlen > kMaxLitCodes || ndist > kMaxDistCodes) {
				return false;
			}

			std::uint16_t lengths[kMaxCodes]{};
			for (int index = 0; index < ncode; ++index) {
				const int v = a_in.bits(3);
				if (v < 0) {
					return false;
				}
				lengths[order[index]] = static_cast<std::uint16_t>(v);
			}
			Huffman lenCode;
			if (Construct(lenCode, lengths, 19) != 0) {
				return false;
			}

			int index = 0;
			while (index < nlen + ndist) {
				int symbol = Decode(a_in, lenCode);
				if (symbol < 0) {
					return false;
				}
				if (symbol < 16) {
					lengths[index++] = static_cast<std::uint16_t>(symbol);
				} else {
					std::uint16_t len = 0;
					int           repeat;
					if (symbol == 16) {
						if (index == 0) {
							return false;
						}
						len = lengths[index - 1];
						const int r = a_in.bits(2);
						if (r < 0) {
							return false;
						}
						repeat = 3 + r;
					} else if (symbol == 17) {
						const int r = a_in.bits(3);
						if (r < 0) {
							return false;
						}
						repeat = 3 + r;
					} else {
						const int r = a_in.bits(7);
						if (r < 0) {
							return false;
						}
						repeat = 11 + r;
					}
					if (index + repeat > nlen + ndist) {
						return false;
					}
					while (repeat--) {
						lengths[index++] = len;
					}
				}
			}
			if (lengths[256] == 0) {
				return false;
			}

			Huffman distCode;
			int     err = Construct(lenCode, lengths, nlen);
			if (err < 0 || (err > 0 && nlen - lenCode.count[0] != 1)) {
				return false;
			}
			err = Construct(distCode, lengths + nlen, ndist);
			if (err < 0 || (err > 0 && ndist - distCode.count[0] != 1)) {
				return false;
			}
			return Codes(a_in, a_out, lenCode, distCode);
		}
	}

	bool Inflate(const std::uint8_t* a_data, std::size_t a_size, std::vector<std::uint8_t>& a_out)
	{
		if (!a_data || a_size < 2) {
			return false;
		}
		// zlib header: CM must be 8 (deflate); skip it and any preset dictionary flag.
		if ((a_data[0] & 0x0F) != 8 || ((a_data[0] << 8) | a_data[1]) % 31 != 0) {
			return false;
		}
		BitInput in{ a_data + 2, a_size - 2 };
		int      last;
		do {
			last = in.bits(1);
			const int type = in.bits(2);
			if (last < 0 || type < 0) {
				return false;
			}
			bool ok;
			switch (type) {
			case 0:
				ok = Stored(in, a_out);
				break;
			case 1:
				ok = Fixed(in, a_out);
				break;
			case 2:
				ok = Dynamic(in, a_out);
				break;
			default:
				ok = false;
				break;
			}
			if (!ok) {
				return false;
			}
		} while (!last);
		return true;
	}

	// -----------------------------------------------------------------------
	// SWF parsing
	// -----------------------------------------------------------------------
	namespace
	{
		struct Reader
		{
			const std::uint8_t* data;
			std::size_t         size;
			std::size_t         pos{ 0 };
			int                 bit{ 0 };  // bits already consumed in data[pos]
			bool                overrun{ false };

			void align() noexcept
			{
				if (bit) {
					bit = 0;
					++pos;
				}
			}

			std::uint32_t ub(int a_bits) noexcept
			{
				std::uint32_t v = 0;
				for (int i = 0; i < a_bits; ++i) {
					if (pos >= size) {
						overrun = true;
						return 0;
					}
					const int b = (data[pos] >> (7 - bit)) & 1;
					v = (v << 1) | static_cast<std::uint32_t>(b);
					if (++bit == 8) {
						bit = 0;
						++pos;
					}
				}
				return v;
			}

			std::int32_t sb(int a_bits) noexcept
			{
				if (a_bits == 0) {
					return 0;
				}
				const std::uint32_t v = ub(a_bits);
				const std::uint32_t signBit = 1u << (a_bits - 1);
				return (v & signBit) ? static_cast<std::int32_t>(v | ~(signBit | (signBit - 1))) : static_cast<std::int32_t>(v);
			}

			std::uint8_t u8() noexcept
			{
				align();
				if (pos >= size) {
					overrun = true;
					return 0;
				}
				return data[pos++];
			}

			std::uint16_t u16() noexcept
			{
				const std::uint16_t lo = u8();
				const std::uint16_t hi = u8();
				return static_cast<std::uint16_t>(lo | (hi << 8));
			}

			std::uint32_t u32() noexcept
			{
				const std::uint32_t lo = u16();
				const std::uint32_t hi = u16();
				return lo | (hi << 16);
			}

			std::int16_t s16() noexcept { return static_cast<std::int16_t>(u16()); }

			void skipRect() noexcept
			{
				align();
				const int n = static_cast<int>(ub(5));
				ub(n);
				ub(n);
				ub(n);
				ub(n);
				align();
			}
		};

		void Flatten(Glyph& a_glyph, float a_x0, float a_y0, float a_cx, float a_cy, float a_x1, float a_y1, bool a_reverse)
		{
			constexpr int kSegments = 8;
			float         px = a_x0;
			float         py = a_y0;
			for (int i = 1; i <= kSegments; ++i) {
				const float t = static_cast<float>(i) / kSegments;
				const float u = 1.0f - t;
				const float x = u * u * a_x0 + 2.0f * u * t * a_cx + t * t * a_x1;
				const float y = u * u * a_y0 + 2.0f * u * t * a_cy + t * t * a_y1;
				if (a_reverse) {
					a_glyph.edges.push_back({ x, y, px, py });
				} else {
					a_glyph.edges.push_back({ px, py, x, y });
				}
				px = x;
				py = y;
			}
		}

		// One glyph SHAPE record: no style arrays, just the record stream.
		bool ParseGlyphShape(Reader& a_r, Glyph& a_glyph)
		{
			a_r.align();
			const int fillBits = static_cast<int>(a_r.ub(4));
			const int lineBits = static_cast<int>(a_r.ub(4));

			float x = 0.0f;
			float y = 0.0f;
			std::uint32_t fill0 = 0;
			std::uint32_t fill1 = 0;

			for (;;) {
				if (a_r.overrun) {
					return false;
				}
				const bool edgeRecord = a_r.ub(1) != 0;
				if (!edgeRecord) {
					const std::uint32_t flags = a_r.ub(5);
					if (flags == 0) {
						break;  // end of shape
					}
					if (flags & 0x01) {
						const int moveBits = static_cast<int>(a_r.ub(5));
						x = static_cast<float>(a_r.sb(moveBits));
						y = static_cast<float>(a_r.sb(moveBits));
					}
					if (flags & 0x02) {
						fill0 = a_r.ub(fillBits);
					}
					if (flags & 0x04) {
						fill1 = a_r.ub(fillBits);
					}
					if (flags & 0x08) {
						a_r.ub(lineBits);
					}
					if (flags & 0x10) {
						return false;  // new style arrays are not legal inside a glyph
					}
					continue;
				}

				// Interior is on the right of a FillStyle1 edge and on the left
				// of a FillStyle0 edge; flip the latter so every edge agrees.
				const bool boundary = (fill0 != 0) != (fill1 != 0);
				const bool reverse = fill0 != 0 && fill1 == 0;

				const bool straight = a_r.ub(1) != 0;
				const int  numBits = static_cast<int>(a_r.ub(4)) + 2;
				if (straight) {
					float dx = 0.0f;
					float dy = 0.0f;
					if (a_r.ub(1)) {
						dx = static_cast<float>(a_r.sb(numBits));
						dy = static_cast<float>(a_r.sb(numBits));
					} else if (a_r.ub(1)) {
						dy = static_cast<float>(a_r.sb(numBits));
					} else {
						dx = static_cast<float>(a_r.sb(numBits));
					}
					const float nx = x + dx;
					const float ny = y + dy;
					if (boundary) {
						if (reverse) {
							a_glyph.edges.push_back({ nx, ny, x, y });
						} else {
							a_glyph.edges.push_back({ x, y, nx, ny });
						}
					}
					x = nx;
					y = ny;
				} else {
					const float cx = x + static_cast<float>(a_r.sb(numBits));
					const float cy = y + static_cast<float>(a_r.sb(numBits));
					const float ax = cx + static_cast<float>(a_r.sb(numBits));
					const float ay = cy + static_cast<float>(a_r.sb(numBits));
					if (boundary) {
						Flatten(a_glyph, x, y, cx, cy, ax, ay, reverse);
					}
					x = ax;
					y = ay;
				}
			}

			if (!a_glyph.edges.empty()) {
				a_glyph.minX = a_glyph.maxX = a_glyph.edges[0].x0;
				a_glyph.minY = a_glyph.maxY = a_glyph.edges[0].y0;
				for (const auto& e : a_glyph.edges) {
					a_glyph.minX = std::min({ a_glyph.minX, e.x0, e.x1 });
					a_glyph.maxX = std::max({ a_glyph.maxX, e.x0, e.x1 });
					a_glyph.minY = std::min({ a_glyph.minY, e.y0, e.y1 });
					a_glyph.maxY = std::max({ a_glyph.maxY, e.y0, e.y1 });
				}
			}
			return !a_r.overrun;
		}

		bool ParseDefineFont(Reader a_r, std::size_t a_tagEnd, bool a_font3, Font& a_font)
		{
			a_r.u16();  // font id
			const std::uint8_t flags = a_r.u8();
			a_r.u8();  // language
			const std::uint8_t nameLen = a_r.u8();
			a_font.name.clear();
			for (std::uint8_t i = 0; i < nameLen; ++i) {
				const char c = static_cast<char>(a_r.u8());
				if (c != '\0') {
					a_font.name.push_back(c);
				}
			}
			const bool hasLayout = (flags & 0x80) != 0;
			const bool wideOffsets = (flags & 0x08) != 0;
			const bool wideCodes = (flags & 0x04) != 0 || a_font3;
			a_font.italic = (flags & 0x02) != 0;
			a_font.bold = (flags & 0x01) != 0;
			a_font.unitsPerEm = a_font3 ? 1024.0f * 20.0f : 1024.0f;
			a_font.hasLayout = hasLayout;

			const std::uint16_t numGlyphs = a_r.u16();
			if (numGlyphs == 0 || a_r.overrun) {
				return false;
			}

			const std::size_t tableStart = a_r.pos;
			std::vector<std::uint32_t> offsets(numGlyphs);
			for (auto& o : offsets) {
				o = wideOffsets ? a_r.u32() : a_r.u16();
			}
			const std::uint32_t codeTableOffset = wideOffsets ? a_r.u32() : a_r.u16();
			if (a_r.overrun) {
				return false;
			}

			a_font.glyphs.assign(numGlyphs, Glyph{});
			for (std::uint16_t i = 0; i < numGlyphs; ++i) {
				Reader g = a_r;
				g.pos = tableStart + offsets[i];
				g.bit = 0;
				if (g.pos >= a_tagEnd) {
					return false;
				}
				ParseGlyphShape(g, a_font.glyphs[i]);  // a bad glyph stays empty rather than failing the font
			}

			a_r.pos = tableStart + codeTableOffset;
			a_r.bit = 0;
			for (std::uint16_t i = 0; i < numGlyphs; ++i) {
				const std::uint16_t code = wideCodes ? a_r.u16() : a_r.u8();
				a_font.codeToGlyph.emplace(code, i);
			}
			if (a_r.overrun || a_r.pos > a_tagEnd) {
				return false;
			}

			if (hasLayout) {
				a_font.ascent = static_cast<float>(a_r.s16());
				a_font.descent = static_cast<float>(a_r.s16());
				a_r.s16();  // leading
				for (std::uint16_t i = 0; i < numGlyphs; ++i) {
					a_font.glyphs[i].advance = static_cast<float>(a_r.s16());
				}
				if (a_r.overrun) {
					a_font.hasLayout = false;
				}
			}

			if (!a_font.hasLayout) {
				// No layout: take the metrics from the outlines themselves.
				float top = 0.0f;
				float bottom = 0.0f;
				for (auto& g : a_font.glyphs) {
					if (!g.empty()) {
						top = std::min(top, g.minY);
						bottom = std::max(bottom, g.maxY);
						g.advance = g.maxX + a_font.unitsPerEm * 0.05f;
					} else {
						g.advance = a_font.unitsPerEm * 0.3f;
					}
				}
				a_font.ascent = -top;
				a_font.descent = bottom;
			}
			return true;
		}
	}

	bool ParseFonts(const std::uint8_t* a_data, std::size_t a_size, std::vector<Font>& a_out, std::string& a_error)
	{
		if (!a_data || a_size < 8) {
			a_error = "file too short";
			return false;
		}
		if (a_data[1] != 'W' || a_data[2] != 'S') {
			a_error = "not a SWF file";
			return false;
		}

		std::vector<std::uint8_t> body;
		const std::uint8_t*       bytes = nullptr;
		std::size_t               count = 0;
		switch (a_data[0]) {
		case 'F':
			bytes = a_data + 8;
			count = a_size - 8;
			break;
		case 'C':
			if (!Inflate(a_data + 8, a_size - 8, body) && body.empty()) {
				a_error = "zlib stream is damaged";
				return false;
			}
			bytes = body.data();
			count = body.size();
			break;
		case 'Z':
			a_error = "LZMA-compressed SWF (ZWS) is not supported";
			return false;
		default:
			a_error = "unknown SWF signature";
			return false;
		}

		Reader r{ bytes, count };
		r.skipRect();  // frame size
		r.u16();       // frame rate
		r.u16();       // frame count

		while (!r.overrun && r.pos + 2 <= count) {
			const std::uint16_t header = r.u16();
			const std::uint16_t code = header >> 6;
			std::uint32_t       length = header & 0x3F;
			if (length == 0x3F) {
				length = r.u32();
			}
			const std::size_t tagStart = r.pos;
			const std::size_t tagEnd = tagStart + length;
			if (tagEnd > count) {
				break;
			}
			if (code == 0) {
				break;  // End
			}
			if (code == 48 || code == 75) {
				Font   font;
				Reader tag{ bytes, tagEnd, tagStart };
				if (ParseDefineFont(tag, tagEnd, code == 75, font)) {
					a_out.push_back(std::move(font));
				}
			}
			r.pos = tagEnd;
			r.bit = 0;
		}
		return true;
	}

	// -----------------------------------------------------------------------
	// fontconfig.txt
	// -----------------------------------------------------------------------
	namespace
	{
		std::string_view Trim(std::string_view a_s)
		{
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
				a_s.remove_prefix(1);
			}
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
				a_s.remove_suffix(1);
			}
			return a_s;
		}

		// Next "quoted" token after a_from; returns npos when there is none.
		std::size_t Quoted(std::string_view a_line, std::size_t a_from, std::string& a_out)
		{
			const auto open = a_line.find('"', a_from);
			if (open == std::string_view::npos) {
				return std::string_view::npos;
			}
			const auto close = a_line.find('"', open + 1);
			if (close == std::string_view::npos) {
				return std::string_view::npos;
			}
			a_out.assign(a_line.substr(open + 1, close - open - 1));
			return close + 1;
		}

		bool StartsWithWord(std::string_view a_line, std::string_view a_word)
		{
			return a_line.size() > a_word.size() && a_line.substr(0, a_word.size()) == a_word && std::isspace(static_cast<unsigned char>(a_line[a_word.size()]));
		}
	}

	FontConfig ParseFontConfig(std::string_view a_text)
	{
		FontConfig config;
		// Skip a UTF-8 byte order mark if the file has one.
		if (a_text.size() >= 3 && static_cast<unsigned char>(a_text[0]) == 0xEF && static_cast<unsigned char>(a_text[1]) == 0xBB && static_cast<unsigned char>(a_text[2]) == 0xBF) {
			a_text.remove_prefix(3);
		}

		std::size_t start = 0;
		while (start <= a_text.size()) {
			auto end = a_text.find('\n', start);
			if (end == std::string_view::npos) {
				end = a_text.size();
			}
			const auto line = Trim(a_text.substr(start, end - start));
			start = end + 1;
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}

			if (StartsWithWord(line, "fontlib")) {
				std::string path;
				if (Quoted(line, 7, path) != std::string_view::npos) {
					config.fontLibs.push_back(std::move(path));
				}
			} else if (StartsWithWord(line, "map")) {
				std::string alias;
				std::string fontName;
				auto        next = Quoted(line, 3, alias);
				if (next == std::string_view::npos) {
					continue;
				}
				next = Quoted(line, next, fontName);
				if (next == std::string_view::npos) {
					continue;
				}
				FontConfig::Mapping mapping;
				mapping.fontName = std::move(fontName);
				mapping.style.assign(Trim(line.substr(next)));
				config.mappings[alias] = std::move(mapping);
			}
		}
		return config;
	}

	void StyleFlags(std::string_view a_style, bool& a_bold, bool& a_italic)
	{
		std::string lower;
		lower.reserve(a_style.size());
		for (const char c : a_style) {
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		a_bold = lower.find("bold") != std::string::npos;
		a_italic = lower.find("italic") != std::string::npos;
	}

	// -----------------------------------------------------------------------
	// Rasterizer: non-zero winding scanline fill with vertical supersampling
	// and exact horizontal span coverage.
	// -----------------------------------------------------------------------
	Bitmap Rasterize(const Glyph& a_glyph, float a_pixelsPerUnit)
	{
		Bitmap bitmap;
		if (a_glyph.empty() || a_pixelsPerUnit <= 0.0f) {
			return bitmap;
		}

		const float s = a_pixelsPerUnit;
		const int   left = static_cast<int>(std::floor(a_glyph.minX * s)) - 1;
		const int   top = static_cast<int>(std::floor(a_glyph.minY * s)) - 1;
		const int   right = static_cast<int>(std::ceil(a_glyph.maxX * s)) + 1;
		const int   bottom = static_cast<int>(std::ceil(a_glyph.maxY * s)) + 1;
		const int   width = right - left;
		const int   height = bottom - top;
		if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
			return bitmap;
		}

		struct PxEdge
		{
			float x0, y0, x1, y1;
		};
		std::vector<PxEdge> edges;
		edges.reserve(a_glyph.edges.size());
		for (const auto& e : a_glyph.edges) {
			if (e.y0 != e.y1) {
				edges.push_back({ e.x0 * s - static_cast<float>(left), e.y0 * s - static_cast<float>(top), e.x1 * s - static_cast<float>(left), e.y1 * s - static_cast<float>(top) });
			}
		}

		constexpr int kSub = 4;
		const float   weight = 1.0f / kSub;
		std::vector<float> acc(static_cast<std::size_t>(width) * height, 0.0f);

		struct Crossing
		{
			float x;
			int   winding;
		};
		std::vector<Crossing> crossings;

		for (int py = 0; py < height; ++py) {
			float* row = acc.data() + static_cast<std::size_t>(py) * width;
			for (int sub = 0; sub < kSub; ++sub) {
				const float y = static_cast<float>(py) + (static_cast<float>(sub) + 0.5f) / kSub;
				crossings.clear();
				for (const auto& e : edges) {
					if (e.y0 <= y && y < e.y1) {
						crossings.push_back({ e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), 1 });
					} else if (e.y1 <= y && y < e.y0) {
						crossings.push_back({ e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), -1 });
					}
				}
				if (crossings.size() < 2) {
					continue;
				}
				std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });

				int winding = 0;
				for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
					winding += crossings[i].winding;
					if (winding == 0) {
						continue;
					}
					const float xa = std::clamp(crossings[i].x, 0.0f, static_cast<float>(width));
					const float xb = std::clamp(crossings[i + 1].x, 0.0f, static_cast<float>(width));
					if (xb <= xa) {
						continue;
					}
					const int ia = static_cast<int>(xa);
					const int ib = static_cast<int>(xb);
					if (ia == ib) {
						row[ia] += (xb - xa) * weight;
					} else {
						row[ia] += (static_cast<float>(ia + 1) - xa) * weight;
						for (int k = ia + 1; k < ib; ++k) {
							row[k] += weight;
						}
						if (ib < width) {
							row[ib] += (xb - static_cast<float>(ib)) * weight;
						}
					}
				}
			}
		}

		bitmap.width = width;
		bitmap.height = height;
		bitmap.left = left;
		bitmap.top = top;
		bitmap.coverage.resize(acc.size());
		for (std::size_t i = 0; i < acc.size(); ++i) {
			bitmap.coverage[i] = static_cast<std::uint8_t>(std::clamp(acc[i], 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		return bitmap;
	}
}
