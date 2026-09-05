# Tower Events At Sunrise

A Sunrise fork for exploring the Tower's seasonal events on Destiny 2 build **86657**.
Based on [stanuwu/Sunrise](https://github.com/stanuwu/Sunrise), upstream commit
`4aebb148e92176c2b9d64a07b94068d759945853` (upstream source version 0.3.2.0).
Includes the supporting 0.3.2-era fixes from the validated event branch.
Unreleased 0.4 changes are excluded. The package release is **0.1.0**. The in-game banner and version remain the
upstream Sunrise defaults (**SUNRISE 0.3.2.0**).

## Events

Open **Server → Events** in the in-game menu to select decorations and music.
Save your selection, then return to orbit and load the Tower again.

- Festival of the Lost: Courtyard, Bazaar, Hangar and Annex.
- The Dawning: Courtyard, Bazaar snow, Hangar snow and skating rink.
- Iron Banner, Crimson Days and Solstice: their authored Courtyard decorations.
- Trials / Saint-14: the authored Hangar setup.
- The Farm: Dawning and Crimson Days decorations.
- Seasonal music, including Guardian Games. Guardian Games has no restored podium;
  its authored placement list is empty in this game build.

Festival tree pickups award **Candy** through compiled C++ server behavior:
Courtyard blues give **50**, Bazaar blues give **60**, and purples give **250**.
See [the protocol and reward notes](docs/festival-pickups.md).

This fork publishes the game's authored event roster groups and uses the normal
server inventory replication path for rewards. Its event features do not use gameplay
scripts. This does not implement every event quest, vendor reward, or seasonal gameplay activity.

## Install

1. Set up the supported old game build using the
   [upstream installation instructions](https://github.com/stanuwu/Sunrise/wiki/Installing).
2. Close the game and back up your existing `bin/x64/steam_api64.dll` and
   `bin/x64/Sunrise` folder.
3. Extract this fork's release archive into the game folder, replacing the DLL.
4. Start the game. Use **Server → Events**, then make an orbit round-trip.

The archive does not contain game assets, account settings, or game keys. Existing
settings and event selections are retained. Cache format 49 makes the runtime rebuild
older extracted content caches on first start; that start can take longer.

Event settings live in `bin/x64/Sunrise/roster_exclude_keys.txt` and `event_music.txt`.
With no exclusion file, all supported event decorations are enabled. The included
`event_presets` are optional: copy one preset's contents into `roster_exclude_keys.txt`.
At the Farm, changing music may require restarting the game.
The bundled `vendor_rules` folder contains optional upstream reference files; see its
README before copying rules beside `settings.json`. Existing authored rules take precedence.

## Build and test

Use Visual Studio 2026, the v145 C++ toolset and Windows SDK 10.0.26100.0:

```powershell
msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64
cmake -S tests -B build/tests
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

The DLL is `build/x64/Release/steam_api64.dll`. To prepare a release archive, run
`tools/package-release.ps1` after building. CI builds the DLL, runs the regression
tests and uploads the same archive.

## Upstream and license

Sunrise is created by [stanuwu and contributors](https://github.com/stanuwu/Sunrise).
This is a separate event-focused project, not an official Sunrise release.
The original Git history, [license](LICENSE), third-party notices and
[upstream README](docs/upstream-readme.md) are preserved. Report fork-specific issues
in this repository. This project is free and open source.
