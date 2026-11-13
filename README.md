<div align="center">

# 🏎️ TMNF BlockMover Studio

**Real-Time 3D Track Editor & Blueprint Suite for TrackMania Nations Forever**

[![Release](https://img.shields.io/badge/Release-v1.0.0-00e5ff?style=for-the-badge)](../../releases)
[![Platform](https://img.shields.io/badge/Platform-Win32%20(x86)-0078d7?style=for-the-badge&logo=windows)](#)
[![DirectX](https://img.shields.io/badge/DirectX-9.0c-107c10?style=for-the-badge)](#)
[![Loader](https://img.shields.io/badge/TMLoader-Modded%20Forever%202.13-ff9800?style=for-the-badge)](#)
[![Compatibility](https://img.shields.io/badge/TMUnlimiter-2.0.2-9c27b0?style=for-the-badge)](#)

</div>

---

## 📖 Overview

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

## 🚀 Installation Guide (TMLoader & BlockMover)

### Step 1: Install TMLoader (if not already installed)
1. Visit the official **TMLoader** website: [**https://tomashu.dev/software/tmloader/**](https://tomashu.dev/software/tmloader/)
2. Download the latest **TMLoader** release and extract it to a folder of your choice (or into your TrackMania Nations Forever installation directory).
3. Run **`TMLoader.exe`** once so it initializes its local database folders and downloads the base mods (**Modded Forever / CoreMod** and **TMUnlimiter**).

### Step 2: Install BlockMover Studio
1. Download **`BlockMover.zip`** from the [**Releases**](../../releases) page of this repository.
2. Press **`Win + R`** on your keyboard, paste the following path, and hit **Enter**:
   ```text
   %localappdata%\TMLoader\database\TmForever\products
   ```
3. Extract **`BlockMover.zip`** into this folder so that the `BlockMover` directory sits directly inside `products`:
   ```text
   ...\TMLoader\database\TmForever\products\BlockMover\1.0.0\BlockMover.dll
   ...\TMLoader\database\TmForever\products\BlockMover\1.0.0\description.yaml
   ```

### Step 3: Activate Mods in TMLoader (`◆` Diamond Icons)
1. Launch **`TMLoader.exe`** and select or edit your game profile.
2. In the mod list, make sure you enable **all three** of the following mods by clicking the **diamond icon** next to them:
   * **`◆` CoreMod** *(Modded Forever)*
   * **`◆` TMUnlimiter** *(2.0.2)*
   * **`◆` BlockMover** *(1.0.0)*
   > 💡 **Important:** A mod is only active when its diamond icon is **filled (`◆`)**. If the diamond is **hollow (`◇`)**, the mod is disabled—click the diamond once so it becomes filled!
3. Launch the game via **TMLoader**, open any map in the **Track Editor**, and press **`F3`** to open or close the BlockMover Studio overlay.

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

## 📸 Screenshots & Preview

| Live 3D Selection & Controls (`Tab 1`) | Blueprint Air-Spawner (`Tab 2`) |
| :---: | :---: |
| ![Studio UI Preview](https://github.com/user-attachments/assets/39a0da8d-5412-4835-896b-ccfa725a742b) | ![Blueprints Preview](https://github.com/user-attachments/assets/60be524b-4476-49c6-a387-d7429b438bb0) |

---

### 2. Live 3D Selection & Controls (`Tab 1`)

https://github.com/user-attachments/assets/eb93dc89-faf7-4a84-9729-8b45b5ba6e3a

### Blueprint Air-Spawner (`Tab 2`)

https://github.com/user-attachments/assets/fd67257d-3ca7-4d5c-8d81-f0f615851fc8

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
