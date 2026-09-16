#pragma once

#include "TrueDirectionalMovementAPI.h"

// ---------------------------------------------------------------------------
// True Directional Movement bridge.
//
// TDM turns the player toward the crosshair while blocking, in turn-rate
// steps every frame. With the clash camera aimed away from the player's
// heading that fight never ends: TDM turns, the hold snaps back, and the graph
// plays its turn-in-place cycle underneath the block. TDM's API lets another
// plugin take yaw control for a while, so during a clash the plugin owns the
// player's yaw (and disables TDM's directional movement, head tracking and
// target lock), then hands everything back at the resolve.
// ---------------------------------------------------------------------------
class ClashTDM
{
public:
	[[nodiscard]] static ClashTDM* GetSingleton();

	// kPostLoad or later.
	void Init();

	[[nodiscard]] bool IsAvailable() const noexcept { return _api != nullptr; }

	void BeginControl();
	void SetYaw(float a_yaw);
	void EndControl();

private:
	ClashTDM() = default;
	ClashTDM(const ClashTDM&) = delete;
	ClashTDM(ClashTDM&&) = delete;
	~ClashTDM() = default;
	ClashTDM& operator=(const ClashTDM&) = delete;
	ClashTDM& operator=(ClashTDM&&) = delete;

	TDM_API::IVTDM2*          _api{ nullptr };
	TDM_API::InterfaceVersion _version{ TDM_API::InterfaceVersion::V1 };
	bool                      _yawOwned{ false };
	bool                      _movementDisabled{ false };
	bool                      _headtrackingDisabled{ false };
	bool                      _targetLockDisabled{ false };
};
