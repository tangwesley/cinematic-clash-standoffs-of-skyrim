#pragma once

// ---------------------------------------------------------------------------
// In-game settings menu, through SKSE Menu Framework 3 (optional at runtime:
// without its DLL nothing is registered and the INIs remain the only way in).
//
// One page per INI section, built from Settings::Entries() so the menu and
// the INI can never list different keys. A change is written straight back to
// CinematicClash.ini (or the presets file) in place, comments and all, and the
// file is re-read so what the menu shows is exactly what is on disk. The other
// direction works too: while a page is open the files are polled once a
// second, and an edit saved from a text editor shows up in the widgets.
// ---------------------------------------------------------------------------
class ClashMenu
{
public:
	// Call at kPostLoad: every SKSE plugin, the framework's DLL included, has
	// been loaded by then, so its exports resolve.
	static void Register();
};
