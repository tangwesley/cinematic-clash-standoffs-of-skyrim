#include "Settings.h"

#include "IniFile.h"

#include <SimpleIni.h>

namespace
{
	using SD = SettingsData;
	using CF = SettingsData::CameraFraming;
	constexpr float kInf = Settings::kUnbounded;

	template <class... Ts>
	struct Overloaded : Ts...
	{
		using Ts::operator()...;
	};

	constexpr const char* kDifficultyChoices[] = { "Easy", "Normal", "Hard" };
	constexpr const char* kShieldChoices[] = { "Leave alone", "Hide the shield model" };
	constexpr const char* kTimeoutChoices[] = { "Draw", "The side the meter favours wins" };

	// ---------------------------------------------------------------------
	// The key table. Order is the INI's order; the help text is the INI's
	// comment, shortened, and is what the menu shows as a tooltip.
	// ---------------------------------------------------------------------
	const Settings::Entry kEntries[] = {
		// [General]
		{ "General", "bEnabled", &SD::enabled, 0, 1, "Enabled", "Master switch." },
		{ "General", "fTriggerChance", &SD::triggerChance, 0, 100, "Trigger chance (%)", "Percent chance that a detected weapon clash turns into a cinematic standoff." },
		{ "General", "fCooldownSeconds", &SD::cooldownSeconds, 0, kInf, "Cooldown (s)", "Minimum seconds between two standoffs." },
		{ "General", "bRequireHostile", &SD::requireHostile, 0, 1, "Require hostile", "Only clash with NPCs that are hostile to the player." },
		{ "General", "bBossesOnly", &SD::bossesOnly, 0, 1, "Bosses only", "Only clash with opponents recognised as a boss: the races, NPCs and location boss markers listed in SKSE/Plugins/CinematicClash/BossRecognition/*.ini, plus TrueHUD's own lists when it is installed. TrueHUD is not required." },
		{ "General", "fMaxStartDistance", &SD::maxStartDistance, 0, kInf, "Max start distance", "A standoff will not start if the two actors are further apart than this (units)." },

		// [Standoff]
		{ "Standoff", "fDuration", &SD::duration, 0.25f, kInf, "Duration (s)", "Seconds the quick time event lasts. Reaching the end without either side pushed off the meter is a draw, or a win for the side the meter favours -- see Timer runs out, under Outcome." },
		{ "Standoff", "fPressGain", &SD::pressGain, 0, kInf, "Press gain", "Meter gained per attack press (meter runs 0..1, starts at 0.5). How hard the opponent pushes back is set under Difficulty." },
		{ "Standoff", "bStaminaAffectsPlayer", &SD::staminaAffectsPlayer, 0, 1, "Stamina affects player", "Low player stamina weakens each press (down to 50% at empty)." },
		{ "Standoff", "bStaminaAffectsNpc", &SD::staminaAffectsNpc, 0, 1, "Stamina affects NPC", "Low NPC stamina weakens its push (down to 50% at empty)." },
		{ "Standoff", "fStaminaCostPerPress", &SD::staminaCostPerPress, 0, kInf, "Stamina cost per press", "Player stamina spent per press." },
		{ "Standoff", "fClashDistance", &SD::clashDistance, 20, kInf, "Clash distance", "Distance between the two actors while locked together (units). Ignored while Solve clash distance is on and the solve succeeds." },
		{ "Standoff", "bSolveClashDistance", &SD::solveClashDistance, 0, 1, "Solve clash distance", "Measure both weapons off their own meshes once the block pose is up and stand the pair at the distance that makes the two blades touch, instead of the fixed Clash distance. When no distance in the range below makes them meet (the miss is sideways or vertical, which sliding the pair cannot fix) Clash distance is used and the reason is logged." },
		{ "Standoff", "fSolveDistanceMin", &SD::solveDistanceMin, 20, kInf, "Solve distance min", "Closest the solve may put the two actors (units). Two daggers would otherwise solve to inside each other." },
		{ "Standoff", "fSolveDistanceMax", &SD::solveDistanceMax, 20, kInf, "Solve distance max", "Furthest the solve may put the two actors (units). Two greatswords would otherwise solve to arm's length apart." },
		{ "Standoff", "fSolveBite", &SD::solveBite, 0, kInf, "Solve bite", "Units closer than first contact, so the blades visibly cross instead of just touching." },
		{ "Standoff", "bTiltToMeet", &SD::tiltToMeet, 0, 1, "Tilt to meet", "When the two blades miss each other vertically -- a height or scale difference, or two block poses that hold the guard at different heights -- bend the fighter whose blade sits higher down towards the other one until they cross. Half the movement comes from their spine and half from their sword arm's shoulder. Nothing to do with a sideways miss, and it does nothing at all when the blades already meet." },
		{ "Standoff", "fTiltMaxDegrees", &SD::tiltMaxDegrees, 0, 45, "Tilt max (deg)", "Most either joint may be turned. The spine and the shoulder each take half the correction, so the blade swings up to about twice this. Past 15 degrees or so the pose starts to look wrong; 0 = off." },
		{ "Standoff", "bClearWeaponClipping", &SD::clearWeaponClipping, 0, 1, "Clear weapon clipping", "Some block animations swing the weapon round far enough that the blade ends up inside the other fighter. When that happens the blade is turned back out of them until it clears, at the wrist first and then at the weapon itself, and the correction eases off again as soon as the animation stops needing it." },
		{ "Standoff", "fClearanceMaxDegrees", &SD::clearanceMaxDegrees, 0, 60, "Clearance max (deg)", "Most either the wrist or the weapon may be turned to clear the other fighter. The wrist takes as much as it can first, because the hand turns with it; only what is left over goes to the weapon, where the grip visibly slips in the fist past 15 degrees or so. 0 = off." },
		{ "Standoff", "fClearanceBodyRadius", &SD::clearanceBodyRadius, 1, 60, "Clearance body radius", "How wide the other fighter is taken to be: the blade is kept this many units clear of the line from their hips to their head, scaled by their size. Larger keeps blades further out of the chest but turns them more." },
		{ "Standoff", "bContactFromWeapons", &SD::contactFromWeapons, 0, 1, "Contact from weapons", "Put the sparks, the scrape loop and the camera's aim at the point where the two measured blades are actually closest. Off, or when a weapon's mesh cannot be read, the contact point is the midpoint between the actors at [Sparks] fHeight / [Camera] fAimHeight above the ground." },
		{ "Standoff", "fPushDistance", &SD::pushDistance, 0, kInf, "Push distance", "How far the pair slides along the clash axis as the meter swings (units at meter 0 or 1). The slide can read as walking or turning to the movement system. 0 = off." },
		{ "Standoff", "fApproachTime", &SD::approachTime, 0.05f, kInf, "Approach time (s)", "Seconds to slide both actors into position." },
		{ "Standoff", "fSettleTime", &SD::settleTime, 0, kInf, "Settle time (s)", "Seconds after the clash begins during which the actors keep being turned to face each other. After that their headings freeze and head/spine tracking is switched off." },
		{ "Standoff", "bFreezeAfterSettle", &SD::freezeAfterSettle, 0, 1, "Freeze after settle", "Once settled and both actors are in the block, stop the opponent's animation graph so they hold the pose dead still until the resolve. Off by default: idle motion reads better." },
		{ "Standoff", "bDisableCollision", &SD::disableCollision, 0, 1, "Disable collision", "While locked, the two actors stop colliding with each other; their character controllers otherwise push each other apart at close range. With Precision installed only that pair is affected." },
		{ "Standoff", "fTimeMultiplier", &SD::timeMultiplier, 0.1f, 1, "Time multiplier", "Global time multiplier during the standoff. 1 = no slow motion." },
		{ "Standoff", "iShieldMode", &SD::shieldMode, 0, 1, "Shields", "What happens to a shield during a clash. Which block animation plays is handled by the shipped Open Animation Replacer folder.", kShieldChoices },
		{ "Standoff", "bShakeOnPress", &SD::shakeOnPress, 0, 1, "Shake on press", "Small camera shake on every press for feedback." },
		{ "Standoff", "fShakeStrength", &SD::shakeStrength, 0, kInf, "Shake strength", "Strength of the per-press camera shake." },

		// [Difficulty]
		{ "Difficulty", "iMode", &SD::difficultyMode, 0, 2, "Difficulty", "How hard the opponent pushes back: the number of attack presses per second that exactly holds the meter still against an opponent of equal weapon skill. Press faster and the meter climbs to your end, slower and it falls to theirs.", kDifficultyChoices },
		{ "Difficulty", "fEasyPressesPerSecond", &SD::easyPressesPerSecond, 0, 30, "Easy presses per second", "Presses per second that hold the meter on Easy." },
		{ "Difficulty", "fNormalPressesPerSecond", &SD::normalPressesPerSecond, 0, 30, "Normal presses per second", "Presses per second that hold the meter on Normal." },
		{ "Difficulty", "fHardPressesPerSecond", &SD::hardPressesPerSecond, 0, 30, "Hard presses per second", "Presses per second that hold the meter on Hard." },
		{ "Difficulty", "fSkillInfluence", &SD::skillInfluence, 0, 1, "Skill influence", "The two governing weapon skills (One-Handed or Two-Handed, whichever each weapon uses, buffs included) scale the push: a better-skilled opponent pushes harder, a worse one softer. 0 = ignore skills, 1 = up to +/-100% at the extremes." },
		{ "Difficulty", "fAutoWinSkillGap", &SD::autoWinSkillGap, 0, kInf, "Auto-win skill gap", "An opponent whose weapon skill is at least this many points below yours never gets a standoff: the clash resolves on the spot and they take the loser's stagger. The reverse never applies. 0 = off." },
		{ "Difficulty", "bHoldToMash", &SD::holdToMash, 0, 1, "Hold to mash", "Accessibility: hold the attack button instead of mashing it. Held time counts as presses at the chosen difficulty's rate, so a held button exactly matches an equal opponent and the skill gap decides." },

		// [Rumble]
		{ "Rumble", "bEnabled", &SD::rumbleEnabled, 0, 1, "Enabled", "Controller rumble while the weapons are locked (XInput pads, including anything Steam Input presents as one)." },
		{ "Rumble", "fLargeMotor", &SD::rumbleLargeMotor, 0, 1, "Large motor", "Level held on the large (low-frequency) motor for the standoff." },
		{ "Rumble", "fSmallMotor", &SD::rumbleSmallMotor, 0, 1, "Small motor", "Level held on the small (high-frequency) motor for the standoff." },
		{ "Rumble", "fPressPulse", &SD::rumblePressPulse, 0, 1, "Press pulse", "Extra kick on the small motor for every counted press. 0 = off." },
		{ "Rumble", "fPulseDuration", &SD::rumblePulseDuration, 0, 1, "Pulse duration (s)", "How long each press kick lasts." },
		{ "Rumble", "bRespectGameSetting", &SD::rumbleRespectGameSetting, 0, 1, "Respect game setting", "Stay silent when the game's own controller rumble is switched off in its settings." },

		// [Camera]
		{ "Camera", "bEnabled", &SD::cameraEnabled, 0, 1, "Enabled", "Move the camera behind the player's shoulder for the standoff." },
		{ "Camera", "", std::monostate{}, 0, 0, "Framing", "The shot: the keys every camera preset can override." },
		{ "Camera", "bForceThirdPerson", &SD::forceThirdPerson, 0, 1, "Force third person", "When a clash starts in first person, switch to third person for the shoulder shot. Off: stay in first person for the whole standoff (the block is held and movement locked as usual, and no camera shot is applied)." },
		{ "Camera", "bRestoreFirstPerson", &SD::restoreFirstPerson, 0, 1, "Restore first person", "If the clash started in first person and was forced to third person, go back to first person afterwards." },
		{ "Camera", "fFirstPersonRestoreDelay", &SD::firstPersonRestoreDelay, 0, kInf, "First person restore delay (s)", "Seconds after the clash before first person is restored." },
		{ "Camera", "sPreset", &SD::cameraPresetList, 0, 0, "Preset pool", "Comma-separated preset names from CinematicClash_CameraPresets.ini. Every clash picks one at random; a preset whose camera position is walled off is skipped, and when every one is the first listed is used. One name = always that shot. Empty = the keys above as written. The list below edits this." },
		{ "Camera", "fWallMargin", &SD::cameraWallMargin, 0, kInf, "Wall margin", "Clearance a shot needs: a ray from the point where the weapons meet to the camera, extended by this many units, must not hit anything but the two fighters. 0 = never check." },

		// [Sparks]
		{ "Sparks", "bEnabled", &SD::sparksEnabled, 0, 1, "Enabled", "Continuous sparks at the point of contact while the weapons are locked." },
		{ "Sparks", "fInterval", &SD::sparksInterval, 0.02f, 2, "Interval (s)", "Seconds between bursts." },
		{ "Sparks", "fHeight", &SD::sparksHeight, -kInf, kInf, "Height", "Height of the contact point above the ground, scaled by the player's size." },
		{ "Sparks", "fScale", &SD::sparksScale, 0.1f, 10, "Scale", "Size of each burst." },
		{ "Sparks", "fJitter", &SD::sparksJitter, 0, kInf, "Jitter", "Random offset per burst in units, so the shower moves around a little." },
		{ "Sparks", "sModel", &SD::sparksModel, 0, 0, "Model", "Optional .nif path (relative to Data\\meshes) to use instead of the weapon's own block-impact spark model, e.g. effects\\impacteffects\\impactmetalsparks01.nif" },

		// [Audio]
		{ "Audio", "bEnabled", &SD::audioEnabled, 0, 1, "Enabled", "Each sound is either a sound descriptor editor ID (game audio, positioned in 3D) or a loose .wav path relative to Data (PCM 16-bit, 44.1 kHz), played on the plugin's own voices. Leave a sound empty to use the weapon's own block-impact sounds." },
		{ "Audio", "sImpactSound", &SD::impactSound, 0, 0, "Impact sound", "One shot when the weapons lock." },
		{ "Audio", "fImpactVolume", &SD::impactVolume, 0, 1, "Impact volume", "Volume of the impact sound." },
		{ "Audio", "sLoopSound", &SD::loopSound, 0, 0, "Loop sound", "Kept going at the contact point for the whole standoff. A looping descriptor plays once; a one-shot descriptor is re-triggered every loop interval." },
		{ "Audio", "fLoopInterval", &SD::loopInterval, 0.05f, 5, "Loop interval (s)", "Re-trigger interval for a one-shot loop sound." },
		{ "Audio", "fLoopVolume", &SD::loopVolume, 0, 1, "Loop volume", "Volume of the loop sound." },
		{ "Audio", "sOverpowerSound", &SD::overpowerSound, 0, 0, "Overpower sound", "One shot when the standoff resolves." },
		{ "Audio", "fOverpowerVolume", &SD::overpowerVolume, 0, 1, "Overpower volume", "Volume of the overpower sound." },
		{ "Audio", "bEnemyShout", &SD::enemyShout, 0, 1, "Enemy shout", "The opponent shouts a combat taunt in their own voice when the clash starts, through the dialogue system." },
		{ "Audio", "sShoutTopic", &SD::shoutTopic, 0, 0, "Shout topic", "Dialogue topic editor ID for the taunt. Empty = the generic combat taunt." },
		{ "Audio", "sShoutSound", &SD::shoutSound, 0, 0, "Shout sound", "Sound descriptor to play instead of a voice line." },

		// [HUD]
		{ "HUD", "bEnabled", &SD::hudEnabled, 0, 1, "Enabled", "Draw the tug-of-war meter at the bottom centre of the screen during a standoff." },
		{ "HUD", "fScale", &SD::hudScale, 0.25f, 4, "Scale", "Size multiplier for the meter and its text." },
		{ "HUD", "fVerticalPosition", &SD::hudVerticalPosition, 0.05f, 0.98f, "Vertical position", "Vertical position of the bar: 0 = top of the screen, 1 = bottom." },
		{ "HUD", "sTextureFolder", &SD::hudTextureFolder, 0, 0, "Texture folder", "Optional PNG skin folder under Data: frame.png, fill_player.png, fill_opponent.png, marker.png, timer.png and key_<LABEL>.png replace the drawn elements; missing files keep the built-in drawing. See README.txt in the folder." },
		{ "HUD", "iTextureGridWidth", &SD::hudTextureGridWidth, 64, 16384, "Texture grid width", "The pixel grid the skin was drawn on: a PNG this many pixels wide is shown as wide as the bar (32% of the screen, times the scale)." },
		{ "HUD", "sFontFile", &SD::hudFontFile, 0, 0, "Font file", "A .ttf path relative to Data (for example Interface\\Fonts\\MyFont.ttf). Empty = the same font the game's own UI uses." },
		{ "HUD", "sGameFont", &SD::hudGameFont, 0, 0, "Game font", "Which Interface\\fontconfig.txt mapping to follow when the font file is empty: $EverywhereFont, $EverywhereBoldFont, $StartMenuFont or $DialogueFont." },

		// [Outcome]
		{ "Outcome", "fLoserStaggerMagnitude", &SD::loserStaggerMagnitude, 0, 1, "Loser stagger", "Stagger magnitude sent to the loser (1.0 = the large stagger, 0 = none)." },
		{ "Outcome", "fWinnerStaggerMagnitude", &SD::winnerStaggerMagnitude, 0, 1, "Winner stagger", "Stagger magnitude sent to the winner (0 = none)." },
		{ "Outcome", "fDrawStaggerMagnitude", &SD::drawStaggerMagnitude, 0, 1, "Draw stagger", "Stagger both actors take when the clash ends in a draw (0.25 = the small stagger, 0 = none)." },
		{ "Outcome", "iTimeoutResolution", &SD::timeoutResolution, 0, 1, "Timer runs out", "What a standoff nobody pushed off the meter becomes when the timer runs out. Draw: both break off with the draw stagger and neither counts as winner or loser. The side the meter favours wins: whichever end the marker sits nearer to takes the win, with the usual winner and loser staggers. A marker dead on the centre is a draw either way.", kTimeoutChoices },
		{ "Outcome", "fOutcomeWindow", &SD::outcomeWindow, 0, kInf, "Outcome window (s)", "Seconds the CinematicClash_IsClashWinner / _IsClashLoser OAR conditions stay true." },

		// [Messages]
		{ "Messages", "bEnabled", &SD::messagesEnabled, 0, 1, "Enabled", "Show the HUD notifications below. Off = no message for any clash event." },
		{ "Messages", "sStart", &SD::messageStart, 0, 0, "Start", "HUD notification when a standoff starts. Empty = show nothing." },
		{ "Messages", "sStartHold", &SD::messageStartHold, 0, 0, "Start (hold to mash)", "Shown instead of Start when Hold to mash is on." },
		{ "Messages", "sAutoWin", &SD::messageAutoWin, 0, 0, "Auto-win", "Shown when the opponent is skipped past the standoff by the auto-win skill gap." },
		{ "Messages", "sWin", &SD::messageWin, 0, 0, "Win", "Shown when you overpower the opponent." },
		{ "Messages", "sLose", &SD::messageLose, 0, 0, "Lose", "Shown when the opponent overpowers you." },
		{ "Messages", "sDraw", &SD::messageDraw, 0, 0, "Draw", "Shown when the timer runs out and both break off." },
		{ "Messages", "sPressSound", &SD::pressSound, 0, 0, "Press sound", "Sound descriptor editor ID played on every press (e.g. WPNBlockWeaponBlade). Empty = none." },

		// [Debug]
		{ "Debug", "bDebugLog", &SD::debugLog, 0, 1, "Debug log", "Verbose logging to Documents\\My Games\\Skyrim Special Edition\\SKSE\\CinematicClash.log" },
		{ "Debug", "bForceClashOnHit", &SD::forceClashOnHit, 0, 1, "Force clash on hit", "Testing aid: every melee hit the player lands on a humanoid NPC starts a clash, skipping the weapon-parry check, hostility, bosses-only and chance. Cooldown still applies. Leave off for normal play." },
	};

	const Settings::FramingEntry kFramingEntries[] = {
		{ "fOffsetX", &CF::offsetX, -kInf, kInf, "Offset X", "Camera position from the player's feet, in the player's own frame and scaled by their size. X = right (negative = left)." },
		{ "fOffsetY", &CF::offsetY, -kInf, kInf, "Offset Y", "Y = forward (negative = behind the player)." },
		{ "fOffsetZ", &CF::offsetZ, -kInf, kInf, "Offset Z", "Z = up from the feet." },
		{ "bAimAtContact", &CF::aimAtContact, 0, 1, "Aim at contact", "Aim the camera at the point where the weapons meet, so any position frames the fight without hand-tuning angles." },
		{ "fAimHeight", &CF::aimHeight, -kInf, kInf, "Aim height", "Height of the contact point above the ground that the camera looks at." },
		{ "fYawDegrees", &CF::yawDegrees, -180, 180, "Yaw (deg)", "Aim adjustment left/right (positive = right). With Aim at contact on this nudges the aim; with it off it is the whole aim, relative to the player's heading." },
		{ "fPitchDegrees", &CF::pitchDegrees, -80, 80, "Pitch (deg)", "Aim adjustment up/down (positive = tilts down)." },
		{ "fFOV", &CF::fov, 0, 150, "Field of view", "World field of view during the clash (20..150), blended in and out with the camera and restored exactly afterwards. 0 = leave the FOV alone." },
		{ "fBlendIn", &CF::blendIn, 0.01f, kInf, "Blend in (s)", "Seconds to blend from the current camera position into the shot." },
		{ "fBlendOut", &CF::blendOut, 0.01f, kInf, "Blend out (s)", "Seconds to blend back to the normal camera when the clash ends." },
	};

	float ReadFloat(const CSimpleIniA& a_ini, const char* a_section, const char* a_key, float a_default)
	{
		return static_cast<float>(a_ini.GetDoubleValue(a_section, a_key, static_cast<double>(a_default)));
	}

	std::string ReadString(const CSimpleIniA& a_ini, const char* a_section, const char* a_key, const std::string& a_default)
	{
		const auto* value = a_ini.GetValue(a_section, a_key, a_default.c_str());
		return value ? std::string{ value } : a_default;
	}

	std::string Trim(std::string a_value)
	{
		const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
		a_value.erase(a_value.begin(), std::find_if(a_value.begin(), a_value.end(), notSpace));
		a_value.erase(std::find_if(a_value.rbegin(), a_value.rend(), notSpace).base(), a_value.end());
		return a_value;
	}

	bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		return a_lhs.size() == a_rhs.size() &&
		       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](unsigned char l, unsigned char r) {
				   return std::tolower(l) == std::tolower(r);
			   });
	}

	// Shortest text that reads back as the same float: 0.09, 100, -70.
	std::string FormatFloat(float a_value)
	{
		return std::format("{}", a_value);
	}

	std::vector<std::string> SplitList(const std::string& a_list)
	{
		std::vector<std::string> names;
		std::size_t              start = 0;
		while (start <= a_list.size()) {
			const auto end = a_list.find(',', start);
			auto       name = Trim(a_list.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (!name.empty()) {
				names.push_back(std::move(name));
			}
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
		return names;
	}

	std::string JoinList(const std::vector<std::string>& a_names)
	{
		std::string list;
		for (const auto& name : a_names) {
			if (!list.empty()) {
				list += ", ";
			}
			list += name;
		}
		return list;
	}

	// The framing keys shared by [Camera] in the main INI and by every preset
	// section in CinematicClash_CameraPresets.ini. Keys the section lacks keep their value.
	void ReadCameraFraming(CF& a_framing, const CSimpleIniA& a_ini, const char* a_section)
	{
		for (const auto& entry : Settings::FramingEntries()) {
			std::visit(Overloaded{
						   [&](bool CF::*a_member) {
							   a_framing.*a_member = a_ini.GetBoolValue(a_section, entry.key, a_framing.*a_member);
						   },
						   [&](float CF::*a_member) {
							   a_framing.*a_member = std::clamp(ReadFloat(a_ini, a_section, entry.key, a_framing.*a_member), entry.min, entry.max);
						   } },
				entry.member);
		}
		if (a_framing.fov != 0.0f) {
			a_framing.fov = std::clamp(a_framing.fov, 20.0f, 150.0f);
		}
	}

	void WriteCameraFraming(IniFile& a_file, std::string_view a_section, const CF& a_framing)
	{
		for (const auto& entry : Settings::FramingEntries()) {
			std::visit(Overloaded{
						   [&](bool CF::*a_member) { a_file.Set(a_section, entry.key, a_framing.*a_member ? "1" : "0"); },
						   [&](float CF::*a_member) { a_file.Set(a_section, entry.key, FormatFloat(a_framing.*a_member)); } },
				entry.member);
		}
	}
}

Settings* Settings::GetSingleton()
{
	static Settings singleton;
	return std::addressof(singleton);
}

std::span<const Settings::Entry> Settings::Entries()
{
	return kEntries;
}

std::span<const Settings::FramingEntry> Settings::FramingEntries()
{
	return kFramingEntries;
}

const SettingsData& Settings::Defaults()
{
	static const SettingsData defaults{};
	return defaults;
}

void Settings::Load()
{
	CSimpleIniA ini;
	ini.SetUnicode();

	const auto rc = ini.LoadFile(kConfigPath);
	if (rc < 0) {
		logger::warn("Could not read {} (error {}), using built-in defaults", kConfigPath, static_cast<int>(rc));
		Validate();
		BuildCameraPresets();
		ApplyLogLevel();
		return;
	}

	for (const auto& entry : Entries()) {
		std::visit(Overloaded{
					   [&](std::monostate) { ReadCameraFraming(cameraFraming, ini, entry.section); },
					   [&](bool SD::*a_member) { this->*a_member = ini.GetBoolValue(entry.section, entry.key, this->*a_member); },
					   [&](float SD::*a_member) {
						   this->*a_member = std::clamp(ReadFloat(ini, entry.section, entry.key, this->*a_member), entry.min, entry.max);
					   },
					   [&](int SD::*a_member) {
						   const auto value = static_cast<int>(ini.GetLongValue(entry.section, entry.key, this->*a_member));
						   this->*a_member = std::clamp(value, static_cast<int>(entry.min), static_cast<int>(entry.max));
					   },
					   [&](std::string SD::*a_member) { this->*a_member = ReadString(ini, entry.section, entry.key, this->*a_member); } },
			entry.member);
	}

	Validate();
	BuildCameraPresets();
	ApplyLogLevel();

	logger::info("Settings loaded from {}", kConfigPath);
	logger::info("  enabled={} chance={}% cooldown={}s duration={}s pressGain={}",
		enabled, triggerChance, cooldownSeconds, duration, pressGain);
	logger::info("  difficulty mode={} ({} presses/s) skillInfluence={} autoWinSkillGap={} holdToMash={} rumble={}",
		difficultyMode, PressesPerSecond(), skillInfluence, autoWinSkillGap, holdToMash, rumbleEnabled);
	logger::info("  camera enabled={} presets='{}' ({} usable of {}) wallMargin={} base offset=({}, {}, {}) aimHeight={} fov={} blend={}s/{}s timeMultiplier={}",
		cameraEnabled, cameraPresetList, cameraPresets.size(), cameraPresetLibrary.size(), cameraWallMargin, cameraFraming.offsetX, cameraFraming.offsetY,
		cameraFraming.offsetZ, cameraFraming.aimHeight, cameraFraming.fov, cameraFraming.blendIn, cameraFraming.blendOut, timeMultiplier);

	std::error_code ec;
	_lastWrite = std::filesystem::last_write_time(kConfigPath, ec);
}

void Settings::Validate()
{
	cameraPresetList = Trim(cameraPresetList);
	if (solveDistanceMax < solveDistanceMin) {
		std::swap(solveDistanceMin, solveDistanceMax);
	}
	if (hudGameFont.empty()) {
		hudGameFont = "$EverywhereFont";
	}
	if (forceClashOnHit) {
		logger::warn("Debug: bForceClashOnHit is on; every player melee hit on a humanoid starts a clash");
	}
}

void Settings::ApplyLogLevel() const
{
#ifdef NDEBUG
	constexpr auto base = spdlog::level::info;
#else
	constexpr auto base = spdlog::level::trace;
#endif
	const auto level = debugLog && base > spdlog::level::debug ? spdlog::level::debug : base;
	if (auto log = spdlog::default_logger()) {
		log->set_level(level);
	}
}

void Settings::BuildCameraPresets()
{
	cameraPresets.clear();
	cameraPresetLibrary.clear();

	// Stamp the presets file even when no preset is listed, so listing one
	// later and saving the file still triggers a reload.
	std::error_code ec;
	_presetsLastWrite = std::filesystem::last_write_time(kPresetsPath, ec);

	CSimpleIniA presets;
	presets.SetUnicode();
	const auto rc = presets.LoadFile(kPresetsPath);
	if (rc >= 0) {
		CSimpleIniA::TNamesDepend sections;
		presets.GetAllSections(sections);
		sections.sort(CSimpleIniA::Entry::LoadOrder());
		for (const auto& section : sections) {
			CameraPreset preset{ section.pItem, cameraFraming };
			ReadCameraFraming(preset.framing, presets, section.pItem);
			logger::debug("Camera preset '{}': offset=({}, {}, {}) aimHeight={} yaw={} pitch={} fov={} blend={}s/{}s",
				preset.name, preset.framing.offsetX, preset.framing.offsetY, preset.framing.offsetZ, preset.framing.aimHeight,
				preset.framing.yawDegrees, preset.framing.pitchDegrees, preset.framing.fov, preset.framing.blendIn, preset.framing.blendOut);
			cameraPresetLibrary.push_back(std::move(preset));
		}
	}

	const auto names = SplitList(cameraPresetList);
	if (names.empty()) {
		return;
	}
	if (rc < 0) {
		logger::warn("Camera presets '{}' requested but {} could not be read (error {}); using the [Camera] keys", cameraPresetList, kPresetsPath, static_cast<int>(rc));
		return;
	}

	for (const auto& name : names) {
		const auto it = std::find_if(cameraPresetLibrary.begin(), cameraPresetLibrary.end(), [&](const CameraPreset& a_preset) {
			return IEquals(a_preset.name, name);
		});
		if (it == cameraPresetLibrary.end()) {
			logger::warn("Camera preset '{}' not found in {}; skipped", name, kPresetsPath);
			continue;
		}
		cameraPresets.push_back(*it);
	}
	if (cameraPresets.empty()) {
		logger::warn("None of the listed camera presets exist in {}; using the [Camera] keys", kPresetsPath);
	}
}

float Settings::PressesPerSecond() const noexcept
{
	switch (difficultyMode) {
	case 0:
		return easyPressesPerSecond;
	case 2:
		return hardPressesPerSecond;
	default:
		return normalPressesPerSecond;
	}
}

bool Settings::ReloadIfChanged()
{
	std::scoped_lock lock(_ioMutex);

	std::error_code ec;
	const auto      stamp = std::filesystem::last_write_time(kConfigPath, ec);
	const bool      iniChanged = !ec && stamp != _lastWrite;

	std::error_code presetsEc;
	const auto      presetsStamp = std::filesystem::last_write_time(kPresetsPath, presetsEc);
	const bool      presetsChanged = !presetsEc && presetsStamp != _presetsLastWrite;

	if (!iniChanged && !presetsChanged) {
		return false;
	}
	logger::info("{} changed on disk; reloading", iniChanged ? kConfigPath : kPresetsPath);
	Load();
	return true;
}

// ---------------------------------------------------------------------------
// Writing back
// ---------------------------------------------------------------------------

bool Settings::Save()
{
	std::scoped_lock lock(_ioMutex);

	IniFile file;
	file.Load(kConfigPath);
	for (const auto& entry : Entries()) {
		std::visit(Overloaded{
					   [&](std::monostate) { WriteCameraFraming(file, entry.section, cameraFraming); },
					   [&](bool SD::*a_member) { file.Set(entry.section, entry.key, this->*a_member ? "1" : "0"); },
					   [&](float SD::*a_member) { file.Set(entry.section, entry.key, FormatFloat(this->*a_member)); },
					   [&](int SD::*a_member) { file.Set(entry.section, entry.key, std::to_string(this->*a_member)); },
					   [&](std::string SD::*a_member) { file.Set(entry.section, entry.key, this->*a_member); } },
			entry.member);
	}
	if (!file.Save(kConfigPath)) {
		logger::error("Could not write {}", kConfigPath);
		return false;
	}
	logger::info("Settings written to {}", kConfigPath);
	Load();
	return true;
}

void Settings::ResetSection(std::string_view a_section)
{
	const auto& defaults = Defaults();
	for (const auto& entry : Entries()) {
		if (!IEquals(entry.section, a_section)) {
			continue;
		}
		std::visit(Overloaded{
					   [&](std::monostate) { cameraFraming = defaults.cameraFraming; },
					   [&](auto SD::*a_member) { this->*a_member = defaults.*a_member; } },
			entry.member);
	}
}

bool Settings::SavePreset(const CameraPreset& a_preset)
{
	{
		std::scoped_lock lock(_ioMutex);

		IniFile file;
		file.Load(kPresetsPath);
		WriteCameraFraming(file, a_preset.name, a_preset.framing);
		if (!file.Save(kPresetsPath)) {
			logger::error("Could not write {}", kPresetsPath);
			return false;
		}
		logger::info("Camera preset '{}' written to {}", a_preset.name, kPresetsPath);
		Load();
	}
	return true;
}

bool Settings::AddPreset(std::string a_name)
{
	a_name = Trim(a_name);
	if (a_name.empty() || a_name.find_first_of("[],;=") != std::string::npos) {
		logger::warn("Camera preset name '{}' is empty or contains [ ] , ; =", a_name);
		return false;
	}
	const bool exists = std::any_of(cameraPresetLibrary.begin(), cameraPresetLibrary.end(), [&](const CameraPreset& a_preset) {
		return IEquals(a_preset.name, a_name);
	});
	if (exists) {
		logger::warn("Camera preset '{}' already exists", a_name);
		return false;
	}
	if (!SavePreset(CameraPreset{ a_name, cameraFraming })) {
		return false;
	}
	return SetPresetInPool(a_name, true);
}

bool Settings::RemovePreset(std::string_view a_name)
{
	{
		std::scoped_lock lock(_ioMutex);

		IniFile file;
		file.Load(kPresetsPath);
		if (!file.RemoveSection(a_name)) {
			logger::warn("Camera preset '{}' not found in {}", a_name, kPresetsPath);
			return false;
		}
		if (!file.Save(kPresetsPath)) {
			logger::error("Could not write {}", kPresetsPath);
			return false;
		}
		logger::info("Camera preset '{}' removed from {}", a_name, kPresetsPath);
	}
	return SetPresetInPool(a_name, false);
}

bool Settings::SetPresetInPool(std::string_view a_name, bool a_inPool)
{
	auto       names = SplitList(cameraPresetList);
	const auto match = [&](const std::string& a_entry) { return IEquals(a_entry, a_name); };
	const bool present = std::any_of(names.begin(), names.end(), match);
	if (a_inPool && !present) {
		names.emplace_back(a_name);
	} else if (!a_inPool && present) {
		names.erase(std::remove_if(names.begin(), names.end(), match), names.end());
	}
	cameraPresetList = JoinList(names);
	return Save();
}

bool Settings::IsPresetInPool(std::string_view a_name) const
{
	const auto names = SplitList(cameraPresetList);
	return std::any_of(names.begin(), names.end(), [&](const std::string& a_entry) { return IEquals(a_entry, a_name); });
}
