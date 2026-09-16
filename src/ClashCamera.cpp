#include "ClashCamera.h"

#include "ClashController.h"
#include "Settings.h"

namespace
{
	constexpr float kPi = std::numbers::pi_v<float>;
	constexpr float kHalfPi = kPi * 0.5f;
	constexpr float kDegToRad = kPi / 180.0f;

	[[nodiscard]] float SmoothStep(float a_t)
	{
		a_t = std::clamp(a_t, 0.0f, 1.0f);
		return a_t * a_t * (3.0f - 2.0f * a_t);
	}

	[[nodiscard]] float RealDelta()
	{
		const auto timer = RE::BSTimer::GetSingleton();
		const float dt = timer ? timer->realTimeDelta : 0.0f;
		return (dt > 0.0f && dt < 0.5f) ? dt : (1.0f / 60.0f);
	}

	[[nodiscard]] RE::NiPoint3 Lerp(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_t)
	{
		return a_from + (a_to - a_from) * a_t;
	}

	[[nodiscard]] float WrapAngle(float a_angle)
	{
		while (a_angle > kPi) {
			a_angle -= 2.0f * kPi;
		}
		while (a_angle < -kPi) {
			a_angle += 2.0f * kPi;
		}
		return a_angle;
	}

	[[nodiscard]] float LerpAngle(float a_from, float a_to, float a_t)
	{
		return a_from + WrapAngle(a_to - a_from) * a_t;
	}

	[[nodiscard]] const char* ToString(SmoothCamAPI::APIResult a_result)
	{
		using enum SmoothCamAPI::APIResult;
		switch (a_result) {
		case OK:
			return "OK";
		case NotOwner:
			return "NotOwner";
		case MustKeep:
			return "MustKeep";
		case AlreadyGiven:
			return "AlreadyGiven";
		case AlreadyTaken:
			return "AlreadyTaken";
		case BadThread:
			return "BadThread";
		default:
			return "Unknown";
		}
	}

	[[nodiscard]] bool IsCurrentState(RE::ThirdPersonState* a_state)
	{
		const auto camera = RE::PlayerCamera::GetSingleton();
		return camera && camera->currentState.get() == a_state;
	}

	// 3x3 helpers in plain math convention (m[row][col]).
	using Mat3 = std::array<std::array<float, 3>, 3>;

	[[nodiscard]] Mat3 Mul(const Mat3& a, const Mat3& b)
	{
		Mat3 r{};
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
			}
		}
		return r;
	}

	[[nodiscard]] Mat3 RotX(float a) { return { { { 1, 0, 0 }, { 0, std::cos(a), -std::sin(a) }, { 0, std::sin(a), std::cos(a) } } }; }
	[[nodiscard]] Mat3 RotY(float a) { return { { { std::cos(a), 0, std::sin(a) }, { 0, 1, 0 }, { -std::sin(a), 0, std::cos(a) } } }; }
	[[nodiscard]] Mat3 RotZ(float a) { return { { { std::cos(a), -std::sin(a), 0 }, { std::sin(a), std::cos(a), 0 }, { 0, 0, 1 } } }; }
}

ClashCamera* ClashCamera::GetSingleton()
{
	static ClashCamera singleton;
	return std::addressof(singleton);
}

void ClashCamera::InstallHooks()
{
	if (REL::Module::IsVR()) {
		logger::info("VR runtime: the clash camera is disabled (the HMD owns the camera)");
		return;
	}
	// TESCameraState::Update -- slot 3 on SE/AE (see RE/T/TESCameraState.h).
	stl::write_vfunc<RE::ThirdPersonState, ThirdPersonUpdateHook>(3);
	logger::info("Installed ThirdPersonState::Update hook");
}

void ClashCamera::OnSmoothCamInterface(void* a_interface, SmoothCamAPI::InterfaceVersion a_version)
{
	_smoothCam = static_cast<SmoothCamAPI::IVSmoothCam1*>(a_interface);
	_smoothCamVersion = a_version;
	logger::info("Obtained SmoothCam API (interface version {})", static_cast<int>(a_version) + 1);
}

bool ClashCamera::Begin()
{
	const auto settings = Settings::GetSingleton();
	// The plain [Camera] keys until a preset is chosen below; GetContactPoint
	// reads the aim height from here even when the camera itself is skipped.
	_framing = settings->cameraFraming;
	if (!settings->cameraEnabled || REL::Module::IsVR()) {
		return false;
	}

	const auto camera = RE::PlayerCamera::GetSingleton();
	if (!camera) {
		return false;
	}

	// A clash starting mid blend-out just takes over again.
	if (_releasing) {
		_releasing = false;
	}

	bool forcedThirdPerson = false;
	if (camera->IsInFirstPerson()) {
		forcedThirdPerson = camera->ForceThirdPerson() && settings->restoreFirstPerson;
	}

	if (_smoothCam && !_ownsSmoothCam) {
		const auto result = _smoothCam->RequestCameraControl(SKSE::GetPluginHandle());
		_ownsSmoothCam = result == SmoothCamAPI::APIResult::OK || result == SmoothCamAPI::APIResult::AlreadyGiven;
		logger::debug("cam: SmoothCam RequestCameraControl -> {}", ToString(result));

		if (!_ownsSmoothCam) {
			bool smoothCamRunning = true;
			if (_smoothCamVersion >= SmoothCamAPI::InterfaceVersion::V2) {
				smoothCamRunning = static_cast<SmoothCamAPI::IVSmoothCam2*>(_smoothCam)->IsCameraEnabled();
			}
			if (smoothCamRunning) {
				logger::warn("SmoothCam refused camera control ({}); skipping the clash camera", ToString(result));
				return forcedThirdPerson;
			}
		}
	}

	if (const auto player = RE::PlayerCharacter::GetSingleton()) {
		_framing = ChooseFraming(player);
	}

	_active = true;
	_haveStart = false;
	_blendTime = 0.0f;
	_lastLogTime = 0.0f;

	// Remember whatever FOV is in effect (vanilla, SmoothCam zoom, an FOV mod)
	// so it can be handed back untouched.
	if (_framing.fov > 0.0f && !_fovSaved) {
		_savedFOV = camera->GetRuntimeData2().worldFOV;
		_lastFOV = _savedFOV;
		_fovSaved = true;
	}
	return forcedThirdPerson;
}

void ClashCamera::ApplyFOV(float a_fov)
{
	if (const auto camera = RE::PlayerCamera::GetSingleton()) {
		camera->GetRuntimeData2().worldFOV = a_fov;
		_lastFOV = a_fov;
	}
}

void ClashCamera::RestoreFOV()
{
	if (!_fovSaved) {
		return;
	}
	_fovSaved = false;
	if (const auto camera = RE::PlayerCamera::GetSingleton()) {
		camera->GetRuntimeData2().worldFOV = _savedFOV;
	}
}

void ClashCamera::End()
{
	if (!_active) {
		if (_ownsSmoothCam) {
			FinishRelease();
		}
		return;
	}
	_active = false;
	_releasing = true;
	_releaseTime = 0.0f;
	_releaseStartFOV = _lastFOV;
	_releaseStartYaw = _lastYaw;
	_releaseStartPitch = _lastPitch;
}

void ClashCamera::FinishRelease()
{
	_active = false;
	_releasing = false;
	_haveStart = false;
	RestoreFOV();

	if (_ownsSmoothCam && _smoothCam) {
		const auto handle = SKSE::GetPluginHandle();
		if (_smoothCamVersion >= SmoothCamAPI::InterfaceVersion::V2) {
			static_cast<SmoothCamAPI::IVSmoothCam2*>(_smoothCam)->SendToGoalPosition(handle, true, false, RE::PlayerCharacter::GetSingleton());
		}
		_smoothCam->ReleaseCameraControl(handle);
	}
	_ownsSmoothCam = false;
}

void ClashCamera::RestoreFirstPerson()
{
	if (_releasing) {
		FinishRelease();
	}
	if (const auto camera = RE::PlayerCamera::GetSingleton()) {
		camera->ForceFirstPerson();
	}
}

void ClashCamera::BeforeThirdPersonUpdate(RE::ThirdPersonState* a_state)
{
	if (!_active || !a_state || !IsCurrentState(a_state)) {
		return;
	}
	// No free-rotation offsets at all: while blocking, the engine turns the
	// player toward the camera state's yaw every frame, so that yaw must equal
	// the player's own heading. The shot's aim is written to the camera nodes
	// directly afterwards instead.
	a_state->freeRotationEnabled = true;
	a_state->freeRotation.x = 0.0f;
	a_state->freeRotation.y = 0.0f;
}

void ClashCamera::ComputeAim(RE::Actor* a_player, const RE::NiPoint3& a_cameraPos, float& a_yaw, float& a_pitch) const
{
	a_yaw = a_player->GetAngleZ() + _framing.yawDegrees * kDegToRad;
	a_pitch = a_player->GetAngleX() + _framing.pitchDegrees * kDegToRad;

	RE::NiPoint3 contact;
	if (_framing.aimAtContact && ClashController::GetSingleton()->GetContactPoint(contact)) {
		const auto  delta = contact - a_cameraPos;
		const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
		a_yaw = std::atan2(delta.x, delta.y) + _framing.yawDegrees * kDegToRad;
		a_pitch = -std::atan2(delta.z, flat) + _framing.pitchDegrees * kDegToRad;  // positive = down
	}
}

void ClashCamera::AfterThirdPersonUpdate(RE::ThirdPersonState* a_state)
{
	if ((!_active && !_releasing) || !a_state) {
		return;
	}
	if (!IsCurrentState(a_state)) {
		if (_releasing) {
			FinishRelease();
		}
		return;
	}

	const auto settings = Settings::GetSingleton();
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!player) {
		return;
	}

	if (_releasing) {
		// Blend from where we left the camera to wherever the engine's own
		// camera is now (position and its view along the player), then hand
		// control back.
		_releaseTime += RealDelta();
		const float t = SmoothStep(_releaseTime / _framing.blendOut);
		ApplyPosition(a_state, Lerp(_lastPosition, a_state->translation, t));
		ApplyRotation(LerpAngle(_releaseStartYaw, player->GetAngleZ(), t), LerpAngle(_releaseStartPitch, player->GetAngleX(), t));
		if (_fovSaved) {
			ApplyFOV(_releaseStartFOV + (_savedFOV - _releaseStartFOV) * t);
		}
		if (t >= 1.0f) {
			FinishRelease();
		}
		return;
	}

	const auto target = ComputeTarget(player);

	if (!_haveStart) {
		_startTranslation = a_state->translation;
		_startYaw = player->GetAngleZ();
		_startPitch = player->GetAngleX();
		_haveStart = true;
	}

	_blendTime += RealDelta();
	const float t = SmoothStep(_blendTime / _framing.blendIn);
	const auto  position = Lerp(_startTranslation, target, t);

	float aimYaw = 0.0f;
	float aimPitch = 0.0f;
	ComputeAim(player, position, aimYaw, aimPitch);
	const float yaw = LerpAngle(_startYaw, aimYaw, t);
	const float pitch = LerpAngle(_startPitch, aimPitch, t);

	if (settings->debugLog && (_lastLogTime == 0.0f || _blendTime - _lastLogTime >= 0.5f)) {
		_lastLogTime = _blendTime;
		const auto  p = player->GetPosition();
		const auto& v = a_state->translation;
		logger::debug("cam: engine=({:.0f},{:.0f},{:.0f}) ours=({:.0f},{:.0f},{:.0f}) player=({:.0f},{:.0f},{:.0f}) playerAngle=({:.1f},{:.1f}) aim=({:.1f},{:.1f}) deg",
			v.x, v.y, v.z, position.x, position.y, position.z, p.x, p.y, p.z,
			player->GetAngleX() / kDegToRad, player->GetAngleZ() / kDegToRad, pitch / kDegToRad, yaw / kDegToRad);
	}

	ApplyPosition(a_state, position);
	ApplyRotation(yaw, pitch);

	if (_fovSaved && _framing.fov > 0.0f) {
		ApplyFOV(_savedFOV + (_framing.fov - _savedFOV) * t);
	}
}

// Mirrors SmoothCam's Camera::SetPosition: the state, the camera root and the
// NiCamera child all get the same world position.
void ClashCamera::ApplyPosition(RE::ThirdPersonState* a_state, const RE::NiPoint3& a_position)
{
	_lastPosition = a_position;
	a_state->translation = a_position;

	const auto camera = RE::PlayerCamera::GetSingleton();
	if (!camera || !camera->cameraRoot) {
		return;
	}

	const auto root = camera->cameraRoot.get();
	root->local.translate = a_position;
	root->world.translate = a_position;

	for (auto& child : root->GetChildren()) {
		if (const auto niCamera = child ? netimmerse_cast<RE::NiCamera*>(child.get()) : nullptr) {
			niCamera->world.translate = a_position;
		}
	}
}

// Mirrors SmoothCam's Thirdperson::SetCameraRotation: the NiCamera's world
// rotation is Rx(-90) * Ry(-pitch - 90) * Rz(yaw - 90) transposed into
// NiMatrix3 layout, and the camera root's rotation is that matrix with its
// columns rotated one place.
void ClashCamera::ApplyRotation(float a_yaw, float a_pitch)
{
	_lastYaw = a_yaw;
	_lastPitch = a_pitch;

	const auto camera = RE::PlayerCamera::GetSingleton();
	if (!camera || !camera->cameraRoot) {
		return;
	}

	// SmoothCam's own form has an extra -90 degrees on the pitch term because its
	// pitch value comes out of its Euler conversion already offset by that much.
	// With pitch as a plain angle (positive = down) the NiCamera columns are
	// forward, up, right; with the extra offset the first column points down.
	const Mat3 m = Mul(Mul(RotX(-kHalfPi), RotY(-a_pitch)), RotZ(a_yaw - kHalfPi));

	RE::NiMatrix3 niCameraRotation;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			niCameraRotation.entry[i][j] = m[j][i];  // glm column-major -> NiMatrix3 entry[col][row]
		}
	}

	RE::NiMatrix3 rootRotation;
	for (int r = 0; r < 3; ++r) {
		rootRotation.entry[r][1] = niCameraRotation.entry[r][0];
		rootRotation.entry[r][2] = niCameraRotation.entry[r][1];
		rootRotation.entry[r][0] = niCameraRotation.entry[r][2];
	}

	const auto root = camera->cameraRoot.get();
	root->local.rotate = rootRotation;
	root->world.rotate = rootRotation;

	for (auto& child : root->GetChildren()) {
		if (const auto niCamera = child ? netimmerse_cast<RE::NiCamera*>(child.get()) : nullptr) {
			niCamera->world.rotate = niCameraRotation;
		}
	}
}

namespace
{
	// The camera position a framing puts at a player standing at a_position
	// with heading a_yaw: purely the offsets in the player's frame; the aim is
	// computed separately (see ComputeAim).
	// Heading 0 = +Y, clockwise positive: forward = (sin, cos), right = (cos, -sin).
	[[nodiscard]] RE::NiPoint3 FramingPosition(const RE::NiPoint3& a_position, float a_yaw, float a_scale, const Settings::CameraFraming& a_framing)
	{
		const RE::NiPoint3 forward{ std::sin(a_yaw), std::cos(a_yaw), 0.0f };
		const RE::NiPoint3 right{ std::cos(a_yaw), -std::sin(a_yaw), 0.0f };
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };

		return a_position +
		       right * (a_framing.offsetX * a_scale) +
		       forward * (a_framing.offsetY * a_scale) +
		       up * (a_framing.offsetZ * a_scale);
	}

	// True when the ray from a_from to a_to, extended a_margin units past a_to,
	// hits anything but an actor. Cast on the line-of-sight layer with the
	// player's own collision group so their body is ignored; other actors (the
	// opponent in particular, who stands between the contact point and any
	// reverse-angle shot) are stepped through.
	[[nodiscard]] bool RayBlocked(RE::Actor* a_player, const RE::NiPoint3& a_from, RE::NiPoint3 a_to, float a_margin)
	{
		const auto cell = a_player->GetParentCell();
		const auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			return false;
		}

		auto        direction = a_to - a_from;
		const float length = direction.Length();
		if (length < 1.0f) {
			return false;
		}
		direction *= 1.0f / length;
		a_to += direction * a_margin;

		RE::CFilter playerFilter;
		a_player->GetCollisionFilterInfo(playerFilter);
		const std::uint32_t filter = (playerFilter.filter & 0xFFFF0000u) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);

		const float  toHavok = RE::bhkWorld::GetWorldScale();
		RE::NiPoint3 start = a_from;
		for (int pass = 0; pass < 4; ++pass) {
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(start * toHavok);
			pick.rayInput.to = RE::hkVector4(a_to * toHavok);
			pick.rayInput.enableShapeCollectionFilter = false;
			pick.rayInput.filterInfo.filter = filter;
			{
				RE::BSReadLockGuard lock(world->worldLock);
				world->PickObject(pick);
			}
			if (!pick.rayOutput.HasHit()) {
				return false;
			}

			const auto ref = pick.rayOutput.rootCollidable ? RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable) : nullptr;
			if (!ref || !ref->As<RE::Actor>()) {
				return true;
			}

			// An actor is in the way: carry on from just past the hit.
			const auto hit = start + (a_to - start) * pick.rayOutput.hitFraction;
			start = hit + direction * 5.0f;
			const auto remaining = a_to - start;
			if (remaining.x * direction.x + remaining.y * direction.y + remaining.z * direction.z <= 0.0f) {
				return false;
			}
		}
		return false;
	}
}

RE::NiPoint3 ClashCamera::ComputeTarget(RE::Actor* a_player) const
{
	return FramingPosition(a_player->GetPosition(), a_player->GetAngleZ(), a_player->GetScale(), _framing);
}

Settings::CameraFraming ClashCamera::ChooseFraming(RE::Actor* a_player) const
{
	const auto  settings = Settings::GetSingleton();
	const auto& presets = settings->cameraPresets;
	if (presets.empty()) {
		return settings->cameraFraming;
	}

	// The standoff geometry is fixed by now (SetupGeometry and FaceEachOther
	// run before Begin), so the check uses where the player will stand once
	// the approach slide finishes, not where they are this frame.
	ClashController::StandoffPose pose;
	if (!ClashController::GetSingleton()->GetStandoffPose(pose)) {
		pose.playerPosition = a_player->GetPosition();
		pose.playerHeading = a_player->GetAngleZ();
		pose.contactBase = pose.playerPosition;
	}
	const float scale = a_player->GetScale();

	static std::mt19937      rng{ std::random_device{}() };
	std::vector<std::size_t> order(presets.size());
	for (std::size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::shuffle(order.begin(), order.end(), rng);

	for (const auto index : order) {
		const auto& preset = presets[index];
		const auto  cameraPos = FramingPosition(pose.playerPosition, pose.playerHeading, scale, preset.framing);
		auto        from = pose.contactBase;
		from.z += preset.framing.aimHeight * scale;

		if (settings->cameraWallMargin > 0.0f && RayBlocked(a_player, from, cameraPos, settings->cameraWallMargin)) {
			logger::info("cam: preset '{}' is walled off at ({:.0f}, {:.0f}, {:.0f}); trying another", preset.name, cameraPos.x, cameraPos.y, cameraPos.z);
			continue;
		}
		logger::info("cam: preset '{}' chosen", preset.name);
		return preset.framing;
	}

	logger::info("cam: every listed preset is walled off; using '{}'", presets.front().name);
	return presets.front().framing;
}
