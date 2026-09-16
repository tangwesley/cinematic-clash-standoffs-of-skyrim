#pragma once

// Edits an INI file in place. A value is replaced on the line it already has,
// a key the file lacks is appended to the end of its section, and a section
// the file lacks is appended to the end of the file, so the comments, blank
// lines and key order of the user's copy survive every save. This is how the
// in-game menu writes back to the shipped INIs; reading stays with SimpleIni.
class IniFile
{
public:
	// A file that cannot be read leaves the object empty; Save then creates it.
	bool Load(const std::filesystem::path& a_path);
	bool Save(const std::filesystem::path& a_path) const;

	void Set(std::string_view a_section, std::string_view a_key, std::string_view a_value);

	// Removes the header, every line up to the next header, and the comment
	// block sitting directly above the header (up to the first blank line).
	bool RemoveSection(std::string_view a_section);

	[[nodiscard]] bool                     HasSection(std::string_view a_section) const;
	[[nodiscard]] std::vector<std::string> Sections() const;

private:
	static constexpr std::size_t npos = std::string::npos;

	[[nodiscard]] std::size_t FindSection(std::string_view a_section) const;
	[[nodiscard]] std::size_t SectionEnd(std::size_t a_header) const;

	std::vector<std::string> _lines;
	bool                     _crlf{ true };
	bool                     _bom{ false };
};
