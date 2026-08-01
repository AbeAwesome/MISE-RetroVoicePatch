# Monkey Island SE - Retro Voice Patch
In 2009, ***The Secret of Monkey Island: Special Edition*** was released with (to quote the Steam page):
> - All new re-imagined contemporary art style, hand-drawn to feature animation quality
> - Complete voice over using the original Monkey Island™ franchise cast brings the story and characters of the original adventures to life like never before
> - Scene-for-scene hot swap allows for seamless transition between special edition and classic modes anywhere and at any time

That said, hot-swapping to "classic mode" disables the voice over; while the "contemporary art style" is an *acquired taste*, to say the least. To this day, every time I wanted to play the game, I had to use the ***Monkey Island Ultimate Talkie Edition***, extract the voice files, and run the game using ***ScummVM***; which isn't a huge deal... but it requires installing extra software and takes up between 150 to 800 MB of disk space.

This patch is a **10 KB** proxy DLL that prevents the Steam executable from muting the voice-over when "*classic mode*" is toggled on. It also allows the **optional** enabling of SE's ambient sounds and some extra sound-effects.

## User Guide
### Installation
1. Place `d3d9.dll` in the MISE root folder (`[SteamLibrary]\steamapps\common\The Secret of Monkey Island Special Edition` by default).
2. Launch the game, use **F10** to toggle to "*classic mode*", the voice-over should be intact.

Simply delete `d3d9.dll` to disable/uninstall the patch.

### Hotkeys (*Classic Mode only*)
- **F9** toggles *ambient sounds*
- `,`, `.`, and the `Right Mouse Button` skip dialog line-by-line.
- `Mouse Wheel` scrolls through dialog options.


## Notes
### Caveats
- I only tested this on the current Steam patch, I don't own the GOG version to test, and I no longer have my retail DVD.
- I only develop in JavaScript or 6502 Assembly, so apart from my diagnosis of the problem, all the work and code was done by a Claude Opus 5 agent. It's very possible that there are mistakes or oversights, that's why I am making the code public, so that people who know better can audit it.
- Checking the .dll using VirusTotal, between 3 and 5 vendors flag it as potentially malicious, including Microsoft in some cases. This is a false-positive based on heuristics, but you shouldn't trust me (again, that's why the code is public).
- Check [`IMPLEMENTATION.md`](IMPLEMENTATION.md) to read implementation notes from the Opus 5 AI agent. *Written by AI, like the code.*

### Changelog
- **v0.6.5** — Extra SE sound-effects in classic mode (`F9`). Dialog options scrolling (`Mouse Wheel`). Fixed/adjusted `F9` and dialog-skip behavior.
- **v0.5.2** — Ambient sounds in classic mode (`F9`). Line-skip restored (`,`, `.`, or `RMB`).
- **v0.4** — Fixed subtitle/voice de-sync.
- **v0.3** — First public release: voice acting in classic mode.