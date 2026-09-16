#pragma once

// The SKSE Menu Framework header uses std::wstring_convert; MSVC flags it as
// deprecated at the declaration, which the STL headers below carry.
#ifndef _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#	define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif

#pragma warning(push)
#pragma warning(disable : 4189)  // local variable is initialized but not referenced
#pragma warning(disable : 5105)  // macro expansion producing 'defined' has undefined behavior

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <Windows.h>

// Windows.h family macros that collide with engine names.
#ifdef PlaySound
#	undef PlaySound
#endif
#ifdef max
#	undef max
#endif
#ifdef min
#	undef min
#endif

#ifdef NDEBUG
#	include <spdlog/sinks/basic_file_sink.h>
#else
#	include <spdlog/sinks/msvc_sink.h>
#endif

#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;

namespace stl
{
	using namespace SKSE::stl;

	// Detour a virtual function on F's vtable. Used instead of an address-library
	// call hook wherever possible so the plugin carries as few per-runtime
	// offsets of its own as it can.
	template <class F, std::size_t idx, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[0] };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	// Same, with the slot chosen at runtime (some slots shift by one on VR).
	template <class F, class T>
	void write_vfunc(std::size_t a_idx)
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[0] };
		T::func = vtbl.write_vfunc(a_idx, T::thunk);
	}
}
