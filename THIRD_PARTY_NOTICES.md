# Third-party notices

IntelLite's own code and content are licensed GPL-3.0-or-later (see LICENSE). It also contains portions adapted from IntelEngine-NativePlugin under the Apache License 2.0. It builds on the work below.

## Included in this repository

| File(s) | Component | Notes |
| --- | --- | --- |
| `src/vendor/skyrimnet/PublicAPI.h`, `PublicAPIMemoryQuery.h`, `PublicAPIDiaryQuery.h` | SkyrimNet public C++ API (MinLL) | Distributed as part of the SkyrimNet developer kit for integration. All rights remain with the SkyrimNet author; not covered by IntelLite's GPL. See https://github.com/MinLL/SkyrimNet-GamePlugin |
| `src/vendor/magelight/MagelightUI_API.h` | Magelight UI public API header (Severause) | MIT; see https://github.com/Severause/MagelightUI |
| `IntelLiteData.esp` | IntelLite authored records | GPL-3.0-or-later. Uses vanilla record templates; no Bethesda assets are redistributed. |
| `mod/` (Magelight pages and the SkyrimNet content bundle) | IntelLite authored content | GPL-3.0-or-later. |

## Adapted source code

| Component | Source | License and requirements |
| --- | --- | --- |
| IntelEngine-NativePlugin | https://github.com/galanx/IntelEngine-NativePlugin | Copyright 2026 Galanx. Apache License 2.0; the full text is in `LICENSES/Apache-2.0.txt` and the required NOTICE is in `NOTICE`. Portions of the travel-package, NPC-index, location-resolver, fuzzy-matching and Papyrus-VM dispatch code are adapted from it; the affected files carry a change notice. "IntelEngine" is a trademark of Galanx; IntelLite is not affiliated with or endorsed by Galanx. |

IntelLite as a whole is distributed under GPL-3.0-or-later. The IntelEngine-derived portions remain available under Apache-2.0, which is compatible with GPLv3; the Apache-2.0 NOTICE, attribution and trademark terms continue to apply to those portions.

## Linked into IntelLite.dll, or used at build time

| Component | What it is | License / notes |
| --- | --- | --- |
| CommonLibSSE-NG (alandtse/CommonLibVR, branch ng) | SKSE plugin framework | GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception; see https://github.com/alandtse/CommonLibSSE-NG (COPYING.txt and EXCEPTIONS.md). |
| spdlog | logging | MIT |
| fmt | formatting | MIT |
| nlohmann/json | JSON for the panel | MIT |
| DirectXMath | CommonLib dependency | MIT |
| DirectXTK | CommonLib dependency | MIT |
| rapidcsv | CommonLib dependency | MIT |
| Xbyak | CommonLib dependency (listed in vcpkg.json) | BSD-2-Clause |
| CommonLib's bundled third-party components (for example hde64) | Bundled with CommonLibSSE-NG | See CommonLibSSE-NG's COPYING.txt, EXCEPTIONS.md and licenses/ directory. |
| Microsoft Visual C++ runtime | the DLL uses the dynamic CRT; users install the Microsoft Visual C++ Redistributable 2015-2022 (x64) | Microsoft Visual C++ runtime terms |

## Runtime requirements (installed by the player, not redistributed)

SKSE64, Address Library for SKSE Plugins, SkyrimNet, Magelight UI, PapyrusUtil
SE, powerofthree's Papyrus Extender, and powerofthree's Tweaks. Each is the
work of its own authors and under its own terms.

The Elder Scrolls V: Skyrim is (c) Bethesda Softworks / ZeniMax Media. This is
an unofficial fan modification. SkyrimNet is the work of its developer;
IntelLite is an independent add-on and not part of SkyrimNet.
