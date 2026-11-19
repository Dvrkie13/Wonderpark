# Contributing to Wonderpark

Thanks for wanting to help! Bug reports, tests on other PCs and controllers, and code are all welcome. A few rules keep the project safe for everyone:

## Never add game files

Don't attach or commit anything from the game: disc images, extracted files, recompiled or decompiled code, textures, models, audio, fonts, text (message files or text copied out of them), icons, layouts, saves with game data, or screenshots and video of the game in the repository itself. Wonderpark builds all of that on each player's PC from their own disc, and `.gitignore` keeps `game/`, `generated/` and `build/` out of git.

Screenshots, video and logs are fine **in an issue** to show a bug.

## Only code we're allowed to use

- Wonderpark is GPL-3.0-or-later ([LICENSE](LICENSE)). By sending code you agree to release it under that license.
- Don't copy code from projects whose license doesn't allow it in ours (GPL-3.0-compatible licenses only).
- Code from CC0 projects (such as MildRain479's PokePark-Wiicompiled and PokePark-2-Wiicompiled) is welcome, with credit in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the README.
- Never copy decompiled game code from any project, whatever its license.
- If you add a library or asset, add it to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) with its license.

## No links to games

Don't post or ask for links to games, ISOs or ROMs, anywhere in the project. Such issues are closed. To make your own disc image, see [Dolphin's guide to ripping games](https://dolphin-emu.org/docs/guides/ripping-games/).

## Text we write ourselves

The Friend Book shows the game's own hint for each Pokémon, read from the player's disc at run time; none of that text is in the repository. If Wonderpark ever gets hint texts of its own (`data/friend-hints.json`), they must be written in our own words, keyed by the game's internal ID, with no names or text from the game. Never paste text from wikis or guides.

## How to work on it

- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): the commands, where each kind of fix belongs (game behavior goes in `config/` patches or `runtime/hle/` hooks, never in generated code), and every setting and environment variable.
- [docs/recomp-status.md](docs/recomp-status.md): what has been found so far.
- Small commits with clear messages. Update `docs/DEVELOPMENT.md` when you add a setting, environment variable or patch.

Wonderpark is free and non-commercial: no donations, sponsorships or ads.
