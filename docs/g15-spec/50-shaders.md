# Shaders, launch records and tile programs

Tags follow the [README](README.md#how-facts-are-tagged). This section covers
the shader-facing G15 interface implemented by the current Mesa port.

## 1. USC address model

| Rule | Tag |
|---|---|
| The USC fetch base is fixed at `0x100_0000_0000`. A USC reference supplies a 32-bit offset from that base | [HW] |
| User objects fetched by the USC must lie in `[0x100_1000_0000, 0x101_0000_0000)`. This includes shader bodies, launch records, resource tables, binding tables, statics and preambles | [HW] |
| `usc_exec_base` does not relocate USC fetches on G15 | [HW] |
| The kernel reports the usable range as `usc_window_start/end`; Mesa keeps its USC pool inside that range | [HW] |
| The current G15 launcher requires a 64-byte-aligned shader entry and launch allocation | [INFERRED] |
| Ordinary render targets and SSBO data need not be in the USC window. They are reached through full addresses loaded from a USC-resident table | [HW] for placement; table semantics [INFERRED] |

The USC window is independent of the render-context window used for VDM and
PPP state. VDM and PPP addresses use offsets from `0x10_0000_0000`; shader and
launch addresses use offsets from the USC base above. [HW]

## 2. Compiler and ISA

The working G15 path uses Mesa's Apple9 compiler for vertex, fragment and
compute shaders. G15 has a distinct launch-target encoding from the compact
Apple9 target used by the existing G16/G17 path. [HW] for executed compiler
output; target-format distinction [INFERRED]

The compiler pipeline lowers NIR to Apple9 VIR, allocates and schedules GPRs,
then emits the machine program. It reserves at least eight GPRs and records the
highest published 32-bit slot as the publication count. The emitted shader
body ends with `STOP`. [INFERRED]

The following compiler behavior has passed byte-exact Linux tests on G15:

| Feature | Evidence | Tag |
|---|---|---|
| Vertex position and fragment RGBA output | Constant and VBO-backed triangles | [HW] |
| Smooth perspective varyings | Exact gradients, interleaved position/colour VBOs and a shader using 39 scalar varyings | [HW] |
| Non-perspective (`linear`) varyings | Exact colour oracle at the four R4 sizes and 2560x1664 | [HW] |
| API uniforms and compiler preambles | GLES uniform and clear paths | [HW] |
| Fragment texture access | `textureLod` and `texelFetch` against a saved input image | [HW] |
| Vertex texture access | Two explicitly-lod-sampled textures; exact at the R4 sizes and 2560x1664, plus glmark2 terrain | [HW] |
| Fragment image block stores | Mesa's end-of-tile program at all four qualified target sizes | [HW] |
| Fragment discard | Exact discard oracle and the full glmark2 default scene set | [HW] |
| Shader-written D32F depth | Late depth export and its companion test | [HW] |
| Early fragment-test metadata | Preserved through compilation; the forced-early depth companion matches the qualified native behavior | [NATIVE] for the reference; compiler qualification [INFERRED] |
| Coarse-derivative metadata | A live derivative sets `disable_tri_merging`; the corresponding PPP control change executes and the one-triangle image remains exact | [HW] |

The qualified native depth shader uses `access(1)`, `depth_store p0,p1` and
`fence(1)` before termination. Mesa's supported shader-written-depth case
matches that sequence. [NATIVE] for the reference; compiler match [INFERRED];
Linux depth result [HW]

The current software gate accepts at most 96 scalar varying components. The
39-scalar case is hardware-tested; 96 is a compiler capacity and is not a
measured G15 hardware limit. [HW] for 39; [INFERRED] for 96

Apple9 machine encoding is not assumed to be compatible with the G13/G14
Apple8 ISA. The port has no exhaustive G15 opcode, system-register or
scheduling description. [INFERRED] for incompatibility; [UNKNOWN] for the
complete ISA

## 3. Launch records

G15 uses separate vertex, fragment and compute launch records. The current
allocator reserves 256 bytes for a graphics launch and 1024 bytes for a
compute launch. Hardware has executed records built in those allocations; the
unused padding has no established hardware meaning. [HW] for execution;
allocation sizes and padding [INFERRED]

The current builder emits these logical operations in order:

1. Load the full resource-table GPU address into a GPR pair. [INFERRED]
2. Load the table roots needed by the stage. [INFERRED]
3. Declare the stage allocation. [INFERRED]
4. Select the shader entry with the G15 absolute target format. [HW] for the working target; field semantics [INFERRED]
5. Declare the publication frame and transfer arguments. [INFERRED]
6. Branch to an optional compiler-owned preamble, or terminate setup directly. [HW] for preamble execution; record grammar [INFERRED]

The target represents an address in the fixed 1 TiB USC window. Vertex uses a
different target-control value from fragment and compute in every qualified
record. [NATIVE] for the control distinction; addressing [HW]

The resource-table root is a full GPU address. The table and every pointer the
USC follows from it must still obey the USC window rule when the destination is
USC-fetched state. Buffer payload addresses may point to ordinary user VM
mappings. [INFERRED] for root semantics; placement [HW]

The compiler publication high-water mark becomes the launch publication
count. The current builder refuses a count above 1022. This is a software
bound, not a measured G15 limit. [INFERRED]

Graphics launch records and their resource tables are batch-owned allocations
in Mesa's USC pool. VDM and PPP state refer to them by their 32-bit USC
offsets. [HW]

## 4. Current graphics shader profile

The Linux G15 graphics path currently accepts a bounded subset:

| Property | Current rule | Tag |
|---|---|---|
| Vertex outputs | Four-component position plus up to 96 scalar varyings | [INFERRED]; 39 varying scalars [HW] |
| Fragment outputs | One colour render target; one sample | [HW] |
| Interpolation | Smooth perspective, non-perspective (`linear`) and flat interpolation (float and integer; the last vertex provokes, as GLES requires) | [HW] |
| Buffers | API UBOs, vertex buffers and the private sysval buffer; vertex stages also the draw-parameter table (binding 241, `AGX_SYSVAL_TABLE_PARAMS`), which instanced attribute fetch reads for the base instance; at most 32 table entries | [HW] for used forms; limit [INFERRED] |
| Texture resources | Textures and samplers in fragment shaders; bounded explicit-LOD texture sampling in vertex shaders | [HW] |
| Stores | Fragment image block stores are admitted for the internal tile store helper; general graphics SSBO stores are rejected | [HW] for the helper; general restriction [INFERRED] |
| Scratch | Vertex and fragment scratch are rejected | [INFERRED] |
| Depth | Fragment depth export is supported when early fragment tests are not requested | [HW] |
| Discard | Fragment discard is supported | [HW] |
| Clip distances | One through eight vertex clip-distance outputs are supported | [HW] |
| Other rejected inputs or outputs | Tile reads, fragment Z reads, primitive ID, point size, layer/viewport and point coordinates | [INFERRED] |

The render path currently requires a four-component vertex position and one
fragment colour output. Flat varyings use the per-component encoding of
`apple9_build_cf_bindings` (shade 0/2 for the first/last provoking vertex)
and are on by default; an `ASAHI_G15_ALLOW` list without `flat` turns them
off, as for `linear`. [HW]

## 5. Compute frontend

Ordinary Mesa compute uses a user VM_BIND CDM buffer, a generated launch
record in the USC pool and ordinary sync objects. Three consecutive
`write1337` dispatches pass on a fresh G15 boot and write the expected word to
the mapped output. [HW]

The current G15 compute profile deliberately accepts only this shape:

| Property | Current rule | Tag |
|---|---|---|
| Dispatch | Direct dispatch with global size `1x1x1` and local size `1x1x1` | [HW] for the accepted dispatch; gate [INFERRED] |
| Resources | Exactly one SSBO at binding 0; writes to binding 0 are required and reads may only use binding 0 | [HW] for the tested SSBO; gate [INFERRED] |
| Textures and samplers | Rejected | [INFERRED] |
| Raw global stores | Rejected | [INFERRED] |
| Variable local size | Rejected | [INFERRED] |
| Threadgroup memory, scratch, preamble and atomic frame | Rejected | [INFERRED] |
| CDM command | One direct-dispatch record, 44 bytes through its terminator | [HW] for the working command; size [INFERRED] |

The wider Apple9 source path contains indirect dispatch, descriptor-table,
texture, shared-memory and larger-grid support used by other chips. The G15
selection gate does not expose those paths. [INFERRED]

The experimental G15 path does not launch Mesa precompiled helper kernels.
This is a current software restriction, not evidence that G15 cannot execute
such programs. [INFERRED]

## 6. Background and end-of-tile programs

Mesa compiles colour reload and store helpers for each batch. The reload
helper samples the preserved linear RGBA8 target; the store helper writes the
tilebuffer to that target with fragment image block stores. Their launch
records and descriptor tables are batch-owned USC allocations. [HW]

Mesa's background reload and end-of-tile store are byte-exact at 128x128,
160x160, 256x256 and 200x120. Draw/read/draw preserves the first image, and
the GLES baseline passes with Mesa's end-of-tile helper enabled. Profile-free
R9 has no captured fallback store program, so Mesa's helper is required;
disabling it is not a supported configuration. [HW]

A clear-only batch must still publish a terminating VDM stream even though no
draw created one. The Mesa G15 path does so before submission. [HW]

The normal background and end-of-tile command fields carry the helper's
32-bit USC offset and a resource specification. The working Mesa helpers use
a zero low 32-bit resource specification. Native background state contains
additional high bits whose complete meaning is not known. [HW] for the Mesa
values; native high bits [NATIVE]; meaning [UNKNOWN]

## 7. Partial-render programs

Mesa assigns its normal colour reload helper to `partial_bg` and its normal
colour store helper to `partial_eot`. The kernel places those programs in the
G15 partial slots in `RunFragment`. This is the default.
`ASAHI_G15_MESA_PARTIAL=0` instead selects the captured macOS programs'
addresses; no program payload is included, so with the built-in profile
those addresses are unbacked and fault. [HW]

Native captures show that partial EOT aliases normal EOT in every qualified
case. Partial BG aliases normal BG for a depth Load pass and can differ for a
clear pass. [NATIVE]

A native 513x257 pressure job completes four partial renders and preserves the
entire colour and D32F result. It uses the fixed TVB without a GrowTVB request
or acknowledgement. [NATIVE]

On Linux, with Mesa's programs, one render of 262,144 triangles over a 128x128
D32F grid runs four partial renders and its colour and depth match the
oracle exactly; 5,000 full-panel triangles in one draw run 72 partial renders
over three frames with every pixel exact. [HW]

Earlier Linux runs never retired a partial render: their renders hung at the
per-tile list limit first (fixed; see
[Known limits](70-known-limits.md)). [HW]

## 8. Open questions

| Question | Tag |
|---|---|
| What are the complete G15 Apple9 opcode, system-register and scheduling differences from G16/G17? | [UNKNOWN] |
| What are the G15 hardware limits for publications, varyings, resource tables and launch allocation? | [UNKNOWN] |
| Which launch control fields differ by stage beyond the values already observed? | [UNKNOWN] |
| How are scratch, general SSBO writes, larger compute grids, indirect dispatch, threadgroup memory and compute textures launched on G15? | [UNKNOWN] |
| Do multiple render targets, MSAA and the remaining graphics inputs/outputs require only compiler work or additional submission state? | [UNKNOWN] |
