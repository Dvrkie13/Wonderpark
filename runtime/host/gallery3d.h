/* The Gallery's 3D view (gallery3d.cpp): the F1 notebook says what to show
 * and where; the renderer (gl_backend.c) asks for each frame's vertices. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
#include <vector>
struct G3Texture {
    int w = 0, h = 0, wrapS = 1, wrapT = 1; /* GX wrap: 0 clamp, 1 repeat, 2 mirror */
    std::vector<uint8_t> rgba;
};
extern "C" {
#else
typedef struct G3Texture G3Texture;
#endif

#define G3_ANIMS 4   /* Idle, Happy, Walk, Random */
#define G3_FLOATS 10 /* per vertex: clip x y z w, u v, r g b a (lit) */

typedef struct {
    int tex;          /* -1: none */
    int first, count; /* vertices (triangles) */
    int cull;         /* 0 none, 1 front, 2 back, 3 all */
    int xlu;          /* blended, drawn after the rest */
} G3Draw;

typedef struct {
    const float* verts;
    int nverts;
    const G3Draw* draws;
    int ndraws;
    const G3Texture* textures; /* C++ objects: read with gallery3d_texture */
    int ntextures;
    unsigned texGen;           /* changes when the textures do */
    int x, y, w, h;            /* the stage, window pixels, GL style (y from the bottom) */
} G3Frame;

/* The notebook: show this character's model (its internal name, as its
 * icon Ic<name>), animation anim (0..G3_ANIMS-1), in the window rectangle
 * x, y (from the top), w, h. Called when the viewer is drawn. */
void gallery3d_show(const char* character, int anim, float x, float y, float w, float h);
void gallery3d_hide(void);
/* 0 nothing, 1 loading, 2 ready, 3 failed (no model for it). */
int gallery3d_state(void);
int gallery3d_has_anim(int anim);
/* The mouse dragged across the stage by dx window pixels: turn it. */
void gallery3d_drag(float dx);
/* Scroll to zoom: steps up (positive) come closer, 0.6x to 1.8x. */
void gallery3d_zoom(float steps);
/* The renderer: this frame's model, posed and projected; 0 if none. */
int gallery3d_frame(G3Frame* out, int winW, int winH);
/* A texture's size, wrap modes and RGBA8 pixels (top row first). */
const uint8_t* gallery3d_texture(const G3Frame* f, int i, int* w, int* h, int* wrapS, int* wrapT);

#ifdef __cplusplus
}
#endif
