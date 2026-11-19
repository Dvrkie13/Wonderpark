// The HD Pack (hdpack.cpp): the game's textures, read from the player's own
// extracted disc, upscaled 4x on this PC and installed as a texture mod
// (mods\hd-pack\, on in mods.ini). Nothing of the game's goes anywhere else.
//
// The upscaler is downloaded on demand, each file checked by its SHA-256:
//   - ONNX Runtime 1.24.4 (Microsoft, MIT): onnxruntime.dll from the
//     Microsoft.ML.OnnxRuntime.DirectML NuGet package. It runs on the
//     graphics card through the DirectML that comes with Windows (Microsoft's
//     separate DirectML package isn't downloaded), else on the processor.
//   - 4xNomosUni_span_multijpg by Philip Hofmann (Phhofm), CC BY 4.0: the
//     fp32 ONNX model from the author's GitHub release.
// Both go in <Wonderpark folder>\hd\upscaler\ with their licenses, and the
// pictures in hd\textures\ (beside the build, not in %APPDATA%: about 2.7
// GB); never in the repo (hd\ is git-ignored). The mod folder in the data
// folder (mods\hd-pack\) points the game there with textures.txt.
#pragma once

#include <mutex>
#include <string>

struct HdBuild {
    std::mutex m;
    bool running = false, finished = false;
    std::wstring stage;    // what it's doing, in plain words
    std::wstring device;   // "your graphics card" or "the processor"
    float progress = 0;    // 0-100
    int done = 0, total = 0, made = 0;
    std::wstring error;    // a plain-language message when it failed or was stopped
};
extern HdBuild g_hd;

// Starts building the pack from root\game\files (root: the Wonderpark
// folder) on its own thread; wake() is called when something changes. limit:
// at most that many textures (0: all; for tests). Textures already made are
// kept, so a stopped build goes on where it stopped.
void hd_start(const std::wstring& root, int limit, void (*wake)());
void hd_cancel();
void hd_join();

// The pack as installed: whether it's there, and how many textures it has.
bool hd_installed(int* textures);
// Removes the pack (its textures, its mod folder and its line in mods.ini).
// The upscaler's download is kept.
bool hd_remove(const std::wstring& root);
