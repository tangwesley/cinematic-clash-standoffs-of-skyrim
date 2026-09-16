#pragma once

// ---------------------------------------------------------------------------
// Reader for the fonts Skyrim's UI actually draws with.
//
// The game has no TrueType files: Interface/fontconfig.txt lists "fontlib"
// SWF files and maps names like $EverywhereFont onto a font inside them, and
// each font is a Flash DefineFont2/3 tag holding glyph outlines. Font
// replacer mods ship their own fontconfig.txt and SWFs, so reading the same
// files the game reads is how the overlay ends up in the same face.
//
// This module is deliberately free of engine and ImGui dependencies so it
// can be exercised offline: give it file bytes, get fonts and bitmaps back.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swf
{
	// Outline segment in font units. Y grows downward, the baseline is y = 0.
	// Segments are oriented so that a non-zero winding fill paints the inside.
	struct Edge
	{
		float x0, y0, x1, y1;
	};

	struct Glyph
	{
		std::vector<Edge> edges;
		float             advance{ 0.0f };
		float             minX{ 0.0f }, minY{ 0.0f }, maxX{ 0.0f }, maxY{ 0.0f };

		[[nodiscard]] bool empty() const noexcept { return edges.empty(); }
	};

	struct Font
	{
		std::string name;
		bool        bold{ false };
		bool        italic{ false };
		float       unitsPerEm{ 1024.0f };
		float       ascent{ 0.0f };   // font units above the baseline
		float       descent{ 0.0f };  // font units below the baseline
		bool        hasLayout{ false };

		std::vector<Glyph>                              glyphs;
		std::unordered_map<std::uint16_t, std::size_t> codeToGlyph;  // UCS-2 code point -> index into glyphs
	};

	// Inflate a zlib stream (2-byte header + deflate blocks). Returns false on
	// malformed input; a_out then holds whatever was decoded before the error.
	bool Inflate(const std::uint8_t* a_data, std::size_t a_size, std::vector<std::uint8_t>& a_out);

	// Parse every DefineFont2/3 tag of an FWS or CWS file. Fonts without
	// glyphs (imports) are skipped. ZWS (LZMA) files are reported as an error.
	bool ParseFonts(const std::uint8_t* a_data, std::size_t a_size, std::vector<Font>& a_out, std::string& a_error);

	struct FontConfig
	{
		struct Mapping
		{
			std::string fontName;
			std::string style;  // Normal, Bold, Italic, BoldItalic, or whatever the file says
		};

		std::vector<std::string>                 fontLibs;  // as written, e.g. Interface\fonts_en.swf
		std::unordered_map<std::string, Mapping> mappings;  // "$EverywhereFont" -> font
	};

	FontConfig ParseFontConfig(std::string_view a_text);

	// Style flags a mapping's style word asks for.
	void StyleFlags(std::string_view a_style, bool& a_bold, bool& a_italic);

	// Anti-aliased coverage bitmap of one glyph at the given scale. left/top
	// are the pixel offsets of the bitmap's corner from the pen position on
	// the baseline (y down). Empty glyphs return a 0x0 bitmap.
	struct Bitmap
	{
		int                       width{ 0 };
		int                       height{ 0 };
		int                       left{ 0 };
		int                       top{ 0 };
		std::vector<std::uint8_t> coverage;  // width * height, row-major, 0..255
	};

	Bitmap Rasterize(const Glyph& a_glyph, float a_pixelsPerUnit);
}
