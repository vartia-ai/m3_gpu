# Apple G15 GPU (M3) interface notes

This describes how the Apple G15 GPU in the M3 (T8122, G15G, firmware
V14_7) is driven. The notes come from a working Linux port of the Asahi AGX
kernel driver and Mesa to a MacBook Air M3 (J613). They are written for
developers who already know the M1/M2 (G13/G14) driver, so they cover mainly
what is different on G15.

## Status of the port this is based on

| Area | State |
|---|---|
| Firmware boot, channels, events | Working |
| Compute (VM_BIND buffers, Mesa-shaped submits) | Working |
| MMU-fault and firmware-timeout recovery | Working, without a reboot |
| Render: Mesa VS/FS, uniforms, textures (fragment and vertex), clears, reload/store programs, depth (D32F, D16), stencil, depth-only passes, mip-level targets | Working: linear and GPU-tiled RGBA8 and BGRA8 targets up to 8192x8192, GPU-tiled R8; instanced draws, flat varyings |
| Render setup | Derived from the command plus constants documented per field (see [Render jobs](30-render.md#where-render-state-comes-from)); no captured bundle at render time |
| Per-tile list limit (G2) | Fixed: the cause was bit 0 of the Fragment ISP control word `0x15021`, which the kernel now clears in every render. Renders have no primitive or draw cap. See [Known limits](70-known-limits.md) |
| Partial renders | Working with Mesa's reload/store programs: a 262,144-triangle render runs 4 partial renders (as macOS does) with exact colour and depth |
| Desktop GL | All 306 tests of the 306-test list pass; scissor, blending, logic ops, clip planes, discard, non-perspective varyings; glmark2 full run completes (score 443); its terrain scene's compute stage is refused (G15 graphics-mode compute is not supported), as expected |
| Long-running use | glmark2 full run at 2560x1664 with no errors; Hyprland with GL clients, one-hour soak (676,080 jobs) with no errors; several GL processes at once; 1,000 fresh GL processes in a row |
| Hangs the firmware does not report | Fail-stop cleanly. A reboot is still needed |
| Display | Mesa frames scanned out full screen through the DCP display driver (KMS/GBM); see [Display](60-display.md) |

## How facts are tagged

| Tag | Meaning |
|---|---|
| **[HW]** | Verified on hardware under Linux: a test whose result depends on it passed byte for byte |
| **[NATIVE]** | Observed in captures of macOS running the GPU under a hypervisor |
| **[INFERRED]** | Deduced from code structure, earlier generations or consistent indirect evidence; not tested directly |
| **[STATIC]** | Read from the macOS firmware or kernel binaries, not executed |
| **[UNKNOWN]** | Open question |

## Sections

1. [Memory map and address spaces](10-memory-map.md)
2. [Firmware interface and submission](20-firmware-and-submission.md)
3. [Render jobs](30-render.md)
4. [Compute jobs, faults and recovery](40-compute-and-recovery.md)
5. [Shaders, launch records and tile programs](50-shaders.md)
6. [Display](60-display.md)
7. [Known limits and open problems](70-known-limits.md)

## What this is not

It contains no Apple firmware, binaries, dumps or captured data. It only
describes structures, fields and behaviour. Power and clock values are
machine-specific: read them from the device's own Apple Device Tree.

## Things that will bite you first

- **Every user shader and everything the USC fetches lives in one window.** The window is [0x100_1000_0000, 0x101_0000_0000). USC fetch is a fixed 1 TiB base plus a 32-bit offset; `usc_exec_base` is ignored. [HW]
- **VDM streams and PPP state live in [0x10_0400_0000, 0x11_0000_0000).** Their pointers are offsets from 0x10_0000_0000. [HW]
- **G15 always soft-faults.** Stores to unmapped or read-only memory are dropped silently, so check results byte for byte. [HW]
- **One job runs at a time, device-wide.** [HW]
- **HwDataB +0xa74 must be 0.** The legacy layout's 1 there makes the
  firmware's power task read a GPU counter while the GPU is powered down,
  and the firmware crashes under sustained load. See [Firmware](20-firmware-and-submission.md#hwdatab-0xa74-and-the-power-down-serror). [HW]
- **Tiling geometry and tiler mode follow the target, not a small capture.**
  See [Render jobs](30-render.md#4-registers-taken-from-cmd_render). [HW]
- **A hang the firmware does not detect never recovers.** Examples are an infinite shader loop and some render hangs. Closing the client while one runs crashes the firmware. A host watchdog that fail-stops without touching the firmware is the safe response. [HW]
