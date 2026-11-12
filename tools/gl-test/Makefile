# GLES 3.0 test client for the G15 Mesa driver. Each mode draws a known scene
# and checks it against a CPU oracle (render-ref.h) or llvmpipe.
# Run with the G15 Mesa build (ASAHI_G15_GRAPHICS=1, LD_LIBRARY_PATH to its
# libs) or against llvmpipe (--driver software).
CC ?= cc
g15-gl-triangle: g15-gl-triangle.c render-ref.h
	$(CC) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -I. $(shell pkg-config --cflags libdrm) -o $@ g15-gl-triangle.c -lEGL -lGLESv2 -lm $(shell pkg-config --libs libdrm)
g15-gl-webrender: g15-gl-webrender.c
	$(CC) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra $(shell pkg-config --cflags libdrm) -o $@ g15-gl-webrender.c -lEGL -lGLESv2 $(shell pkg-config --libs libdrm)
clean:
	rm -f g15-gl-triangle g15-gl-webrender
