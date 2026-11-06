# Launcher icon

`launcher.png` is the approved launcher artwork. Two large pastel eighth notes float over a twin-arch stone bridge viewed diagonally from above, on a transparent background. The artwork was generated with ChatGPT's built-in image generation tool.

`launcher.ico` contains PNG-compressed images at 16, 24, 32, 48, 64, 128 and 256 pixels, preserving alpha transparency. It is embedded in the launcher through `launcher.rc`.

To regenerate on Windows (PowerShell and System.Drawing; no Python dependency):

```powershell
.\tools\make-icon.ps1
```

The committed ICO is used by normal CMake builds, so icon conversion is not needed to build the EXE. The artwork and conversion script are included under the project's BSD 2-Clause License.

## Previous icons

The previous side-view stone bridge is preserved as [launcher-side-view.png](launcher-side-view.png) and [launcher-side-view.ico](launcher-side-view.ico).

The original box-and-notes artwork is preserved unchanged as [launcher-box.png](launcher-box.png), with its original [launcher-box.ico](launcher-box.ico). These are reference assets and are not included in the distribution ZIP. Non-square replacement artwork is fitted into square ICO canvases with transparent padding, preserving its aspect ratio.
