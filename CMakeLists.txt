cmake_minimum_required(VERSION 3.21)

project(
	CinematicClash
	VERSION 1.0.0
	DESCRIPTION "Cinematic weapon-clash standoffs between the player and humanoid enemies."
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# CommonLibSSE-NG (https://github.com/alandtse/CommonLibSSE-NG, tag v8.0.1)
# ---------------------------------------------------------------------------
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)

# Let CommonLibSSE-NG fetch its published prebuilt binary for the checked-out
# release tag instead of compiling ~16 minutes of source. It silently falls back
# to a source build when the toolset differs or the download is unavailable.
option(COMMONLIB_PREBUILT "Use the CommonLibSSE-NG prebuilt binary when available" ON)

add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/cmake")
include(CommonLibSSE)

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
set(SOURCES
	include/PCH.h
	include/PrecisionAPI.h
	include/SmoothCamAPI.h

	# SKSE Menu Framework 3 API header, copied verbatim from
	# QTR-Modding/SKSE-Menu-Framework-3 (resources/SKSEMenuFramework.h).
	include/SKSEMenuFramework.h

	# Open Animation Replacer modder API, copied verbatim from
	# ersh1/OpenAnimationReplacer (src/API).
	include/OAR/OpenAnimationReplacer-SharedTypes.h
	include/OAR/OpenAnimationReplacer-ConditionTypes.h
	include/OAR/OpenAnimationReplacer-ConditionTypes.cpp
	include/OAR/OpenAnimationReplacerAPI-Conditions.h
	include/OAR/OpenAnimationReplacerAPI-Conditions.cpp

	# Weapon-swing parry detection, vendored from
	# doodlum/MaxsuWeaponSwingParry-ng (GPL-3.0).
	include/MaxsuWeaponParry/WeaponParry_Functions.h
	include/MaxsuWeaponParry/WeaponParry_Functions.cpp

	src/Settings.h
	src/Settings.cpp
	src/BossRecognition.h
	src/BossRecognition.cpp
	src/ClashController.h
	src/ClashController.cpp
	src/ClashAudio.h
	src/ClashAudio.cpp
	src/ClashCamera.h
	src/ClashCamera.cpp
	src/ClashDetection.h
	src/ClashDetection.cpp
	src/ClashHUD.h
	src/ClashHUD.cpp
	src/ClashInput.h
	src/ClashInput.cpp
	src/ClashMenu.h
	src/ClashMenu.cpp
	src/ClashRumble.h
	src/ClashRumble.cpp
	src/ClashTDM.h
	src/ClashTDM.cpp
	src/FileAudio.h
	src/FileAudio.cpp
	src/IniFile.h
	src/IniFile.cpp
	src/OARConditions.h
	src/OARConditions.cpp
	src/SwfFont.h
	src/SwfFont.cpp
	src/Plugin.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES})

add_commonlibsse_plugin(
	${PROJECT_NAME}
	AUTHOR Wesley
	NAME CinematicClash
	VERSION ${PROJECT_VERSION}
	USE_ADDRESS_LIBRARY
	SOURCES ${SOURCES}
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/include"
		"${CMAKE_CURRENT_SOURCE_DIR}/include/OAR"
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

# Dear ImGui (DX11 + Win32 backends) draws the standoff meter over the game;
# the DirectXTK WIC loader reads the optional PNG skin for it.
find_package(imgui CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui Microsoft::DirectXTK d3d11 dxgi d3dcompiler xaudio2 ole32)

if(MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/W4
			/permissive-
			/Zc:preprocessor
			/EHsc
			/MP
	)
endif()

# ---------------------------------------------------------------------------
# Staging: lay the built DLL next to the shipped INI and OAR files so the whole
# "package" folder can be zipped or dropped into a mod manager directly.
# ---------------------------------------------------------------------------
add_custom_command(
	TARGET ${PROJECT_NAME}
	POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/SKSE/Plugins"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${CMAKE_CURRENT_SOURCE_DIR}/package/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
	VERBATIM
)
