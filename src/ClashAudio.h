#pragma once

// ---------------------------------------------------------------------------
// Sounds for a clash.
//
//   impact     one shot when the weapons lock
//   loop       kept going at the contact point for the whole standoff; either a
//              genuinely looping descriptor from the INI, or (default) the
//              weapon's block-impact scrape re-triggered on a short interval
//   overpower  one shot when the standoff resolves
//   shout      the opponent speaks a combat taunt through the dialogue system
//              (their own voice), with a plain sound descriptor as fallback
//
// Defaults come from the same block-impact data that feeds the sparks, so with
// nothing configured the clash sounds like the weapons involved.
// ---------------------------------------------------------------------------
class ClashAudio
{
public:
	[[nodiscard]] static ClashAudio* GetSingleton();

	// Called at clash start with the resolved block-impact data (may be null).
	void Setup(RE::BGSImpactData* a_impact);

	// a_follow is the scene node the sound is attached to (the player's 3D);
	// positional handles that are only given coordinates end up inaudible.
	void PlayImpact(const RE::NiPoint3& a_point, RE::NiAVObject* a_follow);
	void UpdateLoop(const RE::NiPoint3& a_point, float a_dt, RE::NiAVObject* a_follow);  // starts the loop on first call
	void StopLoop();
	void PlayOverpower(const RE::NiPoint3& a_point, RE::NiAVObject* a_follow);
	void PlayEnemyShout(RE::Actor* a_npc);

private:
	ClashAudio() = default;
	ClashAudio(const ClashAudio&) = delete;
	ClashAudio(ClashAudio&&) = delete;
	~ClashAudio() = default;
	ClashAudio& operator=(const ClashAudio&) = delete;
	ClashAudio& operator=(ClashAudio&&) = delete;

	[[nodiscard]] bool Acquire(RE::BSSoundHandle& a_handle, const std::string& a_editorID, RE::BGSSoundDescriptorForm* a_fallback);
	void               PlayOneShot(const std::string& a_editorID, RE::BGSSoundDescriptorForm* a_fallback, const RE::NiPoint3& a_point, RE::NiAVObject* a_follow, float a_volume, float a_frequency);

	bool _fileLoop{ false };  // the standoff loop is a loose file on our own engine

	[[nodiscard]] RE::TESTopic* FindShoutTopic();
	[[nodiscard]] bool          SayTopic(RE::Actor* a_npc, RE::TESTopic* a_topic) const;

	RE::BGSImpactData* _impact{ nullptr };

	RE::BSSoundHandle _loop{};
	bool              _loopStarted{ false };
	float             _loopTimer{ 0.0f };

	RE::TESTopic* _shoutTopic{ nullptr };
	bool          _shoutTopicSearched{ false };
};
