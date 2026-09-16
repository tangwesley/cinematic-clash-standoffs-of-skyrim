#include "IniFile.h"

#include <fstream>

namespace
{
	constexpr std::string_view kBom = "\xEF\xBB\xBF";

	std::string_view TrimView(std::string_view a_text)
	{
		const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
		while (!a_text.empty() && space(a_text.front())) {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && space(a_text.back())) {
			a_text.remove_suffix(1);
		}
		return a_text;
	}

	bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		return a_lhs.size() == a_rhs.size() &&
		       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](unsigned char l, unsigned char r) {
				   return std::tolower(l) == std::tolower(r);
			   });
	}

	// "[Name]" -> "Name"
	std::optional<std::string_view> HeaderName(std::string_view a_line)
	{
		const auto text = TrimView(a_line);
		if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
			return TrimView(text.substr(1, text.size() - 2));
		}
		return std::nullopt;
	}

	// "key = value" -> "key"; comments, headers and blank lines give nothing.
	std::optional<std::string_view> KeyName(std::string_view a_line)
	{
		const auto text = TrimView(a_line);
		if (text.empty() || text.front() == ';' || text.front() == '#' || text.front() == '[') {
			return std::nullopt;
		}
		const auto eq = text.find('=');
		if (eq == std::string_view::npos) {
			return std::nullopt;
		}
		const auto key = TrimView(text.substr(0, eq));
		return key.empty() ? std::nullopt : std::optional{ key };
	}

	bool IsComment(std::string_view a_line)
	{
		const auto text = TrimView(a_line);
		return !text.empty() && (text.front() == ';' || text.front() == '#');
	}

	bool IsBlank(std::string_view a_line)
	{
		return TrimView(a_line).empty();
	}
}

bool IniFile::Load(const std::filesystem::path& a_path)
{
	_lines.clear();
	_crlf = true;
	_bom = false;

	std::ifstream in(a_path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	if (content.starts_with(kBom)) {
		_bom = true;
		content.erase(0, kBom.size());
	}
	_crlf = content.find('\n') == std::string::npos || content.find("\r\n") != std::string::npos;

	std::size_t start = 0;
	while (start < content.size()) {
		auto end = content.find('\n', start);
		if (end == std::string::npos) {
			end = content.size();
		}
		auto line = content.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		_lines.push_back(std::move(line));
		start = end + 1;
	}
	return true;
}

bool IniFile::Save(const std::filesystem::path& a_path) const
{
	std::error_code ec;
	std::filesystem::create_directories(a_path.parent_path(), ec);

	std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
	if (!out) {
		return false;
	}
	if (_bom) {
		out << kBom;
	}
	const char* eol = _crlf ? "\r\n" : "\n";
	for (const auto& line : _lines) {
		out << line << eol;
	}
	return static_cast<bool>(out);
}

std::size_t IniFile::FindSection(std::string_view a_section) const
{
	for (std::size_t i = 0; i < _lines.size(); ++i) {
		if (const auto name = HeaderName(_lines[i]); name && IEquals(*name, a_section)) {
			return i;
		}
	}
	return npos;
}

std::size_t IniFile::SectionEnd(std::size_t a_header) const
{
	for (std::size_t i = a_header + 1; i < _lines.size(); ++i) {
		if (HeaderName(_lines[i])) {
			return i;
		}
	}
	return _lines.size();
}

void IniFile::Set(std::string_view a_section, std::string_view a_key, std::string_view a_value)
{
	std::string line = a_value.empty() ? std::format("{} =", a_key) : std::format("{} = {}", a_key, a_value);

	const auto header = FindSection(a_section);
	if (header == npos) {
		if (!_lines.empty() && !IsBlank(_lines.back())) {
			_lines.emplace_back();
		}
		_lines.push_back(std::format("[{}]", a_section));
		_lines.push_back(std::move(line));
		return;
	}

	const auto end = SectionEnd(header);
	for (std::size_t i = header + 1; i < end; ++i) {
		if (const auto key = KeyName(_lines[i]); key && IEquals(*key, a_key)) {
			_lines[i] = std::move(line);
			return;
		}
	}

	// New key: goes at the end of the section, above the blank lines that
	// separate it from the next header.
	auto insertAt = end;
	while (insertAt > header + 1 && IsBlank(_lines[insertAt - 1])) {
		--insertAt;
	}
	_lines.insert(_lines.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(line));
}

bool IniFile::RemoveSection(std::string_view a_section)
{
	const auto header = FindSection(a_section);
	if (header == npos) {
		return false;
	}
	auto first = header;
	while (first > 0 && IsComment(_lines[first - 1])) {
		--first;
	}
	auto last = SectionEnd(header);
	// Take the blank lines that trailed the section so no double gap is left.
	while (last > header + 1 && IsBlank(_lines[last - 1])) {
		--last;
	}
	while (last < _lines.size() && IsBlank(_lines[last])) {
		++last;
	}
	_lines.erase(_lines.begin() + static_cast<std::ptrdiff_t>(first), _lines.begin() + static_cast<std::ptrdiff_t>(last));
	if (first < _lines.size() && first > 0 && !IsBlank(_lines[first - 1]) && HeaderName(_lines[first])) {
		_lines.insert(_lines.begin() + static_cast<std::ptrdiff_t>(first), std::string{});
	}
	return true;
}

bool IniFile::HasSection(std::string_view a_section) const
{
	return FindSection(a_section) != npos;
}

std::vector<std::string> IniFile::Sections() const
{
	std::vector<std::string> names;
	for (const auto& line : _lines) {
		if (const auto name = HeaderName(line)) {
			names.emplace_back(*name);
		}
	}
	return names;
}
