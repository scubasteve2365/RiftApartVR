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
// Game thread: wait for the headset's next frame, at most one and a half periods.
void XrPaceWait();
// Present thread: copy both eye images to the swapchains and end the open frame.
void XrOnPresent(void* const resources[2], const uint32_t states[2]);
