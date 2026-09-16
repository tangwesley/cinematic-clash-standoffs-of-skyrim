#pragma once

// ---------------------------------------------------------------------------
// Controller rumble for the clash.
//
// The engine's own vibration entry point (BSGamepadDevice::SetVibration) is a
// stub on the PC runtimes, so the motors are driven directly through
// XInputSetState on the user index the game's XInput device reports. XInput is
// loaded lazily on first use; without it, or without a pad, nothing happens.
// A DualShock on the engine's native (Orbis) path has no XInput motors and is
// left alone.
//
// Levels: a base level is held for the whole clash and re-applied every
// frame from the controller's frame driver, and short pulses (one per counted
// press, one for the auto-win blow) ride on top of it. The frame driver stops
// while the game is paused, so a menu opening zeroes the motors straight
// away; the next frame after it closes puts the base level back.
// ---------------------------------------------------------------------------
class ClashRumble : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	[[nodiscard]] static ClashRumble* GetSingleton();

	// Menu sink. Call at kDataLoaded, once the UI exists.
	void Register();

	// Level held on both motors (0..1) until Stop. Re-reads the game's own
	// rumble preference here, so that lookup is not done every frame.
	void SetBase(float a_large, float a_small);

	// One-off kick on top of the base level; the larger of the two wins.
	void Pulse(float a_large, float a_small, float a_seconds);

	// Drive the pad. Main thread, once per frame while a clash is running.
	void Update(float a_dt);

	// Motors off, levels cleared.
	void Stop();

	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source) override;

private:
	ClashRumble() = default;
	ClashRumble(const ClashRumble&) = delete;
	ClashRumble(ClashRumble&&) = delete;
	~ClashRumble() override = default;
	ClashRumble& operator=(const ClashRumble&) = delete;
	ClashRumble& operator=(ClashRumble&&) = delete;

	// DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
	using SetStateFn = std::uint32_t(__stdcall*)(std::uint32_t a_userIndex, void* a_vibration);

	bool               LoadXInput();
	[[nodiscard]] int  UserIndex() const;
	[[nodiscard]] bool GameAllowsRumble() const;
	void               Apply(float a_large, float a_small);

	SetStateFn _setState{ nullptr };
	bool       _loadTried{ false };
	bool       _registered{ false };
	bool       _gameAllows{ true };

	float _baseLarge{ 0.0f };
	float _baseSmall{ 0.0f };
	float _pulseLarge{ 0.0f };
	float _pulseSmall{ 0.0f };
	float _pulseLeft{ 0.0f };

	float _appliedLarge{ 0.0f };
	float _appliedSmall{ 0.0f };
	float _refreshTimer{ 0.0f };
};
