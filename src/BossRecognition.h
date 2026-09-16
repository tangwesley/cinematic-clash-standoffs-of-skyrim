#pragma once

#include <unordered_set>

// ---------------------------------------------------------------------------
// Boss classification, for [General] bBossesOnly. Done here so the plugin has
// no dependency on TrueHUD, but in TrueHUD's data format so the two agree:
// a [BossRecognition] section of Race, NPC, LocRefType and NPCBlacklist keys
// (each "Plugin.esp:0xFormID", with a Remove* counterpart), read from
//
//   Data/SKSE/Plugins/TrueDirectionalMovement/*.ini          TrueHUD's legacy folder
//   Data/SKSE/Plugins/TrueHUD/*.ini                          TrueHUD's lists, when installed
//   Data/SKSE/Plugins/CinematicClash/BossRecognition/*.ini   this mod's, read last
//
// in that order, base file first within each folder, so every boss patch a
// user has for TrueHUD carries over and this mod's own files have the last
// word. An actor is a boss when its race is listed, its base NPC (or the
// leveled template it was resolved from) is listed, or the current location
// marks that reference with a listed location ref type (the vanilla "Boss"
// marker), and it is not blacklisted.
// ---------------------------------------------------------------------------
class BossRecognition
{
public:
	static constexpr auto kOwnFolder = "Data/SKSE/Plugins/CinematicClash/BossRecognition";

	[[nodiscard]] static BossRecognition* GetSingleton();

	// kDataLoaded or later: the entries are resolved to forms. Re-reads
	// everything each call, so the menu's "Reload from disk" can use it too.
	void Load();

	[[nodiscard]] bool IsBoss(RE::Actor* a_actor) const;

private:
	BossRecognition() = default;
	BossRecognition(const BossRecognition&) = delete;
	BossRecognition(BossRecognition&&) = delete;
	~BossRecognition() = default;
	BossRecognition& operator=(const BossRecognition&) = delete;
	BossRecognition& operator=(BossRecognition&&) = delete;

	struct Lists
	{
		std::unordered_set<const RE::TESRace*>            races;
		std::unordered_set<const RE::TESActorBase*>       npcs;
		std::unordered_set<const RE::TESActorBase*>       blacklist;
		std::unordered_set<const RE::BGSLocationRefType*> locRefTypes;
	};

	static void ReadFile(const std::filesystem::path& a_path, Lists& a_lists);
	static void ReadFolder(const std::filesystem::path& a_folder, std::string_view a_baseName, Lists& a_lists, int& a_fileCount);

	// Load can come from the render thread (menu) while IsBoss runs on the game thread.
	mutable std::mutex _mutex;
	Lists              _lists;
};
