#pragma once

// Plugin configuration, read from Data/SKSE/Plugins/CinematicClash.ini and
// written back to it by the in-game menu (ClashMenu, through SKSE Menu
// Framework). Every INI key is described once, in the table behind
// Settings::Entries(): the reader, the writer and the menu all walk that
// table, so a new key is one field here plus one table row in Settings.cpp.
// The comments in the shipped INI are the user-facing documentation; these are
// the defaults.
struct SettingsData
{
	// --- [General] ---------------------------------------------------------
	bool  enabled{ true };
	float triggerChance{ 100.0f };       // percent, per detected weapon clash
	float cooldownSeconds{ 8.0f };       // minimum time between two clashes
	bool  requireHostile{ true };        // NPC must be hostile to the player
	bool  bossesOnly{ false };         // only opponents the BossRecognition lists class as a boss
	float maxStartDistance{ 260.0f };    // clash cannot start if the pair is further apart

	// --- [Standoff] --------------------------------------------------------
	float duration{ 3.0f };              // seconds the quick time event lasts
	float pressGain{ 0.09f };            // meter gained per attack press
	bool  staminaAffectsPlayer{ true };  // low player stamina weakens presses
	bool  staminaAffectsNpc{ true };     // low NPC stamina weakens its push
	float staminaCostPerPress{ 3.0f };   // player stamina spent per press
	float clashDistance{ 95.0f };        // distance between the two actors while locked
	// Separation solve: both weapons measured off their meshes (BladeGeometry)
	// once the block pose is up, and the pair stood at the distance that makes
	// the blades touch, within the bounds below. A miss no distance closes is
	// sideways or vertical; fClashDistance is kept.
	bool  solveClashDistance{ true };
	float solveDistanceMin{ 40.0f };
	float solveDistanceMax{ 130.0f };
	float solveBite{ 2.0f };             // units past first contact, so the blades visibly cross
	// Height misses: the higher blade's owner is bent down to the other, half
	// from the spine and half from the sword arm's shoulder.
	bool  tiltToMeet{ true };
	float tiltMaxDegrees{ 12.0f };       // most either joint may be turned
	// Blades a block animation swings into the other fighter are turned back
	// out, at the wrist first and then at the weapon. Maintained every frame,
	// so it follows the animation and eases off when not needed.
	bool  clearWeaponClipping{ true };
	float clearanceMaxDegrees{ 20.0f };  // most either of those two may be turned
	float clearanceBodyRadius{ 16.0f };  // torso half-width the blade is kept out of, scaled by size
	// Take the point where the weapons meet (sparks, scrape loop, camera aim)
	// from the measured blades rather than the midpoint at a fixed height.
	bool  contactFromWeapons{ true };
	float pushDistance{ 0.0f };          // how far the pair slides as the meter moves (0 = off; the slide reads as motion)
	float approachTime{ 0.25f };         // seconds to slide the actors into position
	float settleTime{ 0.5f };            // after this, headings freeze and spine tracking is off
	bool  freezeAfterSettle{ false };    // opt-in: stop both animation graphs dead in the block pose
	bool  disableCollision{ true };      // the two actors stop colliding with each other while locked
	float timeMultiplier{ 1.0f };        // global time scale during the standoff, 1 = off
	// Shields during a clash: 0 = leave alone, 1 = hide the shield model. The
	// sword-instead-of-shield block comes from the shipped OAR replacer.
	int   shieldMode{ 1 };
	bool  shakeOnPress{ true };
	float shakeStrength{ 0.12f };

	// --- [Difficulty] ------------------------------------------------------
	// The opponent drains the meter at the rate of this many attack presses
	// per second (times fPressGain) when both weapon skills are equal. Pressing
	// faster drives the meter to the player's end, slower lets it fall to the
	// opponent's; exactly that rate holds the centre until the timer, a draw.
	int   difficultyMode{ 1 };               // 0 easy, 1 normal, 2 hard
	float easyPressesPerSecond{ 3.0f };
	float normalPressesPerSecond{ 4.0f };
	float hardPressesPerSecond{ 5.0f };
	// Scales the opponent's push by the gap between the two actors' governing
	// weapon skills (One-Handed / Two-Handed for the weapon each holds).
	float skillInfluence{ 0.8f };
	// An opponent whose weapon skill trails the player's by at least this many
	// points never gets a standoff: the clash resolves on the spot with the
	// large stagger. 0 = off. Only ever in the player's favour.
	float autoWinSkillGap{ 15.0f };
	// Accessibility: holding the attack button counts as mashing it at the
	// difficulty's presses-per-second rate. Discrete presses are ignored.
	bool  holdToMash{ false };

	// --- [Rumble] ----------------------------------------------------------
	bool  rumbleEnabled{ true };
	float rumbleLargeMotor{ 0.45f };       // low-frequency motor level held during the clash, 0..1
	float rumbleSmallMotor{ 0.2f };        // high-frequency motor level held during the clash, 0..1
	float rumblePressPulse{ 0.7f };        // small-motor kick per counted press, 0 = off
	float rumblePulseDuration{ 0.08f };    // seconds each kick lasts
	bool  rumbleRespectGameSetting{ true };  // off when the game's own bGamePadRumble is off

	// --- [Camera] ----------------------------------------------------------
	// One shot's framing: the keys shared by [Camera] in the main INI and by
	// every preset section in CinematicClash_CameraPresets.ini.
	struct CameraFraming
	{
		// Camera position relative to the player, in the player's own frame and
		// scaled by their size: X right, Y forward (negative = behind), Z up.
		float offsetX{ 38.0f };
		float offsetY{ -70.0f };
		float offsetZ{ 112.0f };
		// Aim: with aim-at-contact on, the camera looks at the point where the
		// weapons meet (aimHeight above the ground) and the angles are nudges on
		// top; with it off they are the whole aim, relative to the player's
		// heading and look pitch. Yaw positive = right, pitch positive = down.
		bool  aimAtContact{ true };
		float aimHeight{ 90.0f };
		float yawDegrees{ 0.0f };
		float pitchDegrees{ 0.0f };
		float fov{ 75.0f };       // world FOV during the clash; 0 = leave it alone
		float blendIn{ 0.3f };    // seconds to move from the current camera position
		float blendOut{ 0.4f };   // seconds to move back to the engine's camera afterwards
	};
	struct CameraPreset
	{
		std::string   name;
		CameraFraming framing;
	};

	bool          cameraEnabled{ true };
	CameraFraming cameraFraming;         // the [Camera] keys as written
	// sPreset: comma-separated preset names. Each entry is cameraFraming
	// overlaid with that section of CinematicClash_CameraPresets.ini, kept in list order;
	// names with no section are dropped. The camera picks one at random per
	// clash, skipping any whose position is walled off, and falls back to the
	// first when every one is. Empty = cameraFraming alone.
	std::string cameraPresetList{};
	// Clearance a shot needs: the ray from the contact point to the camera,
	// extended by this many units, must not hit anything but the two actors.
	// 0 = no wall check.
	float cameraWallMargin{ 15.0f };
	// A clash that starts in first person: switch to third person for the
	// shoulder shot (true), or stay in first person with the block held from
	// the player's own eyes and no camera override at all (false).
	bool  forceThirdPerson{ true };
	bool  restoreFirstPerson{ true };    // switch back if the clash forced third person
	float firstPersonRestoreDelay{ 0.75f };

	// --- [Sparks] ----------------------------------------------------------
	bool        sparksEnabled{ true };
	float       sparksInterval{ 0.08f };   // seconds between bursts
	float       sparksHeight{ 105.0f };    // units above the ground at the contact point, scaled by player size
	float       sparksScale{ 1.0f };
	float       sparksJitter{ 6.0f };      // random offset per burst, units
	std::string sparksModel{};             // .nif path override; empty = the weapon's own block-impact model

	// --- [Audio] -----------------------------------------------------------
	// Sound descriptor editor IDs; empty = the weapon's own block-impact sounds.
	bool        audioEnabled{ true };
	std::string impactSound{};
	float       impactVolume{ 1.0f };
	std::string loopSound{};
	float       loopInterval{ 0.35f };     // re-trigger interval for non-looping descriptors
	float       loopVolume{ 0.5f };
	std::string overpowerSound{};
	float       overpowerVolume{ 1.0f };
	bool        enemyShout{ true };        // opponent speaks a combat taunt when the clash starts
	std::string shoutTopic{};              // dialogue topic editor ID; empty = the generic combat taunt
	std::string shoutSound{};              // sound descriptor to play instead of a voice line

	// --- [HUD] -------------------------------------------------------------
	bool  hudEnabled{ true };            // draw the tug-of-war meter at the bottom of the screen
	float hudScale{ 1.0f };              // size multiplier for the meter and its text
	float hudVerticalPosition{ 0.86f };  // 0 = top of the screen, 1 = bottom
	// Optional PNG skin: folder under Data holding frame.png, fill_player.png,
	// fill_opponent.png, marker.png, timer.png and key_<LABEL>.png files for
	// the attack button glyph. Any file present replaces the drawn version of
	// that element. The grid width is how many texture pixels span the bar
	// width (32% of the screen); every PNG is drawn at that same pixel ratio.
	std::string hudTextureFolder{ "SKSE/Plugins/CinematicClash/HUD" };
	int         hudTextureGridWidth{ 1024 };
	// Text font. With sFontFile empty the meter uses the same face the game's
	// own UI draws with: Interface/fontconfig.txt is read (loose or from the
	// BSA, so font replacer mods are honoured) and the font mapped to
	// hudGameFont is pulled out of its fontlib SWF. A .ttf path overrides that.
	std::string hudFontFile{};
	std::string hudGameFont{ "$EverywhereFont" };

	// --- [Outcome] ---------------------------------------------------------
	float loserStaggerMagnitude{ 1.0f };   // 1.0 = the large stagger
	float winnerStaggerMagnitude{ 0.0f };  // 0 = winner is not staggered
	float drawStaggerMagnitude{ 0.25f };   // both actors when the timer runs out; small stagger
	float outcomeWindow{ 1.0f };           // seconds the winner/loser OAR conditions stay true

	// --- [Messages] --------------------------------------------------------
	bool        messagesEnabled{ true };   // master switch for every HUD notification below
	std::string messageStart{ "Weapon clash! Mash attack!" };
	std::string messageStartHold{ "Weapon clash! Hold attack!" };
	std::string messageAutoWin{ "Your skill breaks through their guard!" };
	std::string messageWin{ "You overpowered your opponent!" };
	std::string messageLose{ "You were overpowered!" };
	std::string messageDraw{ "Neither of you gives way!" };
	std::string pressSound{};              // sound descriptor editor ID, empty = none

	// --- [Debug] -----------------------------------------------------------
	bool debugLog{ false };
	// Testing aid: every melee hit the player lands on a humanoid NPC starts a
	// clash, skipping the parry check, the hostility and bosses-only
	// requirements and the chance roll. Cooldown still applies.
	bool forceClashOnHit{ false };
};

class Settings : public SettingsData
{
public:
	static constexpr auto kConfigPath = "Data/SKSE/Plugins/CinematicClash.ini";
	static constexpr auto kPresetsPath = "Data/SKSE/Plugins/CinematicClash_CameraPresets.ini";

	// The INI sections in file order; the menu shows one page per section.
	static constexpr std::array<const char*, 11> kSections{
		"General", "Standoff", "Difficulty", "Rumble", "Camera", "Sparks", "Audio", "HUD", "Outcome", "Messages", "Debug"
	};

	[[nodiscard]] static Settings* GetSingleton();

	// Read both INIs. Values keep what they had for any key the file lacks.
	void Load();

	// Re-read the INIs if either timestamp changed since the last load, so
	// values can be tuned while the game is running. Cheap; call at ~1 Hz.
	// Returns true when a reload happened.
	bool ReloadIfChanged();

	[[nodiscard]] float PressesPerSecond() const noexcept;

	// Usable presets: cameraPresetList resolved against the presets file, in
	// list order. Rebuilt by every Load.
	std::vector<CameraPreset> cameraPresets;
	// Every section of the presets file, in file order, whether listed or not.
	// What the menu's preset editor shows.
	std::vector<CameraPreset> cameraPresetLibrary;

	// --- Key table --------------------------------------------------------
	// One INI key. A monostate member stands for the [Camera] framing block
	// (FramingEntries applied to cameraFraming) so the table keeps the INI's
	// key order. min/max clamp the value on load and bound the menu widget;
	// kUnbounded on either side means no clamp there and a drag box instead
	// of a slider. Int keys with choices are shown as a combo box.
	static constexpr float kUnbounded = std::numeric_limits<float>::infinity();

	struct Entry
	{
		using Member = std::variant<std::monostate, bool SettingsData::*, float SettingsData::*, int SettingsData::*, std::string SettingsData::*>;

		const char*                  section;
		const char*                  key;
		Member                       member;
		float                        min;
		float                        max;
		const char*                  label;
		const char*                  help;
		std::span<const char* const> choices{};
	};
	struct FramingEntry
	{
		using Member = std::variant<bool CameraFraming::*, float CameraFraming::*>;

		const char* key;
		Member      member;
		float       min;
		float       max;
		const char* label;
		const char* help;
	};

	[[nodiscard]] static std::span<const Entry>        Entries();
	[[nodiscard]] static std::span<const FramingEntry> FramingEntries();
	[[nodiscard]] static const SettingsData&           Defaults();

	// --- Writing back (the menu) ------------------------------------------
	// Write every key to CinematicClash.ini in place, keeping the file's
	// comments and layout, then re-read it so memory matches disk exactly.
	bool Save();
	// Put one section's keys back to their defaults, in memory. Follow with Save().
	void ResetSection(std::string_view a_section);

	// Presets file. Each call writes the file, then reloads everything.
	bool               SavePreset(const CameraPreset& a_preset);
	bool               AddPreset(std::string a_name);  // starts as a copy of the [Camera] framing and joins the pool
	bool               RemovePreset(std::string_view a_name);
	bool               SetPresetInPool(std::string_view a_name, bool a_inPool);  // edits sPreset
	[[nodiscard]] bool IsPresetInPool(std::string_view a_name) const;

private:
	std::mutex                      _ioMutex;  // Save/reload can come from the render thread (menu) or the game thread
	std::filesystem::file_time_type _lastWrite{};
	std::filesystem::file_time_type _presetsLastWrite{};

	void Validate();
	void ApplyLogLevel() const;
	// Fill cameraPresetLibrary and cameraPresets from CinematicClash_CameraPresets.ini.
	void BuildCameraPresets();

	Settings() = default;
	Settings(const Settings&) = delete;
	Settings(Settings&&) = delete;
	~Settings() = default;
	Settings& operator=(const Settings&) = delete;
	Settings& operator=(Settings&&) = delete;
};
