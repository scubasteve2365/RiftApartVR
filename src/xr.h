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
// Starts the session on the game's device and queue, with swapchains of this size.
bool XrStartSession(void* device, void* queue, int eyeW, int eyeH);
// Game thread: newest head pose for this frame's eyes. False until the runtime gives a frame.
bool XrLocate(XrEyePose out[2]);
// Game thread: the left eye's world position just computed from that pose, so the pose can be found again later.
void XrTagPose(const float worldPos[3]);
// Render thread: this world position is what is being drawn now.
void XrMarkRendered(const float worldPos[3]);
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
// HUD panel in metres.
void XrSetHud(float width, float distance, float drop);
// Any thread: the mod menu's picture, RGBA, top row first, width a multiple of 64. Placed ahead of the head when it opens.
void XrMenuShow(const uint32_t* rgba, int width, int height);
void XrMenuHide();
// Game thread: cutscene now. Each eye's image goes on a screen fixed in the room instead of filling the view.
void XrSetCinema(bool on);
// Game thread: a small dot along this direction in room space, from this point, while aiming. Null hides it.
void XrSetAimDot(const float* roomDir, const float* roomFrom = nullptr);
// Game thread: cut the middle out of the first HUD image, where the game's reticle sits.
void XrSetHudHole(bool on);
// Lab only: dump the HUD image as sent, hole included, at the next fresh copy.
void XrDumpHudAfter(const wchar_t* path);
// Present thread, before XrOnPresent: show the left eye in this back buffer, cropped to it. Null leaves the game's picture.
void XrSetMirror(void* backBuffer);
// Lab only: replace what the headset reports, so head movement and lens shape can be tested without a headset.
// Degrees and metres. Positive yaw turns left, positive pitch looks up, as OpenXR rotates.
void XrFakeHead(bool on, float yaw, float pitch, float roll, float x, float y, float z, float ipd);
// Left eye tangents, the right eye gets the mirror image.
void XrFakeLens(bool on, float left, float right, float up, float down);
