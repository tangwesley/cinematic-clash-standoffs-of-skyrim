#include "OARConditions.h"

#include "ClashController.h"
#include "Settings.h"

namespace Conditions
{
	bool IsInClashCondition::EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator, void*) const
	{
		const bool result = ClashController::GetSingleton()->IsInClash(a_refr);
		if (Settings::GetSingleton()->debugLog) {
			const char* clip = a_clipGenerator && a_clipGenerator->animationName.data() ? a_clipGenerator->animationName.data() : "?";
			logger::debug("OAR: IsInClash evaluated for {} on clip '{}' -> {} (phase {})", a_refr ? a_refr->GetName() : "null", clip, result,
				static_cast<int>(ClashController::GetSingleton()->GetPhase()));
		}
		return result;
	}

	bool IsClashWinnerCondition::EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const
	{
		return ClashController::GetSingleton()->IsWinner(a_refr);
	}

	bool IsClashLoserCondition::EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const
	{
		return ClashController::GetSingleton()->IsLoser(a_refr);
	}

	ClashMeterCondition::ClashMeterCondition()
	{
		_comparison = static_cast<IComparisonConditionComponent*>(AddBaseComponent(ConditionComponentType::kComparison, "Comparison"));
		_numeric = static_cast<INumericConditionComponent*>(AddBaseComponent(ConditionComponentType::kNumeric, "Numeric value"));
	}

	RE::BSString ClashMeterCondition::GetArgument() const
	{
		const auto comparison = _comparison->GetArgument();
		const auto numeric = _numeric->GetArgument();
		return std::format("Clash meter {} {}", comparison.data(), numeric.data()).data();
	}

	RE::BSString ClashMeterCondition::GetCurrent(RE::TESObjectREFR* a_refr) const
	{
		const auto controller = ClashController::GetSingleton();
		if (!controller->IsInClash(a_refr)) {
			return "not in a clash";
		}
		return std::format("{:.2f}", controller->GetMeterFor(a_refr)).data();
	}

	bool ClashMeterCondition::EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, void*) const
	{
		const auto controller = ClashController::GetSingleton();
		if (!controller->IsInClash(a_refr)) {
			return false;
		}
		return _comparison->GetComparisonResult(controller->GetMeterFor(a_refr), _numeric->GetNumericValue(a_refr));
	}
}

namespace
{
	template <class T>
	void RegisterOne()
	{
		using enum OAR_API::Conditions::APIResult;
		switch (OAR_API::Conditions::AddCustomCondition<T>()) {
		case OK:
			logger::info("Registered OAR condition {}", T::CONDITION_NAME);
			break;
		case AlreadyRegistered:
			logger::warn("OAR condition {} is already registered", T::CONDITION_NAME);
			break;
		case Invalid:
			logger::error("OAR condition {} was rejected as invalid", T::CONDITION_NAME);
			break;
		case Failed:
			logger::error("Failed to register OAR condition {}", T::CONDITION_NAME);
			break;
		}
	}
}

namespace OARConditions
{
	void Register()
	{
		if (!OAR_API::Conditions::GetAPI(OAR_API::Conditions::InterfaceVersion::Latest)) {
			logger::info("Open Animation Replacer not found; custom clash conditions unavailable (the clash itself still works)");
			return;
		}

		RegisterOne<Conditions::IsInClashCondition>();
		RegisterOne<Conditions::IsClashWinnerCondition>();
		RegisterOne<Conditions::IsClashLoserCondition>();
		RegisterOne<Conditions::ClashMeterCondition>();
	}
}
