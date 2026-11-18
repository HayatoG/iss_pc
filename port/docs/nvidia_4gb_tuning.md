# NVIDIA 4 GB GPU & Laptop Optimization Guide

Tested hardware: **NVIDIA GeForce RTX 3050 Ti Laptop GPU (4 GB GDDR6, 60–75W TGP)**, Intel Core i7-11800H, 16 GB DDR4 RAM.
Game Version: **CUSA00223 (US) Update 01.07** on renderer v2 (`gpu_v2`).

---

## 1. Summary of Findings

On 4 GB NVIDIA GPUs (RTX 3050 Laptop, RTX 3050 Ti Laptop, GTX 1650, etc.), default configurations can encounter two major bottlenecks:
1. **CPU Fence Stalls (`ISS_UNIFIED_MEMORY=1`):** Causes ~28 CPU/GPU fence synchronization stalls per frame, cutting framerate from ~50 FPS down to ~15–20 FPS.
2. **City VRAM Saturation & Texture Garbage Collector Thrashing:** In open-world Seattle, default VRAM budgets (>3,500 MB) exceed available device-local memory (after Windows DWM allocation), triggering high-frequency texture evictions (`TEXTURE_GC trigger=358 MB`).
3. **Distance Texture Blurring (`mip_lod_bias > 0`):** Positive mip bias creates noticeable texture blur on road surfaces and building walls until the player is within close proximity.

---

## 2. Recommended Settings for 4 GB GPUs

In `user/tlg.ini`:

```ini
# Performance & Framerate
fps_limit=60
render_resolution=720p
fsr1=1
rcas=1
rcas_strength=60
vsync=0

# Geometry & Draw Distance
lod_scale=0.750
region_lod=0.000
region_fade=0.300
near_clip=10.000
far_clip=250000
lod_bands=400,750,1500,3500,6000,15000

# Texture Sharpness & Streaming Buffer
mip_lod_bias=0.000
adv_staging_keep_mb=256
adv_prep_workers=8
adv_copy_threads=4
```

In `gpu_v2/defaults.env`:

```env
ISS_UNIFIED_MEMORY=0
```

---

## 3. Benchmarked Metrics

| Scene | With `ISS_UNIFIED_MEMORY=1` | With `ISS_UNIFIED_MEMORY=0` & 4GB Tuning | VRAM Footprint | Stability |
| :--- | :--- | :--- | :--- | :--- |
| **Akomish Beach (Prologue)** | ~25–30 FPS | **58–60 FPS** | ~1,650 MB | 0 timeouts, 0 spills |
| **Lantern Vista / Bridge** | ~20–24 FPS | **52–55 FPS** | ~2,400 MB | 0 timeouts, 0 spills |
| **Seattle City (Open World)**| ~18–22 FPS | **48–54 FPS** | ~3,050 MB | VRAM headroom maintained |

---

## 4. Key Takeaways

* **`ISS_UNIFIED_MEMORY=0` is critical for NVIDIA GPUs** to allow the command processor to record and submit without per-draw fence waits.
* **Keep `mip_lod_bias=0.000`**; sampling hardware on modern Turing/Ampere GPUs processes native mip levels with virtually zero performance difference compared to biased mips, completely avoiding delayed road/wall pop-in.
* **Staging Buffer (`adv_staging_keep_mb=256`)** provides sufficient PCIe transfer bandwidth to keep up with Delsin's high-speed smoke/neon traversal in the open world.
