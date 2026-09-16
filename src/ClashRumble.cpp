#include "ClashRumble.h"

#include "Settings.h"

namespace
{
	struct XInputVibration
	{
		std::uint16_t leftMotor;   // large, low-frequency
		std::uint16_t rightMotor;  // small, high-frequency
	};

	// XInput keeps the last level indefinitely, but Steam Input and some
	// wrappers do not; re-sending an unchanged level this often keeps it up.
	constexpr float kRefreshInterval = 0.1f;

	[[nodiscard]] std::uint16_t ToMotor(float a_level)
	{
		return static_cast<std::uint16_t>(std::clamp(a_level, 0.0f, 1.0f) * 65535.0f + 0.5f);
	}
}

ClashRumble* ClashRumble::GetSingleton()
{
	static ClashRumble singleton;
	return std::addressof(singleton);
}

void ClashRumble::Register()
{
	if (_registered) {
		return;
	}
	const auto ui = RE::UI::GetSingleton();
	if (!ui) {
		logger::warn("Rumble: UI unavailable; the pad will keep buzzing through a pause during a clash");
		return;
	}
	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(this);
	_registered = true;
}

bool ClashRumble::LoadXInput()
{
	if (_loadTried) {
		return _setState != nullptr;
	}
	_loadTried = true;

	for (const char* name : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }) {
		const HMODULE module = LoadLibraryA(name);
		if (!module) {
			continue;
		}
		_setState = reinterpret_cast<SetStateFn>(GetProcAddress(module, "XInputSetState"));
		if (_setState) {
			logger::info("Rumble: XInputSetState from {}", name);
			return true;
		}
		FreeLibrary(module);
	}
	logger::warn("Rumble: no XInput library found; controller rumble is off");
	return false;
}

int ClashRumble::UserIndex() const
{
	const auto manager = RE::BSInputDeviceManager::GetSingleton();
	if (!manager || !manager->IsGamepadEnabled()) {
		return -1;
	}
	const auto delegate = manager->GetGamepad();
	if (!delegate) {
		return -1;
	}
	// Only the XInput device carries an XInput user index; a DualShock on
	// the engine's Orbis path has no XInput motors to drive.
	const auto xinput = skyrim_cast<RE::BSWin32GamepadDevice*>(delegate);
	if (!xinput) {
		return -1;
	}
	const auto& data = static_cast<RE::BSGamepadDevice*>(xinput)->GetRuntimeData();
	return data.connected ? data.userIndex : -1;
}

bool ClashRumble::GameAllowsRumble() const
{
	if (!Settings::GetSingleton()->rumbleRespectGameSetting) {
		return true;
	}
	if (const auto prefs = RE::INIPrefSettingCollection::GetSingleton()) {
		if (const auto setting = prefs->GetSetting("bGamePadRumble:Controls")) {
			return setting->GetBool();
		}
	}
	if (const auto ini = RE::INISettingCollection::GetSingleton()) {
		if (const auto setting = ini->GetSetting("bGamePadRumble:Controls")) {
			return setting->GetBool();
		}
	}
	return true;
}

void ClashRumble::SetBase(float a_large, float a_small)
{
	_baseLarge = std::clamp(a_large, 0.0f, 1.0f);
	_baseSmall = std::clamp(a_small, 0.0f, 1.0f);
	_gameAllows = GameAllowsRumble();
}

void ClashRumble::Pulse(float a_large, float a_small, float a_seconds)
{
	if (a_seconds <= 0.0f || (a_large <= 0.0f && a_small <= 0.0f)) {
		return;
	}
	if (_baseLarge <= 0.0f && _baseSmall <= 0.0f) {
		_gameAllows = GameAllowsRumble();  // a pulse on its own, outside a running standoff
	}
	_pulseLarge = std::clamp(a_large, 0.0f, 1.0f);
	_pulseSmall = std::clamp(a_small, 0.0f, 1.0f);
	_pulseLeft = a_seconds;
}

void ClashRumble::Update(float a_dt)
{
	if (_pulseLeft > 0.0f) {
		_pulseLeft -= a_dt;
	}

	// ("small" is a Windows macro, hence the longer names.)
	float largeLevel = _baseLarge;
	float smallLevel = _baseSmall;
	if (_pulseLeft > 0.0f) {
		largeLevel = std::max(largeLevel, _pulseLarge);
		smallLevel = std::max(smallLevel, _pulseSmall);
	}
	if (!_gameAllows) {
		largeLevel = smallLevel = 0.0f;
	} else if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
		largeLevel = smallLevel = 0.0f;
	}

	_refreshTimer += a_dt;
	const bool changed = largeLevel != _appliedLarge || smallLevel != _appliedSmall;
	if (!changed && (_refreshTimer < kRefreshInterval || (largeLevel <= 0.0f && smallLevel <= 0.0f))) {
		return;
	}
	_refreshTimer = 0.0f;
	Apply(largeLevel, smallLevel);
}

void ClashRumble::Stop()
{
	_baseLarge = 0.0f;
	_baseSmall = 0.0f;
	_pulseLeft = 0.0f;
	if (_appliedLarge > 0.0f || _appliedSmall > 0.0f) {
		Apply(0.0f, 0.0f);
	}
}

void ClashRumble::Apply(float a_large, float a_small)
{
	_appliedLarge = a_large;
	_appliedSmall = a_small;

	const int index = UserIndex();
	if (index < 0 || !LoadXInput()) {
		return;
	}
	XInputVibration vibration{ ToMotor(a_large), ToMotor(a_small) };
	_setState(static_cast<std::uint32_t>(index), &vibration);
}

RE::BSEventNotifyControl ClashRumble::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (!a_event || !a_event->opening) {
		return RE::BSEventNotifyControl::kContinue;
	}
	// The frame driver stops while the game is paused, so the pad would keep
	// whatever level was last sent; zero it now. Update puts it back once
	// frames run again.
	if (_appliedLarge > 0.0f || _appliedSmall > 0.0f) {
		if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			Apply(0.0f, 0.0f);
		}
	}
	return RE::BSEventNotifyControl::kContinue;
}
