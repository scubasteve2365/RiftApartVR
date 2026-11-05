// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
#pragma once
#include <cstdint>

// Starts a thread that finds the game's queue and Present, starting from any texture the game made.
void CaptureSetup(void* anyGameResource);
// Copies these textures to BMP files after the next Present's queued work. False until the setup has finished.
bool CaptureRequest(void* const* resources, const uint32_t* states, const wchar_t* const* paths, int count);
bool CaptureReady();
void* CaptureDevice();
void* CaptureQueue();
// Called on the Present thread, before the game's Present, with the back buffer: resource, width, height, DXGI format.
void CaptureSetPresentCallback(void (*fn)(void*, unsigned, unsigned, int));
// While VR runs the flat window must not hold the game to the monitor's rate.
void CaptureNoVsync(bool on);
// Lab: busy GPU ms on the game's queue and how long those submissions span, since the last call.
void CaptureGpuBusy(double* busyMs, double* spanMs, uint64_t* submits);
