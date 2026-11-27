# 4 GB NVIDIA Laptop Testing & Baseline Guide

Tested hardware: **NVIDIA GeForce RTX 3050 Ti Laptop GPU (4 GB GDDR6, 60–75W TGP)**, Intel Core i7-11800H, 16 GB DDR4 RAM.  
Game Version: **CUSA00223 (US) Update 01.07** on renderer v2 (`gpu_v2`), **Beta Build: `sunny-beta-2026.11.12` (`7398060+sunny-beta-20261112`)**.

---

## 1. Past Beta Testing Findings (Now Fixed Upstream)

During testing of earlier `gpu_v2` betas on 4 GB NVIDIA laptops, two major bottlenecks were reproduced and reported:

1. **CPU/GPU Fence Stalls (`ISS_UNIFIED_MEMORY=1` in early beta `defaults.env`):**  
   Triggered ~28 fence synchronization stalls per frame on discrete NVIDIA cards, dropping framerate from ~50 FPS to ~15–20 FPS. *(Resolved in newer builds; `ISS_UNIFIED_MEMORY` is no longer forced on.)*
2. **Seattle Combat `vk::Result=ErrorOutOfDeviceMemory` (2.5 GB Lighting/Skinning Table):**  
   In open-world Seattle combat, huge read-only bindings (`BindBuffers` clamping 2.5 GB down to 1024 MB) exhausted 4 GB device-local VRAM and tripped the residency pool assertion.  
   Testing with a 64 MB window (`ISS_HUGE_READ_WINDOW_MB=64`) dropped peak VRAM to **3,433 MB / 4,096 MB** with `arena_spilled=0 MB` and eliminated the crash across 8+ minutes of combat. *(Superseded upstream: lighting memory usage has since been reduced to ~32 MB across all GPUs regardless of VRAM.)*

---

## 2. Current Recommended Baseline for 4 GB GPUs

For 4 GB cards (`RTX 3050 / 3050 Ti Laptop`, `GTX 1650`, etc.), keep internal resolution at **720p** and avoid positive mip bias:

```ini
# Performance & Upscaling
fps_limit=60
render_resolution=720p
vsync=0

# Texture Sharpness & Streaming
mip_lod_bias=0.000
adv_staging_keep_mb=256
```

* **`render_resolution=720p`:** Leaves enough VRAM headroom under 4 GB for open-world Seattle streaming and upscaling (`FSR 1` / `FSR 3.1`).
* **`mip_lod_bias=0.000`:** Prevents blurry road and wall textures at medium distance with negligible GPU cost on Turing/Ampere.
* **`adv_staging_keep_mb=256`:** Keeps PCIe staging transfers smooth during high-speed smoke/neon traversal.

---

## 3. Observed RTX 3050 Ti (4 GB) Benchmark Baseline (`sunny-beta-2026.11.12`)

| Scene | Beta Build | Unpatched Defaults | Tuned 4 GB Baseline (720p, Memory-Capped) | Peak VRAM |
| :--- | :--- | :--- | :--- | :--- |
| **Akomish Beach (Prologue)** | `7398060+sunny-beta-20261112` | ~25–30 FPS | **58–60 FPS** | ~1,650 MB |
| **Lantern Vista / Bridge** | `7398060+sunny-beta-20261112` | ~20–24 FPS | **52–55 FPS** | ~2,400 MB |
| **Seattle City (Combat)** | `7398060+sunny-beta-20261112` | Crash (`ErrorOutOfDeviceMemory`) | **55–60 FPS** | **3,433 MB** (`0 MB` spilled) |
