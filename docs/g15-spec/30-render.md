# Render jobs

A G15 render is still a TA (vertex/tiling) pass followed by a 3D (fragment)
pass, published as a `RunVertex`/`RunFragment` pair on the TA and 3D
channels, with a Barrier and an InitBuffer in front. What changes from
G13/G14 is the shape of those commands, the register model, the address
model of the VDM/PPP streams, and how the tiled vertex buffer (TVB) is
described. This section covers each in turn and ends with what is still
unknown.

Tags follow the [README](README.md#how-facts-are-tagged). Offsets are byte
offsets into the named structure unless stated otherwise.

## 1. How a G15 render is assembled

On G13/G14 the kernel authors every firmware object from `drm_asahi_cmd_render`
fields (the legacy `render.rs` path). The working G15 path is a hybrid:

| Piece | Source |
|---|---|
| RunVertex/RunFragment command images, SKU streams, Barrier, InitBuffer | A render bundle captured on macOS (command templates plus patch lists), with kernel overlays for addresses and lifecycle scalars [HW] |
| Register lists (58 TA, 75 3D entries in the triangle bundle) | The bundle, with a fixed set of entries overridden from `cmd_render` (section 4) [HW] |
| VDM stream, PPP state, VS/FS launch records, shaders | Userspace (Mesa), in user VM_BIND buffers [HW] |
| Background/end-of-tile programs, partial programs | Userspace when given, else the bundle's (section 7) [HW] |
| Render target, depth buffer | Userspace attachments [HW] |
| TVB, tilemap, heap metadata, TPC, PM records, UMA pool | Kernel-allocated, sized from the bundle (section 8) [HW] |

The bundle is installed once per device by a privileged client; an ordinary
Mesa `cmd_render` (no G15-private flag bits) then runs on a per-submit copy of
it. Registers that the kernel overrides must be plain native constants in the
bundle; an override of a relocated or derived entry is rejected with EINVAL
[HW].

Fields the plain-command path refuses: `VERTEX_SCRATCH` flag, vertex/fragment
helper programs, sampler heap/count, timestamps, stencil, and depth
compression all return ENODEV. Unknown flag bits are EINVAL. Render flag bit 3
is `HAS_LOAD_CLEAR` in this kernel's UAPI but `VDM_BARRIER_FRAGMENT` in newer
Mesa; it is refused until reconciled [HW].

Submission constraints (all [HW]):

- One render per submit. A submit carrying compute and render returns ENODEV.
- One G15 job runs at a time across the device.
- A client must opt in through a G15-private ioctl before any SUBMIT;
  otherwise SUBMIT is ENODEV. This keeps stock userspace from submitting
  work it cannot describe correctly.
- The first fragment attachment is the colour render target. Every attachment
  must lie in a user VM_BIND READ|WRITE mapping outside the kernel's fixed
  windows.

## 2. VDM and PPP stream placement

| Rule | Tag |
|---|---|
| VDM control streams and PPP state live in the render-context window [0x10_0400_0000, 0x11_0000_0000). GET_PARAMS reports it as `render_context_start/end` | [HW] |
| The window base is 0x10_0000_0000. Pointers in this space are 32-bit offsets from it, not full VAs | [HW] |
| TA register 0x1c880 (VDM control stream base) holds `vdm_va - 0x10_0000_0000`. The legacy G15 arm wrote the full VA, which is wrong | [HW] |
| PPP State pointers inside the VDM stream (word 1 of each PPP packet) are offsets from 0x10_0000_0000 | [HW] |
| [0x10_0000_0000, 0x10_0400_0000) belongs to the kernel: four fixed windows near the bottom (0x10_0000_0000..0x10_0028_8000) hold the TVB, its page list, TPC and the PM scene. A user VDM below 0x10_0400_0000 is EINVAL | [HW] |
| A VDM buffer bound read-only works. PPP works bound RO or RW | [HW] |
| The VDM vertex launch and PPP fragment launch reference USC code as offsets from the fixed 1 TiB USC base (0x100_0000_0000). Code must sit in the USC window [0x100_1000_0000, 0x101_0000_0000) | [HW] |
| TA register 0x1c950 still carries the bundle's old PPP page pointer; renders do not depend on it | [HW] |
| A stream of only Stream Terminate completes with no draws and no hang | [HW] |

Mesa allocates a dedicated context heap over the reported window and keeps it
disjoint from the main heap and the USC window.

## 3. Command structures

### RunVertex (TA), 0x920 bytes

| Offset | Field | Tag |
|---|---|---|
| +0x04 | Counter (n + 1) | [HW] |
| +0x0c | Context ID | [HW] |
| +0x14 | Event-control block FWVA (shared with 3D +0x20) | [HW] |
| +0x24 | BufferInfo (parameter-buffer Info, 0x80 bytes) FWVA | [HW] |
| +0x2c / +0x34 | PM record / PM state FWVA | [HW] |
| +0x40 | RegisterArray (12-byte entries) | [HW] |
| +0x760 / +0x768 | Tail pointer cache (TPC) address / size (TileInfo: 0x800 at 128, 0xa000 at 2560x1664) | [HW] |
| +0x770 / +0x778 | TA SKU stream FWVA / size | [HW] |
| +0x77c / +0x780 | 3D stamp slot / value | [HW] |
| +0x7a8 | Context-switch secondary pointer (also TA reg 0x10111) | [HW] |
| +0x840 | Submission tag (equals 3D +0xb88) | [HW]; producer [UNKNOWN] |
| +0x8b0 | Per-pass timing record | [HW] |
| +0x8d6 / +0x8de / +0x8df / +0x8e7 / +0x8ef | UMA page-pool state FWVA, prepared flag, min/ideal pool size, metrics FWVA (byte-packed, unaligned) | [HW] |
| +0x8f7 | Context generation | [HW] |
| +0x8f8..+0x91f | SKU timing, firmware-owned | [NATIVE] |

### RunFragment (3D), 0xc60 bytes

| Offset | Field | Tag |
|---|---|---|
| +0x04 / +0x0c | Counter (n) / context ID | [HW] |
| +0x14 / +0x1c | 3D SKU stream FWVA / size (0x340) | [HW] |
| +0x20 | Event-control block FWVA | [HW] |
| +0x28 / +0x30 / +0x38 | BufferInfo, PM record, PM state FWVA | [HW] |
| +0x40 | TVB tilemap; equals 3D reg 0x16429 and TA reg 0x1c039 + 0x10_0000_0000 | [HW] |
| +0x48..+0x7f | RTM-derived block (replaces the legacy tile fields) | [NATIVE] |
| +0x68 / +0x6c | `isp_merge_upper_x` / `_y` (copies of 3D 0x15131 / 0x15139) | [HW] |
| +0x78 | Tile count `tx * ty` (32x32 tiles) | [HW] |
| +0x80 | RegisterArray | [HW] |
| +0x790..+0xa47 | JobParameters3 (G15 layout, below) | [HW] |
| +0xa50 | Copy of 3D 0x15321 `isp_zls_pixels` | [HW] |
| +0xa58..+0xb5f | Zeroed by the host, firmware-active | [NATIVE] |
| +0xb60 | Busy flag | [NATIVE] |
| +0xb64 | Legacy `tvb_overflow_count` position. Stays 0 on G15, even when partials run | [NATIVE] |
| +0xb6c..+0xb93 | Encoder state (0x28 bytes) | [NATIVE] |
| +0xb74 | Partial renders the firmware ran for this pass, read at completion | [NATIVE]; never nonzero on Linux yet |
| +0xb88 | Submission tag (equals TA +0x840) | [HW] |
| +0xb9c | Context ID - 1 | [HW] |
| +0xbf8 / +0xc00 | Timing record start / end (+8) | [HW] |
| +0xc1e / +0xc26 / +0xc27 / +0xc2f / +0xc37 | UMA page-pool state, prepared, min/ideal size, metrics (as TA) | [HW] |
| +0xc3f | Context generation | [HW] |
| +0xc40..+0xc5f | SKU timing, firmware-owned | [NATIVE] |

### JobParameters3 on G15

G15 does not use the legacy semantic JobParameters3 layout. The firmware's
submit path copies slices of its 3D descriptor into +0x790..+0xa47 in source
order, with unwritten gaps [NATIVE]. Many words mirror a 3D register value
and must be kept equal to the register the command carries, including after
an override [HW]:

| RunFragment offset | Mirrors | Meaning | Tag |
|---|---|---|---|
| +0x7a0 | 0x15101 | Depth-bias array base | [HW] |
| +0x7b0 | 0x15109 | Scissor array base | [HW] |
| +0x8f8 / +0x900 | 0x15369 / 0x15371 | Background resource spec / USC address | [HW] |
| +0x928 / +0x930 | (none) | Partial background spec / USC address | [NATIVE]; written [HW] |
| +0x938 | 0x15319 | ZLS control | [HW] |
| +0x948 | 0x15329 | Z base | [HW] |
| +0x960 / +0x968 | 0x15329 / 0x15331 | Z load base / Z store base | [HW] |
| +0x9b8 | 0x10051 | Sample size | [NATIVE] |
| +0x9c0 / +0x9c8 | 0x15021 / 0x15211 | ISP control / `height << 32 \| width` | [HW] |
| +0x9d8 | 0x10039 | Fragment tile config (empty-tile processing) | [NATIVE] |
| +0x9fc | 0x15381 | End-of-tile USC address | [HW] |
| +0x9f4 | possibly 0x15379 (end-of-tile resource spec) | Not confirmed as a mirror; the kernel leaves it as captured | [UNKNOWN] |
| +0xa14 / +0xa1c | (none) | Partial end-of-tile spec / USC address | [NATIVE]; written [HW] |

The partial-program words have no register; no separate partial register
appears in the native lists [NATIVE].

## 4. Registers taken from `cmd_render`

Of the bundle's register lists, these entries are overridden on every render.
The kernel applies them after emitting the native list. "Mask" means only
those bits change; the bundle's value keeps the rest.

| Engine | Register | Value | Tag |
|---|---|---|---|
| TA | 0x10129 | `(height - 1) << 16 \| (width - 1)` | [HW] |
| TA | 0x101b9 | `(ty - 1) << 12 \| (tx - 1)` | [HW] |
| 3D | 0x100d9 | `(ty - 1) << 12 \| (tx - 1)` | [HW] |
| 3D | 0x16068 | Native constant with `tx - 1` in the 9-bit field at bit 44 and `ty - 1` in the 9-bit field at bit 53 (all seven native references hold only these in those windows) | [HW], exact images up to 80x52 tiles (2560x1664) |
| 3D | 0x15131 / 0x15139 | `isp_merge_upper_x` / `_y` = `fui(sqrt(3) / width)`, `fui(sqrt(3) / height)` | [HW] |
| TA | 0x10121 | `ppp_ctrl` (Mesa: 0x202) | [HW] |
| 3D | 0x15319 | `zls_ctrl` | [HW] |
| 3D / TA | 0x10019 / 0x10139 | `ppp_multisamplectl` | [HW] (1 sample only) |
| 3D | 0x15301 | `isp_bgobjdepth` | [HW] |
| 3D | 0x15309 | `isp_bgobjvals` | [HW] |
| 3D | 0x15211 | `height << 32 \| width` | [HW] |
| 3D | 0x15321 | `isp_zls_pixels` (`width - 1`, `height - 1`; 0 without depth) | [HW] |
| 3D | 0x15329 / 0x15331 | `depth.base` (Z load and store base; 0 without depth) | [HW] |
| 3D | 0x15371 / 0x15369 | Background: `0x100_0000_0000 \| bg.usc`, `bg.rsrc_spec` | [HW] |
| 3D | 0x15381 / 0x15379 | End-of-tile: `0x100_0000_0000 \| eot.usc`, `eot.rsrc_spec` | [HW] |

Tiling geometry, also per render (the formulas of the G13/G14 driver's
`get_tiling_params`, with 32x32 utiles, one layer, 4x4 macrotiles):
`tpm_x = align(ceil(tx / 4), 4)`, `tpm_y = align(ceil(ty / 4), 4)`,
`tiles_per_mtile = tpm_x * tpm_y`, `rgn_size = align(5 * tiles_per_mtile, 4) / 4`.
The native references (all 256 px or smaller) only show the 128x128 values,
which these reproduce; larger targets need their own, or the tiler walks a
wrongly shaped tilemap (IPF faults past its end at 4096x4096) [HW].

| Engine | Register | Value | Tag |
|---|---|---|---|
| TA | 0x1c0b1 / 0x1c850 | `rgn_size` | [HW] [NATIVE] 2560x1664 |
| 3D | 0x16431 | `rgn_size << 26` | [HW] [NATIVE] 2560x1664 |
| TA | 0x1c069 / 0x1c071 | `3t \| 2t << 9 \| t << 18` for `t = tpm_x` / `tpm_y` | [HW] [NATIVE] 2560x1664 |
| TA | 0x1c081 | `tiles_per_mtile` | [HW] [NATIVE] 2560x1664 |
| TA | 0x1c0a9 | `2 * tiles_per_mtile` (TPC stride) | [HW] [NATIVE] 2560x1664 |
| 3D | 0x100b1 / 0x16030 | `tpm_y \| tpm_x << 16` (ISP macrotile size) | [HW] [NATIVE] 2560x1664 |

Tiler mode selectors: 3D 0x10039 bits 0x280, TA 0x10141 = 0x200 and 3D 0x16068
bit 37 are on in the small native references and off in the captured
2560x1664 clear+store, load+store pair. Linux turns them off for every render
and sets 0x10039's process-empty-tiles bit (0x10000) from
`DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES` [HW]. With them on, large
full-surface load renders made the firmware's power-down race (see
[Firmware](20-firmware-and-submission.md#hwdatab-0xa74-and-the-power-down-serror))
far more likely. What Apple keys them on is [UNKNOWN]: another 2560x1664
render in the same capture has them on. 3D 0x1c838 equals the ParamBuffer
slot (TA 0x1c830), not a size field [NATIVE].

`tx = ceil(width / 32)`, `ty = ceil(height / 32)`, each at least 1. A 200x120
target gives 0x10129 = 0x7700c7, 0x101b9 = 0x100d9 = 0x3006 and 0x16068 with
tile fields 6 and 3, identical to the native capture [HW].

Notes on the formulas:

- The UAPI comment describing `isp_merge_upper` as "tan * width" is wrong; the
  native value is bit-exact with Mesa's `sqrt(3)/size` formula [NATIVE].
- The legacy G14 formula for 0x16068 does not reproduce the native value;
  only the tile fields above are known to follow size [NATIVE].
- 0x15021 natively is the G14X pattern plus bit 0, which the G14X formula
  lacks; it stays the bundle's [NATIVE].
- The PPP fragment-control word's bit 16 enables the scissor, as on G13;
  set it whenever the scissor is on. (An early "bit 16 rasterizes nothing"
  came from an all-zero scissor array.) An empty scissor keeps its raw
  min > max descriptor: a zero descriptor admits pixels [HW] [NATIVE].
- Scissor (0x15109), depth bias (0x15101), occlusion query (0x15311), layers,
  utile (0x10009/0x10149) and sample registers stay the bundle's [HW].
- TA 0x1c880 is derived from the VDM VA (section 2) [HW].

Kernel-owned registers relocated per pass [HW]: TA 0x1c039 tilemap (3D
0x16429), heap/layer metadata (TA 0x1c031 = 0x1c079, 3D 0x16098 = 0x16060),
context switch (TA 0x10119, 0x1c9b8, 0x10111), scene slot (TA 0x1c830) and PM
page metrics (TA 0x1c910).

## 5. Render target

| Fact | Tag |
|---|---|
| Format: RGBA8, BGRA8 (and their X variants for scanout) UNORM, linear or uncompressed GPU-tiled, 1 layer, 1 sample; R8 UNORM uncompressed GPU-tiled (the background and end-of-tile programs follow the tilebuffer layout for any format; clear and draw values exact at 32x32, 1024x64, 3x770, 4096x16 and 2048x2048 [HW]). Mip levels above 0 for GPU-tiled targets: the attachment is the whole mip tree and the tile programs' descriptors address the level (texture lod, PBE level field at descriptor word 21 bits 4..), exact for every level to 8192x8192 [HW]. Channel order and tiling are carried by the PBE/texture descriptors of the tile programs; no command field changes | [HW] |
| Size: each side 1..8192 (`g15_render_max_dim`, up to 16384 accepted). Exact at 128x128, 160x160, 256x256, 200x120, 512x512, 1024x768, 1920x1080, 2560x1664, 4096x4096, 5120x3328 and 8192x8192. The attachment range must fit 256 MiB (`DRM_ASAHI_G15_RENDER_USER_RT_MAX_SIZE`), so 16384x16384 RGBA8 is refused | [HW] at those sizes |
| Depth-only passes (no colour attachment): Mesa binds a scratch RGBA8 target of the pass's size and discards its contents; exact with Z32F and Z16 | [HW] |
| Stride: `align(width * 4, 128)` bytes (Mesa's AGX linear layout). At 200 wide this is 896, where Metal used 800 | [HW] |
| Allocation: the kernel takes a target only if its attachment holds `align(width * 4, 128) * height` bytes (`render_input.rs` `target_size`), whatever the format. A narrow GPU-tiled texture is smaller (3x770 RGBA8: 16 KiB against 96 KiB) and its renders were EINVAL, so Mesa pads render-target allocations of formats up to 4 bytes per pixel to that size; layout and offsets are unchanged | [HW] |
| TPC, tilemap and heap metadata follow the tree's TileInfo formulas (TPC 0x800 at 128, 0x1000 at 513x257 natively, 0xa000 at 2560x1664); the TA command's TPC size (+0x768) is the allocation's length | [NATIVE] to 513x257, [HW] exact images to 2560x1664 |
| The target address reaches the GPU only through the tile programs' descriptors (background texture, partial-background texture, end-of-tile PBE). There is no render-target register | [HW] |
| The target is an ordinary user VM_BIND RW buffer (Mesa allocates it from its main heap); it need not be in the USC or render-context window | [HW] |

Descriptor size fields (24-byte descriptors whose qword 1 holds the target
address >> 4 in bits 0..36):

| Descriptor | Width - 1 | Height - 1 | Stride - 16 | Tag |
|---|---|---|---|---|
| Texture (background, partial background) | qword 0 bit 28 (14 bits) | qword 0 bit 42 (14 bits) | qword 1 bit 42 (14 bits) | [HW] |
| PBE (end-of-tile) | qword 0 bit 24 (14 bits) | qword 0 bit 38 (14 bits) | qword 1 bit 40 (16 bits) | [HW] |

The stride field is `stride - 16` in both layouts, not `stride / 16 - 1`
[HW]. Patched this way, the three descriptors equal the native ones at every
captured size apart from address bits [NATIVE].

## 6. Depth

| Fact | Tag |
|---|---|
| Format: Z32F (GL's DEPTH24 formats qualify, since AGX stores them as Z32F) or Z16 (ZLS control Z format bits 25-26 = 2; depth bytes equal llvmpipe's) | [HW] |
| Layout: GPU-tiled, uncompressed, single level; 64x64 pixels per 16 KiB page for Z32F, 128x64 for Z16. ZLS addresses smaller targets in the same page layout, so Mesa lays out a single-level buffer narrower or shorter than a page as at least one page in each direction (ail alone would pack it more tightly), and the kernel counts at least one page per direction. Exact from 1x1 to 1920x1080, including 1x64, 64x1, 1x1024, 32x1024 and 1024x32 (depth rejected and passed, and kept across a readback) | [HW] |
| The depth image (`ceil(w/64) * ceil(h/64) * 16 KiB`) must lie inside one of the command's attachments. `depth.base` must be 128-byte aligned. `isp_zls_pixels` packs `width - 1` and `height - 1`, so it must be 0 without a depth or stencil buffer, and 0 is also the 1x1 value with one | [HW] |
| Registers: 0x15321 = `isp_zls_pixels`; 0x15329 and 0x15331 = `depth.base`. Mirrors: +0x938 (ZLS control), +0x948, +0x960, +0x968 (Z bases), +0xa50 (ZLS pixels) | [HW] |
| ZLS control: 0x80000 for store only, 0x88000 for load and store (Mesa's existing packing gives both) | [HW] |
| Depth clear uses the ZLS background (`isp_bgobjdepth` = `fui(depth)`, `isp_bgobjvals` = `0x300 \| stencil`) | [HW] |
| PPP fragment-control word (PPP group + 0x34): bit 18 set with colour only (native 0x40200), cleared when a depth buffer is bound (native 0x200). The G16 encoding, which sets bit 16 instead, is accepted but rasterises nothing | [HW] |
| Native face words give LESS with writes using Mesa's existing encoding. Late depth (`gl_FragDepth`) and depth-mask-off draws work | [HW] |
| Stencil: a separate uncompressed GPU-tiled S8 buffer (128x128 pixels per 16 KiB page, sides >= 128) at `stencil.base` -> 3D 0x15339 (load) and 0x15341 (store) and 0x15229 (base & ~0x7f); ZLS control S load bit 14, S store bit 18; clear value in `isp_bgobjvals` bits 0..7. Kept across renders exactly (8-64 renders, 128x128..2560x1664). TA 0x1c1b9 and the stencil strides are not needed for one level | [HW] |
| Depth and stencil compression, stencil level offsets | [UNKNOWN]; refused |

## 7. Background and end-of-tile programs

| Fact | Tag |
|---|---|
| `bg.usc` / `eot.usc` (and the partial pair) carry a 32-bit offset from the 1 TiB USC base. The kernel writes `0x100_0000_0000 \| usc` into the USC register or command word | [HW] |
| A value with only low flag bits set (`usc & ~0x3f == 0`, e.g. Mesa's generic fill of 4) means "no program": the bundle's program stays. A nonzero address must be 64-byte aligned, else EINVAL | [HW] |
| `rsrc_spec` is 32 bits in the UAPI. The native background spec register has high bits set; the native partial-background spec is `0xffff800000000000 \| low32`, which the kernel reproduces. The partial end-of-tile spec is 0 | [NATIVE]; high bits' role [UNKNOWN] |
| The end-of-tile state loader register 0x15351 is 0 natively; G15 does not need `eot_state_loader` | [NATIVE] |
| Mesa's own reload background (texel fetch of the target) and its end-of-tile store (an image block store per tile) run in place of the bundle's programs; draw, read, draw keeps every byte | [HW] |
| A clear-only batch with Mesa's end-of-tile program needs an explicit VDM stop word, since the tile helpers emit no draw | [HW] |
| Natively, a clear pass's partial background differs from its normal background; in a depth Load pass they alias. Partial end-of-tile aliases normal end-of-tile in every capture | [NATIVE] |

## 8. TVB, BufferInfo and the UMA pool

### TVB geometry

| Fact | Tag |
|---|---|
| TVB page size 32 KiB, 4 pages per block, block 128 KiB (0x20000) | [HW] |
| The bundle installs 11 blocks at fixed page IDs, based at 0x10_0000_0000 | [HW] |
| Page list: one u32 page ID per page; native window 0x28000 bytes. The firmware permutes its order between jobs without growing it | [NATIVE] |
| Block list: one 8-byte entry per block ID (first u32 = ID), 0x14000-byte object (0x26cd entries). Natively at 0x100_033c_0000 with the cached FW+GPU RW PTE class; static across jobs, including partial renders | [NATIVE] |
| Block control: natively the lower half of the 0x80-byte PM scene-statistics (PSTS) object, first two words `{11, 11}`. PM records +0x40 point at the upper half | [NATIVE] |
| Counter: a 4-byte object 0x8000 after the PSTS base, in the same queue-state range. Starts at 1 and increments exactly once per fragment completion | [NATIVE] |
| No GrowTVB request or acknowledgement occurs natively, even when partials run; the firmware works within the fixed 11 blocks | [NATIVE] |

### BufferInfo (0x80 bytes, G15 layout)

G15 keeps the legacy prefix through +0x48, then switches to a compact layout.

| Offset | Field | Value | Tag |
|---|---|---|---|
| +0x0c | `cur_id` | 0 at creation (not -1) | [NATIVE] |
| +0x1c | Page list address (u64) | | [HW] |
| +0x24 | Page list size | 0x28000 natively | [NATIVE] |
| +0x28 | Page count | | [INFERRED] |
| +0x2c | `max_blocks` | 0x26cd natively | [NATIVE] |
| +0x30 | Block count | | [INFERRED] |
| +0x38 | Block list address | | [HW] |
| +0x40 | Block control address (PSTS lower half) | | [NATIVE] |
| +0x48 | Last page | | [INFERRED] |
| +0x4c | Block size in bytes | 0x20000 | [NATIVE] |
| +0x50 | Completion statistic; fragment completion max-tracks its low dword into PSTS +0x04 | | [INFERRED] |
| +0x58 | Counter address (u64) | | [NATIVE] |
| +0x64 / +0x68 / +0x6c | Partial-render counts; go 0 -> 4 with RunFragment +0xb74 | | [NATIVE] |
| +0x70 | Max pages = `max_blocks * 4` (0x9b34) | Zero here stalls a tile list's first growth | [NATIVE] |
| +0x74 | Max pages without memoryless = `(max_blocks / 3) * 4` (0x33bc) | | [NATIVE] |

With every one of these set to the native value, pointer range and PTE class,
renders up to the failure point below are exact and the failure is unchanged
[HW].

### UMA page pool

TA and 3D share one render UMA pool [HW]. The triangle bundle sizes it at 25
resident 4 MiB blocks (25600 4 KiB pages) [NATIVE]. The firmware sees each
pool through a 0x70-byte Page Pool State object and a 0x20-byte record in a
descriptor table indexed by hardware buffer ID:

| Descriptor qword | Packing | Tag |
|---|---|---|
| q0 | `(list_capacity << 41) \| (page_pool_list_fwva >> 7)` | [NATIVE] |
| q1 | `(state_bit << 61) \| (cursor_b << 33) \| (cursor_a << 5)`; cursor A = `(q1 >> 5) & 0x3fffff`, cursor B = `(q1 >> 33) & 0x3fffff` | [NATIVE] |
| q2 | Page count (low 22 bits) | [NATIVE] |

Page Pool State fields: +0x14 page-pool list FWVA, +0x1c capacity (bytes / 8),
+0x20 / +0x24 the two ring cursors (host seeds 0 and `page_count %
capacity`), +0x28 ring state bit, +0x2c page count, +0x34 backup page list,
+0x44 backup entry count, +0x48 FW uncached state [NATIVE]. The pool is a
ring of page VAs: each job advances both cursors (native per-job deltas of
208 to 656 pages; Linux passing jobs advance 208 to 224) [HW]. Which cursor is
producer and which consumer is not assigned [UNKNOWN]. Starting with 10 of 25
blocks admitted, as native pre-publication state shows, changes nothing [HW].

macOS registers each async-grow render pool with DeviceControl 0x1b before
its first submission: +4 pool ID, +8 0x3f, +0xc Page Pool State FWVA, +0x14
backup page list, +0x1c backup cursor (list + admitted blocks x 0x40), +0x24
0x78, +0x28 hardware buffer ID, +0x2c async-grow flag [NATIVE]. The
firmware answers with Event 0x0e echoing those fields; it grows the
descriptor (q2) at the reply and copies the growth into the Page Pool State
at completion; Apple's handler only retires the request [NATIVE] [HW].
Linux renders without it, and reproducing it in native order changes nothing
[HW].

## 9. Tile lists and the per-tile list limit (G2)

### Tile-list format

| Fact | Tag |
|---|---|
| Each tile's primitive list is a chain of 128-byte chunks in TVB memory, 0x80-aligned | [HW] |
| A TPC entry is 8 bytes: the list head (in 128-byte units from the TVB base 0x10_0000_0000), then `0xb0000000 \| tail` | [HW] |
| `tail` = 2 x the byte offset of the 0xa8 terminator within the final chunk | [HW] |
| The TA fills a chunk to +0x77 and ends it with a link at +0x78: `(target & 0xffffffff) \| 0x69`, target 0x80-aligned (native x1000: base 0x80000, links 0x800e9, 0x80169) | [NATIVE] |
| The stream grammar matches PowerVR's IPF/SIPF2 (Mesa `src/imagination/csbgen/rogue/ipf.xml`): byte-granular primitive headers, a 4-byte link packet, a 1-byte terminator | [INFERRED] |
| Linux and macOS TAs write byte-identical lists for the same work (x160 and x192 records, terminators and padding match) | [HW] [NATIVE] |

### The limit and its cause

Before the fix, Linux's fragment pass could not consume the second 64-byte
half of a chunk: a tile whose list carried content past byte 0x40 hung the
fragment unit (the firmware parked in its wait on unit 1, no pixel of that
tile was shaded, `Timeout`), while macOS consumed the same bytes [HW]
[NATIVE]. One tile passed at 160 stacked triangles (list ends 0x3f) and hung
at 168 (0x43); full-panel overdraw passed at 50 quads and hung at 51 [HW].

The cause is bit 0 of the Fragment ISP control word `0x15021`. The built-in
profile carried `0xc001` from macOS's small renders; macOS's long-list renders
use `0xc000`. Changing only that bit moves the failure both ways: macOS's own
long-list job, replayed through the Linux driver, hangs with `0xc001`, and
Mesa's long-list renders complete exactly with `0xc000` [HW] [NATIVE]. What
the bit selects in the fragment pass is not known [UNKNOWN]. The kernel
clears it in every render (`g15_frag_15021_bit0`, default 0).

Earlier experiments that are now explained by the bit: inputs, device setup,
timing and placement all matched macOS without effect, because the setting
that differed was in the profile's own register list. The list relinker that
worked around the limit (rewriting long lists into chunks that end by +0x38
with a link) is off by default (`g15_relink`, kept for diagnostics) [HW].

## Where render state comes from

A Linux render no longer needs any state captured on macOS at render time [HW]:

| Former source | Now |
|---|---|
| Captured register lists, SKU and command templates | Kernel constants: every word annotated as derived (computed per render), constant (identical in seven native references) or a Linux policy choice |
| Captured payload sections (shader libraries, USC spans, uniforms, PBE, PPP page, auxiliary data and code, auxiliary framebuffer) | Zero-filled kernel memory at the same VAs. With Mesa's own tile programs no content is read: every image stays exact with all of them zeroed |
| Background (clear) program | Mesa's clear program, run per tile as the background |
| Reload and end-of-tile programs | Mesa's |
| ISP scissor / depth-bias arrays (3D 0x15109 / 0x15101) | `cmd_render`'s `isp_scissor_base` / `isp_dbias_base` (low two bits cleared) |
| Auxiliary framebuffer (3D 0x16461 / 0x16090) | Zeroed kernel memory |

The 12 template bytes and the 95 constructor fields the native inventory left
unknown are kept at their native values; renders do not depend on them as far
as the tests reach [HW], but their meaning is [UNKNOWN].

## Open questions

- **Fragment ISP control bit 0 (`0x15021`).** Clearing it fixed the per-tile
  list limit; what it selects is not known. [UNKNOWN]
- **Constants still taken from one capture.** Renders run without any captured
  state, but these values are native constants whose producers are not
  known, so they may need to vary for workloads not yet tested:
  - the PM scene offset (0x220000), which should come from the live scene
    record;
  - the TVB block count, the BufferInfo limits and the TVB growth path;
  - the RTM block +0x48..+0x7f apart from +0x68/+0x6c/+0x78;
  - the submission tag (TA +0x840, 3D +0xb88);
  - the background spec register's high bits, and whether +0x9f4 mirrors the
    end-of-tile resource spec (0x15379).
- **Full-screen fragment timeouts.** At 2560x1664, shader-heavy scenes
  occasionally hit a firmware timeout with no partial render, although the
  same programs complete in most frames. Not seen since the tiling geometry
  and HwDataB +0xa74 fixes; unconfirmed whether they were the cause. [UNKNOWN]
- MSAA, layered targets, formats other than RGBA8/BGRA8/R8, compression.
  [UNKNOWN]
- How G15 takes vertex/fragment helper (scratch) programs and the sampler
  heap; the native lists have no register for either. [UNKNOWN]
- Compute and render in one submit, which needs the G15 Barrier ordering.
  [UNKNOWN]
- More than one render in flight per queue and more than one bundle per
  device. [UNKNOWN]
