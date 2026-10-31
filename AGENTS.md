# Project coordination

- Canonical repository: https://github.com/yshdsnd/mc-hires-bridge
- Track bugs, enhancements, and follow-up work in this repository's GitHub Issues.
- Never commit private measurement logs, user paths, music metadata, PCM, generated binaries, or build directories.
- Keep the normal observation DLL and bypass variants separate: they hook the same functions and must not be loaded together.
- Document live playback verification separately from build-only checks. Do not claim 24bit output or general compatibility from the current 96kHz/16bit test.
- The project's own code uses the BSD 2-Clause License in LICENSE. Preserve the separate bundled MinHook license.
