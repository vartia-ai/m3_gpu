// g15-gl-webrender: GLES 3.0 checks for what Firefox's WebRender and
// niri/Smithay need from the G15 Mesa driver. Each mode renders offscreen,
// reads pixels back and compares them with the expected colours.
//   g15-gl-webrender MODE [W H]
//   noinst      four glDrawArrays, position/colour as uniforms (control)
//   instid      one glDrawArraysInstanced, position/colour from gl_InstanceID
//   attrib      one glDrawArraysInstanced, per-instance attributes (divisor 1),
//               as niri/Smithay and WebRender draw
//   flat        flat float varyings (last provoking vertex)
//   flatint     flat integer varyings
//   size W H    RGBA8 GPU-tiled texture target of any size, narrow ones too
//   r8 W H      R8 GPU-tiled texture target (WebRender's clip masks)
//   depth W H   RGBA8 + DEPTH24 of the same size: a red quad on the lower-left
//               quarter at depth 0.5, then green over everything at 0.7 with
//               GL_LESS (sides >= 2: the quarter must cover a pixel centre)
//   depthfull W H  the same with full-coverage quads (any size, 1x1 too): red
//               0.5, green 0.7 (rejected), then blue 0.3 (drawn) after a
//               readback, so depth is stored and reloaded
// Prints one G15_* PASS/FAIL line; exits 0 on PASS. With
// G15_TEST_DRIVER=software it runs on llvmpipe (reference).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#define W 128
#define H 128
static const float offs[4][2] = {{-0.75f, -0.75f}, {0.25f, -0.75f}, {-0.75f, 0.25f}, {0.25f, 0.25f}};
static const float cols[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
#define SIDE 0.375f /* 24 px of 128 in NDC (2.0 = 128 px) */

static const char *vs_noinst =
    "#version 300 es\nlayout(location=0) in vec2 corner;\nuniform vec2 off;\nuniform vec4 col;\n"
    "out vec4 c;\nvoid main(){ c = col; gl_Position = vec4(off + corner * 0.375, 0.0, 1.0); }\n";
static const char *vs_instid =
    "#version 300 es\nlayout(location=0) in vec2 corner;\nuniform vec2 offs[4];\nuniform vec4 cols[4];\n"
    "out vec4 c;\nvoid main(){ c = cols[gl_InstanceID]; gl_Position = vec4(offs[gl_InstanceID] + corner * 0.375, 0.0, 1.0); }\n";
static const char *vs_attrib =
    "#version 300 es\nlayout(location=0) in vec2 corner;\nlayout(location=1) in vec2 off;\nlayout(location=2) in vec4 col;\n"
    "out vec4 c;\nvoid main(){ c = col; gl_Position = vec4(off + corner * 0.375, 0.0, 1.0); }\n";
/* flat: two triangles; GLES's provoking vertex is the last one of each triangle. */
static const char *vs_flat =
    "#version 300 es\nlayout(location=0) in vec2 pos;\nlayout(location=1) in vec4 col;\n"
    "flat out vec4 c;\nvoid main(){ c = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_flat =
    "#version 300 es\nprecision mediump float;\nflat in vec4 c;\nout vec4 o;\nvoid main(){ o = c; }\n";
static const char *vs_flatint =
    "#version 300 es\nlayout(location=0) in vec2 pos;\nlayout(location=1) in vec4 col;\n"
    "flat out ivec2 k;\nvoid main(){ k = ivec2(int(col.r * 2.0 + col.g), int(col.b)); gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_flatint =
    "#version 300 es\nprecision mediump float;\nflat in ivec2 k;\nout vec4 o;\n"
    "void main(){ o = vec4(float(k.x >= 2), float(k.x == 1 || k.x == 3), float(k.y), 1.0); }\n";
static const char *vs_full =
    "#version 300 es\nlayout(location=0) in vec2 corner;\nuniform vec4 col;\n"
    "out vec4 c;\nvoid main(){ c = col; gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *vs_depth =
    "#version 300 es\nlayout(location=0) in vec2 corner;\nuniform vec4 col;\nuniform vec3 box;\n"
    "out vec4 c;\nvoid main(){ c = col; gl_Position = vec4(corner * box.xy * 2.0 - 1.0, box.z, 1.0); }\n";
static const char *fs =
    "#version 300 es\nprecision mediump float;\nin vec4 c;\nout vec4 o;\nvoid main(){ o = c; }\n";

static GLuint sh(GLenum t, const char *s) {
    GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x);
    GLint ok = 0; glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(x, sizeof l, NULL, l); fprintf(stderr, "shader: %s\n", l); exit(2); }
    return x;
}
static GLuint prog2(const char *vs, const char *f) {
    GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, f));
    glLinkProgram(p); GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(2); }
    return p;
}
static GLuint prog(const char *vs) {
    GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p); GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(2); }
    return p;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "attrib";
    PFNEGLQUERYDEVICESEXTPROC qd = (void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qs = (void *)eglGetProcAddress("eglQueryDeviceStringEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!qd || !qs || !gpd) { fprintf(stderr, "EGL device extensions missing\n"); return 2; }
    EGLDeviceEXT devs[16]; EGLint n = 0; qd(16, devs, &n);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    const char *drv = getenv("G15_TEST_DRIVER");
    int software = drv && !strcmp(drv, "software");
    for (int i = 0; i < n && dpy == EGL_NO_DISPLAY; i++) {
        if (software) {
            const char *ext = qs(devs[i], EGL_EXTENSIONS);
            if (ext && strstr(ext, "EGL_MESA_device_software")) { fprintf(stderr, "using llvmpipe\n"); dpy = gpd(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL); }
            continue;
        }
        const char *node = qs(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT); if (!node) continue;
        int fd = open(node, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        drmVersionPtr v = drmGetVersion(fd);
        if (v && !strcmp(v->name, "asahi")) { fprintf(stderr, "using %s (asahi)\n", node); dpy = gpd(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL); }
        drmFreeVersion(v); close(fd);
    }
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, software ? "no llvmpipe EGL device\n" : "no asahi EGL display\n"); return 2; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "no GLES3 context\n"); return 2; }
    fprintf(stderr, "GL %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    /* depthfull W H: full-coverage quads, so it also checks 1-pixel sizes. Red at
     * depth 0.5, green at 0.7 (must fail GL_LESS), blue at 0.3 (must pass): blue. */
    int full = !strcmp(mode, "depthfull");
    if (!strcmp(mode, "depth") || full) {
        /* RGBA8 colour + 24-bit depth (as WebRender), same size. Clear depth 1.0;
         * red quad over the lower-left part at z=0.0 (depth 0.5); then a full green
         * quad at z=0.4 (depth 0.7) with GL_LESS: red stays where it was drawn. */
        int w = argc > 2 ? atoi(argv[2]) : 32, h = argc > 3 ? atoi(argv[3]) : 1024;
        GLuint tex, db, f; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &db); glBindRenderbuffer(GL_RENDERBUFFER, db);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("G15_DEPTH %dx%d fbo-incomplete\n", w, h); return 2; }
        glViewport(0, 0, w, h); glClearColor(0, 0, 0, 1); glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
        static const float q[] = {0, 0, 1, 0, 0, 1, 1, 1};
        GLuint vao2, b2; glGenVertexArrays(1, &vao2); glBindVertexArray(vao2); glGenBuffers(1, &b2);
        glBindBuffer(GL_ARRAY_BUFFER, b2); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        GLuint p = prog(vs_depth); glUseProgram(p);
        GLint lc = glGetUniformLocation(p, "col"), lb = glGetUniformLocation(p, "box");
        if (full) {
            unsigned char px[3][4]; int ok = 1;
            glUniform4f(lc, 1, 0, 0, 1); glUniform3f(lb, 1.0f, 1.0f, 0.0f); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glUniform4f(lc, 0, 1, 0, 1); glUniform3f(lb, 1.0f, 1.0f, 0.4f); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);          /* red: green rejected */
            glUniform4f(lc, 0, 0, 1, 1); glUniform3f(lb, 1.0f, 1.0f, -0.4f); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);          /* blue: nearer passes */
            glReadPixels(w - 1, h - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[2]);  /* far corner blue too */
            ok = px[0][0] == 255 && px[0][1] == 0 && px[1][2] == 255 && px[1][0] == 0 && px[2][2] == 255 && px[2][1] == 0;
            printf("G15_DEPTHFULL %dx%d %s after-green=%d,%d,%d after-blue=%d,%d,%d corner=%d,%d,%d "
                   "(want 255,0,0 / 0,0,255 / 0,0,255) glerror=0x%x\n", w, h, ok ? "PASS" : "FAIL",
                   px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0], px[2][1], px[2][2], glGetError());
            return ok ? 0 : 1;
        }
        glUniform4f(lc, 1, 0, 0, 1); glUniform3f(lb, 0.5f, 0.5f, 0.0f); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glUniform4f(lc, 0, 1, 0, 1); glUniform3f(lb, 1.0f, 1.0f, 0.4f); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        unsigned char in[4], out[4];
        glReadPixels(w / 4, h / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, in);           /* inside red quad */
        glReadPixels(3 * w / 4, 3 * h / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);  /* outside */
        int ok = in[0] == 255 && in[1] == 0 && out[0] == 0 && out[1] == 255;
        printf("G15_DEPTH %dx%d %s inside=%d,%d outside=%d,%d (want 255,0 / 0,255) glerror=0x%x\n", w, h,
               ok ? "PASS" : "FAIL", in[0], in[1], out[0], out[1], glGetError());
        return ok ? 0 : 1;
    }
    if (!strcmp(mode, "r8")) {
        /* R8 render target (GPU-tiled), as WebRender's clip masks: clear to 0.25,
         * then draw 0.5 over the left half. Expect ~64 on the right, ~128 on the left. */
        int w = argc > 2 ? atoi(argv[2]) : 256, h = argc > 3 ? atoi(argv[3]) : 256;
        GLuint tex, f; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, w, h);
        glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("G15_R8 %dx%d fbo-incomplete\n", w, h); return 2; }
        glViewport(0, 0, w, h); glClearColor(0.25f, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        static const float q[] = {0, 0, 0.5f, 0, 0, 1, 0.5f, 1};   /* left half */
        GLuint vao2, b2; glGenVertexArrays(1, &vao2); glBindVertexArray(vao2); glGenBuffers(1, &b2);
        glBindBuffer(GL_ARRAY_BUFFER, b2); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        static const float half[4] = {0.5f, 0, 0, 1};
        GLuint p = prog(vs_full); glUseProgram(p); glUniform4fv(glGetUniformLocation(p, "col"), 1, half);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish();
        unsigned char L[4], R[4], C[4];   /* RGBA/UNSIGNED_BYTE readback is always allowed */
        glReadPixels(w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, L);
        glReadPixels(3 * w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, R);
        glReadPixels(w - 1, h - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, C);
        int ok = abs(L[0] - 128) <= 2 && abs(R[0] - 64) <= 2 && abs(C[0] - 64) <= 2;
        printf("G15_R8 %dx%d %s left=%d right=%d corner=%d (want 128/64/64) glerror=0x%x\n", w, h,
               ok ? "PASS" : "FAIL", L[0], R[0], C[0], glGetError());
        return ok ? 0 : 1;
    }
    if (!strcmp(mode, "size")) {
        /* render into a w x h RGBA8 texture (GPU-tiled, like WebRender's targets) */
        int w = argc > 2 ? atoi(argv[2]) : 3, h = argc > 3 ? atoi(argv[3]) : 770;
        GLuint tex, f; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("G15_SIZE %dx%d fbo-incomplete\n", w, h); return 2; }
        glViewport(0, 0, w, h); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        static const float q[] = {0, 0, 1, 0, 0, 1, 1, 1};
        GLuint vao2, b2; glGenVertexArrays(1, &vao2); glBindVertexArray(vao2); glGenBuffers(1, &b2);
        glBindBuffer(GL_ARRAY_BUFFER, b2); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        static const float red[4] = {1, 0, 0, 1};
        GLuint p = prog(vs_full); glUseProgram(p);
        glUniform4fv(glGetUniformLocation(p, "col"), 1, red);
        glVertexAttribDivisor(0, 0);
        /* corner*0.375 + 0 covers only part; scale the quad to the full target */
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish();
        unsigned char a0[4], a1[4], a2[4];
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a0);
        glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a1);
        glReadPixels(w - 1, h - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a2);
        int ok = a0[0] == 255 && !a0[1] && a1[0] == 255 && !a1[1] && a2[0] == 255 && !a2[1];
        printf("G15_SIZE %dx%d %s corner=%d,%d centre=%d,%d far=%d,%d glerror=0x%x\n", w, h, ok ? "RED-OK" : "WRONG",
               a0[0], a0[1], a1[0], a1[1], a2[0], a2[1], glGetError());
        return ok ? 0 : 1;
    }
    GLuint rb, fbo; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 2; }
    glViewport(0, 0, W, H); glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);

    static const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
    GLuint vao, vb[3]; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(3, vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb[0]); glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    if (!strcmp(mode, "flat") || !strcmp(mode, "flatint")) {
        /* left triangle: red, green, BLUE (provoking); right: white, red, GREEN (provoking) */
        static const float v[] = {-0.9f, -0.9f, -0.1f, -0.9f, -0.9f, 0.9f,   0.1f, -0.9f, 0.9f, -0.9f, 0.9f, 0.9f};
        static const float c[] = {1,0,0,1, 0,1,0,1, 0,0,1,1,   1,1,1,1, 1,0,0,1, 0,1,0,1};
        GLuint p = !strcmp(mode, "flat") ? prog2(vs_flat, fs_flat) : prog2(vs_flatint, fs_flatint); glUseProgram(p);
        glBindBuffer(GL_ARRAY_BUFFER, vb[1]); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vb[2]); glBufferData(GL_ARRAY_BUFFER, sizeof c, c, GL_STATIC_DRAW);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6); glFinish();
        static unsigned char fp[W * H * 4]; glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fp);
        GLenum e = glGetError(); int bad = 0;
        /* sample points well inside each triangle, near all three corners */
        const int pts[2][3][2] = {{{12, 12}, {44, 12}, {12, 100}}, {{116, 12}, {84, 12}, {116, 100}}};
        const unsigned char want[2][3] = {{0, 0, 255}, {0, 255, 0}};
        for (int t = 0; t < 2; t++) for (int k = 0; k < 3; k++) {
            unsigned char *q = &fp[(pts[t][k][1] * W + pts[t][k][0]) * 4];
            if (abs(q[0] - want[t][0]) > 2 || abs(q[1] - want[t][1]) > 2 || abs(q[2] - want[t][2]) > 2) {
                bad++; fprintf(stderr, "triangle %d point (%d,%d): got %d,%d,%d want %d,%d,%d\n", t, pts[t][k][0], pts[t][k][1], q[0], q[1], q[2], want[t][0], want[t][1], want[t][2]); }
        }
        printf("G15_INST_RESULT %s %s points_ok=%d/6 glerror=0x%x\n", bad || e ? "FAIL" : "PASS", mode, 6 - bad, e);
        return bad || e ? 1 : 0;
    }
    if (!strcmp(mode, "noinst")) {
        GLuint p = prog(vs_noinst); glUseProgram(p);
        for (int i = 0; i < 4; i++) {
            glUniform2fv(glGetUniformLocation(p, "off"), 1, offs[i]);
            glUniform4fv(glGetUniformLocation(p, "col"), 1, cols[i]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    } else if (!strcmp(mode, "instid")) {
        GLuint p = prog(vs_instid); glUseProgram(p);
        glUniform2fv(glGetUniformLocation(p, "offs"), 4, &offs[0][0]);
        glUniform4fv(glGetUniformLocation(p, "cols"), 4, &cols[0][0]);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 4);
    } else if (!strcmp(mode, "attrib")) {
        GLuint p = prog(vs_attrib); glUseProgram(p);
        glBindBuffer(GL_ARRAY_BUFFER, vb[1]); glBufferData(GL_ARRAY_BUFFER, sizeof offs, offs, GL_STATIC_DRAW);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0); glVertexAttribDivisor(1, 1);
        glBindBuffer(GL_ARRAY_BUFFER, vb[2]); glBufferData(GL_ARRAY_BUFFER, sizeof cols, cols, GL_STATIC_DRAW);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, 0); glVertexAttribDivisor(2, 1);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 4);
    } else { fprintf(stderr, "mode: noinst|instid|attrib|flat|flatint\n"); return 2; }
    glFinish();

    static unsigned char px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    int fails = 0;
    for (int i = 0; i < 4; i++) {   /* square i: NDC offs + 0.1875 -> pixel centre */
        int x = (int)((offs[i][0] + SIDE / 2 + 1.0f) * W / 2), y = (int)((offs[i][1] + SIDE / 2 + 1.0f) * H / 2);
        unsigned char *p = &px[(y * W + x) * 4];
        int ok = 1; for (int k = 0; k < 4; k++) ok &= abs(p[k] - (int)(cols[i][k] * 255)) <= 2;
        if (!ok) { fails++; fprintf(stderr, "square %d at (%d,%d): got %d,%d,%d,%d\n", i, x, y, p[0], p[1], p[2], p[3]); }
    }
    unsigned char *bg = &px[((H / 2) * W + W / 2) * 4];   /* centre gap between squares */
    if (bg[0] || bg[1] || bg[2]) { fails++; fprintf(stderr, "background at centre: %d,%d,%d\n", bg[0], bg[1], bg[2]); }
    printf("G15_INST_RESULT %s %s squares_ok=%d background_ok=%d glerror=0x%x\n",
           fails || err ? "FAIL" : "PASS", mode, 4 - (fails - (bg[0] || bg[1] || bg[2])), !(bg[0] || bg[1] || bg[2]), err);
    return fails || err ? 1 : 0;
}
