#include "ClashInput.h"

#include "ClashController.h"

ClashInput* ClashInput::GetSingleton()
{
	static ClashInput singleton;
	return std::addressof(singleton);
}

void ClashInput::Register()
{
	if (_registered) {
		return;
	}
	const auto manager = RE::BSInputDeviceManager::GetSingleton();
	if (!manager) {
		logger::warn("BSInputDeviceManager unavailable; attack presses cannot be read");
		return;
	}
	manager->AddEventSink(this);
	_registered = true;
	logger::info("Registered input sink");
}

void ClashInput::InstallHooks()
{
	// BSTEventSink<InputEvent*> is the first base of both classes, so
	// ProcessEvent is slot 1 of their first vtable on every runtime.
	{
		REL::Relocation<std::uintptr_t> menuVtbl{ RE::VTABLE_MenuControls[0] };
		ProcessHook<RE::MenuControls>::func = menuVtbl.write_vfunc(1, ProcessHook<RE::MenuControls>::thunk);
	}
	stl::write_vfunc<RE::PlayerControls, 1, ProcessHook<RE::PlayerControls>>();
	logger::info("Installed input filter hooks");
}

void ClashInput::Binding::Capture(const RE::ControlMap* a_map, std::string_view a_event)
{
	keyboard = a_map->GetMappedKey(a_event, RE::INPUT_DEVICE::kKeyboard);
	mouse = a_map->GetMappedKey(a_event, RE::INPUT_DEVICE::kMouse);
	gamepad = a_map->GetMappedKey(a_event, RE::INPUT_DEVICE::kGamepad);
}

bool ClashInput::Binding::Matches(const RE::ButtonEvent* a_button) const noexcept
{
	const auto code = a_button->GetIDCode();
	switch (a_button->GetDevice()) {
	case RE::INPUT_DEVICE::kKeyboard:
		return keyboard != kInvalidKey && code == keyboard;
	case RE::INPUT_DEVICE::kMouse:
		return mouse != kInvalidKey && code == mouse;
	case RE::INPUT_DEVICE::kGamepad:
		return gamepad != kInvalidKey && code == gamepad;
	default:
		return false;
	}
}

void ClashInput::CaptureBindings()
{
	const auto controlMap = RE::ControlMap::GetSingleton();
	const auto userEvents = RE::UserEvents::GetSingleton();
	if (!controlMap || !userEvents) {
		return;
	}

	_attack.Capture(controlMap, userEvents->rightAttack.c_str());
	_journal.Capture(controlMap, userEvents->journal.c_str());

	logger::debug("Attack bindings: keyboard {:#x} mouse {:#x} gamepad {:#x}; journal: keyboard {:#x} gamepad {:#x}",
		_attack.keyboard, _attack.mouse, _attack.gamepad, _journal.keyboard, _journal.gamepad);
}

bool ClashInput::IsAttackButton(const RE::ButtonEvent* a_button) const
{
	const auto userEvents = RE::UserEvents::GetSingleton();
	return (userEvents && a_button->QUserEvent() == userEvents->rightAttack) || _attack.Matches(a_button);
}

bool ClashInput::IsJournalButton(const RE::ButtonEvent* a_button) const
{
	const auto userEvents = RE::UserEvents::GetSingleton();
	return (userEvents && a_button->QUserEvent() == userEvents->journal) || _journal.Matches(a_button);
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

void ClashInput::BeginBlocking()
{
	_blocking = true;
	_pressedWhileLocked.clear();
}

void ClashInput::EndBlocking()
{
	_blocking = false;
	_pressedWhileLocked.clear();
}

// Only while the gameplay context is on top: once the Journal (or anything
// else that pushes a context, such as the console) is open, its own input
// must reach it untouched.
bool ClashInput::IsFilterActive() const
{
	if (!_blocking || !ClashController::GetSingleton()->IsPlayerLocked()) {
		return false;
	}
	const auto controlMap = RE::ControlMap::GetSingleton();
	if (!controlMap) {
		return false;
	}
	const auto& stack = controlMap->GetRuntimeData().contextPriorityStack;
	return stack.empty() || stack[stack.size() - 1] == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
}

bool ClashInput::ShouldBlock(RE::InputEvent* a_event)
{
	switch (a_event->GetEventType()) {
	case RE::INPUT_EVENT_TYPE::kMouseMove:
	case RE::INPUT_EVENT_TYPE::kThumbstick:
		return true;
	case RE::INPUT_EVENT_TYPE::kButton:
		break;
	default:
		return false;
	}

	const auto button = static_cast<RE::ButtonEvent*>(a_event);
	if (IsAttackButton(button) || IsJournalButton(button)) {
		return false;
	}

	const std::uint64_t key = (static_cast<std::uint64_t>(button->GetDevice()) << 32) | button->GetIDCode();
	const bool          seen = std::find(_pressedWhileLocked.begin(), _pressedWhileLocked.end(), key) != _pressedWhileLocked.end();
	if (button->IsDown()) {
		if (!seen) {
			_pressedWhileLocked.push_back(key);
		}
		return true;
	}
	// Holds and releases: blocked only for buttons first pressed under the
	// lock. Anything held since before it keeps flowing so the game sees the
	// release and no handler is left thinking the button is still down.
	return seen;
}

std::vector<ClashInput::Blanked> ClashInput::BlankBlockedEvents(RE::InputEvent* const* a_event)
{
	std::vector<Blanked> blanked;
	if (!a_event || !*a_event || !IsFilterActive()) {
		return blanked;
	}
	for (auto event = *a_event; event; event = event->next) {
		if (!ShouldBlock(event)) {
			continue;
		}
		const auto idEvent = static_cast<RE::IDEvent*>(event);
		if (idEvent->userEvent.empty()) {
			continue;  // already a disabled control as far as the game is concerned
		}
		blanked.push_back({ idEvent, idEvent->userEvent });
		idEvent->userEvent = RE::BSFixedString{};
	}
	return blanked;
}

void ClashInput::RestoreBlankedEvents(std::vector<Blanked>& a_blanked)
{
	for (auto& entry : a_blanked) {
		entry.event->userEvent = entry.name;
	}
	a_blanked.clear();
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

RE::BSEventNotifyControl ClashInput::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*)
{
	if (!a_event || !*a_event) {
		return RE::BSEventNotifyControl::kContinue;
	}

	const auto controller = ClashController::GetSingleton();
	const auto phase = controller->GetPhase();
	const bool standoff = phase == ClashPhase::kStandoff;
	// Outside the lock the sink only stays interested until a held attack
	// button is seen released, so the flag cannot outlive the clash.
	if (!standoff && phase != ClashPhase::kApproach && !_attackHeld.load(std::memory_order_relaxed)) {
		return RE::BSEventNotifyControl::kContinue;
	}

	for (auto event = *a_event; event; event = event->next) {
		if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
			continue;
		}
		const auto button = static_cast<RE::ButtonEvent*>(event);
		if (!IsAttackButton(button)) {
			continue;
		}
		_attackHeld.store(button->IsPressed(), std::memory_order_relaxed);
		if (standoff && button->IsDown()) {
			controller->OnAttackPressed();
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}
