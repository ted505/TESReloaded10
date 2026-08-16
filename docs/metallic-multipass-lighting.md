# Metallic Materials and the Engine's Multi-Pass Lighting

## Background

The engine does not render an object's lighting in one draw. It decomposes it across
several passes and re-picks the decomposition **per draw**, from the active light set and
the material's flags. The same geometry can be a single combined pass one frame and five
additive passes the next, with nothing but a small camera move between them.

Two decompositions observed on the same mesh (`Line01:0`, a corrugated metal roof):

| | combined | split |
|---|---|---|
| base | `208 BSSM_ADTS10` → `SLS2034.pso` `{LIGHTS=4, SPECULAR}`, opaque | `233 BSSM_AD3` → `SLS2041.pso` `{ONLY_LIGHT, OPT, LIGHTS=3}`, opaque |
| then | *(nothing — the pass does everything)* | `306 DIFFUSEPT2` additive → `334 TEXTURE_Vc` multiply → `346 SPECULARDIR` + `358` + `370` additive |

Note `LIGHTS=4` versus `LIGHTS=3`: those select **two separate pixel-shader bodies** in
`ObjectTemplate.hlsl`, not two configurations of one. Terms must be kept in step across
both by hand.

The split form is the classic Gamebryo structure: light a white surface, multiply the
texture in, then **add** specular on top. The multiply happens *between* the lighting and
the specular, which is the source of most of what follows.

## Why metal is uniquely exposed

`PBR.hlsl:92` and `:136` — reflectance is

```
reflectance = lerp(0.04, albedo, metallicness)
```

At `metallicness = 0` reflectance is a constant `0.04`, independent of albedo. Vanilla's
specular is `gloss * pow(NdotH, p)` — colourless, and a minor highlight over a dominant
diffuse term.

At `metallicness = 1` two things change at once:

- **Reflectance becomes the albedo.** Anything that perturbs albedo now perturbs the
  specular colour directly.
- **Diffuse goes to zero** (`(1 - metallicness) * albedo`). The specular lobe is no longer
  a highlight on the surface, it *is* the surface.

So any inconsistency between decompositions that touches albedo or the specular term is
invisible on every dielectric in the game and total on metal. Every defect below had been
latent for as long as the split-specular path has existed.

**Corollary for diagnosis:** a binary metallic map (0 or 1, no intermediates) means the
diffuse term is zero on the metal in *both* decompositions. Anything reached through
`PBRDiffuse` cannot explain a difference there. That eliminates a large part of the
lighting path immediately.

## Defects found

### 1. Split specular passes have no albedo

`ObjectTemplate.hlsl:569` — an `ONLY_SPECULAR` pass sets `baseColor = 1`. The engine binds
`NormalMap` to `s0` and no diffuse at all, because vanilla specular never reads albedo.
With PBR, reflectance falls back to white and the metal renders as a white mirror.

Fixed by aliasing the already-loaded diffuse to `s8` from the draw hook
(`Render.cpp:108`), declared at `ObjectTemplate.hlsl:523` and `ParallaxTemplate.hlsl:321`.
`s8` is free in both: object permutations top out at `s7` and the shadow atlas is `s9`.

`ParallaxTemplate` had no split-albedo path at all, and samples at `offsetUV` so the albedo
lands on the same parallax-shifted texel as the normal.

### 2. The alias sampled differently from the engine

The alias was bound with `D3DSAMP_SRGBTEXTURE = 1`; the engine samples the same texture
raw. Measured directly rather than assumed:

```
SPLITALBEDO engine s0 srgb=0 mag=3 addrU=1 | alias s8 srgb=1 mag=3 addrU=1
```

Filtering and addressing already agreed. sRGB did not, so the split pass received
`albedo^2.2` — a mid-tone 0.5 reads as 0.21. `Render.cpp:271` now copies all six sampler
states from `s0` instead of restating them; hardcoding is what got it wrong.

### 3. Pass classification was a guessed enum window

`IsSplitSpecularPass` (`Render.cpp:120`) tests a contiguous range, 346–375. A split pass
outside it still samples `s8`, just unbound. `Render.cpp:172` now binds for **every** draw
of a material carrying a metallic map, so classification is no longer load-bearing; the
enum test survives only as a union term for unmapped geometry taking metallicness from the
global setting.

### 4. Split specular passes never applied vertex colour

The root cause of the reported pop-in. `ObjectTemplate.hlsl:142` — `ONLY_SPECULAR` implies
`ONLY_LIGHT`, which at `:155` implied `NO_VERTEX_COLOR`.

That rule is correct for the diffuse chain: an `ONLY_LIGHT` pass lights a white surface and
the `TEXTURE_Vc` pass that follows multiplies texture **and** vertex colour back in. But a
split specular pass is `ONLY_LIGHT` by the same rule while being *added after* that
multiply, so nothing ever applied vertex colour to it.

Result: reflectance was `tex2D(BaseMap) × vertexColor` in the combined permutation and
`tex2D(s8)` alone in the split one. On the test roof the vertex colour carried the rust, so
the metal alternated between rust-tinted and untinted as the engine re-decomposed.

`NO_VERTEX_COLOR` is now suppressed for `ONLY_SPECULAR` in both templates. The hair branch
of that block (`ObjectTemplate.hlsl:612`) is excluded for split specular: it reads
`GlowMap`, which is deliberately not declared there, and an additive pass must not re-add
emittance.

## Invariants worth preserving

- **Anything added after `TEXTURE_Vc` must carry its own albedo and vertex colour.** The
  multiply cannot reach it. This applies to every additive specular pass.
- **`ONLY_SPECULAR` inherits `ONLY_LIGHT`.** Any rule written for `ONLY_LIGHT` silently
  applies to split specular, where it may not belong. Check both.
- **The two pixel-shader bodies must agree term for term.** `LIGHTS >= 4` packs the eye
  vector into `lightDir.w`/`light2.w`/`light3.w` for want of interpolators; `LIGHTS < 4`
  has a dedicated `viewDir`. Both resolve to `mul(tbn, EyePosition - position)`, and the
  directional light is `LightData[0].xyz` un-subtracted in both.
- **`TESR_MaterialMetallic` is written raw, not by name.** `Render.cpp:60` pins the
  register and must track `Includes/Object.hlsl:23` by hand. `ShaderRecord::CreateCT` cannot
  redirect a raw `SetPixelShaderConstantF`. `c137` upward belongs to `SkyAmbient.hlsl` —
  nine float4s for `TESR_SkyIrradiance[9]` in mode 0 — so a register there aliases the sky
  silently, since name binding makes the overlap legal at compile time.

## Verified engine layout

From the runtime image (`CoverAnimations.runtime-image.bin`, base `0x400000`, mapped so
file offset equals RVA):

- `BSShaderPPLightingProperty` constructor at `0x00B66F80` — `fst` sweeps `0x84`→`0xA0`
  (the two `NiColorAlpha`s), `mov [esi+0xA4], 0` is `spTextureSet`, `lea ecx, [esi+0xAC]`
  is `ppTextures`, and `cmp ax, [ebx+0xA8]` is `usLandscapeTextures`. Matches `Game.h`.
- `0x00B67280` — no-arg base-texture accessor, `mov eax,[ecx+0xAC]; mov eax,[eax]`.
  Identical to `*pp->ppTextures[0]`, confirming slot 0 is the base map.
- `0x00B67290` — `SetTexture(n, tex)` writes `ppTextures[0][n]`.
- `0x00B67260` — `GetTexture(array, index)` reads `ppTextures[array][index]`.

## Diagnosing this class of bug

The pattern that worked: **measure the divergence, do not infer it.** Source comparison of
the two shader bodies found no differing term, twice, while a real one existed.

- **`METALDRAW` / `METALPASS` / `METALALBEDO`** (`Render.cpp`) log every distinct pass a
  mapped material draws with, its blend state, and whether the albedo reached it. The
  `Pass` shader trace is **incomplete** — it captures only the first draw of each pass per
  frame — so use `METALDRAW` for composition.
- **`DebugVar1`** (`[_Main.Develop.Main]`, reaches shaders as `TESR_DebugVar.x`, live from
  the toml): `1` permutation class as flat colour, `2` the sun specular term, `3`
  reflectance, `4` the inputs behind reflectance on separate channels, `5` the albedo the
  pass holds. Modes 2–5 gate on a single owning pass and return black elsewhere — black is
  the identity for the additive blends and annihilates the `TEXTURE_Vc` multiply, so either
  decomposition puts the owner's value on screen untouched and the two become directly
  comparable.
- Bisecting with these took four rounds to reach the vertex-colour term; each mode
  eliminated a layer that source reading had wrongly cleared.

Before trusting any in-game result, confirm the deployed DLL and shaders are from the same
build. A shader-only deploy leaves a stale DLL and silently produces a meaningless test.

## Open

- `ParallaxTemplate`'s split-albedo and vertex-colour paths are written but have never
  executed — no parallax metal surface has appeared in a trace.
- Env-mapped surfaces (`SLS2050`/`SLS2057`/`SLS2059`) are still vanilla: no shadows, no
  PBR, no skylighting on that additive pass.
- The diagnostics above are compiled into every object permutation. Inert at
  `DebugVar1 = 0`, but the logging runs per draw on mapped geometry.
