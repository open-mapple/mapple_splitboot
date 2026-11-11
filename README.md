<!-- SPDX-License-Identifier: GPL-3.0-only -->
# mapple_splitboot

The optional dual boot, Android and iOS on one phone: carving a `darwin` partition out of `userdata`
(`tools/gpt-carve.py`), the data volume in it (`tools/mkdatavol.py`, `tools/var-store-read.py`) and chaining
mtkloader in front of Android's kernel (`tools/chainboot.py`). Not in the default checkout: add it with
`repo init ... -g default,splitboot`.

Checked out at `mapple/splitboot` by the [Open Mapple manifest](https://github.com/open-mapple/manifest), which says how to get, build and install it.

Licence: GPL-3.0-only (`LICENSE`). The files marked MIT come from HakiOS (`NOTICE`, `LICENSES/MIT`).
