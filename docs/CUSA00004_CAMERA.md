# CUSA00004 camera compatibility

The camera mod accepts the verified 1.07 executable in the European CUSA00004 dump as well as CUSA00223. The tested European merged dump retains APP_VER 01.00 despite containing the exact same unwrapped ELF as the verified US 1.07 build (SHA-256 b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605).

For that old European metadata, the launcher offers the camera mod only when the entire unwrapped ELF matches this known hash. It does not rewrite param.sfo or reinterpret VERSION as APP_VER. Ordinary XML mods retain their original version and title filtering.

Runtime title/version, executable-segment and instruction-signature checks remain mandatory for both camera hooks. An unexpected signature still disables the hook. The option remains off by default and uses the existing 1.5x camera/diagonal slowdown implementation.

Windows tests executed both generated hooks with the verified European executable, including wrong-signature and truncated-image rejection. This verifies code compatibility, not in-game camera feel; gameplay confirmation and Linux validation remain required. No game bytes or player files are included.
