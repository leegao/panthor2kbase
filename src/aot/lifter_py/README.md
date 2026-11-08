# ARM64 -> C lifter

Copied from [LASRecomp](https://github.com/peiikai/LASRecomp) `tools/` at commit `da35b83a9611d046a9a681aae668d7085954d37f`
(MIT, `LICENSE-LASRecomp`): `gen.py`, `lifter.py`, `simd_c.py`, `analyze.py`, `nso.py`.

TOTK changes:
- `nso.py`: DT_RELR packed relative relocations and DT_GNU_HASH symbol counts (TOTK 1.4.3 modules).
- `gen.py`: Link's Awakening randomizer variants removed; `patches.py` is TOTK's own (empty for now).
