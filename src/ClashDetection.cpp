#include "ClashDetection.h"

#include "ClashController.h"
#include "MaxsuWeaponParry/WeaponParry_Functions.h"
#include "Settings.h"

void ClashDetection::InstallHook()
{
	// Identical call site to Simple Weapon Swing Parry's MeleeHitHook, so the
	// two plugins chain through each other whichever is loaded first.
	REL::Relocation<std::uintptr_t> base{ REL::RelocationID(37650, 38603) };
	auto&                           trampoline = SKSE::GetTrampoline();
	_OnMeleeHit = trampoline.write_call<5>(base.address() + REL::Relocate(0x38B, 0x45A), OnMeleeHit);
	logger::info("Installed melee hit hook");
}

void ClashDetection::TryRegisterPrecision()
{
	void* api = PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V2);
	if (!api) {
		api = PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V1);
	}
	if (!api) {
		logger::info("Precision not found; using the vanilla hit path only");
		return;
	}

	_precision = static_cast<PRECISION_API::IVPrecision1*>(api);

	const auto result = _precision->AddPreHitCallback(SKSE::GetPluginHandle(),
		[](const PRECISION_API::PrecisionHitData& a_hit) {
			PRECISION_API::PreHitCallbackReturn ret;
			const auto                          target = a_hit.target ? a_hit.target->As<RE::Actor>() : nullptr;
			if (Evaluate(a_hit.attacker, target)) {
				ret.bIgnoreHit = true;
			}
			return ret;
		});

	if (result == PRECISION_API::APIResult::OK) {
		logger::info("Registered Precision pre-hit callback");
	} else {
		logger::warn("Precision pre-hit callback registration failed ({})", static_cast<int>(result));
	}

	// Lets the two clash participants pass through each other while locked,
	// without touching their collision with anything else. Called from havok
	// threads many times per frame; the controller keeps the check to a few
	// atomic reads.
	const auto filterResult = _precision->AddCollisionFilterComparisonCallback(SKSE::GetPluginHandle(),
		[](RE::bhkCollisionFilter*, std::uint32_t a_filterA, std::uint32_t a_filterB) {
			return ClashController::GetSingleton()->ShouldIgnoreCollision(a_filterA, a_filterB) ?
			           PRECISION_API::CollisionFilterComparisonResult::Ignore :
			           PRECISION_API::CollisionFilterComparisonResult::Continue;
		});
	if (filterResult == PRECISION_API::APIResult::OK) {
		logger::info("Registered Precision collision-filter callback");
	} else {
		logger::warn("Precision collision-filter callback registration failed ({})", static_cast<int>(filterResult));
	}
}

bool ClashDetection::Evaluate(RE::Actor* a_attacker, RE::Actor* a_target)
{
	const auto controller = ClashController::GetSingleton();

	if (controller->ShouldSwallowHit(a_attacker, a_target)) {
		return true;
	}
	if (!a_attacker || !a_target) {
		return false;
	}

	// Debug: any hit the player lands counts as a clash, no parry needed.
	if (Settings::GetSingleton()->forceClashOnHit && a_attacker == RE::PlayerCharacter::GetSingleton()) {
		logger::debug("Forced clash (bForceClashOnHit): {} -> {}", a_attacker->GetName(), a_target->GetName());
		return controller->OnClashDetected(a_attacker, a_target);
	}

	bool       maxsuFlag = false;
	const bool parryByMaxsu = a_target->GetGraphVariableBool("bMaxsuWeaponParry_InWeaponParry", maxsuFlag) && maxsuFlag;

	if (!parryByMaxsu && !MaxsuWeaponParry::ParryCheck::ShouldParry(a_attacker, a_target)) {
		return false;
	}

	logger::debug("Weapon clash detected: {} -> {}{}", a_attacker->GetName(), a_target->GetName(),
		parryByMaxsu ? " (flagged by Simple Weapon Swing Parry)" : "");

	return controller->OnClashDetected(a_attacker, a_target);
}

void ClashDetection::OnMeleeHit(RE::Actor* a_attacker, RE::Actor* a_target, std::int64_t a_int1, bool a_bool, void* a_unkptr)
{
	if (Evaluate(a_attacker, a_target)) {
		return;
	}
	_OnMeleeHit(a_attacker, a_target, a_int1, a_bool, a_unkptr);
}
