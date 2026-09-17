#include "BossRecognition.h"

#include <SimpleIni.h>

#include <charconv>

namespace
{
	constexpr auto kSection = "BossRecognition";

	struct FormRef
	{
		std::string mod;
		RE::FormID  localID;
	};

	// "Skyrim.esm:0x012E82 ; DragonRace" -> { "Skyrim.esm", 0x012E82 }. The
	// 0x prefix is optional and anything after the ID is ignored.
	std::optional<FormRef> ParseFormRef(std::string_view a_text)
	{
		const auto trim = [](std::string_view a_sv) {
			while (!a_sv.empty() && std::isspace(static_cast<unsigned char>(a_sv.front()))) {
				a_sv.remove_prefix(1);
			}
			while (!a_sv.empty() && std::isspace(static_cast<unsigned char>(a_sv.back()))) {
				a_sv.remove_suffix(1);
			}
			return a_sv;
		};

		const auto colon = a_text.find(':');
		if (colon == std::string_view::npos) {
			return std::nullopt;
		}
		const auto mod = trim(a_text.substr(0, colon));
		auto       id = trim(a_text.substr(colon + 1));
		if (const auto end = id.find_first_of(" \t;"); end != std::string_view::npos) {
			id = id.substr(0, end);
		}
		if (id.starts_with("0x") || id.starts_with("0X")) {
			id.remove_prefix(2);
		}
		if (mod.empty() || id.empty()) {
			return std::nullopt;
		}

		std::uint32_t value = 0;
		const auto    result = std::from_chars(id.data(), id.data() + id.size(), value, 16);
		if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) {
			return std::nullopt;
		}
		return FormRef{ std::string(mod), value };
	}

	std::string Lower(std::string a_text)
	{
		for (auto& c : a_text) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return a_text;
	}

	// Runs a_apply(form) for every value of a_key in the section that names a
	// form of type T present in the load order.
	template <class T, class F>
	void ForEachForm(const CSimpleIniA& a_ini, const char* a_key, const std::filesystem::path& a_path, F&& a_apply)
	{
		CSimpleIniA::TNamesDepend values;
		if (!a_ini.GetAllValues(kSection, a_key, values)) {
			return;
		}
		values.sort(CSimpleIniA::Entry::LoadOrder());
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		for (const auto& entry : values) {
			const auto ref = ParseFormRef(entry.pItem);
			if (!ref) {
				logger::warn("{}: cannot parse {} = \"{}\" (expected Plugin.esp:0xFormID)", a_path.filename().string(), a_key, entry.pItem);
				continue;
			}
			if (const auto form = dataHandler->LookupForm<T>(ref->localID, ref->mod)) {
				a_apply(form);
			} else {
				// A plugin the user does not have; every list mentions some.
				logger::debug("{}: {} {}:{:06X} not found, skipped", a_path.filename().string(), a_key, ref->mod, ref->localID);
			}
		}
	}
}

BossRecognition* BossRecognition::GetSingleton()
{
	static BossRecognition singleton;
	return std::addressof(singleton);
}

void BossRecognition::ReadFile(const std::filesystem::path& a_path, Lists& a_lists)
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.SetMultiKey();
	if (const auto rc = ini.LoadFile(a_path.string().c_str()); rc < 0) {
		logger::warn("Could not read {} (error {})", a_path.string(), static_cast<int>(rc));
		return;
	}

	ForEachForm<RE::TESRace>(ini, "Race", a_path, [&](const RE::TESRace* a_race) { a_lists.races.insert(a_race); });
	ForEachForm<RE::TESRace>(ini, "RemoveRace", a_path, [&](const RE::TESRace* a_race) { a_lists.races.erase(a_race); });
	ForEachForm<RE::BGSLocationRefType>(ini, "LocRefType", a_path, [&](const RE::BGSLocationRefType* a_type) { a_lists.locRefTypes.insert(a_type); });
	ForEachForm<RE::BGSLocationRefType>(ini, "RemoveLocRefType", a_path, [&](const RE::BGSLocationRefType* a_type) { a_lists.locRefTypes.erase(a_type); });
	ForEachForm<RE::TESNPC>(ini, "NPC", a_path, [&](const RE::TESNPC* a_npc) { a_lists.npcs.insert(a_npc); });
	ForEachForm<RE::TESNPC>(ini, "RemoveNPC", a_path, [&](const RE::TESNPC* a_npc) { a_lists.npcs.erase(a_npc); });
	ForEachForm<RE::TESNPC>(ini, "NPCBlacklist", a_path, [&](const RE::TESNPC* a_npc) { a_lists.blacklist.insert(a_npc); });
	ForEachForm<RE::TESNPC>(ini, "RemoveNPCBlacklist", a_path, [&](const RE::TESNPC* a_npc) { a_lists.blacklist.erase(a_npc); });
}

void BossRecognition::ReadFolder(const std::filesystem::path& a_folder, std::string_view a_baseName, Lists& a_lists, int& a_fileCount)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(a_folder, ec)) {
		return;
	}

	// The base file first, then the rest by name, so patches override it.
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(a_folder, ec)) {
		if (entry.is_regular_file(ec) && Lower(entry.path().extension().string()) == ".ini") {
			files.push_back(entry.path());
		}
	}
	const auto base = Lower(std::string(a_baseName));
	std::ranges::sort(files, [&](const auto& a_lhs, const auto& a_rhs) {
		const auto l = Lower(a_lhs.filename().string());
		const auto r = Lower(a_rhs.filename().string());
		if ((l == base) != (r == base)) {
			return l == base;
		}
		return l < r;
	});

	for (const auto& file : files) {
		logger::info("  Reading {}", file.string());
		ReadFile(file, a_lists);
		++a_fileCount;
	}
}

void BossRecognition::Load()
{
	if (!RE::TESDataHandler::GetSingleton()) {
		logger::warn("Boss recognition lists requested before the data handler exists; skipped");
		return;
	}

	logger::info("Reading boss recognition lists...");
	Lists lists;
	int   fileCount = 0;
	ReadFolder("Data/SKSE/Plugins/TrueDirectionalMovement", "TrueDirectionalMovement_base.ini", lists, fileCount);
	ReadFolder("Data/SKSE/Plugins/TrueHUD", "TrueHUD_base.ini", lists, fileCount);
	ReadFolder(kOwnFolder, "CinematicClash_base.ini", lists, fileCount);

	logger::info("Boss recognition: {} file(s); {} races, {} NPCs, {} location ref types, {} blacklisted NPCs",
		fileCount, lists.races.size(), lists.npcs.size(), lists.locRefTypes.size(), lists.blacklist.size());
	if (fileCount == 0) {
		logger::warn("No boss recognition INI found (expected at least one in {}); bBossesOnly will reject every opponent", kOwnFolder);
	}

	std::scoped_lock lock(_mutex);
	_lists = std::move(lists);
}

bool BossRecognition::IsBoss(RE::Actor* a_actor) const
{
	if (!a_actor) {
		return false;
	}
	std::scoped_lock lock(_mutex);

	const RE::TESActorBase* base = a_actor->GetActorBase();
	const RE::TESActorBase* originalBase = nullptr;
	if (const auto leveled = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>()) {
		originalBase = leveled->originalBase;  // the leveled template a "LvlBanditBoss" resolved from
	}
	const auto listed = [&](const std::unordered_set<const RE::TESActorBase*>& a_set) {
		return (base && a_set.contains(base)) || (originalBase && a_set.contains(originalBase));
	};

	if (listed(_lists.blacklist)) {
		return false;
	}
	if (const auto race = a_actor->GetRace(); race && _lists.races.contains(race)) {
		return true;
	}
	if (listed(_lists.npcs)) {
		return true;
	}
	if (!_lists.locRefTypes.empty()) {
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto location = player ? player->GetPlayerRuntimeData().currentLocation : nullptr;
		if (location) {
			for (const auto& ref : location->specialRefs) {
				if (ref.type && _lists.locRefTypes.contains(ref.type) && ref.refData.refID == a_actor->GetFormID()) {
					return true;
				}
			}
		}
	}
	return false;
}
