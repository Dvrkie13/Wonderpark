# The Wonderpark mod list (wonderpark-mods-index)

The launcher's **Extras > Mods > Get mods** reads a curated list: one JSON
file in a separate repository, `wonderpark-mods-index`. Wonderpark hosts
nothing but that list; each mod is downloaded from its author's own link.
Until the list exists and the project is public, the page says "The mod
list isn't online yet". Its address is `[mods] index_url` in
`%APPDATA%\Wonderpark\launcher.ini`.

Each entry:

```json
{ "id": "sunset-glow", "name": "Sunset Glow", "author": "parkfan",
  "summary": "Warmer, golden evening light over the whole park.",
  "version": "1.0", "size": 2097152,
  "url": "https://the-authors-own-site/sunset-glow-1.0.zip",
  "sha256": "…", "icon": "https://…/icon.png" }
```

The launcher downloads from `url`, checks `sha256`, and installs it like
any .zip (the same checks as Install from .zip).

## Checklist before a mod is added

A mod goes on the list only if **every** box is ticked. When in doubt, it
stays off.

- [ ] Downloaded from the author's own link; its sha256 written down from
      that download.
- [ ] `tools/check_release.py <the unzipped mod>` says "Clean" (no disc
      files, no game file formats, no recompiled code).
- [ ] No files from the game, even changed: no textures exported from the
      game and edited, no `files\` that are the game's own files with
      changes, no game text, sound, models or fonts. Textures must be the
      author's own drawings.
- [ ] Not an HD pack made from the game's art (Wonderpark's own HD Pack, an
      AI-upscaled dump, or anyone else's): those are always refused.
- [ ] No code: only `mod.json`, `textures\` and `files\` that are the
      author's own work.
- [ ] The author owns what's in it, or it's under a license that lets it be
      shared (named in the entry's summary or the mod's mod.json).
- [ ] Nothing hateful, sexual, or that imitates official Nintendo, The
      Pokémon Company, Creatures or Game Freak material (logos, box art).
- [ ] Tried in the game: it installs, switches on and off, and nothing
      breaks.

Checked by: (name), on (date). Removed at once if any of this turns out to
be wrong.
