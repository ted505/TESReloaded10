// Sky-colour ambient for the lighting shaders (objects, parallax, terrain, skin, hair, grass,
// decals).
//
// Two models, selected at COMPILE time by [Shaders.PBR.Main] SkylightingMode. ps_3_0 flattens
// runtime branches, so a switch here would make every lit pixel pay for both paths; the macro
// comes through ShaderRecord::LoadShader the same way FORWARD_SHADOWS does, and changing the
// setting alters the preprocessed source so the shader cache recompiles.
//
//   0 -- spherical harmonic irradiance (default)
//   1 -- single directional sample, the older model
//
// Both return a GAMMA-ENCODED value. The object and terrain shaders carry no gamma conversion
// anywhere, so their ambient is gamma-encoded, and SKY.pso delinearises GetSkyColor before
// writing it. Keeping both modes in that space means switching between them compares the two
// MODELS rather than confounding the comparison with a colour-space difference.
//
// The encode is sqrt(), i.e. gamma 2.0, not the sRGB curve. One instruction per channel against
// a pow, and the few percent it differs from sRGB is far smaller than the error it replaces:
// integrating encoded values instead of encoding the result cost walls about a third of their
// brightness. Both modes use the same encode so the comparison stays honest.
#ifndef SKYAMBIENT_INCLUDED
#define SKYAMBIENT_INCLUDED

#ifndef SKYLIGHTING_MODE
    #define SKYLIGHTING_MODE 0
#endif

// Both modes need PI, and several shaders include PBRScale -- and so this file -- before their
// own Helpers include. Guarded there, so including it early is free.
#if defined(__INTELLISENSE__)
    #include "Helpers.hlsl"
#else
    #include "includes/Helpers.hlsl"
#endif

#if SKYLIGHTING_MODE == 1

// ---------------------------------------------------------------------------
// Mode 1: one sample of the sky, in a direction leaning from straight up toward the surface
// normal, scaled by a hand-written cosine form factor.
//
// Known shortcomings, kept deliberately so the two can be compared:
//   - lerp(up, N, d) is `up` for ANY d when N is up, so directionality does nothing at all on
//     floors and terrain -- they are pinned to the zenith colour.
//   - a point sample cannot represent a hemisphere: the cosine-weighted density peaks 45
//     degrees off the normal, so the horizon band is under-weighted.
//   - the horizon falloff has to be softened below SKY.pso's own 8, or sunInfluence -- which
//     only reaches the output through terms scaled by athmosphere -- vanishes at the lifted
//     sample direction.
// ---------------------------------------------------------------------------
float4 TESR_SkyColor     : register(c137);
float4 TESR_SkyLowColor  : register(c138);
float4 TESR_HorizonColor : register(c139);
float4 TESR_SunPosition  : register(c140);
float4 TESR_SkyData      : register(c141); // x: atmosphere thickness, y: sun influence, z: sun strength
float4 TESR_SunAmount    : register(c142); // x: dayTime
float4 TESR_SunDiskColor : register(c143);
float4 TESR_SunsetColor  : register(c144);

// Sky.hlsl uses linearize() from Helpers, included above the mode split.
#if defined(__INTELLISENSE__)
    #include "Sky.hlsl"
#else
    #include "includes/Sky.hlsl"
#endif

#ifndef SKY_AMBIENT_ATMOSPHERE_POW
    #define SKY_AMBIENT_ATMOSPHERE_POW 2
#endif

// Mirrors SKY.pso's derivation with the sample direction substituted for its eye ray.
float3 GetSkyRadiance(float3 dir) {
    float3 up = float3(0.0f, 0.0f, 1.0f);

    float verticality = pows(compress(dot(dir, up)), 3);
    float sunHeight = shade(TESR_SunPosition.xyz, up);
    float athmosphere = pows(1 - verticality, SKY_AMBIENT_ATMOSPHERE_POW) * TESR_SkyData.x;
    float sunInfluence = pows(compress(dot(dir, TESR_SunPosition.xyz)), 1 / TESR_SkyData.y);

    float3 sunColor = GetSunColor(sunHeight, TESR_SkyData.x, TESR_SunAmount.x,
                                  TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb);

    return GetSkyColor(verticality, athmosphere, sunHeight, sunInfluence, TESR_SkyData.z,
                       TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, sunColor);
}

// directionality: [Shaders.PBR.*] / [Shaders.Terrain.*] SkylightingDirectionality.
float3 SkyAmbientRadiance(float3 worldNormal, float directionality) {
    float3 up = float3(0.0f, 0.0f, 1.0f);

    // lerp(up, N, d) is the ZERO vector when N points straight down and d is exactly 0.5, and
    // ceilings and undersides have precisely that normal. normalize() of it is NaN.
    float3 v = lerp(up, worldNormal, directionality);
    float len = length(v);
    float3 dir = (len > 1e-4f) ? (v / len) : up;

    // Cosine-weighted form factor for the visible hemisphere: 1 facing up, 0 facing down.
    float wSky = 0.5f * dot(worldNormal, up) + 0.5f;

    // GetSkyColor returns linear; encode to match the space the callers work in.
    return sqrt(max(GetSkyRadiance(dir), 0.0f)) * wSky;
}

// Radiance looking along dir, with no cosine form factor: a specular lobe samples what the sky
// looks like that way, not how much of the hemisphere a surface can see.
float3 SkyRadianceAlong(float3 dir) {
    return sqrt(max(GetSkyRadiance(dir), 0.0f));
}

#else

// ---------------------------------------------------------------------------
// Mode 0: order-2 spherical harmonic irradiance.
//
// SkyShaders::UpdateConstants projects the sky -- the same GetSkyColor that SKY.pso renders the
// dome with -- onto 9 coefficients once per frame and convolves them with the clamped-cosine
// kernel, so this evaluates
//
//     E(N) = INTEGRAL L(w) max(N.w, 0) dw
//
// rather than approximating it by a sample direction. The cosine kernel is a severe low-pass
// filter, so 9 coefficients carry that integral to within about 1% (Ramamoorthi & Hanrahan
// 2001).
//
// The projection runs on LINEAR radiance, which is what the integral is defined on; the
// reconstruction below is encoded.
//
// The cosine form factor, the wall/floor split and the sun-side azimuthal bias are all inherent
// to the convolution. There is no direction to lean, so `directionality` is ignored here.
// ---------------------------------------------------------------------------
float4 TESR_SkyIrradiance[9] : register(c137);

// LINEAR irradiance along n. Callers encode.
float3 SkyIrradianceLinear(float3 n) {
    return TESR_SkyIrradiance[0].rgb
         + TESR_SkyIrradiance[1].rgb * n.y
         + TESR_SkyIrradiance[2].rgb * n.z
         + TESR_SkyIrradiance[3].rgb * n.x
         + TESR_SkyIrradiance[4].rgb * (n.x * n.y)
         + TESR_SkyIrradiance[5].rgb * (n.y * n.z)
         + TESR_SkyIrradiance[6].rgb * (3.0f * n.z * n.z - 1.0f)
         + TESR_SkyIrradiance[7].rgb * (n.x * n.z)
         + TESR_SkyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
}

// worldNormal must be the GEOMETRIC world normal, unit length.
float3 SkyAmbientRadiance(float3 worldNormal, float directionality) {
    // max() before sqrt because an order-2 SH fit can ring slightly negative, and sqrt of a
    // negative is NaN.
    return sqrt(max(SkyIrradianceLinear(worldNormal), 0.0f));
}

// Radiance looking along dir. These coefficients carry IRRADIANCE -- the projection folded in
// the clamped-cosine kernel -- so dividing by PI recovers the average radiance that produced
// it, E = PI * L for a uniform source. That is the right magnitude but not the right sharpness:
// the cosine kernel is a severe low-pass, so this cannot represent a mirror image of the sky,
// only its gross gradient. Adequate here because the sky is smooth and the one high-frequency
// feature in it, the sun disk, already has its own lobe in PBRSunSpecular.
float3 SkyRadianceAlong(float3 dir) {
    return sqrt(max(SkyIrradianceLinear(dir), 0.0f) / PI);
}

#endif

// --- Sky specular ---------------------------------------------------------------------------
// The image-based half of the sky light: what the surface reflects, as opposed to how much of
// the sky it collects. Metals have no diffuse term at all, so without this a metal in shadow is
// lit by nothing and reads as flat paint.
//
// Split-sum, minus the cubemap. The prefiltered-radiance half comes from evaluating the sky
// model along the reflection vector -- the sky is an analytic function here, so a direction can
// be sampled outright and no environment map has to be built, filtered or stored. The BRDF half
// uses Lazarov's analytic fit to the environment BRDF integral, which replaces the usual 2D LUT
// with a handful of ALU.
//
// What this cannot do is reflect anything except the sky: no geometry, no terrain, no horizon
// occlusion. A metal facing a wall still mirrors open sky.

// Karis / Lazarov, "Physically Based Shading on Mobile". Approximates the split-sum
// integral SUM(F * G * vis) over the hemisphere as F0 * A + B.
float3 EnvBRDFApprox(float3 f0, float roughness, float NdotV) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f,  0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.04f,  -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// worldNormal is the GEOMETRIC world normal, so the reflection does not follow normal-mapped
// detail -- there is no world-space tangent frame at this point to perturb it with. worldView
// points from the surface toward the camera.
//
// The reflection vector is pulled toward the normal as roughness rises. A rough lobe averages a
// wide cone rather than the mirror direction, and with only a low-frequency sky to sample,
// leaning toward the normal is what that averaging amounts to.
float3 SkyAmbientSpecular(float3 worldNormal, float3 worldView, float roughness, float3 f0,
                          float directionality) {
    float3 mirror = reflect(-worldView, worldNormal);
    float3 dir = normalize(lerp(mirror, worldNormal, roughness * roughness));

    float NdotV = saturate(dot(worldNormal, worldView));
    return SkyRadianceAlong(dir) * EnvBRDFApprox(f0, roughness, NdotV);
}

#endif
