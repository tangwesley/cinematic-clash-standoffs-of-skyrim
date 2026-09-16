#pragma once

#include "OpenAnimationReplacerAPI-Conditions.h"

// ---------------------------------------------------------------------------
// Custom Open Animation Replacer conditions.
//
// The clash works with the vanilla block and stagger animations, but these
// conditions let an OAR replacer swap in dedicated ones (a strained weapon
// lock instead of a calm block idle, a dramatic knock-down for the loser, ...)
// without any behavior edits. See package/meshes/.../Cinematic Clash for the
// shipped config templates.
//
//   CinematicClash_IsInClash      true for both actors while locked together
//   CinematicClash_IsClashWinner  true for the winner while the outcome plays
//   CinematicClash_IsClashLoser   true for the loser while the outcome plays
//   CinematicClash_ClashMeter     compares this actor's 0..1 advantage
// ---------------------------------------------------------------------------
namespace Conditions
{
	class IsInClashCondition : public CustomCondition
	{
	public:
		constexpr static inline std::string_view CONDITION_NAME = "CinematicClash_IsInClash"sv;

		RE::BSString GetName() const override { return CONDITION_NAME.data(); }
		RE::BSString GetDescription() const override { return "True while the actor is locked in a Cinematic Clash standoff (approach and mash phases)."sv.data(); }
		constexpr REL::Version GetRequiredVersion() const override { return { 1, 0, 0 }; }

	protected:
		bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator, void* a_subMod) const override;
	};

	class IsClashWinnerCondition : public CustomCondition
	{
	public:
		constexpr static inline std::string_view CONDITION_NAME = "CinematicClash_IsClashWinner"sv;

		RE::BSString GetName() const override { return CONDITION_NAME.data(); }
		RE::BSString GetDescription() const override { return "True for the actor that won a Cinematic Clash, from the moment the standoff resolves until the outcome window ends."sv.data(); }
		constexpr REL::Version GetRequiredVersion() const override { return { 1, 0, 0 }; }

	protected:
		bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator, void* a_subMod) const override;
	};

	class IsClashLoserCondition : public CustomCondition
	{
	public:
		constexpr static inline std::string_view CONDITION_NAME = "CinematicClash_IsClashLoser"sv;

		RE::BSString GetName() const override { return CONDITION_NAME.data(); }
		RE::BSString GetDescription() const override { return "True for the actor that lost a Cinematic Clash, from the moment the standoff resolves until the outcome window ends. Use it to replace the large stagger."sv.data(); }
		constexpr REL::Version GetRequiredVersion() const override { return { 1, 0, 0 }; }

	protected:
		bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator, void* a_subMod) const override;
	};

	class ClashMeterCondition : public CustomCondition
	{
	public:
		constexpr static inline std::string_view CONDITION_NAME = "CinematicClash_ClashMeter"sv;

		ClashMeterCondition();

		RE::BSString GetName() const override { return CONDITION_NAME.data(); }
		RE::BSString GetDescription() const override { return "Compares this actor's advantage in the current Cinematic Clash standoff (0 = losing badly, 0.5 = even, 1 = winning). False outside a clash."sv.data(); }
		constexpr REL::Version GetRequiredVersion() const override { return { 1, 0, 0 }; }

		RE::BSString GetArgument() const override;
		RE::BSString GetCurrent(RE::TESObjectREFR* a_refr) const override;

	protected:
		bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator, void* a_subMod) const override;

		IComparisonConditionComponent* _comparison{ nullptr };
		INumericConditionComponent*    _numeric{ nullptr };
	};
}

namespace OARConditions
{
	// Must run during SKSE kPostLoad; OAR freezes its condition registry after.
	void Register();
}
