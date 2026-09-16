#include "ClashTDM.h"

namespace
{
	[[nodiscard]] const char* ToString(TDM_API::APIResult a_result)
	{
		using enum TDM_API::APIResult;
		switch (a_result) {
		case OK:
			return "OK";
		case NotOwner:
			return "NotOwner";
		case MustKeep:
			return "MustKeep";
		case AlreadyGiven:
			return "AlreadyGiven";
		default:
			return "Unknown";
		}
	}
}

ClashTDM* ClashTDM::GetSingleton()
{
	static ClashTDM singleton;
	return std::addressof(singleton);
}

void ClashTDM::Init()
{
	// Newest first; every version derives from the earlier ones.
	const TDM_API::InterfaceVersion versions[] = {
		TDM_API::InterfaceVersion::V5, TDM_API::InterfaceVersion::V4, TDM_API::InterfaceVersion::V3, TDM_API::InterfaceVersion::V2
	};
	for (const auto version : versions) {
		if (const auto api = TDM_API::RequestPluginAPI(version)) {
			_api = static_cast<TDM_API::IVTDM2*>(api);
			_version = version;
			logger::info("Obtained True Directional Movement API (interface version {})", static_cast<int>(version) + 1);
			return;
		}
	}
	logger::info("True Directional Movement not found (or too old for yaw control)");
}

void ClashTDM::BeginControl()
{
	if (!_api) {
		return;
	}
	const auto handle = SKSE::GetPluginHandle();

	auto result = _api->RequestYawControl(handle, 0.0f);
	_yawOwned = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;
	logger::debug("TDM: RequestYawControl -> {}", ToString(result));

	result = _api->RequestDisableDirectionalMovement(handle);
	_movementDisabled = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;

	result = _api->RequestDisableHeadtracking(handle);
	_headtrackingDisabled = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;

	if (_version >= TDM_API::InterfaceVersion::V5) {
		result = static_cast<TDM_API::IVTDM5*>(_api)->RequestDisableTargetLock(handle);
		_targetLockDisabled = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;
	}
}

void ClashTDM::SetYaw(float a_yaw)
{
	if (_api && _yawOwned) {
		_api->SetPlayerYaw(SKSE::GetPluginHandle(), a_yaw);
	}
}

void ClashTDM::EndControl()
{
	if (!_api) {
		return;
	}
	const auto handle = SKSE::GetPluginHandle();
	if (_yawOwned) {
		_api->ReleaseYawControl(handle);
		_yawOwned = false;
	}
	if (_movementDisabled) {
		_api->ReleaseDisableDirectionalMovement(handle);
		_movementDisabled = false;
	}
	if (_headtrackingDisabled) {
		_api->ReleaseDisableHeadtracking(handle);
		_headtrackingDisabled = false;
	}
	if (_targetLockDisabled && _version >= TDM_API::InterfaceVersion::V5) {
		static_cast<TDM_API::IVTDM5*>(_api)->ReleaseDisableTargetLock(handle);
		_targetLockDisabled = false;
	}
}
