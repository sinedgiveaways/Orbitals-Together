# Orbitals Together

**English** · [Français](README.fr.md)

**Orbitals Deluxe Edition on PC, for two players: side by side, or remotely with Steam Remote Play Together.**

Orbitals Together is a static recompilation of Orbitals Deluxe Edition (Nintendo Switch 2) for Windows: the game's
code is recompiled for the PC from **your own copy of the game**, and it renders through Vulkan. There is no emulator
and no game file in this repository.

- Current version: **0.3.0**
- Supported game: **Orbitals Deluxe Edition 1.0.5**
- System: Windows 10 or 11, 64-bit

This repository holds the builds considered final and the full source code of each version: anyone can fork it to
make it better (see part 7). Day-to-day development and test builds happen on
[sinedgiveaways/pmocer-latibro](https://github.com/sinedgiveaways/pmocer-latibro).

---

## Contents

1. [What you need](#1-what-you-need)
2. [Download the build](#2-download-the-build)
3. [Install and recompile the game](#3-install-and-recompile-the-game)
4. [Play two players locally](#4-play-two-players-locally)
5. [Play remotely with Steam Remote Play Together](#5-play-remotely-with-steam-remote-play-together)
6. [If something goes wrong](#6-if-something-goes-wrong)
7. [Fork and improve the project](#7-fork-and-improve-the-project)

---

## 1. What you need

- A PC running **Windows 10 or 11 (64-bit)**.
- A **Vulkan 1.3 graphics card** (recent NVIDIA, AMD or Intel) with up-to-date drivers.
- About **30 GB of free space**.
- **Your own copy of Orbitals Deluxe Edition, version 1.0.5**, dumped from your Switch 2:
  - either a **`.nup`** file;
  - or the **`exefs`** and **`romfs`** folders.

  The game is not included and never will be: you must own it and dump it yourself.
- An internet connection during the setup (NVIDIA tools, about 130 MB, are downloaded to translate the game's
  shaders).
- To play remotely: **Steam** installed and signed in (see part 5).

## 2. Download the build

1. Open the releases page: **[Releases](https://github.com/sinedgiveaways/Orbitals-Together/releases/latest)**.
2. Download **`orbitals-together-0.3.0-windows-x64.zip`**.
3. Unzip it, for example into `C:\Users\<you>\Downloads\`. The zip creates an **`orbitals-together`** folder, which
   becomes the game folder. Avoid `C:\Program Files`: the program writes its files into its own folder.

The folder holds `orbitals-together.exe`, its libraries, `steam_api64.dll` and a `setup\` folder (the tools that
recompile the game). Do not move anything inside it.

## 3. Install and recompile the game

The recompilation happens **once**, on your PC, from the launcher.

1. Run **`orbitals-together.exe`**. The launcher opens.
2. Choose **Game files**.
3. Under **Your copy**, tell it where your copy of the game is:
   - **Choose .nup or .nx file** for a `.nup` file,
   - or **Choose folder** for the folder that holds `exefs` and `romfs`.
4. For **RomFS (23 GB)**, choose **Copy into the game folder** (recommended) or **Use it where it is** (no copy, but
   your copy of the game must stay where it is).
5. Click **Set up**. The launcher shows each step:
   1. **check**: the game must be Orbitals Deluxe Edition 1.0.5;
   2. **copy** of `exefs`, then `romfs` (about 23 GB);
   3. **generate** the game's code as C;
   4. **compile** that code for your PC (the longest step);
   5. **translate the shaders** for your graphics card;
   6. **warm-up**: the game runs on its own for a minute to prepare the graphics pipelines;
   7. **cleanup**.

   Expect 20 minutes to over an hour depending on your processor. If you stop halfway, the setup resumes from the
   last finished step.
6. When it is done, the launcher shows **"Game files ready (Orbitals Deluxe Edition 1.0.5)"** at the lower left.
7. Go back to the start screen and click **Play**.

In game, **F1** on the keyboard or **Start + Select** on a controller opens the settings menu (screen, graphics,
sound, controls, language).

## 4. Play two players locally

1. Plug in **two controllers** (Xbox, PlayStation or any controller Windows recognizes).
2. Start the game and choose **Local play**. Each controller is one player.

The game is for 2 players at most.

## 5. Play remotely with Steam Remote Play Together

Steam streams the game's picture and sound to your friend, and their controller (or keyboard) becomes player 2.
**Your friend does not need the game**, only Steam (or the Steam Link app, or a browser with a guest link).

The game goes through a free Steam game that supports Remote Play Together: **RetroArch** (App ID `1118310`). Steam
thinks it starts RetroArch, but Orbitals Together starts instead. Your friends therefore see "RetroArch" in Steam.

### Once: the setup

1. In Steam, install **RetroArch** (free) from the store.
2. In your Steam library: right-click **RetroArch** > **Properties** > **General** > **Launch options**, then type the
   path of `orbitals-together.exe` in quotes, followed by `%command%`:

   ```
   "C:\Users\<you>\Downloads\orbitals-together\orbitals-together.exe" %command%
   ```

   Replace the path with your own folder's.
3. In the game folder, open (or create) **`orbitals.ini`** with Notepad and add:

   ```ini
   [steam]
   steam_appid = 1118310
   ```

   The same setting is in the menu: **Settings > Game > Steam Remote Play Together > Steam App ID**.
4. Check that **`steam_api64.dll`** is next to `orbitals-together.exe` (it is in the zip).

### Each time you play

1. Start the game, from its folder (`orbitals-together.exe`) or with RetroArch's **Play** button in Steam. Steam
   must be running, and RetroArch's launch options (step 2 of the setup) must be in place: without them, your friend's
   controller does not reach the game.
2. In the game's menu, choose **Online play**. Steam's invite list opens: **invite your friend**. (You can also use
   the F1 menu > Game > **Invite friends**, or **Copy guest link** for a friend without Steam.)
3. Once your friend has joined and pressed a button on their controller, start a **Local play** game for 2 players:
   your friend is player 2.

**One controller plugged in on your side.** The game is limited to 2 players: with two controllers plugged into your
PC, there is no room left for your friend. Unplug the second one before inviting them.

**With a single controller in Local play**, the game also offers a choice between "Local players" and "Steam Remote
Play Together (invite friends)".

## 6. If something goes wrong

Open an [issue](https://github.com/sinedgiveaways/pmocer-latibro/issues) and attach the logs from the game folder:

- `logs\orbitals.log` (the game and the launcher);
- `logs\setup.log` (the setup).

Say what you were doing, and add a screenshot or a video if you can.

## 7. Fork and improve the project

Anyone can **fork** this repository (the **Fork** button at the top of the page), change the code and publish their
own version, under the terms of the GPL-3.0.

- **The source code** of the published version is in this repository: `runtime/` (the runtime that replaces the
  console), `recomp/` (the recompilation of the AArch64 code to C), `sm86/` (the shader translation to SPIR-V),
  `tools/` (build, setup, diagnostics), `i18n/` (the menu's translations), `docs/` (documentation and roadmap).
- **Build the Windows package in your fork**: **Actions** tab > **Build Windows** > **Run workflow**. The workflow
  compiles `orbitals-together.exe` (cross-compiled from Linux with llvm-mingw) and gives the full package as an
  artifact. Locally, on Linux: `uv run tools/build/windeps.py`, then
  `uv run tools/build/build.py --target windows --dist --runtime-only`, `uv run tools/build/dist.py --target windows`
  and `uv run tools/build/release.py --target windows --version 0.3.0`.
- **Before changing anything**, read [AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md): no emulation (no
  CPU interpreter, no GPU emulation), **no game file in git**, GPL-3.0-compatible licenses only, the console's
  behaviour by default. [DEVELOPMENT.md](DEVELOPMENT.md) describes how the code is organized and
  [docs/roadmap.md](docs/roadmap.md) what is left to do.
- **Your fork must stay under the GPL-3.0**: keep the [LICENSE](LICENSE) file and the copyright notices, and publish
  the source code of what you distribute.
- **Suggest an improvement here**: open a pull request from your fork. Accepted improvements are tested on the
  development repository before they ship in a final version.

---

## License

Orbitals Together is free software under the **GNU GPL version 3 or later** (see [LICENSE](LICENSE)). The source
code of each version is in this repository, at the tag of the same name (for example `v0.3.0`), and on
[sinedgiveaways/pmocer-latibro](https://github.com/sinedgiveaways/pmocer-latibro/tree/v0.3.0).

Orbitals belongs to its rights holders. This project is not affiliated with or endorsed by them, nor by Nintendo,
Valve or NVIDIA. No game data is distributed: you must own the game.
