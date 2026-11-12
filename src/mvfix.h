// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
// Motion vectors for DLSS in alternate eye: things that move by themselves get their full movement since the eye's history.
#pragma once
#include <cstdint>

// From the game's DLSS constants for an eye viewport (1 or 2): clip to previous clip, and the vectors' scale to image units.
void MvFixSetMatrix(int vp, const float* clipToPrevClip, float scaleX, float scaleY);
// 0 off, 1 statistics only (logs MVFIX lines), 2 fix in alternate eye. factor scales the objects' own movement; convention
// 0 multiplies as a row vector, 1 as a column vector.
void MvFixSetMode(int mode, float factor, int convention);
int MvFixMode();
// At an eye's DLSS call: runs the pass on cmd and returns the texture DLSS should take as motion vectors (in
// NON_PIXEL_SHADER_RESOURCE), or null to keep the game's.
void* MvFixRun(int vp, void* cmd, void* depthRes, void* mvRes, uint32_t mvState, bool alternate);
// How often the MVFIX statistics line is written, in DLSS calls (400 in the lab, 4800 for players).
void MvFixSetLogEvery(unsigned calls);
