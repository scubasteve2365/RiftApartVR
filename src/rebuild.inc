// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
// Depth rebuild for alternate eye. Included by xr.cpp. Written for this mod, no outside shader code.
// Source order 1 (the default from 1.02): the eye not drawn this tick is rebuilt first from its own last image and depth, moved
// to its camera for this tick, as AFW-style methods do. So glows and other effects that write no depth stay where that eye saw
// them, off by one tick of movement at most, instead of jumping between the two eyes' positions. Where its own image has
// nothing (uncovered areas, or something now in front), the eye drawn this tick fills in, through its depth. Last resort:
// its own last image turned to the current view. Order 0 is the 1.0 behaviour: the drawn eye first, own image for gaps.
// Order 2 (lab): own image only. Order 3: as 1, and the drawn eye also where it sees something clearly farther.
// Moving things: the own image is a tick old, so with motion on, each of its texels is moved on by the game's own motion
// vectors (DLSS input), with the camera's part taken out, scaled from that eye's last frame interval to one tick.

static const char kRebuildHlsl[] = R"(
cbuffer C : register(b0) {
    float4 dRight, dUp, dBack, dPos;
    float4 sRight, sUp, sBack, sPos;
    float4 hRight, hUp, hBack; // .w of the three: the held camera's position
    float4 dTan, sTan, hTan;   // left, right, up, down
    float4 misc;               // near, map width, map height, source order
    float4 pRight, pUp, pBack; // the held image's previous camera (its motion vectors point there); .w its position
    float4 pTan;
    float4 motion;             // motion vector scale x, y to image units, ticks ahead over the vectors' interval, on
    float4 fill;               // x: gap filling, 0 nearest neighbour (1.02), 1 from the background side; y: search radius;
                               // z: moving things from the drawn eye above this movement over distance per tick, 0 off;
                               // w: edges, 1 picks among the four nearest depth texels per output pixel (1.05), 2 the same for colours only, 0 nearest
    float4 band;               // x: around moving things, this many depth texels also come from the drawn eye (1.06), 0 off;
                               // y: lab source view; z: lab stats slot; w: lab stats on
};
Texture2D<float4> srcColor : register(t0);
Texture2D<float4> histColor : register(t1);
Texture2D<float>  srcDepth : register(t2);
Texture2D<float>  heldDepth : register(t3);
Texture2D<float2> heldMv : register(t4);
RWTexture2D<uint>   map : register(u0);
RWTexture2D<float4> outColor : register(u1);
RWTexture2D<uint>   ownMap : register(u2);
RWTexture2D<float4> ownMove : register(u3);
RWByteAddressBuffer stats : register(u4);  // lab: per frame slot, pixels that fell back to the drawn eye
SamplerState lin : register(s0);

float3 EyeRay(float2 uv, float4 t) { return float3(lerp(t.x, t.y, uv.x), lerp(t.z, t.w, uv.y), -1); }
float3 ToWorld(float3 p, float4 r, float4 u, float4 b, float4 o) { return o.xyz + r.xyz * p.x + u.xyz * p.y + b.xyz * p.z; }
float3 ToEye(float3 w, float4 r, float4 u, float4 b, float4 o) { float3 v = w - o.xyz; return float3(dot(v, r.xyz), dot(v, u.xyz), dot(v, b.xyz)); }
float2 ToUv(float3 e, float4 t) { float2 q = e.xy / -e.z; return float2((q.x - t.x) / (t.y - t.x), (q.y - t.z) / (t.w - t.z)); }

[numthreads(16, 16, 1)] void Clear(uint3 id : SV_DispatchThreadID) {
    map[id.xy] = 0; ownMap[id.xy] = 0; ownMove[id.xy] = 0;
    if (band.w >= 0.5 && id.x == 0 && id.y == 0) stats.Store3((uint)band.z * 12, uint3(0, 0, 0));
}
groupshared uint gsFallback;

// A texel of the missing eye's own last image: where it is in the world now, and how far it moved since that image.
// The motion vector gives where the texel was in the image before (its previous camera). The same depth from that camera
// gives where it was in the world; for a still thing that is the same point, so only things that move get a movement.
float3 OwnMove(uint2 id, float2 uv, float3 w) {
    if (motion.w < 0.5) return 0;
    float2 prevUv = uv + heldMv[id] * motion.xy;
    float4 pPos = float4(pRight.w, pUp.w, pBack.w, 0);
    float3 ep = ToEye(w, pRight, pUp, pBack, pPos);
    if (ep.z >= -0.001) return 0;
    float3 was = ToWorld(EyeRay(prevUv, pTan) * -ep.z, pRight, pUp, pBack, pPos);
    float3 m = (w - was) * motion.z;
    return length(m) < 50 ? m : 0;
}

bool OwnTarget(uint2 id, out uint2 at, out uint key, out float3 move) {
    uint2 size = uint2(misc.y, misc.z);
    at = 0; key = 0; move = 0;
    if (id.x >= size.x || id.y >= size.y) return false;
    float d = max(heldDepth[id.xy], 0.000001);
    float z = misc.x / d;
    float2 uv = (id.xy + 0.5) / size;
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    float3 w = ToWorld(EyeRay(uv, hTan) * z, hRight, hUp, hBack, hPos);
    move = OwnMove(id.xy, uv, w);
    float3 e = ToEye(w + move, dRight, dUp, dBack, dPos);
    if (e.z >= -0.001) return false;
    float2 t = ToUv(e, dTan);
    if (any(t < 0) || any(t >= 1)) return false;
    at = uint2(t * size);
    key = asuint(misc.x / -e.z);
    return true;
}

// The missing eye's own last depth, from the camera it was drawn with (moved on, with motion), into its camera for this tick.
[numthreads(16, 16, 1)] void SplatOwn(uint3 id : SV_DispatchThreadID) {
    if (band.w >= 0.5 && id.x < (uint)misc.y && id.y < (uint)misc.z) {
        // Lab stats: size of the held image's motion vectors in pixels, over texels that move more than 0.5 pixel.
        float l = length(heldMv[id.xy]);
        if (l > 0.5) {
            uint o;
            stats.InterlockedAdd((uint)band.z * 12 + 4, 1, o);
            stats.InterlockedAdd((uint)band.z * 12 + 8, (uint)(min(l, 4000.0) * 16), o);
        }
    }
    uint2 at; uint key; float3 move;
    if (!OwnTarget(id.xy, at, key, move)) return;
    uint old;
    InterlockedMax(ownMap[at], key, old);
}

// Second pass: the texel that won each place leaves its movement there, so the resolve can find where it came from.
[numthreads(16, 16, 1)] void SplatOwnMove(uint3 id : SV_DispatchThreadID) {
    uint2 at; uint key; float3 move;
    if (!OwnTarget(id.xy, at, key, move)) return;
    if (ownMap[at] == key) ownMove[at] = float4(move, 1);
}

[numthreads(16, 16, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
    uint2 size = uint2(misc.y, misc.z);
    if (id.x >= size.x || id.y >= size.y) return;
    float d = max(srcDepth[id.xy], 0.000001);
    float z = misc.x / d;
    float2 uv = (id.xy + 0.5) / size;
    float3 w = ToWorld(EyeRay(uv, sTan) * z, sRight, sUp, sBack, sPos);
    float3 e = ToEye(w, dRight, dUp, dBack, dPos);
    if (e.z >= -0.001) return;
    float2 t = ToUv(e, dTan);
    if (any(t < 0) || any(t >= 1)) return;
    uint2 at = uint2(t * size);
    uint old;
    InterlockedMax(map[at], asuint(misc.x / -e.z), old);
}

)"  // MSVC caps one string literal at 16 KB: the shader continues in the next literal.
R"(
// One depth guess for this output pixel, looked up in the own held image: how well the held depth agrees there (smaller is
// better, 1e9 when it does not), and the colour.
float TryOwn(uint key, int2 q, float3 ray, int2 size, out float4 color) {
    color = 0;
    if (key == 0) return 1e9;
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    float z = misc.x / asfloat(key);
    float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[q].xyz;
    float3 e = ToEye(p, hRight, hUp, hBack, hPos);
    float2 s = ToUv(e, hTan);
    if (!(e.z < 0 && all(s >= 0) && all(s <= 1))) return 1e9;
    int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
    float seen = misc.x / max(heldDepth[ds], 0.000001);
    if (!(seen > -e.z * 0.93)) return 1e9;
    color = histColor.SampleLevel(lin, s, 0);
    return abs(seen + e.z) / -e.z;
}

float TryOther(uint key, float3 ray, int2 size, out float4 color) {
    color = 0;
    if (key == 0) return 1e9;
    float z = misc.x / asfloat(key);
    float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos);
    float3 e = ToEye(p, sRight, sUp, sBack, sPos);
    float2 s = ToUv(e, sTan);
    if (!(e.z < 0 && all(s >= 0) && all(s <= 1))) return 1e9;
    int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
    float seen = misc.x / max(srcDepth[ds], 0.000001);
    if (!(seen > -e.z * 0.93)) return 1e9;
    color = srcColor.SampleLevel(lin, s, 0);
    return abs(seen + e.z) / -e.z;
}

[numthreads(16, 16, 1)] void Resolve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) gsFallback = 0;
    GroupMemoryBarrierWithGroupSync();
    uint w, h;
    outColor.GetDimensions(w, h);
    bool inside = id.x < w && id.y < h;
    uint fellBack = 0;
    if (inside) {
    float2 uv = (id.xy + 0.5) / float2(w, h);
    int2 size = int2(misc.y, misc.z);
    int2 at = clamp(int2(uv * size), int2(0, 0), size - 1);
    uint c = map[at];
    uint co = ownMap[at];
    int2 coAt = at;
    bool back = fill.x >= 0.5;
    if (c == 0 || co == 0) {
        // Cracks between projected texels. Wider gaps stay holes. Keys are near / depth, so a bigger key is nearer. 1.02 took
        // the nearest neighbour, which grows a moving foreground (Ratchet) by a texel at its outline; filling from the farthest
        // neighbour takes the background, which is what a gap at an outline is.
        bool fillOther = c == 0, fillOwn = co == 0;
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(at + int2(x, y), int2(0, 0), size - 1);
            uint m = map[q], o = ownMap[q];
            if (fillOther && m != 0 && (c == 0 || (back ? m < c : m > c))) c = m;
            if (fillOwn && o != 0 && (co == 0 || (back ? o < co : o > co))) {
                co = o;
                coAt = q;
            }
        }
    }
    float3 ray = EyeRay(uv, dTan);
    float3 worldDir = dRight.xyz * ray.x + dUp.xyz * ray.y + dBack.xyz * ray.z;
    // Both candidates, each checked against the depth of the image it comes from.
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    bool ownOk = false, otherOk = false;
    float4 ownColor = 0, otherColor = 0;
    if (misc.w >= 0.5 && co != 0) {
        float z = misc.x / asfloat(co);
        float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[coAt].xyz;
        float3 e = ToEye(p, hRight, hUp, hBack, hPos);
        float2 s = ToUv(e, hTan);
        if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
            int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
            float seen = misc.x / max(heldDepth[ds], 0.000001);
            if (seen > -e.z * 0.93) {
                ownColor = histColor.SampleLevel(lin, s, 0);
                ownOk = true;
            }
        }
    }
    if (c != 0 && (misc.w < 1.5 || misc.w >= 2.5)) {
        float z = misc.x / asfloat(c);
        float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos);
        float3 e = ToEye(p, sRight, sUp, sBack, sPos);
        float2 s = ToUv(e, sTan);
        if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
            // What the drawn eye holds there must be this surface, not something in front of it.
            int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
            float seen = misc.x / max(srcDepth[ds], 0.000001);
            if (seen > -e.z * 0.93) {
                otherColor = srcColor.SampleLevel(lin, s, 0);
                otherOk = true;
            }
        }
    }
    if (fill.w >= 0.5) {
        // Edges: the depth maps are at scene size, the output twice that. Instead of the nearest texel, try the four around
        // this pixel and keep the one whose surface the source image agrees with best, so outlines follow the source image
        // rather than steps of two output pixels.
        float2 f = uv * size - 0.5;
        int2 b = int2(floor(f));
        float bestOwn = 1e9, bestOther = 1e9;
        float4 colOwn = ownColor, colOther = otherColor;
        uint keyOwn = co, keyOther = c;
        int2 atOwn = coAt;
        for (int k = 0; k < 4; ++k) {
            int2 q = clamp(b + int2(k & 1, k >> 1), int2(0, 0), size - 1);
            float4 cc;
            if (misc.w >= 0.5) {
                uint o = ownMap[q];
                float m = TryOwn(o, q, ray, size, cc);
                if (m < bestOwn) { bestOwn = m; colOwn = cc; keyOwn = o; atOwn = q; }
            }
            if (misc.w < 1.5 || misc.w >= 2.5) {
                uint t = map[q];
                float m = TryOther(t, ray, size, cc);
                if (m < bestOther) { bestOther = m; colOther = cc; keyOther = t; }
            }
        }
        // 1: the chosen texels also feed the choice between the two images below. 2: colours only, that choice as before.
        bool keys = fill.w < 1.5;
        if (bestOwn < 1e8) { ownOk = true; ownColor = colOwn; if (keys) { co = keyOwn; coAt = atOwn; } }
        if (bestOther < 1e8) { otherOk = true; otherColor = colOther; if (keys) c = keyOther; }
    }
    // The drawn eye wins where it sees something clearly nearer than the own image (moved in front since), and in order 3
    // also clearly farther (what the own image shows there has moved away). Otherwise the own image, if it has the pixel.
    bool nearer = c != 0 && co != 0 && asfloat(c) > asfloat(co) * 1.1;
    bool farther = c != 0 && co != 0 && asfloat(c) * 1.1 < asfloat(co);
    // 1.03: things that move by themselves (Ratchet, enemies, traffic) come from the eye drawn this tick, which has their
    // pose and animation now, where both images see the same surface. The own image is a tick old for them. Still things,
    // lights included, stay with the own image.
    bool moving = false;
    if (fill.z > 0 && co != 0 && c != 0 && otherOk) {
        float zo = misc.x / asfloat(co);
        bool same = asfloat(c) < asfloat(co) * 1.1 && asfloat(c) * 1.1 > asfloat(co);
        moving = same && length(ownMove[coAt].xyz) > fill.z * zo;
    }
    // 1.06: a band around anything moving also comes from the drawn eye. The own image's pixels just outside a moving
    // outline are a soft blend of it and the background (DLSS smooths edges), so they would keep a faint old outline.
    if (!moving && band.x >= 1 && fill.z > 0 && otherOk && co != 0) {
        int r = (int)band.x;
        for (int y = -r; y <= r && !moving; ++y) for (int x = -r; x <= r && !moving; ++x) {
            int2 q = clamp(coAt + int2(x, y), int2(0, 0), size - 1);
            uint o = ownMap[q];
            if (o != 0 && length(ownMove[q].xyz) > fill.z * (misc.x / asfloat(o))) moving = true;
        }
    }
    bool preferOther = misc.w < 0.5 || (otherOk && (nearer || moving || (misc.w >= 2.5 && farther)));
    bool ok = true;
    float4 color;
    int src = 0;  // lab view: 1 drawn eye, 2 own image, 3 background stretch, 4 last resort
    if (preferOther && otherOk) { color = otherColor; src = 1; }
    else if (ownOk) { color = ownColor; src = 2; }
    else if (otherOk) { color = otherColor; src = 1; }
    else if (back && misc.w >= 0.5) {
        // Uncovered and seen by neither image (behind a moving Ratchet, say): stretch the nearest background of the own image
        // over it. Search rings for the farthest surface the own map has, then look it up as if it were at this pixel.
        uint bk = 0;
        int2 bAt = at;
        int r = (int)fill.y;
        for (int ring = 2; ring <= r && bk == 0; ring += 2) {
            for (int k = 0; k < 8; ++k) {
                int2 d = int2(k == 0 || k == 1 || k == 7 ? ring : k == 3 || k == 4 || k == 5 ? -ring : 0,
                              k == 1 || k == 2 || k == 3 ? ring : k == 5 || k == 6 || k == 7 ? -ring : 0);
                int2 q = clamp(at + d, int2(0, 0), size - 1);
                uint o = ownMap[q];
                if (o != 0 && (bk == 0 || o < bk)) {
                    bk = o;
                    bAt = q;
                }
            }
        }
        bool got = false;
        if (bk != 0) {
            float z = misc.x / asfloat(bk);
            float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[bAt].xyz;
            float3 e = ToEye(p, hRight, hUp, hBack, hPos);
            float2 s = ToUv(e, hTan);
            if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
                color = histColor.SampleLevel(lin, s, 0);
                got = true;
            }
        }
        if (!got) {
            ok = false;
            float3 v = float3(dot(worldDir, hRight.xyz), dot(worldDir, hUp.xyz), dot(worldDir, hBack.xyz));
            float2 s = v.z < 0 ? ToUv(v, hTan) : uv;
            color = histColor.SampleLevel(lin, saturate(s), 0);
        }
    }
    else {
        ok = false;
        float3 v = float3(dot(worldDir, hRight.xyz), dot(worldDir, hUp.xyz), dot(worldDir, hBack.xyz));
        float2 s = v.z < 0 ? ToUv(v, hTan) : uv;
        color = histColor.SampleLevel(lin, saturate(s), 0);
    }
    if (src == 0) src = ok ? 3 : 4;
    if (band.y >= 0.5) {
        // Lab: which source each pixel came from, over a dimmed picture. Red drawn eye, green own image (moving: yellow), blue
        // background stretch, magenta last resort.
        // Red: the own image had nothing valid here. Cyan: it had, but the drawn eye saw something nearer.
        float3 tint = src == 1 ? (moving ? float3(1, 1, 0) : ownOk ? float3(0, 1, 1) : float3(1, 0, 0)) : src == 2 ? float3(0, 1, 0) : src == 3 ? float3(0, 0, 1) : float3(1, 0, 1);
        color.rgb = color.rgb * 0.35 + tint * 0.65;
    }
    outColor[id.xy] = float4(color.rgb, 1);
    fellBack = (src == 1 && !moving && !ownOk) || src == 4 ? 1 : 0;
    }
    if (band.w >= 0.5) {
        if (fellBack) InterlockedAdd(gsFallback, 1);
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0 && gsFallback > 0) stats.InterlockedAdd((uint)band.z * 12, gsFallback);
    }
}
)";

struct RebuildConstants {
    float dRight[4], dUp[4], dBack[4], dPos[4];
    float sRight[4], sUp[4], sBack[4], sPos[4];
    float hRight[4], hUp[4], hBack[4];
    float dTan[4], sTan[4], hTan[4];
    float misc[4];
    float pRight[4], pUp[4], pBack[4], pTan[4];
    float motion[4];
    float fill[4];
    float band[4];
};
static_assert(sizeof(RebuildConstants) == 88 * 4, "constants");
static std::atomic<int> g_rbBand{0};  // texels around moving things that also come from the drawn eye
// Lab, paired tests: a second resolve of the same frame into its own image with other settings, so two choices can be scored
// against the same real eye. Values: fill mode, fill radius, moving threshold, edge, band, source order. Off when alt[0] < 0.
static float g_rbAlt[6] = {-1, 0, 0, 0, 0, 0};
static ID3D12Resource* g_rbOut2;
void XrRebuildAlt(const float v[6]) {
    memcpy(g_rbAlt, v, sizeof(g_rbAlt));  // lab only; a torn read costs one odd frame
    Log("REBUILD second output %s: fill %.0f radius %.0f moving %.4f edge %.0f band %.0f order %.0f", v[0] < 0 ? "off" : "on", v[0], v[1], v[2], v[3], v[4], v[5]);
}
void* XrRebuildAltResult() { return g_rbAlt[0] >= 0 ? g_rbOut2 : nullptr; }
static std::atomic<bool> g_rbDebugView{false};
static std::atomic<float> g_rbObjFactor{1.f};
void XrRebuildObjFactor(float f) { g_rbObjFactor = f; Log("REBUILD object motion factor %.2f", f); }
static std::atomic<bool> g_rbUsePrevHeld{false};  // lab: hold test, the camera before the held one
void XrRebuildUsePrevHeld(bool on) { g_rbUsePrevHeld = on; Log("REBUILD held camera %s", on ? "previous" : "own"); }
void XrRebuildDebugView(bool on) {
    g_rbDebugView = on;
    Log("REBUILD source view %d", on ? 1 : 0);
}
void XrRebuildBand(int texels) {
    g_rbBand = texels;
    Log("REBUILD band %d", texels);
}
static std::atomic<int> g_rbFill{1};   // gap filling: 0 as 1.02, 1 from the background side
static std::atomic<int> g_rbFillRadius{8};
static std::atomic<float> g_rbMovingFrom{0.003f};  // movement per tick over distance above which the drawn eye supplies it
static std::atomic<int> g_rbEdge{1};  // edges: 1 best of the four nearest depth texels (1.05), 0 nearest
void XrRebuildEdge(int on) {
    g_rbEdge = on;
    Log("REBUILD edge %d", on);
}
void XrRebuildFill(int mode, int radius, float moving) {
    g_rbFill = mode;
    g_rbFillRadius = radius;
    if (moving >= 0) g_rbMovingFrom = moving;
    Log("REBUILD fill %d radius %d, moving from the drawn eye above %.4f", mode, radius, g_rbMovingFrom.load());
}
static const UINT kRbCbSlot = 512, kRbCbSlots = 16;  // a ring of constant blocks in an upload buffer
static ID3D12Resource* g_rbCb;
static uint8_t* g_rbCbMapped;
static UINT g_rbCbNext;

static ID3D12RootSignature* g_rbRoot;
static ID3D12PipelineState* g_rbClear, *g_rbSplat, *g_rbSplatOwn, *g_rbSplatOwnMove, *g_rbResolve;
static ID3D12DescriptorHeap* g_rbHeap;
static UINT g_rbInc;
static ID3D12Resource* g_rbHist[2], *g_rbOut, *g_rbMap, *g_rbDepth[2];
static ID3D12Resource* g_rbHeldDepth[2], *g_rbOwnMap;  // each eye's depth as of its image in g_rbHist; the own-image map
static bool g_rbHeldDepthValid[2];
static ID3D12Resource* g_rbMv[2], *g_rbHeldMv[2], *g_rbOwnMove;  // each eye's motion vectors, as last drawn and as held
static bool g_rbMvValid[2], g_rbHeldMvValid[2];
static RebuildEye g_rbPrevHeld[2];         // the camera before the held one: where the held image's motion vectors point
static std::atomic<bool> g_rbMotion{true};
static std::atomic<float> g_rbMvMulX{1.f}, g_rbMvMulY{1.f};  // lab: sign and scale of the vectors on top of the game's own scale
static float g_rbMvScale[2] = {0, 0};      // the game's mvec scale (to image units), from its DLSS constants
static const UINT kRbTable = 10;           // descriptors per table: 5 shader resources, 5 unordered access
// Lab stats: per rebuilt frame, pixels whose own image had nothing valid (they fell back to the drawn eye). A ring of slots
// on the GPU, copied to a readback ring, read a few frames later and logged with what the CPU knew about that frame.
static const UINT kRbStatSlots = 32;
static ID3D12Resource* g_rbStats, *g_rbStatsRead;
static std::atomic<bool> g_rbStatsOn{false};
struct RbStatInfo { uint64_t serial, heldSerial, prevUsed; int dest; uint64_t timeUs; bool pending; };
static RbStatInfo g_rbStatInfo[kRbStatSlots];
static UINT g_rbStatNext;
uint64_t g_rbCurSerial = 0, g_rbPrevUsedSerial = 0;  // set by the caller before each RbRun
static uint64_t g_rbHeldSerial[2];
void XrRebuildStats(bool on) { g_rbStatsOn = on; Log("REBUILD stats %d", on ? 1 : 0); }
void XrRebuildMotion(int on, float mulX, float mulY) {
    g_rbMotion = on != 0;
    g_rbMvMulX = mulX;
    g_rbMvMulY = mulY;
    Log("REBUILD motion %d, vector multiplier %.2f %.2f", on, mulX, mulY);
}
void XrRebuildMvScale(float x, float y) {
    if (g_rbMvScale[0] != x || g_rbMvScale[1] != y) Log("REBUILD game motion vector scale %.6f %.6f", x, y);
    g_rbMvScale[0] = x;
    g_rbMvScale[1] = y;
}
static std::atomic<int> g_rbOrder{1};      // source order, see the top of this file
void XrRebuildOrder(int order) {
    g_rbOrder = order;
    Log("REBUILD source order %d", order);
}
static UINT g_rbDepthW, g_rbDepthH;
static bool g_rbDepthValid[2];
static std::mutex g_rbLock;
static bool g_rbFailed;
static std::atomic<int> g_rebuildMode{0};  // 0 off, 1 on
static RebuildEye g_rbHeld[2];             // the camera of each eye's image in g_rbHist
static std::atomic<uint64_t> g_rbCount{0};
static std::atomic<int> g_rbStage{3};      // lab: 0 nothing, 1 depth copy, 2 and history, 3 everything
void XrRebuildStage(int stage) { g_rbStage = stage; }

static ID3D12Resource* RbTexture(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("REBUILD texture %ux%u format %d failed %08lx", w, h, static_cast<int>(f), static_cast<unsigned long>(hr));
    return r;
}

static bool RbPipelines() {
    if (g_rbRoot) return true;
    if (g_rbFailed) return false;
    g_rbFailed = true;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    const HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) {
        Log("REBUILD no shader compiler");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 5;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr, *err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rbRoot)))) {
        Log("REBUILD root signature failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return false;
    }
    const struct { const char* entry; ID3D12PipelineState** out; } stages[5] = {{"Clear", &g_rbClear}, {"Splat", &g_rbSplat}, {"SplatOwn", &g_rbSplatOwn}, {"SplatOwnMove", &g_rbSplatOwnMove}, {"Resolve", &g_rbResolve}};
    for (const auto& st : stages) {
        ID3DBlob* cs = nullptr, *e = nullptr;
        if (FAILED(compile(kRebuildHlsl, sizeof(kRebuildHlsl) - 1, "rebuild", nullptr, nullptr, st.entry, "cs_5_1", 0, 0, &cs, &e))) {
            Log("REBUILD shader %s: %s", st.entry, e ? static_cast<const char*>(e->GetBufferPointer()) : "");
            g_rbRoot = nullptr;
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rbRoot;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(st.out)))) {
            Log("REBUILD pipeline %s failed", st.entry);
            g_rbRoot = nullptr;
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 * kRbTable;  // one table per drawn eye, and two for the lab's paired output
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rbHeap)))) {
        g_rbRoot = nullptr;
        return false;
    }
    g_rbInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kRbStatSlots * 12;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT}, rh{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC ud2 = bd;
        ud2.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &ud2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_rbStats));
        g_dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_rbStatsRead));
    }
    {
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(kRbCbSlot) * kRbCbSlots;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_rbCb))) ||
            FAILED(g_rbCb->Map(0, nullptr, reinterpret_cast<void**>(&g_rbCbMapped)))) {
            Log("REBUILD constant buffer failed");
            g_rbRoot = nullptr;
            return false;
        }
    }
    g_rbFailed = false;
    Log("REBUILD pipelines ready");
    return true;
}

static void RbBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    list->ResourceBarrier(1, &bar);
}

// table: 0 or 1 for the drawn eye; 2 or 3 the same with the lab's second output (paired tests).
static void RbWriteDescriptors(int source, int table, ID3D12Resource* out) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rbHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(table) * kRbTable * g_rbInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateShaderResourceView(g_rbHist[source], &sd, h);
    h.ptr += g_rbInc;
    g_dev->CreateShaderResourceView(g_rbHist[1 - source], &sd, h);
    h.ptr += g_rbInc;
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    g_dev->CreateShaderResourceView(g_rbDepth[source], &sd, h);
    h.ptr += g_rbInc;
    g_dev->CreateShaderResourceView(g_rbHeldDepth[1 - source], &sd, h);
    h.ptr += g_rbInc;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateShaderResourceView(g_rbHeldMv[1 - source], &sd, h);
    h.ptr += g_rbInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g_rbMap, nullptr, &ud, h);
    h.ptr += g_rbInc;
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(out, nullptr, &ud, h);
    h.ptr += g_rbInc;
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g_rbOwnMap, nullptr, &ud, h);
    h.ptr += g_rbInc;
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_rbOwnMove, nullptr, &ud, h);
    h.ptr += g_rbInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bu{};
    bu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bu.Format = DXGI_FORMAT_R32_TYPELESS;
    bu.Buffer.NumElements = kRbStatSlots * 3;
    bu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(g_rbStats, nullptr, &bu, h);
}

// Called on the game's list right at its DLSS call, while the eye's depth is there to be read. In GENERIC_READ, which allows a copy.
void XrRebuildCopyDepth(int eye, void* cmd, void* depth) {
    if (g_rebuildMode.load() == 0 || !g_dev || !cmd || !depth || eye < 0 || eye > 1 || g_rbStage.load() < 1) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd);
    auto* res = static_cast<ID3D12Resource*>(depth);
    std::lock_guard<std::mutex> l(g_rbLock);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return;
    if (!g_rbDepth[eye] || g_rbDepthW != d.Width || g_rbDepthH != d.Height) {
        // Size change: start over. The old ones may still be in flight, so they are left to the process.
        g_rbDepthW = static_cast<UINT>(d.Width);
        g_rbDepthH = d.Height;
        g_rbDepthValid[0] = g_rbDepthValid[1] = false;
        g_rbDepth[0] = RbTexture(g_rbDepthW, g_rbDepthH, d.Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbDepth[1] = RbTexture(g_rbDepthW, g_rbDepthH, d.Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbMap = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_rbOwnMap = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_rbOwnMove = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (int e = 0; e < 2; ++e) {
            g_rbHeldDepth[e] = RbTexture(g_rbDepthW, g_rbDepthH, d.Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_rbMv[e] = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_rbHeldMv[e] = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_rbHeldDepthValid[e] = g_rbMvValid[e] = g_rbHeldMvValid[e] = false;
            g_rbHeld[e].valid = g_rbPrevHeld[e].valid = false;
        }
        Log("REBUILD depth %ux%u", g_rbDepthW, g_rbDepthH);
    }
    if (!g_rbDepth[eye]) return;
    RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_rbDepth[eye], res);
    RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_rbDepthValid[eye] = true;
}

// Same moment as the depth: the eye's motion vectors (R16G16_FLOAT, scene size). From the state DLSS is given them in.
void XrRebuildCopyMotion(int eye, void* cmd, void* mv, uint32_t state) {
    if (g_rebuildMode.load() == 0 || !g_dev || !cmd || !mv || eye < 0 || eye > 1 || g_rbStage.load() < 1) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd);
    auto* res = static_cast<ID3D12Resource*>(mv);
    std::lock_guard<std::mutex> l(g_rbLock);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (!g_rbMv[eye] || d.Width != g_rbDepthW || d.Height != g_rbDepthH || (d.Format != DXGI_FORMAT_R16G16_FLOAT && d.Format != DXGI_FORMAT_R16G16_TYPELESS)) {
        static int said = 0;
        if (said++ < 3) Log("REBUILD motion vectors not used: %llux%u format %d (depth %ux%u)", d.Width, d.Height, static_cast<int>(d.Format), g_rbDepthW, g_rbDepthH);
        return;
    }
    const auto st = static_cast<D3D12_RESOURCE_STATES>(state);
    const bool move = (st & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    if (move) RbBarrier(list, res, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RbBarrier(list, g_rbMv[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_rbMv[eye], res);
    RbBarrier(list, g_rbMv[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (move) RbBarrier(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    g_rbMvValid[eye] = true;
}

void XrRebuildSetMode(int mode) {
    g_rebuildMode = mode;
    Log("REBUILD mode %d", mode);
}

void* XrRebuildResult() { return g_rbOut; }

static void Rb3(float* out, const float* in) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
    out[3] = 0;
}
static void RbEye(float* right, float* up, float* back, float* pos, float* tan, const RebuildEye& e) {
    Rb3(right, e.right);
    Rb3(up, e.up);
    Rb3(back, e.back);
    if (pos) Rb3(pos, e.pos);
    memcpy(tan, e.tan, 16);
}

// Copies the drawn eye into its history, then rebuilds the other eye into g_rbOut, left in COPY_SOURCE. False: nothing rebuilt.
// Copies an eye's image and its depth into its history, as it is now: the source for rebuilding it on the next tick.
static void RbHold(ID3D12GraphicsCommandList* list, int eye, const RebuildEye& cam, ID3D12Resource* color, D3D12_RESOURCE_STATES state) {
    RbBarrier(list, color, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RbBarrier(list, g_rbHist[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = g_rbHist[eye];
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = color;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_RESOURCE_DESC sd = color->GetDesc();
    D3D12_BOX box{0, 0, 0, static_cast<UINT>(std::min(static_cast<UINT64>(g_chainW), sd.Width)), std::min(static_cast<UINT>(g_chainH), sd.Height), 1};
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    RbBarrier(list, g_rbHist[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RbBarrier(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (g_rbDepthValid[eye] && g_rbHeldDepth[eye]) {
        RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        RbBarrier(list, g_rbHeldDepth[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(g_rbHeldDepth[eye], g_rbDepth[eye]);
        RbBarrier(list, g_rbHeldDepth[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbHeldDepthValid[eye] = true;
    }
    if (g_rbMvValid[eye] && g_rbHeldMv[eye]) {
        RbBarrier(list, g_rbMv[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        RbBarrier(list, g_rbHeldMv[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(g_rbHeldMv[eye], g_rbMv[eye]);
        RbBarrier(list, g_rbHeldMv[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        RbBarrier(list, g_rbMv[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbHeldMvValid[eye] = true;
    }
    g_rbPrevHeld[eye] = g_rbHeld[eye];
    g_rbHeld[eye] = cam;
    g_rbHeldSerial[eye] = g_rbCurSerial;
}

// testColor: lab only (native stereo): the real other eye, held after the rebuild for the next tick, as alternate eye would.
static bool RbRun(ID3D12GraphicsCommandList* list, int source, const RebuildEye eyes[2], ID3D12Resource* srcColor, D3D12_RESOURCE_STATES srcState, float nearZ,
                  ID3D12Resource* testColor = nullptr, D3D12_RESOURCE_STATES testState = D3D12_RESOURCE_STATE_COMMON) {
    if (g_rbStage.load() < 2 || !RbPipelines()) return false;
    std::lock_guard<std::mutex> l(g_rbLock);
    if (!g_rbDepthValid[source] || !g_rbDepth[0] || !g_rbDepth[1] || !g_rbMap) return false;
    static bool made = false;
    if (!made) {
        made = true;
        for (int e = 0; e < 2; ++e) g_rbHist[e] = RbTexture(g_chainW, g_chainH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbOut = RbTexture(g_chainW, g_chainH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (!g_rbHist[0] || !g_rbHist[1] || !g_rbOut) return false;
    // History of the drawn eye: the source of the other eye's rebuild now, and its own rebuild on the next tick.
    RbHold(list, source, eyes[source], srcColor, srcState);
    if (g_rbStage.load() < 3) return false;
    const int dest = 1 - source;
    const int order = g_rbOrder.load();
    RebuildConstants c{};
    RbEye(c.dRight, c.dUp, c.dBack, c.dPos, c.dTan, eyes[dest]);
    RbEye(c.sRight, c.sUp, c.sBack, c.sPos, c.sTan, eyes[source]);
    // Until this eye has an image of its own, fill from the drawn eye's.
    const bool haveHeld = g_rbHeld[dest].valid;
    const RebuildEye& held = haveHeld ? (g_rbUsePrevHeld.load() && g_rbPrevHeld[dest].valid ? g_rbPrevHeld[dest] : g_rbHeld[dest]) : eyes[source];
    RbEye(c.hRight, c.hUp, c.hBack, nullptr, c.hTan, held);
    c.hRight[3] = held.pos[0];
    c.hUp[3] = held.pos[1];
    c.hBack[3] = held.pos[2];
    c.misc[0] = nearZ;
    c.misc[1] = static_cast<float>(g_rbDepthW);
    c.misc[2] = static_cast<float>(g_rbDepthH);
    // Own image only once that eye has an image and depth of its own.
    c.misc[3] = haveHeld && g_rbHeldDepthValid[dest] ? static_cast<float>(order) : 0.f;
    // Motion: the held image's vectors point to its previous camera. In alternate eye that is two ticks before it and the
    // rebuild is one tick after it, so half the movement; in the lab test (native) both are one tick.
    const RebuildEye& prev = g_rbPrevHeld[dest];
    if (g_rbMotion.load() && haveHeld && prev.valid && g_rbHeldMvValid[dest] && g_rbMvScale[0] != 0) {
        RbEye(c.pRight, c.pUp, c.pBack, nullptr, c.pTan, prev);
        c.pRight[3] = prev.pos[0];
        c.pUp[3] = prev.pos[1];
        c.pBack[3] = prev.pos[2];
        c.motion[0] = g_rbMvScale[0] * g_rbMvMulX.load();
        c.motion[1] = g_rbMvScale[1] * g_rbMvMulY.load();
        // Objects' own vectors span one game tick in both modes (RigB, 1.06: the per object previous position is per tick,
        // not per view), and the rebuild is one tick ahead, so 1. Before 1.06 alternate eye used 0.5.
        c.motion[2] = g_rbObjFactor.load();
        c.motion[3] = 1;
    }
    c.fill[0] = static_cast<float>(g_rbFill.load());
    c.fill[1] = static_cast<float>(g_rbFillRadius.load());
    c.fill[2] = g_rbMotion.load() ? g_rbMovingFrom.load() : 0.f;
    c.fill[3] = static_cast<float>(g_rbEdge.load());
    c.band[0] = static_cast<float>(g_rbBand.load());
    c.band[1] = g_rbDebugView.load() ? 1.f : 0.f;
    UINT statSlot = 0;
    if (g_rbStatsOn.load() && g_rbStats && g_rbStatsRead) {
        // Read the slot written kRbStatSlots frames ago first (long done on the GPU), then reuse it.
        statSlot = g_rbStatNext++ % kRbStatSlots;
        RbStatInfo& old = g_rbStatInfo[statSlot];
        if (old.pending) {
            uint32_t* rp = nullptr;
            D3D12_RANGE rr{statSlot * 12, statSlot * 12 + 12};
            if (SUCCEEDED(g_rbStatsRead->Map(0, &rr, reinterpret_cast<void**>(&rp)))) {
                const uint32_t n = rp[statSlot * 3], mvN = rp[statSlot * 3 + 1], mvSum = rp[statSlot * 3 + 2];
                static uint64_t mvTexels = 0, mvTotal = 0, mvFrames = 0;
                mvTexels += mvN;
                mvTotal += mvSum;
                ++mvFrames;
                if (mvFrames % 200 == 0 && mvTexels) {
                    Log("RBSTAT motion vectors of the held image: %.1f texels a frame move over 0.5 px, mean %.2f px", static_cast<double>(mvTexels) / 200, static_cast<double>(mvTotal) / 16.0 / static_cast<double>(mvTexels));
                    mvTexels = mvTotal = 0;
                }
                D3D12_RANGE none{0, 0};
                g_rbStatsRead->Unmap(0, &none);
                static uint64_t frames = 0, bad = 0, gapBad = 0, gapAll = 0;
                const double share = static_cast<double>(n) / (static_cast<double>(g_chainW) * g_chainH);
                ++frames;
                const bool gap = old.serial - old.prevUsed > 1;
                gapAll += gap;
                if (share > 0.2) {
                    ++bad;
                    gapBad += gap;
                    Log("RBSTAT bad frame: %.1f%% fell back, serial %llu, used before %llu, held serial %llu (age %lld), dest %d", share * 100, old.serial, old.prevUsed, old.heldSerial, static_cast<long long>(old.serial - old.heldSerial), old.dest);
                }
                if (frames % 500 == 0) Log("RBSTAT %llu frames, %llu bad; serial gaps in all %llu, in bad %llu", frames, bad, gapAll, gapBad);
            }
        }
        old = {g_rbCurSerial, g_rbHeldSerial[dest], g_rbPrevUsedSerial, dest, 0, true};
    }
    c.band[2] = static_cast<float>(statSlot);
    c.band[3] = g_rbStatsOn.load() && g_rbStats ? 1.f : 0.f;
    RbWriteDescriptors(source, source, g_rbOut);
    const bool alt = g_rbAlt[0] >= 0 && testColor;
    if (alt && !g_rbOut2) g_rbOut2 = RbTexture(g_chainW, g_chainH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (alt && g_rbOut2) RbWriteDescriptors(source, source + 2, g_rbOut2);
    if (!haveHeld) {
        // The other history holds nothing yet: rebuild it with this eye's image in both places.
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rbHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (static_cast<SIZE_T>(source) * kRbTable + 1) * g_rbInc;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dev->CreateShaderResourceView(g_rbHist[source], &v, h);
    }
    ID3D12DescriptorHeap* heaps[1] = {g_rbHeap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rbRoot);
    const UINT slot = g_rbCbNext++ % kRbCbSlots;
    memcpy(g_rbCbMapped + static_cast<size_t>(slot) * kRbCbSlot, &c, sizeof(c));
    list->SetComputeRootConstantBufferView(0, g_rbCb->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kRbCbSlot);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g_rbHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(source) * kRbTable * g_rbInc;
    list->SetComputeRootDescriptorTable(1, table);
    const UINT mx = (g_rbDepthW + 15) / 16, my = (g_rbDepthH + 15) / 16;
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = g_rbMap;
    list->SetPipelineState(g_rbClear);
    list->Dispatch(mx, my, 1);
    list->ResourceBarrier(1, &uav);
    D3D12_RESOURCE_BARRIER uavOwn[2] = {uav, uav};
    uavOwn[0].UAV.pResource = g_rbOwnMap;
    uavOwn[1].UAV.pResource = g_rbOwnMove;
    list->ResourceBarrier(2, uavOwn);
    list->SetPipelineState(g_rbSplat);
    list->Dispatch(mx, my, 1);
    list->ResourceBarrier(1, &uav);
    if (c.misc[3] >= 0.5f) {
        list->SetPipelineState(g_rbSplatOwn);
        list->Dispatch(mx, my, 1);
        list->ResourceBarrier(1, &uavOwn[0]);
        list->SetPipelineState(g_rbSplatOwnMove);
        list->Dispatch(mx, my, 1);
        list->ResourceBarrier(1, &uavOwn[1]);
    }
    if (c.band[3] >= 0.5f) {
        D3D12_RESOURCE_BARRIER su{};
        su.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        su.UAV.pResource = g_rbStats;
        list->ResourceBarrier(1, &su);
    }
    RbBarrier(list, g_rbOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(g_rbResolve);
    list->Dispatch((g_chainW + 15) / 16, (g_chainH + 15) / 16, 1);
    RbBarrier(list, g_rbOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (c.band[3] >= 0.5f) {
        RbBarrier(list, g_rbStats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(g_rbStatsRead, statSlot * 12, g_rbStats, statSlot * 12, 12);
        RbBarrier(list, g_rbStats, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (alt && g_rbOut2) {
        RebuildConstants c2 = c;
        c2.fill[0] = g_rbAlt[0];
        c2.fill[1] = g_rbAlt[1];
        c2.fill[2] = g_rbMotion.load() ? g_rbAlt[2] : 0.f;
        c2.fill[3] = g_rbAlt[3];
        c2.band[0] = g_rbAlt[4];
        c2.band[1] = 0;  // the second output is always a plain picture
        if (c.misc[3] >= 0.5f) c2.misc[3] = g_rbAlt[5];
        const UINT slot2 = g_rbCbNext++ % kRbCbSlots;
        memcpy(g_rbCbMapped + static_cast<size_t>(slot2) * kRbCbSlot, &c2, sizeof(c2));
        list->SetComputeRootConstantBufferView(0, g_rbCb->GetGPUVirtualAddress() + static_cast<UINT64>(slot2) * kRbCbSlot);
        D3D12_GPU_DESCRIPTOR_HANDLE t2 = g_rbHeap->GetGPUDescriptorHandleForHeapStart();
        t2.ptr += static_cast<UINT64>(source + 2) * kRbTable * g_rbInc;
        list->SetComputeRootDescriptorTable(1, t2);
        RbBarrier(list, g_rbOut2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch((g_chainW + 15) / 16, (g_chainH + 15) / 16, 1);
        RbBarrier(list, g_rbOut2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (testColor) RbHold(list, dest, eyes[dest], testColor, testState);
    ++g_rbCount;
    return true;
}
