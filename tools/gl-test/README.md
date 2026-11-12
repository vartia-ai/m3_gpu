# g15-gl-triangle

A GLES 3.0 test client for the G15 Mesa driver. Each `--mode` draws a known
scene (triangles, clears, reloads, depth, stencil, mip levels, overdraw,
stress grids, external textures, ...) into an offscreen target, reads it back
and checks it against a CPU oracle (`render-ref.h`) or against llvmpipe.

    make
    ASAHI_G15_GRAPHICS=1 LD_LIBRARY_PATH=$G15_MESA/lib ./g15-gl-triangle --mode const
    ./g15-gl-triangle --driver software --mode const     # llvmpipe reference

`--help` lists the modes and options. Each run prints one `G15_GL_*` result
line (PASS/FAIL) and exits nonzero on failure.

# g15-gl-webrender

Checks for the GL features Firefox's WebRender and niri/Smithay use:
instanced draws (`noinst`, `instid`, `attrib`), flat varyings (`flat`,
`flatint`), narrow render targets (`size W H`) and R8 targets (`r8 W H`).
Each mode renders offscreen and compares read-back pixels with the expected
colours.

    make g15-gl-webrender
    ASAHI_G15_GRAPHICS=1 LD_LIBRARY_PATH=$G15_MESA/lib ./g15-gl-webrender attrib
    ./g15-gl-webrender size 3 770
    G15_TEST_DRIVER=software ./g15-gl-webrender attrib    # llvmpipe reference

Each run prints one `G15_*` result line and exits nonzero on failure.
