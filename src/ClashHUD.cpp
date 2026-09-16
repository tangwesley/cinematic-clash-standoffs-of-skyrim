#include "ClashHUD.h"

#include "FileAudio.h"
#include "Settings.h"
#include "SwfFont.h"

#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>

#include <directxtk/WICTextureLoader.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <cctype>
#include <chrono>

namespace
{
	constexpr std::size_t kPresentSlot = 8;  // IDXGISwapChain::Present

	// File names of the skin, indexed by ClashHUD::Tex.
	constexpr std::array<const char*, 5> kSkinFiles{
		"frame.png",
		"fill_player.png",
		"fill_opponent.png",
		"marker.png",
		"timer.png",
	};

	constexpr std::uint32_t kUnmappedKey = 0xFF;  // ControlMap::GetMappedKey for an unbound event

	// Upper-case, spaces to underscores: the form a label takes in a PNG name.
	[[nodiscard]] std::string LabelKey(std::string_view a_label)
	{
		std::string key;
		key.reserve(a_label.size());
		for (const char c : a_label) {
			key.push_back(c == ' ' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}
		return key;
	}

	// Name of a keyboard key from its DirectInput scan code, via the current
	// keyboard layout. Codes 0x80 and up are the E0-prefixed (extended) keys.
	[[nodiscard]] std::string KeyboardKeyName(std::uint32_t a_code)
	{
		const bool     extended = a_code >= 0x80;
		const std::uint32_t scan = a_code & 0x7F;
		const LONG     param = static_cast<LONG>((scan << 16) | (extended ? (1u << 24) : 0u));
		wchar_t        buffer[64]{};
		if (GetKeyNameTextW(param, buffer, static_cast<int>(std::size(buffer))) > 0) {
			const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
			if (size > 1) {
				std::string name(static_cast<std::size_t>(size - 1), '\0');
				WideCharToMultiByte(CP_UTF8, 0, buffer, -1, name.data(), size, nullptr, nullptr);
				for (auto& c : name) {
					c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				}
				return name;
			}
		}
		return std::format("KEY {:02X}", a_code);
	}

	// The game hands out names in the system code page; ImGui wants UTF-8.
	[[nodiscard]] std::string AnsiToUtf8(std::string_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
		const int wide = MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
		if (wide <= 0) {
			return std::string{ a_text };
		}
		std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
		MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), buffer.data(), wide);
		const int narrow = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
		if (narrow <= 0) {
			return std::string{ a_text };
		}
		std::string result(static_cast<std::size_t>(narrow), '\0');
		WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, result.data(), narrow, nullptr, nullptr);
		return result;
	}

	// Whole file through the game's resource system, so loose files and BSA
	// archives are both found, with the same override order the game uses.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadGameFile(const char* a_path)
	{
		RE::BSResourceNiBinaryStream stream(a_path);
		if (!stream.good()) {
			return std::nullopt;
		}

		RE::NiBinaryStream::BufferInfo info{};
		stream.get_info(info);

		std::vector<std::uint8_t> data;
		std::uint8_t              chunk[64 * 1024];
		if (info.totalSize > 0 && info.totalSize < 256u * 1024u * 1024u) {
			data.reserve(info.totalSize);
			std::uint32_t remaining = info.totalSize;
			while (remaining > 0) {
				const std::uint32_t want = std::min<std::uint32_t>(remaining, sizeof(chunk));
				if (!stream.read(chunk, want)) {
					break;
				}
				data.insert(data.end(), chunk, chunk + want);
				remaining -= want;
			}
		} else {
			// Size unknown: read until a short read, sized by the stream position.
			for (;;) {
				const std::uint32_t before = stream.tell();
				const bool          full = stream.read(chunk, static_cast<std::uint32_t>(sizeof(chunk)));
				const std::uint32_t after = stream.tell();
				std::uint32_t       got = full ? static_cast<std::uint32_t>(sizeof(chunk)) : (after > before ? after - before : 0u);
				got = std::min<std::uint32_t>(got, sizeof(chunk));
				data.insert(data.end(), chunk, chunk + got);
				if (!full) {
					break;
				}
			}
		}
		if (data.empty()) {
			return std::nullopt;
		}
		return data;
	}

	[[nodiscard]] bool EqualsNoCase(std::string_view a_lhs, std::string_view a_rhs)
	{
		return a_lhs.size() == a_rhs.size() &&
		       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](char a, char b) {
				   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			   });
	}

	[[nodiscard]] double NowSeconds()
	{
		using clock = std::chrono::steady_clock;
		return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
	}

	[[nodiscard]] ImU32 WithAlpha(ImU32 a_color, float a_alpha)
	{
		const auto a = static_cast<ImU32>(std::clamp(a_alpha, 0.0f, 1.0f) * ((a_color >> IM_COL32_A_SHIFT) & 0xFF));
		return (a_color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}

	void ShadowedText(ImDrawList* a_list, ImFont* a_font, float a_size, ImVec2 a_pos, ImU32 a_color, float a_alpha, const char* a_text)
	{
		a_list->AddText(a_font, a_size, ImVec2(a_pos.x + 1.5f, a_pos.y + 1.5f), WithAlpha(IM_COL32(0, 0, 0, 220), a_alpha), a_text);
		a_list->AddText(a_font, a_size, a_pos, WithAlpha(a_color, a_alpha), a_text);
	}

	[[nodiscard]] float TextWidth(ImFont* a_font, float a_size, const char* a_text)
	{
		return a_font->CalcTextSizeA(a_size, FLT_MAX, 0.0f, a_text).x;
	}

	[[nodiscard]] std::string ModuleNameOf(std::uintptr_t a_address)
	{
		HMODULE module = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_address), &module) && module) {
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(module, path, MAX_PATH)) {
				const std::filesystem::path p{ path };
				return p.filename().string();
			}
		}
		return "unknown module";
	}

	// Size of a 2D texture resource, or 0x0.
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> ResourceSize(ID3D11Resource* a_resource)
	{
		std::pair<std::uint32_t, std::uint32_t> size{ 0, 0 };
		if (!a_resource) {
			return size;
		}
		ID3D11Texture2D* texture = nullptr;
		if (SUCCEEDED(a_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture) {
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			size = { desc.Width, desc.Height };
			texture->Release();
		}
		return size;
	}

	// Size of the texture behind a render target view, or 0x0.
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> TargetSize(ID3D11RenderTargetView* a_view)
	{
		if (!a_view) {
			return { 0, 0 };
		}
		ID3D11Resource* resource = nullptr;
		a_view->GetResource(&resource);
		const auto size = ResourceSize(resource);
		if (resource) {
			resource->Release();
		}
		return size;
	}

	// Rectangle of the given size centred on a point.
	[[nodiscard]] std::pair<ImVec2, ImVec2> Centred(ImVec2 a_centre, ImVec2 a_size)
	{
		return {
			ImVec2(a_centre.x - a_size.x * 0.5f, a_centre.y - a_size.y * 0.5f),
			ImVec2(a_centre.x + a_size.x * 0.5f, a_centre.y + a_size.y * 0.5f)
		};
	}
}

ClashHUD* ClashHUD::GetSingleton()
{
	static ClashHUD singleton;
	return std::addressof(singleton);
}

void ClashHUD::Install()
{
	if (_installed) {
		return;
	}

	const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	if (!renderer) {
		logger::warn("HUD: renderer singleton unavailable; meter disabled");
		return;
	}

	auto&      data = renderer->GetRuntimeData();
	const auto swapChain = data.renderWindows[0].swapChain;
	if (!swapChain || !data.forwarder || !data.context) {
		logger::warn("HUD: swap chain or device not ready; meter disabled");
		return;
	}

	_swapChain = swapChain;
	_device = data.forwarder;
	_context = data.context;
	_hwnd = data.renderWindows[0].hWnd;

	const auto vtable = *reinterpret_cast<std::uintptr_t**>(swapChain);
	REL::Relocation<std::uintptr_t> table{ reinterpret_cast<std::uintptr_t>(vtable) };
	PresentHook::func = table.write_vfunc(kPresentSlot, PresentHook::thunk);

	_installed = true;
	logger::info("HUD: hooked Present on the game's swap chain (next in chain: {})", ModuleNameOf(PresentHook::func.address()));
}

void ClashHUD::SetNames(std::string_view a_player, std::string_view a_opponent)
{
	auto player = AnsiToUtf8(a_player);
	auto opponent = AnsiToUtf8(a_opponent);

	const std::scoped_lock lock(_textMutex);
	_playerName = std::move(player);
	_opponentName = std::move(opponent);
}

void ClashHUD::RefreshAttackGlyph()
{
	using Style = KeyGlyph::Style;
	using Pad = RE::BSWin32GamepadDevice::Key;
	using Mouse = RE::BSWin32MouseDevice::Key;

	KeyGlyph glyph;

	const auto controlMap = RE::ControlMap::GetSingleton();
	const auto userEvents = RE::UserEvents::GetSingleton();
	const auto devices = RE::BSInputDeviceManager::GetSingleton();
	if (controlMap && userEvents) {
		const std::string_view eventName{ userEvents->rightAttack.c_str() };

		// The pad wins while the game treats it as the active device (the same
		// flag that switches its own menus to button art).
		if (devices && devices->IsGamepadEnabled()) {
			switch (controlMap->GetMappedKey(eventName, RE::INPUT_DEVICE::kGamepad)) {
			case Pad::kA:
				glyph = { Style::kPadFace, "A", IM_COL32(96, 192, 80, 255) };
				break;
			case Pad::kB:
				glyph = { Style::kPadFace, "B", IM_COL32(226, 70, 60, 255) };
				break;
			case Pad::kX:
				glyph = { Style::kPadFace, "X", IM_COL32(70, 130, 230, 255) };
				break;
			case Pad::kY:
				glyph = { Style::kPadFace, "Y", IM_COL32(236, 190, 72, 255) };
				break;
			case Pad::kLeftShoulder:
				glyph = { Style::kPadBumper, "LB" };
				break;
			case Pad::kRightShoulder:
				glyph = { Style::kPadBumper, "RB" };
				break;
			case Pad::kLeftTrigger:
				glyph = { Style::kPadTrigger, "LT" };
				break;
			case Pad::kRightTrigger:
				glyph = { Style::kPadTrigger, "RT" };
				break;
			case Pad::kLeftThumb:
				glyph = { Style::kPadStick, "LS" };
				break;
			case Pad::kRightThumb:
				glyph = { Style::kPadStick, "RS" };
				break;
			case Pad::kUp:
				glyph = { Style::kPadDpad, "DPAD UP", 0, 0 };
				break;
			case Pad::kDown:
				glyph = { Style::kPadDpad, "DPAD DOWN", 0, 1 };
				break;
			case Pad::kLeft:
				glyph = { Style::kPadDpad, "DPAD LEFT", 0, 2 };
				break;
			case Pad::kRight:
				glyph = { Style::kPadDpad, "DPAD RIGHT", 0, 3 };
				break;
			case Pad::kStart:
				glyph = { Style::kPadSmall, "START" };
				break;
			case Pad::kBack:
				glyph = { Style::kPadSmall, "BACK" };
				break;
			default:
				break;
			}
		}

		if (glyph.style == Style::kNone) {
			const auto mouse = controlMap->GetMappedKey(eventName, RE::INPUT_DEVICE::kMouse);
			switch (mouse) {
			case kUnmappedKey:
				break;
			case Mouse::kLeftButton:
				glyph = { Style::kMouse, "LMB", 0, 0 };
				break;
			case Mouse::kRightButton:
				glyph = { Style::kMouse, "RMB", 0, 1 };
				break;
			case Mouse::kMiddleButton:
				glyph = { Style::kMouse, "MMB", 0, 2 };
				break;
			case Mouse::kWheelUp:
				glyph = { Style::kMouse, "WHEEL UP", 0, -1 };
				break;
			case Mouse::kWheelDown:
				glyph = { Style::kMouse, "WHEEL DOWN", 0, -1 };
				break;
			default:
				glyph = { Style::kMouse, std::format("M{}", mouse + 1), 0, -1 };
				break;
			}
		}

		if (glyph.style == Style::kNone) {
			const auto key = controlMap->GetMappedKey(eventName, RE::INPUT_DEVICE::kKeyboard);
			if (key != kUnmappedKey) {
				glyph = { Style::kKey, KeyboardKeyName(key) };
			}
		}
	}

	logger::debug("HUD: attack glyph style {} label \"{}\"", static_cast<int>(glyph.style), glyph.label);

	const std::scoped_lock lock(_textMutex);
	_glyph = std::move(glyph);
}

void ClashHUD::SetState(bool a_visible, float a_meter, float a_timeFraction, bool a_standoff)
{
	_meter.store(std::clamp(a_meter, 0.0f, 1.0f), std::memory_order_relaxed);
	_timeFraction.store(std::clamp(a_timeFraction, 0.0f, 1.0f), std::memory_order_relaxed);
	_standoff.store(a_standoff, std::memory_order_relaxed);
	_visible.store(a_visible, std::memory_order_release);
}

void ClashHUD::LoadFont()
{
	const auto settings = Settings::GetSingleton();
	auto&      io = ImGui::GetIO();
	io.Fonts->Clear();

	// Bake at the size the meter draws at so a TrueType font stays crisp; the
	// draw code still scales by the HUD scale, so this only matters for quality.
	const float bakeSize = std::max(8.0f, std::round(_fontSize * settings->hudScale));

	if (!settings->hudFontFile.empty()) {
		const auto      path = std::filesystem::path("Data") / settings->hudFontFile;
		std::error_code ec;
		if (std::filesystem::is_regular_file(path, ec)) {
			if (io.Fonts->AddFontFromFileTTF(path.string().c_str(), bakeSize)) {
				logger::info("HUD: font {} ({}px)", path.string(), bakeSize);
				return;
			}
			logger::warn("HUD: could not load font {}; using the game's font", path.string());
		} else {
			logger::warn("HUD: font {} not found; using the game's font", path.string());
		}
	}

	if (LoadGameFont(bakeSize)) {
		return;
	}

	io.Fonts->Clear();
	ImFontConfig fontConfig;
	fontConfig.SizePixels = _fontSize;
	io.Fonts->AddFontDefault(&fontConfig);
	logger::info("HUD: using the built-in font");
}

bool ClashHUD::LoadGameFont(float a_bakeSize)
{
	const auto settings = Settings::GetSingleton();
	auto&      io = ImGui::GetIO();

	const auto configBytes = ReadGameFile("Interface\\fontconfig.txt");
	if (!configBytes) {
		logger::warn("HUD: Interface/fontconfig.txt not found");
		return false;
	}
	const auto config = swf::ParseFontConfig(std::string_view(reinterpret_cast<const char*>(configBytes->data()), configBytes->size()));
	const auto mapping = config.mappings.find(settings->hudGameFont);
	if (mapping == config.mappings.end()) {
		logger::warn("HUD: fontconfig.txt has no mapping for {}", settings->hudGameFont);
		return false;
	}
	bool wantBold = false;
	bool wantItalic = false;
	swf::StyleFlags(mapping->second.style, wantBold, wantItalic);

	// Best match across every fontlib: name first, then style.
	std::optional<swf::Font> best;
	std::string              bestLib;
	int                      bestScore = -1;
	for (const auto& lib : config.fontLibs) {
		const auto bytes = ReadGameFile(lib.c_str());
		if (!bytes) {
			logger::debug("HUD: fontlib {} not found", lib);
			continue;
		}
		std::vector<swf::Font> fonts;
		std::string            error;
		if (!swf::ParseFonts(bytes->data(), bytes->size(), fonts, error)) {
			logger::warn("HUD: could not read fontlib {}: {}", lib, error);
			continue;
		}
		for (auto& font : fonts) {
			if (!EqualsNoCase(font.name, mapping->second.fontName)) {
				continue;
			}
			const int score = 1 + (font.bold == wantBold ? 1 : 0) + (font.italic == wantItalic ? 1 : 0);
			if (score > bestScore) {
				bestScore = score;
				best = std::move(font);
				bestLib = lib;
			}
		}
	}
	if (!best) {
		logger::warn("HUD: font \"{}\" ({} in fontconfig.txt) not found in any fontlib", mapping->second.fontName, settings->hudGameFont);
		return false;
	}

	// Bake the outlines into the atlas as custom glyphs of a host font. Glyph
	// offsets are measured from the top of the line box, with the baseline at
	// the font's ascent.
	ImFontConfig hostConfig;
	hostConfig.SizePixels = a_bakeSize;
	ImFont* font = io.Fonts->AddFontDefault(&hostConfig);

	const float scale = a_bakeSize / best->unitsPerEm;
	const float ascentPx = std::round(best->ascent * scale);

	std::vector<std::pair<std::uint16_t, std::size_t>> codes(best->codeToGlyph.begin(), best->codeToGlyph.end());
	std::sort(codes.begin(), codes.end());

	struct Baked
	{
		int         rect;
		swf::Bitmap bitmap;
	};
	std::vector<Baked>                       baked;
	std::vector<std::pair<ImWchar, float>>   empties;
	constexpr std::size_t                    kMaxGlyphs = 4096;
	for (const auto& [code, index] : codes) {
		if (code < 0x20 || code >= 0xFFF0 || baked.size() + empties.size() >= kMaxGlyphs) {
			continue;
		}
		const auto& glyph = best->glyphs[index];
		const float advance = glyph.advance * scale;
		auto        bitmap = swf::Rasterize(glyph, scale);
		if (bitmap.width == 0 || bitmap.height == 0) {
			empties.emplace_back(static_cast<ImWchar>(code), advance);
			continue;
		}
		const int rect = io.Fonts->AddCustomRectFontGlyph(font, static_cast<ImWchar>(code), bitmap.width, bitmap.height, advance,
			ImVec2(static_cast<float>(bitmap.left), ascentPx + static_cast<float>(bitmap.top)));
		baked.push_back({ rect, std::move(bitmap) });
	}

	if (!io.Fonts->Build() || !io.Fonts->TexPixelsAlpha8) {
		logger::error("HUD: font atlas build failed for \"{}\"", best->name);
		return false;
	}

	for (const auto& b : baked) {
		const auto* rect = io.Fonts->GetCustomRectByIndex(b.rect);
		if (!rect || !rect->IsPacked()) {
			continue;
		}
		for (int y = 0; y < b.bitmap.height; ++y) {
			auto* dst = io.Fonts->TexPixelsAlpha8 + (static_cast<std::size_t>(rect->Y) + y) * io.Fonts->TexWidth + rect->X;
			std::memcpy(dst, b.bitmap.coverage.data() + static_cast<std::size_t>(y) * b.bitmap.width, static_cast<std::size_t>(b.bitmap.width));
		}
	}
	const auto white = io.Fonts->TexUvWhitePixel;
	for (const auto& [code, advance] : empties) {
		font->AddGlyph(nullptr, code, 0.0f, 0.0f, 0.0f, 0.0f, white.x, white.y, white.x, white.y, advance);
	}
	font->BuildLookupTable();
	font->Ascent = ascentPx;
	font->Descent = -std::round(best->descent * scale);

	logger::info("HUD: font \"{}\" from {} ({} glyphs, {}px, {} = {} {})", best->name, bestLib, baked.size() + empties.size(), a_bakeSize,
		settings->hudGameFont, mapping->second.fontName, mapping->second.style);
	return true;
}

void ClashHUD::ReleaseTextures()
{
	for (auto& texture : _textures) {
		if (texture.view) {
			static_cast<ID3D11ShaderResourceView*>(texture.view)->Release();
		}
		texture = Texture{};
	}
	for (auto& [label, texture] : _keyTextures) {
		if (texture.view) {
			static_cast<ID3D11ShaderResourceView*>(texture.view)->Release();
		}
	}
	_keyTextures.clear();
}

void ClashHUD::LoadTextures()
{
	ReleaseTextures();

	const auto settings = Settings::GetSingleton();
	if (settings->hudTextureFolder.empty()) {
		return;
	}
	const auto folder = std::filesystem::path("Data") / settings->hudTextureFolder;

	// The DirectXTK loader talks to WIC through COM. The render thread keeps
	// running for the life of the process, so the apartment is joined once and
	// deliberately never left (a CoUninitialize could pull the factory the
	// loader caches out from under a later reload).
	static const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	(void)coInit;

	const auto device = static_cast<ID3D11Device*>(_device);
	const auto load = [device](const std::filesystem::path& a_path) -> Texture {
		ID3D11Resource*           resource = nullptr;
		ID3D11ShaderResourceView* view = nullptr;
		const HRESULT             hr = DirectX::CreateWICTextureFromFile(device, a_path.wstring().c_str(), &resource, &view);
		if (FAILED(hr) || !view) {
			logger::warn("HUD: could not load {} (hr=0x{:08X})", a_path.string(), static_cast<std::uint32_t>(hr));
			if (view) {
				view->Release();
			}
			if (resource) {
				resource->Release();
			}
			return {};
		}
		const auto [width, height] = ResourceSize(resource);
		resource->Release();
		logger::info("HUD: loaded {} ({}x{})", a_path.filename().string(), width, height);
		return Texture{ view, static_cast<float>(width), static_cast<float>(height) };
	};

	std::size_t loaded = 0;
	for (std::size_t i = 0; i < kTexCount; ++i) {
		const auto      path = folder / kSkinFiles[i];
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			continue;
		}
		if (_textures[i] = load(path); _textures[i]) {
			++loaded;
		}
	}

	// key_<LABEL>.png: one per attack button the glyph can show.
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		const auto name = LabelKey(entry.path().filename().string());
		if (name.size() <= 8 || !name.starts_with("KEY_") || !name.ends_with(".PNG")) {
			continue;
		}
		if (auto texture = load(entry.path()); texture) {
			_keyTextures[name.substr(4, name.size() - 8)] = texture;
			++loaded;
		}
	}

	if (loaded == 0) {
		logger::info("HUD: no skin PNGs under {}; drawing the built-in meter", folder.string());
	}
}

bool ClashHUD::InitImGui()
{
	const auto swapChain = static_cast<IDXGISwapChain*>(_swapChain);

	DXGI_SWAP_CHAIN_DESC desc{};
	float                height = 1080.0f;
	if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.BufferDesc.Height > 0) {
		height = static_cast<float>(desc.BufferDesc.Height);
	}
	_fontSize = std::round(20.0f * height / 1080.0f);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.MouseDrawCursor = false;

	LoadFont();

	if (!ImGui_ImplWin32_Init(static_cast<HWND>(_hwnd))) {
		logger::error("HUD: ImGui Win32 backend failed to initialise");
		return false;
	}
	if (!ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(_device), static_cast<ID3D11DeviceContext*>(_context))) {
		logger::error("HUD: ImGui DX11 backend failed to initialise");
		return false;
	}

	LoadTextures();
	_reloadAssets.store(false, std::memory_order_release);

	_lastTime = NowSeconds();
	logger::info("HUD: ImGui initialised ({}x{} swap chain, {}px font)", desc.BufferDesc.Width, desc.BufferDesc.Height, _fontSize);
	return true;
}

void ClashHUD::OnPresent(void* a_swapChain)
{
	if (_initFailed || a_swapChain != _swapChain) {
		return;
	}
	if (!_initialized) {
		_initialized = InitImGui();
		if (!_initialized) {
			_initFailed = true;
			return;
		}
	}

	// The INI was re-read: pick up a changed skin folder, font or scale. Done
	// between frames so the font atlas can be rebuilt safely.
	if (_reloadAssets.exchange(false, std::memory_order_acq_rel)) {
		ImGui_ImplDX11_InvalidateDeviceObjects();
		LoadFont();
		ImGui_ImplDX11_CreateDeviceObjects();
		LoadTextures();
	}

	// Smooth the visibility so the meter fades rather than pops.
	const double now = NowSeconds();
	const float  dt = std::clamp(static_cast<float>(now - _lastTime), 0.0f, 0.1f);
	_lastTime = now;

	bool       wantVisible = _visible.load(std::memory_order_acquire);
	const auto ui = RE::UI::GetSingleton();
	const bool paused = ui && ui->GameIsPaused();
	if (paused || !Settings::GetSingleton()->hudEnabled) {
		wantVisible = false;
	}
	// Our own audio voices do not know about the game menu; this runs every
	// frame regardless of pause state, so it is the right place to tell them.
	FileAudio::GetSingleton()->SetPaused(paused);
	const float target = wantVisible ? 1.0f : 0.0f;
	_alpha += (target - _alpha) * std::min(1.0f, dt * 8.0f);
	if (_alpha < 0.01f) {
		_alpha = 0.0f;
		_loggedFrame = false;
		return;
	}

	const auto swapChain = static_cast<IDXGISwapChain*>(a_swapChain);
	const auto device = static_cast<ID3D11Device*>(_device);
	const auto context = static_cast<ID3D11DeviceContext*>(_context);

	// Prefer whatever the game (or ENB) has bound right now; that is the image
	// that goes on to be presented. Fall back to the back buffer.
	ID3D11RenderTargetView* boundTarget = nullptr;
	ID3D11DepthStencilView* boundDepth = nullptr;
	context->OMGetRenderTargets(1, &boundTarget, &boundDepth);

	ID3D11RenderTargetView* drawTarget = boundTarget;
	ID3D11Texture2D*        backBuffer = nullptr;
	ID3D11RenderTargetView* backBufferView = nullptr;
	bool                    usedBackBuffer = false;
	if (!drawTarget) {
		if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer) {
			if (SUCCEEDED(device->CreateRenderTargetView(backBuffer, nullptr, &backBufferView)) && backBufferView) {
				drawTarget = backBufferView;
				usedBackBuffer = true;
			}
		}
	}

	auto [width, height] = TargetSize(drawTarget);
	if (width == 0 || height == 0) {
		DXGI_SWAP_CHAIN_DESC desc{};
		if (SUCCEEDED(swapChain->GetDesc(&desc))) {
			width = desc.BufferDesc.Width;
			height = desc.BufferDesc.Height;
		}
	}

	if (drawTarget && width > 0 && height > 0) {
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
		ImGui::NewFrame();
		Draw(_alpha);
		ImGui::Render();

		// The ImGui backend saves and restores the rest of the pipeline state
		// itself; only the output merger binding is ours to put back.
		context->OMSetRenderTargets(1, &drawTarget, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		context->OMSetRenderTargets(1, &boundTarget, boundDepth);

		if (!_loggedFrame && Settings::GetSingleton()->debugLog) {
			_loggedFrame = true;
			const auto drawData = ImGui::GetDrawData();
			logger::debug("HUD: drew {} vertices into the {} ({}x{})", drawData ? drawData->TotalVtxCount : 0,
				usedBackBuffer ? "back buffer" : "bound render target", width, height);
		}
	} else if (!_loggedFrame && Settings::GetSingleton()->debugLog) {
		_loggedFrame = true;
		logger::debug("HUD: nothing to draw into (target {}, {}x{})", static_cast<void*>(drawTarget), width, height);
	}

	if (backBufferView) {
		backBufferView->Release();
	}
	if (backBuffer) {
		backBuffer->Release();
	}
	if (boundTarget) {
		boundTarget->Release();
	}
	if (boundDepth) {
		boundDepth->Release();
	}
}

void ClashHUD::Draw(float a_alpha)
{
	const auto  settings = Settings::GetSingleton();
	const auto& io = ImGui::GetIO();
	const auto  list = ImGui::GetForegroundDrawList();
	const auto  font = ImGui::GetFont();

	// Font size follows the target height so the meter reads the same at any
	// internal resolution.
	const float scale = settings->hudScale;
	const float fontSize = std::round(20.0f * io.DisplaySize.y / 1080.0f) * scale;
	const float smallFont = fontSize * 0.8f;

	const float meter = _meter.load(std::memory_order_relaxed);
	const float timeFraction = _timeFraction.load(std::memory_order_relaxed);
	const bool  standoff = _standoff.load(std::memory_order_relaxed);

	std::string player;
	std::string opponent;
	KeyGlyph    glyph;
	{
		const std::scoped_lock lock(_textMutex);
		player = _playerName.empty() ? "You" : _playerName;
		opponent = _opponentName.empty() ? "Opponent" : _opponentName;
		glyph = _glyph;
	}

	constexpr ImU32 kPlayerColor = IM_COL32(245, 240, 230, 255);
	constexpr ImU32 kOpponentColor = IM_COL32(245, 240, 230, 255);
	constexpr ImU32 kFrameColor = IM_COL32(20, 18, 14, 200);
	constexpr ImU32 kWhite = IM_COL32(245, 240, 230, 255);
	const ImU32     tint = WithAlpha(IM_COL32_WHITE, a_alpha);

	// Layout. The bar is 32% of the screen wide. A skin PNG drawn on the
	// configured grid maps that many texture pixels onto the bar width, and
	// every skin element uses the same screen-pixels-per-texel ratio, so a set
	// authored on one grid stays in proportion at any resolution.
	const float barWidth = io.DisplaySize.x * 0.32f * scale;
	const float texel = barWidth / static_cast<float>(std::max(1, settings->hudTextureGridWidth));
	const auto  texSize = [texel](const Texture& a_tex) { return ImVec2(a_tex.width * texel, a_tex.height * texel); };
	const auto  texId = [](const Texture& a_tex) { return reinterpret_cast<ImTextureID>(a_tex.view); };

	const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * settings->hudVerticalPosition);

	const auto& frameTex = Skin(Tex::kFrame);
	const auto& playerTex = Skin(Tex::kFillPlayer);
	const auto& opponentTex = Skin(Tex::kFillOpponent);
	const auto& markerTex = Skin(Tex::kMarker);
	const auto& timerTex = Skin(Tex::kTimer);

	// Inner bar: the fill texture size when there is one, else the drawn
	// default. Its width is the 0..100% range of the meter.
	ImVec2 barSize(barWidth, io.DisplaySize.y * 0.022f * scale);
	if (playerTex) {
		barSize = texSize(playerTex);
	} else if (opponentTex) {
		barSize = texSize(opponentTex);
	}
	const auto [bar0, bar1] = Centred(centre, barSize);
	const float x0 = bar0.x;
	const float y0 = bar0.y;
	const float x1 = bar1.x;
	const float y1 = bar1.y;
	const float rounding = barSize.y * 0.25f;
	const float split = x0 + barSize.x * meter;

	// frame
	float frameTop = y0 - 3.0f;
	float frameBottom = y1 + 3.0f;
	if (frameTex) {
		const auto [f0, f1] = Centred(centre, texSize(frameTex));
		list->AddImage(texId(frameTex), f0, f1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
		frameTop = f0.y;
		frameBottom = f1.y;
	} else {
		list->AddRectFilled(ImVec2(x0 - 3.0f, y0 - 3.0f), ImVec2(x1 + 3.0f, y1 + 3.0f), WithAlpha(kFrameColor, a_alpha), rounding + 2.0f);
	}

	// fills: the player share from the left, the opponent share from the
	// right. A textured fill is cropped in texture space so the artwork stays
	// put and is revealed, rather than stretched, as the meter moves.
	if (split > x0 + 0.5f) {
		if (playerTex) {
			const auto [p0, p1] = Centred(centre, texSize(playerTex));
			list->AddImage(texId(playerTex), p0, ImVec2(p0.x + (p1.x - p0.x) * meter, p1.y), ImVec2(0.0f, 0.0f), ImVec2(meter, 1.0f), tint);
		} else {
			list->AddRectFilled(ImVec2(x0, y0), ImVec2(split, y1), WithAlpha(kPlayerColor, a_alpha), rounding, ImDrawFlags_RoundCornersLeft);
		}
	}
	if (split < x1 - 0.5f) {
		if (opponentTex) {
			const auto [o0, o1] = Centred(centre, texSize(opponentTex));
			list->AddImage(texId(opponentTex), ImVec2(o0.x + (o1.x - o0.x) * meter, o0.y), o1, ImVec2(meter, 0.0f), ImVec2(1.0f, 1.0f), tint);
		} else {
			list->AddRectFilled(ImVec2(split, y0), ImVec2(x1, y1), WithAlpha(kOpponentColor, a_alpha), rounding, ImDrawFlags_RoundCornersRight);
		}
	}

	// centre tick, the moving marker and the outline; a skinned frame is
	// expected to paint its own tick and edge.
	if (!frameTex) {
		list->AddLine(ImVec2(centre.x, y0 - 5.0f), ImVec2(centre.x, y1 + 5.0f), WithAlpha(IM_COL32(245, 240, 230, 150), a_alpha), 2.0f);
	}
	if (markerTex) {
		const auto [m0, m1] = Centred(ImVec2(split, centre.y), texSize(markerTex));
		list->AddImage(texId(markerTex), m0, m1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
	} else {
		list->AddRectFilled(ImVec2(split - 3.0f, y0 - 6.0f), ImVec2(split + 3.0f, y1 + 6.0f), WithAlpha(kWhite, a_alpha), 2.0f);
	}
	if (!frameTex) {
		list->AddRect(ImVec2(x0 - 1.0f, y0 - 1.0f), ImVec2(x1 + 1.0f, y1 + 1.0f), WithAlpha(IM_COL32(0, 0, 0, 255), a_alpha), rounding, 0, 1.5f);
	}

	// labels under the bar
	const float labelY = frameBottom + 5.0f;
	ShadowedText(list, font, smallFont, ImVec2(x0, labelY), kWhite, a_alpha, player.c_str());
	const float nameWidth = TextWidth(font, smallFont, opponent.c_str());
	ShadowedText(list, font, smallFont, ImVec2(x1 - nameWidth, labelY), kWhite, a_alpha, opponent.c_str());

	// the attack button above the bar: pulsing like a press while the mash
	// runs, or breathing slowly under a HOLD label in hold-to-mash mode
	if (standoff && glyph.style != KeyGlyph::Style::kNone) {
		const bool  holdMode = _holdMode.load(std::memory_order_relaxed);
		const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(NowSeconds() * (holdMode ? 3.0 : 9.0)));
		const float grow = holdMode ? 0.04f : 0.12f;
		const float glyphHeight = fontSize * 2.0f * (0.94f + grow * pulse);
		DrawGlyph(glyph, centre.x, frameTop - 8.0f, glyphHeight, a_alpha * (0.7f + 0.3f * pulse), texel * (0.94f + grow * pulse));
		if (holdMode) {
			const char* hint = "HOLD";
			const float hintWidth = TextWidth(font, smallFont, hint);
			ShadowedText(list, font, smallFont, ImVec2(centre.x - hintWidth * 0.5f, frameTop - 8.0f - glyphHeight - smallFont - 4.0f), kWhite, a_alpha, hint);
		}
	}

	// time remaining, shrinking from the centre
	if (standoff) {
		const float timerY = labelY + smallFont + 6.0f;
		if (timerTex) {
			const auto  size = texSize(timerTex);
			const float half = size.x * 0.5f * timeFraction;
			if (half > 0.5f) {
				list->AddImage(texId(timerTex), ImVec2(centre.x - half, timerY), ImVec2(centre.x + half, timerY + size.y),
					ImVec2(0.5f - 0.5f * timeFraction, 0.0f), ImVec2(0.5f + 0.5f * timeFraction, 1.0f), tint);
			}
		} else {
			const float timerHalf = barSize.x * 0.5f * timeFraction;
			list->AddRectFilled(ImVec2(x0, timerY), ImVec2(x1, timerY + 4.0f), WithAlpha(IM_COL32(0, 0, 0, 160), a_alpha), 2.0f);
			if (timerHalf > 0.5f) {
				list->AddRectFilled(ImVec2(centre.x - timerHalf, timerY), ImVec2(centre.x + timerHalf, timerY + 4.0f), WithAlpha(kWhite, a_alpha), 2.0f);
			}
		}
	}
}

void ClashHUD::DrawGlyph(const KeyGlyph& a_glyph, float a_centreX, float a_bottom, float a_height, float a_alpha, float a_texel)
{
	using Style = KeyGlyph::Style;

	const auto   list = ImGui::GetForegroundDrawList();
	const auto   font = ImGui::GetFont();
	const float  h = a_height;
	const ImVec2 c(a_centreX, a_bottom - h * 0.5f);

	// A PNG named after the label replaces the drawn glyph outright.
	if (const auto it = _keyTextures.find(LabelKey(a_glyph.label)); it != _keyTextures.end()) {
		const ImVec2 size(it->second.width * a_texel, it->second.height * a_texel);
		list->AddImage(reinterpret_cast<ImTextureID>(it->second.view),
			ImVec2(a_centreX - size.x * 0.5f, a_bottom - size.y), ImVec2(a_centreX + size.x * 0.5f, a_bottom),
			ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), WithAlpha(IM_COL32_WHITE, a_alpha));
		return;
	}

	constexpr ImU32 kDark = IM_COL32(28, 26, 24, 235);
	constexpr ImU32 kMid = IM_COL32(72, 68, 64, 255);
	constexpr ImU32 kLight = IM_COL32(245, 240, 230, 255);
	constexpr ImU32 kCap = IM_COL32(232, 226, 214, 255);
	constexpr ImU32 kInk = IM_COL32(30, 28, 26, 255);
	const ImU32     accent = a_glyph.accent ? a_glyph.accent : IM_COL32(236, 190, 72, 255);
	const float     outline = std::max(1.5f, h * 0.06f);
	const char*     label = a_glyph.label.c_str();

	const auto text = [&](ImVec2 a_at, float a_size, ImU32 a_color, const char* a_text) {
		const float width = TextWidth(font, a_size, a_text);
		const auto  pos = ImVec2(a_at.x - width * 0.5f, a_at.y - a_size * 0.5f);
		// drawn twice, a hair apart, for a heavier weight than the atlas font has
		list->AddText(font, a_size, ImVec2(pos.x + 0.6f, pos.y), WithAlpha(a_color, a_alpha), a_text);
		list->AddText(font, a_size, pos, WithAlpha(a_color, a_alpha), a_text);
	};
	const auto box = [&](ImVec2 a_min, ImVec2 a_max, float a_rounding, ImU32 a_fill, ImU32 a_edge, ImDrawFlags a_flags = 0) {
		list->AddRectFilled(a_min, a_max, WithAlpha(a_fill, a_alpha), a_rounding, a_flags);
		list->AddRect(a_min, a_max, WithAlpha(a_edge, a_alpha), a_rounding, a_flags, outline);
	};

	switch (a_glyph.style) {
	case Style::kPadFace:
		list->AddCircleFilled(c, h * 0.5f, WithAlpha(kDark, a_alpha), 48);
		list->AddCircle(c, h * 0.5f - outline * 0.5f, WithAlpha(accent, a_alpha), 48, outline);
		text(c, h * 0.62f, accent, label);
		break;

	case Style::kPadBumper: {
		const float w = h * 1.7f;
		const float hh = h * 0.62f;
		box(ImVec2(c.x - w * 0.5f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f, c.y + hh * 0.5f), hh * 0.4f, kDark, kLight);
		text(c, h * 0.42f, kLight, label);
		break;
	}

	case Style::kPadTrigger: {
		const float w = h * 1.05f;
		box(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), h * 0.3f, kDark, kLight, ImDrawFlags_RoundCornersTop);
		text(c, h * 0.42f, kLight, label);
		break;
	}

	case Style::kPadStick:
		list->AddCircleFilled(c, h * 0.5f, WithAlpha(kDark, a_alpha), 48);
		list->AddCircle(c, h * 0.5f - outline * 0.5f, WithAlpha(kLight, a_alpha), 48, outline);
		list->AddCircleFilled(c, h * 0.32f, WithAlpha(kMid, a_alpha), 48);
		text(c, h * 0.36f, kLight, label);
		break;

	case Style::kPadDpad: {
		const float t = h * 0.34f;
		const float r = h * 0.5f;
		list->AddRectFilled(ImVec2(c.x - t * 0.5f, c.y - r), ImVec2(c.x + t * 0.5f, c.y + r), WithAlpha(kDark, a_alpha), t * 0.2f);
		list->AddRectFilled(ImVec2(c.x - r, c.y - t * 0.5f), ImVec2(c.x + r, c.y + t * 0.5f), WithAlpha(kDark, a_alpha), t * 0.2f);
		ImVec2 arm0;
		ImVec2 arm1;
		switch (a_glyph.variant) {
		case 0:
			arm0 = ImVec2(c.x - t * 0.5f, c.y - r);
			arm1 = ImVec2(c.x + t * 0.5f, c.y - t * 0.5f);
			break;
		case 1:
			arm0 = ImVec2(c.x - t * 0.5f, c.y + t * 0.5f);
			arm1 = ImVec2(c.x + t * 0.5f, c.y + r);
			break;
		case 2:
			arm0 = ImVec2(c.x - r, c.y - t * 0.5f);
			arm1 = ImVec2(c.x - t * 0.5f, c.y + t * 0.5f);
			break;
		default:
			arm0 = ImVec2(c.x + t * 0.5f, c.y - t * 0.5f);
			arm1 = ImVec2(c.x + r, c.y + t * 0.5f);
			break;
		}
		list->AddRectFilled(ImVec2(arm0.x + outline, arm0.y + outline), ImVec2(arm1.x - outline, arm1.y - outline), WithAlpha(accent, a_alpha), t * 0.15f);
		break;
	}

	case Style::kPadSmall: {
		const float size = h * 0.34f;
		const float w = std::max(h * 1.6f, TextWidth(font, size, label) + h * 0.6f);
		const float hh = h * 0.62f;
		box(ImVec2(c.x - w * 0.5f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f, c.y + hh * 0.5f), hh * 0.5f, kDark, kLight);
		text(c, size, kLight, label);
		break;
	}

	case Style::kKey: {
		const float size = h * 0.5f;
		const float w = std::max(h, TextWidth(font, size, label) + h * 0.5f);
		const auto  p0 = ImVec2(c.x - w * 0.5f, c.y - h * 0.5f);
		const auto  p1 = ImVec2(c.x + w * 0.5f, c.y + h * 0.5f);
		// the cap sits on a darker base so it reads as a physical key
		list->AddRectFilled(ImVec2(p0.x, p0.y + outline * 1.5f), ImVec2(p1.x, p1.y + outline * 1.5f), WithAlpha(kInk, a_alpha), h * 0.18f);
		box(p0, p1, h * 0.18f, kCap, kInk);
		text(c, size, kInk, label);
		break;
	}

	case Style::kMouse: {
		const float w = h * 0.68f;
		const auto  p0 = ImVec2(c.x - w * 0.5f, c.y - h * 0.5f);
		const auto  p1 = ImVec2(c.x + w * 0.5f, c.y + h * 0.5f);
		const float splitY = p0.y + h * 0.42f;
		box(p0, p1, w * 0.42f, kDark, kLight);
		list->AddLine(ImVec2(p0.x, splitY), ImVec2(p1.x, splitY), WithAlpha(kLight, a_alpha), outline * 0.7f);
		list->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, splitY), WithAlpha(kLight, a_alpha), outline * 0.7f);
		const float inset = outline * 1.2f;
		switch (a_glyph.variant) {
		case 0:
			list->AddRectFilled(ImVec2(p0.x + inset, p0.y + inset), ImVec2(c.x - inset * 0.5f, splitY - inset * 0.5f), WithAlpha(accent, a_alpha), w * 0.36f, ImDrawFlags_RoundCornersTopLeft);
			break;
		case 1:
			list->AddRectFilled(ImVec2(c.x + inset * 0.5f, p0.y + inset), ImVec2(p1.x - inset, splitY - inset * 0.5f), WithAlpha(accent, a_alpha), w * 0.36f, ImDrawFlags_RoundCornersTopRight);
			break;
		case 2:
			list->AddRectFilled(ImVec2(c.x - w * 0.09f, p0.y + h * 0.1f), ImVec2(c.x + w * 0.09f, p0.y + h * 0.32f), WithAlpha(accent, a_alpha), w * 0.09f);
			break;
		default:
			// wheel or an extra button: a plain wheel plus the label in the body
			list->AddRectFilled(ImVec2(c.x - w * 0.09f, p0.y + h * 0.1f), ImVec2(c.x + w * 0.09f, p0.y + h * 0.32f), WithAlpha(kMid, a_alpha), w * 0.09f);
			if (a_glyph.label.starts_with("WHEEL")) {
				const bool  up = a_glyph.label.ends_with("UP");
				const float ay = c.y + h * 0.22f;
				const float ah = h * 0.16f;
				list->AddTriangleFilled(
					ImVec2(c.x - ah, up ? ay + ah * 0.5f : ay - ah * 0.5f),
					ImVec2(c.x + ah, up ? ay + ah * 0.5f : ay - ah * 0.5f),
					ImVec2(c.x, up ? ay - ah * 0.5f : ay + ah * 0.5f),
					WithAlpha(accent, a_alpha));
			} else {
				text(ImVec2(c.x, c.y + h * 0.22f), h * 0.26f, accent, label);
			}
			break;
		}
		break;
	}

	case Style::kNone:
	default:
		break;
	}
}
