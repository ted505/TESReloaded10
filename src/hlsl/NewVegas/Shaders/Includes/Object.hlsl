#if defined(__INTELLISENSE__)
    #include "Pointlights.hlsl"
    #include "PBR.hlsl"
#else
    #include "includes/Pointlights.hlsl"
    #include "includes/PBR.hlsl"
#endif

#if defined(__INTELLISENSE__)
    #include "SkyAmbient.hlsl"
#else
    #include "includes/SkyAmbient.hlsl"
#endif

float4 TESR_PBRData : register(c32);
float4 TESR_PBRExtraData : register(c33);
// c136, the gap between TESR_DebugVar (c135) and the sky block. SkyAmbient.hlsl claims c137
// upward in both modes -- nine float4s for TESR_SkyIrradiance[9] in mode 0, eight scalars in
// mode 1 -- so anything from c137 to c145 aliases it. Render.cpp writes this register raw,
// through SetPixelShaderConstantF rather than the name-bound RegisterConstant path, so
// MaterialMetallicRegister there has to track this declaration by hand.
float4 TESR_MaterialMetallic : register(c136);

float getRoughness(float gloss) {
    return saturate(max(0.043, 1 - gloss) * TESR_PBRData.y);
}

// Roughness straight from a packed material map, which stores it directly rather than as the
// gloss the line above has to invert. The 0.043 floor and the Roughness scale are kept so the
// two sources stay comparable: a perfectly smooth surface collapses the BRDF's denominator, and
// the global knob should keep working on mapped materials.
float getMappedRoughness(float roughnessChannel) {
    return saturate(max(0.043, roughnessChannel) * TESR_PBRData.y);
}

// Resolves both surface parameters from one fetch of the packed material map.
//
// Channel order is Unreal's ORM: R occlusion, G roughness, B metallic. R is unused here. The
// map is bound with sRGB off, which matters for more than convention now -- G is a curve, and
// decoding it as colour would bend the roughness response.
//
// Where a map is bound it overrides both vanilla sources: G replaces the roughness derived from
// the normal map's alpha gloss mask, and B replaces the flat global Metallicness. Where none is
// bound TESR_MaterialMetallic.x is 0 and both fall back untouched.
//
// Roughness has its own gate in .w, because a file can supply metallic without supplying
// roughness: a plain greyscale metal mask expands to RGB with green equal to red, so reading G
// off one would overwrite the normal map's gloss with the metal mask. The draw hook settles that
// per file when the texture loads, not per frame.
void getMaterialSurface(float4 materialMap, float gloss, out float roughness, out float metallicness) {
    roughness    = lerp(getRoughness(gloss), getMappedRoughness(materialMap.g), TESR_MaterialMetallic.w);
    metallicness = lerp(TESR_PBRData.x, materialMap.b, TESR_MaterialMetallic.x);
}

float getRoughness(float glossmap, float meshgloss){
    // return pow(glossmap, log(meshgloss));    
    // no gloss = 1
    // full gloss = 0

    return saturate(1 - log(meshgloss) / 4 * glossmap);
    // return 1 - saturate(log(meshgloss)/4 + glossmap);
    // return pow(1 - glossmap, meshgloss);
}

// Vanilla
float3 getVanillaLighting(float3 lightDir, float radius, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float gloss, float glossPower) {
    float att = vanillaAtt(lightDir, radius);
    
    lightDir = normalize(lightDir);
    viewDir = normalize(viewDir);
    float3 halfwayDir = normalize(lightDir + viewDir);
    
    float NdotL = shades(normal.xyz, lightDir.xyz);
    
    #if defined(ONLY_SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #elif defined(SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
        lighting += saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #else
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
    #endif
    
    return lighting;
}

float3 getVanillaLightingAtt(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float gloss, float glossPower) {
    lightDir = normalize(lightDir);
    viewDir = normalize(viewDir);
    float3 halfwayDir = normalize(lightDir + viewDir);
    
    float NdotL = shades(normal.xyz, lightDir.xyz);
    
    #if defined(ONLY_SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #elif defined(SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
        lighting += saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #else
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
    #endif
    
    return lighting;
}

// PBR
float3 getPointLightLighting(float3 lightDir, float radius, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness, float metallicness = 0.0f) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    float att = vanillaAtt(lightDir, radius);
    
    #if defined(ONLY_SPECULAR)
        return att * PBRSpecular(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return att * PBR(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
        return att * PBRDiffuse(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}

float3 getPointLightLightingAtt(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness, float metallicness = 0.0f) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    #if defined(ONLY_SPECULAR)
        return att * PBRSpecular(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return att * PBR(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
    return att * PBRDiffuse(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}

float3 getSunLighting(float3 lightDir, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness, float metallicness = 0.0f) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    #if defined(ONLY_SPECULAR)
        return PBRSunSpecular(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return PBRSun(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
        return PBRDiffuse(metallicness, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}



// [_Main.Develop.Main], via Debug.cpp UpdateSettings. c135: c132 is TESR_ShadowBlur.
// Populated even with Shaders.Debug disabled -- Debug has no per-frame UpdateConstants.
float4 TESR_DebugVar : register(c135);

// --- Hemisphere skylight ------------------------------------------------------------------
// Additive upper-sky term on top of the weather ambient, weighted by w = (1 + N.up) / 2.
// w must stay linear in the dot product: that is the exact cosine-weighted form factor.
// [Shaders.PBR.*] SkylightingScale. No separate toggle: 0 disables the term.
#define SKY_AMBIENT_STRENGTH  (TESR_PBRExtraData.y)      // scale on skyUpper at w = 1

float3 getAmbientLighting(float3 ambient, float3 albedo) {
    return ambient * TESR_PBRData.w * albedo;
}

float3 getAmbientLighting(float3 ambient, float3 albedo, float3 worldNormal, float worldNormalValid) {
    float3 flatAmbient = ambient * TESR_PBRData.w;

    // AmbientScale (TESR_PBRData.w) scales the weather ambient above but not this: the sky is a
    // second, independent light source, so SkylightingScale is its only strength knob and it
    // survives AmbientScale = 0.
    float3 skyTerm = SkyAmbientRadiance(worldNormal, TESR_PBRExtraData.z) * SKY_AMBIENT_STRENGTH;

    // worldNormalValid is 0 under a vanilla VS, where the carried world position is undefined.
    return (flatAmbient + skyTerm * worldNormalValid) * albedo;
}

// Both halves take SkylightingScale, deliberately, and there is no second knob.
//
// The BRDF is physically based; the radiance feeding it is not. GetSkyColor interpolates
// weather-authored display colours and scales them by artistic settings, so it carries no
// radiometric unit -- there is one unknown constant between "the colour the sky dome is painted"
// and "the radiance arriving at a surface", and SkylightingScale is it. That constant is a
// property of the light source, so it is the same number for both halves.
//
// Splitting it in two would let the diffuse and specular responses of one light source be dialled
// against each other, which nothing physical can do: how the sky's energy divides between the two
// is the BRDF's answer, not a setting.

// What the surface reflects of the sky.
//
// This must be added where the TEXTURE_Vc multiply cannot reach it. An ONLY_LIGHT pass lights a
// white surface and has its whole output multiplied by the texture afterwards, which is right
// for the diffuse ambient and wrong for a reflection: reflectance already carries the albedo
// through f0, so passing through that multiply applies it a second time. At metallicness 0 that
// is the entire dielectric reflection tinted by albedo in one decomposition and left alone in
// the other, which pops on every surface, not just metal.
//
// So it belongs to the passes nothing multiplies: the self-contained combined ones, and the
// additive ONLY_SPECULAR ones, which carry the real albedo through the s8 alias.
//
// worldView points from the surface toward the camera. The carried shadow world position is
// camera-relative, so normalising its negation gives it with no extra interpolator.
float3 getSkyReflection(float3 albedo, float3 worldNormal, float worldNormalValid,
                        float3 worldView, float roughness, float metallicness) {
    float3 f0 = lerp(float(0.04).rrr, albedo, metallicness);
    return SkyAmbientSpecular(worldNormal, worldView, roughness, f0, TESR_PBRExtraData.z)
         * SKY_AMBIENT_STRENGTH * worldNormalValid;
}

// Ambient for the permutations that carry a view vector.
//
// Metallicness splits the ambient rather than the reflection being added on top: a metal has no
// diffuse response, so its share moves to getSkyReflection instead of being counted twice.
// Turning SkylightingScale down to 0 therefore leaves a fully metallic surface with no sky
// response at all, diffuse or reflected, which is what a metal under no sky should look like.
float3 getAmbientLighting(float3 ambient, float3 albedo, float3 worldNormal, float worldNormalValid,
                          float3 worldView, float roughness, float metallicness) {
    float3 flatAmbient = ambient * TESR_PBRData.w;
    float3 skyTerm = SkyAmbientRadiance(worldNormal, TESR_PBRExtraData.z) * SKY_AMBIENT_STRENGTH;

    float3 diffuse = (flatAmbient + skyTerm * worldNormalValid) * albedo * (1.0f - metallicness);

    // An ONLY_LIGHT pass is multiplied by the texture afterwards, so the reflection cannot ride
    // along here. The split decomposition adds it from its ONLY_SPECULAR pass instead.
    #if defined(ONLY_LIGHT)
        return diffuse;
    #else
        return diffuse + getSkyReflection(albedo, worldNormal, worldNormalValid,
                                          worldView, roughness, metallicness);
    #endif
}
