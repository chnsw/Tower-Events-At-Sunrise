# Tower Events At Sunrise

<p align="center">
  <a href="https://github.com/chnsw/Tower-Events-At-Sunrise/releases/tag/v0.1.0"><img src="https://img.shields.io/badge/release-0.1.0_preview-cab174?style=flat-square&amp;labelColor=17212b" alt="Release: 0.1.0 preview"></a>
  <a href="https://github.com/stanuwu/Sunrise/releases/tag/0.3.2"><img src="https://img.shields.io/badge/Sunrise-0.3.2.0-9db3c8?style=flat-square&amp;labelColor=17212b" alt="Based on Sunrise 0.3.2.0"></a>
  <img src="https://img.shields.io/badge/game_build-86657-9db3c8?style=flat-square&amp;labelColor=17212b" alt="Destiny 2 game build 86657">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-cab174?style=flat-square&amp;labelColor=17212b" alt="License: GPL 3.0"></a>
</p>

<p align="center">
  <strong><a href="https://github.com/chnsw/Tower-Events-At-Sunrise/releases/tag/v0.1.0">Download the preview</a></strong>
  &nbsp; · &nbsp; <a href="#getting-started">Getting started</a>
  &nbsp; · &nbsp; <a href="#choose-your-event">Event guide</a>
  &nbsp; · &nbsp; <a href="https://github.com/chnsw/Tower-Events-At-Sunrise/issues">Report a bug</a>
</p>

## Revisit the Tower’s seasonal celebrations

Festival trees and candlelight. Dawning snow and the Hangar rink. Crimson Days decorations at the Farm.

**Tower Events At Sunrise** brings the seasonal content authored into Destiny 2 build **86657** into one configurable Sunrise experience. Pick your decorations, choose the music, and explore.

Built on **Sunrise 0.3.2.0**, with the supporting fixes from the tested event branch. The in-game **SUNRISE** banner and version stay unchanged; **0.1.0** identifies this fork’s preview package.

| Make the Tower your own | What you can do |
| :--- | :--- |
| **Choose the decorations** | Show individual events, combine them, or return to an undecorated Tower from the **Events** page. |
| **Set the soundtrack** | Choose a seasonal theme or let the game follow the events you have enabled. |
| **Collect Festival Candy** | Pickups award Candy through the compiled C++ server and normal inventory replication. |
| **Visit the Farm** | Explore its authored Dawning and Crimson Days decorations. |

## Choose your event

Each event appears where its content was authored in this game build.

| Event | Tower locations | Also included |
| :--- | :--- | :--- |
| **Festival of the Lost** | Courtyard · Bazaar · Hangar · Annex | Seasonal music and Candy pickups |
| **The Dawning** | Courtyard · Bazaar snow · Hangar snow and rink | Farm decorations and seasonal music |
| **Crimson Days** | Courtyard | Farm decorations and seasonal music |
| **Solstice of Heroes** | Courtyard | Seasonal music |
| **Iron Banner** | Courtyard | Authored event decorations |
| **Trials of Osiris / Saint-14** | Hangar | Saint-14 and his ship |
| **Guardian Games** | Music only | Its podium placement list is empty in this build |

### Festival Candy rewards

| Pickup | Candy |
| :--- | ---: |
| Courtyard blue | **50** |
| Bazaar blue | **60** |
| Purple in either tree | **250** |

Rewards use the authored pickup’s identity, so the two blue pickup amounts stay distinct. The event features use **native roster publication and C++ server behavior**, with no gameplay scripts. [Read the implementation and evidence notes →](docs/festival-pickups.md)

> **Preview scope:** Decorations, music and the supported Festival pickups are available. This is not a complete restoration of every event quest, vendor reward or seasonal activity.

## Getting started

**Requires Windows x64 and the supported Destiny 2 build 86657.** Start with the [Sunrise installation guide](https://github.com/stanuwu/Sunrise/wiki/Installing) if you do not already have a working installation.

1. **Back up your installation.** Close the game and copy `bin/x64/steam_api64.dll` and the `bin/x64/Sunrise` folder somewhere safe.
2. **Download the [preview ZIP](https://github.com/chnsw/Tower-Events-At-Sunrise/releases/tag/v0.1.0).** Extract it into the game folder and replace the DLL.
3. **Open the Events page.** Start the game, open the Sunrise menu, and choose **Events**.
4. **Choose your atmosphere.** Save your decorations and music, then go to orbit and load the Tower again.

The first start may take longer while Sunrise rebuilds its extracted content cache. The package includes the DLL, presets, reference vendor rules and license notices. **Game assets, keys and personal account settings are not bundled.**

<details>
<summary><strong>Event settings and presets</strong></summary>

Your selections live beside `settings.json` in `bin/x64/Sunrise`:

| File | Purpose |
| :--- | :--- |
| `roster_exclude_keys.txt` | Event groups to hide. Without this file, all supported decorations are enabled. |
| `event_music.txt` | The selected seasonal music theme. |
| `event_presets/` | Optional ready-made selections. Copy a preset’s contents into `roster_exclude_keys.txt`. |

Decoration changes take effect on the next destination load. At the Farm, a music change may require restarting the game.

The `vendor_rules` folder contains optional reference files. Read its README before copying any rules beside `settings.json`; keep your existing custom rules when updating.

</details>

<details>
<summary><strong>Returning from an unreleased Sunrise 0.4 build?</strong></summary>

Restore your **0.3.2 settings backup** first. The 0.4 build migrates `settings.json` to a schema that 0.3.2 cannot read. This fork stays on **0.3.2.0**.

</details>

## Build from source

Use **Visual Studio 2026**, the **v145 C++ toolset**, and **Windows SDK 10.0.26100.0**. Run these commands from a Developer PowerShell with CMake available:

```powershell
msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64
cmake -S tests -B build/tests
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

The DLL is written to `build/x64/Release/steam_api64.dll`. Run `./tools/package-release.ps1` to create the distributable ZIP. The [build workflow](https://github.com/chnsw/Tower-Events-At-Sunrise/actions/workflows/build.yml) builds the DLL, runs the regression tests and packages the result.

## Credits and project scope

Built on **[Sunrise by stanuwu and contributors](https://github.com/stanuwu/Sunrise)**. The release baseline is [`0.3.2`](https://github.com/stanuwu/Sunrise/releases/tag/0.3.2), commit `4aebb148e92176c2b9d64a07b94068d759945853`, with the supporting 0.3.2-era changes from the validated event branch.

This is an independent community fork. The original Git history, [upstream README](docs/upstream-readme.md), [GPL-3.0 license](LICENSE) and third-party notices are preserved. Please report fork-specific issues [here](https://github.com/chnsw/Tower-Events-At-Sunrise/issues).

Free and open source. Made for exploring and preserving the Tower’s seasonal spaces.
