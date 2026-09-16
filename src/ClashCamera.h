#pragma once

#define SMOOTHCAM_API_COMMONLIB
#include "SmoothCamAPI.h"

#include "Settings.h"

// ---------------------------------------------------------------------------
// Over-the-shoulder camera for the duration of a clash.
//
// Hooks ThirdPersonState::Update. Before the game's update runs, the state's
// free-rotation offsets are zeroed (SmoothCam leaves an orbit offset behind,
// and any offset makes the engine turn the blocking player toward the
// camera). After the update, the camera root and the NiCamera under it are
// given our position and our aim directly, the same way SmoothCam does it: the
// state writes the nodes itself during Update, so only node writes afterwards
// take effect. The aim matrices follow SmoothCam's construction with the
// pitch taken as a plain angle (positive = down).
//
// On release, position, aim and FOV blend back to the engine's own camera over
// fBlendOut, then control is handed back.
//
// SmoothCam compatibility: SmoothCam hooks the same virtual and, when another
// plugin holds camera control through its API, runs the vanilla update and
// keeps its hands off the result. So we request control for the clash and, on
// release, ask SmoothCam to glide back to its own goal position. If SmoothCam
// is active and refuses control, the camera part of the clash is skipped rather
// than fought over.
// ---------------------------------------------------------------------------
class ClashCamera
{
public:
	[[nodiscard]] static ClashCamera* GetSingleton();

	void InstallHooks();

	// SmoothCam interface-loader callback (SKSE messaging, kPostLoad).
	void OnSmoothCamInterface(void* a_interface, SmoothCamAPI::InterfaceVersion a_version);

	// Start the override. Returns true when first person had to be turned off,
	// in which case the caller is responsible for calling RestoreFirstPerson().
	bool Begin();
	// Start blending back; control is released once the blend completes.
	void End();
	void RestoreFirstPerson();

	[[nodiscard]] bool IsActive() const noexcept { return _active; }

	// The framing chosen for the current (or most recent) clash: one of the
	// listed presets picked at random in Begin, skipping any walled off, or
	// the plain [Camera] keys when none are listed. The contact point's height
	// comes from here so the aim and the sparks agree with the shot.
	[[nodiscard]] const Settings::CameraFraming& Framing() const noexcept { return _framing; }

	struct ThirdPersonUpdateHook
	{
		static void thunk(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
		{
			const auto self = GetSingleton();
			self->BeforeThirdPersonUpdate(a_this);
			func(a_this, a_nextState);
			self->AfterThirdPersonUpdate(a_this);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	ClashCamera() = default;
	ClashCamera(const ClashCamera&) = delete;
	ClashCamera(ClashCamera&&) = delete;
	~ClashCamera() = default;
	ClashCamera& operator=(const ClashCamera&) = delete;
	ClashCamera& operator=(ClashCamera&&) = delete;

	void BeforeThirdPersonUpdate(RE::ThirdPersonState* a_state);
	void AfterThirdPersonUpdate(RE::ThirdPersonState* a_state);
	void ApplyPosition(RE::ThirdPersonState* a_state, const RE::NiPoint3& a_position);
	// Writes the camera root and NiCamera rotation for a yaw (Skyrim heading)
	// and pitch (positive = down), the way SmoothCam does.
	void ApplyRotation(float a_yaw, float a_pitch);
	void ComputeAim(RE::Actor* a_player, const RE::NiPoint3& a_cameraPos, float& a_yaw, float& a_pitch) const;
	void FinishRelease();

	float _lastYaw{ 0.0f };
	float _lastPitch{ 0.0f };
	float _startYaw{ 0.0f };
	float _startPitch{ 0.0f };
	float _releaseStartYaw{ 0.0f };
	float _releaseStartPitch{ 0.0f };

	[[nodiscard]] RE::NiPoint3 ComputeTarget(RE::Actor* a_player) const;
	// Pick this clash's framing: a random listed preset whose camera position
	// is not walled off from the contact point, else the first listed one.
	[[nodiscard]] Settings::CameraFraming ChooseFraming(RE::Actor* a_player) const;

	Settings::CameraFraming _framing;

	SmoothCamAPI::IVSmoothCam1*    _smoothCam{ nullptr };
	SmoothCamAPI::InterfaceVersion _smoothCamVersion{ SmoothCamAPI::InterfaceVersion::V1 };
	bool                           _ownsSmoothCam{ false };

	bool         _active{ false };     // overriding towards the shoulder shot
	bool         _releasing{ false };  // blending back to the engine's camera
	bool         _haveStart{ false };
	float        _blendTime{ 0.0f };
	float        _releaseTime{ 0.0f };
	float        _lastLogTime{ 0.0f };
	RE::NiPoint3 _startTranslation;
	RE::NiPoint3 _lastPosition;

	// FOV: saved on Begin, blended with the position, put back exactly on release.
	bool  _fovSaved{ false };
	float _savedFOV{ 0.0f };
	float _lastFOV{ 0.0f };
	float _releaseStartFOV{ 0.0f };

	void ApplyFOV(float a_fov);
	void RestoreFOV();
};
