// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
#pragma once
#include <cstdint>

struct XrEyePose {
    float qx, qy, qz, qw, px, py, pz;
    float tanLeft, tanRight, tanUp, tanDown;
};

// Loads openxr_loader.dll from dir and asks the runtime for the recommended eye size. The session comes later.
bool XrInit(const wchar_t* dir);
void XrRecommended(int* w, int* h);
void XrMaxEyeSize(int* w, int* h);
// How often the XRSTAT line is written: 5 s in the lab, 30 s for players.
void XrSetStatsEvery(unsigned ms);
// Picture setting: 0 Auto, 1 Steady (projection layer), 2 Panel (quad layers). Auto: Steady on Virtual Desktop, Panel elsewhere.
void XrPictureSet(int mode);
int XrPictureGet();
bool XrPictureSteady();
const char* XrRuntimeName();
void XrPictureDump(const wchar_t* base);
// Starts the session on the game's device and queue, with swapchains of this size.
bool XrStartSession(void* device, void* queue, int eyeW, int eyeH);
// Game thread: newest head pose for this frame's eyes. False until the runtime gives a frame.
bool XrLocate(XrEyePose out[2]);
// Game thread: the left eye's world position just computed from that pose, so the pose can be found again later.
void XrTagPose(int eye, const float worldPos[3]);
// Render thread: this world position is what is being drawn now.
void XrMarkRendered(int eye, const float worldPos[3]);
// Alternate eye: only the eye drawn this tick is copied. The other keeps its last image and the pose it was drawn with.
void XrSetAlternate(bool on);
void XrSetOverlap(bool on);
// Lab: GPU timestamps of when the mod's copies began, newest kept.
int XrCopyBegins(unsigned long long* out, int max);
// Depth rebuild of the eye not drawn. Mode 0 off, 1 on. Camera of both eyes for this tick, in the game's world.
void XrRebuildSetMode(int mode);
void XrRebuildCopyDepth(int eye, void* cmd, void* depth);
void XrTagEyeCamera(int eye, const float right[3], const float up[3], const float back[3], const float pos[3], float tanLeft, float tanRight, float tanUp, float tanDown, float nearZ);
void* XrRebuildResult();
// Lab: rebuild the right eye from the left in native stereo, to compare with the real right eye.
void XrRebuildTest(bool on);
void XrRebuildStage(int stage);
// Rebuild source order: 0 the drawn eye first (1.0), 1 the eye's own last image first (default), 2 own image only (lab).
void XrRebuildOrder(int order);
// The eye's motion vectors at its DLSS call (state as given to DLSS), and the game's scale for them (to image units).
void XrRebuildCopyMotion(int eye, void* cmd, void* mv, uint32_t state);
void XrRebuildMvScale(float x, float y);
// Lab: motion on or off, and a multiplier on the vectors (sign checks).
void XrRebuildMotion(int on, float mulX, float mulY);
// Gap filling in the rebuild: 0 nearest neighbour (1.02), 1 from the background side (default); search radius in depth texels.
// moving: movement per tick over distance above which a surface comes from the drawn eye (0 off, negative keeps).
void XrRebuildFill(int mode, int radius, float moving);
// Edges in the rebuild: 1 best of the four nearest depth texels per output pixel (default), 0 nearest (1.04).
void XrRebuildEdge(int on);
// Depth texels around moving things that also come from the drawn eye (default 2), 0 off (1.05).
void XrRebuildBand(int texels);
// Lab: colour each rebuilt pixel by where it came from.
void XrRebuildDebugView(bool on);
void XrRebuildUsePrevHeld(bool on);
// Lab: count and log rebuilt frames whose own image fell back almost everywhere.
void XrRebuildStats(bool on);
void XrRebuildObjFactor(float f);
// Lab, paired tests: a second rebuild of the same frame with other settings (fill mode, radius, moving threshold, edge, band,
// order), only in the native lab test. v[0] < 0 turns it off. XrRebuildAltResult gives its image or null.
void XrRebuildAlt(const float v[6]);
void* XrRebuildAltResult();
// Game thread: wait for the headset's next frame, at most one and a half periods.
void XrPaceWait();
// Present thread: copy both eye images to the swapchains and end the open frame.
struct XrUiImage {
    void* resource;
    uint32_t state;
    int width, height;
    bool keepLast;  // Nothing new to copy: show what the layer already holds.
};
void XrOnPresent(void* const resources[2], const uint32_t states[2], const XrUiImage* ui, int uiCount);
// Present thread: no 3D world now, so show this picture on a flat screen fixed in the room.
void XrOnPresentScreen(void* resource, int width, int height, int dxgiFormat);
void XrRecenterScreen();
bool XrTakeSpaceRecenter();  // true once after the runtime recentered the seated space
void XrSetEndOnWorker(bool on);
// HUD panel in metres.
void XrSetHud(float width, float distance, float drop);
// Any thread: the mod menu's picture, RGBA, top row first, width a multiple of 64. Placed ahead of the head when it opens.
void XrMenuShow(const uint32_t* rgba, int width, int height);
void XrMenuHide();
// Game thread: cutscene now. Each eye's image goes on a screen fixed in the room instead of filling the view.
void XrSetCinema(bool on);
// Lab: GPU ms of the mod's own copies each frame, and between the ends of two frames, since the last call.
void XrCopyTimes(double* copyMs, double* betweenMs);
// Game thread: a small dot along this direction in room space, from this point, while aiming. Null hides it.
void XrSetAimDot(const float* roomDir, const float* roomFrom = nullptr);
// Game thread: cut the middle out of the first HUD image, where the game's reticle sits.
// Blank the game's reticle out of the HUD layer: 0 none, 1 the centre dot only, 2 the aiming reticle.
void XrSetHudHole(int mode);
// Lab only: dump the HUD image as sent, hole included, at the next fresh copy.
void XrDumpHudAfter(const wchar_t* path);
// Present thread, before XrOnPresent: show the left eye in this back buffer, cropped to it. Null leaves the game's picture.
void XrSetMirror(void* backBuffer);
// Lab only: replace what the headset reports, so head movement and lens shape can be tested without a headset.
// Degrees and metres. Positive yaw turns left, positive pitch looks up, as OpenXR rotates.
void XrFakeHead(bool on, float yaw, float pitch, float roll, float x, float y, float z, float ipd);
// Left eye tangents, the right eye gets the mirror image.
void XrFakeLens(bool on, float left, float right, float up, float down);
