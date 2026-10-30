// raindesk shaders, built into raindesk.exe. A copy of this file next to the
// exe takes their place and is recompiled whenever it is saved, so the look
// can be tuned while it runs.
//
// Passes, every frame:
//   1. VSSplat + PSMist / PSErase into the persistent mist texture
//   2. VSSplat + PSDrop into the water texture (cleared each frame, additive)
//   3. Onto the screen, each blended over the last:
//      VSFullscreen + PSFlash: lightning lighting up the world outside
//      VSStreak + PSStreak: rain falling outside, behind the glass
//      VSFullscreen + PSComposite: the water on the glass, refracting the desktop
//
// Water textures hold (sum of height * field, sum of field) per pixel, where
// field is each sprite's smooth falloff. The summed field is a metaball: it
// fuses neighbouring drops into one smooth outline, and the drop edge is
// wherever it crosses kEdge. Thickness is rebuilt from the field in the
// composite pass, so overlapping sprites never leave a crease or an inner ring.
//
// Also: VSFullscreen + PSDesktop draws the plain desktop for screenshots.

Texture2D<float4> gDesktop : register(t0);
Texture2D<float2> gWater   : register(t1);
Texture2D<float2> gMist    : register(t2);
SamplerState      gLinear  : register(s0);

cbuffer Frame : register(b0)
{
    float2 gResolution;  // overlay size in pixels
    float  gTime;        // seconds since start
    float  gHasDesktop;  // 1 once the first desktop frame has arrived
    float  gDecay;       // fraction of the mist that dries off this frame
    float  gWiperActive;
    float  gWiperAngle;  // radians from straight up, positive = right
    float  gWiperInner;  // blade span, pixels from the pivot
    float2 gWiperPivot;  // below the bottom edge
    float  gWiperOuter;
    float  gPad;
    float  gFlash;       // lightning brightness now, 0 = none
    float2 gFlashDir;    // direction on the screen toward the flash
    float  gRefraction;  // how strongly drops bend the view, 1 = default
    float4 gSky;         // rgb: the light from the sky outside; a: daylight, 0 = night .. 1 = day
};

struct Splat
{
    float2 pos;     // pixels
    float2 radius;  // sprite half-size, pixels
    float  height;  // peak thickness, pixels
    float3 pad;
};
StructuredBuffer<Splat> gSplats : register(t0);

struct Streak
{
    float2 p0, p1;  // pixels: where the drop was at the start of the exposure, and now
    float  width;   // full width at half maximum, pixels
    float  alpha;   // peak coverage
    float2 pad;
};
StructuredBuffer<Streak> gStreaks : register(t3);

static const float kEdge = 0.12;

// ---------------------------------------------------------------- splats

struct SplatOut
{
    float4 pos    : SV_Position;
    float2 d      : TEXCOORD0;  // -1..1 across the sprite
    float  height : TEXCOORD1;
};

SplatOut VSSplat(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Splat s = gSplats[iid];
    float2 corner = float2(vid & 1, vid >> 1) * 2 - 1;
    float2 px = s.pos + corner * s.radius;
    SplatOut o;
    o.pos = float4(px / gResolution * float2(2, -2) + float2(-1, 1), 0, 1);
    o.d = corner;
    o.height = s.height;
    return o;
}

// Drops on a vertical pane sag: flatter on top, fuller at the bottom.
// Returns (height * field, field).
float2 DropProfile(float2 d, float height)
{
    d.y *= d.y < 0 ? 1.15 : 0.9;
    float q = dot(d, d);
    if (q >= 1) discard;
    float c = 1 - q;
    c *= c;
    return float2(height * c, c);
}

float4 PSDrop(SplatOut i) : SV_Target
{
    return float4(DropProfile(i.d, i.height), 0, 0);
}

float4 PSMist(SplatOut i) : SV_Target
{
    return float4(DropProfile(i.d, i.height), 0, 0);
}

// Blended as dst * (1 - src): wipes mist where running drops pass.
float4 PSErase(SplatOut i) : SV_Target
{
    float r = length(i.d);
    if (r >= 1) discard;
    float e = 1 - smoothstep(0.6, 1.0, r);
    return float4(e, e, 0, 0);
}

// ---------------------------------------------------------------- composite

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

// Fullscreen, blended as dst * (1 - src): mist and streaks slowly dry.
float4 PSDecay(VSOut input) : SV_Target
{
    return float4(gDecay, gDecay, 0, 0);
}

// The wiper: a long rubber blade in a dark metal frame, carried by an arm
// from a pivot below the screen. Drawn over the water.
float4 PSWiper(VSOut input) : SV_Target
{
    if (gWiperActive < 0.5)
        return 0;
    float scale = gResolution.y / 1440.0;
    float2 along = float2(sin(gWiperAngle), -cos(gWiperAngle));
    float2 across = float2(-along.y, along.x);
    float2 d = input.pos.xy - gWiperPivot;
    float t = dot(d, along);
    float u = dot(d, across);

    // Blade: rubber edge in the middle, frame either side, tapered ends.
    float bladeHalf = 9.0 * scale;
    float ends = min(t - gWiperInner, gWiperOuter - t);
    float bladeDist = max(abs(u) - bladeHalf, -ends);
    float blade = saturate(0.5 - bladeDist);
    float rubber = 1 - smoothstep(2.0 * scale, 3.0 * scale, abs(u));
    float3 frame = lerp(0.10, 0.22, saturate(0.5 + u / (bladeHalf * 2)));  // lit along one side
    float3 bladeCol = lerp(frame, 0.02, rubber);

    // Arm: from the pivot to the middle of the blade, a little behind it.
    float armHalf = 5.0 * scale;
    float armLen = lerp(gWiperInner, gWiperOuter, 0.55);
    float armU = u + bladeHalf * 2.2;
    float armDist = max(abs(armU) - armHalf, max(-t, t - armLen));
    float arm = saturate(0.5 - armDist);
    float3 armCol = lerp(0.06, 0.16, saturate(0.5 + armU / (armHalf * 2)));

    float a = max(blade, arm * 0.97);
    float3 col = blade >= arm ? bladeCol : armCol;
    return float4(col * a, a);
}

// ---------------------------------------------------------------- outside

// Lightning lights up everything outside the window, brightest toward the
// flash and the sky above.
float4 PSFlash(VSOut input) : SV_Target
{
    float2 uv = input.uv;
    float2 fromCentre = normalize(uv - 0.5 + float2(0, 0.0001));
    float toward = 0.65 + 0.35 * saturate(dot(fromCentre, gFlashDir));
    float sky = 0.75 + 0.25 * (1 - uv.y);
    float a = saturate(gFlash * 0.22 * toward * sky);
    return float4(float3(0.86, 0.9, 1.0) * a, a);
}

struct StreakOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;  // pixels: along the streak from p0, and across it
    float  len   : TEXCOORD1;
    float  sigma : TEXCOORD2;
    float  alpha : TEXCOORD3;
};

// A quad around the streak, wide enough for its soft edges.
StreakOut VSStreak(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Streak s = gStreaks[iid];
    float2 d = s.p1 - s.p0;
    float len = length(d);
    float2 along = len > 1e-3 ? d / len : float2(0, 1);
    float2 across = float2(-along.y, along.x);
    float sigma = s.width / 2.355;  // Gaussian with that full width at half maximum
    float pad = sigma * 3 + 1;
    float u = (vid & 1) ? len + pad : -pad;
    float v = (vid >> 1) ? pad : -pad;
    float2 px = s.p0 + along * u + across * v;
    StreakOut o;
    o.pos = float4(px / gResolution * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(u, v);
    o.len = len;
    o.sigma = sigma;
    o.alpha = s.alpha;
    return o;
}

// A falling drop smeared over the exposure. Each drop is a tiny wide-angle
// lens, so what it shows is mostly the bright sky (Garg and Nayar: drops are
// nearly always brighter than what is behind them), and in a lightning flash
// every streak lights up.
float4 PSStreak(StreakOut i) : SV_Target
{
    float s2 = 2 * i.sigma * i.sigma;
    float a = i.alpha * exp(-i.uv.y * i.uv.y / s2);
    float beyond = max(-i.uv.x, i.uv.x - i.len);  // round, soft ends
    if (beyond > 0)
        a *= exp(-beyond * beyond / s2);
    if (a < 1.0 / 512)
        discard;
    float3 behind = gDesktop.SampleLevel(gLinear, i.pos.xy / gResolution, 5).rgb;
    // At night, drops near the window catch the light from streetlights and
    // the room, so they still show, warmer and dimmer.
    float3 light = gSky.rgb + (1 - gSky.a) * float3(0.30, 0.24, 0.16);
    float3 col = lerp(behind, light, 0.7) * (1 + 2.5 * gFlash);
    a = saturate(a);
    return float4(saturate(col) * a, a);
}

// ---------------------------------------------------------------- composite helpers

float2 WaterAt(int2 p)
{
    return gWater.Load(int3(p, 0)) + gMist.Load(int3(p, 0));
}

// Height of the water surface in pixels. For a lone drop the field is
// (1 - q)^2, so we can recover q (squared distance from the centre) and build
// a spherical cap that meets the glass at the outline. A sphere maps the view
// behind it smoothly, which avoids rings inside the drop.
float ThicknessAt(int2 p)
{
    float2 w = WaterAt(p);
    float h = w.x / max(w.y, 1e-4);
    // Overlapping sprites (lobes, tails, fused neighbours) push the summed
    // field above 1. Squash it smoothly into 0..1 (unchanged at kEdge) so the
    // top of a merged drop stays rounded instead of clipping into a flat disc.
    float f = 1 - pow(1 - kEdge, w.y / kEdge);
    float qEdge = 1 - sqrt(kEdge);
    float q = (1 - sqrt(f)) / qEdge;
    return h * sqrt(saturate(1 - q));
}

float4 PSDesktop(VSOut input) : SV_Target
{
    return float4(gDesktop.Load(int3(input.pos.xy, 0)).rgb, 1);
}

float4 PSComposite(VSOut input) : SV_Target
{
    if (gHasDesktop < 0.5)
        return 0;

    int2 p = int2(input.pos.xy);
    float2 w = WaterAt(p);
    float field = w.y;
    if (field < kEdge * 0.5)
        return 0;

    float scale = gResolution.y / 1440.0;
    float radius = (w.x / max(field, 1e-4)) * 2.0;  // height is half the radius

    float tL = ThicknessAt(p + int2(-1, 0)), tR = ThicknessAt(p + int2(1, 0));
    float tU = ThicknessAt(p + int2(0, -1)), tD = ThicknessAt(p + int2(0, 1));
    float2 grad = float2(tR - tL, tD - tU) * 0.5;
    float3 n = normalize(float3(-grad * 2.0, 1));
    float steep = 1 - n.z;

    // Each drop is a small lens. Like rain on a window, it shows a shrunken,
    // upside-down and out-of-focus picture of a wide patch of what is behind it.
    float reach = (radius * 2.6 + 2.0 * scale) * gRefraction;
    float2 uv = (input.pos.xy - n.xy * reach) / gResolution;
    float lod = log2(1 + radius * 0.35) + steep * 1.5;
    float3 col = gDesktop.SampleLevel(gLinear, uv, lod).rgb;

    // Light from above gathers in the lower half of the drop...
    // Water too thin to act as a lens gets none of this.
    float lens = smoothstep(0.3, 2.0, radius / scale);
    col *= 1 + lens * (0.15 * saturate(n.y * 1.5) - 0.05);
    // ...and the rim, where light reflects back inside, goes dark, most of all along the top.
    float rim = smoothstep(0.05, 0.7, steep);
    float top = saturate(-n.y / max(length(n.xy), 1e-4) * 0.5 + 0.5);
    // Tiny droplets have a dark band too narrow to resolve; it blends into a
    // light grey speck, not a black ring.
    float sizeK = lerp(0.45, 1.0, smoothstep(1.0, 5.0, radius / scale));
    col *= 1 - rim * (0.1 + 0.7 * top * top) * sizeK;

    // Light from the sky above the window is refracted to the bottom edge
    // of every drop (the lens flips it), giving the bright crescent that
    // makes rain visible even against something dark: a cool grey in the day,
    // a dim orange glow from streetlit clouds at night. It barely shows over
    // bright content, which is already that bright.
    float crescent = saturate(n.y * 1.6) * smoothstep(0.1, 0.8, steep) * lens;
    float3 skyGlow = 0.36 * gSky.rgb + (1 - gSky.a) * float3(0.08, 0.06, 0.04);
    col += skyGlow * crescent * (1 - 0.7 * dot(col, float3(0.3, 0.59, 0.11)));

    // By day, a soft reflection of the bright sky above and to the left.
    // Mist is too small to show a crisp highlight; it would read as noise.
    float3 L = normalize(float3(-0.35, -0.75, 0.55));
    float3 H = normalize(L + float3(0, 0, 1));
    float sheen = smoothstep(1.5, 6.0, radius / scale);
    col += pow(saturate(dot(n, H)), 80.0) * 0.4 * sheen * gSky.a;
    // By night, each drop shows tiny upside-down images of the lights
    // outside: a warm streetlight up to the left appears as a bright point
    // low on the drop's right side, a cooler one up to the right low on its left.
    float night = sheen * (1 - gSky.a);
    float3 lamp = normalize(float3(0.3, 0.55, 0.78));
    float3 lamp2 = normalize(float3(-0.45, 0.35, 0.82));
    col += night * (pow(saturate(dot(n, lamp)), 150.0) * float3(1.0, 0.78, 0.5) +
                    pow(saturate(dot(n, lamp2)), 220.0) * float3(0.4, 0.45, 0.5));

    // Lightning: every drop catches the flash. The lens flips it, so the
    // bright crescent sits on the side away from the flash, with a glint
    // on the side facing it, and the view through the drop brightens too.
    if (gFlash > 0.001)
    {
        float3 F = normalize(float3(gFlashDir * 0.8, 0.6));
        float3 HF = normalize(F + float3(0, 0, 1));
        float away = saturate(dot(n.xy, -gFlashDir) * 1.6) * smoothstep(0.1, 0.8, steep) * lens;
        float glint = pow(saturate(dot(n, HF)), 40.0) * sheen;
        col = col * (1 + 0.6 * gFlash) + gFlash * (0.8 * away + 0.7 * glint + 0.08 * lens);
    }

    // Antialiased edge where the field crosses the threshold.
    float alpha = saturate((field - kEdge) / max(fwidth(field), 1e-4) + 0.5);
    return float4(saturate(col) * alpha, alpha);  // premultiplied
}
