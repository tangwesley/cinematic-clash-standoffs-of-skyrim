#include "ClashController.h"

#include "BossRecognition.h"
#include "ClashAudio.h"
#include "ClashCamera.h"
#include "ClashDetection.h"
#include "ClashHUD.h"
#include "ClashInput.h"
#include "ClashRumble.h"
#include "ClashTDM.h"
#include "Settings.h"

namespace
{
	void SetShieldVisible(RE::Actor* a_actor, bool a_visible, bool a_log);  // defined further down
	[[nodiscard]] bool HasShieldEquipped(RE::Actor* a_actor);              // defined further down

	// Skyrim heading: 0 = +Y, increasing clockwise, so forward = (sin z, cos z).
	[[nodiscard]] float HeadingTo(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		return std::atan2(a_to.x - a_from.x, a_to.y - a_from.y);
	}

	[[nodiscard]] float SmoothStep(float a_t)
	{
		a_t = std::clamp(a_t, 0.0f, 1.0f);
		return a_t * a_t * (3.0f - 2.0f * a_t);
	}

	[[nodiscard]] RE::NiPoint3 Lerp(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_t)
	{
		return a_from + (a_to - a_from) * a_t;
	}

	// Humanoids only. Creature graphs that can block (draugr, falmer, ...) were
	// tried and rejected: the chest-to-chest slide and block poses are tuned
	// for the human skeleton and look wrong on them.
	[[nodiscard]] bool IsHumanoid(RE::Actor* a_actor)
	{
		if (a_actor->HasKeywordString("ActorTypeNPC"sv)) {
			return true;
		}
		const auto race = a_actor->GetRace();
		return race && race->HasKeywordString("ActorTypeNPC"sv);
	}

	// Wall-clock frame time, so the standoff timer is unaffected by the optional
	// slow motion. Falls back to the game delta if the timer looks broken.
	[[nodiscard]] float RealDelta(float a_fallback)
	{
		const auto timer = RE::BSTimer::GetSingleton();
		const float dt = timer ? timer->realTimeDelta : a_fallback;
		if (dt > 0.0f && dt < 0.5f) {
			return dt;
		}
		return std::clamp(a_fallback, 0.0f, 0.5f);
	}

	// Papyrus Actor.SetGhost through the script VM: the engine's own toggle, so
	// whatever it keeps alongside the ExtraGhost entry stays consistent. The
	// call runs on the VM's next update, within a frame.
	bool SetGhost(RE::Actor* a_actor, bool a_ghost)
	{
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm || !a_actor) {
			return false;
		}
		const auto policy = vm->GetObjectHandlePolicy();
		if (!policy) {
			return false;
		}
		const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), a_actor);
		if (handle == policy->EmptyHandle()) {
			return false;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
		const auto args = RE::MakeFunctionArguments(static_cast<bool>(a_ghost));
		return vm->DispatchMethodCall(handle, "Actor", "SetGhost", args, callback);
	}

	// Frames to wait for the asynchronous SetGhost before warning that it never
	// took effect.
	constexpr int kGhostVerifyFrames = 30;

	[[nodiscard]] float StaminaRatio(RE::Actor* a_actor)
	{
		const auto owner = a_actor->AsActorValueOwner();
		if (!owner) {
			return 1.0f;
		}
		const float max = owner->GetPermanentActorValue(RE::ActorValue::kStamina);
		if (max <= 0.0f) {
			return 1.0f;
		}
		return std::clamp(owner->GetActorValue(RE::ActorValue::kStamina) / max, 0.0f, 1.0f);
	}

	// The skill that governs the weapon in hand: One-Handed or Two-Handed from
	// the weapon's own record, right hand first. Bows, staves and empty hands
	// are read as One-Handed; a clash needs a melee weapon to start anyway.
	[[nodiscard]] RE::ActorValue WeaponSkill(RE::Actor* a_actor)
	{
		for (bool leftHand : { false, true }) {
			const auto form = a_actor->GetEquippedObject(leftHand);
			const auto weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
			if (!weapon) {
				continue;
			}
			const auto skill = weapon->weaponData.skill.get();
			if (skill == RE::ActorValue::kOneHanded || skill == RE::ActorValue::kTwoHanded) {
				return skill;
			}
		}
		return RE::ActorValue::kOneHanded;
	}

	// Current value of that skill, buffs and enchantments included.
	[[nodiscard]] float WeaponSkillOf(RE::Actor* a_actor)
	{
		const auto owner = a_actor->AsActorValueOwner();
		return owner ? std::max(0.0f, owner->GetActorValue(WeaponSkill(a_actor))) : 0.0f;
	}

	[[nodiscard]] bool RollChance(float a_percent)
	{
		if (a_percent >= 100.0f) {
			return true;
		}
		if (a_percent <= 0.0f) {
			return false;
		}
		static std::mt19937                   rng{ std::random_device{}() };
		std::uniform_real_distribution<float> dist(0.0f, 100.0f);
		return dist(rng) < a_percent;
	}

	void Notify(const std::string& a_text)
	{
		if (Settings::GetSingleton()->messagesEnabled && !a_text.empty()) {
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
		}
	}

	[[nodiscard]] constexpr std::uint32_t Flag(RE::UserEvents::USER_EVENT_FLAG a_flag)
	{
		return static_cast<std::uint32_t>(a_flag);
	}

	using UEFlag = RE::UserEvents::USER_EVENT_FLAG;

	// Everything the player must not do mid-clash through the ControlMap.
	// kFighting is deliberately absent: disabling it makes the game sheathe the
	// weapon, so fighting input is suppressed at the handler level instead (see
	// CanProcessHook). Everything else (menus, hotkeys, quicksave, console,
	// sprint, ...) is stopped by the input filter in ClashInput, which lets
	// only the attack button and the Journal key through while locked.
	constexpr std::uint32_t kLockedControls =
		Flag(UEFlag::kMovement) | Flag(UEFlag::kLooking) | Flag(UEFlag::kActivate) |
		Flag(UEFlag::kPOVSwitch) | Flag(UEFlag::kSneaking) |
		Flag(UEFlag::kWheelZoom) | Flag(UEFlag::kJumping);

	constexpr float kMinMoveDistance = 0.05f;      // while sliding into place
	constexpr float kSettledMoveDistance = 2.0f;   // once in place: only real shoves are corrected

	// Set while this thread is inside one of our own NotifyAnimationGraph
	// calls, so the event filter lets it through. Per thread: actor updates run
	// on a pool, and a shared flag would wave through an AI attack fired on
	// another thread for as long as we were sending.
	thread_local bool gSendingOwnEvent = false;
}

ClashController* ClashController::GetSingleton()
{
	static ClashController singleton;
	return std::addressof(singleton);
}

void ClashController::InstallHooks()
{
	// Actor::Update(float) -- see RE/A/Actor.h. VR inserts a virtual before it.
	stl::write_vfunc<RE::PlayerCharacter, PlayerUpdateHook>(REL::Module::IsVR() ? 0xAE : 0xAD);
	logger::info("Installed PlayerCharacter::Update hook");

	// PlayerInputHandler::CanProcess is slot 1 on every runtime.
	stl::write_vfunc<RE::AttackBlockHandler, 1, CanProcessHook<RE::AttackBlockHandler>>();
	stl::write_vfunc<RE::ReadyWeaponHandler, 1, CanProcessHook<RE::ReadyWeaponHandler>>();
	stl::write_vfunc<RE::ShoutHandler, 1, CanProcessHook<RE::ShoutHandler>>();
	logger::info("Installed fighting input handler hooks");

	// NPC update, same slot as the player's.
	stl::write_vfunc<RE::Character, CharacterUpdateHook>(REL::Module::IsVR() ? 0xAE : 0xAD);

	// TESObjectREFR::UpdateAnimation, where the frame's pose lands.
	stl::write_vfunc<RE::PlayerCharacter, 0x7D, UpdateAnimationHook<RE::PlayerCharacter>>();
	stl::write_vfunc<RE::Character, 0x7D, UpdateAnimationHook<RE::Character>>();
	logger::info("Installed post-animation hooks");

	// NotifyAnimationGraph lives on the IAnimationGraphManagerHolder vtable,
	// which is the fourth vtable of the actor classes.
	{
		REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[3] };
		NotifyGraphHook<RE::PlayerCharacter>::func = playerVtbl.write_vfunc(1, NotifyGraphHook<RE::PlayerCharacter>::thunk);
		REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[3] };
		NotifyGraphHook<RE::Character>::func = characterVtbl.write_vfunc(1, NotifyGraphHook<RE::Character>::thunk);
	}
	logger::info("Installed animation graph event filter");

	// MagicTarget::AddTarget is slot 1 of the MagicTarget vtable, the fifth
	// vtable of the player class (after TESForm, BSHandleRefObject, the graph
	// event sink and IAnimationGraphManagerHolder). Player only: hostile magic
	// reaching the opponent is left to the game.
	{
		REL::Relocation<std::uintptr_t> magicVtbl{ RE::VTABLE_PlayerCharacter[4] };
		AddTargetHook::func = magicVtbl.write_vfunc(1, AddTargetHook::thunk);
	}
	logger::info("Installed player magic effect filter");
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

bool ClashController::OnClashDetected(RE::Actor* a_attacker, RE::Actor* a_target)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->enabled || !a_attacker || !a_target) {
		return false;
	}
	if (_phase != ClashPhase::kIdle || _pendingStart || _cooldownLeft > 0.0f) {
		return false;
	}

	const auto player = RE::PlayerCharacter::GetSingleton();
	RE::Actor* npc = nullptr;
	if (a_attacker == player) {
		npc = a_target;
	} else if (a_target == player) {
		npc = a_attacker;
	} else {
		return false;
	}
	if (!npc || npc == player) {
		return false;
	}

	if (!IsHumanoid(npc)) {
		logger::debug("Clash rejected: {} is not a humanoid", npc->GetName());
		return false;
	}
	if (npc->IsPlayerTeammate()) {
		return false;
	}
	if (settings->requireHostile && !settings->forceClashOnHit && !npc->IsHostileToActor(player)) {
		logger::debug("Clash rejected: {} is not hostile", npc->GetName());
		return false;
	}
	if (settings->bossesOnly && !settings->forceClashOnHit && !BossRecognition::GetSingleton()->IsBoss(npc)) {
		logger::debug("Clash rejected: {} is not a boss", npc->GetName());
		return false;
	}
	if (player->IsOnMount() || npc->IsOnMount()) {
		return false;
	}
	if (player->IsInKillMove() || npc->IsInKillMove()) {
		return false;
	}
	if (npc->AsActorState()->GetLifeState() != RE::ACTOR_LIFE_STATE::kAlive) {
		logger::debug("Clash rejected: {} is not in the normal life state", npc->GetName());
		return false;
	}
	if (player->AsActorState()->IsSwimming() || npc->AsActorState()->IsSwimming()) {
		return false;
	}
	if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
		return false;
	}

	const float distance = (npc->GetPosition() - player->GetPosition()).Length();
	if (distance > settings->maxStartDistance) {
		logger::debug("Clash rejected: distance {:.0f} exceeds {:.0f}", distance, settings->maxStartDistance);
		return false;
	}

	if (!settings->forceClashOnHit && !RollChance(settings->triggerChance)) {
		// Do not re-roll every hit of the same exchange.
		_cooldownLeft = std::max(_cooldownLeft, 1.0f);
		return false;
	}

	_playerHandle = player->GetHandle();
	_npcHandle = npc->GetHandle();
	_playerID = player->GetFormID();
	_npcID = npc->GetFormID();
	_pendingStart = true;

	// Difficulty: the governing weapon skills. An opponent far enough behind
	// the player never gets a standoff; the reverse gap only makes the
	// standoff harder, never skips it.
	_playerSkill = WeaponSkillOf(player);
	_npcSkill = WeaponSkillOf(npc);
	_instantWin = settings->autoWinSkillGap > 0.0f && _playerSkill - _npcSkill >= settings->autoWinSkillGap;

	logger::info("Clash accepted against {} ({:08X}) at {:.0f} units; weapon skill {:.0f} vs {:.0f}{}",
		npc->GetName(), npc->GetFormID(), distance, _playerSkill, _npcSkill, _instantWin ? " (auto-win)" : "");
	return true;
}

bool ClashController::ShouldSwallowHit(const RE::Actor* a_attacker, const RE::Actor* a_target) const
{
	const bool active = _pendingStart || _phase == ClashPhase::kApproach || _phase == ClashPhase::kStandoff;
	if (!active) {
		return false;
	}
	return IsParticipant(a_attacker) || IsParticipant(a_target);
}

bool ClashController::ShouldRejectEffect(const RE::MagicTarget::AddTargetData& a_data) const
{
	if (!_rejectHostileMagic.load(std::memory_order_relaxed)) {
		return false;
	}
	const auto base = a_data.effect ? a_data.effect->baseEffect : nullptr;
	if (!base || !(base->IsHostile() || base->IsDetrimental())) {
		return false;
	}
	// The player's own effects (perk abilities, self-targeted spells) still apply.
	if (a_data.caster && a_data.caster == RE::PlayerCharacter::GetSingleton()) {
		return false;
	}
	if (Settings::GetSingleton()->debugLog) {
		const char* name = base->GetFullName();
		logger::debug("Refused effect '{}' ({:08X}) from {} on the locked player", name ? name : "", base->GetFormID(),
			a_data.caster ? a_data.caster->GetName() : "no caster");
	}
	return true;
}

void ClashController::OnAttackPressed()
{
	if (_phase == ClashPhase::kStandoff) {
		_pendingPresses.fetch_add(1, std::memory_order_relaxed);
	}
}

// ---------------------------------------------------------------------------
// Frame driver
// ---------------------------------------------------------------------------

void ClashController::OnFrame(float a_delta)
{
	const float dt = RealDelta(a_delta);

	// Pick up INI edits without a restart.
	_settingsPollTimer += dt;
	if (_settingsPollTimer >= 1.0f) {
		_settingsPollTimer = 0.0f;
		if (Settings::GetSingleton()->ReloadIfChanged()) {
			ClashHUD::GetSingleton()->ReloadAssets();
		}
	}

	// A save written while the player was ghosted for a clash brought the flag
	// back with it (the co-save load callback reports this); clear it now that
	// the game is running and the VM is available.
	if (_clearGhostOnLoad) {
		_clearGhostOnLoad = false;
		if (SetGhost(RE::PlayerCharacter::GetSingleton(), false)) {
			logger::info("Cleared the player's ghost flag left by a save written mid-clash");
		} else {
			logger::warn("Could not clear the player's ghost flag left by a save written mid-clash");
		}
	}

	if (_phase == ClashPhase::kIdle) {
		if (_cooldownLeft > 0.0f) {
			_cooldownLeft = std::max(0.0f, _cooldownLeft - dt);
		}
		if (_pendingStart) {
			BeginClash();
		}
		return;
	}

	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();
	RE::Actor* player = playerPtr.get();
	RE::Actor* npc = npcPtr.get();

	if (_phase != ClashPhase::kAftermath) {
		if (!player || !npc || player->IsDead() || npc->IsDead() || !player->Is3DLoaded() || !npc->Is3DLoaded()) {
			Abort("a participant died or unloaded");
			return;
		}
	}

	VerifyGhost(player);
	_phaseTime += dt;

	if (_phase == ClashPhase::kApproach || _phase == ClashPhase::kStandoff) {
		_clashTime += dt;
		if (!_settled && _clashTime >= Settings::GetSingleton()->settleTime) {
			_settled = true;
			logger::debug("Settled: headings frozen, spine tracking off");
		}

		// Weapon geometry, before anything positions the pair this frame. The
		// separation is re-solved only while the block pose is coming up and
		// held from the settle on: a target that keeps moving is a warp every
		// frame, which the movement system reads as walking.
		MeasureBlades(player, npc);
		if (Settings::GetSingleton()->solveClashDistance && !_solveLocked) {
			if (_settled) {
				_solveLocked = true;
			}
			SolveResult result;
			if (!SolveClashDistance(_solveLocked && !_solveReported, result) && _solveLocked) {
				// No standing position closes this gap; bend instead.
				PlanTilt(result, player, npc);
			}
			_solveReported = _solveLocked;
		} else if (_solveLocked && !_tiltResolved && _tiltValid.load(std::memory_order_acquire)) {
			if (_tiltApplied.load(std::memory_order_acquire)) {
				// The bend is in the pose just measured, so calculate separation again. 
				// fail = keep the distance
				// the tilt was planned for.
				_tiltResolved = true;
				const float planned = _solvedDistance;
				SolveResult result;
				if (SolveClashDistance(false, result)) {
					logger::info("Clash distance re-solved over the tilt: {:.0f} units, gap {:.1f}", _solvedDistance, result.gap);
				} else {
					_solvedDistance = planned;
					logger::info("Clash distance after the tilt: {:.1f} units still between the blades at {:.0f} apart",
						result.gap, planned);
				}
			} else if (_clashTime > Settings::GetSingleton()->settleTime + 0.5f) {
				// The hook has not run for that actor: the bend is not reaching
				// the skeleton at all, which should not look like a tilt that
				// merely did not help.
				_tiltResolved = true;
				logger::warn("Clash tilt: planned for {:08X} but never applied; UpdateAnimation has not run for it", _tilt.actor);
			}
		}

		// Blades swung into the other fighter, measured off the blades just
		// read -- which already carry the last frame's correction.
		UpdateClearance(player, npc, dt);

		// Freeze once settled and both are actually in the block pose.
		if (_settled && !_frozen && Settings::GetSingleton()->freezeAfterSettle && _playerBlock.established && _npcBlock.established) {
			Freeze(player, npc);
		}
	}

	switch (_phase) {
	case ClashPhase::kApproach:
		TickApproach(player, npc, dt);
		break;
	case ClashPhase::kStandoff:
		TickStandoff(player, npc, dt);
		break;
	case ClashPhase::kResolve:
		TickResolve(player, npc);
		break;
	case ClashPhase::kAftermath:
		TickAftermath(dt);
		break;
	default:
		break;
	}

	// The pad: base level through approach and standoff, pulses on top, and
	// still ticked through the aftermath so the auto-win kick can play out.
	ClashRumble::GetSingleton()->Update(dt);
}

void ClashController::BeginClash()
{
	_pendingStart = false;

	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();
	RE::Actor* player = playerPtr.get();
	RE::Actor* npc = npcPtr.get();
	if (!player || !npc || player->IsDead() || npc->IsDead()) {
		logger::debug("Pending clash dropped: participant no longer valid");
		ReturnToIdle();
		return;
	}

	const auto settings = Settings::GetSingleton();

	if (_instantWin) {
		BeginInstantWin(player, npc);
		return;
	}

	// Set before the phase that arms the filter: it tests the phase first and
	// the holders second, so a live phase with stale holders passes everything.
	_playerHolder = static_cast<RE::IAnimationGraphManagerHolder*>(player);
	_npcHolder = static_cast<RE::IAnimationGraphManagerHolder*>(npc);

	_phase = ClashPhase::kApproach;
	_phaseTime = 0.0f;
	_meter = 0.5f;
	_outcome = ClashOutcome::kNone;
	_pendingPresses.store(0, std::memory_order_relaxed);
	_holdAccumulator = 0.0f;
	_blockSent = false;
	_playerBlock = {};
	_npcBlock = {};
	_clashTime = 0.0f;
	_settled = false;
	_frozen = false;
	_npcAIDisabledForFreeze = false;
	_solvedDistance = 0.0f;
	_solveLocked = false;
	_solveReported = false;
	ClearTilt();
	ClearClearance();

	SetupGeometry(player, npc);

	// Still in the swing that clashed, so this measurement is where the weapons
	// actually met. Place the audio and first sparks here. The block
	// pose the solve needs only exists from the next graph update.
	MeasureBlades(player, npc);

	// Hold the opponent still. Restrained stops movement and combat decisions
	// but keeps the animation graph running; disabling AI would freeze the
	// graph outright, leaving them stuck mid-swing.
	npc->SetLifeState(RE::ACTOR_LIFE_STATE::kRestrained);
	_npcRestrained = true;

	// The opponent's movement controller keeps turning it toward whatever the
	// AI wants even while restrained; taking it off AI driving stops that
	// without touching the animation graph.
	if (const auto& controller = npc->GetActorRuntimeData().movementController) {
		controller->SetControlsDriven();
		_npcControlsDriven = true;
	}

	// TDM turns the player toward the crosshair while blocking; take its yaw
	// control for the clash (and its directional movement / head tracking /
	// target lock), hand everything back at the resolve.
	ClashTDM::GetSingleton()->BeginControl();

	// Lock the player out of everything except the button they need to mash.
	LockPlayer(RE::PlayerCharacter::GetSingleton());

	// Snap both facings now rather than easing into them, and take shields out
	// of the picture before the block animation is chosen. The pitch is only
	// touched by a first-person standoff, and eased rather than snapped.
	_playerStartPitch = player->GetAngleX();
	_firstPersonClash = false;
	FaceEachOther(player, npc);
	HideShield(player, _playerShieldHidden);
	HideShield(npc, _npcShieldHidden);
	BeginCollisionIgnore(player, npc);

	// Cut whatever both actors are playing and put them in the block stance
	// right now, in this same graph update; the per-frame hold re-asserts it
	// if the graph did not take it.
	CancelAndBlock(player);
	CancelAndBlock(npc);
	_blockSent = true;

	_forcedThirdPerson = ClashCamera::GetSingleton()->Begin();
	// Still in first person after the camera had its say (bForceThirdPerson
	// off, or the clash camera disabled): the standoff is seen from the
	// player's own eyes. Not on VR, where the headset owns the view.
	if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInFirstPerson() && !REL::Module::IsVR()) {
		_firstPersonClash = true;
	}

	if (settings->timeMultiplier < 1.0f) {
		if (const auto timer = RE::BSTimer::GetSingleton()) {
			timer->SetGlobalTimeMultiplier(settings->timeMultiplier, false);
			_timeScaled = true;
		}
	}

	ResolveSparkModel(player, npc);
	_sparkTimer = 0.0f;
	_meterOffset = 0.0f;

	// Sound: the clang of the lock, and the opponent's war cry.
	const auto audio = ClashAudio::GetSingleton();
	audio->Setup(_blockImpact);
	RE::NiPoint3 contact;
	if (GetContactPoint(contact)) {
		audio->PlayImpact(contact, player->Get3D());
	}
	audio->PlayEnemyShout(npc);

	ClashInput::GetSingleton()->CaptureBindings();
	ClashInput::GetSingleton()->ResetHeld();
	ClashHUD::GetSingleton()->RefreshAttackGlyph();
	ClashHUD::GetSingleton()->SetHoldMode(settings->holdToMash);
	ClashHUD::GetSingleton()->SetNames(player->GetName() ? player->GetName() : "", npc->GetName() ? npc->GetName() : "");
	ClashHUD::GetSingleton()->SetState(true, 0.5f, 1.0f, false);
	Notify(settings->holdToMash ? settings->messageStartHold : settings->messageStart);

	if (settings->rumbleEnabled) {
		ClashRumble::GetSingleton()->SetBase(settings->rumbleLargeMotor, settings->rumbleSmallMotor);
	}

	logger::info("Clash started against {} ({:08X}); difficulty {} ({:.1f} presses/s), skill {:.0f} vs {:.0f}, push {:.3f}/s{}{}",
		npc->GetName(), npc->GetFormID(), settings->difficultyMode, settings->PressesPerSecond(),
		_playerSkill, _npcSkill, NpcPushRate(player, npc), settings->holdToMash ? ", hold-to-mash" : "",
		_firstPersonClash ? ", in first person" : "");
}

// The opponent's skill is too far behind for a struggle: the clash resolves
// on the spot. The triggering hit was already swallowed, so this is one
// stagger, the deciding blow's sound and sparks, and the aftermath window so
// the OAR loser condition applies to the stagger. Nothing is locked or moved.
void ClashController::BeginInstantWin(RE::Actor* a_player, RE::Actor* a_npc)
{
	const auto settings = Settings::GetSingleton();
	_instantWin = false;

	_phase = ClashPhase::kAftermath;
	_phaseTime = 0.0f;
	_meter = 1.0f;
	_outcome = ClashOutcome::kPlayerWon;
	_meterOffset = 0.0f;
	SetupGeometry(a_player, a_npc);
	MeasureBlades(a_player, a_npc);

	// The parry mod may have flagged both actors and started recoils; the
	// player's swing is what broke through, so it is left alone bar the recoil.
	for (auto actor : { a_player, a_npc }) {
		actor->SetGraphVariableBool("bMaxsuWeaponParry_InWeaponParry", false);
	}
	SendEvent(a_player, "recoilStop");

	ResolveSparkModel(a_player, a_npc);
	const auto audio = ClashAudio::GetSingleton();
	audio->Setup(_blockImpact);
	RE::NiPoint3 contact = _center;
	contact.z = (a_player->GetPositionZ() + a_npc->GetPositionZ()) * 0.5f + settings->cameraFraming.aimHeight * a_player->GetScale();
	if (settings->contactFromWeapons && _contactValid) {
		contact = _contactPoint;
	}
	audio->PlayOverpower(contact, a_player->Get3D());
	_sparkTimer = settings->sparksInterval;  // one burst, right now
	TickSparks(a_player, a_npc, 0.0f, 0.0f);

	if (settings->loserStaggerMagnitude > 0.0f) {
		SendStagger(a_npc, settings->loserStaggerMagnitude);
	}
	if (settings->rumbleEnabled) {
		ClashRumble::GetSingleton()->Pulse(1.0f, 0.5f, 0.25f);
	}

	Notify(settings->messageAutoWin);
	logger::info("Clash auto-won against {} ({:08X}): weapon skill {:.0f} vs {:.0f} (gap {:.0f} >= {:.0f})",
		a_npc->GetName(), a_npc->GetFormID(), _playerSkill, _npcSkill, _playerSkill - _npcSkill, settings->autoWinSkillGap);
}

void ClashController::SetupGeometry(RE::Actor* a_player, RE::Actor* a_npc)
{
	_playerStart = a_player->GetPosition();
	_npcStart = a_npc->GetPosition();
	_axis = _npcStart - _playerStart;
	_axis.z = 0.0f;
	if (_axis.Length() < 1.0f) {
		const float heading = a_player->GetAngleZ();
		_axis = RE::NiPoint3{ std::sin(heading), std::cos(heading), 0.0f };
	} else {
		_axis.Unitize();
	}
	_center = (_playerStart + _npcStart) * 0.5f;
}

void ClashController::TickApproach(RE::Actor* a_player, RE::Actor* a_npc, float a_dt)
{
	const auto settings = Settings::GetSingleton();

	HoldBlock(a_player, _playerBlock, a_dt);  // the NPC's hold runs after its own update

	const float t = SmoothStep(_phaseTime / settings->approachTime);
	PositionActors(a_player, a_npc, t, 0.0f);
	FaceEachOther(a_player, a_npc);
	if (t >= 0.5f) {
		TickSparks(a_player, a_npc, a_dt, 0.0f);
		RE::NiPoint3 contact;
		if (GetContactPoint(contact)) {
			ClashAudio::GetSingleton()->UpdateLoop(contact, a_dt, a_player->Get3D());
		}
	}

	if (_phaseTime >= settings->approachTime) {
		_phase = ClashPhase::kStandoff;
		_phaseTime = 0.0f;
		_pendingPresses.store(0, std::memory_order_relaxed);
		logger::debug("Standoff begins");
	}
}

void ClashController::TickStandoff(RE::Actor* a_player, RE::Actor* a_npc, float a_dt)
{
	const auto settings = Settings::GetSingleton();

	int presses = _pendingPresses.exchange(0, std::memory_order_relaxed);
	if (settings->holdToMash) {
		// Accessibility: held time is credited as presses at the difficulty's
		// own rate, so a held button exactly matches an equal opponent and the
		// skill gap decides. Discrete presses do not count on top.
		presses = 0;
		if (ClashInput::GetSingleton()->IsAttackHeld()) {
			_holdAccumulator += settings->PressesPerSecond() * a_dt;
			presses = static_cast<int>(_holdAccumulator);
			_holdAccumulator -= static_cast<float>(presses);
		}
	}
	if (presses > 0) {
		if (settings->rumbleEnabled && settings->rumblePressPulse > 0.0f) {
			ClashRumble::GetSingleton()->Pulse(0.0f, settings->rumblePressPulse, settings->rumblePulseDuration);
		}
		float gain = settings->pressGain;
		if (settings->staminaAffectsPlayer) {
			gain *= 0.5f + 0.5f * StaminaRatio(a_player);
		}
		_meter += gain * static_cast<float>(presses);

		if (settings->staminaCostPerPress > 0.0f) {
			if (const auto owner = a_player->AsActorValueOwner()) {
				owner->RestoreActorValue(RE::ActorValue::kStamina,
					-settings->staminaCostPerPress * static_cast<float>(presses));
			}
		}
		if (settings->shakeOnPress) {
			RE::ShakeCamera(settings->shakeStrength, a_player->GetPosition(), 0.12f);
		}
		if (!settings->pressSound.empty()) {
			RE::PlaySound(settings->pressSound.c_str());
		}
	}

	_meter -= NpcPushRate(a_player, a_npc) * a_dt;
	_meter = std::clamp(_meter, 0.0f, 1.0f);

	ClashHUD::GetSingleton()->SetState(true, _meter, 1.0f - _phaseTime / settings->duration, true);

	PositionActors(a_player, a_npc, 1.0f, (_meter - 0.5f) * settings->pushDistance);
	FaceEachOther(a_player, a_npc);
	TickSparks(a_player, a_npc, a_dt, (_meter - 0.5f) * settings->pushDistance);
	{
		RE::NiPoint3 contact;
		if (GetContactPoint(contact)) {
			ClashAudio::GetSingleton()->UpdateLoop(contact, a_dt, a_player->Get3D());
		}
	}

	HoldBlock(a_player, _playerBlock, a_dt);  // the NPC's hold runs after its own update

	if (settings->debugLog) {
		_diagTimer += a_dt;
		if (_diagTimer >= 0.5f) {
			_diagTimer = 0.0f;
			for (auto actor : { a_player, a_npc }) {
				bool         blocking = false;
				std::int32_t leftType = -1;
				actor->GetGraphVariableBool("IsBlocking", blocking);
				actor->GetGraphVariableInt("iLeftHandType", leftType);
				const float locked = actor == a_player ? _playerHeading : _npcHeading;
				logger::debug("state: {} IsBlocking={} iLeftHandType={} weaponState={} attackState={} angleZ={:.1f} locked={:.1f} camYaw={:.1f}",
					actor->GetName(), blocking, leftType, static_cast<int>(actor->AsActorState()->GetWeaponState()),
					static_cast<int>(actor->AsActorState()->GetAttackState()), actor->GetAngleZ() * 180.0f / std::numbers::pi_v<float>,
					locked * 180.0f / std::numbers::pi_v<float>,
					RE::PlayerCamera::GetSingleton() ? RE::PlayerCamera::GetSingleton()->GetRuntimeData2().yaw * 180.0f / std::numbers::pi_v<float> : 0.0f);
			}
			if (_blades.valid) {
				constexpr float kToDegrees = 180.0f / std::numbers::pi_v<float>;
				logger::debug("blades: {:.0f} + {:.0f} units, gap {:.1f} at ({:.0f}, {:.0f}, {:.0f}), standing {:.0f} apart{}; clipping {:.1f}/{:.1f} units, turned back {:.1f}/{:.1f} deg",
					_blades.player.Length(), _blades.npc.Length(), _bladeGap, _contactPoint.x, _contactPoint.y, _contactPoint.z,
					StandoffDistance(), _solvedDistance > 0.0f ? " (solved)" : "",
					_clearanceDepth[0], _clearanceDepth[1], _clearance[0].angle * kToDegrees, _clearance[1].angle * kToDegrees);
			}
		}
	}

	if (_meter >= 1.0f) {
		Finish(ClashOutcome::kPlayerWon);
	} else if (_meter <= 0.0f) {
		Finish(ClashOutcome::kPlayerLost);
	} else if (_phaseTime >= settings->duration) {
		// Time ran out with nobody pushed off the meter.
		Finish(TimeoutOutcome());
	}
}

ClashOutcome ClashController::TimeoutOutcome() const
{
	// Either a draw wherever the marker sits, or the end it is nearer to takes
	// the win. Dead centre is nobody's win, so it stays a draw.
	if (Settings::GetSingleton()->timeoutResolution != 1 || _meter == 0.5f) {
		return ClashOutcome::kDraw;
	}
	return _meter > 0.5f ? ClashOutcome::kPlayerWon : ClashOutcome::kPlayerLost;
}

void ClashController::Finish(ClashOutcome a_outcome)
{
	const auto settings = Settings::GetSingleton();

	// The deciding blow, before the phase changes and the contact point is gone.
	{
		const auto   audio = ClashAudio::GetSingleton();
		RE::NiPoint3 contact;
		audio->StopLoop();
		if (GetContactPoint(contact)) {
			const auto playerPtr = _playerHandle.get();
			audio->PlayOverpower(contact, playerPtr ? playerPtr->Get3D() : nullptr);
		}
	}

	_outcome = a_outcome;
	_phase = ClashPhase::kResolve;
	_phaseTime = 0.0f;

	// Both pose corrections stop here rather than at the return to idle: the
	// stagger that follows must not be bent or turned. Nothing to restore -
	// the next animation update writes the graph's own pose back.
	ClearTilt();
	ClearClearance();

	// Graphs must be running again before they are told to stop blocking.
	Unfreeze();
	EndCollisionIgnore();
	ClashTDM::GetSingleton()->EndControl();
	RestoreNpcMovement();

	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();
	if (playerPtr) {
		SendBlockStop(playerPtr.get());
		RestoreShield(playerPtr.get(), _playerShieldHidden);
		UnlockUpperBody(playerPtr.get());
	}
	if (npcPtr) {
		SendBlockStop(npcPtr.get());
		RestoreShield(npcPtr.get(), _npcShieldHidden);
		UnlockUpperBody(npcPtr.get());
	}

	ClashCamera::GetSingleton()->End();
	ClashHUD::GetSingleton()->Hide();
	ClashRumble::GetSingleton()->Stop();
	ClashInput::GetSingleton()->ResetHeld();
	UnlockPlayer();
	RestoreTime();

	const char* result = "drew";
	if (a_outcome == ClashOutcome::kPlayerWon) {
		result = "won";
		Notify(settings->messageWin);
	} else if (a_outcome == ClashOutcome::kPlayerLost) {
		result = "lost";
		Notify(settings->messageLose);
	} else {
		Notify(settings->messageDraw);
	}
	logger::info("Clash finished: player {} (meter {:.2f})", result, _meter);
}

void ClashController::TickResolve(RE::Actor* a_player, RE::Actor* a_npc)
{
	const auto settings = Settings::GetSingleton();

	if (_outcome == ClashOutcome::kDraw) {
		// Both break off; a small stagger each rather than the loser's large one.
		if (settings->drawStaggerMagnitude > 0.0f) {
			SendStagger(a_player, settings->drawStaggerMagnitude);
			SendStagger(a_npc, settings->drawStaggerMagnitude);
		}
	} else {
		RE::Actor* loser = _outcome == ClashOutcome::kPlayerWon ? a_npc : a_player;
		RE::Actor* winner = _outcome == ClashOutcome::kPlayerWon ? a_player : a_npc;

		if (settings->loserStaggerMagnitude > 0.0f) {
			SendStagger(loser, settings->loserStaggerMagnitude);
		}
		if (settings->winnerStaggerMagnitude > 0.0f) {
			SendStagger(winner, settings->winnerStaggerMagnitude);
		}
	}

	RestoreNpcAI();

	_phase = ClashPhase::kAftermath;
	_phaseTime = 0.0f;
}

void ClashController::TickAftermath(float)
{
	const auto settings = Settings::GetSingleton();

	if (_forcedThirdPerson && _phaseTime >= settings->firstPersonRestoreDelay) {
		ClashCamera::GetSingleton()->RestoreFirstPerson();
		_forcedThirdPerson = false;
	}
	if (!_forcedThirdPerson && _phaseTime >= settings->outcomeWindow) {
		ReturnToIdle();
	}
}

void ClashController::ReturnToIdle()
{
	ClashRumble::GetSingleton()->Stop();  // a pulse must not outlive the last frame we tick
	_phase = ClashPhase::kIdle;
	_phaseTime = 0.0f;
	_cooldownLeft = Settings::GetSingleton()->cooldownSeconds;
	_pendingStart = false;
	_outcome = ClashOutcome::kNone;
	_playerHandle.reset();
	_npcHandle.reset();
	_playerID = 0;
	_npcID = 0;
	_playerHolder = nullptr;
	_npcHolder = nullptr;
	_firstPersonClash = false;
	_blades = {};
	_contactValid = false;
	_bladeGap = 0.0f;
	_solvedDistance = 0.0f;
	_solveLocked = false;
	_solveReported = false;
	ClearTilt();
	ClearClearance();
}

void ClashController::Abort(std::string_view a_reason)
{
	if (_phase == ClashPhase::kIdle && !_pendingStart) {
		return;
	}
	logger::info("Clash aborted: {}", a_reason);

	const bool locked = _phase == ClashPhase::kApproach || _phase == ClashPhase::kStandoff;
	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();

	ClearTilt();
	ClearClearance();
	Unfreeze();
	EndCollisionIgnore();
	ClashTDM::GetSingleton()->EndControl();
	RestoreNpcMovement();

	if (locked) {
		if (playerPtr) {
			SendBlockStop(playerPtr.get());
		}
		if (npcPtr) {
			SendBlockStop(npcPtr.get());
		}
	}
	if (playerPtr) {
		RestoreShield(playerPtr.get(), _playerShieldHidden);
		UnlockUpperBody(playerPtr.get());
	}
	if (npcPtr) {
		RestoreShield(npcPtr.get(), _npcShieldHidden);
		UnlockUpperBody(npcPtr.get());
	}
	_playerShieldHidden = false;
	_npcShieldHidden = false;

	ClashCamera::GetSingleton()->End();
	ClashHUD::GetSingleton()->Hide();
	ClashAudio::GetSingleton()->StopLoop();
	ClashRumble::GetSingleton()->Stop();
	ClashInput::GetSingleton()->ResetHeld();
	_instantWin = false;
	if (_forcedThirdPerson) {
		ClashCamera::GetSingleton()->RestoreFirstPerson();
		_forcedThirdPerson = false;
	}
	UnlockPlayer();
	RestoreNpcAI();
	RestoreTime();
	ReturnToIdle();
}

// ---------------------------------------------------------------------------
// Actor manipulation
// ---------------------------------------------------------------------------

void ClashController::PositionActors(RE::Actor* a_player, RE::Actor* a_npc, float a_blend, float a_meterOffset)
{
	const float half = StandoffDistance() * 0.5f;
	_meterOffset = a_meterOffset;

	// Targets are horizontal only: the character controller owns the height,
	// and fighting it for a fixed Z meant a warp every frame once the actor
	// had settled onto the ground, which the movement system reads as motion
	// (walking, turning to face it). Only a real horizontal displacement is
	// corrected.
	const auto move = [&](RE::Actor* a_actor, const RE::NiPoint3& a_start, const RE::NiPoint3& a_target) {
		const auto current = a_actor->GetPosition();
		auto       desired = Lerp(a_start, a_target, a_blend);
		desired.z = current.z;
		const float dx = desired.x - current.x;
		const float dy = desired.y - current.y;
		const float threshold = a_blend < 1.0f || !_settled ? kMinMoveDistance : kSettledMoveDistance;
		if (std::sqrt(dx * dx + dy * dy) > threshold) {
			a_actor->SetPosition(desired, true);
		}
	};

	const RE::NiPoint3 playerTarget = _center - _axis * half + _axis * a_meterOffset;
	const RE::NiPoint3 npcTarget = _center + _axis * half + _axis * a_meterOffset;
	move(a_player, _playerStart, playerTarget);
	move(a_npc, _npcStart, npcTarget);
}

// ---------------------------------------------------------------------------
// Weapon geometry
// ---------------------------------------------------------------------------

float ClashController::StandoffDistance() const
{
	const auto settings = Settings::GetSingleton();
	if (settings->solveClashDistance && _solvedDistance > 0.0f) {
		return _solvedDistance;
	}
	return settings->clashDistance;
}

// Both blades off their meshes, and the point where they are closest. Every
// frame: the blades move with the pose and with the meter, so the contact
// point has to follow them.
void ClashController::MeasureBlades(RE::Actor* a_player, RE::Actor* a_npc)
{
	_blades = {};
	_contactValid = false;
	if (!a_player || !a_npc) {
		return;
	}

	_blades.playerOrigin = a_player->GetPosition();
	_blades.npcOrigin = a_npc->GetPosition();
	_blades.player = BladeGeometry::Measure(a_player, _blades.npcOrigin);
	_blades.npc = BladeGeometry::Measure(a_npc, _blades.playerOrigin);
	_blades.valid = _blades.player.valid && _blades.npc.valid;
	if (!_blades.valid) {
		return;
	}

	RE::NiPoint3 onPlayer;
	RE::NiPoint3 onNpc;
	_bladeGap = BladeGeometry::Gap(_blades.player, _blades.npc, onPlayer, onNpc);
	_contactPoint = (onPlayer + onNpc) * 0.5f;
	_contactValid = true;
}

// The gap between the two measured blades if the pair stood a_distance apart.
// Headings are locked, so the blades are only translated and a measurement
// taken where the actors stand now still holds once they are slid. The meter
// offset moves both by the same amount and cancels, so it is left out.
float ClashController::GapAt(float a_distance, RE::NiPoint3* a_onPlayer, RE::NiPoint3* a_onNpc) const
{
	const float        half = a_distance * 0.5f;
	const RE::NiPoint3 playerTarget = _center - _axis * half;
	const RE::NiPoint3 npcTarget = _center + _axis * half;

	// PositionActors writes X and Y only; the character controller owns Z.
	const auto flat = [](RE::NiPoint3 a_offset) {
		a_offset.z = 0.0f;
		return a_offset;
	};

	RE::NiPoint3 onPlayer;
	RE::NiPoint3 onNpc;
	const float  gap = BladeGeometry::Gap(
        _blades.player.Translated(flat(playerTarget - _blades.playerOrigin)),
        _blades.npc.Translated(flat(npcTarget - _blades.npcOrigin)),
        onPlayer, onNpc);
	if (a_onPlayer) {
		*a_onPlayer = onPlayer;
	}
	if (a_onNpc) {
		*a_onNpc = onNpc;
	}
	return gap;
}

// The separation at which the blades meet, within the configured range.
//
// The gap is convex in the separation (distance between convex sets is convex
// in their relative translation), so the closest the blades come is one
// ternary search and the widest separation still touching is a bisection above
// it. Widest rather than closest, to clip as little as contact allows.
//
// False when nothing in range brings them together: that miss is sideways or
// vertical, which sliding along the axis cannot fix. fClashDistance stands.
bool ClashController::SolveClashDistance(bool a_log, SolveResult& a_result)
{
	const auto settings = Settings::GetSingleton();
	a_result = {};
	if (!_blades.valid) {
		_solvedDistance = 0.0f;
		if (a_log) {
			logger::info("Clash distance: no weapon mesh to measure on {}; keeping fClashDistance {:.0f}",
				_blades.player.valid ? "the opponent" : "the player", settings->clashDistance);
		}
		return false;
	}

	constexpr float kTouching = 0.5f;  // units: closer together than a blade is thick

	const float low = settings->solveDistanceMin;
	const float high = std::max(low, settings->solveDistanceMax);

	float a = low;
	float b = high;
	for (int i = 0; i < 40 && b - a > 0.05f; ++i) {
		const float m1 = a + (b - a) / 3.0f;
		const float m2 = b - (b - a) / 3.0f;
		if (GapAt(m1) <= GapAt(m2)) {
			b = m2;
		} else {
			a = m1;
		}
	}
	const float  closest = (a + b) * 0.5f;
	RE::NiPoint3 onPlayer;
	RE::NiPoint3 onNpc;
	const float  gap = GapAt(closest, &onPlayer, &onNpc);

	a_result.distance = closest;
	a_result.gap = gap;
	a_result.onPlayer = onPlayer;
	a_result.onNpc = onNpc;

	if (gap > kTouching) {
		_solvedDistance = 0.0f;
		if (a_log) {
			// Split the miss along the pair's own frame: only the first
			// component is ours to close.
			const RE::NiPoint3 perpendicular{ -_axis.y, _axis.x, 0.0f };
			const auto         delta = onNpc - onPlayer;
			logger::info("Clash distance: the blades stay {:.1f} units apart at best ({:.1f} along the axis, {:.1f} sideways, {:.1f} vertical); keeping fClashDistance {:.0f}",
				gap, std::abs(delta.Dot(_axis)), std::abs(delta.Dot(perpendicular)), std::abs(delta.z), settings->clashDistance);
		}
		return false;
	}

	float touching = high;
	if (GapAt(high) > kTouching) {
		float lo = closest;
		float hi = high;
		for (int i = 0; i < 24 && hi - lo > 0.05f; ++i) {
			const float mid = (lo + hi) * 0.5f;
			if (GapAt(mid) <= kTouching) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		touching = lo;
	}

	_solvedDistance = std::clamp(touching - settings->solveBite, low, high);
	a_result.touching = true;
	a_result.distance = _solvedDistance;
	a_result.gap = GapAt(_solvedDistance, &a_result.onPlayer, &a_result.onNpc);
	if (a_log) {
		logger::info("Clash distance solved: {:.0f} units (fClashDistance {:.0f}); blades {:.0f} and {:.0f} units long, touching from {:.0f} units apart",
			_solvedDistance, settings->clashDistance, _blades.player.Length(), _blades.npc.Length(), touching);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Height correction
// ---------------------------------------------------------------------------

namespace
{
	[[nodiscard]] float WrapAngle(float a_angle)
	{
		constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
		while (a_angle > std::numbers::pi_v<float>) {
			a_angle -= kTwoPi;
		}
		while (a_angle < -std::numbers::pi_v<float>) {
			a_angle += kTwoPi;
		}
		return a_angle;
	}

	// A point turned about a horizontal axis reaches height A cos t + B sin t
	// above the pivot (horizontal is what kills the third Rodrigues term), so
	// the angle landing it at a given height is one asin. Of the two roots the
	// smaller turn keeps the pose. False if out of reach or on the axis.
	[[nodiscard]] bool SolvePitch(const RE::NiPoint3& a_pivot, const RE::NiPoint3& a_axis, const RE::NiPoint3& a_point,
		float a_targetZ, float& a_angle)
	{
		const auto  arm = a_point - a_pivot;
		const float cosTerm = arm.z;
		const float sinTerm = a_axis.Cross(arm).z;
		const float radius = std::sqrt(cosTerm * cosTerm + sinTerm * sinTerm);
		const float wanted = a_targetZ - a_pivot.z;
		if (radius < 1.0f || std::abs(wanted) > radius) {
			return false;
		}

		const float phase = std::atan2(cosTerm, sinTerm);
		const float base = std::asin(std::clamp(wanted / radius, -1.0f, 1.0f));
		const float first = WrapAngle(base - phase);
		const float second = WrapAngle(std::numbers::pi_v<float> - base - phase);
		a_angle = std::abs(first) <= std::abs(second) ? first : second;
		return true;
	}

	// Rodrigues by hand rather than NiMatrix3::MakeRotation, so the handedness
	// provably matches the one SolvePitch solved in.
	[[nodiscard]] RE::NiMatrix3 AxisAngle(const RE::NiPoint3& a_axis, float a_angle)
	{
		const float c = std::cos(a_angle);
		const float s = std::sin(a_angle);
		const float k = 1.0f - c;
		const float x = a_axis.x;
		const float y = a_axis.y;
		const float z = a_axis.z;

		RE::NiMatrix3 rotation;
		rotation.entry[0][0] = c + k * x * x;
		rotation.entry[0][1] = k * x * y - s * z;
		rotation.entry[0][2] = k * x * z + s * y;
		rotation.entry[1][0] = k * y * x + s * z;
		rotation.entry[1][1] = c + k * y * y;
		rotation.entry[1][2] = k * y * z - s * x;
		rotation.entry[2][0] = k * z * x - s * y;
		rotation.entry[2][1] = k * z * y + s * x;
		rotation.entry[2][2] = c + k * z * z;
		return rotation;
	}

	[[nodiscard]] float MoveToward(float a_value, float a_target, float a_step)
	{
		const float delta = a_target - a_value;
		if (std::abs(delta) <= a_step) {
			return a_target;
		}
		return a_value + (delta > 0.0f ? a_step : -a_step);
	}

	// Turning a blade about its grip carries a point along (axis x arm), so the
	// axis sending it straight out of a body is perpendicular to both, and the
	// angle is the distance over how fast that turn carries it there. First
	// order, which is all a loop that measures again next frame needs.
	[[nodiscard]] bool SolveClearance(const RE::NiPoint3& a_pivot, const RE::NiPoint3& a_point, const RE::NiPoint3& a_push,
		float a_distance, RE::NiPoint3& a_axis, float& a_angle)
	{
		const auto arm = a_point - a_pivot;
		if (arm.Length() < 5.0f) {
			return false;  // the deepest point is the grip itself: the hand is in them, not the blade
		}

		auto        axis = arm.Cross(a_push);
		const float length = axis.Length();
		if (length < 1e-3f) {
			return false;  // the way out runs along the blade; no turn about the grip goes there
		}
		axis /= length;

		const float rate = axis.Cross(arm).Dot(a_push);  // units the point travels per radian
		if (std::abs(rate) < 1.0f) {
			return false;
		}
		a_axis = axis;
		a_angle = a_distance / rate;
		return true;
	}

	// Best first: Spine1 is the chest, far enough from the blade that a couple
	// of degrees move it without the torso reading as a hunch.
	constexpr const char* kSpineNodes[] = { "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Spine [Spn0]" };

	constexpr float kMinTiltGap = 2.0f;  // vertical miss not worth bending for
}


namespace
{
	// Turn a node about its own origin by a world-space rotation, and put its
	// subtree's world transforms back in step. Joints are written outermost
	// first so that an inner one's parent already carries the outer turn.

	// Teardown may outlive the handles; a plan keeps only the FormID, and the
	// writes went to the third-person skeleton.
	[[nodiscard]] RE::NiAVObject* ThirdPersonRoot(RE::FormID a_id)
	{
		if (a_id == 0) {
			return nullptr;
		}
		const auto actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
		return actor ? actor->Get3D(false) : nullptr;
	}

	void RefreshNode(RE::NiAVObject* a_node)
	{
		RE::NiUpdateData data{};
		data.flags = RE::NiUpdateData::Flag::kDirty;
		a_node->Update(data);
	}

	// Put a node back as it was found, if our write is still the last thing in
	// it. See ClashController::NodeTurn for why.
	void RestoreNode(RE::NiAVObject* a_root, const char* a_name, ClashController::NodeTurn& a_turn)
	{
		if (!a_turn.valid) {
			return;
		}
		a_turn.valid = false;
		if (!a_root || !a_name) {
			return;
		}
		const auto node = a_root->GetObjectByName(a_name);
		if (!node || !(node->local.rotate == a_turn.written)) {
			return;  // something has driven it since; it is not ours to put back
		}
		node->local.rotate = a_turn.input;
		RefreshNode(node);
	}

	void TurnNode(RE::NiAVObject* a_root, const char* a_name, const RE::NiPoint3& a_axis, float a_angle, ClashController::NodeTurn& a_turn)
	{
		if (!a_root || !a_name) {
			a_turn.valid = false;
			return;
		}
		if (a_angle == 0.0f) {
			RestoreNode(a_root, a_name, a_turn);
			return;
		}
		const auto node = a_root->GetObjectByName(a_name);
		if (!node || !node->parent) {
			a_turn.valid = false;
			return;
		}

		// Unchanged since our last write means nothing drives this node, so
		// turn from what was there before rather than from our own output.
		auto input = node->local.rotate;
		if (a_turn.valid && input == a_turn.written) {
			input = a_turn.input;
		}

		const auto parentRotation = node->parent->world.rotate;
		node->local.rotate = parentRotation.Transpose() * AxisAngle(a_axis, a_angle) * parentRotation * input;
		a_turn.input = input;
		a_turn.written = node->local.rotate;
		a_turn.valid = true;

		RefreshNode(node);
	}
}

void ClashController::ClearTilt()
{
	_tiltValid.store(false, std::memory_order_release);
	_tiltApplied.store(false, std::memory_order_relaxed);
	_tiltResolved = false;
	if (_tilt.spineTurn.valid || _tilt.armTurn.valid) {
		// Both are animated bones, so this usually finds nothing left to undo.
		// One comparison, and the rule stays the same for every node we write.
		const auto root = ThirdPersonRoot(_tilt.actor);
		RestoreNode(root, _tilt.spineNode, _tilt.spineTurn);
		RestoreNode(root, _tilt.armNode, _tilt.armTurn);
	}
	_tilt = {};
}

// The solve could not bring the blades together and what is left is mostly
// vertical, so bend the higher one down to the other. Each joint is solved
// from the same untilted pose for half the drop; applied in turn they compose
// to the whole of it.
void ClashController::PlanTilt(const SolveResult& a_result, RE::Actor* a_player, RE::Actor* a_npc)
{
	ClearTilt();

	const auto settings = Settings::GetSingleton();
	if (!settings->tiltToMeet || settings->tiltMaxDegrees <= 0.0f || !_blades.valid || !a_player || !a_npc) {
		return;
	}

	const float vertical = a_result.onNpc.z - a_result.onPlayer.z;
	if (std::abs(vertical) < kMinTiltGap) {
		return;  // the miss is sideways or along the axis; a pitch reaches neither
	}

	const bool  playerIsHigh = vertical < 0.0f;
	RE::Actor*  high = playerIsHigh ? a_player : a_npc;
	const auto& blade = playerIsHigh ? _blades.player : _blades.npc;
	const auto  point = playerIsHigh ? a_result.onPlayer : a_result.onNpc;
	const float drop = std::abs(vertical);
	// Each joint is solved from this same pose for half the drop; applied one
	// after the other they compose to the whole of it.
	const float halfTarget = point.z - drop * 0.5f;
	const float wholeTarget = point.z - drop;

	const auto root = high->Get3D(false);
	if (!root) {
		return;
	}

	// The closest points came back in the frame the pair will stand in, so
	// the pivots have to be moved into it too.
	auto offset = (playerIsHigh ? _center - _axis * (a_result.distance * 0.5f) : _center + _axis * (a_result.distance * 0.5f)) -
	              (playerIsHigh ? _blades.playerOrigin : _blades.npcOrigin);
	offset.z = 0.0f;

	const RE::NiPoint3 axis{ -_axis.y, _axis.x, 0.0f };
	const float        limit = settings->tiltMaxDegrees * std::numbers::pi_v<float> / 180.0f;

	const auto solveJoint = [&](const char* a_name, float a_targetZ, float& a_angle) -> bool {
		const auto node = root->GetObjectByName(a_name);
		if (!node || !node->parent) {
			return false;
		}
		float angle = 0.0f;
		if (!SolvePitch(node->world.translate + offset, axis, point, a_targetZ, angle)) {
			return false;
		}
		a_angle = std::clamp(angle, -limit, limit);
		return true;
	};

	for (const auto name : kSpineNodes) {
		if (solveJoint(name, halfTarget, _tilt.spineAngle)) {
			_tilt.spineNode = name;
			break;
		}
	}
	const char* armNode = blade.leftHand ? "NPC L UpperArm [LUar]" : "NPC R UpperArm [RUar]";
	if (solveJoint(armNode, halfTarget, _tilt.armAngle)) {
		_tilt.armNode = armNode;
	}

	// A joint this skeleton has not got leaves the other one to do all of it.
	if (!_tilt.spineNode && _tilt.armNode) {
		solveJoint(_tilt.armNode, wholeTarget, _tilt.armAngle);
	} else if (!_tilt.armNode && _tilt.spineNode) {
		solveJoint(_tilt.spineNode, wholeTarget, _tilt.spineAngle);
	}

	if (!_tilt.spineNode && !_tilt.armNode) {
		logger::info("Clash tilt: no spine or shoulder node on {}'s skeleton; the {:.0f}-unit height gap stands",
			high->GetName(), drop);
		return;
	}

	// Committed together with the plan: stand at the separation that leaves
	// only the vertical gap, then bend that away. Holding fClashDistance here
	// would leave a gap along the axis as well, and no amount of bending
	// reaches that one.
	_solvedDistance = a_result.distance;
	_tilt.actor = high->GetFormID();
	_tilt.axis = axis;
	_tiltValid.store(true, std::memory_order_release);

	constexpr float kToDegrees = 180.0f / std::numbers::pi_v<float>;
	logger::info("Clash tilt: bending {} down to close a {:.0f}-unit height gap ({:.1f} deg at {}, {:.1f} deg at the {} shoulder), standing {:.0f} apart",
		high->GetName(), drop, _tilt.spineAngle * kToDegrees, _tilt.spineNode ? _tilt.spineNode : "no spine node",
		_tilt.armAngle * kToDegrees, blade.leftHand ? "left" : "right", _solvedDistance);
}

// Runs right after the animation update has written this actor's pose, so
// everything here lands on top of it.
void ClashController::ApplyPoseFixups(RE::Actor* a_actor)
{
	if (!a_actor) {
		return;
	}
	const bool tilt = _tiltValid.load(std::memory_order_acquire) && a_actor->GetFormID() == _tilt.actor;
	const bool clearance = _clearanceValid.load(std::memory_order_acquire);
	if (!tilt && !clearance) {
		return;
	}
	const auto root = a_actor->Get3D(false);
	if (!root) {
		return;
	}

	// Spine and shoulder first, wrist and weapon after: the order the joints
	// come in down the arm.
	if (tilt) {
		ApplyTilt(a_actor, root);
	}
	if (clearance) {
		ApplyClearance(a_actor, root);
	}
}

void ClashController::ApplyTilt(RE::Actor*, RE::NiAVObject* a_root)
{
	TurnNode(a_root, _tilt.spineNode, _tilt.axis, _tilt.spineAngle, _tilt.spineTurn);
	TurnNode(a_root, _tilt.armNode, _tilt.axis, _tilt.armAngle, _tilt.armTurn);
	_tiltApplied.store(true, std::memory_order_release);
}

void ClashController::ApplyClearance(RE::Actor* a_actor, RE::NiAVObject* a_root)
{
	const auto id = a_actor->GetFormID();
	for (auto& clearance : _clearance) {
		if (clearance.actor != id) {
			continue;
		}
		// The wrist takes what it is allowed, since the hand turns with it; only
		// the remainder goes to the weapon, where the handle turns inside a fist
		// that stays put. Zero still comes through, to put the weapon node back.
		const float limit = Settings::GetSingleton()->clearanceMaxDegrees * std::numbers::pi_v<float> / 180.0f;
		const float wrist = std::clamp(clearance.angle, -limit, limit);
		const float weapon = std::clamp(clearance.angle - wrist, -limit, limit);
		TurnNode(a_root, clearance.leftHand ? "NPC L Hand [LHand]" : "NPC R Hand [RHand]", clearance.axis, wrist, clearance.handTurn);
		TurnNode(a_root, clearance.leftHand ? "SHIELD" : "WEAPON", clearance.axis, weapon, clearance.weaponTurn);
		return;
	}
}

// A loop, not a solve: measured each frame off blades already carrying the
// last correction, wound on while the blade is inside the other fighter, held
// through a deadband, eased back to nothing once it is clear.
void ClashController::UpdateClearance(RE::Actor* a_player, RE::Actor* a_npc, float a_dt)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->clearWeaponClipping || settings->clearanceMaxDegrees <= 0.0f || !_blades.valid) {
		ClearClearance();
		return;
	}

	constexpr float kMargin = 1.5f;   // clear the body by this much
	constexpr float kRelease = 4.0f;  // and by this much before easing off again
	const float     step = 2.5f * a_dt;
	// Both joints take up to the configured turn, so the total is twice it.
	const float limit = 2.0f * settings->clearanceMaxDegrees * std::numbers::pi_v<float> / 180.0f;

	struct Side
	{
		RE::Actor*                  actor;
		RE::Actor*                  opponent;
		const BladeGeometry::Blade& blade;
	};
	const Side sides[2] = {
		{ a_player, a_npc, _blades.player },
		{ a_npc, a_player, _blades.npc },
	};

	bool any = false;
	for (std::size_t i = 0; i < std::size(sides); ++i) {
		auto&      clearance = _clearance[i];
		const auto body = BladeGeometry::MeasureBody(sides[i].opponent, settings->clearanceBodyRadius);

		RE::NiPoint3 onBlade;
		RE::NiPoint3 push;
		const float  depth = BladeGeometry::Penetration(sides[i].blade, body, onBlade, push);
		_clearanceDepth[i] = depth;

		float target = clearance.angle;
		if (depth > 0.0f) {
			// Still inside them. An axis in use is kept and only its angle
			// adjusted: the angle means nothing without the axis it was wound
			// about, so swapping one in under an applied correction would snap
			// the blade. The adjustment is signed, so an axis turning the wrong
			// way unwinds itself and carries on through zero. One that cannot
			// move the point out is dropped, but only once eased back to zero.
			const auto  arm = onBlade - sides[i].blade.grip;
			const float rate = clearance.angle != 0.0f ? clearance.axis.Cross(arm).Dot(push) : 0.0f;
			if (std::abs(rate) >= 1.0f) {
				target = clearance.angle + (depth + kMargin) / rate;
			} else if (clearance.angle != 0.0f) {
				target = 0.0f;
			} else {
				RE::NiPoint3 axis;
				float        extra = 0.0f;
				if (SolveClearance(sides[i].blade.grip, onBlade, push, depth + kMargin, axis, extra)) {
					clearance.axis = axis;
					target = extra;
				}
			}
		} else if (depth < -kRelease) {
			target = 0.0f;  // clear with room to spare: hand it back to the animation
		}

		target = std::clamp(target, -limit, limit);
		clearance.angle = MoveToward(clearance.angle, target, step);
		clearance.actor = sides[i].actor->GetFormID();
		clearance.leftHand = sides[i].blade.leftHand;
		// Zero still needs one more pass through the hook to put the weapon node
		// back, so an outstanding write counts as work.
		any = any || clearance.angle != 0.0f || clearance.handTurn.valid || clearance.weaponTurn.valid;
	}

	_clearanceValid.store(any, std::memory_order_release);
}

void ClashController::ClearClearance()
{
	_clearanceValid.store(false, std::memory_order_release);
	for (auto& clearance : _clearance) {
		// The weapon node is not animated: left alone it keeps the last turn
		// for as long as the weapon stays equipped.
		if (clearance.handTurn.valid || clearance.weaponTurn.valid) {
			const auto root = ThirdPersonRoot(clearance.actor);
			RestoreNode(root, clearance.leftHand ? "NPC L Hand [LHand]" : "NPC R Hand [RHand]", clearance.handTurn);
			RestoreNode(root, clearance.leftHand ? "SHIELD" : "WEAPON", clearance.weaponTurn);
		}
		clearance = {};
	}
	_clearanceDepth[0] = 0.0f;
	_clearanceDepth[1] = 0.0f;
}

void ClashController::FaceEachOther(RE::Actor* a_player, RE::Actor* a_npc)
{
	const auto playerPos = a_player->GetPosition();
	const auto npcPos = a_npc->GetPosition();

	if (!_settled) {
		_playerHeading = HeadingTo(playerPos, npcPos);
		_npcHeading = HeadingTo(npcPos, playerPos);
		a_player->SetHeading(_playerHeading);
		a_npc->SetHeading(_npcHeading);
		ClashTDM::GetSingleton()->SetYaw(_playerHeading);
	} else {
		ClashTDM::GetSingleton()->SetYaw(_playerHeading);
		// Past the settle window the headings are held, not merely left alone:
		// with a weapon drawn the engine keeps turning the player to the
		// camera's yaw (which our aim offsets), and head tracking can turn the
		// NPC, so anything that drifts is put straight back.
		constexpr float kTolerance = 0.5f * std::numbers::pi_v<float> / 180.0f;
		const auto      correct = [this](RE::Actor* a_actor, float a_locked) {
            float diff = a_actor->GetAngleZ() - a_locked;
            while (diff > std::numbers::pi_v<float>) {
                diff -= 2.0f * std::numbers::pi_v<float>;
            }
            while (diff < -std::numbers::pi_v<float>) {
                diff += 2.0f * std::numbers::pi_v<float>;
            }
            if (std::abs(diff) > kTolerance) {
                a_actor->SetHeading(a_locked);
                if (Settings::GetSingleton()->debugLog && std::abs(diff) > 4.0f * kTolerance) {
                    logger::debug("Heading: {} drifted {:.1f} deg; corrected", a_actor->GetName(), diff * 180.0f / std::numbers::pi_v<float>);
                }
            }
		};
		// Only the player is corrected. SetHeading has no effect on the
		// restrained opponent (its movement controller owns its rotation and
		// the angle reads unchanged right after the write), so correcting it
		// every frame only feeds the movement system.
		correct(a_player, _playerHeading);

		LockUpperBody(a_player);
		LockUpperBody(a_npc);
	}

	// Seen from the player's own eyes, the view is whatever pitch the swing
	// was made at, which can be the floor or the sky. Ease it level over the
	// settle window (the opponent's face is dead ahead at clash distance) and
	// hold it there; looking is off, so nothing else moves it. The
	// first-person camera reads the pitch from the actor, and so does the
	// first-person torso bend, so the block pose settles with the view.
	if (_firstPersonClash) {
		const float settle = Settings::GetSingleton()->settleTime;
		const float t = settle > 0.0f ? SmoothStep(_clashTime / settle) : 1.0f;
		a_player->data.angle.x = _playerStartPitch * (1.0f - t);
	}

	// Sliding the actors makes the graph read movement and blend a walk into
	// the block. Zero the locomotion inputs after each move so the legs stay
	// planted; the engine rewrites them next frame, and so do we.
	for (auto actor : { a_player, a_npc }) {
		actor->SetGraphVariableFloat("Speed", 0.0f);
		actor->SetGraphVariableFloat("SpeedSampled", 0.0f);
		actor->SetGraphVariableFloat("Direction", 0.0f);
		actor->SetGraphVariableFloat("TurnDelta", 0.0f);
	}

	// Keep hidden shields hidden; equipment nodes can get re-enabled.
	if (_playerShieldHidden) {
		SetShieldVisible(a_player, false, false);
	}
	if (_npcShieldHidden) {
		SetShieldVisible(a_npc, false, false);
	}
}

void ClashController::SendEvent(RE::Actor* a_actor, const char* a_event)
{
	gSendingOwnEvent = true;
	a_actor->NotifyAnimationGraph(a_event);
	gSendingOwnEvent = false;
}

bool ClashController::AllowGraphEvent(RE::IAnimationGraphManagerHolder* a_holder, const RE::BSFixedString& a_event) const
{
	if (gSendingOwnEvent) {
		return true;
	}
	if (_phase != ClashPhase::kApproach && _phase != ClashPhase::kStandoff) {
		return true;
	}
	if (a_holder != _playerHolder && a_holder != _npcHolder) {
		return true;
	}
	const bool  debug = Settings::GetSingleton()->debugLog;
	const char* who = a_holder == _playerHolder ? "player" : "opponent";

	// Actor updates run on a thread pool, so the sender cannot be told from
	// the thread. Filter by name instead: anything that would start a
	// different animation on top of the block is dropped (AI attacks, bashes,
	// Block Overhaul's anticipation and hit-reaction states, movement starts,
	// forced idles, a block stop that is not ours). Everything else, including
	// the graph's own housekeeping events (IdleStop, WeapEquip, CyclicFreeze,
	// moveStop, turnStop, ...), passes so no transition stalls.
	static constexpr std::string_view kDeniedPrefixes[] = {
		"attackStart", "attackPowerStart", "attackRelease",
		"bashStart", "bashRelease", "blockBash",
		"blockAnticipateStart", "blockHitStart", "BlockAndSlash",
		"blockStop", "blockStopInstant",
		"recoilStart", "staggerStart",
		"IdleForceDefaultState",
		"moveStart", "turnLeft", "turnRight", "turnStart",
		"JumpStandingStart", "JumpDirectionalStart", "SprintStart", "SneakStart",
		"weaponSheathe", "WeapUnequip", "Unequip",
	};

	const std::string_view name{ a_event.c_str() ? a_event.c_str() : "" };
	for (const auto prefix : kDeniedPrefixes) {
		if (name.size() >= prefix.size() && _strnicmp(name.data(), prefix.data(), prefix.size()) == 0) {
			if (debug) {
				logger::debug("Graph: blocked event '{}' for the {} during the clash", name, who);
			}
			return false;
		}
	}
	if (debug) {
		logger::debug("Graph: passed event '{}' for the {}", name, who);
	}
	return true;
}

void ClashController::OnCharacterUpdated(RE::Character* a_character)
{
	if (!a_character || _npcID == 0 || a_character->GetFormID() != _npcID) {
		return;
	}
	if (_phase != ClashPhase::kApproach && _phase != ClashPhase::kStandoff) {
		return;
	}
	a_character->SetGraphVariableFloat("Speed", 0.0f);
	a_character->SetGraphVariableFloat("SpeedSampled", 0.0f);
	a_character->SetGraphVariableFloat("Direction", 0.0f);
	a_character->SetGraphVariableFloat("TurnDelta", 0.0f);

	if (_blockSent) {
		HoldBlock(a_character, _npcBlock, RealDelta(RE::GetSecondsSinceLastFrame()));
	}
}

void ClashController::HoldBlock(RE::Actor* a_actor, BlockHold& a_hold, float a_dt)
{
	if (_frozen) {
		return;  // nothing moves; nothing to re-assert
	}

	// An attack still on the books keeps the graph out of the block, and for
	// an opponent asking again does not help: MCO gates the way out on
	// blockStart to the player, so a retry sending only blockStart can never
	// recover an NPC back in a swing. Hence the block counts as held only once
	// the attack state has cleared, and every retry cancels again.
	//
	// After half a second the gate is given up on; there is nothing stronger
	// to send. IdleForceDefaultState would end the attack, but its default
	// state is the unarmed one and the graph leaves it believing the hands are
	// empty while the weapon is still drawn -- see CancelAttack.
	const bool stillAttacking = a_actor->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
	a_hold.attackTime = stillAttacking ? a_hold.attackTime + a_dt : 0.0f;
	a_hold.cancelTimer += a_dt;
	const bool attacking = stillAttacking && a_hold.attackTime < 0.5f;

	// MCO_EndAnimation blends over 0.2s and self-transitions, so a cancel
	// re-sent inside that window restarts its own blend and holds the swing
	// longer. The block may be re-asserted freely; the cancel waits.
	const auto cancel = [&] {
		if (a_hold.cancelTimer >= 0.25f) {
			a_hold.cancelTimer = 0.0f;
			CancelAttack(a_actor);
		}
	};

	if (stillAttacking && !attacking && !a_hold.gaveUpOnAttack) {
		a_hold.gaveUpOnAttack = true;
		logger::info("Block hold: {}'s attack state has not cleared in {:.1f}s; holding the block on its own from here",
			a_actor->GetName(), a_hold.attackTime);
	}

	if (a_actor->IsBlocking() && !attacking) {
		a_hold.established = true;
		a_hold.everEstablished = true;
		return;
	}

	if (a_hold.established) {
		// The graph left the block: re-enter straight away (the raise has to
		// replay from wherever it is anyway), then hold off for a moment so a
		// raise in progress is never restarted every frame.
		a_hold.established = false;
		a_hold.resendTimer = 0.0f;
		if (attacking) {
			cancel();
		}
		SendBlock(a_actor);
		logger::debug("Block hold on {} lost{}; re-entering", a_actor->GetName(), attacking ? " to an attack" : "");
		return;
	}

	// Until the block has been seen once, the graph may still be leaving the
	// cancelled state, so the retry is quick; a lost hold later is re-sent
	// on a longer cooldown so a raise in progress is not restarted.
	a_hold.resendTimer += a_dt;
	if (a_hold.resendTimer < (a_hold.everEstablished ? 0.35f : 0.1f)) {
		return;
	}
	a_hold.resendTimer = 0.0f;

	if (attacking) {
		cancel();
	}
	SendBlock(a_actor);
}

// Interrupt whatever the actor is playing and enter the block stance in the
// same graph update, in this order:
//   iWantBlock = 1      the graph's block intent. The vanilla attackStop
//                       transition lands directly in the block state when this
//                       is set (and no bow/crossbow is held), for NPCs as well.
//   MCO_EndAnimation    Attack MCO-DXP's own global wildcard: any of its attack,
//                       combo, recovery or transition states straight back to
//                       idle, unconditionally. Ignored when MCO is not present.
//   attackStop          the graph's attack interrupt (vanilla, MCO and BFCO all
//                       end their attacks on it); into block via iWantBlock.
//   recoilStop / staggerStop
//                       out of the weapon parry mod's recoil or a stagger.
//   blockStart          the block itself, for whichever state is left.
// The graph's own interrupt events are used rather than IdleForceDefaultState,
// which resets the drawn-weapon state and leaves the actor unable to block now
// or attack afterwards. iWantBlock is cleared again by SendBlockStop.
//
// Everything but the block itself is CancelAttack, which the per-frame hold
// re-sends while an attack refuses to clear: one shot landing in the wrong
// graph update is the difference between a standoff and an opponent swinging
// through it. MCO_EndAnimation is the opponent's only way out of an MCO
// attack -- the transition leaving one on blockStart is conditioned on
// (IsNPC == 0) && (MCO_bEnableBlockCancel). That variable is left alone: it
// would outlive the clash and hand the player a block-cancel they never asked
// for.
void ClashController::CancelAttack(RE::Actor* a_actor)
{
	a_actor->SetGraphVariableInt("iWantBlock", 1);
	SendEvent(a_actor, "MCO_EndAnimation");
	SendEvent(a_actor, "attackStop");
	SendEvent(a_actor, "recoilStop");
	SendEvent(a_actor, "staggerStop");
	// Clear the parry mod's flag in case its post-hit reset was skipped by
	// our hit swallow.
	a_actor->SetGraphVariableBool("bMaxsuWeaponParry_InWeaponParry", false);
}

void ClashController::CancelAndBlock(RE::Actor* a_actor)
{
	CancelAttack(a_actor);
	SendBlock(a_actor);
}

void ClashController::SendBlock(RE::Actor* a_actor)
{
	// Only the event. The graph sets IsBlocking itself when it enters the
	// block state; pre-setting the variable made the graph treat the actor as
	// already blocking, so it never played the transition, and our re-assert
	// saw IsBlocking() as true and never tried again.
	SendEvent(a_actor, "blockStart");
}

void ClashController::SendBlockStop(RE::Actor* a_actor)
{
	a_actor->SetGraphVariableInt("iWantBlock", 0);
	SendEvent(a_actor, "blockStop");
	a_actor->SetGraphVariableBool("IsBlocking", false);
}

void ClashController::SendStagger(RE::Actor* a_actor, float a_magnitude)
{
	// staggerDirection 0 = hit from the front, so the actor reels backwards.
	a_actor->SetGraphVariableFloat("staggerDirection", 0.0f);
	a_actor->SetGraphVariableFloat("staggerMagnitude", a_magnitude);
	SendEvent(a_actor, "staggerStart");
}

// Picks the particle model for the sparks. The weapon's impact data set has an
// entry for the "blocked by a blade" materials, which is exactly what vanilla
// plays when a swing meets a blocking weapon; metal materials are the fallback.
void ClashController::ResolveSparkModel(RE::Actor* a_player, RE::Actor* a_npc)
{
	const auto settings = Settings::GetSingleton();
	_sparkModel.clear();
	_sparkDuration = 0.5f;
	_blockImpact = nullptr;

	constexpr RE::MATERIAL_ID kCandidates[] = {
		RE::MATERIAL_ID::kBlockBlade1Hand,
		RE::MATERIAL_ID::kBlockBlade2Hand,
		RE::MATERIAL_ID::kMetalSolid,
		RE::MATERIAL_ID::kMetalLight,
	};

	for (auto actor : { a_player, a_npc }) {
		for (bool leftHand : { false, true }) {
			const auto form = actor->GetEquippedObject(leftHand);
			const auto weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
			if (!weapon || !weapon->impactDataSet) {
				continue;
			}
			for (const auto id : kCandidates) {
				const auto material = RE::BGSMaterialType::GetMaterialType(id);
				if (!material) {
					continue;
				}
				const auto it = weapon->impactDataSet->impactMap.find(material);
				if (it == weapon->impactDataSet->impactMap.end() || !it->second) {
					continue;
				}
				const auto impact = it->second;

				// Sounds: the first entry found (the block materials come first,
				// and carry the block clang).
				if (!_blockImpact) {
					_blockImpact = impact;
					logger::debug("Impact data for sound: {}'s weapon, material {}", actor->GetName(), material->materialName.c_str());
				}

				// Sparks: the first entry that actually has a particle model; the
				// block entries often have none, the metal ones do.
				if (settings->sparksEnabled && _sparkModel.empty()) {
					if (const char* model = impact->GetModel(); model && *model) {
						_sparkModel = model;
						if (impact->data.effectDuration > 0.0f) {
							_sparkDuration = impact->data.effectDuration;
						}
						logger::debug("Sparks: using '{}' from {}'s weapon, material {}", _sparkModel, actor->GetName(), material->materialName.c_str());
					}
				}

				if (_blockImpact && (!settings->sparksEnabled || !_sparkModel.empty())) {
					break;
				}
			}
			if (_blockImpact && (!settings->sparksEnabled || !_sparkModel.empty())) {
				break;
			}
		}
		if (_blockImpact && (!settings->sparksEnabled || !_sparkModel.empty())) {
			break;
		}
	}

	if (settings->sparksEnabled && !settings->sparksModel.empty()) {
		_sparkModel = settings->sparksModel;
	}
	if (!_blockImpact) {
		logger::info("No weapon block-impact data found; sounds fall back to INI overrides only");
	}
	if (settings->sparksEnabled && _sparkModel.empty()) {
		logger::info("Sparks: no impact model found on either weapon; set [Sparks] sModel to force one");
	}
}

void ClashController::TickSparks(RE::Actor* a_player, RE::Actor* a_npc, float a_dt, float a_meterOffset)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->sparksEnabled || _sparkModel.empty()) {
		return;
	}

	_sparkTimer += a_dt;
	if (_sparkTimer < settings->sparksInterval) {
		return;
	}
	_sparkTimer = 0.0f;

	const auto cell = a_player->GetParentCell();
	if (!cell) {
		return;
	}

	static std::mt19937                   rng{ std::random_device{}() };
	std::uniform_real_distribution<float> jitter(-settings->sparksJitter, settings->sparksJitter);

	RE::NiPoint3 point;
	if (settings->contactFromWeapons && _contactValid) {
		point = _contactPoint;
	} else {
		const float ground = (a_player->GetPositionZ() + a_npc->GetPositionZ()) * 0.5f;
		point = _center + _axis * a_meterOffset;
		point.z = ground + settings->sparksHeight * a_player->GetScale();
	}
	point.x += jitter(rng);
	point.y += jitter(rng);
	point.z += jitter(rng);

	// Face the burst along the clash axis so the streaks spray sideways.
	const RE::NiPoint3 rotation{ 0.0f, 0.0f, std::atan2(_axis.x, _axis.y) };
	RE::BSTempEffectParticle::Spawn(cell, _sparkDuration, _sparkModel.c_str(), rotation, point, settings->sparksScale, 7, nullptr);
}

namespace
{
	[[nodiscard]] bool HasShieldEquipped(RE::Actor* a_actor)
	{
		const auto left = a_actor->GetEquippedObject(true);
		const auto armor = left ? left->As<RE::TESObjectARMO>() : nullptr;
		return armor && armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
	}

	// Cull and collapse the attachment node and everything under it. The
	// engine can re-enable culling on equipment nodes, so this is re-applied
	// every frame while hidden; the zero scale is belt and braces. The player
	// has a second, first-person model with its own SHIELD node, so both of
	// their skeletons are walked (for an NPC the two lookups are the same
	// root); a clash that stays in first person shows that one.
	void SetShieldVisible(RE::Actor* a_actor, bool a_visible, bool a_log)
	{
		RE::NiAVObject* roots[] = { a_actor->Get3D(false), a_actor->Get3D(true) };
		bool            found = false;
		for (std::size_t i = 0; i < std::size(roots); ++i) {
			const auto root = roots[i];
			if (!root || (i > 0 && root == roots[0])) {
				continue;
			}
			const auto node = root->GetObjectByName("SHIELD");
			if (!node) {
				continue;
			}
			found = true;
			node->SetAppCulled(!a_visible);
			node->local.scale = a_visible ? 1.0f : 0.0f;
			if (const auto asNode = node->AsNode()) {
				for (auto& child : asNode->GetChildren()) {
					if (child) {
						child->SetAppCulled(!a_visible);
						child->local.scale = a_visible ? 1.0f : 0.0f;
					}
				}
			}
			if (a_log) {
				logger::debug("Shield: {} on {}'s {} model ({} children)", a_visible ? "shown" : "hidden", a_actor->GetName(),
					i == 0 ? "third-person" : "first-person", node->AsNode() ? node->AsNode()->GetChildren().size() : 0u);
			}
		}
		if (!found && a_log) {
			logger::warn("Shield: no SHIELD node on {}'s 3D", a_actor->GetName());
		}
	}
}

void ClashController::HideShield(RE::Actor* a_actor, bool& a_hidden)
{
	a_hidden = false;
	const auto settings = Settings::GetSingleton();
	if (!a_actor || settings->shieldMode == 0 || !HasShieldEquipped(a_actor)) {
		return;
	}

	// Only the model is hidden. Which block animation plays is left to the
	// graph: lying about the left-hand type either provokes the engine's
	// equip sync into re-equipping every frame or, if restored, makes the
	// graph re-select the shield block. The shipped Open Animation Replacer
	// folder "Shield To Weapon Block" swaps the shield block files for the
	// one-handed weapon block while CinematicClash_IsInClash is true.
	SetShieldVisible(a_actor, false, true);
	a_hidden = true;
}

void ClashController::RestoreShield(RE::Actor* a_actor, bool& a_hidden)
{
	if (!a_hidden) {
		return;
	}
	a_hidden = false;
	if (!a_actor) {
		return;
	}
	SetShieldVisible(a_actor, true, true);
}

// Head and spine tracking twist the upper body toward a look target every
// frame; with both actors already facing each other that twist only fights
// the block pose. Re-applied every frame while settled because the engine
// turns tracking back on by itself.
void ClashController::LockUpperBody(RE::Actor* a_actor)
{
	a_actor->SetGraphVariableBool("bHeadTrackSpine", false);
	a_actor->SetGraphVariableBool("bHeadTracking", false);
	a_actor->SetGraphVariableFloat("TurnDelta", 0.0f);
}

void ClashController::UnlockUpperBody(RE::Actor* a_actor)
{
	a_actor->SetGraphVariableBool("bHeadTrackSpine", true);
	a_actor->SetGraphVariableBool("bHeadTracking", true);
}

// Two character controllers standing chest to chest push each other every
// physics step; that push is movement the graph reads as walking and turning.
void ClashController::BeginCollisionIgnore(RE::Actor* a_player, RE::Actor* a_npc)
{
	if (!Settings::GetSingleton()->disableCollision || !a_player || !a_npc) {
		return;
	}

	if (ClashDetection::HasPrecision()) {
		RE::CFilter playerFilter{};
		RE::CFilter npcFilter{};
		a_player->GetCollisionFilterInfo(playerFilter);
		a_npc->GetCollisionFilterInfo(npcFilter);
		_playerGroup.store(playerFilter.filter >> 16, std::memory_order_relaxed);
		_npcGroup.store(npcFilter.filter >> 16, std::memory_order_relaxed);
		_ignoreCollision.store(true, std::memory_order_release);
		logger::debug("Collision: ignoring pair via Precision (groups {:#x} / {:#x})", playerFilter.filter >> 16, npcFilter.filter >> 16);
		return;
	}

	// No Precision: the opponent stops colliding with everything for the
	// duration. Coarser, but they are held in place anyway.
	a_npc->SetCollision(false);
	_npcCollisionFlagged = true;
	logger::debug("Collision: opponent's collision disabled for the clash (no Precision)");
}

void ClashController::EndCollisionIgnore()
{
	_ignoreCollision.store(false, std::memory_order_release);
	_playerGroup.store(0, std::memory_order_relaxed);
	_npcGroup.store(0, std::memory_order_relaxed);
	if (_npcCollisionFlagged) {
		_npcCollisionFlagged = false;
		if (const auto npc = _npcHandle.get()) {
			npc->SetCollision(true);
		}
	}
}

// Opt-in: stop the opponent's animation graph where it is by disabling its AI
// (the player's graph is not driven from anything this plugin hooks, so only
// the opponent is affected).
void ClashController::Freeze(RE::Actor*, RE::Actor* a_npc)
{
	_frozen = true;
	if (a_npc) {
		a_npc->EnableAI(false);
		_npcAIDisabledForFreeze = true;
	}
	logger::debug("Frozen: both actors held in the block pose");
}

void ClashController::Unfreeze()
{
	if (!_frozen) {
		return;
	}
	_frozen = false;
	if (_npcAIDisabledForFreeze) {
		_npcAIDisabledForFreeze = false;
		if (const auto npc = _npcHandle.get()) {
			npc->EnableAI(true);
		}
	}
	logger::debug("Unfrozen");
}

void ClashController::LockPlayer(RE::PlayerCharacter* a_player)
{
	BeginPlayerImmunity(a_player);

	const auto controlMap = RE::ControlMap::GetSingleton();
	if (!controlMap) {
		return;
	}

	std::uint32_t enabled = 0;
	std::uint32_t stored = 0;
	controlMap->GetControlsState(enabled, stored);

	// Only re-enable what was enabled before us, so another mod's disable is
	// not undone by our restore.
	_savedControls = enabled & kLockedControls;
	controlMap->ToggleControls(static_cast<UEFlag>(kLockedControls), false, false);
	_controlsLocked = true;
	ClashInput::GetSingleton()->BeginBlocking();
}

void ClashController::UnlockPlayer()
{
	EndPlayerImmunity();

	if (!_controlsLocked) {
		return;
	}
	_controlsLocked = false;

	const auto controlMap = RE::ControlMap::GetSingleton();
	if (controlMap && _savedControls != 0) {
		controlMap->ToggleControls(static_cast<UEFlag>(_savedControls), true, false);
	}
	ClashInput::GetSingleton()->EndBlocking();
}

void ClashController::BeginPlayerImmunity(RE::Actor* a_player)
{
	if (_playerImmune || !a_player) {
		return;
	}
	_playerImmune = true;
	_rejectHostileMagic.store(true, std::memory_order_relaxed);
	_ghostVerifyFrames = 0;

	// Another mod's script may already have ghosted the player; then there is
	// nothing to do and nothing to undo later.
	_playerWasGhost = a_player->IsGhost();
	if (_playerWasGhost) {
		logger::debug("Player is already a ghost; leaving the flag alone");
		return;
	}
	if (SetGhost(a_player, true)) {
		_ghostVerifyFrames = kGhostVerifyFrames;
	} else {
		logger::warn("Could not reach the script VM to ghost the player; only hostile magic is filtered for this clash");
	}
}

void ClashController::EndPlayerImmunity()
{
	if (!_playerImmune) {
		return;
	}
	_playerImmune = false;
	_rejectHostileMagic.store(false, std::memory_order_relaxed);
	_ghostVerifyFrames = 0;
	if (_playerWasGhost) {
		return;
	}
	if (!SetGhost(RE::PlayerCharacter::GetSingleton(), false)) {
		logger::warn("Could not clear the player's ghost flag through the script VM");
	}
}

// SetGhost is asynchronous; confirm it landed, and say so if it never does.
void ClashController::VerifyGhost(RE::Actor* a_player)
{
	if (_ghostVerifyFrames <= 0 || !a_player) {
		return;
	}
	if (a_player->IsGhost()) {
		logger::debug("Player ghosted for the clash");
		_ghostVerifyFrames = 0;
		return;
	}
	if (--_ghostVerifyFrames == 0) {
		logger::warn("The player's ghost flag never took effect; arrows and spells can still land during this clash");
	}
}

void ClashController::RestoreNpcAI()
{
	if (!_npcRestrained) {
		return;
	}
	_npcRestrained = false;
	if (const auto npc = _npcHandle.get()) {
		if (npc->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained) {
			npc->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
		}
	}
}

void ClashController::RestoreNpcMovement()
{
	if (!_npcControlsDriven) {
		return;
	}
	_npcControlsDriven = false;
	if (const auto npc = _npcHandle.get()) {
		if (const auto& controller = npc->GetActorRuntimeData().movementController) {
			controller->SetAIDriven();
		}
	}
}

void ClashController::RestoreTime()
{
	if (!_timeScaled) {
		return;
	}
	_timeScaled = false;
	if (const auto timer = RE::BSTimer::GetSingleton()) {
		timer->SetGlobalTimeMultiplier(1.0f, false);
	}
}

float ClashController::NpcPushRate(RE::Actor* a_player, RE::Actor* a_npc) const
{
	const auto settings = Settings::GetSingleton();

	// The difficulty is stated in presses per second: that many presses
	// exactly cancel the opponent's push when the weapon skills are equal.
	float rate = settings->PressesPerSecond() * settings->pressGain;

	// The skill gap scales the push either way (harder against a better
	// swordsman, easier against a worse one) using the relative gap, so 15
	// against 30 is a bigger deal than 85 against 100.
	const float total = _playerSkill + _npcSkill;
	if (total > 0.0f) {
		rate *= 1.0f + settings->skillInfluence * ((_npcSkill - _playerSkill) / total);
	}
	(void)a_player;

	if (settings->staminaAffectsNpc) {
		rate *= 0.5f + 0.5f * StaminaRatio(a_npc);
	}

	return std::max(0.0f, rate);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ClashController::IsParticipant(const RE::TESObjectREFR* a_refr) const noexcept
{
	if (!a_refr || _playerID == 0) {
		return false;
	}
	const auto id = a_refr->GetFormID();
	return id == _playerID || id == _npcID;
}

bool ClashController::IsInClash(const RE::TESObjectREFR* a_refr) const
{
	return (_phase == ClashPhase::kApproach || _phase == ClashPhase::kStandoff) && IsParticipant(a_refr);
}

bool ClashController::IsWinner(const RE::TESObjectREFR* a_refr) const
{
	if (_outcome == ClashOutcome::kNone || !IsParticipant(a_refr)) {
		return false;
	}
	if (_phase != ClashPhase::kResolve && _phase != ClashPhase::kAftermath) {
		return false;
	}
	if (_outcome == ClashOutcome::kDraw) {
		return false;  // nobody won
	}
	const bool isPlayer = a_refr->GetFormID() == _playerID;
	return (_outcome == ClashOutcome::kPlayerWon) == isPlayer;
}

bool ClashController::IsLoser(const RE::TESObjectREFR* a_refr) const
{
	if (_outcome == ClashOutcome::kNone || !IsParticipant(a_refr)) {
		return false;
	}
	if (_phase != ClashPhase::kResolve && _phase != ClashPhase::kAftermath) {
		return false;
	}
	if (_outcome == ClashOutcome::kDraw) {
		return false;  // nobody lost
	}
	const bool isPlayer = a_refr->GetFormID() == _playerID;
	return (_outcome == ClashOutcome::kPlayerLost) == isPlayer;
}

float ClashController::GetMeterFor(const RE::TESObjectREFR* a_refr) const
{
	if (!IsParticipant(a_refr)) {
		return 0.0f;
	}
	return a_refr->GetFormID() == _playerID ? _meter : 1.0f - _meter;
}

bool ClashController::GetContactPoint(RE::NiPoint3& a_point) const
{
	if (_phase != ClashPhase::kApproach && _phase != ClashPhase::kStandoff) {
		return false;
	}
	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();
	if (!playerPtr || !npcPtr) {
		return false;
	}

	// Where the two blades actually are, when they could be measured.
	if (Settings::GetSingleton()->contactFromWeapons && _contactValid) {
		a_point = _contactPoint;
		return true;
	}

	a_point = _center + _axis * _meterOffset;
	a_point.z = (playerPtr->GetPositionZ() + npcPtr->GetPositionZ()) * 0.5f +
	            ClashCamera::GetSingleton()->Framing().aimHeight * playerPtr->GetScale();
	return true;
}

bool ClashController::GetStandoffPose(StandoffPose& a_pose) const
{
	if (_phase != ClashPhase::kApproach && _phase != ClashPhase::kStandoff) {
		return false;
	}
	const auto playerPtr = _playerHandle.get();
	const auto npcPtr = _npcHandle.get();
	if (!playerPtr || !npcPtr) {
		return false;
	}

	const float half = StandoffDistance() * 0.5f;
	a_pose.playerPosition = _center - _axis * half;
	a_pose.playerPosition.z = playerPtr->GetPositionZ();
	a_pose.playerHeading = _playerHeading;
	a_pose.contactBase = _center;
	a_pose.contactBase.z = (playerPtr->GetPositionZ() + npcPtr->GetPositionZ()) * 0.5f;
	return true;
}
