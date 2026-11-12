# runtime/ios

IOS, the Wii's I/O processor, as far as the game uses it (ported from the
reference runtime).

- `ios.c`: request routing (open, read, write, ioctl) to devices.
- `dvd.c`: the disc, served from the extracted files in `game/`; with Fast loading, a thread reads them into RAM at start (all but movies and streamed music), and reads of a file in RAM are copies.
- `nand.c`: the NAND file system, stored in `%APPDATA%\Wonderpark\nand` (saves); its `/tmp` is in the cache folder beside the program and emptied at start.
