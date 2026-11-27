### What

### Tested
<!-- OS, GPU, what was checked (a game run, a capture replay with tools/test/replay.sh, a probe). "Not tested" is a valid answer. -->

### Checklist
<!-- Rules behind each item: https://github.com/sinedgiveaways/pmocer-latibro/blob/HEAD/CONTRIBUTING.md -->
- [ ] Based on current `main`; one topic per PR, follow-ups in a new PR
- [ ] No game code, assets or anything generated from the game (`build/` stays out; `tools/git/pre-commit` checks it)
- [ ] Recompilation, not emulation: no CPU interpreter or JIT, no GPU emulation below NVN2
- [ ] Code taken from another project keeps its header and names it with its license (GPL-3.0-compatible only), in the code and in [docs/research.md](https://github.com/sinedgiveaways/pmocer-latibro/blob/HEAD/docs/research.md#sources)
- [ ] Performance changes measured before and after (A/B inside a run, `tools/run/ab.py`)
- [ ] Every folder's README updated (`uv run tools/docs/check.py` passes)
- [ ] AI-assisted: <!-- yes / no -->
