# Cinematic Clash

An SKSE plugin for Skyrim Special Edition / Anniversary Edition. When the player and a
humanoid enemy swing at each other at the same time and their weapons clash, the fight
stops for a cinematic standoff:

1. The camera swings in behind the player's shoulder.
2. Whatever both actors were playing (vanilla, MCO or BFCO attacks, recoils, staggers) is
   cut on the spot and they enter their block stance in the same frame, then slide
   chest-to-chest.
3. A quick time event runs: mash the attack button to overpower the opponent while they
   push back. The pair visibly slides back and forth as the struggle swings, and a
   tug-of-war meter at the bottom of the screen shows the balance and the time left.
4. The loser plays the large stagger, the winner is free to follow up. If the timer runs
   out with neither side pushed off the meter, it is a draw wherever the marker sits: both
   break off with a small stagger. `[Outcome] iTimeoutResolution = 1` decides those on the
   meter instead, so whichever side the marker sits nearer to wins.

## Requirements

| Requirement | Notes |
| --- | --- |
| SKSE64 | Any runtime supported by CommonLibSSE-NG (1.5.97, 1.6.x, 1.7.x). VR loads but the clash camera is disabled. |
| Address Library for SKSE Plugins | |
| Simple Weapon Swing Parry (MaxsuWeaponSwingParry-ng) | Recommended. Its `ShouldParry` check is what decides that two swings clashed; that code is vendored here so the plugin also works without it installed, but with it installed both mods share one detection. |
| Precision | Optional. When present, hits are read from Precision's pre-hit callback instead of the vanilla hit path. |
| Open Animation Replacer | Optional. Enables the custom conditions below. |
| SmoothCam | Optional. Camera control is requested through the SmoothCam API for the duration of the clash and handed back afterwards. |
| TrueHUD | Not required. When installed, its boss recognition lists are read alongside this mod's own, so `bBossesOnly` agrees with TrueHUD's boss bars. |
| SKSE Menu Framework 3 | Optional. Adds an in-game settings menu (every INI key, plus the camera presets) that writes back to the INI files. |

## Compatibility notes

**SmoothCam.** The plugin never fights SmoothCam for the camera. At the start of a clash it
calls `RequestCameraControl`; SmoothCam then runs the vanilla third-person update untouched
and our shoulder position is applied on top of that. When the clash ends the plugin calls
`SendToGoalPosition` and releases control, so SmoothCam glides back to its own goal. If
SmoothCam is enabled but refuses control, the camera part of the clash is skipped and the
rest still plays.

**Other camera mods.** The override writes the camera nodes after the game's own
third-person update (position and aim, the same way SmoothCam does) and zeroes the state's
free-rotation offsets so the engine has no reason to turn the blocking player toward the
camera. When the override ends, position, aim and FOV blend back to whatever the engine's
camera is doing over `fBlendOut`.

**First person.** A clash that starts in first person switches to third person for the
shoulder shot and, with `bRestoreFirstPerson`, switches back afterwards. With
`[Camera] bForceThirdPerson = 0` the camera stays in first person instead: the player is put
into the block and held there, movement and looking are locked, and the standoff plays from
their own eyes with no camera shot, SmoothCam request or FOV change. The view is eased level
over `fSettleTime` so the opponent's face is dead ahead, and a hidden shield (`iShieldMode`)
is hidden on the first-person model too. The same applies when the clash camera is disabled
outright. Not on VR, where the headset owns the view.

**Simple Weapon Swing Parry.** Both plugins hook the same melee-hit call site and chain
through each other in either load order. When the parry mod runs first, its
`bMaxsuWeaponParry_InWeaponParry` graph variable is honoured; otherwise the vendored check
runs. A clash swallows the triggering hit, so neither actor takes damage or recoils.

**Input while locked.** From the lock until the resolve only two keys do anything: the attack
button being mashed and the Journal key (Escape on the keyboard, Start on a gamepad), so the
game can still be paused, saved or quit through the main menu. Everything else (movement,
looking, the Tween menu, favourites and hotkeys, quicksave and quickload, the console, the
screenshot key, sprint, sheathe, shout, ...) is dropped. Movement, looking, activation and
the like are switched off through the `ControlMap`; fighting input is refused by the
handlers themselves because switching off `kFighting` sheathes the weapon; every other
button has its user-event name blanked on its way into `MenuControls` and `PlayerControls`,
which is how the engine itself represents a disabled control. Once a menu is open the filter
stands down, so the Journal is fully usable. Buttons that were already held when the lock
began are let through so the game still sees them released.

**Arrows, spells and poisons.** While the player is locked (approach and standoff) they are
made a ghost through the engine's own `Actor.SetGhost`, the switch quest scripts use for
cutscene invulnerability: projectiles and spells pass through them and melee does nothing,
so a stray paralysis arrow cannot drop them mid-clash. As a second line, hostile magic
effects cast by anyone else are refused at `MagicTarget::AddTarget` for the same window.
The flag is cleared at the resolve (or on abort); if another mod had already set it, it is
left alone. Because the ghost flag is saved with the game, the plugin records it in its
co-save and clears it when such a save is loaded.

**Precision.** The plugin registers a pre-hit callback and returns `bIgnoreHit` for the hit
that starts a clash and for any hit involving a participant while the standoff runs. It
also registers a collision-filter callback so the two locked actors pass through each
other (`bDisableCollision`) without affecting their collision with anything else; without
Precision the opponent's collision is switched off for the duration instead.

**True Directional Movement.** TDM turns the player toward the crosshair while blocking,
which fights the clash camera's aim and shows up as turn-in-place "walking" under the
block. For the duration of a clash the plugin takes TDM's yaw control through its API
(and disables its directional movement, head tracking and target lock), feeds it the held
heading, and hands everything back at the resolve. The opponent's movement controller is
likewise taken off AI driving for the duration so nothing turns it either.

**TrueHUD.** `[General] bBossesOnly` restricts standoffs to bosses, and "boss" is decided by
the same kind of lists TrueHUD uses for its boss bars: `[BossRecognition]` sections of `Race`,
`NPC`, `LocRefType` and `NPCBlacklist` keys (`Plugin.esp:0xFormID`, with `Remove*` counterparts).
The plugin reads TrueHUD's folders (`SKSE/Plugins/TrueHUD`, `SKSE/Plugins/TrueDirectionalMovement`)
when they exist and then its own `SKSE/Plugins/CinematicClash/BossRecognition/*.ini`, base file
first in each, so any TrueHUD boss patch carries over and this mod's files have the last word.
TrueHUD itself is not needed: the shipped base list is a copy of TrueHUD's vanilla one. An
actor is a boss when its race is listed, its base NPC (or the leveled template it was picked
from) is listed, or the current location marks that reference with a listed ref type (the
vanilla `Boss` marker), and it is not blacklisted. Lists are read at data load and by the
menu's "Reload from disk" button.

**Maxsu Block Overhaul / Dynamic Block Hit.** Both are behaviour patches that extend the
block state machine with anticipation, hit-reaction and block-and-slash states. While two
actors are locked, the plugin filters the animation events sent to them and drops the
ones that would enter those states (`blockAnticipateStart`, `blockHitStart`,
`BlockAndSlash`, along with attacks, bashes, movement starts and foreign `blockStop`),
so the hold is never interrupted; hits never land during a clash anyway. Outside a clash
nothing is filtered. Block Overhaul's own `blockStartOut` and housekeeping events pass.

**MCO / BFCO, and cancelling the opponent's swing.** MCO runs each attack as a state of its
own, and the transition that leaves one on `blockStart` is conditioned on
`(IsNPC == 0) && (MCO_bEnableBlockCancel == 1)` — the player only. An opponent's route out is
`MCO_EndAnimation`, an unconditional wildcard to idle, which is why the cancel sends that
first and why asking a second time for the block alone is useless to an NPC: if anything puts
them back into a swing after the cancel, `blockStart` cannot take them out of it again. So
the per-frame hold re-sends the whole cancel rather than just `blockStart`, for as long as
the actor's attack state refuses to clear, and gives up on that condition after half a second
and holds the block on its own. `IdleForceDefaultState` would end any attack outright and is
gated by nothing, but the default state is the *unarmed* one: the graph leaves it believing
the hands are empty while the weapon is still equipped and still drawn on the model, so the
actor stands wrong, cannot attack and walks unarmed until something resets the graph. It is
not used for that reason. `bDebugLog` prints each actor's `attackState` twice a second.

## Open Animation Replacer conditions

Registered when OAR is present:

| Condition | True when |
| --- | --- |
| `CinematicClash_IsInClash` | The actor is locked in a standoff (approach and mash phases). |
| `CinematicClash_IsClashWinner` | The actor won, from resolution until `fOutcomeWindow` elapses. |
| `CinematicClash_IsClashLoser` | The actor lost, from resolution until `fOutcomeWindow` elapses. |
| `CinematicClash_ClashMeter` | Compares the actor's own advantage (0 losing, 0.5 even, 1 winning) with a numeric value. |

Three replacer folders ship under
`meshes/actors/character/animations/OpenAnimationReplacer/Cinematic Clash/`:

- **Shield To Weapon Block** is complete: it holds the vanilla one-handed block animations
  renamed to the shield block file names, so during a clash a shield user blocks with the
  sword while the plugin hides the shield model (`iShieldMode`). This is the only way to
  get that result; overriding the graph's left-hand type either provokes the engine's
  equip sync into re-equipping every frame or gets re-selected back to the shield block.
- **Clash Weapon Lock** and **Clash Defeat** are templates with only a `config.json`; drop
  replacement `.hkx` files with vanilla names next to them (block idles, the large stagger)
  and they apply only during a clash. Without animation files they do nothing.

## Configuration

`Data/SKSE/Plugins/CinematicClash.ini`. Every key is documented in the file. With
[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) installed
the same keys are also in its Mod Control Panel under "Cinematic Clash", one page per INI
section, with the camera presets editable on the Camera page. The menu and the files stay in
sync both ways: a change in the menu is written to the INI in place (comments and layout
kept) the moment the widget is released, and a change saved to the INI from a text editor
shows up in the menu within a second. The ones you are most likely to tune:

- `[Difficulty] iMode` picks easy, normal or hard: the opponent pushes back at the rate of
  3, 4 or 5 attack presses per second (`fEasyPressesPerSecond` and friends) against an
  opponent of equal weapon skill, so pressing faster than that drives the meter to your
  end and slower lets it fall to theirs; a standoff that reaches the timer instead is a
  draw (`[Outcome] fDrawStaggerMagnitude`), or a win for whichever side the meter favours
  when `[Outcome] iTimeoutResolution` is set to 1.
  `fSkillInfluence` scales the push by the gap between the two governing weapon skills
  (One-Handed or Two-Handed, whichever each weapon uses). With `fAutoWinSkillGap` (default
  15) an opponent that many skill points behind the player skips the standoff and takes the
  loser's stagger on the spot; a better opponent never skips it, they only push harder.
  `bHoldToMash` is the accessibility mode: holding the attack button counts as mashing at
  the difficulty's rate, so a held button exactly matches an equal opponent and the skill
  gap decides.
- `[Standoff] fDuration`, `fPressGain` set the standoff length and how much meter each
  press is worth.
- `[Rumble]` drives the pad's motors while the weapons are locked (a held level plus a kick
  per press), honouring the game's own rumble setting. XInput pads only, which includes
  anything Steam Input presents as one.
- `[Standoff] bSolveClashDistance` (on by default) decides how far apart the pair stands.
  Both weapons are measured off their own meshes once the block pose is up — the segment
  from the hand to the far end of the blade, taken from the bounding spheres of the shapes
  hanging off the `WEAPON` node (`SHIELD` for a left-hand weapon or a shield that was left
  visible) — and the pair is stood at the widest separation, between `fSolveDistanceMin`
  and `fSolveDistanceMax`, at which the two blades still cross, less `fSolveBite`. That
  replaces `fClashDistance`, which otherwise leaves a gap or an overlap depending on which
  block animation each fighter is playing and how long their weapons are. The solve runs
  until the pose settles (`fSettleTime`) and is then held, because a target that keeps
  moving is a warp every frame and the movement system reads that as walking. Only the gap
  *along* the pair's own axis can be closed this way: a miss that is sideways or vertical
  (a height or scale difference, or two block poses that hold the guard at different
  heights) is out of reach, since the character controller owns each actor's height. When
  that happens `fClashDistance` is kept — unless `bTiltToMeet` picks it up — and the log
  says so, with the miss split into its along-axis, sideways and vertical parts. Set
  `bSolveClashDistance = 0` for the old fixed distance.
- `[Standoff] bTiltToMeet` (on by default) handles the vertical miss the separation solve
  cannot. The pair is stood at the closest the two blades come, and the fighter whose blade
  sits higher is bent down to the other one: half the drop out of the spine
  (`NPC Spine1 [Spn1]`) and half out of the sword arm's shoulder (`NPC R UpperArm [RUar]`,
  or the left one for a left-hand weapon), each joint clamped to `fTiltMaxDegrees` (12).
  Both rotations are re-applied after every animation update — the pose is written by the
  graph each frame, so a correction has to be too — and are dropped the instant the
  standoff resolves, which also means nothing has to be restored: the next update is the
  animation's own again. The separation is solved once more over the bent pose. A blade is
  a long lever, so a few degrees at the shoulder move the tip a long way: 12° at each joint
  covers a scaled-up boss, and an ordinary race-height difference needs two or three.
  Sideways misses are not addressed — those want a twist, not a pitch.
- `[Standoff] bClearWeaponClipping` (on by default) turns a blade back out of the other
  fighter when a block animation has swung it into them. The opponent is treated as one
  capsule from hips to head, `fClearanceBodyRadius` (16) wide and scaled by their size; a
  blade inside it is turned about its grip along the shortest way out, `fClearanceMaxDegrees`
  (20) at the wrist (`NPC R Hand [RHand]`, or the left one) and whatever is left over at the
  `WEAPON` node itself. The wrist goes first because the hand turns with it; a turn at the
  weapon node is the handle moving inside a fist that stays put, which shows past 15° or so.
  Unlike the tilt this is not solved once and held: it is measured every frame off a blade
  that already carries the previous frame's correction, wound on while the blade is inside
  them, and eased back to nothing once it is clear, so it tracks a live animation rather than
  a guess. Turning a blade out of a chest can lift it off the blade it is locked against —
  `bDebugLog` prints both the penetration depth and the resulting gap twice a second.
  The `WEAPON` node is the one joint here that no animation drives, so a turn applied to it
  is remembered and re-applied to what was there before rather than to its own output (a
  per-frame delta on a node nothing rewrites compounds into a spin), and it is put back
  explicitly when the correction ends. The same rule covers every node the plugin writes, so
  a frozen graph (`bFreezeAfterSettle`) or an animation update that runs twice in a frame
  cannot make a correction accumulate either.
- `[Standoff] bContactFromWeapons` (on by default) puts the sparks, the scrape loop and the
  camera's aim at the point where the two measured blades are actually closest, instead of
  the midpoint between the actors at `[Sparks] fHeight` / `[Camera] fAimHeight`. Those two
  heights are the fallback for when a weapon's mesh cannot be read.
- `[Standoff] fTimeMultiplier` adds slow motion (1.0 = off).
- `[Standoff] fSettleTime` is how long the actors keep being re-aimed at each other after
  contact; after it their headings freeze and head/spine tracking is off until the clash
  resolves. `bFreezeAfterSettle` (off by default) additionally halts the opponent's graph
  so their pose holds dead still (the player keeps animating); left off, idle motion continues,
  which reads better in the scene.
- `[Camera] fOffsetX`, `fOffsetY`, `fOffsetZ` frame the shot (right, forward, up from the
  player's feet, in the player's own frame). With `bAimAtContact` on (default) the camera
  looks at the point where the weapons meet from wherever the offsets put it, so a side
  view is just a sideways offset; `fYawDegrees` and `fPitchDegrees` nudge that aim.
  `fFOV` sets the field of view for the clash and hands the previous value back after.
  `sPreset` lists sections of `SKSE/Plugins/CinematicClash_CameraPresets.ini`, comma
  separated; each one overrides those framing keys (offsets, aim, angles, FOV, blend times)
  with a saved shot. Every clash draws one of the listed presets at random. A preset whose
  camera position is walled off from the contact point (a line-of-sight ray with
  `fWallMargin` units of clearance past the camera) is skipped and another drawn; when all
  of them are, the first listed is used. The file ships with `LowSide`, `TightShoulder`,
  `Profile`, `LowHero` and `ReverseShoulder` and is documented inline. Both files
  hot-reload; the next clash uses the new values.
  `bForceThirdPerson` (default on) decides what a clash that starts in first person does:
  switch to third person for the shot, or stay in first person for the whole standoff (see
  the first-person compatibility note above).
- `[Sparks]` controls the shower of sparks at the contact point (interval, height, scale,
  optional model override).
- `[Audio]` sets the lock clang, the scrape kept going during the standoff, the overpower
  blow and the opponent's taunt. Each sound is a descriptor editor ID (game audio, 3D) or
  a loose `.wav` path under `Data`, played on the plugin's own XAudio2 voices (2D, gapless
  loop, follows the master volume, pauses with the menu). Sounds default to the weapon's
  own block-impact descriptors; the taunt is a real voice line through the dialogue system.
- `[HUD]` sizes and positions the tug-of-war meter. `sTextureFolder` names a folder under
  `Data` with an optional PNG skin (`frame.png`, `fill_player.png`, `fill_opponent.png`,
  `marker.png`, `timer.png`, plus `key_<LABEL>.png` for the attack-button glyph shown
  during the mash); each file present replaces the drawn version of that element, and
  missing ones keep the built-in drawing. The glyph follows whatever the game has bound
  to attack on the active device (key cap, mouse button, or Xbox-style pad button).
  The PNGs share one pixel grid (`iTextureGridWidth`, default 1024 = the bar width). Text
  uses the game's own UI font, read from `Interface/fontconfig.txt` and its fontlib SWFs
  so font replacer mods carry over (`sGameFont` picks the mapping, `sFontFile` swaps in a
  `.ttf` instead). The shipped
  `SKSE/Plugins/CinematicClash/HUD/README.txt` gives the sizes. The skin reloads whenever
  the INI is saved.
- `[General] fTriggerChance`, `fCooldownSeconds` control how often standoffs happen.
  `bBossesOnly` limits standoffs to opponents the boss recognition lists class as bosses (see
  the TrueHUD compatibility note above for the format and folders).
- `[Debug] bForceClashOnHit` starts a clash on every melee hit the player lands on a
  humanoid NPC, ignoring the parry check, hostility, bosses-only and chance. For testing only.

The INI is re-read whenever the file changes on disk, so every value can be tuned while
the game is running; the next clash uses the new values.

## Building

Requires Visual Studio 2022 (MSVC 14.44), CMake, Ninja and vcpkg with `VCPKG_ROOT` set.

```
build.bat
```

The DLL is copied into `package/SKSE/Plugins/` after a successful build, so the `package`
folder is a ready-to-install mod. `deploy.ps1` copies it into a Mod Organizer 2 mod folder
(`-Target` to choose one); an INI already there (the main one and the camera presets file) is
left untouched apart from appending any keys or sections it lacks. CommonLibSSE-NG is vendored under
`extern/CommonLibSSE-NG` (tag v8.0.1, with its `openvr` submodule checked out; a fresh
clone needs `git submodule update --init extern/openvr` inside it). With
`COMMONLIB_PREBUILT=ON` (the default) it downloads its published prebuilt library instead
of compiling from source when the MSVC toolset matches; otherwise the first build compiles
CommonLib once and later builds are incremental.

## Source layout

| Path | Purpose |
| --- | --- |
| `src/ClashDetection.*` | Melee-hit hook and Precision callback; runs the vendored parry check. |
| `src/ClashController.*` | State machine: approach, standoff (QTE), resolve, aftermath. Actor positioning, animation events, control and AI locks, and the post-animation spine/shoulder bend. |
| `src/BladeGeometry.*` | Where an actor's weapon actually is: the blade as a segment from the hand to the tip, measured off the loaded 3D, the torso as a capsule, and the closest-point solves between them. |
| `src/ClashCamera.*` | `ThirdPersonState::Update` hook, shoulder camera, SmoothCam API handshake. |
| `src/ClashInput.*` | Reads attack presses (and the held state, for hold-to-mash) from the raw input stream during the standoff; filters every other button out of `MenuControls` and `PlayerControls` while the player is locked. |
| `src/ClashRumble.*` | Controller rumble through XInput: a held level for the standoff, pulses per press, cut while a menu pauses the game. |
| `src/ClashHUD.*` | Tug-of-war meter drawn with Dear ImGui from an `IDXGISwapChain::Present` hook. |
| `src/ClashTDM.*` | True Directional Movement API handshake: yaw control, directional movement, head tracking and target lock for the duration of a clash. |
| `src/BossRecognition.*` | Boss classification for `bBossesOnly` from `[BossRecognition]` INI lists (TrueHUD's format and folders, plus this mod's own). |
| `src/OARConditions.*` | Custom Open Animation Replacer conditions. |
| `include/MaxsuWeaponParry/` | `ParryCheck` from MaxsuWeaponSwingParry-ng, unchanged apart from dropping a `FMT_STRING` wrapper. |
| `include/OAR/` | Open Animation Replacer modder API, copied verbatim. |
| `include/PrecisionAPI.h`, `include/SmoothCamAPI.h`, `include/TrueDirectionalMovementAPI.h` | Third-party plugin APIs, copied verbatim. |

## License

GPL-3.0-or-later. The vendored parry detection is GPL-3.0 (see
`include/MaxsuWeaponParry/COPYING`) and CommonLibSSE-NG is GPL-3.0-or-later, which this
project follows.
