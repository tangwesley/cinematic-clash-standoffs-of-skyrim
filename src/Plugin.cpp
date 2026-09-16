#include "BossRecognition.h"
#include "ClashCamera.h"
#include "ClashController.h"
#include "ClashDetection.h"
#include "ClashHUD.h"
#include "ClashInput.h"
#include "ClashMenu.h"
#include "ClashRumble.h"
#include "ClashTDM.h"
#include "OARConditions.h"
#include "Settings.h"

namespace
{
	void InitializeLogging()
	{
#ifdef NDEBUG
		auto path = logger::log_directory();
		if (!path) {
			stl::report_and_fail("Failed to find the SKSE logging directory"sv);
		}
		*path /= "CinematicClash.log"sv;

		auto           sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		constexpr auto level = spdlog::level::info;
#else
		auto           sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
		constexpr auto level = spdlog::level::trace;
#endif

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(level);
		log->flush_on(spdlog::level::info);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e] [%l] %v"s);
	}

	// Co-save. The player's ghost flag (set for the duration of a clash, see
	// ClashController::BeginPlayerImmunity) lives in their extra data and is
	// written into the save. A save made mid-clash and loaded into a fresh
	// session would otherwise leave the player permanently invulnerable, so
	// the save records that we ghosted them and the load clears it.
	constexpr std::uint32_t FourCC(const char (&a_tag)[5])
	{
		return (static_cast<std::uint32_t>(a_tag[0]) << 24) | (static_cast<std::uint32_t>(a_tag[1]) << 16) |
		       (static_cast<std::uint32_t>(a_tag[2]) << 8) | static_cast<std::uint32_t>(a_tag[3]);
	}
	constexpr std::uint32_t kSerializationID = FourCC("CCLS");
	constexpr std::uint32_t kGhostRecord = FourCC("GHST");
	constexpr std::uint32_t kGhostRecordVersion = 1;

	void SaveCallback(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc || !ClashController::GetSingleton()->IsPlayerGhostedForClash()) {
			return;
		}
		if (!a_intfc->OpenRecord(kGhostRecord, kGhostRecordVersion)) {
			logger::warn("Could not write the ghost record to the co-save");
			return;
		}
		const std::uint8_t ghosted = 1;
		a_intfc->WriteRecordData(ghosted);
		logger::info("Save written mid-clash; recorded the player's ghost flag");
	}

	void LoadCallback(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc) {
			return;
		}
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == kGhostRecord) {
				ClashController::GetSingleton()->MarkGhostFromSave();
			}
		}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// OAR only accepts new conditions up to and including kPostLoad.
			OARConditions::Register();
			// Precision's and TDM's DLLs are loaded by now, so their exported requests work.
			ClashDetection::TryRegisterPrecision();
			ClashTDM::GetSingleton()->Init();
			// SKSE Menu Framework, when present, is loaded by now too.
			ClashMenu::Register();
			// SmoothCam answers the interface request through SKSE messaging.
			if (!SmoothCamAPI::RegisterInterfaceLoaderCallback(SKSE::GetMessagingInterface(),
					[](void* a_interface, SmoothCamAPI::InterfaceVersion a_version) {
						ClashCamera::GetSingleton()->OnSmoothCamInterface(a_interface, a_version);
					})) {
				logger::warn("Could not register the SmoothCam interface callback");
			}
			break;

		case SKSE::MessagingInterface::kPostPostLoad:
			if (!SmoothCamAPI::RequestInterface(SKSE::GetMessagingInterface())) {
				logger::info("SmoothCam not present; the vanilla third-person camera will be used during clashes");
			}
			break;

		case SKSE::MessagingInterface::kInputLoaded:
			ClashInput::GetSingleton()->Register();
			break;

		case SKSE::MessagingInterface::kDataLoaded:
			// The renderer and its swap chain exist by now. Always hooked: the HUD
			// checks its enable flag per frame, so it can be switched on from the
			// menu without a restart.
			ClashHUD::GetSingleton()->Install();
			// Menu events: rumble is cut when a menu pauses the game.
			ClashRumble::GetSingleton()->Register();
			// Forms exist now: resolve the boss recognition lists for bBossesOnly.
			BossRecognition::GetSingleton()->Load();
			break;

		case SKSE::MessagingInterface::kNewGame:
		case SKSE::MessagingInterface::kPostLoadGame:
			ClashController::GetSingleton()->Abort("a game was loaded");
			break;

		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLogging();

	const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
	logger::info("{} {} loading", plugin->GetName(), plugin->GetVersion());

	SKSE::Init(a_skse);

	// Also sets the log level from bDebugLog.
	Settings::GetSingleton()->Load();

	SKSE::AllocTrampoline(64);

	ClashDetection::InstallHook();
	ClashController::GetSingleton()->InstallHooks();
	ClashInput::GetSingleton()->InstallHooks();
	ClashCamera::GetSingleton()->InstallHooks();

	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

	const auto serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID(kSerializationID);
	serialization->SetSaveCallback(SaveCallback);
	serialization->SetLoadCallback(LoadCallback);

	logger::info("{} loaded", plugin->GetName());
	return true;
}
