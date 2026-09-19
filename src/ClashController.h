#pragma once

#include "BladeGeometry.h"

// ---------------------------------------------------------------------------
// The clash state machine.
//
//   kIdle      -> nothing happening; a detected parry (see ClashDetection)
//                 marks a pending start that is consumed on the next frame so
//                 that the hit hook never mutates animation state from inside
//                 the game's hit processing.
//   kApproach  -> both actors slide into position facing each other. On entry
//                 whatever they were playing (vanilla, MCO or BFCO attacks,
//                 recoils, staggers) is cut and they are put straight into
//                 their block stance in the same graph update. The camera
//                 blends in behind the player's shoulder (ClashCamera), or,
//                 for a clash that starts in first person with
//                 bForceThirdPerson off, stays in first person while the
//                 player's pitch is eased level. Player controls and the
//                 NPC's AI are disabled for the duration.
//                 The player is made a ghost (hits and hostile magic pass
//                 through them) from the lock until the resolve.
//   kStandoff  -> the quick time event. Attack presses (ClashInput) push the
//                 meter up, the NPC pushes it down every frame. The pair slides
//                 along the clash axis as the meter moves so the struggle is
//                 visible without any HUD element.
//   kResolve   -> one frame after both actors have been told to stop blocking:
//                 the loser is staggered, the NPC's AI is restored.
//   kAftermath -> short window in which the OAR winner/loser conditions stay
//                 true and, if first person was forced off, it is restored.
//
// Auto-win: when the opponent's weapon skill trails the player's by the
// configured gap, the detected clash skips the approach and standoff
// entirely; the opponent is staggered on the next frame and the machine goes
// straight to kAftermath. Only the player ever gets this.
//
// Everything here runs on the main thread from PlayerCharacter::Update, except
// OnAttackPressed which only bumps an atomic counter.
// ---------------------------------------------------------------------------

enum class ClashPhase : std::uint8_t
{
	kIdle,
	kApproach,
	kStandoff,
	kResolve,
	kAftermath
};

enum class ClashOutcome : std::uint8_t
{
	kNone,
	kPlayerWon,
	kPlayerLost,
	kDraw  // the timer ran out: both take the small stagger, no winner or loser
};

class ClashController
{
public:
	[[nodiscard]] static ClashController* GetSingleton();

	void InstallHooks();

	// Detection entry point (main thread). Returns true when a clash has been
	// accepted for this pair, in which case the caller must swallow the hit.
	bool OnClashDetected(RE::Actor* a_attacker, RE::Actor* a_target);

	// True while a hit between these actors must not reach the game.
	[[nodiscard]] bool ShouldSwallowHit(const RE::Actor* a_attacker, const RE::Actor* a_target) const;

	// From the input sink. Safe to call from any thread.
	void OnAttackPressed();

	// Tear everything down right now (game load, actor vanished, ...).
	void Abort(std::string_view a_reason);

	// True while the player's fighting input handlers must ignore input
	// (approach and standoff). Fighting controls are never toggled off through
	// the ControlMap because the game sheathes the weapon when they are.
	[[nodiscard]] bool IsPlayerLocked() const noexcept { return _controlsLocked; }

	// --- Queries used by the Open Animation Replacer conditions ------------
	[[nodiscard]] ClashPhase GetPhase() const noexcept { return _phase; }
	[[nodiscard]] bool       IsInClash(const RE::TESObjectREFR* a_refr) const;
	[[nodiscard]] bool       IsWinner(const RE::TESObjectREFR* a_refr) const;
	[[nodiscard]] bool       IsLoser(const RE::TESObjectREFR* a_refr) const;
	// 0..1 from this actor's point of view (1 = this actor is winning).
	[[nodiscard]] float GetMeterFor(const RE::TESObjectREFR* a_refr) const;

	// Where the pair will stand once the approach slide finishes, in world
	// space: the player's spot and locked heading, and the contact point at
	// ground level (add the framing's aim height). Valid from BeginClash until
	// the resolve; false otherwise.
	struct StandoffPose
	{
		RE::NiPoint3 playerPosition;
		float        playerHeading{ 0.0f };
		RE::NiPoint3 contactBase;
	};
	[[nodiscard]] bool GetStandoffPose(StandoffPose& a_pose) const;

	// Where the weapons meet right now (follows the pair as the meter pushes
	// them). False when no clash is running.
	[[nodiscard]] bool GetContactPoint(RE::NiPoint3& a_point) const;

	// Public because stl::write_vfunc instantiates against it from namespace
	// scope. As a nested class it still reaches the private members below.
	struct PlayerUpdateHook
	{
		static void thunk(RE::PlayerCharacter* a_player, float a_delta)
		{
			func(a_player, a_delta);
			GetSingleton()->OnFrame(a_delta);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Character::Update for NPCs: the clash opponent's locomotion inputs are
	// zeroed right after its own update so the slide never reads as walking.
	struct CharacterUpdateHook
	{
		static void thunk(RE::Character* a_character, float a_delta)
		{
			func(a_character, a_delta);
			GetSingleton()->OnCharacterUpdated(a_character);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	// TESObjectREFR::UpdateAnimation (slot 0x7D; the actor vtables only
	// diverge between runtimes past 0x82). The frame's pose is on the skeleton
	// by the time the original returns, and a node write any earlier is
	// overwritten by it.
	template <class Class>
	struct UpdateAnimationHook
	{
		static void thunk(RE::TESObjectREFR* a_this, float a_delta)
		{
			func(a_this, a_delta);
			GetSingleton()->ApplyPoseFixups(static_cast<Class*>(a_this));
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	// The height correction and the blade turned out of the other fighter.
	// Does nothing for an actor not in a clash. Public for the hook above.
	void ApplyPoseFixups(RE::Actor* a_actor);

	// What a joint write left in a node, so the next one can tell whether
	// anything has driven it since.
	//
	// Turning a node on top of the animation only works while the animation is
	// writing it every frame. Bones are; an attachment node like WEAPON never
	// is, and a frozen graph stops writing even bones -- there a per-frame
	// delta multiplies into itself and the weapon spins. So a node still
	// holding exactly what we wrote is turned from the remembered input
	// instead. The same record puts a node back when the correction ends:
	// bones recover on the next update, WEAPON would stay crooked.
	struct NodeTurn
	{
		RE::NiMatrix3 written;
		RE::NiMatrix3 input;
		bool          valid{ false };
	};

	// IAnimationGraphManagerHolder::NotifyAnimationGraph (slot 1) on the actor
	// classes. While a participant is locked, only our own events and the
	// cancel events reach the graph, so nothing else can start.
	template <class Holder>
	struct NotifyGraphHook
	{
		static bool thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
		{
			if (!GetSingleton()->AllowGraphEvent(a_this, a_event)) {
				return false;
			}
			return func(a_this, a_event);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	[[nodiscard]] bool AllowGraphEvent(RE::IAnimationGraphManagerHolder* a_holder, const RE::BSFixedString& a_event) const;
	void               OnCharacterUpdated(RE::Character* a_character);

	// PlayerInputHandler::CanProcess (slot 1) for the fighting handlers. While
	// the player is locked, attack/block, sheathe and shout input is dropped
	// before the handler sees it, which keeps the weapon drawn.
	template <class Handler>
	struct CanProcessHook
	{
		static bool thunk(Handler* a_this, RE::InputEvent* a_event)
		{
			if (GetSingleton()->IsPlayerLocked()) {
				return false;
			}
			return func(a_this, a_event);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	// MagicTarget::AddTarget (slot 1 of the MagicTarget vtable) on the player.
	// While the player is locked, hostile effects cast by anyone else are
	// refused before an ActiveEffect exists, so a paralysis enchantment on an
	// arrow, a poison or a spell cannot knock the player out of the hold.
	struct AddTargetHook
	{
		static bool thunk(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
		{
			if (GetSingleton()->ShouldRejectEffect(a_data)) {
				return false;
			}
			return func(a_this, a_data);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	[[nodiscard]] bool ShouldRejectEffect(const RE::MagicTarget::AddTargetData& a_data) const;

	// Co-save (see Plugin.cpp): the save callback asks whether the player is
	// ghosted for a clash right now, and the load callback reports a save that
	// was written while they were, so the flag is cleared once the game runs.
	[[nodiscard]] bool IsPlayerGhostedForClash() const noexcept { return _playerImmune && !_playerWasGhost; }
	void               MarkGhostFromSave() noexcept { _clearGhostOnLoad = true; }

private:
	ClashController() = default;
	ClashController(const ClashController&) = delete;
	ClashController(ClashController&&) = delete;
	~ClashController() = default;
	ClashController& operator=(const ClashController&) = delete;
	ClashController& operator=(ClashController&&) = delete;

	void OnFrame(float a_delta);

	void BeginClash();
	void BeginInstantWin(RE::Actor* a_player, RE::Actor* a_npc);
	void SetupGeometry(RE::Actor* a_player, RE::Actor* a_npc);
	void TickApproach(RE::Actor* a_player, RE::Actor* a_npc, float a_dt);
	void TickStandoff(RE::Actor* a_player, RE::Actor* a_npc, float a_dt);
	// What the standoff resolves to when the timer runs out: a draw, or a win
	// for the side the meter favours ([Outcome] iTimeoutResolution).
	[[nodiscard]] ClashOutcome TimeoutOutcome() const;
	void Finish(ClashOutcome a_outcome);
	void TickResolve(RE::Actor* a_player, RE::Actor* a_npc);
	void TickAftermath(float a_dt);
	void ReturnToIdle();

	void PositionActors(RE::Actor* a_player, RE::Actor* a_npc, float a_blend, float a_meterOffset);

	// Weapon geometry: both blades measured off their meshes, the point where
	// they are closest, and the separation that makes them meet.
	struct SolveResult
	{
		bool         touching{ false };  // some separation in range brings the blades together
		float        distance{ 0.0f };   // the separation solved for, touching or closest
		float        gap{ 0.0f };        // what is left between them there
		RE::NiPoint3 onPlayer;           // the closest point on each blade at that separation
		RE::NiPoint3 onNpc;
	};
	void                MeasureBlades(RE::Actor* a_player, RE::Actor* a_npc);
	[[nodiscard]] float GapAt(float a_distance, RE::NiPoint3* a_onPlayer = nullptr, RE::NiPoint3* a_onNpc = nullptr) const;
	bool                SolveClashDistance(bool a_log, SolveResult& a_result);
	// The separation PositionActors holds the pair at: the solved one when
	// there is one, else fClashDistance.
	[[nodiscard]] float StandoffDistance() const;

	void FaceEachOther(RE::Actor* a_player, RE::Actor* a_npc);
	void CancelAttack(RE::Actor* a_actor);
	void CancelAndBlock(RE::Actor* a_actor);
	void SendBlock(RE::Actor* a_actor);
	void SendBlockStop(RE::Actor* a_actor);
	void SendStagger(RE::Actor* a_actor, float a_magnitude);

	void ResolveSparkModel(RE::Actor* a_player, RE::Actor* a_npc);
	void TickSparks(RE::Actor* a_player, RE::Actor* a_npc, float a_dt, float a_meterOffset);

	// A shield would make the graph play the shield block; hide it and tell the
	// graph the left hand is empty so the one-handed weapon block plays instead.
	void HideShield(RE::Actor* a_actor, bool& a_hidden);
	void RestoreShield(RE::Actor* a_actor, bool& a_hidden);

	void LockPlayer(RE::PlayerCharacter* a_player);
	void UnlockPlayer();
	void RestoreNpcAI();
	void RestoreTime();

	[[nodiscard]] bool IsParticipant(const RE::TESObjectREFR* a_refr) const noexcept;
	[[nodiscard]] float NpcPushRate(RE::Actor* a_player, RE::Actor* a_npc) const;

	// --- state ---------------------------------------------------------------
	ClashPhase   _phase{ ClashPhase::kIdle };
	ClashOutcome _outcome{ ClashOutcome::kNone };
	float        _phaseTime{ 0.0f };
	float        _cooldownLeft{ 0.0f };
	bool         _pendingStart{ false };

	RE::ActorHandle _playerHandle;
	RE::ActorHandle _npcHandle;
	RE::FormID      _playerID{ 0 };
	RE::FormID      _npcID{ 0 };

	RE::NiPoint3 _axis;         // unit vector, player -> NPC, XY only
	RE::NiPoint3 _center;       // midpoint at clash start
	RE::NiPoint3 _playerStart;
	RE::NiPoint3 _npcStart;

	float _meter{ 0.5f };
	bool  _blockSent{ false };

	// Difficulty: the governing weapon skills captured when the clash was
	// accepted, the auto-win decision, and the fractional press credit
	// accumulated by a held attack button in hold-to-mash mode.
	bool  _instantWin{ false };
	float _playerSkill{ 0.0f };
	float _npcSkill{ 0.0f };
	float _holdAccumulator{ 0.0f };

	// Per-actor block hold: blockStart is re-sent (with a cooldown) until the
	// graph is seen in the block state, and immediately if it later leaves it.
	struct BlockHold
	{
		bool  established{ false };
		bool  everEstablished{ false };  // seen blocking at least once this clash
		float resendTimer{ 0.0f };
		float cancelTimer{ 0.0f };       // since the last cancel; paced to MCO's blend out of an attack
		float attackTime{ 0.0f };        // how long the attack state has refused to clear
		bool  gaveUpOnAttack{ false };   // stopped waiting for it, and said so once
	};
	BlockHold _playerBlock;
	BlockHold _npcBlock;
	void      HoldBlock(RE::Actor* a_actor, BlockHold& a_hold, float a_dt);

	float _meterOffset{ 0.0f };  // current slide along the axis, from the meter

	// Both blades are re-measured every frame so the contact point follows
	// the pose; the separation solve on top of them is re-run only until the
	// pose settles, because a target that keeps moving is a warp every frame
	// and the movement system reads that as walking.
	struct Blades
	{
		BladeGeometry::Blade player;
		BladeGeometry::Blade npc;
		RE::NiPoint3         playerOrigin;  // actor positions when measured
		RE::NiPoint3         npcOrigin;
		bool                 valid{ false };
	};
	Blades       _blades;
	RE::NiPoint3 _contactPoint;             // where the two blades are closest
	bool         _contactValid{ false };
	float        _bladeGap{ 0.0f };         // distance between them there
	float        _solvedDistance{ 0.0f };   // 0 = no solve; fClashDistance stands
	bool         _solveLocked{ false };     // stop re-solving: the pose has settled
	bool         _solveReported{ false };   // the one log line per clash is out

	// Height correction. A gap the separation solve cannot close is vertical,
	// and no standing position reaches it -- the character controller owns
	// their height. So the higher blade's owner is bent down to the other,
	// half out of the spine and half out of the sword arm's shoulder, and the
	// separation is solved once more against the pose that results.
	struct Tilt
	{
		RE::FormID   actor{ 0 };  // whose skeleton is bent; 0 = nobody
		RE::NiPoint3 axis;        // world rotation axis: horizontal, across the clash
		const char*  spineNode{ nullptr };
		float        spineAngle{ 0.0f };
		NodeTurn     spineTurn;
		const char*  armNode{ nullptr };
		float        armAngle{ 0.0f };
		NodeTurn     armTurn;
	};
	Tilt _tilt;
	// Written on the game thread, read by UpdateAnimation, which the engine
	// may run off a worker; the flag publishes it and retires it.
	std::atomic<bool> _tiltValid{ false };
	std::atomic<bool> _tiltApplied{ false };   // the hook has written it at least once
	bool              _tiltResolved{ false };  // the separation has been re-solved on top of it
	void              PlanTilt(const SolveResult& a_result, RE::Actor* a_player, RE::Actor* a_npc);
	void              ClearTilt();
	void              ApplyTilt(RE::Actor* a_actor, RE::NiAVObject* a_root);

	// Clipping: a blade swung into the other fighter is turned back out, at
	// the wrist and then at the weapon node for the overflow. A running angle
	// rather than a plan -- the animation keeps moving, so it is measured
	// again each frame off a blade that already carries the correction.
	struct Clearance
	{
		RE::FormID   actor{ 0 };
		RE::NiPoint3 axis;
		float        angle{ 0.0f };
		bool         leftHand{ false };  // which hand the blade is in
		NodeTurn     handTurn;
		NodeTurn     weaponTurn;         // the one that is not animated, and so must be put back
	};
	Clearance         _clearance[2];  // player, then opponent
	std::atomic<bool> _clearanceValid{ false };
	float             _clearanceDepth[2]{ 0.0f, 0.0f };  // last measured, for the log
	void              UpdateClearance(RE::Actor* a_player, RE::Actor* a_npc, float a_dt);
	void              ClearClearance();
	void              ApplyClearance(RE::Actor* a_actor, RE::NiAVObject* a_root);

	// Settle: headings are re-aimed every frame until this much time has
	// passed since the clash began; then they freeze and spine tracking is off.
	float _clashTime{ 0.0f };
	bool  _settled{ false };
	float _playerHeading{ 0.0f };  // held after settle
	float _npcHeading{ 0.0f };
	void  LockUpperBody(RE::Actor* a_actor);
	void  UnlockUpperBody(RE::Actor* a_actor);

	// Collision: while locked, the two actors' havok objects ignore each other
	// (through Precision's collision-filter callback when it is present, else
	// by disabling the opponent's collision outright). Queried from havok
	// threads hundreds of times per frame, hence the atomics.
public:
	[[nodiscard]] bool ShouldIgnoreCollision(std::uint32_t a_filterA, std::uint32_t a_filterB) const noexcept
	{
		if (!_ignoreCollision.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto ga = a_filterA >> 16, gb = a_filterB >> 16;
		const auto pg = _playerGroup.load(std::memory_order_relaxed), ng = _npcGroup.load(std::memory_order_relaxed);
		return pg != 0 && ng != 0 && ((ga == pg && gb == ng) || (ga == ng && gb == pg));
	}

private:
	std::atomic<bool>          _ignoreCollision{ false };
	std::atomic<std::uint32_t> _playerGroup{ 0 };
	std::atomic<std::uint32_t> _npcGroup{ 0 };
	bool                       _npcCollisionFlagged{ false };
	void                       BeginCollisionIgnore(RE::Actor* a_player, RE::Actor* a_npc);
	void                       EndCollisionIgnore();

	// Freeze (opt-in): after the settle window the opponent's graph stops
	// advancing (disabled AI) until the resolve. The player is not affected.
	bool _frozen{ false };
	bool _npcAIDisabledForFreeze{ false };
	void Freeze(RE::Actor* a_player, RE::Actor* a_npc);
	void Unfreeze();

	// weapon block-impact data (sparks model and sounds), resolved per clash
	RE::BGSImpactData* _blockImpact{ nullptr };

	// sparks
	std::string _sparkModel;          // resolved per clash; empty = none
	float       _sparkDuration{ 0.5f };
	float       _sparkTimer{ 0.0f };

	std::atomic<int> _pendingPresses{ 0 };

	// things to undo
	std::uint32_t _savedControls{ 0 };
	bool          _controlsLocked{ false };
	bool          _npcRestrained{ false };
	bool          _npcControlsDriven{ false };  // movement controller taken off AI driving
	void          RestoreNpcMovement();
	bool          _playerShieldHidden{ false };
	bool          _npcShieldHidden{ false };
	float         _settingsPollTimer{ 0.0f };

	// Immunity: while the player is locked they are made a ghost (the engine's
	// own cutscene invulnerability, Papyrus Actor.SetGhost: projectiles and
	// spells pass through, melee does nothing) and hostile magic is refused at
	// AddTarget as a second line. The ghost flag is stored in the player's
	// extra data and travels with the save, so a save written mid-clash is
	// recorded in the co-save and the flag cleared on load.
	bool              _playerImmune{ false };
	bool              _playerWasGhost{ false };  // already a ghost before us: left alone
	bool              _clearGhostOnLoad{ false };
	int               _ghostVerifyFrames{ 0 };
	std::atomic<bool> _rejectHostileMagic{ false };  // read from the AddTarget hook
	void              BeginPlayerImmunity(RE::Actor* a_player);
	void              EndPlayerImmunity();
	void              VerifyGhost(RE::Actor* a_player);

	// graph event filtering
	RE::IAnimationGraphManagerHolder* _playerHolder{ nullptr };
	RE::IAnimationGraphManagerHolder* _npcHolder{ nullptr };
	float                             _diagTimer{ 0.0f };

	// Sends one of our own events past the filter.
	void SendEvent(RE::Actor* a_actor, const char* a_event);
	bool          _timeScaled{ false };
	bool          _forcedThirdPerson{ false };

	// First-person standoff: the clash runs with the camera still in first
	// person (bForceThirdPerson off, or the clash camera disabled). The block
	// and the lock are the same; on top, the first-person model's shield is
	// hidden with the third-person one and the player's pitch is eased level
	// over the settle window so they look at the opponent, not the floor.
	bool  _firstPersonClash{ false };
	float _playerStartPitch{ 0.0f };
};
