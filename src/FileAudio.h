#pragma once

// ---------------------------------------------------------------------------
// Plays loose .wav files through the plugin's own XAudio2 engine.
//
// The game's audio system only resolves sound files whose names were
// registered when a plugin's Sound Descriptor records loaded, so a file named
// in the INI cannot be played through it without shipping an .esp. Rather than
// require that, files are decoded here and played on our own voices: one-shots
// on throwaway voices, the standoff loop on a voice with an infinite loop.
//
// Playback is 2D (no distance attenuation), which suits a camera that is a few
// feet from the action. Volume follows the game's master volume slider and
// each sound's own INI volume. Voices pause with the game menu.
//
// Formats: RIFF WAVE with PCM or IEEE float samples, any channel count and
// sample rate; 16-bit mono 44.1 kHz is the sensible choice.
// ---------------------------------------------------------------------------

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

class FileAudio
{
public:
	[[nodiscard]] static FileAudio* GetSingleton();

	// Paths are relative to Data (e.g. Sound\FX\CinematicClash\impact.wav).
	bool PlayOneShot(const std::string& a_path, float a_volume, float a_frequencyRatio);
	bool StartLoop(const std::string& a_path, float a_volume);
	void StopLoop();
	[[nodiscard]] bool IsLoopPlaying();

	// Safe from any thread; called from the render hook with the menu state.
	void SetPaused(bool a_paused);

private:
	FileAudio() = default;
	FileAudio(const FileAudio&) = delete;
	FileAudio(FileAudio&&) = delete;
	~FileAudio();
	FileAudio& operator=(const FileAudio&) = delete;
	FileAudio& operator=(FileAudio&&) = delete;

	struct Clip
	{
		std::vector<std::uint8_t> format;  // WAVEFORMATEX (or EXTENSIBLE) bytes
		std::vector<std::uint8_t> samples;
	};

	bool                 EnsureEngine();
	const Clip*          LoadClip(const std::string& a_path);
	IXAudio2SourceVoice* CreateVoice(const Clip& a_clip, bool a_loop, float a_volume, float a_frequencyRatio);
	void                 Reap();
	[[nodiscard]] float  MasterVolume() const;

	std::mutex _mutex;
	bool       _failed{ false };
	bool       _paused{ false };

	IXAudio2*               _engine{ nullptr };
	IXAudio2MasteringVoice* _master{ nullptr };

	std::unordered_map<std::string, std::unique_ptr<Clip>> _clips;
	std::vector<IXAudio2SourceVoice*>                      _oneShots;
	IXAudio2SourceVoice*                                   _loop{ nullptr };
};
