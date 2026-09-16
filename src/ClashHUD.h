#pragma once

// ---------------------------------------------------------------------------
// The tug-of-war meter drawn at the bottom centre of the screen during a
// standoff.
//
// There is no Scaleform widget to ship, so this draws with Dear ImGui from a
// hook on the game swap chain's Present: the game has finished its frame (HUD
// included), we paint on top, then hand the frame to the real Present. Nothing
// is drawn while the meter is hidden, so the hook costs a couple of loads per
// frame outside a clash.
//
// Above the bar, during the mash, sits the glyph of whatever button the game
// has bound to attack: a key cap, a mouse, or an Xbox-style pad button,
// following the device the game currently considers active. The game's own
// button art lives as vector shapes inside its Flash menus and cannot be
// reused from here, so equivalents are drawn, and any of them can be replaced
// by a PNG.
//
// Skinning: every element (frame, the two fills, the marker, the timer and the
// attack glyph) can be replaced by a PNG from the folder named in the INI. Any
// file that is missing falls back to the built-in vector drawing of that
// element, so a skin can be partial. The PNGs are loaded on the render thread
// with the DirectXTK WIC loader and reloaded whenever the INI is re-read.
//
// ENB / SmoothCam: both sit on this same Present. The important detail, taken
// from how SmoothCam draws its crosshair, is to render into whatever render
// target is bound when Present is called rather than into the swap chain's
// back buffer: with ENB in the chain that bound target is the frame ENB goes
// on to compose, whereas the raw back buffer may never be read. The back
// buffer is only a fallback when nothing is bound.
//
// Threading: the controller pushes a snapshot (meter, timer, visibility) from
// the main thread; the render thread only reads it. The opponent's name and
// the attack glyph are the non-trivial fields and are guarded by a mutex.
// ---------------------------------------------------------------------------
class ClashHUD
{
public:
	[[nodiscard]] static ClashHUD* GetSingleton();

	// Hook Present. Call at kDataLoaded, once the renderer exists.
	void Install();

	// Main-thread snapshot.
	void SetNames(std::string_view a_player, std::string_view a_opponent);
	void SetState(bool a_visible, float a_meter, float a_timeFraction, bool a_standoff);
	void Hide() { SetState(false, 0.5f, 1.0f, false); }

	// Hold-to-mash: the glyph breathes instead of pulsing and reads HOLD.
	void SetHoldMode(bool a_hold) { _holdMode.store(a_hold, std::memory_order_relaxed); }

	// Look up the attack binding for the active input device and remember
	// which glyph to draw for it. Main thread; call when a clash starts.
	void RefreshAttackGlyph();

	// Ask the render thread to re-read the PNG skin and the font from disk on
	// its next frame. Called whenever the INI is reloaded, so saving the INI
	// is also how a skin is refreshed while the game runs.
	void ReloadAssets() { _reloadAssets.store(true, std::memory_order_release); }

	struct PresentHook
	{
		static std::int32_t thunk(void* a_swapChain, std::uint32_t a_syncInterval, std::uint32_t a_flags)
		{
			GetSingleton()->OnPresent(a_swapChain);
			return func(a_swapChain, a_syncInterval, a_flags);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	ClashHUD() = default;
	ClashHUD(const ClashHUD&) = delete;
	ClashHUD(ClashHUD&&) = delete;
	~ClashHUD() = default;
	ClashHUD& operator=(const ClashHUD&) = delete;
	ClashHUD& operator=(ClashHUD&&) = delete;

	// One optional PNG of the skin. Sizes are in texture pixels.
	struct Texture
	{
		void* view{ nullptr };  // ID3D11ShaderResourceView*
		float width{ 0.0f };
		float height{ 0.0f };

		[[nodiscard]] explicit operator bool() const noexcept { return view != nullptr; }
	};

	// Scoped on purpose: an earlier unscoped version was silently shadowed by
	// a local colour constant of the same name, which turned into a wild
	// array index and a crash on the first draw.
	enum class Tex : std::size_t
	{
		kFrame,         // backing behind the bar
		kFillPlayer,    // player's share, cropped from the left
		kFillOpponent,  // opponent's share, cropped from the right
		kMarker,        // moving marker at the split
		kTimer,         // time left, shrinking towards the centre

		kCount
	};
	static constexpr std::size_t kTexCount = static_cast<std::size_t>(Tex::kCount);

	[[nodiscard]] const Texture& Skin(Tex a_tex) const noexcept { return _textures[static_cast<std::size_t>(a_tex)]; }

	// What to draw for the attack button.
	struct KeyGlyph
	{
		enum class Style : std::uint8_t
		{
			kNone,
			kKey,         // keyboard key cap with the key name
			kMouse,       // mouse outline with the bound button highlighted
			kPadFace,     // A / B / X / Y
			kPadBumper,   // LB / RB
			kPadTrigger,  // LT / RT
			kPadStick,    // LS / RS click
			kPadDpad,     // d-pad with one arm highlighted
			kPadSmall     // Start / Back
		};

		Style         style{ Style::kNone };
		std::string   label;          // text on the glyph; also names the PNG override (key_<LABEL>.png)
		std::uint32_t accent{ 0 };    // IM_COL32 colour for face buttons and highlights, 0 = default
		int           variant{ -1 };  // mouse: 0 left, 1 right, 2 middle; d-pad: 0 up, 1 down, 2 left, 3 right
	};

	void OnPresent(void* a_swapChain);
	bool InitImGui();
	void LoadFont();
	bool LoadGameFont(float a_bakeSize);  // the face the game's UI uses, via fontconfig.txt
	void LoadTextures();
	void ReleaseTextures();
	void Draw(float a_alpha);
	void DrawGlyph(const KeyGlyph& a_glyph, float a_centreX, float a_bottom, float a_height, float a_alpha, float a_texel);

	// snapshot
	std::atomic<bool>  _visible{ false };
	std::atomic<bool>  _standoff{ false };
	std::atomic<bool>  _holdMode{ false };
	std::atomic<float> _meter{ 0.5f };
	std::atomic<float> _timeFraction{ 1.0f };
	std::atomic<bool>  _reloadAssets{ false };
	std::mutex         _textMutex;  // guards the names and _glyph
	std::string        _playerName;
	std::string        _opponentName;
	KeyGlyph           _glyph;

	// render-thread state
	bool   _installed{ false };
	bool   _initialized{ false };
	bool   _initFailed{ false };
	bool   _loggedFrame{ false };
	float  _alpha{ 0.0f };
	float  _fontSize{ 20.0f };
	double _lastTime{ 0.0 };
	void*  _swapChain{ nullptr };  // the game's chain (IDXGISwapChain*)
	void*  _device{ nullptr };     // ID3D11Device*
	void*  _context{ nullptr };    // ID3D11DeviceContext*
	void*  _hwnd{ nullptr };       // HWND

	std::array<Texture, kTexCount>           _textures{};
	std::unordered_map<std::string, Texture> _keyTextures;  // key_<LABEL>.png overrides, keyed by LABEL
};
