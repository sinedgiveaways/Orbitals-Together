# MKW removal plan (Étape 1)

Draft for validation, 16/11/2026. Nothing in this plan is applied yet. Branch `cleanup/mkw-removal`; merge to `main`
by pull request only, after Étape 2 is done and checked.

## Decisions already taken

- Project and executable: `orbitals-recomp` / `orbitals-recomp.exe`. The game: Orbitals. Settings file: `orbitals.ini`.
  Variable prefix: `ORBITALS_`. The singular "orbital" goes away except in third-party terms.
- `LICENSE`, `recomp/LICENSE.totk-pc-recomp`, `runtime/LICENSE.totk-pc-recomp` and the copyright lines in file headers
  (for example `Adapted from LAS Recomp ... Copyright (c) 2026 Peiikai` in `recomp/gen.py`, `lifter.py`, `module.py`) stay.
  The name `mkw-recomp` leaves the product only.
- The runtime reads only `ORBITALS_*`: no alias, no fallback, no `#ifdef`. A one-shot script migrates old settings.
- `i18n/game/*.tsv` are removed in their own commit.

## Findings that shape the order

1. **The runtime already migrates `mkw.ini` itself.** `runtime/core/base/cfg.c` (around line 646) renames `mkw.ini` to
   `orbital.ini` at start. That puts the string `mkw.ini` in the binary and contradicts "no fallback". The code moves
   to `tools/migrate_config.py` and is removed from `cfg.c`.
2. **`cfg.c:21` `PREFIX "MKW_"` is the single read point**, and `rt_knob` (`runtime/core/diag/perf.c`) adds the prefix for
   55 more names. Both change together with the ~2000 literals, or reads silently stop working. The change is
   mechanical but has to land as one commit for `runtime/`, `tools/` and `tests/`, otherwise scripts export names the
   runtime no longer reads.
3. **Game-specific code, not names.** `runtime/core/boot/version.c` has one row per MKW build. Its fields are MKW
   offsets (`nvn_ret`, `frame_update`, `game_clock`, `zstd_*`, `rb_*`, `random_ctor*`, `camera_service`, ...). The header
   says a field at 0 means "unknown: the feature is off, with a log line, never a guess". `patches.c`, `game.c`,
   `seqwatch.c` and `ov_zstd.c` use those fields. So an Orbitals row can be all zero and honest.
4. **The update checker points at `adriadam10/mkw-recomp`** (`launcher.cpp:281`, `tools/prepare/update.py`,
   `tools/github/*.py`, `tools/docs/i18n_readme.py`, `release.py`). Renaming only the text would leave the launcher asking
   another project's releases for updates. I need the real repository, see question 1.
5. **`.po` catalogues**: "Mario Kart World" is in the `msgid` of two strings. Changing a `msgid` loses the translations
   unless every `msgstr` is changed in the same commit. A script does both.
6. Existing attributions to totk-pc-recomp, LAS Recomp and others that appear in `docs/roadmap.md` and source comments
   are third-party credits, not MKW traces. They stay.

## Ordered commits

Each commit leaves `uv run pytest tests`, `uv run tools/build/build.py --runtime-only` and the new scan green on the
files it touches. Commits 1-3 change no behavior.

| # | Commit | Content | Files (approx.) | Risk |
|---|---|---|---|---|
| 1 | `chore: add name scanner` | `tools/build/check_names.py`: scans a folder or an exe for `MKW`, `MarioKart`, `Mario Kart`, `mkw-recomp`, as UTF-8 and UTF-16LE, and reads the PE version resource. An allowlist file (`tools/build/check_names.allow`) names what may keep the strings: `LICENSE*`, `THIRD_PARTY_NOTICES`, `docs/mkw-removal*.md`, `tools/migrate/`. Report-only at first; fatal once the renames are done | 2 new | Low |
| 2 | `chore: remove MKW i18n game tables` | `git rm i18n/game/{ca,eu,gl}.tsv i18n/game/1.8.0/*`, update `i18n/game/README.md`, `tools/i18n/game_texts.py` (no tables: the step is skipped, no error), any test that reads them. No new `orbitals-*.tsv` for now (they can be empty, but an empty file nothing reads is clutter; I'll add them when the first Orbitals text is ready) | ~8 | Low; Catalan, Basque and Galician game text is lost for now |
| 3 | `feat(migrate): one-shot MKW to ORBITALS migration` | `tools/migrate_config.py` and `tools/migrate/mkw-to-orbitals.tsv` (header: one-shot migration, never loaded by the runtime); `docs/environment-variables.md` with the mapping table. See "Migration script" below | 3 new | Medium (touches user files) |
| 4 | `refactor: version table for Orbitals` | `recomp/versions.py`: the two MKW rows become one Orbitals row (below); `romfs_products` keyed to MKW's `ResourceSizeTable.Product.*` becomes optional (Orbitals' RomFS layout is unknown to me: the check is skipped when the file is absent); `tests/test_setup.py` (uses `VERSIONS[0]` and `[1]`); `port_check.py:335` label; delete `recomp/patches-mkw*.txt` (setup finds spin loops itself when `patches` is `None`) | ~6 | Medium |
| 5 | `refactor(runtime): Orbitals version row, MKW-only offsets out` | `version.c/.h`: MKW rows removed, one Orbitals row with unknown fields at 0 (`TODO` list below); `seqwatch.c` stops keying on `number == 100`; the MKW-only probes `ov_mkw_probe.c`/`ov_mkw_list.h` removed, since they test MKW's SDK shape; `loader.c` comparisons against "MKW 1.0.0's" import tables become a port-mode print only | ~15 | High: the runtime can't be run here, only compiled |
| 6 | `refactor: ORBITALS_ prefix` | `cfg.c` `PREFIX`, `rt_knob`, all `cfg_get/cfg_value/cfg_int/getenv` literals, `tools/run/envs.py` and its regex, `tools/run/*.sh`, `tests/`, `.github/`; the `mkw.ini` self-migration in `cfg.c` is deleted | ~300 | High but mechanical |
| 7 | `refactor: orbitals-recomp artifact names` | `build/mkw` to `build/orbitals-recomp`, `libmkw.dll.a`, `mkw.def`, `mkw.rc`/`.manifest`/`.ico`, linker sections `mkw_ovr/obs/fns`, `ov_mkw_*`, `mkw_vpool.c`, `probe_*_mkw`, log `logs/orbitals-recomp.log`, `orbital.ini` to `orbitals.ini`, Linux data directory, AppImage/Wine markers, `mkw-prof`, `mkw-neo`; `build.py`, `dist.py`, `release.py`, `windeps.py` | ~120 | High (build) |
| 8 | `feat(win): version resource and icon` | Add a `VERSIONINFO` block to `orbitals-recomp.rc` (`ProductName`, `FileDescription`, `OriginalFilename`, `CompanyName`, `LegalCopyright` that keeps the MIT holders), neutral generated icon in `icon.py` (not a racing flag) | 3 | Low |
| 9 | `refactor(i18n): game name in catalogues` | `i18n/mkw.pot` to `orbitals.pot`; "Mario Kart World" to "Orbitals" in every `msgid` and `msgstr` by script, then `msgfmt --check`; `tools/i18n` | 24 | Medium |
| 10 | `docs: Orbitals names in user documentation` | `README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `docs/*.md`, issue templates, `release-template.md`, 6 translated READMEs (`tools/docs/i18n_readme.py` regenerates them), singular "orbital" to `orbitals-recomp` / Orbitals | ~40 | Low |
| 11 | `docs: changelog reset` | See question 2 | 1 | Low |
| 12 | `docs: MKW removal report` | `docs/mkw-removal.md` with the two categories, the mapping, and the scanner output on a built exe. Scanner switched to fatal | 2 | Low |

Estimated effort for Étape 2: 12 commits, about 480 files and 3500 lines, nearly all by scripted replacement with a
diff shown after each commit, as you asked.

### Orbitals row for `versions.py`

```python
Version(
    name="327680",   # update; the display version is unknown to me, see question 3
    number=1,
    main_build_id="d6efcc0a51c1c792",
    romfs_files=0,   # not counted yet: no check
    patches=None,    # setup finds the spin loops
    program_ids=("040032B023CA4000", "040032B023CA4800", "040032B023CA5001"),   # base, update, add-on
    base_version=65536, update_version=327680,
    module_ids={"main": "d6efcc0a51c1c792", "rtld": "f9060b30c6fa2b69",
                "sdk": "daa31832ca979cdf", "subsdk0": "caf9437bacb2aa16"},
)
```

The dataclass has no program-id or per-module fields today, so they are added (they feed `port_check.py` and the
module check in `boot.c`). The module sizes (157.8, 0.1, 16.4, 0.8 MiB) are rounded, so I'd use them in a test as a
sanity range, not as exact values. The 16-hex-digit ids are the first 8 bytes of each NSO build id, the form
`versions.py` already uses. The `unknown()` path (number 0, "another game") stays for future games.

### Boot constants to find (TODO)

Not available to me; each stays 0 in the row, so the feature is off with a log line:

- `nvn_ret`, `frame_update`, `frame_slot`, `game_clock`, `zstd_multiframe`, `zstd_stream`, `rb_create`,
  `rb_alloc_init`, `rb_pool_new`, `rb_pool_init`, `expheap_create`, `random_ctor_lr`, `random_ctor2_lr`, `ui_text_lr`,
  `ui_focus_fn`, `camera_service`.
- `sdk_name_pool`, `sdk_name_offsets`, `sdk_pointers`, `sdk_last_index`: `tools/gpu/nvn2_procs.py` locates them from the
  `sdk` module. Needed for NVN2.
- `inits` (DT_INIT_ARRAYSZ per module): `--probe-loader` on a run.
- Heap sizes, load addresses and thread priorities are not in `version.c`: they come from the loader and the kernel
  layer (`loader.c`, `runtime/cpu`, `runtime/sdk`). I'd record what Orbitals requests from a log run
  (`ORBITALS_TRACE` set) and list them in `docs/audit-current-state.md`, rather than guess.

Each is filled from a run with logs on your machine; I will list the exact command in Étape 2 commit 5.

### Migration script (`tools/migrate_config.py`)

One-shot, after the user's consent, never part of the runtime:

- Default is `--dry-run`: it prints every change and writes nothing. `--apply` is required to change files.
- Before any change it writes a `.bak` copy of each file it edits.
- It converts `mkw.ini` to `orbitals.ini` (keys are unprefixed, so only the file name and the `MKW_` names inside
  comments change), moves the log and data folders only with `--move-data`, and rewrites `MKW_*` in: `~/.bashrc`,
  `~/.profile`, `~/.zshrc`, `~/.config/environment.d/*.conf`, the PowerShell `$PROFILE`, and the user-level Windows
  environment (`HKCU\Environment`, through `[Environment]::SetEnvironmentVariable`).
- The final message says: migration one-shot, old variables removed after migration.
- The mapping is `tools/migrate/mkw-to-orbitals.tsv` (old name, new name). Where the old name is only a prefix change
  the script applies the rule, and the table lists the renamed ones (`MKW_VK_PRESENT` and similar) for reference.

### Verification after each commit

- Python tests, `uv run tools/build/build.py --runtime-only` and the C probes that compile on Linux. A clean build of
  `build/dist/windows` can only be done on your Windows machine; I'll give you the exact command and the scanner
  output to paste back.
- `tools/build/check_names.py build/dist/<target>` as the evidence for `docs/mkw-removal.md`. `Select-String` on a
  binary reads text only; the scanner also checks UTF-16 and the version resource.
- A check for buffers sized for the old names: `ORBITALS_` is 5 characters longer than `MKW_`. I'll grep `sizeof` and
  fixed `char key[N]` uses near the renamed calls and build with `-Wformat-truncation`.

## What I need from you

1. **Repository identity.** The launcher's update check, bug-report button and `tools/github/*` use
   `adriadam10/mkw-recomp`. Which `owner/name` should they use: `sinedgiveaways/pmocer-latibro`? And is that the final
   public name (the project is `orbitals-recomp`)?
2. **`CHANGELOG.md` (0.1.0 to 0.1.12).** It is entirely MKW release history. Options: (a) delete and start at 0.2.0,
   history stays in git; (b) keep it in `docs/legacy/` marked as the MKW-era changelog, which keeps "Mario Kart World"
   in the repository, outside the build; (c) rewrite entries by replacing names, which misstates history. I recommend
   (a). `tools/build/notes.py` reads this file, so it must have a valid first section.
3. **Display version of the Orbitals update** (what the game shows, for `Version.name` and the menu). I did not
   convert 327680 to a name; if unknown, the name stays `327680`.
4. **Icon.** `icon.py` draws a chequered flag. Is a plain generated placeholder acceptable for now, or do you have an
   Orbitals-compatible icon of your own?
5. **MKW-only probes and patches (commit 5).** I'd delete `ov_mkw_probe.c`, `ov_mkw_list.h`, `runtime/probes/ov_probe.c`'s
   MKW tables, and the MKW-specific patch sets (`set shadow`, `set camera`, `set seed`, `set aspect`, `zstd`). Option B
   keeps them compiled but off while the offsets are 0. A keeps the repository free of MKW logic (cleaner, they are
   recoverable from git when Orbitals needs similar ones); B keeps working reference code. I recommend A. Your call.
6. **Audit inventory file.** `docs/audit-mkw-inventory.tsv` is not in your allowed `.tsv` cases and contains MKW names, so I left it
   untracked. Delete it, or add it under `docs/` as an audit file outside the runtime?
