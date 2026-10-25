# Direct mouse aiming candidate

Local Windows Renderer v2 test, 2026-10-23. Opt in with
`ISS_DIRECT_MOUSE_AIM=1`; default is unchanged mouse-to-stick input.
The corrected candidate was installed normally after user gameplay confirmation.

Reference: axylpup/shadPS4 tag `second-son-v1.1`,
`src/core/mouse_aim.cpp` and `.h` (GPL-2.0-or-later). The useful mechanism is
adding an atomic mailbox's angular deltas at the final camera-angle store,
after stick processing. Emulator input/timer infrastructure is not reused.

The local CUSA00223 01.07 ELF independently matches the 27-byte store signature
at RVA 0x14ebc5, data load at 0x14dfb2, context spill at 0x14ea75, pitch-limit
loads at 0x14e38f and no-follow-byte reference at 0x14936e. Matching offsets in
this case are evidence from the US file, not an assumption of EU compatibility.
The loader also requires US title/version and executable/writable coverage.
Any mismatch retains the existing mouse-to-stick path without patching it.

The callback preserves game registers through the existing bridge (new argument
mode 4), edits only the saved angle pair, then replays both original stores.
It combines data/context pitch bounds, preserves an already-outside game tilt,
and rejects invalid bounds. Other controller state and angular-speed hooks are
untouched. A packed atomic mailbox is consumed once per camera update; inactive
capture, sensor/touchpad modes and a camera gap over 250 ms discard pending
motion. The existing no-follow byte is temporarily held during mouse use and
its prior value restored after inactivity. Sensitivity 1 = 5.12 degrees per
100 relative mouse counts, matching the reference's default gain. Existing
Controls sensitivity and Invert Y apply; stick smoothing/acceleration/dead-zone
controls are disabled in the overlay while the direct hook is installed.
The reference's separate hotkeys, sensitivity file and unverified game-options
inversion offsets are not imported.

Validation: all ten CTest checks pass, including the new math/mailbox tests and
an executed native bridge fixture checking context/data arguments, angle edits,
original store replay and XMM1 preservation. The local ELF guards pass before
test-profile preparation. A fresh copied profile protects 487 normal files;
test exit checks their hashes again. Runtime confirms the hook installed with
the HUD, camera speed, diagonal safeguard and Karma hooks.

Still requires user gameplay validation: yaw/pitch direction, small and fast
motion, hipfire and aiming, vertical limits, menu/focus resume without jumps,
controller handoff, and stability. No claim of cross-hardware or EU support.
During candidate testing the normal installation was left unchanged.

First gameplay test failed: input and the hook were active, but the initial
per-range +/-3.2 guard rejected the context's legitimate [-10,10] range.
Diagnostic trace captured data bounds [-1.45,1.525], context [-10,10]. The
corrected candidate validates the finite, ordered ranges and their effective
intersection instead. An exact regression case passes; all ten tests pass.
User confirmed the corrected candidate "works great" after the normal/aiming
test request. Runtime trace confirms applied=1 for both positive and negative
yaw/pitch deltas with the captured US limits. Longer gameplay, menu/focus
handoff and controller regression checks were requested; user subsequently
reported "works good". Broad hardware validation remains pending.

Installed at explicit user request, 2026-10-23: executable SHA256
e401be506b7e23ed7d10ee72c11e9e92aca45960305583420f11cbc4acf07da7.
The installer verified 486 protected progression/mod files unchanged, replaced
zero saves or tracking records, and backed up the previous binary, defaults,
configuration, saves and Karma data. Persistent defaults enable direct aiming,
HUD corners and the existing texture-hash/pipeline-cache workarounds. A
shareable update ZIP includes the same executable, installer and changed source;
no personal profile data is included. The installer also passed a separate
fixture check preserving saves, ledger contents and unrelated settings.
