# tools/migrate

| File | What it is |
|---|---|
| `mkw-to-orbitals.tsv` | The exceptions to the rename rule `MKW_X` to `ORBITALS_X` that `tools/migrate_config.py` applies once to a user's settings, profiles and Windows environment. One-shot migration data, never loaded by the runtime; empty at present |

`tools/migrate_config.py` (dry run by default, `--apply` to write, a `.bak` of every file it changes) is described in
its own header and in [docs/environment-variables.md](../../docs/environment-variables.md).
