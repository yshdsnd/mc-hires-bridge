# Project coordination

- Canonical repository: https://github.com/yshdsnd/mc-hires-bridge
- Track bugs, enhancements, and follow-up work in this repository's GitHub Issues.
- Never commit private measurement logs, user paths, music metadata, PCM, generated binaries, or build directories.
- Keep the normal observation DLL and bypass variants separate: they hook the same functions and must not be loaded together.
- Document live playback verification separately from build-only checks. Shared 24bit output was verified only for the scoped flac24_override experiment with 96kHz/24bit/stereo FLAC on Music Center 2.7.4 x86; do not generalize it to other formats, versions, or the old 16bit timing bypass.
- The project's own code uses the BSD 2-Clause License in LICENSE. Preserve the separate bundled MinHook license.
