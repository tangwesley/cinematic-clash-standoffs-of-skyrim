#pragma once

// Watches raw input for the attack button while a standoff is running, and
// keeps every other button away from the game while the player is locked.
//
// Fighting controls are disabled during the clash so the player cannot start
// a real attack, but BSInputDeviceManager still dispatches every button to
// its sinks, so the mash is read here. A press counts if either the event's
// user-event name is "Right Attack/Block" or its device/key code matches the
// binding captured when the clash started.
//
// The button's held state is tracked as well, for the hold-to-mash
// accessibility mode: the controller converts held time into presses at the
// difficulty's rate. Releases are watched even after the clash ends so the
// flag can never stick.
//
// Input filter: MenuControls and PlayerControls are the two game objects that
// turn input events into actions (menus, hotkeys, quicksave, console, sprint,
// ...). Their ProcessEvent is hooked; while the player is locked and the
// gameplay input context is on top, every button, stick and mouse-move event
// other than the attack button and the Journal key (the main menu: Escape on
// the keyboard, Start on a gamepad) has its user-event name blanked for the
// duration of the call, which is how the engine itself represents a disabled
// control. Buttons that were already held when the lock began keep their
// names so the game still sees them released.
class ClashInput : public RE::BSTEventSink<RE::InputEvent*>
{
public:
	[[nodiscard]] static ClashInput* GetSingleton();

	void Register();

	// Hooks MenuControls / PlayerControls ProcessEvent (load time).
	void InstallHooks();

	// Snapshot the current attack and Journal bindings (keyboard, mouse, gamepad).
	void CaptureBindings();

	// The controller calls these when the player lock begins and ends.
	void BeginBlocking();
	void EndBlocking();

	// True while the attack button is down. Safe from any thread.
	[[nodiscard]] bool IsAttackHeld() const noexcept { return _attackHeld.load(std::memory_order_relaxed); }
	void               ResetHeld() noexcept { _attackHeld.store(false, std::memory_order_relaxed); }

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source) override;

	// One event whose user-event name was blanked, and what it was.
	struct Blanked
	{
		RE::IDEvent*      event;
		RE::BSFixedString name;
	};

	template <class Controls>
	struct ProcessHook
	{
		static RE::BSEventNotifyControl thunk(Controls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source)
		{
			auto       blanked = GetSingleton()->BlankBlockedEvents(a_event);
			const auto result = func(a_this, a_event, a_source);
			RestoreBlankedEvents(blanked);
			return result;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	ClashInput() = default;
	ClashInput(const ClashInput&) = delete;
	ClashInput(ClashInput&&) = delete;
	~ClashInput() override = default;
	ClashInput& operator=(const ClashInput&) = delete;
	ClashInput& operator=(ClashInput&&) = delete;

	static constexpr std::uint32_t kInvalidKey = 0xFF;

	struct Binding
	{
		std::uint32_t keyboard{ kInvalidKey };
		std::uint32_t mouse{ kInvalidKey };
		std::uint32_t gamepad{ kInvalidKey };

		void               Capture(const RE::ControlMap* a_map, std::string_view a_event);
		[[nodiscard]] bool Matches(const RE::ButtonEvent* a_button) const noexcept;
	};

	[[nodiscard]] bool IsAttackButton(const RE::ButtonEvent* a_button) const;
	[[nodiscard]] bool IsJournalButton(const RE::ButtonEvent* a_button) const;

	[[nodiscard]] std::vector<Blanked> BlankBlockedEvents(RE::InputEvent* const* a_event);
	static void                        RestoreBlankedEvents(std::vector<Blanked>& a_blanked);
	[[nodiscard]] bool                 IsFilterActive() const;
	[[nodiscard]] bool                 ShouldBlock(RE::InputEvent* a_event);

	Binding _attack;
	Binding _journal;
	bool    _registered{ false };
	bool    _blocking{ false };

	// device << 32 | key code of every button first pressed while the lock
	// was on. Their releases are blocked too, since the game never saw the
	// press; the set is dropped when the lock ends.
	std::vector<std::uint64_t> _pressedWhileLocked;

	std::atomic<bool> _attackHeld{ false };
};
