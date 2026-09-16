#include "ClashMenu.h"

#include "ClashHUD.h"
#include "Settings.h"

// The framework header is a thin GetProcAddress layer over the framework's
// own Dear ImGui, in its own namespace, so it lives alongside the ImGui the
// HUD links (which never appears in this file).
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)

#include <cfloat>
#include <chrono>

namespace
{
	namespace UI = ImGuiMCP;
	using SD = SettingsData;
	using CF = SettingsData::CameraFraming;

	template <class... Ts>
	struct Overloaded : Ts...
	{
		using Ts::operator()...;
	};

	bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		return a_lhs.size() == a_rhs.size() &&
		       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](unsigned char l, unsigned char r) {
				   return std::tolower(l) == std::tolower(r);
			   });
	}

	// Work recorded while a page draws and run once it has finished: a save
	// re-reads Settings and rebuilds its preset vectors, which must not happen
	// while widgets still point into them.
	struct Pending
	{
		bool                                        saveMain{ false };
		bool                                        resetSection{ false };
		bool                                        reloadFromDisk{ false };
		std::optional<SD::CameraPreset>             savePreset;
		std::optional<std::string>                  removePreset;
		std::optional<std::string>                  addPreset;
		std::optional<std::pair<std::string, bool>> poolChange;
	};

	void Help(const char* a_help)
	{
		if (a_help && *a_help && UI::IsItemHovered(UI::ImGuiHoveredFlags_ForTooltip)) {
			UI::SetTooltip("%s", a_help);
		}
	}

	bool Bounded(float a_min, float a_max)
	{
		return std::isfinite(a_min) && std::isfinite(a_max);
	}

	float DragLimit(float a_limit)
	{
		return std::isfinite(a_limit) ? a_limit : std::copysign(FLT_MAX, a_limit);
	}

	// Units per pixel for open-ended values, from the size of the default.
	float DragSpeed(float a_default)
	{
		const float magnitude = std::abs(a_default);
		return magnitude >= 50.0f ? 1.0f : magnitude >= 5.0f ? 0.1f : 0.01f;
	}

	// Each widget returns true when the value should go to disk: toggles and
	// combos on the change itself; sliders, drags and text boxes when the
	// item is released or loses focus, so one drag is one write.
	bool BoolWidget(const char* a_label, bool& a_value, const char* a_help)
	{
		const bool changed = UI::Checkbox(a_label, &a_value);
		Help(a_help);
		return changed;
	}

	bool FloatWidget(const char* a_label, float& a_value, float a_min, float a_max, float a_default, const char* a_help)
	{
		if (Bounded(a_min, a_max)) {
			UI::SliderFloat(a_label, &a_value, a_min, a_max, "%.2f", UI::ImGuiSliderFlags_AlwaysClamp);
		} else {
			UI::DragFloat(a_label, &a_value, DragSpeed(a_default), DragLimit(a_min), DragLimit(a_max), "%.2f", UI::ImGuiSliderFlags_AlwaysClamp);
		}
		const bool done = UI::IsItemDeactivatedAfterEdit();
		Help(a_help);
		return done;
	}

	bool IntWidget(const char* a_label, int& a_value, int a_min, int a_max, std::span<const char* const> a_choices, const char* a_help)
	{
		bool done = false;
		if (!a_choices.empty()) {
			done = UI::Combo(a_label, &a_value, a_choices.data(), static_cast<int>(a_choices.size()));
		} else {
			UI::SliderInt(a_label, &a_value, a_min, a_max, "%d", UI::ImGuiSliderFlags_AlwaysClamp);
			done = UI::IsItemDeactivatedAfterEdit();
		}
		Help(a_help);
		return done;
	}

	bool StringWidget(const char* a_label, std::string& a_value, const char* a_help)
	{
		char       buffer[1024];
		const auto length = std::min(a_value.size(), sizeof(buffer) - 1);
		std::memcpy(buffer, a_value.data(), length);
		buffer[length] = '\0';
		if (UI::InputText(a_label, buffer, sizeof(buffer))) {
			a_value = buffer;
		}
		const bool done = UI::IsItemDeactivatedAfterEdit();
		Help(a_help);
		return done;
	}

	// The framing keys, for [Camera] and for each preset.
	bool DrawFraming(CF& a_framing)
	{
		const auto& defaults = Settings::Defaults().cameraFraming;
		bool        commit = false;
		for (const auto& entry : Settings::FramingEntries()) {
			const bool done = std::visit(Overloaded{
											 [&](bool CF::*a_member) { return BoolWidget(entry.label, a_framing.*a_member, entry.help); },
											 [&](float CF::*a_member) {
												 return FloatWidget(entry.label, a_framing.*a_member, entry.min, entry.max, defaults.*a_member, entry.help);
											 } },
				entry.member);
			if (done) {
				commit = true;
			}
		}
		return commit;
	}

	bool DrawEntry(Settings& a_settings, const Settings::Entry& a_entry)
	{
		const auto& defaults = Settings::Defaults();
		return std::visit(Overloaded{
							  [&](std::monostate) {
								  UI::SeparatorText(a_entry.label);
								  const bool done = DrawFraming(a_settings.cameraFraming);
								  UI::Separator();
								  return done;
							  },
							  [&](bool SD::*a_member) { return BoolWidget(a_entry.label, a_settings.*a_member, a_entry.help); },
							  [&](float SD::*a_member) {
								  return FloatWidget(a_entry.label, a_settings.*a_member, a_entry.min, a_entry.max, defaults.*a_member, a_entry.help);
							  },
							  [&](int SD::*a_member) {
								  return IntWidget(a_entry.label, a_settings.*a_member, static_cast<int>(a_entry.min), static_cast<int>(a_entry.max), a_entry.choices, a_entry.help);
							  },
							  [&](std::string SD::*a_member) { return StringWidget(a_entry.label, a_settings.*a_member, a_entry.help); } },
			a_entry.member);
	}

	// The presets file: every section, editable, with a tick for membership
	// in the random pool.
	void DrawPresets(Settings& a_settings, Pending& a_pending)
	{
		UI::SeparatorText("Camera presets");
		UI::TextWrapped("%s", "Each preset is a section of CinematicClash_CameraPresets.ini with its own copy of the framing keys above. "
							  "Ticked presets are in the pool (the Preset pool list): every clash draws one of them at random. "
							  "Open a preset to edit it; changes are written to the presets file.");

		for (std::size_t i = 0; i < a_settings.cameraPresetLibrary.size(); ++i) {
			auto& preset = a_settings.cameraPresetLibrary[i];
			UI::PushID(static_cast<int>(i));

			bool inPool = a_settings.IsPresetInPool(preset.name);
			if (UI::Checkbox("##pool", &inPool)) {
				a_pending.poolChange = std::pair{ preset.name, inPool };
			}
			Help("Ticked: this preset is in the random pool.");
			UI::SameLine();
			if (UI::TreeNode("preset", "%s", preset.name.c_str())) {
				if (DrawFraming(preset.framing)) {
					a_pending.savePreset = preset;
				}
				if (UI::Button("Delete preset")) {
					a_pending.removePreset = preset.name;
				}
				Help("Removes this section from the presets file and the name from the pool.");
				UI::TreePop();
			}
			UI::PopID();
		}

		static char newName[64]{};
		UI::SetNextItemWidth(UI::GetFontSize() * 12.0f);
		UI::InputTextWithHint("##newpreset", "New preset name", newName, sizeof(newName));
		UI::SameLine();
		if (UI::Button("Add preset") && newName[0] != '\0') {
			a_pending.addPreset = newName;
			newName[0] = '\0';
		}
		Help("Adds a preset that starts as a copy of the framing keys above and puts it in the pool.");
	}

	// Once a second while a page is open: pick up edits made to either file
	// from outside the game.
	void PollDisk()
	{
		using namespace std::chrono;
		static auto lastPoll = steady_clock::now();
		const auto  now = steady_clock::now();
		if (now - lastPoll < 1s) {
			return;
		}
		lastPoll = now;
		if (Settings::GetSingleton()->ReloadIfChanged()) {
			ClashHUD::GetSingleton()->ReloadAssets();
		}
	}

	void Apply(Settings& a_settings, const char* a_section, Pending& a_pending)
	{
		if (a_pending.reloadFromDisk) {
			a_settings.Load();
			ClashHUD::GetSingleton()->ReloadAssets();
		}
		if (a_pending.resetSection) {
			a_settings.ResetSection(a_section);
			a_pending.saveMain = true;
		}

		bool saved = false;
		if (a_pending.addPreset && a_settings.AddPreset(*a_pending.addPreset)) {
			saved = true;
		}
		if (a_pending.removePreset && a_settings.RemovePreset(*a_pending.removePreset)) {
			saved = true;
		}
		if (a_pending.poolChange && a_settings.SetPresetInPool(a_pending.poolChange->first, a_pending.poolChange->second)) {
			saved = true;
		}
		if (a_pending.savePreset && a_settings.SavePreset(*a_pending.savePreset)) {
			saved = true;
		}
		if (a_pending.saveMain && a_settings.Save()) {
			saved = true;
		}
		// The skin folder, font and scale are read when the HUD's assets load.
		if (saved && IEquals(a_section, "HUD")) {
			ClashHUD::GetSingleton()->ReloadAssets();
		}
	}

	void RenderSection(std::size_t a_index)
	{
		auto&       settings = *Settings::GetSingleton();
		const char* section = Settings::kSections[a_index];

		PollDisk();

		Pending pending;
		UI::PushID(section);

		UI::TextDisabled("[%s] in Data/SKSE/Plugins/CinematicClash.ini", section);
		UI::TextWrapped("%s", "Changes are written to the file as you make them, and edits saved to the file while the game runs show up here. Hover a setting for its description.");
		UI::Separator();

		for (const auto& entry : Settings::Entries()) {
			if (IEquals(entry.section, section) && DrawEntry(settings, entry)) {
				pending.saveMain = true;
			}
		}
		if (IEquals(section, "Camera")) {
			DrawPresets(settings, pending);
		}

		UI::Separator();
		if (UI::Button("Reset this section to defaults")) {
			pending.resetSection = true;
		}
		Help("Puts every key on this page back to the plugin's built-in default and writes the file. Camera presets are left alone.");
		UI::SameLine();
		if (UI::Button("Reload from disk")) {
			pending.reloadFromDisk = true;
		}
		Help("Re-reads both INI files now.");

		UI::PopID();
		Apply(settings, section, pending);
	}

	// The framework takes a plain function pointer per page and passes no
	// user data back, so each page gets its own instantiation.
	template <std::size_t N>
	void __stdcall RenderPage()
	{
		RenderSection(N);
	}

	template <std::size_t... Is>
	constexpr auto MakeRenderers(std::index_sequence<Is...>)
	{
		return std::array<SKSEMenuFramework::Model::RenderFunction, sizeof...(Is)>{ &RenderPage<Is>... };
	}

	constexpr auto kRenderers = MakeRenderers(std::make_index_sequence<Settings::kSections.size()>{});
}

void ClashMenu::Register()
{
	if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule()) {
		logger::info("SKSE Menu Framework not found; no in-game settings menu (the INI files still hot-reload)");
		return;
	}

	SKSEMenuFramework::SetSection("Cinematic Clash");
	for (std::size_t i = 0; i < Settings::kSections.size(); ++i) {
		SKSEMenuFramework::AddSectionItem(Settings::kSections[i], kRenderers[i]);
	}
	logger::info("SKSE Menu Framework {} (API {}) found; settings menu registered with {} pages",
		SKSEMenuFramework::GetMenuFrameworkVersion(), SKSEMenuFramework::GetMenuFrameworkAPIVersion(), Settings::kSections.size());
}
