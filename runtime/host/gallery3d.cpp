// The Gallery's 3D view (F1 notebook > Gallery > a friend): the Pokémon's
// own model and animations, read from the player's extracted disc, drawn
// turning on its pedestal while the game is paused.
//
// The model is a BRRES file: files/Pokemon/<Name>.brres, or <Name>.brres in
// one of files/Pokemon/Res00-03.dan (U8 archives; only the file's own bytes
// are read). Read on a thread of its own: the game and render threads never
// wait for the disc.
//
// What's used of it (NW4R formats, as BrawlBox and the like document them):
//   MDL0 v11: bones (their bind pose, parent and inverse bind matrix),
//     vertex, normal, colour and UV arrays, objects (their GX display lists,
//     indexed), materials (cull mode, translucency, the first texture's name
//     and wrap modes), the draw lists (which material each object uses) and
//     the envelopes (vertices weighted between bones).
//   TEX0: decoded by the runtime's GX decoders (gx_texture.c).
//   CHR0 v5: bone animation (scale, rotation in degrees X-Y-Z, translation;
//     constant, i4/i6/i12 Hermite keys, linear rotations).
// Skinning, lighting and the projection are done here, on the CPU (a few
// thousand vertices), each frame shown; gl_backend.c draws the result
// (gallery3d_frame) into the viewer's stage.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gallery3d.h"

extern "C" {
int gx_texture_size(int fmt, int width, int height);
void gx_decode_texture(uint8_t* out, const uint8_t* src, int fmt, int width, int height, const uint8_t* tlut,
                       int tlut_fmt);
const char* dvd_root(void);
}

namespace {

typedef std::vector<uint8_t> Bytes;

uint32_t u32(const Bytes& d, size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3] : 0; }
int32_t s32(const Bytes& d, size_t o) { return (int32_t)u32(d, o); }
uint16_t u16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? (uint16_t)(d[o] << 8 | d[o + 1]) : 0; }
int16_t s16(const Bytes& d, size_t o) { return (int16_t)u16(d, o); }
float f32(const Bytes& d, size_t o) {
    uint32_t v = u32(d, o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
std::string cstr(const Bytes& d, size_t o) {
    std::string s;
    while (o < d.size() && d[o] && s.size() < 256) s += (char)d[o++];
    return s;
}

struct Bad {};
void need(bool ok) {
    if (!ok) throw Bad{};
}

// A resource group: its entries' names and data offsets (from the group).
std::vector<std::pair<std::string, size_t>> group(const Bytes& d, size_t off) {
    std::vector<std::pair<std::string, size_t>> out;
    need(off + 8 <= d.size());
    uint32_t n = u32(d, off + 4);
    need(n < 4096);
    for (uint32_t i = 1; i <= n; i++) {
        size_t e = off + 8 + 16 * i;
        need(e + 16 <= d.size());
        out.push_back({cstr(d, off + s32(d, e + 8)), off + s32(d, e + 12)});
    }
    return out;
}

// ------------------------------------------------------------------------
// Matrices: 3x4, row-major (the bottom row 0 0 0 1 implied).

typedef std::array<float, 12> M34;
M34 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }
M34 mul(const M34& a, const M34& b) {
    M34 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            float v = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
            if (j == 3) v += a[4 * i + 3];
            r[4 * i + j] = v;
        }
    return r;
}
// Scale, then rotation X, Y, Z (degrees), then translation.
M34 srt(const float* s, const float* r, const float* t) {
    const float k = 3.14159265f / 180;
    float cx = cosf(r[0] * k), sx = sinf(r[0] * k), cy = cosf(r[1] * k), sy = sinf(r[1] * k), cz = cosf(r[2] * k),
          sz = sinf(r[2] * k);
    float m[9] = {cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz, cy * sz, sx * sy * sz + cx * cz,
                  cx * sy * sz - sx * cz, -sy, sx * cy, cx * cy};
    return {m[0] * s[0], m[1] * s[1], m[2] * s[2], t[0], m[3] * s[0], m[4] * s[1], m[5] * s[2], t[1],
            m[6] * s[0], m[7] * s[1], m[8] * s[2], t[2]};
}

// ------------------------------------------------------------------------
// The model

struct Bone {
    std::string name;
    int parent = -1;
    uint32_t node = 0;
    float s[3], r[3], t[3];
    M34 inv;  // inverse bind
};

struct Vertex {      // one corner of a triangle, as stored
    int node;         // its matrix: a bone's node id or an envelope's
    float p[3], n[3], uv[2];
    float c[4];
};

struct Draw {
    int tex;          // index in Model::tex, -1 none
    int first, count; // in Model::verts
    int cull;         // GX: 0 none, 1 front, 2 back, 3 all
    bool xlu;
};

struct Anim {
    int frames = 0;
    bool loop = true;
    // Per bone (by name): 9 tracks (scale xyz, rotation xyz, translation
    // xyz), each frames + 1 values, or empty (the bind pose's).
    std::map<std::string, std::array<std::vector<float>, 9>> bones;
};

struct Model {
    std::vector<Bone> bones;
    std::map<uint32_t, int> nodeBone;                               // node id -> bone
    std::map<uint32_t, std::vector<std::pair<uint32_t, float>>> env;  // envelope node -> (bone node, weight)
    std::vector<Vertex> verts;
    std::vector<Draw> draws;
    std::vector<G3Texture> tex;
    std::vector<std::string> texNames;
    std::map<std::string, Anim> anims;
    float center[3] = {0, 0, 0}, radius = 1;
    float floor = 0, height = 1;  // the idle pose's lowest point, and its height
};

std::vector<float> hermite_track(const std::vector<std::array<float, 3>>& keys, int frames) {
    std::vector<float> out(frames + 1);
    for (int f = 0; f <= frames; f++) {
        float v;
        if (keys.empty()) v = 0;
        else if (f <= keys.front()[0]) v = keys.front()[1];
        else if (f >= keys.back()[0]) v = keys.back()[1];
        else {
            v = keys.back()[1];
            for (size_t k = 0; k + 1 < keys.size(); k++) {
                float f0 = keys[k][0], f1 = keys[k + 1][0];
                if (f < f0 || f > f1) continue;
                float span = f1 - f0;
                if (span <= 0) {
                    v = keys[k + 1][1];
                    break;
                }
                float t = (f - f0) / span, t2 = t * t, t3 = t2 * t;
                v = (2 * t3 - 3 * t2 + 1) * keys[k][1] + (-2 * t3 + 3 * t2) * keys[k + 1][1] +
                    (t3 - 2 * t2 + t) * keys[k][2] * span + (t3 - t2) * keys[k + 1][2] * span;
                break;
            }
        }
        out[f] = v;
    }
    return out;
}

std::vector<float> read_track(const Bytes& d, size_t p, int fmt, bool rot, int frames) {
    need(p + 4 <= d.size());
    if (fmt >= 1 && fmt <= 3) {
        int n = u16(d, p);
        need(n < 4096);
        std::vector<std::array<float, 3>> keys;
        if (fmt == 3) {
            for (int k = 0; k < n; k++) {
                size_t q = p + 8 + 12 * k;
                need(q + 12 <= d.size());
                keys.push_back({f32(d, q), f32(d, q + 4), f32(d, q + 8)});
            }
        } else {
            float step = f32(d, p + 8), base = f32(d, p + 12);
            for (int k = 0; k < n; k++) {
                if (fmt == 1) {
                    size_t q = p + 16 + 4 * k;
                    need(q + 4 <= d.size());
                    uint32_t w = u32(d, q);
                    int tan = (int)(w & 0xFFF);
                    if (tan & 0x800) tan -= 0x1000;
                    keys.push_back({(float)(w >> 24), base + step * ((w >> 12) & 0xFFF), tan / 32.f});
                } else {
                    size_t q = p + 16 + 6 * k;
                    need(q + 6 <= d.size());
                    keys.push_back({u16(d, q) / 32.f, base + step * u16(d, q + 2), s16(d, q + 4) / 256.f});
                }
            }
        }
        return hermite_track(keys, frames);
    }
    std::vector<float> out(frames + 1);
    if (rot && (fmt == 4 || fmt == 5)) {
        float step = f32(d, p), base = f32(d, p + 4);
        for (int f = 0; f <= frames; f++)
            out[f] = base + step * (fmt == 4 ? (p + 8 + f < d.size() ? d[p + 8 + f] : 0) : u16(d, p + 8 + 2 * f));
        return out;
    }
    if (rot && fmt == 6) {
        for (int f = 0; f <= frames; f++) out[f] = f32(d, p + 4 * f);
        return out;
    }
    throw Bad{};
}

Anim read_chr0(const Bytes& d, size_t off) {
    Anim a;
    need(off + 0x2C <= d.size() && !memcmp(&d[off], "CHR0", 4));
    a.frames = u16(d, off + 0x20);
    a.loop = u32(d, off + 0x24) != 0;
    need(a.frames < 10000);
    for (auto& [name, e] : group(d, off + s32(d, off + 0x10))) {
        uint32_t flags = u32(d, e + 4);
        size_t p = e + 8;
        std::array<std::vector<float>, 9> tr;
        struct K {
            int has, iso, fixed0, fmtShift, fmtMask;
            bool rot;
        } kinds[3] = {{22, 4, 13, 25, 3, false}, {23, 5, 16, 27, 7, true}, {24, 6, 19, 30, 3, false}};
        for (int k = 0; k < 3; k++) {
            const K& c = kinds[k];
            if (!(flags >> c.has & 1)) continue;
            int fmt = flags >> c.fmtShift & c.fmtMask;
            int n = flags >> c.iso & 1 ? 1 : 3;
            for (int i = 0; i < n; i++) {
                std::vector<float> v;
                if (flags >> (c.fixed0 + i) & 1) v.assign(a.frames + 1, f32(d, p));
                else v = read_track(d, e + s32(d, p), fmt, c.rot, a.frames);
                p += 4;
                if (n == 1) tr[3 * k] = tr[3 * k + 1] = tr[3 * k + 2] = v;
                else tr[3 * k + i] = v;
            }
        }
        a.bones[name] = std::move(tr);
    }
    return a;
}

void read_array(const Bytes& d, size_t a, int comps, std::vector<std::array<float, 4>>& out, bool color) {
    size_t data = a + u32(d, a + 8);
    uint32_t comp = u32(d, a + 0x14), fmt = u32(d, a + 0x18);
    int div = d.size() > a + 0x1C ? d[a + 0x1C] : 0, stride = d.size() > a + 0x1D ? d[a + 0x1D] : 0;
    int count = u16(d, a + 0x1E);
    out.assign(count, {0, 0, 0, 1});
    for (int i = 0; i < count; i++) {
        size_t p = data + (size_t)i * stride;
        if (color) {
            need(p + 4 <= d.size());
            if (fmt == 5) out[i] = {d[p] / 255.f, d[p + 1] / 255.f, d[p + 2] / 255.f, d[p + 3] / 255.f};
            else if (fmt == 2) out[i] = {d[p] / 255.f, d[p + 1] / 255.f, d[p + 2] / 255.f, 1};
            else out[i] = {1, 1, 1, 1};
            continue;
        }
        int n = comps == 3 ? (comp == 1 || comps == 3 && comp >= 1 ? 3 : 2) : (comp == 1 ? 2 : 1);
        if (comps == 3 && comp == 0 && comps != 2) n = 2;  // XY positions
        if (comps == -3) n = 3;                           // normals
        int size = fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4;
        float sc = fmt == 4 ? 1.f : 1.f / (float)(1 << div);
        need(p + (size_t)n * size <= d.size());
        for (int k = 0; k < n; k++) {
            size_t q = p + (size_t)k * size;
            float v;
            switch (fmt) {
            case 0: v = d[q]; break;
            case 1: v = (int8_t)d[q]; break;
            case 2: v = u16(d, q); break;
            case 3: v = s16(d, q); break;
            default: v = f32(d, q); break;
            }
            out[i][k] = fmt == 4 ? v : v * sc;
        }
    }
}

Model read_model(const Bytes& d) {
    Model M;
    need(d.size() > 0x20 && !memcmp(d.data(), "bres", 4));
    size_t root = u16(d, 0xC);
    std::map<std::string, std::vector<std::pair<std::string, size_t>>> ents;
    for (auto& [name, off] : group(d, root + 8)) ents[name] = group(d, off);
    need(ents.count("3DModels(NW4R)") && !ents["3DModels(NW4R)"].empty());
    size_t m = ents["3DModels(NW4R)"][0].second;
    need(m + 0x80 <= d.size() && !memcmp(&d[m], "MDL0", 4) && u32(d, m + 8) >= 9);
    int32_t secs[14];
    for (int i = 0; i < 14; i++) secs[i] = s32(d, m + 16 + 4 * i);
    auto sec = [&](int i) { return secs[i] ? group(d, m + secs[i]) : std::vector<std::pair<std::string, size_t>>(); };

    // Bones.
    auto bsec = sec(1);
    std::map<size_t, int> byOff;
    for (size_t i = 0; i < bsec.size(); i++) byOff[bsec[i].second] = (int)i;
    for (auto& [name, b] : bsec) {
        Bone B;
        B.name = name;
        B.node = u32(d, b + 0x10);
        for (int k = 0; k < 3; k++) B.s[k] = f32(d, b + 0x20 + 4 * k), B.r[k] = f32(d, b + 0x2C + 4 * k), B.t[k] = f32(d, b + 0x38 + 4 * k);
        for (int k = 0; k < 12; k++) B.inv[k] = f32(d, b + 0xA0 + 4 * k);
        int32_t par = s32(d, b + 0x5C);
        B.parent = par && byOff.count(b + par) ? byOff[b + par] : -1;
        M.nodeBone[B.node] = (int)M.bones.size();
        M.bones.push_back(B);
    }
    // Arrays, by each one's own index.
    std::map<int, std::vector<std::array<float, 4>>> pos, nrm, clr, uv;
    for (auto& [n, a] : sec(2)) read_array(d, a, 3, pos[(int)u32(d, a + 0x10)], false);
    for (auto& [n, a] : sec(3)) read_array(d, a, -3, nrm[(int)u32(d, a + 0x10)], false);
    for (auto& [n, a] : sec(4)) read_array(d, a, 4, clr[(int)u32(d, a + 0x10)], true);
    for (auto& [n, a] : sec(5)) read_array(d, a, 2, uv[(int)u32(d, a + 0x10)], false);
    // Materials.
    struct Mat {
        int cull;
        bool xlu;
        std::string tex;
        int wrapS, wrapT;
    };
    std::vector<Mat> mats;
    for (auto& [name, a] : sec(8)) {
        Mat t{(int)u32(d, a + 0x18), (u32(d, a + 0x10) & 0x80000000u) != 0, "", 1, 1};
        if (u32(d, a + 0x2C)) {
            size_t lay = a + s32(d, a + 0x30);
            t.tex = cstr(d, lay + s32(d, lay));
            t.wrapS = (int)u32(d, lay + 0x18), t.wrapT = (int)u32(d, lay + 0x1C);
        }
        mats.push_back(t);
    }
    // Definitions: draw lists and envelopes.
    std::vector<std::array<int, 3>> draws;  // material, object, (xlu list)
    for (auto& [name, a] : sec(0)) {
        size_t p = a;
        for (int guard = 0; guard < 100000 && p < d.size(); guard++) {
            int op = d[p];
            if (op == 1) break;
            else if (op == 2 || op == 5 || op == 6) p += 5;
            else if (op == 3) {
                uint16_t node = u16(d, p + 1);
                int cnt = d[p + 3];
                auto& w = M.env[node];
                for (int k = 0; k < cnt; k++) w.push_back({u16(d, p + 4 + 6 * k), f32(d, p + 6 + 6 * k)});
                p += 4 + 6 * cnt;
            } else if (op == 4) {
                draws.push_back({u16(d, p + 1), u16(d, p + 3), name == "DrawXlu" ? 1 : 0});
                p += 8;
            } else
                break;
        }
    }
    // Textures (decoded) by name, as the materials use them.
    std::map<std::string, size_t> tex0;
    for (auto& [n, o] : ents["Textures(NW4R)"]) tex0[n] = o;
    std::map<std::string, std::vector<std::pair<std::string, size_t>>::value_type> unused;
    auto texture = [&](const Mat& mt) -> int {
        if (mt.tex.empty() || !tex0.count(mt.tex)) return -1;
        for (size_t i = 0; i < M.texNames.size(); i++)
            if (M.texNames[i] == mt.tex) return (int)i;
        size_t o = tex0[mt.tex];
        int w = u16(d, o + 0x1C), h = u16(d, o + 0x1E), fmt = (int)u32(d, o + 0x20);
        size_t data = o + s32(d, o + 0x10);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || fmt == 8 || fmt == 9 || fmt == 10) return -1;  // palettes: not used by the models
        int size = gx_texture_size(fmt, w, h);
        if (size <= 0 || data + size > d.size()) return -1;
        G3Texture t;
        t.w = w, t.h = h, t.wrapS = mt.wrapS, t.wrapT = mt.wrapT;
        t.rgba.resize((size_t)w * h * 4);
        gx_decode_texture(t.rgba.data(), &d[data], fmt, w, h, nullptr, 0);
        M.tex.push_back(std::move(t));
        M.texNames.push_back(mt.tex);
        return (int)M.tex.size() - 1;
    };
    // Objects, triangulated, in draw-list order.
    auto osec = sec(10);
    for (auto& dr : draws) {
        if (dr[1] >= (int)osec.size() || dr[0] >= (int)mats.size()) continue;
        size_t o = osec[dr[1]].second;
        int single = s32(d, o + 0x08);
        uint32_t lo = u32(d, o + 0x0C), hi = u32(d, o + 0x10);
        size_t primSize = u32(d, o + 0x28), p = (o + 0x24) + s32(d, o + 0x2C), end = p + primSize;
        need(end <= d.size());
        int ids[4] = {s16(d, o + 0x48), s16(d, o + 0x4A), s16(d, o + 0x4C), s16(d, o + 0x50)};
        // The vertex layout: (kind, bytes); kinds 0 pnmtx, 1 texmtx, 2 pos, 3 nrm, 4 clr0, 5 clr1, 6+ uv n.
        std::vector<std::pair<int, int>> attrs;
        if (lo & 1) attrs.push_back({0, 1});
        for (int t = 0; t < 8; t++)
            if (lo >> (1 + t) & 1) attrs.push_back({1, 1});
        static const int kBits[4] = {9, 11, 13, 15};
        for (int k = 0; k < 4; k++) {
            int t = lo >> kBits[k] & 3;
            if (t) attrs.push_back({2 + k, t == 2 ? 1 : t == 3 ? 2 : 0});
        }
        for (int t = 0; t < 8; t++) {
            int tt = hi >> (2 * t) & 3;
            if (tt) attrs.push_back({6 + t, tt == 2 ? 1 : tt == 3 ? 2 : 0});
        }
        uint32_t slots[16] = {};
        Draw out;
        out.tex = texture(mats[dr[0]]);
        out.cull = mats[dr[0]].cull;
        out.xlu = dr[2] || mats[dr[0]].xlu;
        out.first = (int)M.verts.size();
        auto& P = pos[ids[0]];
        auto* N = ids[1] >= 0 && nrm.count(ids[1]) ? &nrm[ids[1]] : nullptr;
        auto* C = ids[2] >= 0 && clr.count(ids[2]) ? &clr[ids[2]] : nullptr;
        auto* U = ids[3] >= 0 && uv.count(ids[3]) ? &uv[ids[3]] : nullptr;
        while (p < end) {
            int op = d[p];
            if (op == 0) {
                p++;
                continue;
            }
            if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
                if (op == 0x20) slots[((u16(d, p + 3) & 0xFFF) / 12) & 15] = u16(d, p + 1);
                p += 5;
                continue;
            }
            int prim = op & 0xF8;
            if (prim != 0x80 && prim != 0x90 && prim != 0x98 && prim != 0xA0) break;  // lines and points: none in the models
            int cnt = u16(d, p + 1);
            p += 3;
            std::vector<Vertex> vs;
            for (int i = 0; i < cnt; i++) {
                int idx[14];
                std::fill(idx, idx + 14, -1);
                for (auto& [k, s] : attrs) {
                    if (!s) continue;  // direct data: not used by the models
                    need(p + s <= end);
                    int v = s == 1 ? d[p] : u16(d, p);
                    p += s;
                    if (k < 14 && idx[k] < 0) idx[k] = v;
                }
                Vertex v{};
                v.node = single >= 0 ? single : (int)slots[((idx[0] < 0 ? 0 : idx[0]) / 3) & 15];
                need(idx[2] >= 0 && idx[2] < (int)P.size());
                for (int k = 0; k < 3; k++) v.p[k] = P[idx[2]][k];
                if (N && idx[3] >= 0 && idx[3] < (int)N->size()) for (int k = 0; k < 3; k++) v.n[k] = (*N)[idx[3]][k];
                else v.n[1] = 1;
                if (C && idx[4] >= 0 && idx[4] < (int)C->size()) for (int k = 0; k < 4; k++) v.c[k] = (*C)[idx[4]][k];
                else v.c[0] = v.c[1] = v.c[2] = v.c[3] = 1;
                if (U && idx[6] >= 0 && idx[6] < (int)U->size()) v.uv[0] = (*U)[idx[6]][0], v.uv[1] = (*U)[idx[6]][1];
                vs.push_back(v);
            }
            auto tri = [&](int a, int b, int c) {
                M.verts.push_back(vs[a]);
                M.verts.push_back(vs[b]);
                M.verts.push_back(vs[c]);
            };
            if (prim == 0x90)
                for (int i = 0; i + 2 < cnt; i += 3) tri(i, i + 1, i + 2);
            else if (prim == 0x98)
                for (int i = 0; i + 2 < cnt; i++) i % 2 == 0 ? tri(i, i + 1, i + 2) : tri(i + 1, i, i + 2);
            else if (prim == 0xA0)
                for (int i = 1; i + 1 < cnt; i++) tri(0, i, i + 1);
            else
                for (int i = 0; i + 3 < cnt; i += 4) tri(i, i + 1, i + 2), tri(i, i + 2, i + 3);
        }
        out.count = (int)M.verts.size() - out.first;
        if (out.count) M.draws.push_back(out);
    }
    // Translucent ones last.
    std::stable_sort(M.draws.begin(), M.draws.end(), [](const Draw& a, const Draw& b) { return !a.xlu && b.xlu; });
    // Animations.
    for (auto& [n, o] : ents["AnmChr(NW4R)"]) {
        try {
            M.anims[n] = read_chr0(d, o);
        } catch (Bad&) {
        }
    }
    return M;
}

// Node matrices for a frame of anim (or the bind pose).
std::map<uint32_t, M34> pose(const Model& M, const Anim* a, float frame) {
    std::vector<M34> world(M.bones.size());
    int f0 = 0, f1 = 0;
    float ft = 0;
    if (a && a->frames > 0) {
        float f = fmodf(frame, (float)a->frames);
        if (f < 0) f += a->frames;
        f0 = (int)f, f1 = std::min(f0 + 1, a->frames), ft = f - f0;
    }
    for (size_t i = 0; i < M.bones.size(); i++) {
        const Bone& b = M.bones[i];
        float v[9];
        for (int k = 0; k < 3; k++) v[k] = b.s[k], v[3 + k] = b.r[k], v[6 + k] = b.t[k];
        if (a) {
            auto it = a->bones.find(b.name);
            // The top two bones carry the Pokémon across the ground (Walk):
            // in the Gallery it walks on the spot, so their animated X and Z
            // translation are left at the model's (Y, the bounce, stays).
            bool root = b.parent < 0 || M.bones[b.parent].parent < 0;
            if (it != a->bones.end())
                for (int k = 0; k < 9; k++) {
                    const auto& tr = it->second[k];
                    if (tr.empty() || (root && (k == 6 || k == 8))) continue;
                    float x0 = tr[std::min<size_t>(f0, tr.size() - 1)], x1 = tr[std::min<size_t>(f1, tr.size() - 1)];
                    float dx = x1 - x0;
                    // Rotations (degrees) the short way round: from 179 to
                    // -179 is 2 degrees, not 358 (a limb spun round for a
                    // moment otherwise).
                    if (k >= 3 && k < 6) dx -= 360.f * roundf(dx / 360.f);
                    v[k] = x0 + dx * ft;
                }
        }
        M34 local = srt(v, v + 3, v + 6);
        world[i] = b.parent >= 0 && b.parent < (int)i ? mul(world[b.parent], local) : local;
    }
    {
        // WONDERPARK_G3_DUMP=<frame>: each bone's world position at that
        // frame of the animation shown, once (checking the decoding).
        static int dumped;
        const char* e = getenv("WONDERPARK_G3_DUMP");
        if (e && !dumped && a && (int)frame >= atoi(e)) {
            dumped = 1;
            for (size_t i = 0; i < M.bones.size(); i++)
                fprintf(stderr, "[g3] %s %.3f %.3f %.3f\n", M.bones[i].name.c_str(), world[i][3], world[i][7], world[i][11]);
        }
    }
    std::map<uint32_t, M34> nodes;
    std::map<uint32_t, M34> skin;
    for (size_t i = 0; i < M.bones.size(); i++) {
        nodes[M.bones[i].node] = world[i];
        skin[M.bones[i].node] = mul(world[i], M.bones[i].inv);
    }
    for (auto& [env, ws] : M.env) {
        if (nodes.count(env)) continue;
        M34 m{};
        for (auto& [nid, w] : ws) {
            auto it = skin.find(nid);
            if (it == skin.end()) continue;
            for (int k = 0; k < 12; k++) m[k] += w * it->second[k];
        }
        nodes[env] = m;
    }
    return nodes;
}

// ------------------------------------------------------------------------
// Reading the file

Bytes read_range(FILE* f, long off, size_t n) {
    Bytes b(n);
    if (fseek(f, off, SEEK_SET) || fread(b.data(), 1, n, f) != n) b.clear();
    return b;
}

// Where each Pokémon model on the disc is: files/Pokemon/<name>.brres, or
// <name>.brres inside Res00-03.dan (only their file tables are read).
struct Source {
    std::string path;     // the file
    long off = 0;         // the model's bytes in it (size 0: the whole file)
    uint32_t size = 0;
};
std::map<std::string, Source> list_models() {
    std::map<std::string, Source> out;
    std::string root = dvd_root() ? dvd_root() : "";
    std::string dir = root + "/files/Pokemon/";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.brres").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = fd.cFileName;
            out[n.substr(0, n.size() - 6)] = {dir + n, 0, 0};
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const char* dan : {"Res00.dan", "Res01.dan", "Res02.dan", "Res03.dan"}) {
        FILE* f = fopen((dir + dan).c_str(), "rb");
        if (!f) continue;
        Bytes hd = read_range(f, 0, 0x20);
        if (hd.size() == 0x20 && u32(hd, 0) == 0x55AA382Du) {
            uint32_t rootOff = u32(hd, 4), fst = u32(hd, 8);
            Bytes t = fst < (8u << 20) ? read_range(f, rootOff, fst) : Bytes();
            uint32_t total = t.empty() ? 0 : u32(t, 8);
            size_t strs = (size_t)total * 12;
            for (uint32_t k = 1; k < total && strs < t.size(); k++) {
                size_t e = (size_t)k * 12;
                if (t[e] != 0) continue;
                std::string n = cstr(t, strs + (u32(t, e) & 0xFFFFFF));
                if (n.size() > 6 && n.compare(n.size() - 6, 6, ".brres") == 0 && !out.count(n.substr(0, n.size() - 6)))
                    out[n.substr(0, n.size() - 6)] = {dir + dan, (long)u32(t, e + 4), u32(t, e + 8)};
            }
        }
        fclose(f);
    }
    return out;
}

// The model for a character: its own name; else one that starts with it
// (BassraoR for Bassrao, PururillF for Pururill; not the FR or WP
// variants if there's another); else the longest shared start, 5 letters
// or more (Dustdus for Dustdas, Onond for Onondo).
Bytes find_brres(const std::string& name) {
    static std::map<std::string, Source> models = list_models();
    const Source* s = nullptr;
    auto it = models.find(name);
    if (it != models.end()) s = &it->second;
    if (!s) {
        std::string best;
        for (auto& [n, src] : models) {
            if (n.compare(0, name.size(), name) != 0) continue;
            bool variant = n.size() >= 2 && (n.compare(n.size() - 2, 2, "FR") == 0 || n.compare(n.size() - 2, 2, "WP") == 0);
            bool bestVariant = best.size() >= 2 && (best.compare(best.size() - 2, 2, "FR") == 0 || best.compare(best.size() - 2, 2, "WP") == 0);
            if (best.empty() || (bestVariant && !variant) || (variant == bestVariant && n.size() < best.size())) best = n;
        }
        if (best.empty()) {
            size_t bestN = 4;
            for (auto& [n, src] : models) {
                size_t k = 0;
                while (k < n.size() && k < name.size() && n[k] == name[k]) k++;
                if (k > bestN || (k == bestN && !best.empty() && n.size() < best.size())) bestN = k, best = n;
            }
        }
        if (!best.empty()) s = &models[best];
    }
    if (!s) return {};
    FILE* f = fopen(s->path.c_str(), "rb");
    if (!f) return {};
    Bytes b;
    if (s->size) b = read_range(f, s->off, s->size);
    else {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        if (n > 0 && n < (64 << 20)) b = read_range(f, 0, (size_t)n);
    }
    fclose(f);
    return b;
}

// ------------------------------------------------------------------------
// The state the F1 notebook and the renderer share

std::mutex g_m;
std::string g_want;                 // the character asked for
std::string g_loaded;               // ... and the one loaded (or failed)
std::shared_ptr<Model> g_model;     // null while loading or when it failed
std::atomic<int> g_state{0};        // 0 nothing, 1 loading, 2 ready, 3 failed
std::atomic<bool> g_visible{false};
int g_anim = 0;
float g_rect[4];                    // window pixels: x, y (from the top), w, h
double g_t0;
float g_yaw = 0, g_dragYaw = 0;
float g_zoom = 1;  // scroll to zoom (gallery3d_zoom)
unsigned g_texGen = 0;

const char* const kAnims[G3_ANIMS] = {"NorIdle0", "Joy0", "Walk0", "Random00"};

double now() {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

void start_load(const std::string& name) {
    g_state = 1;
    std::thread([name] {
        std::shared_ptr<Model> m;
        try {
            Bytes b = find_brres(name);
            if (!b.empty()) m = std::make_shared<Model>(read_model(b));
        } catch (Bad&) {
            m.reset();
        } catch (std::exception&) {
            m.reset();
        }
        if (m && m->verts.empty()) m.reset();
        if (m) {
            // Its size, from the idle pose (the bind pose is a T).
            const Anim* a = m->anims.count("NorIdle0") ? &m->anims["NorIdle0"] : nullptr;
            auto nodes = pose(*m, a, 0);
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (auto& v : m->verts) {
                auto it = nodes.find((uint32_t)v.node);
                float p[3] = {v.p[0], v.p[1], v.p[2]};
                if (it != nodes.end()) {
                    const M34& x = it->second;
                    for (int k = 0; k < 3; k++) p[k] = x[4 * k] * v.p[0] + x[4 * k + 1] * v.p[1] + x[4 * k + 2] * v.p[2] + x[4 * k + 3];
                }
                for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
            }
            for (int k = 0; k < 3; k++) m->center[k] = (lo[k] + hi[k]) / 2;
            m->radius = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) / 2;
            if (!(m->radius > 0)) m->radius = 1;
            m->floor = lo[1];
            m->height = std::max(hi[1] - lo[1], 1e-3f);
        }
        std::lock_guard<std::mutex> lk(g_m);
        if (g_want != name) return;  // another was asked for meanwhile
        g_model = m;
        g_loaded = name;
        g_texGen++;
        g_state = m ? 2 : 3;
        if (!m) fprintf(stderr, "gallery3d: no model for %s\n", name.c_str());
    }).detach();
}

}  // namespace

extern "C" void gallery3d_show(const char* character, int anim, float x, float y, float w, float h) {
    std::lock_guard<std::mutex> lk(g_m);
    g_rect[0] = x, g_rect[1] = y, g_rect[2] = w, g_rect[3] = h;
    if (anim != g_anim) g_anim = anim, g_t0 = now();
    if (!g_visible) g_t0 = now(), g_dragYaw = 0, g_zoom = 1;
    g_visible = true;
    std::string c = character ? character : "";
    {
        // WONDERPARK_G3_CHARACTER=<name>,<name>,... (tests): these models
        // instead of the friend's, a new one every 1.5 s.
        static double first;
        const char* e = getenv("WONDERPARK_G3_CHARACTER");
        if (e && *e) {
            std::vector<std::string> list;
            std::string s = e;
            for (size_t p = 0, q; p <= s.size(); p = q + 1) {
                q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                if (q > p) list.push_back(s.substr(p, q - p));
            }
            if (!first) first = now();
            if (!list.empty()) c = list[(size_t)((now() - first) / 1.5) % list.size()];
        }
    }
    if (c != g_want) {
        g_want = c;
        g_model.reset();
        g_dragYaw = 0;
        g_zoom = 1;
        if (c.empty()) g_state = 3;
        else start_load(c);
    }
}

extern "C" void gallery3d_hide(void) { g_visible = false; }

extern "C" int gallery3d_state(void) { return g_state; }

extern "C" int gallery3d_has_anim(int anim) {
    std::lock_guard<std::mutex> lk(g_m);
    return g_model && anim >= 0 && anim < G3_ANIMS && g_model->anims.count(kAnims[anim]);
}

extern "C" void gallery3d_drag(float dx) { g_dragYaw += dx * 0.6f; }

// Scroll to zoom: closer (up) or further (down), 0.6x to 1.8x the fitted
// size; back to 1 when another Pokémon is shown.
extern "C" void gallery3d_zoom(float steps) {
    g_zoom *= powf(1.15f, steps);
    g_zoom = std::min(1.8f, std::max(0.6f, g_zoom));
}

extern "C" const uint8_t* gallery3d_texture(const G3Frame* f, int i, int* w, int* h, int* wrapS, int* wrapT) {
    if (!f || i < 0 || i >= f->ntextures) return nullptr;
    const G3Texture& t = f->textures[i];
    *w = t.w, *h = t.h, *wrapS = t.wrapS, *wrapT = t.wrapT;
    return t.rgba.data();
}

extern "C" int gallery3d_frame(G3Frame* out, int winW, int winH) {
    if (!g_visible || g_state != 2) return 0;
    std::shared_ptr<Model> m;
    float rect[4];
    int anim;
    double t0;
    {
        std::lock_guard<std::mutex> lk(g_m);
        m = g_model;
        memcpy(rect, g_rect, sizeof(rect));
        anim = g_anim;
        t0 = g_t0;
    }
    if (!m || rect[2] < 8 || rect[3] < 8) return 0;
    double t = now() - t0;
    const Anim* a = nullptr;
    auto it = m->anims.find(kAnims[anim]);
    if (it == m->anims.end()) it = m->anims.find("NorIdle0");
    if (it != m->anims.end()) a = &it->second;
    auto nodes = pose(*m, a, (float)(t * 60.0));  // 60 frames a second, as the game
    // The camera: turning slowly (and as dragged), the model fitted to the
    // stage, slightly from above; perspective.
    float yaw = (float)(t * 0.35) + g_dragYaw * 3.14159265f / 180;
    float cy = cosf(yaw), sy = sinf(yaw);
    float aspect = rect[2] / rect[3];
    float r = m->radius, fov = 0.62f;  // vertical, radians
    float f = 1 / tanf(fov / 2);
    // Far enough back that the whole Pokémon fits (its height at most 76%
    // of the stage, its width 70%), standing on the pedestal: its feet at
    // the ring, 82% of the way down the stage (settings_ui.cpp draw_viewer).
    float dist = (std::max({m->height * f / 1.52f, r * 2 * f / (1.4f * aspect), r * 2.2f}) + r) / g_zoom;
    float feet = (m->floor - m->center[1]) * f / dist;  // where the feet would be (clip y), centred
    float lift = -0.64f - feet;
    float pitch = 0.18f, cp = cosf(pitch), sp = sinf(pitch);
    float zn = dist - r * 2, zf = dist + r * 2;
    if (zn < r * 0.05f) zn = r * 0.05f;
    static std::vector<float> buf;
    buf.resize(m->verts.size() * G3_FLOATS);
    float L[3] = {0.35f, 0.55f, 0.76f};  // light, in view space (from the viewer, above, a little left)
    for (size_t i = 0; i < m->verts.size(); i++) {
        const Vertex& v = m->verts[i];
        float p[3] = {v.p[0], v.p[1], v.p[2]}, n[3] = {v.n[0], v.n[1], v.n[2]};
        auto nit = nodes.find((uint32_t)v.node);
        if (nit != nodes.end()) {
            const M34& x = nit->second;
            float q[3], nn[3];
            for (int k = 0; k < 3; k++) {
                q[k] = x[4 * k] * p[0] + x[4 * k + 1] * p[1] + x[4 * k + 2] * p[2] + x[4 * k + 3];
                nn[k] = x[4 * k] * n[0] + x[4 * k + 1] * n[1] + x[4 * k + 2] * n[2];
            }
            memcpy(p, q, sizeof(p)), memcpy(n, nn, sizeof(n));
        }
        // Model -> view: centred, turned by yaw, tilted by pitch.
        float x0 = p[0] - m->center[0], y0 = p[1] - m->center[1], z0 = p[2] - m->center[2];
        float x1 = cy * x0 + sy * z0, z1 = -sy * x0 + cy * z0;
        float y2 = cp * y0 - sp * z1, z2 = sp * y0 + cp * z1;
        float nx = cy * n[0] + sy * n[2], nz = -sy * n[0] + cy * n[2];
        float ny2 = cp * n[1] - sp * nz, nz2 = sp * n[1] + cp * nz;
        float ln = sqrtf(nx * nx + ny2 * ny2 + nz2 * nz2);
        float lit = 1;
        if (ln > 0) {
            float dl = (nx * L[0] + ny2 * L[1] + nz2 * L[2]) / ln;
            lit = 0.62f + 0.48f * std::max(0.f, dl);
        }
        float ze = z2 - dist;  // the eye at +dist, looking down -z
        float* o = &buf[i * G3_FLOATS];
        o[0] = x1 * f / aspect;
        o[1] = y2 * f + lift * -ze;  // shifted on screen (clip y is divided by w = -ze)
        o[2] = ((zf + zn) / (zn - zf)) * ze + (2 * zf * zn) / (zn - zf);
        o[3] = -ze;
        o[4] = v.uv[0];
        o[5] = v.uv[1];
        o[6] = v.c[0] * lit;
        o[7] = v.c[1] * lit;
        o[8] = v.c[2] * lit;
        o[9] = v.c[3];
    }
    static std::vector<G3Draw> draws;
    draws.clear();
    for (auto& d : m->draws) draws.push_back({d.tex, d.first, d.count, d.cull, d.xlu ? 1 : 0});
    out->verts = buf.data();
    out->nverts = (int)m->verts.size();
    out->draws = draws.data();
    out->ndraws = (int)draws.size();
    out->textures = m->tex.data();
    out->ntextures = (int)m->tex.size();
    out->texGen = g_texGen;
    out->x = (int)rect[0];
    out->y = winH - (int)(rect[1] + rect[3]);  // GL: from the bottom
    out->w = (int)rect[2];
    out->h = (int)rect[3];
    (void)winW;
    return 1;
}
