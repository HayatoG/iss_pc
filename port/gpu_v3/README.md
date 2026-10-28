# gpu_v3 (experiment, not built yet)

Import of the graphics core that the `Liondarkz/issn_pc` port ships in `gpu/`, to compare with and port ideas into
renderer v2. Nothing here is wired into the build.

- Origin: `Liondarkz/issn_pc` @ e7a3ca8f1808bb02b90e60c6c3f919ac188ac2d7 (GPL-2.0), `gpu/shadps4`, `gpu/shim`, `gpu/patches`, `gpu/issngpu.h`, `gpu/CMakeLists.txt`.
- That core is axylpup's inFAMOUS Second Son build of shadPS4 (https://github.com/axylpup/shadPS4, GPL-2.0-or-later, tag
  second-son-v1.1.1 = 57c9f746bbc3b91966b216435a14447c13a89da3), plus issn_pc's port changes. The detail is in
  `VENDOR_ISSN.txt` (their own vendoring notes).
- Not copied: `third_party` (this repository has its own), `dlss_bridge`, `tools`, and the FSR 4 weights (provenance unverified).
- Credit: keep every SPDX header; commits that touch these files cite the origin commit. See `docs/RENDERER_V3_AXYLPUP.md`.
