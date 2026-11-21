/* From MildRain479/PokePark-Wiicompiled recomp/gpu/gl_backend.c (CC0). */
/* OpenGL device for the GX decoder (gx_device.h).
 *
 * The EFB (the GPU's embedded framebuffer, 640x528) is an offscreen
 * framebuffer rendered at a multiple of the native resolution (the
 * settings' internal resolution, by default whatever fits the window),
 * wider again in 16:9, with multisample anti-aliasing if the
 * settings ask for it. TEV, the GPU's programmable color combiner, runs
 * in one "uber" fragment shader driven by uniforms, so any stage setup
 * draws without compiling a new shader. Copies of the EFB to the XFB are
 * presented in a window, or borderless fullscreen, at the shape they have
 * on a TV, with the settings menu over them while it is open;
 * RECOMP_GX_DUMP=<dir> also writes them as BMPs. gx_frames.c decides
 * what is drawn when; the window is window_win32.c's.
 *
 * Environment:
 *   RECOMP_GX_SCALE=n          internal resolution multiplier, over the settings
 *   RECOMP_GX_DUMP=<dir>       write presented frames as BMP files
 *   RECOMP_GX_DUMP_EVERY=n     ...every nth frame (default 30)
 *   RECOMP_GX_DUMP_FROM=n      ...from the nth on (default 0)
 *   RECOMP_GL_PRESENT=swapbuffers|dxgi  how frames are presented (default: DXGI on NVIDIA, SwapBuffers elsewhere; see "Presenting")
 *   RECOMP_GL_DEBUG=1          debug context, printing the driver's error messages */
#include <io.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h> /* the Pictures folder (Gallery > Save a picture) */
#include <GL/gl.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")


#include "gx_device.h"
#include "host_settings.h"
#include "wp_stats.h" /* Show FPS: last second's frame rate */
#include "gallery3d.h" /* the Gallery's 3D view */

/* ------------------------------------------------------------------------ */
/* GL 2+ entry points */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_LOWER_LEFT 0x8CA1
#define GL_ZERO_TO_ONE 0x935F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_MAX_SAMPLES 0x8D57
#define GL_BGRA 0x80E1

#define GL_FUNCS(X) \
    X(void, glGenBuffers, (GLsizei, GLuint*)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*)) \
    X(void, glCopyBufferSubData, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar*)) \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint*)) \
    X(void, glUniform4iv, (GLint, GLsizei, const GLint*)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glClipControl, (GLenum, GLenum)) \
    X(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))

/* Entry points that may be missing (GL 3.3/4.1/ARB): NULL if so, and what
 * needs them is left out. */
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
typedef struct __GLsync* GLsync;
typedef uint64_t GLuint64;
#define GL_OPT_FUNCS(X) \
    X(void, glGenQueries, (GLsizei, GLuint*)) \
    X(void, glBeginQuery, (GLenum, GLuint)) \
    X(void, glEndQuery, (GLenum)) \
    X(void, glGetQueryObjectiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64*)) \
    X(void, glGetProgramBinary, (GLuint, GLsizei, GLsizei*, GLenum*, void*)) \
    X(void, glProgramBinary, (GLuint, GLenum, const void*, GLsizei)) \
    X(void, glProgramParameteri, (GLuint, GLenum, GLint)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLsync, glFenceSync, (GLenum, GLbitfield)) \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, glDeleteSync, (GLsync))

#define DECLARE(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(DECLARE)
GL_OPT_FUNCS(DECLARE)
#undef DECLARE

static void gl_forget(void);
/* wglSwapIntervalEXT. Set to 0: presenting never waits for the display.
 * The game paces itself by its emulated retrace (and the frames between
 * by gx_frames.c), and in a window or borderless window the compositor
 * shows only whole frames anyway. Waiting held up the game thread: with
 * a display whose refresh stalls (a virtual display with nobody
 * watching), every present took 110 ms and the game ran at 7 fps. The
 * V-Sync setting (g_x[X_VSYNC], off by default) sets it to 1, for screens
 * where the picture tears (exclusive fullscreen): window_present. */
static BOOL(WINAPI* pSwapInterval)(int);

static void load_gl(void) {
#define LOAD(ret, name, args) \
    p_##name = (PFN_##name)wglGetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "gl: missing %s\n", #name); exit(1); }
    GL_FUNCS(LOAD)
#undef LOAD
#define LOAD_OPT(ret, name, args) p_##name = (PFN_##name)wglGetProcAddress(#name);
    GL_OPT_FUNCS(LOAD_OPT)
#undef LOAD_OPT
}

/* ------------------------------------------------------------------------ */
/* Shaders */

#define STR2(x) #x
#define STR(x) STR2(x)
#define GX_XF_WORDS_STR STR(GX_XF_WORDS)
#define GX_XF_MEM_WORDS_STR STR(GX_XF_MEM_WORDS)

/* The GPU's transform unit (XF), per vertex: position and normal
 * transform by the vertex's matrices, projection, the two color channels
 * with lighting, and texture coordinate generation. All state comes from
 * the XF memory and registers (gx.h), in a uniform buffer.
 *
 * Lighting follows the hardware model as Dolphin documents it: a lit
 * channel is material * clamp(ambient + sum of its lights), where each
 * light contributes color * attenuation * diffuse term; attenuation is
 * none, specular or spot, the diffuse term none, signed or clamped
 * N.L. */
/* XF memory access and the hardware's lighting (below), shared by the
 * vertex shader and, for per-pixel lighting, the fragment shader. */
#define GLSL_XF_LIGHTING \
    "uint xw(int a) { int i = a < 0x1000 ? a : a - 0x1000 + " GX_XF_MEM_WORDS_STR "; return xfm[i >> 2][i & 3]; }\n" \
    "float xfl(int a) { return uintBitsToFloat(xw(a)); }\n" \
    "vec4 row(int a) { return uintBitsToFloat(xfm[a >> 2]); }\n" /* rows of 4 start on multiples of 4 */ \
    "vec3 vec3at(int a) { return vec3(xfl(a), xfl(a + 1), xfl(a + 2)); }\n" \
    "vec4 unpack(uint c) { return vec4(float((c >> 24) & 255u), float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }\n" \
    "vec4 light(int j, uint ctrl, vec3 pos, vec3 n) {\n" \
    "  int b = 0x600 + j * 16;\n" \
    "  vec3 cosatt = vec3at(b + 4), distatt = vec3at(b + 7), lpos = vec3at(b + 10), ldirv = vec3at(b + 13);\n" \
    "  uint dfn = (ctrl >> 7) & 3u, afn = (ctrl >> 9) & 3u;\n" \
    "  vec3 ldir; float attn;\n" \
    "  if (afn == 1u) {\n" /* specular */ \
    "    ldir = normalize(lpos - pos);\n" \
    "    attn = dot(n, ldir) >= 0.0 ? max(0.0, dot(n, ldirv)) : 0.0;\n" \
    "    vec3 da = dfn == 0u ? distatt : normalize(distatt);\n" \
    "    attn = max(0.0, dot(cosatt, vec3(1.0, attn, attn * attn))) / dot(da, vec3(1.0, attn, attn * attn));\n" \
    "  } else if (afn == 3u) {\n" /* spot */ \
    "    vec3 d = lpos - pos; float dist2 = dot(d, d), dist = sqrt(dist2); ldir = d / dist;\n" \
    "    attn = max(0.0, dot(ldir, ldirv));\n" \
    "    attn = max(0.0, cosatt.x + cosatt.y * attn + cosatt.z * attn * attn) / dot(distatt, vec3(1.0, dist, dist2));\n" \
    "  } else {\n" /* none, directional */ \
    "    vec3 d = lpos - pos; ldir = length(d) == 0.0 ? n : normalize(d); attn = 1.0;\n" \
    "  }\n" \
    "  float diff = dfn == 0u ? 1.0 : (dfn == 1u ? dot(ldir, n) : max(0.0, dot(ldir, n)));\n" \
    "  return attn * diff * unpack(xw(b + 3));\n" \
    "}\n" \
    "vec4 lights(uint ctrl, vec3 pos, vec3 n) {\n" \
    "  uint mask = ((ctrl >> 2) & 15u) | (((ctrl >> 11) & 15u) << 4);\n" \
    "  vec4 sum = vec4(0.0);\n" \
    "  for (int j = 0; j < 8; j++) if ((mask & (1u << uint(j))) != 0u) sum += light(j, ctrl, pos, n);\n" \
    "  return sum;\n" \
    "}\n" \
    "vec4 channel(int i, vec4 vcol, vec3 pos, vec3 n) {\n" \
    "  uint cc = xw(0x100E + i), ac = xw(0x1010 + i);\n" \
    "  vec4 matr = unpack(xw(0x100C + i)), ambr = unpack(xw(0x100A + i)), r;\n" \
    "  vec3 mat = (cc & 1u) != 0u ? vcol.rgb : matr.rgb;\n" \
    "  if ((cc & 2u) != 0u) {\n" \
    "    vec3 amb = (cc & 64u) != 0u ? vcol.rgb : ambr.rgb;\n" \
    "    r.rgb = mat * clamp(amb + lights(cc, pos, n).rgb, 0.0, 1.0);\n" \
    "  } else r.rgb = mat;\n" \
    "  float mata = (ac & 1u) != 0u ? vcol.a : matr.a;\n" \
    "  if ((ac & 2u) != 0u) {\n" \
    "    float amba = (ac & 64u) != 0u ? vcol.a : ambr.a;\n" \
    "    r.a = mata * clamp(amba + lights(ac, pos, n).a, 0.0, 1.0);\n" \
    "  } else r.a = mata;\n" \
    "  return clamp(r, 0.0, 1.0);\n" \
    "}\n"

static const char* kVertexShader =
    "#version 330 compatibility\n"
    "layout(std140) uniform XF { uvec4 xfm[" GX_XF_WORDS_STR " / 4]; };\n"
    "in vec4 a_mtx0, a_mtx1, a_mtx2; in vec3 a_pos, a_nrm; in vec4 a_col0, a_col1;\n"
    "in vec4 a_tex01, a_tex23, a_tex45, a_tex67;\n"
    "uniform vec4 u_wide;\n"
    "out vec4 v_col0; out vec4 v_col1; out vec3 v_tex[8];\n"
    /* For per-pixel lighting: the position and normal (view space) and the
     * vertex colors, lit again for each pixel by the fragment shader. */
    "out vec3 v_world; out vec3 v_nrm; out vec4 v_vcol0; out vec4 v_vcol1;\n"
    GLSL_XF_LIGHTING
    "int texmtx(int i) { return int(i < 3 ? a_mtx0[i + 1] : i < 7 ? a_mtx1[i - 3] : a_mtx2.x); }\n"
    "vec2 texin(int i) { vec4 p = i < 2 ? a_tex01 : i < 4 ? a_tex23 : i < 6 ? a_tex45 : a_tex67; return (i & 1) == 0 ? p.xy : p.zw; }\n"
    "void main() {\n"
    "  int pn = int(a_mtx0.x);\n"
    "  vec4 in4 = vec4(a_pos, 1.0);\n"
    "  vec3 world = vec3(dot(row(pn * 4), in4), dot(row(pn * 4 + 4), in4), dot(row(pn * 4 + 8), in4));\n"
    "  int nb = 0x400 + (pn & 31) * 3;\n"
    "  vec3 n = vec3(dot(vec3at(nb), a_nrm), dot(vec3at(nb + 3), a_nrm), dot(vec3at(nb + 6), a_nrm));\n"
    "  if (dot(n, n) > 0.0) n = normalize(n);\n"
    "  float p0 = xfl(0x1020), p1 = xfl(0x1021), p2 = xfl(0x1022), p3 = xfl(0x1023), p4 = xfl(0x1024), p5 = xfl(0x1025);\n"
    "  vec4 clip = xw(0x1026) == 0u\n"
    "    ? vec4(p0 * world.x + p1 * world.z, p2 * world.y + p3 * world.z, p4 * world.z + p5, -world.z)\n"
    "    : vec4(p0 * world.x + p1, p2 * world.y + p3, p4 * world.z + p5, 1.0);\n"
    /* GX depth runs from -w (near) to 0 (far). The EFB depth is stored
     * reversed (see gxd_init), so the far plane, where GX scenes keep
     * nearly everything, gets the float precision: 0..w, far to near. */
    /* u_wide: the widescreen transform of clip x (gx_fifo.c wide_for_draw). */
    "  gl_Position = vec4(clip.x * u_wide.x + u_wide.y * clip.w, clip.y * (u_wide.z != 0.0 ? u_wide.z : 1.0), -clip.z, clip.w);\n"
    "  v_col0 = channel(0, a_col0, world, n);\n"
    "  v_col1 = channel(1, a_col1, world, n);\n"
    "  v_world = world; v_nrm = n; v_vcol0 = a_col0; v_vcol1 = a_col1;\n"
    "  int ntex = int(xw(0x103F) & 15u);\n"
    "  bool dual = (xw(0x1012) & 1u) != 0u;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    if (i >= ntex) { v_tex[i] = vec3(0.0, 0.0, 1.0); continue; }\n"
    "    uint info = xw(0x1040 + i);\n"
    "    int src_row = int((info >> 7) & 31u), type = int((info >> 4) & 7u);\n"
    "    vec4 src = vec4(0.0, 0.0, 1.0, 1.0);\n"
    "    if (src_row == 0) src = in4;\n"
    "    else if (src_row == 1) src.xyz = a_nrm;\n"
    "    else if (src_row >= 5 && src_row <= 12) src.xy = texin(src_row - 5);\n"
    "    vec3 stq;\n"
    "    if (type == 0) {\n"
    "      int m = texmtx(i) * 4;\n"
    "      stq = vec3(dot(row(m), src), dot(row(m + 4), src), ((info >> 1) & 1u) != 0u ? dot(row(m + 8), src) : 1.0);\n"
    "    } else stq = vec3(src.xy, 1.0);\n"
    "    if (dual) {\n" /* post-transform */
    "      uint pt = xw(0x1050 + i);\n"
    "      vec4 v = vec4(stq, 1.0);\n"
    "      if (((pt >> 8) & 1u) != 0u && dot(v.xyz, v.xyz) > 0.0) v.xyz = normalize(v.xyz);\n"
    "      int m = 0x500 + int(pt & 63u) * 4;\n"
    "      stq = vec3(dot(row(m), v), dot(row(m + 4), v), dot(row(m + 8), v));\n"
    "    }\n"
    "    v_tex[i] = stq;\n"
    "  }\n"
    "}\n";

/* TEV: each stage computes, per color channel and for alpha,
 *   d + lerp(a, b, c) (or d - lerp(...)), plus a bias, times a scale,
 * optionally clamped, into one of four registers; or, with bias 3, a
 * comparison of a and b selecting c or 0. Inputs are registers, the
 * texture, the rasterized color, constants and "konst" colors. */
/* Compiled as is, it is the ubershader: every setup from uniforms. With
 * SPEC defined and the S_ constants before it (spec_source), the setup
 * that decides the code's shape (stages, inputs, operations, swaps, alpha
 * test, fog kind, indirect commands) is constant: the stage loop unrolls
 * and the branches fold away. Values that change often (colors, konst,
 * fog distances, texture sizes, indirect matrices) stay uniforms. */
static const char* kFsVersion = "#version 330 compatibility\n";
static const char* kFragmentShader =
    "in vec4 v_col0; in vec4 v_col1; in vec3 v_tex[8];\n"
    "in vec3 v_world; in vec3 v_nrm; in vec4 v_vcol0; in vec4 v_vcol1;\n"
    "layout(std140) uniform XF { uvec4 xfm[" GX_XF_WORDS_STR " / 4]; };\n"
    "uniform ivec4 u_ppl; /* x: per-pixel lighting on */\n"
    GLSL_XF_LIGHTING
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3, u_tex4, u_tex5, u_tex6, u_tex7;\n"
    "uniform int u_num_stages;\n"
    "uniform ivec4 u_cin[16], u_ain[16], u_cop[16], u_aop[16], u_map[16], u_sel[16];\n"
    "uniform vec4 u_reg[4], u_konst[4];\n"
    "uniform ivec4 u_swap[4];\n"
    "uniform ivec4 u_alpha;\n"
    "uniform vec4 u_alpha_ref;\n"
    "uniform ivec4 u_fog; /* kind, orthographic, b's magnitude and shift (gx.h) */\n"
    "uniform vec4 u_fogf; /* a, c */\n"
    "uniform vec4 u_fog_color;\n"
    "uniform vec4 u_fog_range[4]; /* the offset from the center as x a + b, on; ten cosines */\n"
    "/* Indirect texturing (gx.h): the indirect stages and whether any stage is\n"
    " * indirect; each stage's command; the indirect stages' maps, coordinates\n"
    " * and scales; the matrices' rows, each with its shift; the coordinates'\n"
    " * sizes, two to a vector. */\n"
    "uniform ivec4 u_ind, u_ind_cmd[4], u_ind_stage[4], u_ind_mtx[6], u_tc_size[4];\n"
    "#ifdef SPEC\n"
    "#define u_num_stages S_N\n"
    "#define u_cin S_CIN\n"
    "#define u_ain S_AIN\n"
    "#define u_cop S_COP\n"
    "#define u_aop S_AOP\n"
    "#define u_map S_MAP\n"
    "#define u_sel S_SEL\n"
    "#define u_swap S_SWAP\n"
    "#define u_alpha S_ALPHA\n"
    "#define u_ind S_IND\n"
    "#define u_ind_cmd S_IND_CMD\n"
    "#define u_ind_stage S_IND_STAGE\n"
    "#define FOG_KIND S_FOG.x\n"
    "#define FOG_ORTHO S_FOG.y\n"
    "#else\n"
    "#define FOG_KIND u_fog.x\n"
    "#define FOG_ORTHO u_fog.y\n"
    "#endif\n"
    "vec4 sample_map(int m, vec2 uv) {\n"
    "  if (m == 0) return texture(u_tex0, uv); if (m == 1) return texture(u_tex1, uv);\n"
    "  if (m == 2) return texture(u_tex2, uv); if (m == 3) return texture(u_tex3, uv);\n"
    "  if (m == 4) return texture(u_tex4, uv); if (m == 5) return texture(u_tex5, uv);\n"
    "  if (m == 6) return texture(u_tex6, uv); return texture(u_tex7, uv);\n"
    "}\n"
    "ivec2 map_size(int m) {\n"
    "  if (m == 0) return textureSize(u_tex0, 0); if (m == 1) return textureSize(u_tex1, 0);\n"
    "  if (m == 2) return textureSize(u_tex2, 0); if (m == 3) return textureSize(u_tex3, 0);\n"
    "  if (m == 4) return textureSize(u_tex4, 0); if (m == 5) return textureSize(u_tex5, 0);\n"
    "  if (m == 6) return textureSize(u_tex6, 0); return textureSize(u_tex7, 0);\n"
    "}\n"
    "/* A map sampled at a coordinate in texels with 7 fractional bits. */\n"
    "vec4 sample_at(int m, ivec2 c) { return sample_map(m, vec2(c) / (vec2(map_size(m)) * 128.0)); }\n"
    "vec3 texc(int n) {\n"
    "  if (n < 4) return n == 0 ? v_tex[0] : (n == 1 ? v_tex[1] : (n == 2 ? v_tex[2] : v_tex[3]));\n"
    "  return n == 4 ? v_tex[4] : (n == 5 ? v_tex[5] : (n == 6 ? v_tex[6] : v_tex[7]));\n"
    "}\n"
    "ivec2 fixpoint_uv(int n) {\n"
    "  vec3 tc = texc(n); ivec4 sz = u_tc_size[n >> 1];\n"
    "  return ivec2(tc.xy / (tc.z == 0.0 ? 1.0 : tc.z) * vec2((n & 1) != 0 ? sz.zw : sz.xy) * 128.0);\n"
    "}\n"
    "int wrap(int v, int w) { return w == 0 ? v : (w == 6 ? 0 : v & ((256 >> (w - 1)) * 128 - 1)); }\n"
    /* No local array or vector is indexed with a value known only when the
     * shader runs: a GPU keeps such arrays in memory rather than registers
     * (scratch), for every pixel. Selects and branches on the draw's own
     * (uniform) setup instead. */
    "float comp(vec4 c, int i) { return i == 0 ? c.x : (i == 1 ? c.y : (i == 2 ? c.z : c.w)); }\n"
    "vec4 swizzle(vec4 c, int t) { ivec4 s = u_swap[t]; return vec4(comp(c, s.x), comp(c, s.y), comp(c, s.z), comp(c, s.w)); }\n"
    "vec4 regsel(int r, vec4 r0, vec4 r1, vec4 r2, vec4 r3) { return r == 0 ? r0 : (r == 1 ? r1 : (r == 2 ? r2 : r3)); }\n"
    "vec3 csel(int s, vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec4 tex, vec4 ras, vec3 kc) {\n"
    "  if (s < 8) { vec4 r = regsel(s >> 1, r0, r1, r2, r3); return (s & 1) != 0 ? vec3(r.a) : r.rgb; }\n"
    "  if (s < 10) return s == 8 ? tex.rgb : vec3(tex.a);\n"
    "  if (s < 12) return s == 10 ? ras.rgb : vec3(ras.a);\n"
    "  return s == 12 ? vec3(1.0) : (s == 13 ? vec3(0.5) : (s == 14 ? kc : vec3(0.0)));\n"
    "}\n"
    "float asel(int s, vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec4 tex, vec4 ras, float ka) {\n"
    "  if (s < 4) return regsel(s, r0, r1, r2, r3).a;\n"
    "  return s == 4 ? tex.a : (s == 5 ? ras.a : (s == 6 ? ka : 0.0));\n"
    "}\n"
    "float kfrac(int s) { return s < 8 ? float(8 - s) / 8.0 : 0.0; }\n"
    "vec3 kcolor(int s) {\n"
    "  if (s < 8) return vec3(kfrac(s));\n"
    "  if (s >= 12 && s < 16) return u_konst[s - 12].rgb;\n"
    "  if (s >= 16) { int k = (s - 16) & 3; int c = (s - 16) >> 2; return vec3(u_konst[k][c]); }\n"
    "  return vec3(0.0);\n"
    "}\n"
    "float kalpha(int s) {\n"
    "  if (s < 8) return kfrac(s);\n"
    "  if (s >= 16) { int k = (s - 16) & 3; int c = (s - 16) >> 2; return u_konst[k][c]; }\n"
    "  return 0.0;\n"
    "}\n"
    "bool acmp(int f, float a, float r) {\n"
    "  if (f == 0) return false; if (f == 1) return a < r; if (f == 2) return a == r;\n"
    "  if (f == 3) return a <= r; if (f == 4) return a > r; if (f == 5) return a != r;\n"
    "  if (f == 6) return a >= r; return true;\n"
    "}\n"
    "float q8(float x) { return floor(x * 255.0 + 0.5) / 255.0; }\n"
    "vec3 q8(vec3 x) { return floor(x * 255.0 + 0.5) / 255.0; }\n"
    "void main() {\n"
    "  vec4 r0 = u_reg[0], r1 = u_reg[1], r2 = u_reg[2], r3 = u_reg[3];\n"
    "  vec4 last = r0;\n"
    /* Per-pixel lighting: the two color channels lit here, for each pixel,
     * from the interpolated position and normal; the vertices' own when
     * off (or for flat, unlit draws, which come out the same). */
    "  vec4 col0 = v_col0, col1 = v_col1;\n"
    "  if (u_ppl.x != 0) {\n"
    "    vec3 n = dot(v_nrm, v_nrm) > 0.0 ? normalize(v_nrm) : v_nrm;\n"
    "    col0 = channel(0, v_vcol0, v_world, n);\n"
    "    col1 = channel(1, v_vcol1, v_world, n);\n"
    "  }\n"
    "  ivec3 it0 = ivec3(0), it1 = ivec3(0), it2 = ivec3(0), it3 = ivec3(0); ivec2 ind_coord = ivec2(0);\n"
    "  if (u_ind.y != 0) {\n"
    "    for (int k = 0; k < u_ind.x && k < 4; k++) {\n"
    "      ivec4 is = u_ind_stage[k];\n"
    "      ivec3 t = ivec3(floor(sample_at(is.x, fixpoint_uv(is.y) >> is.zw).abg * 255.0 + 0.5));\n"
    "      if (k == 0) it0 = t; else if (k == 1) it1 = t; else if (k == 2) it2 = t; else it3 = t;\n"
    "    }\n"
    "  }\n"
    "  for (int i = 0; i < u_num_stages; i++) {\n"
    "    ivec4 map = u_map[i]; ivec4 sel = u_sel[i];\n"
    "    int cmd = u_ind.y != 0 ? u_ind_cmd[i >> 2][i & 3] : 0, bump = 0;\n"
    "    if (cmd != 0) {\n"
    "      /* As gx.frag's indirect: the indirect stage's texel offsets the coordinate. */\n"
    "      int bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;\n"
    "      ivec2 uv = fixpoint_uv(map.y), trans = ivec2(0);\n"
    "      if (bt < u_ind.x && (bs != 0 || mid != 0)) {\n"
    "        ivec3 crd = bt == 0 ? it0 : (bt == 1 ? it1 : (bt == 2 ? it2 : it3));\n"
    "        if (bs != 0) bump = (bs == 1 ? crd.x : (bs == 2 ? crd.y : crd.z)) & (fmt == 1 ? 0xE0 : (fmt == 2 ? 0xF0 : 0xF8));\n"
    "        crd &= ivec3(fmt == 0 ? 255 : (fmt == 1 ? 31 : (fmt == 2 ? 15 : 7)));\n"
    "        int add = fmt == 0 ? -128 : 1;\n"
    "        crd += ivec3((bias & 1) != 0 ? add : 0, (bias & 2) != 0 ? add : 0, (bias & 4) != 0 ? add : 0);\n"
    "        int m = (mid & 3) - 1;\n"
    "        if (m >= 0) {\n"
    "          ivec4 a = u_ind_mtx[2 * m], b = u_ind_mtx[2 * m + 1];\n"
    "          if ((mid >> 2) == 0) trans = ivec2(a.x * crd.x + a.y * crd.y + a.z * crd.z, b.x * crd.x + b.y * crd.y + b.z * crd.z) >> 3;\n"
    "          else if ((mid >> 2) == 1) trans = (uv * crd.x) >> 8;\n"
    "          else trans = (uv * crd.y) >> 8;\n"
    "          trans = a.w >= 0 ? trans >> a.w : trans << -a.w;\n"
    "        }\n"
    "      }\n"
    "      ivec2 c = ivec2(wrap(uv.x, (cmd >> 13) & 7), wrap(uv.y, (cmd >> 16) & 7)) + trans;\n"
    "      ind_coord = ((cmd >> 20) & 1) != 0 ? ind_coord + c : c;\n"
    "      ind_coord = (ind_coord << 8) >> 8;\n"
    "    }\n"
    "    vec4 tex = vec4(1.0);\n"
    "    if (map.z != 0) {\n"
    "      if (cmd != 0) tex = sample_at(map.x, ind_coord);\n"
    "      else { vec3 tc = texc(map.y); tex = sample_map(map.x, tc.xy / (tc.z == 0.0 ? 1.0 : tc.z)); }\n"
    "    }\n"
    "    tex = swizzle(tex, sel.w & 3);\n"
    "    vec4 ras = map.w == 0 ? col0 : (map.w == 1 ? col1 : vec4(0.0));\n"
    "    if (map.w == 5) ras = vec4(0.0, 0.0, 0.0, float(bump) / 255.0);\n"
    "    else if (map.w == 6) ras = vec4(0.0, 0.0, 0.0, float(bump | (bump >> 5)) / 255.0);\n"
    "    ras = swizzle(ras, sel.w >> 2);\n"
    "    vec3 kc = kcolor(sel.z & 31); float ka = kalpha(sel.z >> 5);\n"
    "    ivec4 ci = u_cin[i]; ivec4 co = u_cop[i];\n"
    "    vec3 a = clamp(csel(ci.x, r0, r1, r2, r3, tex, ras, kc), 0.0, 1.0);\n"
    "    vec3 b = clamp(csel(ci.y, r0, r1, r2, r3, tex, ras, kc), 0.0, 1.0);\n"
    "    vec3 c = clamp(csel(ci.z, r0, r1, r2, r3, tex, ras, kc), 0.0, 1.0);\n"
    "    vec3 d = csel(ci.w, r0, r1, r2, r3, tex, ras, kc);\n"
    "    vec3 rc;\n"
    "    if (co.y == 3) {\n"
    "      vec3 r;\n"
    "      if (co.z == 3) r = vec3(co.x == 0 ? greaterThan(q8(a), q8(b)) : equal(q8(a), q8(b)));\n"
    "      else { float av = dot(q8(a), vec3(65536.0, 256.0, 1.0)), bv = dot(q8(b), vec3(65536.0, 256.0, 1.0));\n"
    "        if (co.z == 0) { av = q8(a.r); bv = q8(b.r); }\n"
    "        else if (co.z == 1) { av = q8(a.r) + q8(a.g) * 256.0; bv = q8(b.r) + q8(b.g) * 256.0; }\n"
    "        else { av = q8(a.r) + q8(a.g) * 256.0 + q8(a.b) * 65536.0; bv = q8(b.r) + q8(b.g) * 256.0 + q8(b.b) * 65536.0; }\n"
    "        r = vec3(co.x == 0 ? float(av > bv) : float(av == bv)); }\n"
    "      rc = d + r * c;\n"
    "    } else {\n"
    "      vec3 l = mix(a, b, c);\n"
    "      rc = co.x == 0 ? d + l : d - l;\n"
    "      rc += co.y == 1 ? 0.5 : (co.y == 2 ? -0.5 : 0.0);\n"
    "      rc *= co.z == 1 ? 2.0 : (co.z == 2 ? 4.0 : (co.z == 3 ? 0.5 : 1.0));\n"
    "    }\n"
    "    rc = co.w != 0 ? clamp(rc, 0.0, 1.0) : clamp(rc, -1024.0 / 255.0, 1023.0 / 255.0);\n"
    "    ivec4 ai = u_ain[i]; ivec4 ao = u_aop[i];\n"
    "    float aa = clamp(asel(ai.x, r0, r1, r2, r3, tex, ras, ka), 0.0, 1.0);\n"
    "    float ab = clamp(asel(ai.y, r0, r1, r2, r3, tex, ras, ka), 0.0, 1.0);\n"
    "    float ac = clamp(asel(ai.z, r0, r1, r2, r3, tex, ras, ka), 0.0, 1.0);\n"
    "    float ad = asel(ai.w, r0, r1, r2, r3, tex, ras, ka);\n"
    "    float ra;\n"
    "    if (ao.y == 3) {\n"
    "      float r = ao.x == 0 ? float(q8(aa) > q8(ab)) : float(q8(aa) == q8(ab));\n"
    "      ra = ad + r * ac;\n"
    "    } else {\n"
    "      float l = mix(aa, ab, ac);\n"
    "      ra = ao.x == 0 ? ad + l : ad - l;\n"
    "      ra += ao.y == 1 ? 0.5 : (ao.y == 2 ? -0.5 : 0.0);\n"
    "      ra *= ao.z == 1 ? 2.0 : (ao.z == 2 ? 4.0 : (ao.z == 3 ? 0.5 : 1.0));\n"
    "    }\n"
    "    ra = ao.w != 0 ? clamp(ra, 0.0, 1.0) : clamp(ra, -1024.0 / 255.0, 1023.0 / 255.0);\n"
    "    if (sel.x == 0) r0.rgb = rc; else if (sel.x == 1) r1.rgb = rc; else if (sel.x == 2) r2.rgb = rc; else r3.rgb = rc;\n"
    "    if (sel.y == 0) r0.a = ra; else if (sel.y == 1) r1.a = ra; else if (sel.y == 2) r2.a = ra; else r3.a = ra;\n"
    "    last = vec4(rc, ra);\n"
    "  }\n"
    "  last = clamp(last, 0.0, 1.0);\n"
    "  float a8 = floor(last.a * 255.0 + 0.5);\n"
    "  bool p0 = acmp(u_alpha.x, a8, u_alpha_ref.x), p1 = acmp(u_alpha.y, a8, u_alpha_ref.y);\n"
    "  bool pass = u_alpha.z == 0 ? (p0 && p1) : (u_alpha.z == 1 ? (p0 || p1) : (u_alpha.z == 2 ? (p0 != p1) : (p0 == p1)));\n"
    "  if (!pass) discard;\n"
    "  if (FOG_KIND != 0) {\n"
    "    int z = int(clamp((1.0 - gl_FragCoord.z) * 16777216.0, 0.0, 16777215.0));\n"
    "    float ze = FOG_ORTHO == 0 ? u_fogf.x * 16777216.0 / float(u_fog.z - (z >> u_fog.w)) : u_fogf.x * float(z) / 16777216.0;\n"
    "    if (u_fog_range[0].z != 0.0) {\n"
    "      float t = clamp(abs(gl_FragCoord.x * u_fog_range[0].x + u_fog_range[0].y) / 32.0 - 1.0, -1.0, 9.0);\n"
    "      int lo = int(floor(t)), hi = min(lo + 1, 9);\n"
    "      float kl = lo < 0 ? 1.0 : u_fog_range[(lo + 4) >> 2][lo & 3];\n"
    "      float kh = u_fog_range[(hi + 4) >> 2][hi & 3];\n"
    "      ze /= max(mix(kl, kh, t - float(lo)), 1.0 / 256.0);\n"
    "    }\n"
    "    float f = clamp(ze - u_fogf.y, 0.0, 1.0);\n"
    "    if (FOG_KIND == 4) f = 1.0 - exp2(-8.0 * f);\n"
    "    else if (FOG_KIND == 5) f = 1.0 - exp2(-8.0 * f * f);\n"
    "    else if (FOG_KIND == 6) f = exp2(-8.0 * (1.0 - f));\n"
    "    else if (FOG_KIND == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"
    "    last.rgb = mix(last.rgb, u_fog_color.rgb, floor(f * 256.0 + 0.5) / 256.0);\n"
    "  }\n"
    "  o_color = last;\n"
    "}\n";

/* A draw program (the ubershader, or one specialised for a TEV setup):
 * where its uniforms are (-1 for those a specialised one has made
 * constant) and the values last sent to each, so a draw sends only what
 * changed for the program it uses. */
typedef struct DrawProg {
    GLuint id;
    GLint uNumStages, uCin, uAin, uCop, uAop, uMap, uSel, uReg, uKonst, uSwap, uAlpha, uAlphaRef, uFog, uFogF,
        uFogColor, uInd, uIndCmd, uIndStage, uIndMtx, uTcSize, uFogRange, uWide, uPpl;
    GLfloat lWide[4];
    GLint lCin[64], lAin[64], lCop[64], lAop[64], lMap[64], lSel[64], lSwap[16], lAlpha[4], lFog[4];
    GLfloat lReg[16], lKonst[16], lAlphaRef[4], lFogF[4], lFogColor[4], lFogRange[16];
    GLint lInd[4], lIndCmd[16], lIndStage[16], lIndMtx[24], lTcSize[16], lPpl[4];
    int set, stages;
} DrawProg;

static DrawProg sUber;
static GLuint sProgram; /* the draw program in use */
static GLuint sVs;      /* the vertex shader, shared by all draw programs */
static GLuint sVao, sVbo, sIbo, sXfUbo;
static GLuint sArenaVao, sArenaVbo; /* the vertex arena (gl_arena_alloc) */
static GLuint sBoundVao; /* the vertex array bound: the per-draw one or the arena's */

/* Compiles a shader from n strings; 0 (and the log printed) if it fails. */
static GLuint compile_n(GLenum type, int n, const char* const* src) {
    extern unsigned g_cnt_shader_compiles; /* debug overlay */
    GLuint s = p_glCreateShader(type);
    GLint ok;
    g_cnt_shader_compiles++;
    p_glShaderSource(s, n, src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gl: shader error:\n%s\n", log);
        return 0;
    }
    return s;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = compile_n(type, 1, &src);
    if (!s) exit(1);
    return s;
}

static const char* const kAttribs[] = {"a_mtx0", "a_mtx1", "a_mtx2", "a_pos", "a_nrm", "a_col0",
                                       "a_col1", "a_tex01", "a_tex23", "a_tex45", "a_tex67"};

/* With the program in use: its samplers and XF block bound, and where its
 * uniforms are. Called on whichever context made it. */
static void prog_locate(DrawProg* p) {
    int i;
    GLuint block;
    for (i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u_tex%d", i);
        p_glUniform1i(p_glGetUniformLocation(p->id, name), i);
    }
#define LOC(field, name) p->field = p_glGetUniformLocation(p->id, name)
    LOC(uNumStages, "u_num_stages");
    LOC(uCin, "u_cin");
    LOC(uAin, "u_ain");
    LOC(uCop, "u_cop");
    LOC(uAop, "u_aop");
    LOC(uMap, "u_map");
    LOC(uSel, "u_sel");
    LOC(uReg, "u_reg");
    LOC(uKonst, "u_konst");
    LOC(uSwap, "u_swap");
    LOC(uAlpha, "u_alpha");
    LOC(uAlphaRef, "u_alpha_ref");
    LOC(uFog, "u_fog");
    LOC(uFogF, "u_fogf");
    LOC(uFogColor, "u_fog_color");
    LOC(uInd, "u_ind");
    LOC(uIndCmd, "u_ind_cmd");
    LOC(uIndStage, "u_ind_stage");
    LOC(uIndMtx, "u_ind_mtx");
    LOC(uTcSize, "u_tc_size");
    LOC(uFogRange, "u_fog_range");
    LOC(uWide, "u_wide");
    LOC(uPpl, "u_ppl");
#undef LOC
    block = p_glGetUniformBlockIndex(p->id, "XF");
    if (block != 0xFFFFFFFFu) p_glUniformBlockBinding(p->id, block, 0);
    p->set = 0;
}

/* Links the shared vertex shader with fragment shader fs; 0 if it fails. */
static GLuint link_draw_program(GLuint fs, int retrievable) {
    GLuint p = p_glCreateProgram();
    GLint ok;
    int i;
    p_glAttachShader(p, sVs);
    p_glAttachShader(p, fs);
    for (i = 0; i < 11; i++) p_glBindAttribLocation(p, (GLuint)i, kAttribs[i]);
    if (retrievable && p_glProgramParameteri) p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, 1);
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        if (p_glDeleteProgram) p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Points the arena's vertex array at buffer b. */
static void arena_vao(GLuint b) {
    int i;
    p_glBindVertexArray(sArenaVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, b);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIbo);
    for (i = 0; i < 3; i++) {
        p_glEnableVertexAttribArray(i);
        p_glVertexAttribPointer(i, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, mtx) + i * 4));
    }
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, pos));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, nrm));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[0]));
    p_glEnableVertexAttribArray(6);
    p_glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[1]));
    for (i = 0; i < 4; i++) {
        p_glEnableVertexAttribArray(7 + i);
        p_glVertexAttribPointer(7 + i, 4, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, tex) + i * 4 * sizeof(float)));
    }
    p_glBindVertexArray(sVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
}

static int sSimpleFs; /* RECOMP_GL_SIMPLE_FS */

static void build_program(void) {
    GLuint fs;
    int i;
    const char* e = getenv("RECOMP_GL_SIMPLE_FS");
    sVs = compile(GL_VERTEX_SHADER, kVertexShader);
    if (e && *e == '1') {
        /* RECOMP_GL_SIMPLE_FS=1 (measurements only; the picture is wrong):
         * a trivial fragment shader, to see what the TEV shader costs. */
        static const char* kSimple =
            "#version 330 compatibility\n"
            "in vec4 v_col0; in vec4 v_col1; in vec3 v_tex[8]; out vec4 o_color; uniform sampler2D u_tex0;\n"
            "void main() { vec3 t = v_tex[0]; o_color = texture(u_tex0, t.xy / (t.z == 0.0 ? 1.0 : t.z)) * v_col0; }\n";
        fs = compile(GL_FRAGMENT_SHADER, kSimple);
        sSimpleFs = 1;
    } else {
        const char* src[2] = {kFsVersion, kFragmentShader};
        if (!(fs = compile_n(GL_FRAGMENT_SHADER, 2, src))) exit(1);
    }
    if (!(sUber.id = link_draw_program(fs, 0))) exit(1);
    sProgram = sUber.id;
    p_glUseProgram(sProgram);
    prog_locate(&sUber);

    p_glGenVertexArrays(1, &sVao);
    p_glBindVertexArray(sVao);
    p_glGenBuffers(1, &sVbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    p_glGenBuffers(1, &sIbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIbo); /* part of the vertex array's state */
    for (i = 0; i < 3; i++) {
        p_glEnableVertexAttribArray(i);
        p_glVertexAttribPointer(i, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, mtx) + i * 4));
    }
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, pos));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, nrm));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[0]));
    p_glEnableVertexAttribArray(6);
    p_glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[1]));
    for (i = 0; i < 4; i++) {
        p_glEnableVertexAttribArray(7 + i);
        p_glVertexAttribPointer(7 + i, 4, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, tex) + i * 4 * sizeof(float)));
    }

    /* The same layout over the vertex arena (gl_arena_alloc). */
    p_glGenVertexArrays(1, &sArenaVao);
    p_glGenBuffers(1, &sArenaVbo);
    arena_vao(sArenaVbo);
    p_glBindVertexArray(sVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);

    /* The XF state, for the vertex shader. */
    p_glGenBuffers(1, &sXfUbo);
    p_glBindBuffer(GL_UNIFORM_BUFFER, sXfUbo);
    p_glBufferData(GL_UNIFORM_BUFFER, GX_XF_WORDS * 4, NULL, GL_DYNAMIC_DRAW);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, sXfUbo);
}

/* ------------------------------------------------------------------------ */
/* Specialised draw programs (off by default: WONDERPARK_SHADERS=spec)
 *
 * Each TEV setup a draw uses (its "key": the stages' inputs, operations
 * and destinations, swaps, alpha test, fog kind and indirect commands) can
 * get a program of its own, the ubershader with the key as constants.
 * Until it is ready the ubershader draws it, so a new setup never waits
 * for a compile.
 *
 * Why it is off: on AMD's OpenGL driver (RX 9060 XT, 26.10) the first draw
 * with any new program blocks the calling thread for 50-300 ms (the driver
 * compiles the GPU code then), however it was made: compiled in parallel
 * by the driver, made from a program binary, or compiled and drawn once on
 * a second context (which blocks the main context meanwhile). So it can't
 * be done without stutter there. The ubershader without indexed local
 * arrays (kFragmentShader) is fast enough on its own: Wish Park at 4x takes
 * 12 ms of GPU time a frame (63 ms before; 5 ms with these).
 *
 * Compiling never holds up the game thread:
 * - With KHR/ARB_parallel_shader_compile (AMD, NVIDIA, Mesa), the main
 *   thread hands new programs to the driver, which compiles them on
 *   threads of its own, and only asks whether they are done. At most
 *   SPEC_IN_FLIGHT at a time: with dozens queued, the first draw with a
 *   finished one waited behind them inside the driver (AMD, up to 850 ms).
 * - Without it, a worker thread compiles them on a GL context of its own
 *   that shares objects with the main one. (Not where the extension is: on
 *   AMD, a compile there blocked the main context's calls for up to 900 ms.)
 *
 * The keys seen are kept in <data dir>/shadercache/gl.bin (by default
 * %APPDATA%\Wonderpark\shadercache), and at start all of them are compiled
 * again in the background, so a setup seen before is specialised within the
 * first seconds. Not the driver's program binaries: on AMD, the first draw
 * with a program made from one took ~300 ms. A worker thread reads and
 * writes the file, so no disk access happens on the game thread. The file
 * starts over when the shader source changes. WONDERPARK_SHADERS=spec
 * turns it on; =thread uses the worker context even where the extension
 * is. */

#define SPEC_MAX_KEY (1 + 16 * 24 + 16 + 3 + 4 + 16 + 16)
#define SPEC_SLOTS 8192
#define SPEC_IN_FLIGHT 2
#define GL_COMPLETION_STATUS 0x91B1
enum { SPEC_WAITING, SPEC_READY, SPEC_FAILED };

typedef struct SpecEntry {
    uint64_t hash;
    int len;
    GLint* key;
    volatile LONG state;
    DrawProg prog;
    char* src;     /* the key as GLSL constants, until compiled */
    GLuint fs;     /* its fragment shader, while compiling */
    int saved;     /* in the file already */
    struct SpecEntry* next;  /* the main thread's lists */
    struct SpecEntry* qnext; /* the worker's queue */
} SpecEntry;

static int sSpecOn, sSpecParallel;
/* The main thread's: the table, setups waiting to be handed to the driver
 * and those it is compiling (parallel mode). */
static SpecEntry* sSpecTable[SPEC_SLOTS];
static int sSpecCount, sSpecInFlight;
static SpecEntry *sSpecNew, *sSpecNewTail, *sSpecCompiling;
/* Shared with the worker, under sSpecLock: its queue (keys to write, and
 * in worker-context mode setups to compile) and the keys read from the
 * file. */
static CRITICAL_SECTION sSpecLock;
static CONDITION_VARIABLE sSpecWake;
static SpecEntry *sSpecHead, *sSpecTail, *sSpecLoaded;
static HDC sSpecDC;
static HGLRC sSpecRC;
static FILE* sSpecFile;
static uint64_t sSpecStamp; /* the shader source */
/* Draws by a specialised program and by the ubershader, programs compiled,
 * keys read from the file, and setups waiting, for the RECOMP_FPS report. */
unsigned g_spec_draws, g_uber_draws, g_spec_compiled, g_spec_loaded, g_spec_waiting;

static uint64_t spec_hash(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* The key as GLSL constants (kFragmentShader's SPEC block). */
static char* spec_source(const GLint* k) {
    static const char* const kArr[6] = {"S_CIN", "S_AIN", "S_COP", "S_AOP", "S_MAP", "S_SEL"};
    size_t cap = 16384, at = 0;
    char* s = (char*)malloc(cap);
    int n = k[0], size = n > 0 ? n : 1, a, i, j;
    const GLint* rest = k + 1 + 24 * n;
#define PUT(...) (at += (size_t)snprintf(s + at, cap - at, __VA_ARGS__))
    PUT("#define SPEC 1\nconst int S_N = %d;\n", n);
    for (a = 0; a < 6; a++) {
        PUT("const ivec4 %s[%d] = ivec4[%d](", kArr[a], size, size);
        for (i = 0; i < size; i++) {
            const GLint* v = i < n ? k + 1 + 24 * i + 4 * a : NULL;
            PUT("%sivec4(%d,%d,%d,%d)", i ? "," : "", v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 0);
        }
        PUT(");\n");
    }
    PUT("const ivec4 S_SWAP[4] = ivec4[4](");
    for (i = 0; i < 4; i++) PUT("%sivec4(%d,%d,%d,%d)", i ? "," : "", rest[4 * i], rest[4 * i + 1], rest[4 * i + 2], rest[4 * i + 3]);
    rest += 16;
    PUT(");\nconst ivec4 S_ALPHA = ivec4(%d,%d,%d,0);\n", rest[0], rest[1], rest[2]);
    rest += 3;
    PUT("const ivec4 S_FOG = ivec4(%d,%d,0,0);\nconst ivec4 S_IND = ivec4(%d,%d,0,0);\n", rest[0], rest[1], rest[2], rest[3]);
    {
        int ind = rest[3];
        rest += 4;
        for (j = 0; j < 2; j++) {
            PUT("const ivec4 %s[4] = ivec4[4](", j ? "S_IND_STAGE" : "S_IND_CMD");
            for (i = 0; i < 4; i++) {
                const GLint* v = ind ? rest + 16 * j + 4 * i : NULL;
                PUT("%sivec4(%d,%d,%d,%d)", i ? "," : "", v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 0);
            }
            PUT(");\n");
        }
    }
#undef PUT
    return s;
}

/* To the worker's queue. */
static void spec_post(SpecEntry* e) {
    EnterCriticalSection(&sSpecLock);
    e->qnext = NULL;
    if (sSpecTail) sSpecTail->qnext = e;
    else sSpecHead = e;
    sSpecTail = e;
    LeaveCriticalSection(&sSpecLock);
    WakeConditionVariable(&sSpecWake);
}

static SpecEntry** spec_slot(const GLint* k, int n, uint64_t h) {
    unsigned i = (unsigned)h & (SPEC_SLOTS - 1);
    while (sSpecTable[i]) {
        SpecEntry* e = sSpecTable[i];
        if (e->hash == h && e->len == n && memcmp(e->key, k, (size_t)n * sizeof(GLint)) == 0) return &sSpecTable[i];
        i = (i + 1) & (SPEC_SLOTS - 1);
    }
    return &sSpecTable[i];
}

static SpecEntry* spec_new(const GLint* k, int n) {
    SpecEntry* e = (SpecEntry*)calloc(1, sizeof(SpecEntry));
    e->hash = spec_hash(k, (size_t)n * sizeof(GLint), 0xCBF29CE484222325ull);
    e->len = n;
    e->key = (GLint*)malloc((size_t)n * sizeof(GLint));
    memcpy(e->key, k, (size_t)n * sizeof(GLint));
    return e;
}

/* A new setup (key k, n ints) into the table and on its way to being
 * compiled (and written to the file, unless it came from there). */
static SpecEntry* spec_add(SpecEntry** slot, SpecEntry* e) {
    e->src = spec_source(e->key);
    *slot = e;
    sSpecCount++;
    g_spec_waiting++;
    if (sSpecParallel) {
        e->next = NULL;
        if (sSpecNewTail) sSpecNewTail->next = e;
        else sSpecNew = e;
        sSpecNewTail = e;
        if (!e->saved) spec_post(e); /* only to write the key */
    } else {
        spec_post(e); /* to compile, and write the key */
    }
    return e;
}

/* The program for key k (n ints): a specialised one if it is ready, else
 * NULL (the ubershader draws); a key not seen before is queued. */
static DrawProg* spec_get(const GLint* k, int n) {
    static GLint last[SPEC_MAX_KEY];
    static int lastN = -1;
    static SpecEntry* lastE;
    SpecEntry** slot;
    SpecEntry* e;
    if (!sSpecOn) return NULL;
    if (n == lastN && memcmp(k, last, (size_t)n * sizeof(GLint)) == 0) {
        e = lastE;
    } else {
        slot = spec_slot(k, n, spec_hash(k, (size_t)n * sizeof(GLint), 0xCBF29CE484222325ull));
        if (*slot) e = *slot;
        else if (sSpecCount >= SPEC_SLOTS * 3 / 4) return NULL;
        else e = spec_add(slot, spec_new(k, n));
        memcpy(last, k, (size_t)n * sizeof(GLint));
        lastN = n;
        lastE = e;
    }
    return e->state == SPEC_READY ? &e->prog : NULL;
}

/* Appends a key to the file (the worker's). */
static void spec_write(const SpecEntry* e) {
    uint32_t n = (uint32_t)e->len;
    if (!sSpecFile) return;
    fwrite(&n, 4, 1, sSpecFile);
    fwrite(e->key, sizeof(GLint), (size_t)e->len, sSpecFile);
    fflush(sSpecFile);
}

/* The worker: opens the file and hands the keys in it to the main thread
 * (if it was made for this shader source), leaving it open for appending. */
static void spec_load(void) {
    extern const char* wp_data_dir(void);
    char path[1100];
    uint64_t stamp = 0;
    long good = 16;
    unsigned count = 0;
    FILE* f;
    snprintf(path, sizeof(path), "%s/shadercache", wp_data_dir());
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s/shadercache/gl.bin", wp_data_dir());
    f = fopen(path, "r+b");
    if (f) {
        char magic[8];
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "WPSHADK1", 8) != 0 || fread(&stamp, 8, 1, f) != 1 ||
            stamp != sSpecStamp) {
            fclose(f);
            f = NULL;
        }
    }
    if (f) {
        SpecEntry *list = NULL, *tail = NULL;
        for (;;) {
            uint32_t n;
            GLint key[SPEC_MAX_KEY];
            SpecEntry* e;
            if (fread(&n, 4, 1, f) != 1 || n == 0 || n > SPEC_MAX_KEY || fread(key, sizeof(GLint), n, f) != n) break;
            good = ftell(f);
            e = spec_new(key, (int)n);
            e->saved = 1;
            if (tail) tail->next = e; /* kept in file order: first seen, first compiled */
            else list = e;
            tail = e;
            count++;
        }
        EnterCriticalSection(&sSpecLock);
        sSpecLoaded = list;
        LeaveCriticalSection(&sSpecLock);
        /* Anything after the last whole record is cut off. */
        fflush(f);
        _chsize(_fileno(f), good);
        fseek(f, good, SEEK_SET);
    } else {
        f = fopen(path, "wb");
        if (f) {
            fwrite("WPSHADK1", 1, 8, f);
            fwrite(&sSpecStamp, 8, 1, f);
            fflush(f);
        }
    }
    sSpecFile = f;
    if (getenv("RECOMP_FPS")) fprintf(stderr, "gl: %u shader setups in %s\n", count, path);
}

/* The worker's own context: one setup compiled, waiting for it. */
static void spec_build(SpecEntry* e) {
    const char* src[3] = {kFsVersion, e->src, kFragmentShader};
    GLuint fs = compile_n(GL_FRAGMENT_SHADER, 3, src), p = 0;
    if (fs) {
        p = link_draw_program(fs, 0);
        if (p_glDeleteShader) p_glDeleteShader(fs);
    }
    free(e->src);
    e->src = NULL;
    g_spec_waiting--;
    if (!p) {
        InterlockedExchange(&e->state, SPEC_FAILED);
        return;
    }
    e->prog.id = p;
    p_glUseProgram(p);
    prog_locate(&e->prog);
    {
        /* One draw with it, into a pixel of this context's own, so the
         * driver finishes compiling it here (AMD compiles the GPU code at
         * a program's first draw) rather than at the game's first use. */
        static GLuint fbo, tex, vao;
        if (!fbo) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            p_glGenFramebuffers(1, &fbo);
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            p_glGenVertexArrays(1, &vao);
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glBindVertexArray(vao);
        glViewport(0, 0, 1, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glFinish(); /* all of it done before the main context uses it */
    InterlockedExchange(&e->state, SPEC_READY);
    g_spec_compiled++;
}

static DWORD WINAPI spec_worker(LPVOID arg) {
    (void)arg;
    if (!sSpecParallel && !wglMakeCurrent(sSpecDC, sSpecRC)) {
        fprintf(stderr, "gl: the shader thread's context failed; ubershader only\n");
        sSpecOn = 0;
        return 0;
    }
    spec_load();
    for (;;) {
        SpecEntry* e;
        EnterCriticalSection(&sSpecLock);
        while (!sSpecHead) SleepConditionVariableCS(&sSpecWake, &sSpecLock, INFINITE);
        e = sSpecHead;
        sSpecHead = e->qnext;
        if (!sSpecHead) sSpecTail = NULL;
        LeaveCriticalSection(&sSpecLock);
        if (!e->saved) spec_write(e);
        if (!sSpecParallel) spec_build(e);
    }
    return 0;
}

/* The main thread, once a frame (after presenting): keys from the file
 * added, finished compiles taken, and new ones handed to the driver. */
static void spec_frame(void) {
    SpecEntry* e;
    int touched = 0;
    if (!sSpecOn) return;
    if (sSpecLoaded) {
        SpecEntry* list;
        EnterCriticalSection(&sSpecLock);
        list = sSpecLoaded;
        sSpecLoaded = NULL;
        LeaveCriticalSection(&sSpecLock);
        while ((e = list) != NULL) {
            SpecEntry** slot = spec_slot(e->key, e->len, e->hash);
            list = e->next;
            if (*slot || sSpecCount >= SPEC_SLOTS * 3 / 4) {
                free(e->key);
                free(e);
                continue;
            }
            spec_add(slot, e);
            g_spec_loaded++;
        }
    }
    if (!sSpecParallel) return;
    {
        SpecEntry** pp = &sSpecCompiling;
        while ((e = *pp) != NULL) {
            GLint done = 0, ok = 0;
            p_glGetProgramiv(e->prog.id, GL_COMPLETION_STATUS, &done);
            if (!done) {
                pp = &e->next;
                continue;
            }
            *pp = e->next;
            sSpecInFlight--;
            g_spec_waiting--;
            p_glGetProgramiv(e->prog.id, GL_LINK_STATUS, &ok);
            if (p_glDeleteShader) p_glDeleteShader(e->fs);
            e->fs = 0;
            if (!ok) {
                char log[4096];
                p_glGetProgramInfoLog(e->prog.id, sizeof(log), NULL, log);
                fprintf(stderr, "gl: specialised program failed:\n%s\n", log);
                if (p_glDeleteProgram) p_glDeleteProgram(e->prog.id);
                e->prog.id = 0;
                e->state = SPEC_FAILED;
                continue;
            }
            p_glUseProgram(e->prog.id);
            prog_locate(&e->prog);
            touched = 1;
            g_spec_compiled++;
            e->state = SPEC_READY;
        }
    }
    while (sSpecInFlight < SPEC_IN_FLIGHT && (e = sSpecNew) != NULL) {
        const char* src[3];
        int k;
        extern unsigned g_cnt_shader_compiles; /* debug overlay */
        sSpecNew = e->next;
        if (!sSpecNew) sSpecNewTail = NULL;
        src[0] = kFsVersion, src[1] = e->src, src[2] = kFragmentShader;
        e->fs = p_glCreateShader(GL_FRAGMENT_SHADER);
        p_glShaderSource(e->fs, 3, src, NULL);
        p_glCompileShader(e->fs);
        e->prog.id = p_glCreateProgram();
        p_glAttachShader(e->prog.id, sVs);
        p_glAttachShader(e->prog.id, e->fs);
        for (k = 0; k < 11; k++) p_glBindAttribLocation(e->prog.id, (GLuint)k, kAttribs[k]);
        p_glLinkProgram(e->prog.id);
        free(e->src);
        e->src = NULL;
        e->next = sSpecCompiling;
        sSpecCompiling = e;
        sSpecInFlight++;
        g_cnt_shader_compiles++;
    }
    if (touched) {
        /* prog_locate changed the program in use. */
        p_glUseProgram(sProgram);
        gl_forget();
    }
}

/* Starts it: the driver's parallel compiling if it has it, else a worker
 * context sharing objects with main_rc. */
static void spec_init(HGLRC main_rc, int pixel_format, PIXELFORMATDESCRIPTOR* pfd) {
    typedef void(APIENTRY * PFN_threads)(GLuint);
    const char* e = getenv("WONDERPARK_SHADERS");
    PFN_threads threads;
    HANDLE t;
    /* The Shader compilation setting (Graphics; from the next start): Fast
     * and Precompile turn them on, Smooth (the default) leaves the
     * ubershader alone. WONDERPARK_SHADERS over it. */
    if (!e && (g_x[X_SHADER_MODE] == 1 || g_x[X_SHADER_MODE] == 2)) e = "spec";
    if (sSimpleFs || !e || (strcmp(e, "spec") != 0 && strcmp(e, "thread") != 0)) return;
    threads = (PFN_threads)(void*)wglGetProcAddress("glMaxShaderCompilerThreadsKHR");
    if (!threads) threads = (PFN_threads)(void*)wglGetProcAddress("glMaxShaderCompilerThreadsARB");
    if (threads && !(e && strcmp(e, "thread") == 0)) {
        threads(0xFFFFFFFFu); /* as many as the driver likes */
        sSpecParallel = 1;
    } else {
        HWND w = CreateWindowA("WonderparkGL", "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, GetModuleHandleA(NULL), NULL);
        sSpecDC = w ? GetDC(w) : NULL;
        if (!sSpecDC || !SetPixelFormat(sSpecDC, pixel_format, pfd) || !(sSpecRC = wglCreateContext(sSpecDC)) ||
            !wglShareLists(main_rc, sSpecRC)) {
            fprintf(stderr, "gl: no shared context for compiling shaders; ubershader only\n");
            return;
        }
    }
    sSpecStamp = spec_hash(kVertexShader, strlen(kVertexShader), 0xCBF29CE484222325ull);
    sSpecStamp = spec_hash(kFragmentShader, strlen(kFragmentShader), sSpecStamp);
    InitializeCriticalSection(&sSpecLock);
    InitializeConditionVariable(&sSpecWake);
    sSpecOn = 1;
    t = CreateThread(NULL, 0, spec_worker, NULL, 0, NULL);
    if (!t) {
        sSpecOn = 0;
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    fprintf(stderr, "gl: specialised shaders, compiled %s\n",
            sSpecParallel ? "by the driver in parallel" : "on a shader thread");
}

/* EFB copies: a full-screen triangle that samples the EFB rectangle and
 * converts it to the copy format, stored as the texture reads back. The
 * value of a narrow format is truncated to its bits and expanded again,
 * as the console's copy and texture fetch do. */
static const char* kCopyVertexShader =
    "#version 330 core\n"
    "uniform vec4 u_src;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID >> 1) * 2);\n"
    "    v_uv = mix(u_src.xy, u_src.zw, p);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* kCopyFragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_efb;\n"
    "uniform ivec4 u_mode; /* copy format, intensity, EFB has alpha */\n"
    "float bits(float v, int n) {\n"
    "    float i = float(int(v * 255.0 + 0.5) >> (8 - n));\n"
    "    return floor(i * 255.0 / float((1 << n) - 1) + 0.5) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = texture(u_efb, v_uv);\n"
    "    int f = u_mode.x;\n"
    "    float i;\n"
    "    if (u_mode.z == 0) c.a = 1.0;\n"
    "    /* The intensity formats store luma (BT.601, studio range). */\n"
    "    i = u_mode.y != 0 ? clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0) : c.r;\n"
    "    if (f == 0) o_color = vec4(bits(i, 4));\n"
    "    else if (f == 1 || f == 8) o_color = vec4(i);\n"
    "    else if (f == 2) o_color = vec4(vec3(bits(i, 4)), bits(c.a, 4));\n"
    "    else if (f == 3) o_color = vec4(vec3(i), c.a);\n"
    "    else if (f == 4) o_color = vec4(bits(c.r, 5), bits(c.g, 6), bits(c.b, 5), 1.0);\n"
    "    else if (f == 5 && c.a >= 224.0 / 255.0) o_color = vec4(bits(c.r, 5), bits(c.g, 5), bits(c.b, 5), 1.0);\n"
    "    else if (f == 5) o_color = vec4(bits(c.r, 4), bits(c.g, 4), bits(c.b, 4), bits(c.a, 3));\n"
    "    else if (f == 7) o_color = vec4(c.a);\n"
    "    else if (f == 9) o_color = vec4(c.g);\n"
    "    else if (f == 10) o_color = vec4(c.b);\n"
    "    else if (f == 11) o_color = vec4(vec3(c.g), c.r); /* IA8 layout: A = R, I = G */\n"
    "    else if (f == 12) o_color = vec4(vec3(c.b), c.g);\n"
    "    else o_color = c;\n"
    "}\n";

static GLuint sCopyProgram, sCopyVao;
static GLint uCopySrc, uCopyMode;

static void build_copy_program(void) {
    GLint ok;
    sCopyProgram = p_glCreateProgram();
    p_glAttachShader(sCopyProgram, compile(GL_VERTEX_SHADER, kCopyVertexShader));
    p_glAttachShader(sCopyProgram, compile(GL_FRAGMENT_SHADER, kCopyFragmentShader));
    p_glLinkProgram(sCopyProgram);
    p_glGetProgramiv(sCopyProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sCopyProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        exit(1);
    }
    p_glUseProgram(sCopyProgram);
    p_glUniform1i(p_glGetUniformLocation(sCopyProgram, "u_efb"), 0);
    uCopySrc = p_glGetUniformLocation(sCopyProgram, "u_src");
    uCopyMode = p_glGetUniformLocation(sCopyProgram, "u_mode");
    p_glGenVertexArrays(1, &sCopyVao);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
}

/* The settings menu, an image with alpha laid over the presented picture.
 * Its rows run top to bottom. */
static const char* kOverlayVertexShader =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID >> 1) * 2);\n"
    "    v_uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* kOverlayFragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_image;\n"
    "void main() { o_color = texture(u_image, v_uv); }\n";

/* The picture's own look, from the F1 tab's settings, drawn from the kept
 * picture (sLastTex) back into the window's framebuffer: Vivid colors
 * (more saturation, a little contrast), gamma (a curve on the midtones, so
 * whites stay white and nothing clips), brightness (the black level: up
 * lifts the shadows toward grey, down dims everything), the color-blind
 * filter and Mirror world (flipped left to right). Only when one of them
 * isn't the default: otherwise no pass at all.
 *
 * The color-blind filter shifts the colors a kind of color blindness
 * can't tell apart into ones it can ("daltonizing"): the picture as that
 * eye sees it (Machado, Oliveira and Fernandes 2009, full strength, in
 * linear light), the difference from the real one, and that difference
 * moved into the channels still seen (green and blue for red-green
 * blindness, red and green for blue-yellow). u_cb: the simulation's rows,
 * then the shift's. */
static const char* kLookFragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_image;\n"
    "uniform vec4 u_look; /* gamma, saturation, contrast, mirror */\n"
    "uniform vec4 u_look2; /* brightness (-0.3 to 0.15), color-blind filter on */\n"
    "uniform vec4 u_cb[6];\n"
    "uniform vec4 u_look3; /* Silly: color filter (0-4), retro look (0-2), block size over the picture's (x, y) */\n"
    "uniform vec4 u_look4; /* Time of day and the season: the light's tint (rgb) */\n"
    "float bayer4(vec2 p) {\n"
    "    int x = int(mod(p.x, 4.0)), y = int(mod(p.y, 4.0));\n"
    "    int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
    "    return (float(m[y * 4 + x]) + 0.5) / 16.0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = vec2(u_look.w != 0.0 ? 1.0 - v_uv.x : v_uv.x, 1.0 - v_uv.y);\n"
    "    vec2 cell = vec2(0.0);\n"
    "    if (u_look3.y != 0.0) {\n"
    "        /* Retro look: big square pixels, each sampled at its middle. */\n"
    "        cell = floor(uv / u_look3.zw);\n"
    "        uv = (cell + 0.5) * u_look3.zw;\n"
    "    }\n"
    "    vec3 c = texture(u_image, uv).rgb;\n"
    "    c = clamp(c * u_look4.rgb, 0.0, 1.0);\n"
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    c = clamp(mix(vec3(l), c, u_look.y), 0.0, 1.0);\n"
    "    c = clamp((c - 0.5) * u_look.z + 0.5, 0.0, 1.0);\n"
    "    c = pow(c, vec3(u_look.x));\n"
    "    c = u_look2.x >= 0.0 ? 1.0 - (1.0 - c) * (1.0 - u_look2.x) : c * (1.0 + u_look2.x);\n"
    "    if (u_look2.y != 0.0) {\n"
    "        vec3 lin = pow(c, vec3(2.2));\n"
    "        vec3 sim = vec3(dot(u_cb[0].xyz, lin), dot(u_cb[1].xyz, lin), dot(u_cb[2].xyz, lin));\n"
    "        vec3 e = lin - sim;\n"
    "        lin = clamp(lin + vec3(dot(u_cb[3].xyz, e), dot(u_cb[4].xyz, e), dot(u_cb[5].xyz, e)), 0.0, 1.0);\n"
    "        c = pow(lin, vec3(1.0 / 2.2));\n"
    "    }\n"
    "    int filt = int(u_look3.x + 0.5);\n"
    "    float y = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "    if (filt == 1) {\n"
    "        /* Old photo: sepia, faded, darker at the edges. */\n"
    "        vec3 s = vec3(y * 1.07, y * 0.87, y * 0.66) + vec3(0.06, 0.04, 0.02);\n"
    "        vec2 d = v_uv - 0.5;\n"
    "        c = clamp(s * (1.0 - 0.55 * dot(d, d)), 0.0, 1.0);\n"
    "    } else if (filt == 2) {\n"
    "        /* Night: dark and blue. */\n"
    "        c = clamp(mix(vec3(y), c, 0.55) * vec3(0.48, 0.58, 0.95) + vec3(0.0, 0.01, 0.05), 0.0, 1.0);\n"
    "    } else if (filt == 3) {\n"
    "        /* Pocket: four shades of green (dithered with Retro look's). */\n"
    "        float l = y + (u_look3.y > 1.5 ? (bayer4(cell) - 0.5) / 4.0 : 0.0);\n"
    "        float k = clamp(floor(l * 4.0), 0.0, 3.0);\n"
    "        c = k < 0.5 ? vec3(0.06, 0.22, 0.06) : k < 1.5 ? vec3(0.19, 0.38, 0.19) : k < 2.5 ? vec3(0.55, 0.67, 0.06) : vec3(0.61, 0.74, 0.06);\n"
    "    } else if (filt == 4) {\n"
    "        /* Candy: brighter colors, a pink glow. */\n"
    "        c = clamp(mix(vec3(y), c, 1.55), 0.0, 1.0);\n"
    "        c = mix(c, vec3(1.0, 0.72, 0.88), 0.16);\n"
    "    }\n"
    "    if (u_look3.y > 1.5 && filt != 3) {\n"
    "        /* Pixel + dither: 5 levels a channel, ordered dithering. */\n"
    "        c = floor(c * 4.0 + bayer4(cell)) / 4.0;\n"
    "    }\n"
    "    o_color = vec4(c, 1.0);\n"
    "}\n";

static GLuint sLookProgram;
static GLint uLook, uLook2, uCb, uLook3, uLook4;

/* Time of day and Seasonal touches (seasons.c): the light's grade, worked
 * out once a presented frame (it eases, so it's asked every frame). */
extern int seasons_grade(float rgb[3]);
static float sGrade[3] = {1, 1, 1};

static int look_needed(void) {
    int grade = seasons_grade(sGrade);
    return g_x[X_BRIGHTNESS] != 100 || g_x[X_GAMMA] != 100 || g_x[X_COLORBLIND] || g_x[X_COLORS] ||
           g_x[X_MIRROR_WORLD] || g_x[X_COLOR_FILTER] || g_x[X_RETRO_LOOK] || grade;
}

/* The color-blind filter's matrices (rows), per kind: deuteranopia,
 * protanopia, tritanopia (Machado et al. 2009, severity 1), then the shift. */
static const float kCbSim[3][3][4] = {
    {{0.367322f, 0.860646f, -0.227968f, 0}, {0.280085f, 0.672501f, 0.047413f, 0}, {-0.011820f, 0.042940f, 0.968881f, 0}},
    {{0.152286f, 1.052583f, -0.204868f, 0}, {0.114503f, 0.786281f, 0.099216f, 0}, {-0.003882f, -0.048116f, 1.051998f, 0}},
    {{1.255528f, -0.076749f, -0.178779f, 0}, {-0.078411f, 0.930809f, 0.147602f, 0}, {0.004733f, 0.691367f, 0.303900f, 0}},
};
static const float kCbShiftRG[3][4] = {{0, 0, 0, 0}, {0.7f, 1, 0, 0}, {0.7f, 0, 1, 0}};
static const float kCbShiftBY[3][4] = {{1, 0, 0.7f, 0}, {0, 1, 0.7f, 0}, {0, 0, 0, 0}};

static void build_look_program(void) {
    GLint ok;
    sLookProgram = p_glCreateProgram();
    p_glAttachShader(sLookProgram, compile(GL_VERTEX_SHADER, kOverlayVertexShader));
    p_glAttachShader(sLookProgram, compile(GL_FRAGMENT_SHADER, kLookFragmentShader));
    p_glLinkProgram(sLookProgram);
    p_glGetProgramiv(sLookProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sLookProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        sLookProgram = 0;
        return;
    }
    p_glUseProgram(sLookProgram);
    p_glUniform1i(p_glGetUniformLocation(sLookProgram, "u_image"), 0);
    uLook = p_glGetUniformLocation(sLookProgram, "u_look");
    uLook2 = p_glGetUniformLocation(sLookProgram, "u_look2");
    uCb = p_glGetUniformLocation(sLookProgram, "u_cb");
    uLook3 = p_glGetUniformLocation(sLookProgram, "u_look3");
    uLook4 = p_glGetUniformLocation(sLookProgram, "u_look4");
    p_glUseProgram(sProgram);
}

static GLuint sOverlayProgram, sOverlayTex;

static void build_overlay_program(void) {
    GLint ok;
    sOverlayProgram = p_glCreateProgram();
    p_glAttachShader(sOverlayProgram, compile(GL_VERTEX_SHADER, kOverlayVertexShader));
    p_glAttachShader(sOverlayProgram, compile(GL_FRAGMENT_SHADER, kOverlayFragmentShader));
    p_glLinkProgram(sOverlayProgram);
    p_glGetProgramiv(sOverlayProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sOverlayProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        exit(1);
    }
    p_glUseProgram(sOverlayProgram);
    p_glUniform1i(p_glGetUniformLocation(sOverlayProgram, "u_image"), 0);
    p_glUseProgram(sProgram);
    glGenTextures(1, &sOverlayTex);
    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* ------------------------------------------------------------------------ */
/* Window and EFB */

static HDC sDC; /* the game window's */
/* The DC the GL context is current on: a hidden window's of its own when
 * presenting through DXGI, else sDC. */
static HDC sGlDC;
/* What the window's picture is drawn into: sWinFbo, the size of the
 * window's client area, when presenting through DXGI, else 0. */
static GLuint sWinFbo;
/* EFB pixels to GL pixels. In 16:9 an EFB pixel is shown wider
 * (win_pixel_aspect), so the EFB gets as many more pixels across: the
 * stretched picture is then at full resolution. */
static double sScaleX = 2, sScaleY = 2;
static double sPixelAspect = 1; /* win_pixel_aspect() */
#define GLX(x) ((GLint)floor((x) * sScaleX + 0.5))
#define GLY(y) ((GLint)floor((y) * sScaleY + 0.5))
/* Draws go to sDrawFbo. Without anti-aliasing that is sEfbFbo, whose
 * color texture the copies and the presented picture read; with it, a
 * multisampled framebuffer resolved into sEfbFbo when they need it. */
static GLuint sEfbFbo, sEfbColor, sEfbDepth;
static GLuint sDrawFbo, sMsFbo, sMsColor, sMsDepth;
static int sMsDirty;
static GLuint sWhite;
static const char* sDumpDir;
static int sDumpEvery = 30;
static unsigned sDumpFrom;
static unsigned sPresented;
/* The settings as applied. */
static int sScale, sMsaa = 1, sMaxSamples = 1;
static float sMaxAniso; /* the most anisotropic filtering this GPU does; 0 if none */

static int gl_scale(void) { return sScale; }
static int gl_msaa(void) {
    int m;
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    return m;
}

/* (Re)allocates the EFB at the current scale and sample count, and the
 * copy textures at the matching scale (keeping their names: the texture
 * cache holds them). What they held is lost; the game redraws it. */
static void alloc_efb(void);

static void gl_apply_settings(void) {
    extern int hle_widescreen(void);
    extern float g_wide_r, g_wide_shown; /* gx_fifo.c, "Widescreen" */
    int w, h, s, m;
    double pa = win_pixel_aspect();
    win_size(&w, &h);
    if (h <= 0) return; /* minimized */
    s = gx_wanted_scale(h);
    {
        /* In the game's 16:9 mode, a wider window (up to 32:9) gets an EFB
         * as wide as it, and the draws are narrowed to fit; a narrower one
         * sees the picture cropped at the sides (gl_present). */
        float r = 1, shown = 1;
        if (hle_widescreen() && w > 0) {
            double a = (double)w / h, base = 16.0 / 9.0;
            if (a > 32.0 / 9.0) a = 32.0 / 9.0;
            if (a > base * 1.005) r = (float)(a / base), pa = a * 456.0 / 640.0;
            else if (a < base * 0.995) shown = (float)(a / base);
        }
        g_wide_r = r;
        g_wide_shown = shown;
    }
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    if (s == sScale && m == sMsaa && pa == sPixelAspect) return;
    sPixelAspect = pa;
    sScale = s;
    sMsaa = m;
    sScaleY = s;
    sScaleX = s * sPixelAspect;
    alloc_efb();
    fprintf(stderr, "gl: internal resolution %dx (%d x %d), anti-aliasing %s\n", s, GLX(EFB_W), GLY(EFB_H),
            m > 1 ? (m == 2 ? "2x" : m == 4 ? "4x" : "8x") : "off");
}

/* Brings sEfbFbo up to date with the multisampled draws. */
static void resolve(void) {
    gl_forget();
    if (sDrawFbo == sEfbFbo || !sMsDirty) return;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sMsFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEfbFbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, GLX(EFB_W), GLY(EFB_H), 0, 0, GLX(EFB_W), GLY(EFB_H), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    sMsDirty = 0;
}

/* ------------------------------------------------------------------------ */
/* Presenting
 *
 * The window is shown through a DXGI swapchain of our own, with the GL
 * picture handed over in a D3D11 texture (WGL_NV_DX_interop2), rather
 * than by SwapBuffers. On NVIDIA, SwapBuffers on a window presents
 * through a DXGI swapchain the driver keeps itself ("layered on DXGI"),
 * and that path crashed about once in 40 minutes of play, usually just
 * after a slow frame: one of the driver's threads (or the game thread,
 * inside SwapBuffers) called ID3D11DeviceContext4::Wait with a fence that
 * was never made, a read of address 0 in d3d11.dll. That call is not made
 * for ordinary frames (none in 15,000 presents), only on some rarer path
 * of the driver's, and nothing in the GL calls made before it is wrong
 * (a debug context reports nothing). The GL context is made on a hidden
 * window of its own, never presented, so that path is never entered.
 *
 * Flip model, discarding, and with tearing allowed where the system
 * supports it, so presenting doesn't wait for the display (as
 * SwapInterval 0 did). Each frame the picture is drawn into sWinFbo as
 * it was into the window, then flipped into the shared texture (D3D's
 * row 0 is the top) and copied to the back buffer.
 *
 * Without the extension, or with RECOMP_GL_PRESENT=swapbuffers, the
 * context moves to the game's window and SwapBuffers presents it. */

typedef HANDLE(WINAPI* PFN_wglDXOpenDeviceNV)(void*);
typedef HANDLE(WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE*);
static PFN_wglDXRegisterObjectNV p_wglDXRegisterObjectNV;
static PFN_wglDXUnregisterObjectNV p_wglDXUnregisterObjectNV;
static PFN_wglDXLockObjectsNV p_wglDXLockObjectsNV, p_wglDXUnlockObjectsNV;
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

static ID3D11Device* sD3d;
static ID3D11DeviceContext* sD3dCtx;
static IDXGISwapChain1* sChain;
static ID3D11Texture2D* sShared; /* the picture, written by GL */
static HANDLE sInteropDev, sInterop;
static GLuint sWinColor, sSharedFbo, sSharedColor;
static int sChainW, sChainH, sTearing;

/* Sets up presenting through DXGI; 0 if it can't be. The D3D11 device
 * must be on the GL context's adapter: the first adapter the interop
 * accepts. */
static int dx_init(void) {
    PFN_wglDXOpenDeviceNV open = (PFN_wglDXOpenDeviceNV)(void*)wglGetProcAddress("wglDXOpenDeviceNV");
    IDXGIFactory1* f1 = NULL;
    IDXGIFactory2* f2 = NULL;
    IDXGIFactory5* f5 = NULL;
    IDXGIAdapter1* adapter;
    DXGI_SWAP_CHAIN_DESC1 d;
    UINT i;
    p_wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)(void*)wglGetProcAddress("wglDXRegisterObjectNV");
    p_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)(void*)wglGetProcAddress("wglDXUnregisterObjectNV");
    p_wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)(void*)wglGetProcAddress("wglDXLockObjectsNV");
    p_wglDXUnlockObjectsNV = (PFN_wglDXLockObjectsNV)(void*)wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!open || !p_wglDXRegisterObjectNV || !p_wglDXUnregisterObjectNV || !p_wglDXLockObjectsNV ||
        !p_wglDXUnlockObjectsNV) {
        fprintf(stderr, "gl: no WGL_NV_DX_interop2\n");
        return 0;
    }
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&f1))) return 0;
    for (i = 0; !sInteropDev && IDXGIFactory1_EnumAdapters1(f1, i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 ad;
        IDXGIAdapter1_GetDesc1(adapter, &ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D11CreateDevice((IDXGIAdapter*)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                        D3D11_SDK_VERSION, &sD3d, NULL, &sD3dCtx))) {
            sInteropDev = open(sD3d);
            if (!sInteropDev) {
                ID3D11DeviceContext_Release(sD3dCtx);
                ID3D11Device_Release(sD3d);
                sD3d = NULL;
                sD3dCtx = NULL;
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    if (!sInteropDev || FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory2, (void**)&f2))) {
        fprintf(stderr, "gl: no D3D11 device the GL context can share with\n");
        IDXGIFactory1_Release(f1);
        return 0;
    }
    if (SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory5, (void**)&f5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                        sizeof(tearing))))
            sTearing = tearing != FALSE;
        IDXGIFactory5_Release(f5);
    }
    memset(&d, 0, sizeof(d));
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = sTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)sD3d, (HWND)win_handle(), &d, NULL, NULL, &sChain))) {
        fprintf(stderr, "gl: no DXGI swapchain for the window\n");
        sChain = NULL;
    } else {
        /* Alt+Enter is the game's own (borderless fullscreen). */
        IDXGIFactory2_MakeWindowAssociation(f2, (HWND)win_handle(), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    IDXGIFactory2_Release(f2);
    IDXGIFactory1_Release(f1);
    if (!sChain) return 0;
    p_glGenFramebuffers(1, &sWinFbo);
    p_glGenFramebuffers(1, &sSharedFbo);
    return 1;
}

/* Sizes the swapchain, the shared texture and sWinFbo to the window's
 * client area, w x h (not 0). */
static void dx_size(int w, int h) {
    D3D11_TEXTURE2D_DESC t;
    HRESULT hr;
    if (w == sChainW && h == sChainH) return;
    if (sInterop) {
        p_wglDXUnregisterObjectNV(sInteropDev, sInterop);
        sInterop = NULL;
        p_glDeleteRenderbuffers(1, &sSharedColor);
        p_glDeleteRenderbuffers(1, &sWinColor);
        ID3D11Texture2D_Release(sShared);
        sShared = NULL;
    }
    hr = IDXGISwapChain1_ResizeBuffers(sChain, 0, w, h, DXGI_FORMAT_UNKNOWN,
                                       sTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        /* Nothing is shown until a resize works (tried each frame). */
        static int reported;
        if (!reported++) fprintf(stderr, "gl: resizing the swapchain to %d x %d failed (%08lX)\n", w, h, (unsigned long)hr);
        return;
    }
    memset(&t, 0, sizeof(t));
    t.Width = w;
    t.Height = h;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(sD3d, &t, NULL, &sShared))) {
        fprintf(stderr, "gl: no %d x %d shared texture\n", w, h);
        sShared = NULL;
        return;
    }
    p_glGenRenderbuffers(1, &sSharedColor);
    sInterop = p_wglDXRegisterObjectNV(sInteropDev, sShared, sSharedColor, GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!sInterop) {
        fprintf(stderr, "gl: sharing the %d x %d texture failed\n", w, h);
        p_glDeleteRenderbuffers(1, &sSharedColor);
        ID3D11Texture2D_Release(sShared);
        sShared = NULL;
        return;
    }
    p_wglDXLockObjectsNV(sInteropDev, 1, &sInterop);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sSharedFbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sSharedColor);
    p_wglDXUnlockObjectsNV(sInteropDev, 1, &sInterop);
    p_glGenRenderbuffers(1, &sWinColor);
    p_glBindRenderbuffer(GL_RENDERBUFFER, sWinColor);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sWinColor);
    sChainW = w;
    sChainH = h;
}

/* Writes the shared texture, as presented, to a BMP (RECOMP_GX_DUMP). */
static void dump_shared(const char* path) {
    D3D11_TEXTURE2D_DESC t;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D* staging;
    FILE* f;
    if (!sShared) return;
    ID3D11Texture2D_GetDesc(sShared, &t);
    t.Usage = D3D11_USAGE_STAGING;
    t.BindFlags = 0;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(sD3d, &t, NULL, &staging))) return;
    ID3D11DeviceContext_CopyResource(sD3dCtx, (ID3D11Resource*)staging, (ID3D11Resource*)sShared);
    if (SUCCEEDED(ID3D11DeviceContext_Map(sD3dCtx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        if ((f = fopen(path, "wb")) != NULL) {
            uint8_t hdr[54] = {'B', 'M'};
            uint8_t* row = (uint8_t*)malloc((size_t)t.Width * 4);
            UINT x, y;
            *(uint32_t*)(hdr + 2) = 54 + t.Width * t.Height * 4;
            hdr[10] = 54;
            hdr[14] = 40;
            *(int32_t*)(hdr + 18) = (int32_t)t.Width;
            *(int32_t*)(hdr + 22) = -(int32_t)t.Height; /* top row first, as D3D has it */
            hdr[26] = 1;
            hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < t.Height; y++) {
                const uint8_t* px = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (x = 0; x < t.Width; x++) {
                    row[x * 4] = px[x * 4 + 2];
                    row[x * 4 + 1] = px[x * 4 + 1];
                    row[x * 4 + 2] = px[x * 4];
                    row[x * 4 + 3] = 255;
                }
                fwrite(row, 4, t.Width, f);
            }
            free(row);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(sD3dCtx, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

/* Shows what was drawn into the window's framebuffer, w x h; 1 if it was
 * left out, as may_drop allows. */
static int window_present(int w, int h, int may_drop) {
    ID3D11Texture2D* back;
    HRESULT hr;
    static int failures, swapInterval;
    int vsync = g_x[X_VSYNC] != 0;
    if (!sChain) {
        /* V-Sync (off by default: see pSwapInterval). */
        if (pSwapInterval && vsync != swapInterval) pSwapInterval(swapInterval = vsync);
        SwapBuffers(sDC);
        return 0;
    }
    if (!sInterop || w != sChainW || h != sChainH) return 0; /* minimized, or no shared texture */
    if (!p_wglDXLockObjectsNV(sInteropDev, 1, &sInterop)) {
        if (failures++ < 5) fprintf(stderr, "gl: locking the shared texture failed\n");
        return 0;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sWinFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sSharedFbo);
    p_glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_wglDXUnlockObjectsNV(sInteropDev, 1, &sInterop);
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(sChain, 0, &IID_ID3D11Texture2D, (void**)&back))) {
        ID3D11DeviceContext_CopyResource(sD3dCtx, (ID3D11Resource*)back, (ID3D11Resource*)sShared);
        ID3D11Texture2D_Release(back);
    }
    hr = IDXGISwapChain1_Present(sChain, vsync ? 1 : 0,
                                 (sTearing && !vsync ? DXGI_PRESENT_ALLOW_TEARING : 0) |
                                     (may_drop ? DXGI_PRESENT_DO_NOT_WAIT : 0));
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 1;
    if (FAILED(hr) && failures++ < 5) fprintf(stderr, "gl: present failed (%08lX)\n", (unsigned long)hr);
    return 0;
}

/* RECOMP_GL_DEBUG=1: a debug context (compatibility profile, as the
 * shaders need), with the driver's messages about errors and misuse
 * printed as each call makes them. */
static void APIENTRY debug_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                   const GLchar* message, const void* user) {
    static unsigned count;
    (void)source;
    (void)length;
    (void)user;
    if (severity == 0x826B /* GL_DEBUG_SEVERITY_NOTIFICATION */) return;
    if (++count > 200) return;
    fprintf(stderr, "gl debug: type %04X id %u severity %04X: %s\n", type, id, severity, message);
}

static HGLRC debug_context(HGLRC legacy) {
    typedef HGLRC(WINAPI * PFN_create)(HDC, HGLRC, const int*);
    typedef void(APIENTRY * PFN_callback)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);
    typedef void(APIENTRY * PFN_set_callback)(PFN_callback, const void*);
    static const int attribs[] = {0x2091, 4,      /* WGL_CONTEXT_MAJOR_VERSION_ARB */
                                  0x2092, 6,      /* WGL_CONTEXT_MINOR_VERSION_ARB */
                                  0x2094, 1,      /* WGL_CONTEXT_FLAGS_ARB: WGL_CONTEXT_DEBUG_BIT_ARB */
                                  0x9126, 2,      /* WGL_CONTEXT_PROFILE_MASK_ARB: compatibility */
                                  0};
    PFN_create create = (PFN_create)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
    PFN_set_callback set_callback;
    HGLRC rc = create ? create(sGlDC, NULL, attribs) : NULL;
    if (!rc) {
        fprintf(stderr, "gl: no debug context\n");
        return legacy;
    }
    wglMakeCurrent(sGlDC, rc);
    wglDeleteContext(legacy);
    set_callback = (PFN_set_callback)(void*)wglGetProcAddress("glDebugMessageCallback");
    if (set_callback) {
        glEnable(0x92E0); /* GL_DEBUG_OUTPUT */
        glEnable(0x8242); /* GL_DEBUG_OUTPUT_SYNCHRONOUS */
        set_callback(debug_message, NULL);
    }
    fprintf(stderr, "gl: debug context, synchronous debug output\n");
    return rc;
}

static int gl_init(void) {
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC rc;
    const char* v;
    uint32_t white = 0xFFFFFFFF;
    sPixelAspect = win_pixel_aspect();
    sDumpDir = getenv("RECOMP_GX_DUMP");
    if ((v = getenv("RECOMP_GX_DUMP_EVERY")) && atoi(v) > 0) sDumpEvery = atoi(v);
    if ((v = getenv("RECOMP_GX_DUMP_FROM")) && atoi(v) > 0) sDumpFrom = (unsigned)atoi(v);

    sDC = GetDC((HWND)win_handle());
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    /* The context starts on a hidden window of its own (see Presenting). */
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "WonderparkGL";
    RegisterClassA(&wc);
    sGlDC = GetDC(CreateWindowA("WonderparkGL", "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL));
    SetPixelFormat(sGlDC, ChoosePixelFormat(sGlDC, &pfd), &pfd);
    rc = wglCreateContext(sGlDC);
    wglMakeCurrent(sGlDC, rc);
    if ((v = getenv("RECOMP_GL_DEBUG")) && *v && *v != '0') rc = debug_context(rc);
    load_gl();
    v = getenv("RECOMP_GL_PRESENT");
    /* Wonderpark: the DXGI path works around an NVIDIA driver crash (above);
     * elsewhere it costs more than it saves (on AMD, locking the shared
     * texture waited about 10 ms a frame), so other GPUs use SwapBuffers.
     * RECOMP_GL_PRESENT=dxgi or =swapbuffers chooses either way. */
    if (!v || !*v) {
        const char* vendor = (const char*)glGetString(GL_VENDOR);
        v = vendor && strstr(vendor, "NVIDIA") ? "dxgi" : "swapbuffers";
    }
    if (!(v && strcmp(v, "swapbuffers") == 0) && dx_init()) {
        fprintf(stderr, "gl: presenting through DXGI (flip model%s)\n", sTearing ? ", tearing allowed" : "");
    } else {
        /* SwapBuffers on the game's window: the context moves there. */
        SetPixelFormat(sDC, GetPixelFormat(sGlDC), &pfd);
        wglMakeCurrent(sDC, rc);
        sGlDC = sDC;
        fprintf(stderr, "gl: presenting with SwapBuffers\n");
    }
    fprintf(stderr, "gl: %s, %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    glGetIntegerv(GL_MAX_SAMPLES, &sMaxSamples);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &sMaxAniso);
    while (glGetError() != GL_NO_ERROR) {}
    build_program();
    build_copy_program();
    build_overlay_program();
    build_look_program();
    spec_init(rc, GetPixelFormat(sGlDC), &pfd);
    pSwapInterval = (BOOL(WINAPI*)(int))(void*)wglGetProcAddress("wglSwapIntervalEXT");
    if (pSwapInterval) pSwapInterval(0);

    glGenTextures(1, &sEfbColor);
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &sEfbDepth);
    p_glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    p_glGenFramebuffers(1, &sEfbFbo);
    gl_apply_settings();

    glGenTextures(1, &sWhite);
    glBindTexture(GL_TEXTURE_2D, sWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Textures */

static GLint wrap_mode(int w) { return w == 0 ? GL_CLAMP_TO_EDGE : (w == 1 ? GL_REPEAT : GL_MIRRORED_REPEAT); }

static void upload_levels(int fresh, int width, int height, int levels, const uint8_t* rgba) {
    int l;
    for (l = 0; l < levels; l++) {
        if (fresh) glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        else glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        rgba += (size_t)width * height * 4;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
}

/* Sampled as the game set it: TX_SETMODE0's wrap, magnification and
 * minification filters (mipmap: none, nearest or linear), and LOD bias.
 * Anisotropic filtering, as the setting says, on what is filtered
 * linearly. */
/* Textures are RGBA8 alone here (no texture_pixels). */
static uint32_t gl_texture(int pix, int width, int height, int levels, const uint8_t* rgba, uint32_t mode0) {
    static const GLenum kMin[2][3] = {{GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR},
                                      {GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR}};
    int mip = (mode0 >> 5) & 3, min_linear = (mode0 >> 7) & 1;
    GLuint t;
    (void)pix;
    gl_forget();
    if (mip > 2) mip = 2;
    if (levels <= 1) mip = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    upload_levels(1, width, height, levels, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(mode0 & 3));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((mode0 >> 2) & 3));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kMin[min_linear][mip]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    if (mip) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f);
    if (sMaxAniso > 1 && g_settings.aniso > 1 && min_linear)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                        (float)g_settings.aniso < sMaxAniso ? (float)g_settings.aniso : sMaxAniso);
    return t;
}

static void gl_texture_update(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* rgba) {
    (void)pix;
    gl_forget();
    glBindTexture(GL_TEXTURE_2D, handle);
    upload_levels(0, width, height, levels, rgba);
}

static void gl_texture_delete(const uint32_t* handles, int n) { glDeleteTextures(n, handles); }

/* ------------------------------------------------------------------------ */
/* Drawing */

static const GLenum kBlendSrc[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
/* Without an alpha channel (RGB8 and RGB565 EFB formats) the EFB's
 * alpha reads as 1: the bloom in Mew's intro weights its copies by it. */
static GLenum blend_factor(GLenum f, int efb_alpha) {
    if (efb_alpha) return f;
    if (f == GL_DST_ALPHA) return GL_ONE;
    if (f == GL_ONE_MINUS_DST_ALPHA) return GL_ZERO;
    return f;
}
/* GL state as draws last set it, so a draw changes only what differs
 * (consecutive batches differ in a few fields; each GL call costs driver
 * time). Anything else that changes GL state (copies, clears, presenting,
 * making textures) calls gl_forget, and the next draw sets it all. The
 * TEV uniforms belong to the draw program alone and are never forgotten. */
static struct {
    int valid;
    GLuint tex[8];
    int blend;
    GLenum blend_src, blend_dst, blend_eq;
    GLboolean color_mask[4], depth_mask;
    int depth;
    GLenum depth_func;
    int cull;
    GLenum cull_face;
    GLint viewport[4], scissor[4];
    double depth_range[2];
} sGl;

static void gl_forget(void) { sGl.valid = 0; }

/* Uploads a uniform array to program p only when it differs from what was
 * uploaded to it (and only if p has it: a specialised program has made
 * some constant). */
#define UNIFORM_IF_CHANGED(fn, p, name, n, values)                                                            \
    do {                                                                                                     \
        if ((p)->u##name >= 0 &&                                                                             \
            (!(p)->set || memcmp(values, (p)->l##name, (size_t)(n) * 4 * sizeof(values[0])) != 0)) {        \
            memcpy((p)->l##name, values, (size_t)(n) * 4 * sizeof(values[0]));                              \
            fn((p)->u##name, n, values);                                                                     \
        }                                                                                                    \
    } while (0)

/* Mirrored for the reversed depth. */
static const GLenum kDepthFunc[8] = {GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS};

/* Sets everything a draw needs but its XF state and vertices. */
static void apply_draw_state(const GXDrawState* st) {
    GLint cin[64], ain[64], cop[64], aop[64], map[64], sel[64], swap[16], alpha[4];
    GLfloat reg[16], konst[16], aref[4], fogf[4], fogc[4];
    GLint fog[4];
    GLint ind[4] = {0}, cmd[16] = {0}, istage[16] = {0}, mtx[24] = {0}, size[16] = {0};
    GLint key[SPEC_MAX_KEY];
    DrawProg* P;
    int i, n, all = !sGl.valid;
    sMsDirty = 1;
    if (all) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
        p_glBindVertexArray(sVao);
        p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        p_glBindBuffer(GL_UNIFORM_BUFFER, sXfUbo);
    }

    for (i = 0; i < st->num_stages; i++) {
        const GXTevStage* s = &st->stage[i];
        int k;
        for (k = 0; k < 4; k++) {
            cin[4 * i + k] = s->color_in[k];
            ain[4 * i + k] = s->alpha_in[k];
        }
        cop[4 * i + 0] = s->color_op;
        cop[4 * i + 1] = s->color_bias;
        cop[4 * i + 2] = s->color_scale;
        cop[4 * i + 3] = s->color_clamp;
        aop[4 * i + 0] = s->alpha_op;
        aop[4 * i + 1] = s->alpha_bias;
        aop[4 * i + 2] = s->alpha_scale;
        aop[4 * i + 3] = s->alpha_clamp;
        map[4 * i + 0] = s->texmap;
        map[4 * i + 1] = s->texcoord;
        map[4 * i + 2] = s->tex_enable;
        /* (The bump alphas, 5 and 6, with an indirect stage to give them.) */
        map[4 * i + 3] = s->channel == 7 || (st->ind_any && (s->channel == 5 || s->channel == 6)) ? s->channel : (s->channel & 1);
        sel[4 * i + 0] = s->color_dest;
        sel[4 * i + 1] = s->alpha_dest;
        sel[4 * i + 2] = s->kcolor_sel | (s->kalpha_sel << 5);
        sel[4 * i + 3] = s->tex_swap | (s->ras_swap << 2);
    }
    for (i = 0; i < 4; i++) {
        int k;
        for (k = 0; k < 4; k++) {
            reg[4 * i + k] = st->tev_reg[i][k];
            konst[4 * i + k] = st->konst[i][k];
            swap[4 * i + k] = st->swap_table[i][k];
        }
    }
    alpha[0] = st->alpha_func[0];
    alpha[1] = st->alpha_func[1];
    alpha[2] = st->alpha_logic;
    alpha[3] = 0;
    aref[0] = (float)st->alpha_ref[0];
    aref[1] = (float)st->alpha_ref[1];
    aref[2] = aref[3] = 0;
    fog[0] = st->fog_type;
    fog[1] = st->fog_ortho;
    fog[2] = st->fog_b_mag;
    fog[3] = st->fog_b_shift;
    fogf[0] = st->fog_a;
    fogf[1] = st->fog_c;
    fogf[2] = fogf[3] = 0;
    fogc[0] = st->fog_color[0];
    fogc[1] = st->fog_color[1];
    fogc[2] = st->fog_color[2];
    fogc[3] = 1;
    if (st->ind_any) {
        int k;
        ind[0] = st->ind_stages;
        ind[1] = 1;
        for (i = 0; i < st->num_stages; i++) cmd[i] = (GLint)st->ind_cmd[i];
        for (i = 0; i < 4; i++) {
            istage[4 * i] = st->ind_map[i];
            istage[4 * i + 1] = st->ind_coord[i];
            istage[4 * i + 2] = st->ind_scale[i][0];
            istage[4 * i + 3] = st->ind_scale[i][1];
        }
        for (i = 0; i < 3; i++) {
            for (k = 0; k < 3; k++) {
                mtx[8 * i + k] = st->ind_mtx[i][0][k];
                mtx[8 * i + 4 + k] = st->ind_mtx[i][1][k];
            }
            mtx[8 * i + 3] = mtx[8 * i + 7] = 17 - st->ind_mtx_scale[i];
        }
        for (i = 0; i < 8; i++) {
            size[2 * i] = st->tc_size[i][0];
            size[2 * i + 1] = st->tc_size[i][1];
        }
    }

    /* The program: the one specialised for this TEV setup once it's
     * compiled, else the ubershader. The key's layout is spec_source's. */
    P = NULL;
    if (sSpecOn) {
        n = 0;
        key[n++] = st->num_stages;
        for (i = 0; i < st->num_stages; i++) {
            memcpy(key + n, cin + 4 * i, 16), n += 4;
            memcpy(key + n, ain + 4 * i, 16), n += 4;
            memcpy(key + n, cop + 4 * i, 16), n += 4;
            memcpy(key + n, aop + 4 * i, 16), n += 4;
            memcpy(key + n, map + 4 * i, 16), n += 4;
            memcpy(key + n, sel + 4 * i, 16), n += 4;
        }
        memcpy(key + n, swap, 64), n += 16;
        key[n++] = alpha[0], key[n++] = alpha[1], key[n++] = alpha[2];
        key[n++] = fog[0], key[n++] = fog[1], key[n++] = ind[0], key[n++] = ind[1];
        if (ind[1]) memcpy(key + n, cmd, 64), n += 16, memcpy(key + n, istage, 64), n += 16;
        P = spec_get(key, n);
    }
    if (P) g_spec_draws++;
    else P = &sUber, g_uber_draws++;
    if (all || sProgram != P->id) {
        sProgram = P->id;
        p_glUseProgram(sProgram);
    }

    if (P->uNumStages >= 0 && (!P->set || P->stages != st->num_stages)) {
        p_glUniform1i(P->uNumStages, st->num_stages);
        P->stages = st->num_stages;
        P->set = 0; /* the arrays' lengths changed: upload them all */
    }
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Cin, st->num_stages, cin);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Ain, st->num_stages, ain);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Cop, st->num_stages, cop);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Aop, st->num_stages, aop);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Map, st->num_stages, map);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Sel, st->num_stages, sel);
    UNIFORM_IF_CHANGED(p_glUniform4fv, P, Reg, 4, reg);
    UNIFORM_IF_CHANGED(p_glUniform4fv, P, Konst, 4, konst);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Swap, 4, swap);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Alpha, 1, alpha);
    UNIFORM_IF_CHANGED(p_glUniform4fv, P, AlphaRef, 1, aref);
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Fog, 1, fog);
    UNIFORM_IF_CHANGED(p_glUniform4fv, P, FogF, 1, fogf);
    UNIFORM_IF_CHANGED(p_glUniform4fv, P, FogColor, 1, fogc);
    {
        GLfloat range[16] = {0};
        if (st->fog_range) {
            range[0] = (GLfloat)(1.0 / sScaleX);
            range[1] = -st->fog_center;
            range[2] = 1;
            for (i = 0; i < 10; i++) range[4 + i] = st->fog_k[i];
        }
        UNIFORM_IF_CHANGED(p_glUniform4fv, P, FogRange, 4, range);
    }
    {
        GLfloat wide[4] = {st->wide_sx, st->wide_ox, st->wide_sy, 0};
        UNIFORM_IF_CHANGED(p_glUniform4fv, P, Wide, 1, wide);
    }
    {
        /* Per-pixel lighting (Graphics; [graphics] per_pixel_lighting). */
        GLint ppl[4] = {g_x[X_PER_PIXEL_LIGHTING] != 0, 0, 0, 0};
        UNIFORM_IF_CHANGED(p_glUniform4iv, P, Ppl, 1, ppl);
    }
    UNIFORM_IF_CHANGED(p_glUniform4iv, P, Ind, 1, ind);
    if (st->ind_any) {
        UNIFORM_IF_CHANGED(p_glUniform4iv, P, IndCmd, 4, cmd);
        UNIFORM_IF_CHANGED(p_glUniform4iv, P, IndStage, 4, istage);
        UNIFORM_IF_CHANGED(p_glUniform4iv, P, IndMtx, 6, mtx);
        UNIFORM_IF_CHANGED(p_glUniform4iv, P, TcSize, 4, size);
    }
    P->set = 1;

    for (i = 0; i < 8; i++) {
        GLuint t = st->texture[i] ? st->texture[i] : sWhite;
        if (all || sGl.tex[i] != t) {
            p_glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, t);
            sGl.tex[i] = t;
        }
    }
    p_glActiveTexture(GL_TEXTURE0);

    {
        int blend = st->blend_enable;
        GLenum src = blend_factor(kBlendSrc[st->blend_src], st->efb_alpha);
        GLenum dst = blend_factor(kBlendDst[st->blend_dst], st->efb_alpha);
        GLenum eq = st->blend_subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        if (st->blend_subtract) src = dst = GL_ONE;
        if (all || sGl.blend != blend) {
            if (blend) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            sGl.blend = blend;
        }
        if (blend && (all || sGl.blend_src != src || sGl.blend_dst != dst)) {
            glBlendFunc(src, dst);
            sGl.blend_src = src;
            sGl.blend_dst = dst;
        }
        if (blend && (all || sGl.blend_eq != eq)) {
            p_glBlendEquation(eq);
            sGl.blend_eq = eq;
        }
        if (!blend && all) sGl.blend_src = sGl.blend_dst = sGl.blend_eq = 0; /* set again when blending is on */
    }
    {
        GLboolean c = (GLboolean)st->color_update, a = (GLboolean)(st->alpha_update && st->efb_alpha);
        GLboolean r = c, gb = c;
        if (all || sGl.color_mask[0] != r || sGl.color_mask[1] != gb || sGl.color_mask[3] != a) {
            glColorMask(r, gb, gb, a);
            sGl.color_mask[0] = r;
            sGl.color_mask[1] = sGl.color_mask[2] = gb;
            sGl.color_mask[3] = a;
        }
    }
    {
        int depth = st->z_enable;
        GLboolean mask = depth && st->z_update ? GL_TRUE : GL_FALSE;
        if (all || sGl.depth != depth) {
            if (depth) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            sGl.depth = depth;
        }
        if (depth && (all || sGl.depth_func != kDepthFunc[st->z_func])) {
            glDepthFunc(kDepthFunc[st->z_func]);
            sGl.depth_func = kDepthFunc[st->z_func];
        }
        if (!depth && all) sGl.depth_func = 0;
        if (all || sGl.depth_mask != mask) {
            glDepthMask(mask);
            sGl.depth_mask = mask;
        }
    }
    {
        /* Edges are scaled and rounded, rather than the size, so
         * neighbouring rectangles still meet at a fractional scale. */
        GLint vp[4], sc[4];
        double dr[2];
        vp[0] = GLX(st->viewport[0]);
        vp[1] = GLY(EFB_H - (st->viewport[1] + st->viewport[3]));
        vp[2] = GLX(st->viewport[0] + st->viewport[2]) - GLX(st->viewport[0]);
        vp[3] = GLY(EFB_H - st->viewport[1]) - GLY(EFB_H - (st->viewport[1] + st->viewport[3]));
        dr[0] = 1.0 - (st->viewport[5] > 1 ? 1 : st->viewport[5]);
        dr[1] = 1.0 - (st->viewport[4] < 0 ? 0 : st->viewport[4]);
        sc[0] = GLX(st->scissor[0]);
        sc[1] = GLY(EFB_H - st->scissor[3]);
        sc[2] = GLX(st->scissor[2]) - GLX(st->scissor[0]);
        sc[3] = GLY(EFB_H - st->scissor[1]) - GLY(EFB_H - st->scissor[3]);
        if (all || memcmp(vp, sGl.viewport, sizeof(vp)) != 0) {
            glViewport(vp[0], vp[1], vp[2], vp[3]);
            memcpy(sGl.viewport, vp, sizeof(vp));
        }
        if (all || memcmp(dr, sGl.depth_range, sizeof(dr)) != 0) {
            glDepthRange(dr[0], dr[1]);
            memcpy(sGl.depth_range, dr, sizeof(dr));
        }
        if (all) glEnable(GL_SCISSOR_TEST);
        if (all || memcmp(sc, sGl.scissor, sizeof(sc)) != 0) {
            glScissor(sc[0], sc[1], sc[2], sc[3]);
            memcpy(sGl.scissor, sc, sizeof(sc));
        }
    }
    /* Face culling (GX_CULL_NONE, FRONT, BACK, ALL). GX's front faces wind
     * counter-clockwise in GL's window coordinates, GL's default. Games
     * rely on it: the picture confirmation dialog hides a placeholder pane
     * by turning it away from the camera. */
    {
        static const GLenum kCull[4] = {GL_BACK, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
        int cull = st->cull_mode != 0;
        if (all || sGl.cull != cull) {
            if (cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            sGl.cull = cull;
        }
        if (cull && (all || sGl.cull_face != kCull[st->cull_mode])) {
            glCullFace(kCull[st->cull_mode]);
            sGl.cull_face = kCull[st->cull_mode];
        }
        if (!cull && all) sGl.cull_face = 0;
    }
    sGl.valid = 1;
}

/* The XF uniform buffer holds what the decoder's draws last left in it,
 * unless a recorded frame was drawn since: then its next draw sends all of
 * it (sXfStale), and the recording's draws go by their own copy of what
 * it holds (sXfShadow). */
static int sXfStale;
static uint32_t sXfShadow[GX_XF_WORDS];
static int sXfShadowValid;

/* The vertex arena (gxb_arena): vertices kept in a buffer on the GPU from
 * frame to frame, so a display list's are decoded and sent once, not every
 * time it is called (gx_fifo.c; nearly all of the park's 220,000 vertices
 * a frame). Handed out in spans; the free spans are kept in address order,
 * joined where they touch. Writes go to a copy in memory and reach the
 * buffer as one range before the next draw from it; OpenGL keeps them in
 * order with the draws, so a span is free again as soon as it is let go. */
typedef struct Span {
    int first, n;
} Span;
static GXVertex* sArenaMem;
static int sArenaVerts, sArenaSize; /* vertices in the copy; in the buffer */
/* The parts of the copy written since the buffer was last brought up to
 * date: separate ranges, as freed spans are reused anywhere in the arena.
 * (One range from the lowest write to the highest sent tens of MB at a
 * time when lists were decoded far apart, as turning the camera does.) */
#define ARENA_DIRTY 64
static struct { int lo, hi; } sArenaDirty[ARENA_DIRTY];
static int sArenaDirtyCount;
static Span* sSpans;
static int sSpanCount, sSpanCap;

static void span_add(int first, int n) {
    int lo = 0, hi = sSpanCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (sSpans[mid].first < first) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0 && sSpans[lo - 1].first + sSpans[lo - 1].n == first) {
        sSpans[lo - 1].n += n;
        if (lo < sSpanCount && sSpans[lo - 1].first + sSpans[lo - 1].n == sSpans[lo].first) {
            sSpans[lo - 1].n += sSpans[lo].n;
            memmove(sSpans + lo, sSpans + lo + 1, (size_t)(sSpanCount - lo - 1) * sizeof(Span));
            sSpanCount--;
        }
        return;
    }
    if (lo < sSpanCount && first + n == sSpans[lo].first) {
        sSpans[lo].first = first;
        sSpans[lo].n += n;
        return;
    }
    if (sSpanCount == sSpanCap) {
        sSpanCap = sSpanCap ? sSpanCap * 2 : 256;
        sSpans = (Span*)realloc(sSpans, (size_t)sSpanCap * sizeof(Span));
    }
    memmove(sSpans + lo + 1, sSpans + lo, (size_t)(sSpanCount - lo) * sizeof(Span));
    sSpans[lo].first = first;
    sSpans[lo].n = n;
    sSpanCount++;
}

static int gl_arena_alloc(int n) {
    int i, first;
    if (n <= 0) return 0;
    for (;;) {
        for (i = 0; i < sSpanCount && sSpans[i].n < n; i++) {}
        if (i < sSpanCount) break;
        {
            /* Twice as big (the buffer follows at the next draw). */
            int verts = sArenaVerts ? sArenaVerts * 2 : 1 << 20; /* 108 MB: the park needs about that */
            while (verts < sArenaVerts + n) verts *= 2;
            sArenaMem = (GXVertex*)realloc(sArenaMem, (size_t)verts * sizeof(GXVertex));
            span_add(sArenaVerts, verts - sArenaVerts);
            sArenaVerts = verts;
        }
    }
    first = sSpans[i].first;
    sSpans[i].first += n;
    sSpans[i].n -= n;
    if (!sSpans[i].n) {
        memmove(sSpans + i, sSpans + i + 1, (size_t)(sSpanCount - i - 1) * sizeof(Span));
        sSpanCount--;
    }
    return first;
}

static void gl_arena_write(int first, const GXVertex* v, int n) {
    if (n <= 0) return;
    memcpy(sArenaMem + first, v, (size_t)n * sizeof(GXVertex));
    {
        int i, lo = first, hi = first + n;
        /* Joined to a range it touches or overlaps (usually the last). */
        for (i = sArenaDirtyCount - 1; i >= 0; i--) {
            if (lo <= sArenaDirty[i].hi && hi >= sArenaDirty[i].lo) {
                if (lo < sArenaDirty[i].lo) sArenaDirty[i].lo = lo;
                if (hi > sArenaDirty[i].hi) sArenaDirty[i].hi = hi;
                return;
            }
        }
        if (sArenaDirtyCount == ARENA_DIRTY) {
            /* Full: the two ranges closest together made one. */
            int a = 0, b = 1, j;
            long long best = -1;
            for (i = 0; i < sArenaDirtyCount; i++)
                for (j = i + 1; j < sArenaDirtyCount; j++) {
                    long long gap = sArenaDirty[i].lo > sArenaDirty[j].hi ? (long long)sArenaDirty[i].lo - sArenaDirty[j].hi
                                                                          : (long long)sArenaDirty[j].lo - sArenaDirty[i].hi;
                    if (best < 0 || gap < best) best = gap, a = i, b = j;
                }
            if (sArenaDirty[b].lo < sArenaDirty[a].lo) sArenaDirty[a].lo = sArenaDirty[b].lo;
            if (sArenaDirty[b].hi > sArenaDirty[a].hi) sArenaDirty[a].hi = sArenaDirty[b].hi;
            sArenaDirty[b] = sArenaDirty[--sArenaDirtyCount];
        }
        sArenaDirty[sArenaDirtyCount].lo = lo;
        sArenaDirty[sArenaDirtyCount].hi = hi;
        sArenaDirtyCount++;
    }
}

static void gl_arena_free(int first, int n) {
    if (n > 0) span_add(first, n);
}

/* The buffer brought up to date with the copy, before a draw from it. */
static void arena_upload(void) {
    if (sArenaSize != sArenaVerts) {
        /* Grown: a new buffer, with the old one's contents copied over on
         * the GPU (sent again from memory, 216 MB took 48 ms and left the
         * driver busy for 300 ms more), then the writes since. */
        GLuint b;
        p_glGenBuffers(1, &b);
        p_glBindBuffer(GL_COPY_WRITE_BUFFER, b);
        p_glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)sArenaVerts * sizeof(GXVertex), NULL, GL_DYNAMIC_DRAW);
        if (sArenaSize) {
            p_glBindBuffer(GL_COPY_READ_BUFFER, sArenaVbo);
            p_glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                                  (GLsizeiptr)sArenaSize * sizeof(GXVertex));
        }
        p_glDeleteBuffers(1, &sArenaVbo);
        sArenaVbo = b;
        arena_vao(b);
        sBoundVao = 0;
        if (getenv("RECOMP_GX_CACHE_LOG"))
            fprintf(stderr, "gl: vertex arena now %d vertices (%.0f MB)\n", sArenaVerts,
                    sArenaVerts * (double)sizeof(GXVertex) / 1048576.0);
        sArenaSize = sArenaVerts;
    }
    if (sArenaDirtyCount) {
        int i;
        p_glBindBuffer(GL_ARRAY_BUFFER, sArenaVbo);
        for (i = 0; i < sArenaDirtyCount; i++)
            p_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)sArenaDirty[i].lo * sizeof(GXVertex),
                              (GLsizeiptr)(sArenaDirty[i].hi - sArenaDirty[i].lo) * sizeof(GXVertex),
                              sArenaMem + sArenaDirty[i].lo);
        p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    }
    sArenaDirtyCount = 0;
}


static void use_vao(GLuint vao, int fresh) {
    if (fresh || vao != sBoundVao) {
        p_glBindVertexArray(vao);
        sBoundVao = vao;
    }
}

static void gl_draw(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
              int first_index, int count) {
    int fresh = !sGl.valid; /* apply_draw_state binds the per-draw vertex array then */
    apply_draw_state(st);
    if (st->xf && sXfStale) {
        p_glBufferSubData(GL_UNIFORM_BUFFER, 0, GX_XF_WORDS * 4, st->xf);
        sXfStale = 0;
    } else if (st->xf && st->xf_dirty_lo < st->xf_dirty_hi) {
        /* Only the XF words written since the last draw. */
        p_glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)st->xf_dirty_lo * 4,
                          (GLsizeiptr)(st->xf_dirty_hi - st->xf_dirty_lo) * 4, st->xf + st->xf_dirty_lo);
    }
    sXfShadowValid = 0;
    if (!v) {
        /* From the arena, by index from its start. */
        arena_upload();
        use_vao(sArenaVao, fresh);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * sizeof(uint32_t), index + first_index, GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, NULL);
        return;
    }
    use_vao(sVao, fresh);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertices * sizeof(GXVertex), v + first_vertex, GL_STREAM_DRAW);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * sizeof(uint32_t), index + first_index, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, NULL);
}

/* The frame's vertices and indices go to buffers made again each frame at
 * a size that only grows. Made at the frame's own size, a little
 * different every frame (24 MB of vertices in the park), the driver kept
 * the old ones' memory: RAM and GPU memory grew by 24 MB at a time, to
 * gigabytes. */
static GLsizeiptr sFrameVbSize, sFrameIbSize;

static void frame_buffer(GLenum target, GLsizeiptr* size, GLsizeiptr bytes, const void* data) {
    if (bytes > *size) *size = (bytes + bytes / 4 + 0xFFFFF) & ~(GLsizeiptr)0xFFFFF;
    p_glBufferData(target, *size, NULL, GL_STREAM_DRAW);
    p_glBufferSubData(target, 0, bytes, data);
}

/* A recorded frame's vertices are the arena's when v is NULL. */
static int sFrameFromArena;

static void gl_frame_data(const GXVertex* v, int vertices, const uint32_t* index, int count) {
    sFrameFromArena = !v;
    if (!v) {
        arena_upload();
        p_glBindVertexArray(sArenaVao);
    } else {
        p_glBindVertexArray(sVao);
        p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        frame_buffer(GL_ARRAY_BUFFER, &sFrameVbSize, (GLsizeiptr)vertices * sizeof(GXVertex), v);
    }
    frame_buffer(GL_ELEMENT_ARRAY_BUFFER, &sFrameIbSize, (GLsizeiptr)count * sizeof(uint32_t), index);
    sBoundVao = 0; /* bound again by the next draw */
}

/* Sends the words of xf that differ from what the buffer holds. */
static void upload_xf(const uint32_t* xf) {
    int lo = 0, hi = GX_XF_WORDS;
    if (sXfShadowValid) {
        while (lo < GX_XF_WORDS && xf[lo] == sXfShadow[lo]) lo++;
        if (lo == GX_XF_WORDS) return;
        while (hi > lo && xf[hi - 1] == sXfShadow[hi - 1]) hi--;
    }
    p_glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)lo * 4, (GLsizeiptr)(hi - lo) * 4, xf + lo);
    memcpy(sXfShadow + lo, xf + lo, (size_t)(hi - lo) * 4);
    sXfShadowValid = 1;
    sXfStale = 1;
}

static void gl_draw_recorded(const GXDrawState* st, const uint32_t* xf, int first_vertex, int first_index, int count) {
    int fresh = !sGl.valid;
    apply_draw_state(st);
    if (sFrameFromArena) arena_upload();
    use_vao(sFrameFromArena ? sArenaVao : sVao, fresh);
    upload_xf(xf);
    p_glDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_INT, (void*)((size_t)first_index * 4), first_vertex);
}

static void gl_clear(int color, int alpha, int depth, uint32_t rgba, uint32_t z) {
    GLbitfield mask = 0;
    gl_forget();
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    sMsDirty = 1;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(color, color, color, alpha);
    glDepthMask(depth ? GL_TRUE : GL_FALSE);
    glClearColor(((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                 (rgba & 0xFF) / 255.0f);
    glClearDepth(1.0 - z / 16777215.0);
    if (color || alpha) mask |= GL_COLOR_BUFFER_BIT;
    if (depth) mask |= GL_DEPTH_BUFFER_BIT;
    if (mask) glClear(mask);
}

/* EFB copies to textures. GX textures have row 0 at the top while the EFB
 * framebuffer has it at the bottom, so the copy flips. */
typedef struct CopyTarget {
    uint32_t addr;
    int w, h;
    GLuint tex, fbo;
} CopyTarget;
static CopyTarget sTargets[64];
static int sTargetCount;

static int gl_copy_target(uint32_t addr, int w, int h) {
    CopyTarget* t;
    int i;
    gl_forget();
    for (i = 0; i < sTargetCount; i++) {
        if (sTargets[i].addr == addr && sTargets[i].w == w && sTargets[i].h == h) return i;
    }
    if (sTargetCount == 64) return -1;
    t = &sTargets[sTargetCount];
    t->addr = addr;
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLX(w), GLY(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glGenFramebuffers(1, &t->fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    return sTargetCount++;
}

static uint32_t gl_copy_texture(int target) { return sTargets[target].tex; }

/* The copy's texture read back (gx_frames.c's write backs). */
static uint8_t* gl_copy_read(int target, int* w, int* h) {
    extern unsigned g_cnt_readbacks; /* debug overlay */
    g_cnt_readbacks++;
    CopyTarget* t = &sTargets[target];
    uint8_t* out;
    gl_forget();
    *w = GLX(t->w);
    *h = GLY(t->h);
    out = (uint8_t*)malloc((size_t)*w * *h * 4);
    if (!out) return NULL;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return out;
}

/* Draws the EFB rectangle into the copy's texture. */
static void gl_copy(int target, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha) {
    CopyTarget* t = &sTargets[target];
    GLint mode[4];
    GLfloat src[4];
    gl_forget();
    resolve();
    /* Row 0 of the texture is the top row of the rectangle. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, GLX(w), GLY(h));
    p_glUseProgram(sCopyProgram);
    p_glBindVertexArray(sCopyVao);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    src[0] = (float)x / EFB_W;
    src[1] = (float)(EFB_H - y) / EFB_H;
    src[2] = (float)(x + w) / EFB_W;
    src[3] = (float)(EFB_H - y - h) / EFB_H;
    p_glUniform4fv(uCopySrc, 1, src);
    mode[0] = fmt;
    mode[1] = intensity;
    mode[2] = efb_alpha;
    mode[3] = 0;
    p_glUniform4iv(uCopyMode, 1, mode);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
}

static void alloc_efb(void) {
    int w = GLX(EFB_W), h = GLY(EFB_H), i;
    gl_forget();
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glBindRenderbuffer(GL_RENDERBUFFER, sEfbDepth);
    /* Depth is stored reversed, 1 - GX depth, in a float buffer: GX
     * scenes put their depths within a few millionths of 1, where a 24-bit
     * buffer fed by float math can't tell surfaces one GX step apart (the
     * picture in the picture confirmation dialog lost to its frame). */
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, sMsaa > 1 ? 1 : w, sMsaa > 1 ? 1 : h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sEfbFbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEfbColor, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMsaa > 1 ? 0 : sEfbDepth);
    if (sMsFbo) {
        p_glDeleteFramebuffers(1, &sMsFbo);
        p_glDeleteRenderbuffers(1, &sMsColor);
        p_glDeleteRenderbuffers(1, &sMsDepth);
        sMsFbo = sMsColor = sMsDepth = 0;
    }
    sDrawFbo = sEfbFbo;
    if (sMsaa > 1) {
        p_glGenRenderbuffers(1, &sMsColor);
        p_glBindRenderbuffer(GL_RENDERBUFFER, sMsColor);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, sMsaa, GL_RGBA8, w, h);
        p_glGenRenderbuffers(1, &sMsDepth);
        p_glBindRenderbuffer(GL_RENDERBUFFER, sMsDepth);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, sMsaa, GL_DEPTH_COMPONENT32F, w, h);
        p_glGenFramebuffers(1, &sMsFbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, sMsFbo);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sMsColor);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMsDepth);
        sDrawFbo = sMsFbo;
    }
    for (i = 0; i < sTargetCount; i++) {
        glBindTexture(GL_TEXTURE_2D, sTargets[i].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLX(sTargets[i].w), GLY(sTargets[i].h), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0); /* the far plane, reversed */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    sMsDirty = 0;
}

static void dump_bmp(const char* path, int w, int h) {
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    FILE* f = fopen(path, "wb");
    uint8_t hdr[54] = {'B', 'M'};
    int i;
    if (!f) {
        free(px);
        return;
    }
    glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    *(uint32_t*)(hdr + 2) = 54 + w * h * 4;
    hdr[10] = 54;
    hdr[14] = 40;
    *(int32_t*)(hdr + 18) = w;
    *(int32_t*)(hdr + 22) = h;
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
    free(px);
}

/* ---- Gallery > Save a picture: the viewer's stage as shown, into a PNG in
 * Pictures\Wonderpark. The renderer reads the square back after drawing a
 * frame (gl_picture_capture); a thread of its own writes the file (never
 * the game's or the renderer's: the drive may be a spinning disk). */
static volatile LONG sPicWant;
static int sPicRect[4];
static wchar_t sPicName[80];
volatile int g_picture_saved; /* 1 saved, 2 failed (settings_ui.cpp shows it) */

void gl_request_picture(int x, int y, int w, int h, const wchar_t* name) {
    sPicRect[0] = x, sPicRect[1] = y, sPicRect[2] = w, sPicRect[3] = h;
    _snwprintf(sPicName, 79, L"%ls", name ? name : L"Pokemon");
    sPicName[79] = 0;
    g_picture_saved = 0;
    InterlockedExchange(&sPicWant, 1);
}

typedef struct {
    uint8_t* rgb; /* top row first */
    int w, h;
    wchar_t name[80];
} PicJob;

static uint32_t png_crc(uint32_t c, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    size_t i;
    if (!table[1]) {
        uint32_t k, b, v;
        for (k = 0; k < 256; k++) {
            for (v = k, b = 0; b < 8; b++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
    }
    for (i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    uint32_t c = png_crc(0xFFFFFFFFu, (const uint8_t*)type, 4);
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    c = ~png_crc(c, data, n);
    be[0] = (uint8_t)(c >> 24), be[1] = (uint8_t)(c >> 16), be[2] = (uint8_t)(c >> 8), be[3] = (uint8_t)c;
    fwrite(be, 1, 4, f);
}

/* A PNG with the image stored (zlib's "stored" blocks): no compressor needed. */
static int write_png(const wchar_t* path, const uint8_t* rgb, int w, int h) {
    size_t row = (size_t)w * 3 + 1, raw = row * h, blocks = (raw + 65534) / 65535, i, at = 0;
    size_t zn = 2 + raw + blocks * 5 + 4;
    uint8_t *z = (uint8_t*)malloc(zn), ihdr[13];
    uint32_t a = 1, b = 0;
    FILE* f;
    size_t zi = 0, k;
    if (!z) return 0;
    z[zi++] = 0x78, z[zi++] = 0x01;
    for (i = 0; i < blocks; i++) {
        size_t n = raw - at < 65535 ? raw - at : 65535;
        z[zi++] = (uint8_t)(i + 1 == blocks);
        z[zi++] = (uint8_t)n, z[zi++] = (uint8_t)(n >> 8), z[zi++] = (uint8_t)~n, z[zi++] = (uint8_t)(~n >> 8);
        for (k = 0; k < n; k++, at++) {
            size_t y = at / row, x = at % row;
            uint8_t v = x == 0 ? 0 : rgb[y * w * 3 + x - 1]; /* each row: filter 0, then its pixels */
            z[zi++] = v;
            a = (a + v) % 65521, b = (b + a) % 65521;
        }
    }
    z[zi++] = (uint8_t)(b >> 8), z[zi++] = (uint8_t)b, z[zi++] = (uint8_t)(a >> 8), z[zi++] = (uint8_t)a;
    ihdr[0] = (uint8_t)(w >> 24), ihdr[1] = (uint8_t)(w >> 16), ihdr[2] = (uint8_t)(w >> 8), ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24), ihdr[5] = (uint8_t)(h >> 16), ihdr[6] = (uint8_t)(h >> 8), ihdr[7] = (uint8_t)h;
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    f = _wfopen(path, L"wb");
    if (!f) {
        free(z);
        return 0;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zi);
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}

static DWORD WINAPI picture_thread(LPVOID arg) {
    PicJob* j = (PicJob*)arg;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 160];
    SYSTEMTIME t;
    int ok = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, dir))) {
        wcscat(dir, L"\\Wonderpark");
        CreateDirectoryW(dir, NULL);
        GetLocalTime(&t);
        _snwprintf(path, MAX_PATH + 159, L"%ls\\%ls %04d-%02d-%02d %02d-%02d-%02d.png", dir, j->name, t.wYear, t.wMonth,
                   t.wDay, t.wHour, t.wMinute, t.wSecond);
        path[MAX_PATH + 159] = 0;
        ok = write_png(path, j->rgb, j->w, j->h);
    }
    g_picture_saved = ok ? 1 : 2;
    free(j->rgb);
    free(j);
    return 0;
}

/* After a frame is drawn, before it's shown: the stage asked for, read back
 * from the window's framebuffer (bottom row first) and handed to a thread. */
static void gl_picture_capture(GLuint fbo, int winW, int winH) {
    int x, y, w, h, r;
    uint8_t* px;
    PicJob* j;
    HANDLE t;
    if (!sPicWant || !InterlockedExchange(&sPicWant, 0)) return;
    x = sPicRect[0], w = sPicRect[2], h = sPicRect[3];
    y = winH - (sPicRect[1] + h); /* GL's rows start at the bottom */
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > winW) w = winW - x;
    if (y + h > winH) h = winH - y;
    j = (PicJob*)calloc(1, sizeof(*j));
    px = (uint8_t*)malloc((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1) * 3);
    if (w <= 0 || h <= 0 || !j || !px) {
        free(j), free(px);
        g_picture_saved = 2;
        return;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    j->rgb = (uint8_t*)malloc((size_t)w * h * 3);
    if (!j->rgb) {
        free(j), free(px);
        g_picture_saved = 2;
        return;
    }
    for (r = 0; r < h; r++) memcpy(j->rgb + (size_t)r * w * 3, px + (size_t)(h - 1 - r) * w * 3, (size_t)w * 3); /* top first */
    free(px);
    j->w = w, j->h = h;
    memcpy(j->name, sPicName, sizeof(j->name));
    t = CreateThread(NULL, 0, picture_thread, j, 0, NULL);
    if (t) CloseHandle(t);
    else free(j->rgb), free(j), g_picture_saved = 2;
}

/* Where the game's picture is in the window: x, y, width, height. */
int g_picture_rect[4];

/* The last picture shown, before the overlay (the F1 tab's pause shows it
 * again: gl_present_last). One blit a frame. */
static GLuint sLastFbo, sLastTex;
static int sLastW, sLastH, sHaveLast;

static void keep_last(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!sLastFbo) {
        p_glGenFramebuffers(1, &sLastFbo);
        glGenTextures(1, &sLastTex);
        glBindTexture(GL_TEXTURE_2D, sLastTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (w != sLastW || h != sLastH) {
        glBindTexture(GL_TEXTURE_2D, sLastTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glBindFramebuffer(GL_FRAMEBUFFER, sLastFbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sLastTex, 0);
        sLastW = w, sLastH = h;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sWinFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sLastFbo);
    p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    sHaveLast = 1;
}

/* The kept picture (sLastTex) drawn into the window's framebuffer, w x h,
 * with the look settings (kLookFragmentShader). */
static void draw_look(int w, int h) {
    float b = g_x[X_BRIGHTNESS] / 100.0f - 1.0f, g = g_x[X_GAMMA] / 100.0f;
    int cb = g_x[X_COLORBLIND];
    p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glUseProgram(sLookProgram);
    p_glBindVertexArray(sCopyVao);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sLastTex);
    {
        /* Brightness 50-150%: down to 70% as dim, or the shadows up to 15%
         * of the way to white. Gamma 50-150%: midtones darker to lighter. */
        float look[4] = {1.0f / (g > 0.1f ? g : 0.1f), g_x[X_COLORS] ? 1.3f : 1.0f, g_x[X_COLORS] ? 1.06f : 1.0f,
                         g_x[X_MIRROR_WORLD] ? 1.0f : 0.0f};
        float look2[4] = {b >= 0 ? b * 0.3f : b * 0.6f, cb >= 1 && cb <= 3 ? 1.0f : 0.0f, 0, 0};
        p_glUniform4fv(uLook, 1, look);
        p_glUniform4fv(uLook2, 1, look2);
        {
            /* Retro look: square pixels about 240 to the picture's height. */
            float block = h > 240 ? (float)h / 240.0f : 1;
            float look3[4] = {(float)g_x[X_COLOR_FILTER], (float)g_x[X_RETRO_LOOK], w > 0 ? block / w : 0, h > 0 ? block / h : 0};
            p_glUniform4fv(uLook3, 1, look3);
        }
        {
            float look4[4] = {sGrade[0], sGrade[1], sGrade[2], 0};
            p_glUniform4fv(uLook4, 1, look4);
        }
        if (cb >= 1 && cb <= 3) {
            float m[6][4];
            memcpy(m, kCbSim[cb - 1], sizeof(kCbSim[0]));
            memcpy(m[3], cb == 3 ? kCbShiftBY : kCbShiftRG, sizeof(kCbShiftRG));
            p_glUniform4fv(uCb, 6, &m[0][0]);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
}

/* The GPU's own time for everything between two presents, measured with
 * timer queries and read back a few presents later (never waited for):
 * g_gl_gpu_ms sums it, for the RECOMP_FPS report (gx_frames.c). */
double g_gl_gpu_ms;
unsigned g_gl_gpu_n; /* presents measured */
#define GPU_TIMERS 4
static GLuint sGpuQuery[GPU_TIMERS];
static unsigned sGpuQueryAt, sGpuQueryOpen;

static void gpu_timer_end(void) {
    if (!sGpuQueryOpen) return;
    p_glEndQuery(GL_TIME_ELAPSED);
    sGpuQueryOpen = 0;
    sGpuQueryAt++;
}

static void gpu_timer_begin(void) {
    GLuint q;
    if (!p_glGenQueries || !p_glGetQueryObjectui64v) return;
    if (!sGpuQuery[0]) p_glGenQueries(GPU_TIMERS, sGpuQuery);
    q = sGpuQuery[sGpuQueryAt % GPU_TIMERS];
    if (sGpuQueryAt >= GPU_TIMERS) {
        /* The oldest one, ended GPU_TIMERS - 1 presents ago. */
        GLint ready = 0;
        p_glGetQueryObjectiv(q, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready) {
            GLuint64 ns = 0;
            p_glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
            g_gl_gpu_ms += (double)ns / 1e6;
            g_gl_gpu_n++;
        } else {
            return; /* still running: skip a frame rather than wait */
        }
    }
    p_glBeginQuery(GL_TIME_ELAPSED, q);
    sGpuQueryOpen = 1;
}

/* After the picture is in the window's framebuffer: the overlay (debug panel,
 * F1 tab) over it, frame dumps, and presenting. 1 if left out. */
static int finish_present(int w, int h, int may_drop);

/* Shows the last picture again, with the overlay as it now is (the game
 * paused: gx_frames.c gxb_pause_loop). 0 if there is none. */
static int gl_present_last(void) {
    int w, h;
    if (!sHaveLast) return 0;
    gl_forget();
    win_size(&w, &h);
    if (w <= 0 || h <= 0) return 1;
    if (sChain) dx_size(w, h);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sLastFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sWinFbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (sLookProgram && look_needed()) draw_look(w, h);
    else p_glBlitFramebuffer(0, 0, sLastW, sLastH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    finish_present(w, h, 0);
    return 1;
}

static int gl_present(int x, int y, int cw, int ch, int may_drop) {
    int w, h, dw, dh;
    double aspect;
    gl_forget();
    win_size(&w, &h);
    {
        /* Widescreen, a window narrower than 16:9: the sides cropped. */
        extern float g_wide_shown;
        if (g_wide_shown < 1) {
            int keep = (int)(cw * g_wide_shown + 0.5f);
            x += (cw - keep) / 2;
            cw = keep;
        }
    }
    /* The copied rectangle of the EFB, as large as fits in the window at
     * the shape it has on a TV (an EFB pixel is square in 4:3, wider in
     * 16:9), with black bars on the sides left over. */
    aspect = ch > 0 ? cw * sPixelAspect / ch : 1;
    dw = w;
    dh = (int)(w / aspect + 0.5);
    if (dh > h) {
        dh = h;
        dw = (int)(h * aspect + 0.5);
    }
    if (sChain && w > 0 && h > 0) dx_size(w, h);
    {
        /* Where the game's picture is in the window (the Options page's PC
         * rows line up with it: settings_ui.cpp). */
        extern int g_picture_rect[4];
        g_picture_rect[0] = (w - dw) / 2, g_picture_rect[1] = (h - dh) / 2;
        g_picture_rect[2] = dw, g_picture_rect[3] = dh;
    }
    resolve();
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sEfbFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sWinFbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(GLX(x), GLY(EFB_H - y - ch), GLX(x + cw), GLY(EFB_H - y), (w - dw) / 2, (h - dh) / 2,
                        (w - dw) / 2 + dw, (h - dh) / 2 + dh, GL_COLOR_BUFFER_BIT,
                        g_x[X_SCALING_FILTER] ? GL_NEAREST : GL_LINEAR);
    keep_last(w, h);
    if (sLookProgram && look_needed()) draw_look(w, h);
    return finish_present(w, h, may_drop);
}

/* The performance overlay (F1 > Display; [overlay]): perf_overlay.cpp draws
 * its picture on a thread of its own a few times a second; here it's laid
 * over the window, uploaded only when it changed. */
static void draw_fps(int w, int h) {
    extern const uint32_t* perf_overlay_lock(int win_w, int win_h, int* bw, int* bh, int* x, int* y, unsigned* serial);
    extern void perf_overlay_unlock(void);
    extern void perf_overlay_render_time(double ms);
    static GLuint tex;
    static unsigned lastSerial = ~0u;
    static int texW, texH;
    int bw = 0, bh = 0, x0 = 0, ytop = 0, y0;
    unsigned serial = 0;
    const uint32_t* img;
    LARGE_INTEGER t0, t1, fq;
    if (!g_x[X_SHOW_FPS] || h <= 0) return;
    QueryPerformanceCounter(&t0);
    img = perf_overlay_lock(w, h, &bw, &bh, &x0, &ytop, &serial);
    if (!img) return;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    y0 = h - ytop - bh; /* GL counts up from the bottom */
    p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
    p_glUseProgram(sOverlayProgram);
    p_glBindVertexArray(sCopyVao);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (serial != lastSerial) {
        /* The same storage while the size stays (no reallocation). */
        if (bw != texW || bh != texH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_BGRA, GL_UNSIGNED_BYTE, img);
            texW = bw, texH = bh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bw, bh, GL_BGRA, GL_UNSIGNED_BYTE, img);
        }
        lastSerial = serial;
    }
    perf_overlay_unlock();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); /* the picture is premultiplied (perf_overlay.cpp) */
    glViewport(x0, y0, bw, bh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
    gl_forget();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    perf_overlay_render_time((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
}

/* The Gallery's 3D view (gallery3d.cpp): the friend's model, posed and
 * projected on the CPU, drawn into a framebuffer of its own (colour and
 * depth) the size of the viewer's stage, then laid over the window there,
 * over the F1 notebook's picture. Every present, so it moves smoothly while
 * the game is paused. */
static const char* kG3VertexShader =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec4 a_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() { v_uv = a_uv; v_col = a_col; gl_Position = a_pos; }\n";
static const char* kG3FragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform ivec4 u_mode; /* has a texture, alpha test */\n"
    "void main() {\n"
    "    vec4 c = v_col;\n"
    "    if (u_mode.x != 0) c *= texture(u_tex, v_uv);\n"
    "    if (u_mode.y != 0 && c.a < 0.5) discard;\n"
    "    o_color = vec4(min(c.rgb, vec3(1.0)), c.a);\n"
    "}\n";
static const char* kG3BlitVertexShader =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID >> 1) * 2);\n"
    "    v_uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static GLuint sG3Prog, sG3Blit, sG3Vao, sG3Vbo, sG3Fbo, sG3Color, sG3Depth, sG3Tex[64];
static GLint uG3Mode;
static int sG3W, sG3H, sG3TexCount, sG3Broken;
static unsigned sG3Gen = ~0u;

static GLuint g3_program(const char* vs, const char* fs) {
    GLint ok;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    p_glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "gl: gallery 3D link error:\n%s\n", log);
        return 0;
    }
    return p;
}

static void draw_gallery3d(int winW, int winH) {
    G3Frame f;
    int i;
    if (sG3Broken || !gallery3d_frame(&f, winW, winH)) return;
    if (f.w > winW) f.w = winW;
    if (f.h > winH) f.h = winH;
    if (!sG3Prog) {
        sG3Prog = g3_program(kG3VertexShader, kG3FragmentShader);
        sG3Blit = g3_program(kG3BlitVertexShader, kOverlayFragmentShader);
        if (!sG3Prog || !sG3Blit) {
            sG3Broken = 1;
            return;
        }
        p_glUseProgram(sG3Prog);
        p_glUniform1i(p_glGetUniformLocation(sG3Prog, "u_tex"), 0);
        uG3Mode = p_glGetUniformLocation(sG3Prog, "u_mode");
        p_glUseProgram(sG3Blit);
        p_glUniform1i(p_glGetUniformLocation(sG3Blit, "u_image"), 0);
        p_glGenVertexArrays(1, &sG3Vao);
        p_glGenBuffers(1, &sG3Vbo);
        p_glBindVertexArray(sG3Vao);
        p_glBindBuffer(GL_ARRAY_BUFFER, sG3Vbo);
        p_glEnableVertexAttribArray(0);
        p_glEnableVertexAttribArray(1);
        p_glEnableVertexAttribArray(2);
        p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, G3_FLOATS * 4, (void*)0);
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, G3_FLOATS * 4, (void*)16);
        p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, G3_FLOATS * 4, (void*)24);
        p_glGenFramebuffers(1, &sG3Fbo);
        glGenTextures(1, &sG3Color);
        p_glGenRenderbuffers(1, &sG3Depth);
    }
    /* The stage's framebuffer, at its size. */
    if (f.w != sG3W || f.h != sG3H) {
        glBindTexture(GL_TEXTURE_2D, sG3Color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.w, f.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glBindRenderbuffer(GL_RENDERBUFFER, sG3Depth);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, f.w, f.h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, sG3Fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sG3Color, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sG3Depth);
        sG3W = f.w, sG3H = f.h;
    }
    /* The model's textures, when they change (another friend). */
    if (f.texGen != sG3Gen) {
        if (sG3TexCount) glDeleteTextures(sG3TexCount, sG3Tex);
        sG3TexCount = f.ntextures < 64 ? f.ntextures : 64;
        glGenTextures(sG3TexCount, sG3Tex);
        for (i = 0; i < sG3TexCount; i++) {
            int tw, th, ws, wt;
            const uint8_t* px = gallery3d_texture(&f, i, &tw, &th, &ws, &wt);
            glBindTexture(GL_TEXTURE_2D, sG3Tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(ws));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(wt));
        }
        sG3Gen = f.texGen;
    }
    /* The model. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, sG3Fbo);
    glViewport(0, 0, f.w, f.h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    p_glUseProgram(sG3Prog);
    p_glBindVertexArray(sG3Vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, sG3Vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)f.nverts * G3_FLOATS * 4, f.verts, GL_STREAM_DRAW);
    p_glActiveTexture(GL_TEXTURE0);
    glFrontFace(GL_CW); /* GX: front faces wind clockwise */
    for (i = 0; i < f.ndraws; i++) {
        const G3Draw* d = &f.draws[i];
        int mode[4] = {d->tex >= 0 && d->tex < sG3TexCount, !d->xlu, 0, 0};
        if (d->cull == 0) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(d->cull == 1 ? GL_FRONT : d->cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
        }
        if (d->xlu) {
            glEnable(GL_BLEND);
            p_glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
        if (mode[0]) glBindTexture(GL_TEXTURE_2D, sG3Tex[d->tex]);
        p_glUniform4iv(uG3Mode, 1, mode);
        glDrawArrays(GL_TRIANGLES, d->first, d->count);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glDisable(GL_DEPTH_TEST);
    /* Over the window, at the stage. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
    glViewport(f.x, f.y, f.w, f.h);
    p_glUseProgram(sG3Blit);
    p_glBindVertexArray(sCopyVao);
    glBindTexture(GL_TEXTURE_2D, sG3Color);
    glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glViewport(0, 0, winW, winH);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
    gl_forget();
}

/* Whether the GPU is still working on the last frame shown (gx_frames.c
 * leaves a frame between out rather than queue another behind it: a
 * readback in the game's next frame would wait for all of them, and the
 * game thread would miss its audio DMAs, which popped above 60 fps). */
/* One frame may be in flight (none left out every frame between): busy
 * while the frame before the last isn't finished. */
static GLsync sFrameFence, sOlderFence;
static int gl_gpu_busy(void) {
    GLenum r;
    if (!sOlderFence || !p_glClientWaitSync) return 0;
    r = p_glClientWaitSync(sOlderFence, 0, 0);
    if (r == 0x911B /* GL_TIMEOUT_EXPIRED */) return 1;
    p_glDeleteSync(sOlderFence);
    sOlderFence = NULL;
    return 0;
}

static int finish_present(int w, int h, int may_drop) {
    int dropped;
    draw_fps(w, h);
    {
        /* Seasonal touches (seasons.c): snow, leaves or petals, as small
         * filled rectangles over the picture. */
        extern int seasons_pieces(float* x, float* y, float* pw, float* ph, uint32_t* rgb, int max);
        enum { MAX_FLAKES = 160 };
        static float sx[MAX_FLAKES], sy[MAX_FLAKES], sw[MAX_FLAKES], shh[MAX_FLAKES];
        static uint32_t sc[MAX_FLAKES];
        int n = seasons_pieces(sx, sy, sw, shh, sc, MAX_FLAKES), i;
        if (n > 0) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_SCISSOR_TEST);
            for (i = 0; i < n; i++) {
                int pw = (int)(sw[i] * h) + 1, ph = (int)(shh[i] * h) + 1;
                int px = (int)(sx[i] * w) - pw / 2, py = h - (int)(sy[i] * h) - ph / 2;
                if (px + pw < 0 || py + ph < 0 || px >= w || py >= h) continue;
                glScissor(px, py, pw, ph);
                glClearColor((sc[i] >> 16 & 255) / 255.0f, (sc[i] >> 8 & 255) / 255.0f, (sc[i] & 255) / 255.0f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            gl_forget();
        }
    }
    {
        /* Victory confetti (confetti.c): its pieces as small filled
         * rectangles over the picture, under the overlay. */
        extern int confetti_pieces(float* x, float* y, float* pw, float* ph, uint32_t* rgb, float* alpha, int max);
        enum { MAX_PIECES = 200 };
        static float cx[MAX_PIECES], cy[MAX_PIECES], cw[MAX_PIECES], chh[MAX_PIECES], ca[MAX_PIECES];
        static uint32_t cc[MAX_PIECES];
        int n = confetti_pieces(cx, cy, cw, chh, cc, ca, MAX_PIECES), i;
        if (n > 0) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_SCISSOR_TEST);
            for (i = 0; i < n; i++) {
                int pw = (int)(cw[i] * h) + 1, ph = (int)(chh[i] * h) + 1;
                int px = (int)(cx[i] * w) - pw / 2, py = h - (int)(cy[i] * h) - ph / 2;
                if (px + pw < 0 || py + ph < 0 || px >= w || py >= h) continue;
                glScissor(px, py, pw, ph);
                glClearColor((cc[i] >> 16 & 255) / 255.0f, (cc[i] >> 8 & 255) / 255.0f, (cc[i] & 255) / 255.0f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            gl_forget();
        }
    }
    {
        const uint32_t* menu = menu_draw(w, h);
        if (menu) {
            /* The overlay's image is uploaded only when it changed. */
            static const uint32_t* lastImage;
            static unsigned lastGen = ~0u;
            static int lastW, lastH;
            unsigned gen = menu_generation();
            p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
            p_glUseProgram(sOverlayProgram);
            p_glBindVertexArray(sCopyVao);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sOverlayTex);
            if (menu != lastImage || gen != lastGen || w != lastW || h != lastH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, menu);
                lastImage = menu, lastGen = gen, lastW = w, lastH = h;
            }
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            p_glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
            p_glUseProgram(sProgram);
            p_glBindVertexArray(sVao);
        }
    }
    draw_gallery3d(w, h);
    gl_picture_capture(sWinFbo, w, h); /* (the window's framebuffer, as the look pass draws into) */
    if (sDumpDir && sPresented >= sDumpFrom && sPresented % (unsigned)sDumpEvery == 0 && !sChain) {
        char path[512];
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        snprintf(path, sizeof(path), "%s/frame_%05u.bmp", sDumpDir, sPresented);
        dump_bmp(path, w, h);
    }
    {
        /* A single frame as shown, overlay and all, asked for by the F1 tab's
         * test shots (settings_ui.cpp). */
        extern char g_dump_next[512];
        if (g_dump_next[0] && !sChain) {
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            dump_bmp(g_dump_next, w, h);
            g_dump_next[0] = 0;
        }
    }
    gpu_timer_end();
    dropped = window_present(w, h, may_drop);
    /* A fence after each frame shown: gl_gpu_busy says whether the GPU has
     * finished it yet. */
    if (p_glFenceSync) {
        if (sOlderFence) p_glDeleteSync(sOlderFence);
        sOlderFence = sFrameFence;
        sFrameFence = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    gpu_timer_begin();
    spec_frame();
    {
        extern char g_dump_next[512];
        if (g_dump_next[0] && sChain) {
            dump_shared(g_dump_next);
            g_dump_next[0] = 0;
        }
    }
    if (sDumpDir && sPresented >= sDumpFrom && sPresented % (unsigned)sDumpEvery == 0 && sChain) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05u.bmp", sDumpDir, sPresented);
        dump_shared(path);
    }
    sPresented++;
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    return dropped;
}

const GXDevice g_gl_device = {
    "OpenGL",       gl_init,       gl_texture,     gl_texture_update, gl_texture_delete, gl_draw,
    gl_frame_data,  gl_draw_recorded, gl_clear,    gl_copy_target,    gl_copy_texture,   gl_copy,
    gl_present,     gl_apply_settings, gl_scale,   gl_msaa,          gl_arena_alloc,    gl_arena_write,
    gl_arena_free,  NULL,          gl_copy_read,      gl_present_last,   gl_gpu_busy,
};
