# TMNF BlockMover Studio

**BlockMover Studio** is a native C++ / Direct3D 9 DLL modification (`BlockMover.dll`) for the **TrackMania Nations Forever** track editor (`TmForever.exe`), built for **TMLoader / Modded Forever (2.13.0)** with **TMUnlimiter (2.0.2)** and **CoreMod**.

It enables real-time 3D block manipulation, camera-relative movement, smart multi-block selection, blueprint saving/loading (`.tmgroup`), and instant air-spawning directly at the editor cursor without terrain holes or camera stuttering.

---

## ✨ Features

* **Live 3D Block Moving & Rotation:** Move and rotate placed blocks in real time (individually or as a group) without deleting or rebuilding parts of your track.
* **Camera-Relative Movement:** Moving blocks forward, backward, left, or right automatically aligns with your current 3D camera perspective (can also be toggled to fixed world axes).
* **Smart Selection Tools:**
  * Single-click toggle & continuous drag-selection (including blocks placed high up in the air).
  * Quick-select tools for: Last placed block, all track blocks, blocks at the same Y-height, or blocks of the same type.
  * 3D Box Selection between two cursor corners.
* **Blueprint Suite (`.tmgroup`):**
  * Save any block formation into reusable `.tmgroup` blueprint files (`Ctrl + C` or via UI).
  * Spawn blueprints directly at the target cursor (`Ctrl + V` or `I`), both on the ground and mid-air, without creating white stadium grass holes or unwanted support pillars.
* **In-Game Dear ImGui Studio Overlay (`F3`):**
  * Clean tabbed interface with projected 3D wireframe selection boxes.
  * Live in-game language switcher (`EN <-> DE`).
  * All 22 functions and selection tools are freely customizable via hotkeys (`DEL` to unbind) so you never lose game window focus.

---

## 🚀 Installation

1. Download `BlockMover.dll` from the **Releases** page (or compile it from source as `Release | x86`).
2. Place `BlockMover.dll` into your TrackMania Nations Forever directory / load it via **TMLoader** (Modded Forever).
3. Launch the game, open a map in the **Track Editor**, and press **`F3`** to open or close the BlockMover Studio overlay.

---

## 🎮 Default Controls

| Action | Preset 1 (Default) | Preset 2 (Numpad) |
| :--- | :--- | :--- |
| **Open / Close Studio Menu** | `F3` | `F3` |
| **Enable / Pause Mod** | `F6` | `F6` |
| **Select / Drag Block** | `E` | `F` |
| **Deselect All** | `Numpad 0` / `Backspace` | `Numpad 0` / `Backspace` |
| **Move Forward / Back** | `W` / `V` | `Numpad 8` / `Numpad 2` |
| **Move Left / Right** | `A` / `D` | `Numpad 4` / `Numpad 6` |
| **Move Up / Down (+Y / -Y)** | `Page Up` / `Page Down` | `Numpad 9` / `Numpad 3` |
| **Rotate 90° CW / CCW** | `R` / `Q` | `Numpad 7` / `Numpad 1` |
| **Teleport Selection to Cursor** | `Numpad 5` | `Numpad 5` |
| **Clone Selection at Cursor** | `L` | *(Unbound)* |
| **Copy Selection** | `Ctrl + C` | `Ctrl + C` |
| **Spawn Blueprint at Cursor** | `I` or `Ctrl + V` | `Ctrl + V` |

*All 22 hotkeys can be customized or cleared with `DEL` in Tab 4 of the `F3` menu and are automatically saved in `BlockMover.ini`.*

---

## 🛠️ Building from Source

* **IDE:** Microsoft Visual Studio (C++)
* **Configuration / Platform:** `Release | Win32 (x86 / 32-Bit)`
* **Runtime Library:** Multi-threaded (`/MT`)
* **Dependencies:**
  * DirectX 9 (`d3d9.lib`)
  * Dear ImGui 1.93.0 (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_impl_dx9.cpp`, `imgui_impl_win32.cpp`) inside `./imgui/`

---

## 📄 License

This project is open-source and provided for non-commercial educational and TrackMania modding purposes.
