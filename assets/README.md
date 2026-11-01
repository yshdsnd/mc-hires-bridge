# Launcher icon

`launcher.png` is the user-approved, AI-generated artwork: musical notes flying out of an open box, on a transparent background. It was generated with ChatGPT's built-in image generation tool for this project.

`launcher.ico` contains PNG-compressed images at 16, 24, 32, 48, 64, 128 and 256 pixels, preserving alpha transparency. It is embedded in the launcher through `launcher.rc`.

To regenerate on Windows (PowerShell and System.Drawing; no Python dependency):

```powershell
.\tools\make-icon.ps1
```

The committed ICO is used by normal CMake builds, so icon conversion is not needed to build the EXE. The artwork and conversion script are included under the project's BSD 2-Clause License.
