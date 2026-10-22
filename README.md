# IntelLite

IntelLite is a SkyrimNet companion SKSE plugin. It gives NPCs a small set of concrete actions they can propose in dialogue and
carry out in the world: fetching a person, delivering a message, arranging a later meeting, and taking an assassination contract. The player can issue the same actions from a MagelightUI panel.

## Status

- **Skyrim AE 1.6.1170 is the only runtime played and tested.**
- The same DLL is built for SE 1.5.97 and other AE 1.6.x / 1.7.x builds, but **those are
  untested**.
- **Skyrim VR is not tested / support is unknown.**

## Requirements

- Skyrim
- SKSE64 for the exact game version.
- Address Library for SKSE Plugins, v13 all-in-one.
- SkyrimNet 0.25.0 or newer, configured and working with its own dependencies.
- Magelight UI 0.16.0 or newer.
- PapyrusUtil SE and powerofthree's Papyrus Extender.
- powerofthree's Tweaks (editor IDs).
- Microsoft Visual C++ Redistributable 2015-2022 (x64).
- `IntelLiteData.esp` (included, ESL-flagged) enabled. Without it, travel falls back to simulation.

## Installing

1. Install the release archive with MO2 or Vortex.
2. Enable `IntelLiteData.esp`.
3. Launch the game through SKSE.
(4. Open the panel with the hotkey. Right Alt by default.)

The first run creates `Data/SKSE/Plugins/IntelLite/settings.json`. Under MO2, writes there go to the overwrite folder. There is no load-order position requirement; the plugin is ESL-flagged.

The release consists of one DLL (`IntelLite.dll`), the authored plugin (`IntelLiteData.esp`), a small SkyrimNet content bundle (id `jykej.intellite`, one character-bio awareness submodule) and the Magelight page files. There are no gameplay Papyrus scripts.

## What it does

IntelLite registers five actions natively with SkyrimNet: Fetch Person, Deliver Message, Meet Later, Assassinate and Cancel All. NPCs can propose the actions during dialogue, and the player can issue them from the panel. An NPC-initiated action raises a consent card (optional) with Agree, Decline and Change. Both On-Screen and Off-Screen travels are supported. 

### Panel

The Actions tab lists nearby NPCs, lets you pick an executor and a target, choose an action, fill in its parameters and Execute. Active Actions lists the running tasks and offers Cancel for each. The Settings tab holds the confirmation toggle, a hotkey hint, the blacklist and version info. Opening the panel pauses the game and suppresses SkyrimNet's native hotkeys while it is open.

### Fetch Person

The executor walks to the target and escorts them back. The target then lingers near the player and is released by distance (about 800 units), death, unload or a space change.

### Deliver Message

The executor walks to the recipient, delivers the message (a SkyrimNet persistent event, a HUD line, and an entry in the recipient's message history) and is released there. There is no return leg.

### Meet Later

The executor travels to a named place, or to "Here" (the spot where you made the arrangement), and waits. The task completes when you arrive, or after a 6-hour grace period. The delay can be set from 1 to 48 hours.

### Assassinate

The executor travels to the target and kills them for an optional fee (0 means free). Only essential targets are refused; protected targets are killable. A mortal target that is not 3D-loaded is resolved off-screen. A 3D-loaded mortal target gets a staged approach and the vanilla paired killmove. A 3D-loaded essential target gets a cosmetic swing and a refusal, not a kill. A bounty guard clears the crime-gold increase.
A committed assassination is not cancellable.

### Cancel All

Cancel All cancels every running task for the agent. A committed assassination is skipped.

### Names and travel

A new task for an agent supersedes that agent's previous task. Names are resolved fuzzily, tolerating case, punctuation and typos, with an editorid blacklist you can edit in the panel. Travel is package-based (PapyrusUtil and powerofthree's Papyrus Extender package overrides) when `IntelLiteData.esp` is available. Far or off-screen legs complete on a game-time deadline, with door and gate staging and a simulated fallback. Active task rows show an ETA.

### Awareness

The SkyrimNet content bundle lets an NPC know its own pending meeting, contract and delivered messages through its bio prompt.

Task state and the confirmation setting persist in the co-save and survive save/load.

Failed Off-Screen assassination attempts make a world knowledge entry gated to the relevant crime and town faction. 

## Configuration

### settings.json

`Data/SKSE/Plugins/IntelLite/settings.json` takes either a named key or a numeric scancode:

```
"hotkey": "<name>"
"hotkeyScancode": <1-255>
```

The numeric value wins if both are present. The default is Right Alt. The file is read at game start; there is no in-game key capture, so edit the file and restart. A `_help` line in the file lists names and example codes: RightAlt=184, G=34, F10=68.

### blacklist.json

`blacklist.json` is a JSON array of NPC base editorids that the fuzzy resolver must never pick. Edit it from the Settings tab, or with the Exclude button on a search row.

### Confirmation

The confirmation toggle is stored in the co-save. With it off, NPC-initiated actions execute directly without the consent card.

## Compatibility

| Runtime | SKSE | Status |
| --- | --- | --- |
| SE 1.5.97 | 2.0.20 | Untested |
| AE 1.6.1170 | 2.2.6 | Tested |
| AE 1.6.1179 (GOG) | 2.2.6 GOG | Untested |
| AE 1.7.104 | 2.3.1 | Untested |
| VR 1.4.15 | SKSEVR | Unknown |

Other AE builds use their matching SKSE and Address Library; all are untested. Mods that override follower AI or packages can affect walking: IntelLite uses package overrides at priority 100 for travel and 95 for the linger sandbox. The mod can be removed from an existing save; tasks simply stop.

## Troubleshooting

The log is at `Documents/My Games/Skyrim Special Edition/SKSE/IntelLite.log` (the GOG path differs). `skse64.log` is also useful.

The startup banner, written at the end of `kDataLoaded`, reports what was found:

```
IntelLite: runtime <SE|AE|VR> <version> | SkyrimNet <API vN|absent> | Magelight <version|absent> | movement <package travel|simulation>
```

| Symptom | Fix |
| --- | --- |
| `SkyrimNet absent` | Install and enable SkyrimNet 0.25 or newer; check `skse64.log`. |
| `Magelight absent` | Install and enable Magelight UI 0.16 or newer; the panel and consent card need it. |
| `movement simulation` | Enable `IntelLiteData.esp`; check PapyrusUtil SE and powerofthree's Papyrus Extender. |
| SKSE error about Address Library | Install Address Library v13 all-in-one matching the game version. |
| Hotkey unavailable | Edit `settings.json` to another key and restart. |
| Task stuck | The log names the leg and the recover reason. A locked door, combat or an off-screen deadline explains most cases. Include the log in a report. |

When reporting, include the game version, SKSE version, Address Library version, SkyrimNet version, Magelight version, `IntelLite.log`, and what you were doing.

## Building

See [BUILD.md](BUILD.md). In short: Visual Studio 2022 with the C++ workload, CMake 3.21 or newer, and vcpkg. Clone CommonLibSSE-NG (alandtse/CommonLibVR, branch `ng`) to `refs/CommonLibVR`, then run:

```
cmake --preset vs2022
cmake --build --preset vs2022
```

The `ninja` preset is also available. The output is `IntelLite.dll` plus a PDB. `IntelLiteData.esp` is authored content and is not produced by the build.

Repo layout:

- `src/` - C++ sources.
- `mod/` - Data-shaped content.
- `LICENSES/`, `NOTICE` - third-party license text and attribution notices.
- `cmake/deploy.cmake`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` - build files.
- `BUILD.md` - build instructions.

## License

IntelLite is licensed GPL-3.0-or-later; see [LICENSE](LICENSE). The LICENSE file is the
GNU GPL v3 text.

Portions adapted from IntelEngine-NativePlugin (Copyright 2026 Galanx) are licensed under the Apache License 2.0; the full text is in
[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) and the required NOTICE is in [NOTICE](NOTICE). Apache-2.0 is compatible with GPLv3, and those portions remain available under Apache-2.0. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

CommonLibSSE-NG is GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No Bethesda assets are redistributed. IntelLite is an independent add-on, not part of SkyrimNet.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

## Credits

- SkyrimNet (MinLL) for the public C++ API and runtime.
- Magelight UI (Severause) for the UI host.
- CommonLibSSE-NG (alandtse) for the plugin framework.
- PapyrusUtil SE, powerofthree's Papyrus Extender and powerofthree's Tweaks (powerofthree).
- IntelEngine-NativePlugin (Galanx) - portions of the travel-package, NPC-index, location-resolver, fuzzy-matching and Papyrus-VM dispatch code are adapted from it under the Apache License 2.0. IntelLite is not affiliated with or endorsed by Galanx; the name is used only to describe the origin and give credits. See [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Bethesda for Skyrim.
