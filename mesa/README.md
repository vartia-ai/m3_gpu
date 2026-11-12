# Mesa patch for G15 rendering

`g15-render.patch` is the complete G15 render-submission and driver change set
as one diff against Gravity Mesa commit `87ac51d411f0e58a9433567ec2666f457be89e1d`
(the Asahi Gallium driver with Apple9/G15 compiler support). It adds:

- G15 device selection and the G15 GPU VA layout;
- the Gallium render flush for G15 (VDM/PPP streams, attachments, render
  targets up to 8192x8192, linear and GPU-tiled RGBA8/BGRA8, D32F/D16 depth,
  S8 stencil, depth-only passes, mip levels);
- Mesa's own background/reload, end-of-tile and partial-render programs
  (partial renders let a render hold more geometry than the tiler buffer;
  renders have no primitive or draw cap by default);
- external textures (needed by Hyprland);
- instanced vertex attributes, flat varyings, R8 GPU-tiled render targets,
  and render-target allocations padded to the size the kernel checks (what
  Firefox's WebRender and niri/Smithay need; `tools/gl-test/g15-gl-webrender`);
- depth buffers smaller than one ZLS page laid out as a full page (WebRender's
  32x1024 and 1024x32 targets);
- environment switches (`ASAHI_G15_*`) for experiments and diagnostics.

The public patch omits one private diagnostic (loading macOS-compiled
fragment shaders from local files); otherwise it equals the development branch.

Apply with `git apply g15-render.patch` on that commit, or fetch, patch and
build it with `build.sh SRCDIR PREFIX` (steps: [../AGENTS.md](../AGENTS.md),
section 4). The driver talks to the G15 kernel
driver's UAPI (`DRM_ASAHI_*` G15 extensions) in [../kernel](../kernel/README.md).

Set `ASAHI_G15_GRAPHICS=1` to enable G15 rendering.
