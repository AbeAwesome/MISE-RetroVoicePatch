# Monkey Island SE - Retro Voice Patch
In 2009, ***The Secret of Monkey Island: Special Edition*** was released with (to quote the Steam page):
> - All new re-imagined contemporary art style, hand-drawn to feature animation quality
> - Complete voice over using the original Monkey Island™ franchise cast brings the story and characters of the original adventures to life like never before
> - Scene-for-scene hot swap allows for seamless transition between special edition and classic modes anywhere and at any time

That said, hot-swapping to "classic mode" disables the voice over; while the "contemporary art style" is an *acquired taste*, to say the least. To this day, every time I wanted to play the game, I had to use the ***Monkey Island Ultimate Talkie Edition***, extract the voice files, and run the game using ***ScummVM***; which isn't a huge deal... but it requires installing extra software and takes up between 150 to 800 MB of disk space.

This patch is a **7.5 KB** dll injector that prevents the Steam executable from muting the voice-over when "*classic mode*" is toggled on. For now, the rest of the sound effects are not effected (I believe Talkie Edition does keep some of the cleaner SE files, my patch currently doesn't).

## Install
1. Place the `d3d9.dll` in the MISE root folder (`[SteamLibrary]\steamapps\common\The Secret of Monkey Island Special Edition` by default).
2. Launch the game, use F10 to toggle to "*classic mode*", the voice-over should be intact.

Simply delete `d3d9.dll` to disable/uninstall the patch.

## Caveats
- I only tested this on the current Steam patch, I don't own the GOG version to test, and I no longer have my retail DVD.
- I only develop in JavaScript or 6502 Assembly, so apart from my diagnosis of the problem, all the work and code was done by a Claude Opus 5 agent. It's very possible that there are mistakes or oversights, that's why I am making the code public, so that people who know better can audit it.
- Checking the .dll using Virus Total, between 3 and 5 vendors flag it as potentially malicious, including Microsoft in some cases. This is a false-positive based on heuristics, but you shouldn't trust me (again, that's why the code is public)

## Implementation Notes
*this section was written by AI*

The Special Edition mutes dialogue in classic mode in **two** independent places, and both have to be undone:

1. **Audio data.** All 4,547 cues in `SpeechCues.xsb` carry an XACT curve that returns −96 dB when the game's `NewVersion` variable is 0 — which is exactly what classic mode sets it to. That curve can't simply be flattened, because it's also what mutes the *new* music and sound effects: the game fires every cue into both the "New" and "Original" soundbanks and lets the curve decide which one you hear. So it's detached from the speech bank alone, swapped for a curve that evaluates to exactly 0 dB.
2. **Code.** `MISE.exe` re-sets the Speech category volume to `(1 − classicBlend) × voiceVolume` every frame. A three-byte instruction patch removes that factor.
