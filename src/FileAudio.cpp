#include "FileAudio.h"

// CommonLib targets Windows 7 for the sake of its own headers; the inbox
// XAudio2 2.9 header insists on Windows 8+, which every supported game runtime
// has. Windows.h is already in the PCH, so only this header sees the change.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <xaudio2.h>
#pragma pop_macro("_WIN32_WINNT")

#include <fstream>

namespace
{
	[[nodiscard]] std::string NormalisePath(const std::string& a_path)
	{
		std::string path = a_path;
		std::replace(path.begin(), path.end(), '/', '\\');
		if (path.size() > 5 && _strnicmp(path.c_str(), "data\\", 5) == 0) {
			path.erase(0, 5);
		}
		return path;
	}

	[[nodiscard]] std::string LowerKey(const std::string& a_path)
	{
		std::string key = a_path;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return key;
	}

	[[nodiscard]] std::uint32_t ReadU32(const std::uint8_t* a_data)
	{
		std::uint32_t value = 0;
		std::memcpy(&value, a_data, sizeof(value));
		return value;
	}
}

FileAudio* FileAudio::GetSingleton()
{
	static FileAudio singleton;
	return std::addressof(singleton);
}

FileAudio::~FileAudio()
{
	// Process teardown: the engine goes away with the process; nothing to do
	// that would be safe this late.
}

bool FileAudio::EnsureEngine()
{
	if (_engine) {
		return true;
	}
	if (_failed) {
		return false;
	}

	IXAudio2* engine = nullptr;
	HRESULT   hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr) || !engine) {
		logger::error("FileAudio: XAudio2Create failed ({:#x})", static_cast<std::uint32_t>(hr));
		_failed = true;
		return false;
	}

	IXAudio2MasteringVoice* master = nullptr;
	hr = engine->CreateMasteringVoice(&master);
	if (FAILED(hr) || !master) {
		logger::error("FileAudio: CreateMasteringVoice failed ({:#x})", static_cast<std::uint32_t>(hr));
		engine->Release();
		_failed = true;
		return false;
	}

	_engine = engine;
	_master = master;
	logger::info("FileAudio: XAudio2 engine ready");
	return true;
}

const FileAudio::Clip* FileAudio::LoadClip(const std::string& a_path)
{
	const auto key = LowerKey(a_path);
	if (const auto it = _clips.find(key); it != _clips.end()) {
		return it->second.get();
	}

	const auto fullPath = std::filesystem::path("Data") / a_path;
	std::ifstream file(fullPath, std::ios::binary);
	if (!file) {
		logger::warn("FileAudio: cannot open '{}'", fullPath.string());
		_clips.emplace(key, nullptr);
		return nullptr;
	}
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
		logger::warn("FileAudio: '{}' is not a RIFF WAVE file", fullPath.string());
		_clips.emplace(key, nullptr);
		return nullptr;
	}

	auto clip = std::make_unique<Clip>();
	std::size_t offset = 12;
	while (offset + 8 <= bytes.size()) {
		const auto id = std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), 4);
		const auto size = ReadU32(bytes.data() + offset + 4);
		const auto body = offset + 8;
		if (body + size > bytes.size()) {
			break;
		}
		if (id == "fmt ") {
			clip->format.assign(bytes.begin() + body, bytes.begin() + body + size);
			if (clip->format.size() < sizeof(WAVEFORMATEX)) {
				clip->format.resize(sizeof(WAVEFORMATEX), 0);  // cbSize = 0
			}
		} else if (id == "data") {
			clip->samples.assign(bytes.begin() + body, bytes.begin() + body + size);
		}
		offset = body + size + (size & 1);  // chunks are word aligned
	}

	if (clip->format.empty() || clip->samples.empty()) {
		logger::warn("FileAudio: '{}' has no fmt/data chunk", fullPath.string());
		_clips.emplace(key, nullptr);
		return nullptr;
	}

	const auto format = reinterpret_cast<const WAVEFORMATEX*>(clip->format.data());
	logger::info("FileAudio: loaded '{}' ({} ch, {} Hz, {}-bit, {:.2f} s)", a_path, format->nChannels, format->nSamplesPerSec,
		format->wBitsPerSample, format->nAvgBytesPerSec > 0 ? static_cast<float>(clip->samples.size()) / format->nAvgBytesPerSec : 0.0f);

	const auto result = clip.get();
	_clips.emplace(key, std::move(clip));
	return result;
}

float FileAudio::MasterVolume() const
{
	if (const auto prefs = RE::INIPrefSettingCollection::GetSingleton()) {
		if (const auto setting = prefs->GetSetting("fAudioMasterVolume:AudioMenu")) {
			return std::clamp(setting->GetFloat(), 0.0f, 1.0f);
		}
	}
	return 1.0f;
}

IXAudio2SourceVoice* FileAudio::CreateVoice(const Clip& a_clip, bool a_loop, float a_volume, float a_frequencyRatio)
{
	IXAudio2SourceVoice* voice = nullptr;
	const auto           format = reinterpret_cast<const WAVEFORMATEX*>(a_clip.format.data());
	const HRESULT        hr = _engine->CreateSourceVoice(&voice, format, 0, XAUDIO2_DEFAULT_FREQ_RATIO);
	if (FAILED(hr) || !voice) {
		logger::warn("FileAudio: CreateSourceVoice failed ({:#x})", static_cast<std::uint32_t>(hr));
		return nullptr;
	}

	XAUDIO2_BUFFER buffer{};
	buffer.AudioBytes = static_cast<UINT32>(a_clip.samples.size());
	buffer.pAudioData = a_clip.samples.data();
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	buffer.LoopCount = a_loop ? XAUDIO2_LOOP_INFINITE : 0;
	if (FAILED(voice->SubmitSourceBuffer(&buffer))) {
		voice->DestroyVoice();
		return nullptr;
	}

	voice->SetVolume(std::clamp(a_volume, 0.0f, 1.0f) * MasterVolume());
	if (a_frequencyRatio > 0.0f && std::abs(a_frequencyRatio - 1.0f) > 0.001f) {
		voice->SetFrequencyRatio(std::clamp(a_frequencyRatio, XAUDIO2_MIN_FREQ_RATIO, 2.0f));
	}
	if (!_paused) {
		voice->Start(0);
	}
	return voice;
}

void FileAudio::Reap()
{
	std::erase_if(_oneShots, [](IXAudio2SourceVoice* a_voice) {
		XAUDIO2_VOICE_STATE state{};
		a_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		if (state.BuffersQueued == 0) {
			a_voice->DestroyVoice();
			return true;
		}
		return false;
	});
}

bool FileAudio::PlayOneShot(const std::string& a_path, float a_volume, float a_frequencyRatio)
{
	const std::scoped_lock lock(_mutex);
	if (!EnsureEngine()) {
		return false;
	}
	Reap();

	const auto clip = LoadClip(NormalisePath(a_path));
	if (!clip) {
		return false;
	}
	if (const auto voice = CreateVoice(*clip, false, a_volume, a_frequencyRatio)) {
		_oneShots.push_back(voice);
		return true;
	}
	return false;
}

bool FileAudio::StartLoop(const std::string& a_path, float a_volume)
{
	const std::scoped_lock lock(_mutex);
	if (!EnsureEngine()) {
		return false;
	}
	if (_loop) {
		_loop->Stop(0);
		_loop->DestroyVoice();
		_loop = nullptr;
	}

	const auto clip = LoadClip(NormalisePath(a_path));
	if (!clip) {
		return false;
	}
	_loop = CreateVoice(*clip, true, a_volume, 1.0f);
	return _loop != nullptr;
}

void FileAudio::StopLoop()
{
	const std::scoped_lock lock(_mutex);
	if (_loop) {
		_loop->Stop(0);
		_loop->DestroyVoice();
		_loop = nullptr;
	}
}

bool FileAudio::IsLoopPlaying()
{
	const std::scoped_lock lock(_mutex);
	return _loop != nullptr;
}

void FileAudio::SetPaused(bool a_paused)
{
	const std::scoped_lock lock(_mutex);
	if (_paused == a_paused || !_engine) {
		_paused = a_paused;
		return;
	}
	_paused = a_paused;
	if (_loop) {
		if (a_paused) {
			_loop->Stop(0);
		} else {
			_loop->Start(0);
		}
	}
	for (const auto voice : _oneShots) {
		if (a_paused) {
			voice->Stop(0);
		} else {
			voice->Start(0);
		}
	}
}
