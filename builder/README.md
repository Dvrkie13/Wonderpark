# builder

The one-time builder: turns the player's own disc image into
`build/game/Wonderpark.exe`, on their PC.

- `pipeline.py`: the four stages (verify the disc, extract it, recompile,
  compile), with progress lines for the launcher; `--status` says whether
  the build is current, `--rebuild` builds again from the extracted disc.
- `launcher/`: the launcher window (`WonderparkLauncher.exe`, built by
  `tools/build_launcher.py`), drawn in code to match
  `docs/launcher-mockup.html`. Its one button runs the pipeline on a worker
  thread and then starts the game. `fonts/` holds Fredoka and Nunito with
  their licences (SIL OFL); they're built into the executable.
- `launcher/mods.cpp`: the Mods page's side: installing a mod from a .zip
  on a worker thread (its own zip reader and inflate; checked paths and
  sizes), the installed mods, and `mods.ini`. The game reads the mods at
  start (`runtime/host/mods.cpp`).

How the launcher works, its test options, and the pipeline's output lines:
`docs/DEVELOPMENT.md` ("The launcher").
