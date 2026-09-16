#pragma once

#include "PrecisionAPI.h"

// ---------------------------------------------------------------------------
// Where clashes come from.
//
// The detection itself is not ours: it is the ShouldParry() check from
// Simple Weapon Swing Parry (doodlum/MaxsuWeaponSwingParry-ng), vendored
// unchanged under include/MaxsuWeaponParry. We sit at the same two entry
// points that mod uses:
//
//   1. the vanilla melee-hit call (trampoline hook on the exact call site the
//      parry mod patches, so both plugins chain regardless of load order), and
//   2. Precision's pre-hit callback when Precision is installed, since it
//      replaces the vanilla hit path with its own collisions.
//
// If the parry mod runs before us on a hit it will already have raised its
// bMaxsuWeaponParry_InWeaponParry graph variable, which we honour directly;
// otherwise we run the identical check ourselves. Either way the outcome of a
// weapon clash is handed to ClashController, and if it accepts, the hit is
// swallowed so neither actor takes damage or recoils.
// ---------------------------------------------------------------------------
class ClashDetection
{
public:
	static void InstallHook();
	static void TryRegisterPrecision();

	[[nodiscard]] static bool HasPrecision() noexcept { return _precision != nullptr; }

private:
	// Returns true when the hit must not be processed by the game.
	static bool Evaluate(RE::Actor* a_attacker, RE::Actor* a_target);

	static void OnMeleeHit(RE::Actor* a_attacker, RE::Actor* a_target, std::int64_t a_int1, bool a_bool, void* a_unkptr);

	static inline REL::Relocation<decltype(OnMeleeHit)> _OnMeleeHit;
	static inline PRECISION_API::IVPrecision1*          _precision{ nullptr };
};
