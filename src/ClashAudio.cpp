#include "ClashAudio.h"

#include "FileAudio.h"
#include "Settings.h"

namespace
{
	constexpr std::uint32_t kSoundFlags = 0x1A;  // same default CommonLib uses for descriptor handles

	// Loose files go through our own XAudio2 player; everything else is a sound
	// descriptor editor ID played through the game's audio system.
	[[nodiscard]] bool IsFilePath(const std::string& a_value)
	{
		if (a_value.size() < 5) {
			return false;
		}
		const auto tail = a_value.substr(a_value.size() - 4);
		return _stricmp(tail.c_str(), ".wav") == 0;
	}

	// Vanilla descriptors to try when the weapon's impact data has none.
	constexpr const char* kFallbackImpactNames[] = {
		"WPNBlockBladeBlade",
		"WPNBlockBlade",
		"WPNImpactBladeMetal",
		"WPNBashBlade",
		"WPNBlockShieldMetal",
	};

	// First impact entry of a material's physics impact set that has a sound.
	[[nodiscard]] RE::BGSSoundDescriptorForm* MaterialImpactSound(RE::MATERIAL_ID a_material)
	{
		const auto material = RE::BGSMaterialType::GetMaterialType(a_material);
		if (!material || !material->havokImpactDataSet) {
			return nullptr;
		}
		for (const auto& [key, data] : material->havokImpactDataSet->impactMap) {
			if (data && data->sound1) {
				return data->sound1;
			}
		}
		return nullptr;
	}
}

ClashAudio* ClashAudio::GetSingleton()
{
	static ClashAudio singleton;
	return std::addressof(singleton);
}

void ClashAudio::Setup(RE::BGSImpactData* a_impact)
{
	StopLoop();
	_impact = a_impact;
	_loopTimer = 0.0f;
}

bool ClashAudio::Acquire(RE::BSSoundHandle& a_handle, const std::string& a_editorID, RE::BGSSoundDescriptorForm* a_fallback)
{
	const auto manager = RE::BSAudioManager::GetSingleton();
	if (!manager) {
		logger::warn("Audio: no audio manager");
		return false;
	}
	const bool debug = Settings::GetSingleton()->debugLog;

	if (!a_editorID.empty()) {
		manager->GetSoundHandleByName(a_handle, a_editorID.c_str(), kSoundFlags);
		if (a_handle.IsValid()) {
			if (debug) {
				logger::debug("Audio: acquired '{}' (id {})", a_editorID, a_handle.soundID);
			}
			return true;
		}
		logger::warn("Audio: sound descriptor '{}' not found", a_editorID);
	}

	if (a_fallback) {
		if (!a_fallback->soundDescriptor) {
			logger::warn("Audio: descriptor form {:08X} has no sound data", a_fallback->GetFormID());
		} else {
			manager->GetSoundHandle(a_handle, a_fallback->soundDescriptor, kSoundFlags);
			if (debug) {
				logger::debug("Audio: descriptor form {:08X} -> valid {}, id {}", a_fallback->GetFormID(), a_handle.IsValid(), a_handle.soundID);
			}
			if (a_handle.IsValid()) {
				return true;
			}
		}
	}

	// Last resort: material physics impacts, then vanilla names.
	if (const auto metal = MaterialImpactSound(RE::MATERIAL_ID::kMetalSolid); metal && metal->soundDescriptor) {
		manager->GetSoundHandle(a_handle, metal->soundDescriptor, kSoundFlags);
		if (a_handle.IsValid()) {
			if (debug) {
				logger::debug("Audio: using metal material impact sound {:08X}", metal->GetFormID());
			}
			return true;
		}
	}
	for (const auto name : kFallbackImpactNames) {
		manager->GetSoundHandleByName(a_handle, name, kSoundFlags);
		if (a_handle.IsValid()) {
			if (debug) {
				logger::debug("Audio: using fallback descriptor '{}'", name);
			}
			return true;
		}
	}
	logger::warn("Audio: no playable sound found");
	return false;
}

void ClashAudio::PlayOneShot(const std::string& a_editorID, RE::BGSSoundDescriptorForm* a_fallback, const RE::NiPoint3& a_point, RE::NiAVObject* a_follow, float a_volume, float a_frequency)
{
	if (IsFilePath(a_editorID)) {
		const bool ok = FileAudio::GetSingleton()->PlayOneShot(a_editorID, a_volume, a_frequency);
		if (Settings::GetSingleton()->debugLog) {
			logger::debug("Audio: file one-shot '{}' -> {}", a_editorID, ok);
		}
		return;
	}

	RE::BSSoundHandle handle;
	if (!Acquire(handle, a_editorID, a_fallback)) {
		return;
	}
	if (a_follow) {
		handle.SetObjectToFollow(a_follow);
	}
	handle.SetPosition(a_point);
	handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));
	if (a_frequency > 0.0f && std::abs(a_frequency - 1.0f) > 0.001f) {
		handle.SetFrequency(a_frequency);
	}
	const bool played = handle.Play();
	if (Settings::GetSingleton()->debugLog) {
		logger::debug("Audio: one-shot id {} follow {} play {}", handle.soundID, static_cast<void*>(a_follow), played);
	}
}

void ClashAudio::PlayImpact(const RE::NiPoint3& a_point, RE::NiAVObject* a_follow)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->audioEnabled) {
		return;
	}
	PlayOneShot(settings->impactSound, _impact ? _impact->sound1 : nullptr, a_point, a_follow, settings->impactVolume, 1.0f);
}

void ClashAudio::UpdateLoop(const RE::NiPoint3& a_point, float a_dt, RE::NiAVObject* a_follow)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->audioEnabled) {
		return;
	}

	// A loose file loops on our own engine: start it once and leave it.
	if (IsFilePath(settings->loopSound)) {
		if (!_loopStarted) {
			_loopStarted = true;
			_fileLoop = FileAudio::GetSingleton()->StartLoop(settings->loopSound, settings->loopVolume);
			if (settings->debugLog) {
				logger::debug("Audio: file loop '{}' -> {}", settings->loopSound, _fileLoop);
			}
		}
		return;
	}

	// The weapon's second impact sound is usually the lighter scrape; fall
	// back to the main clang if there is none.
	RE::BGSSoundDescriptorForm* fallback = nullptr;
	if (_impact) {
		fallback = _impact->sound2 ? _impact->sound2 : _impact->sound1;
	}

	if (_loopStarted && _loop.IsValid() && _loop.IsPlaying()) {
		_loop.SetPosition(a_point);
	}

	_loopTimer += a_dt;
	const bool needsStart = !_loopStarted || !_loop.IsValid() || !_loop.IsPlaying();
	if (needsStart && (!_loopStarted || _loopTimer >= settings->loopInterval)) {
		_loopTimer = 0.0f;
		_loopStarted = true;
		if (Acquire(_loop, settings->loopSound, fallback)) {
			if (a_follow) {
				_loop.SetObjectToFollow(a_follow);
			}
			_loop.SetPosition(a_point);
			_loop.SetVolume(std::clamp(settings->loopVolume, 0.0f, 1.0f));
			const bool played = _loop.Play();
			if (settings->debugLog) {
				logger::debug("Audio: loop id {} play {} (fallback {:08X})", _loop.soundID, played, fallback ? fallback->GetFormID() : 0u);
			}
		}
	}
}

void ClashAudio::StopLoop()
{
	if (_fileLoop) {
		FileAudio::GetSingleton()->StopLoop();
		_fileLoop = false;
	}
	if (_loopStarted && _loop.IsValid()) {
		_loop.FadeOutAndRelease(150);
	}
	_loop = RE::BSSoundHandle{};
	_loopStarted = false;
	_loopTimer = 0.0f;
}

void ClashAudio::PlayOverpower(const RE::NiPoint3& a_point, RE::NiAVObject* a_follow)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->audioEnabled) {
		return;
	}
	// A slightly lower pitch on the default clang gives the deciding blow weight.
	const float frequency = settings->overpowerSound.empty() ? 0.85f : 1.0f;
	PlayOneShot(settings->overpowerSound, _impact ? _impact->sound1 : nullptr, a_point, a_follow, settings->overpowerVolume, frequency);
}

// ---------------------------------------------------------------------------
// Opponent's shout
// ---------------------------------------------------------------------------

RE::TESTopic* ClashAudio::FindShoutTopic()
{
	if (_shoutTopicSearched) {
		return _shoutTopic;
	}
	_shoutTopicSearched = true;

	const auto settings = Settings::GetSingleton();
	const auto dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		return nullptr;
	}

	using Subtype = RE::DIALOGUE_DATA::Subtype;

	RE::TESTopic* best = nullptr;
	std::uint32_t bestInfos = 0;
	for (const auto topic : dataHandler->GetFormArray<RE::TESTopic>()) {
		if (!topic) {
			continue;
		}
		if (!settings->shoutTopic.empty()) {
			if (_stricmp(topic->formEditorID.c_str(), settings->shoutTopic.c_str()) == 0) {
				best = topic;
				break;
			}
			continue;
		}
		if (topic->data.type != RE::DIALOGUE_TYPE::kCombat) {
			continue;
		}
		const auto subtype = *topic->data.subtype;
		if (subtype != Subtype::kTaunt) {
			continue;
		}
		// The generic combat taunt topic carries lines for every voice type,
		// so it is the one with by far the most infos.
		if (topic->numTopicInfos > bestInfos) {
			bestInfos = topic->numTopicInfos;
			best = topic;
		}
	}

	_shoutTopic = best;
	if (best) {
		logger::info("Audio: opponent shout uses topic '{}' ({:08X}, {} lines)", best->formEditorID.c_str(), best->GetFormID(), best->numTopicInfos);
	} else {
		logger::warn("Audio: no combat taunt topic found; falling back to the shout sound descriptor");
	}
	return best;
}

bool ClashAudio::SayTopic(RE::Actor* a_npc, RE::TESTopic* a_topic) const
{
	const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
	if (!vm || !a_topic) {
		return false;
	}
	const auto policy = vm->GetObjectHandlePolicy();
	if (!policy) {
		return false;
	}

	const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), a_npc);
	if (handle == policy->EmptyHandle()) {
		return false;
	}

	// ObjectReference.Say(Topic akTopicToSay, Actor akActorToSpeakAs = None, bool abSpeakInPlayersHead = false)
	RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
	const auto args = RE::MakeFunctionArguments(static_cast<RE::TESTopic*>(a_topic), static_cast<RE::Actor*>(nullptr), false);
	return vm->DispatchMethodCall(handle, "Actor", "Say", args, callback);
}

void ClashAudio::PlayEnemyShout(RE::Actor* a_npc)
{
	const auto settings = Settings::GetSingleton();
	if (!settings->audioEnabled || !settings->enemyShout || !a_npc) {
		return;
	}

	// An explicit sound (descriptor or file) wins; otherwise a real voice line.
	if (!settings->shoutSound.empty()) {
		PlayOneShot(settings->shoutSound, nullptr, a_npc->GetPosition(), a_npc->Get3D(), 1.0f, 1.0f);
		return;
	}

	if (const auto topic = FindShoutTopic(); topic && SayTopic(a_npc, topic)) {
		logger::debug("Audio: {} says a taunt", a_npc->GetName());
		return;
	}

	logger::debug("Audio: could not trigger a taunt for {}", a_npc->GetName());
}
