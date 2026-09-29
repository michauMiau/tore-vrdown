# Layout buforów renderera — z deklaracji w obrazie

Silnik trzyma teksty HLSL jako stringi. Deklaracje są kompletne, z komentarzami.
`DECLARE_CONSTANT_BUFFER(nazwa, rejestr)` — drugi argument to rejestr **b** w HLSL,
czyli slot w root signature.

## SceneDynamicBuffer — rejestr 1, 500 B

```hlsl
DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)
{
    float4x4 mubVpMatrix;
    float4   mubViewXDir;        // xyz - dir, w - unused
    float4   mubViewYDir;        // xyz - dir, w - unused
    float4   mubViewZDir;        // xyz - dir, w - near plane bound sphere radius
    // x - 1/m[0], y - 1/m[5], z - m[8]/m[0], w - m[9]/m[5]
    float4   mubProjectionData;
    float4x4 mubViewMatrix;
    float4x4 mubOldViewMatrix;

    float4x4 mubStableVpMatrix;
    float4x4 mubOldStableVpMatrix;

    float4   mubCameraPos;       // rgb
    float4   mubPlayerPos;       // rgb

    float4   mubFrameParams;     // r Time, g ScreenshotModeSimTime, b SimTime, a FrameRnd

    float4x4 mubTransformMatrix;

    uint     mubGenericShaderFlag;
};
```

| offset | pole | rozmiar |
|---|---|---|
| `0x000` | `mubVpMatrix` | 64 |
| `0x040` | `mubViewXDir` | 16 |
| `0x050` | `mubYDir` | 16 |
| `0x060` | `mubViewZDir` | 16 |
| `0x070` | `mubProjectionData` | 16 |
| `0x080` | `mubViewMatrix` | 64 |
| `0x0C0` | `mubOldViewMatrix` | 64 |
| `0x100` | `mubStableVpMatrix` | 64 |
| `0x140` | `mubOldStableVpMatrix` | 64 |
| `0x180` | `mubCameraPos` | 16 |
| `0x190` | `mubPlayerPos` | 16 |
| `0x1A0` | `mubFrameParams` | 16 |
| `0x1B0` | `mubTransformMatrix` | 64 |
| `0x1F0` | `mubGenericShaderFlag` | 4 |

Suma: `0x1F4` = **500**, zgadza się z `mov [rsp+0x24], 0x1F4` w kodzie tworzenia.

## SceneImmutableBuffer — rejestr 0, 336 B

```hlsl
float4   mubAmbientColor;
float4   mubConstantColor;
float4   mubAmbientParams;        // r Exponent, g Scale
float4x4 mubCubeMapMatrix;
float4   mubCubeMapColor;
float4   mubSunColor;
float4   mubSunDir;
float3   mubSunParams;            // r SunFogScale, g SunLength, b SunSpread
uint     mubFogType;
float3   mubFogColor;
float    mubFogHeightOffset;
float4   mubFogParams;            // r FogStart, g 1/FogDist|FogDensity, b FogMax, a HeightExponent
float4   mubShadowVolumeResolution;
float4   mubInvShadowVolumeResolution;
float4   mubShadowVolumeTexelSize;
float4   mubShadowVolumeOffset;
float4   mubWetParams;            // r Wetness, g PuddleAmount, b PuddleFreq
float4   mubBoundaryColor;
float4   mubBoundaryVisibleDistance;
float4   mubIsOpenGLTexSpace;
```

## SceneUpdatableBuffer — rejestr 2, 500 B przydzielone

```hlsl
float4 mubPixelSize;         // rg PixelSize, ba 1/PixelSize
float4 mubNativePixelSize;   // rg PixelSize po upscale
float4 mubNearFar;           // r Near, g Far, b 1/Far, a (near - far)
```

48 B treści przy 500 B przydziale — reszta to wyrównanie.

## Pozostałe

```
ExposurePerObjectBuffer  rejestr 3   mubExposureParams, mubSceneBrightness
PostUpdatableBuffer      rejestr 3   mubColorBalance, mubPostParams
OutcomposeBuffer         rejestr 3   mubHdrParams
LinesBuffer              rejestr 3   mubVpMatrix, mubPixelSize, mubInvFar, mubAlpha
EditorVoxDynamicBuffer   rejestr 1   mubVpMatrix, mubVpInvMatrix, mubCameraPos, mubIsoForward
```

`EditorVoxDynamicBuffer` i `LinesBuffer` też mają własny `mubVpMatrix` — te trzeba
podać identycznie, inaczej linie i voxele nie trafią.

## Co to zmienia dla stereo

Per-eye trzeba podmienić:

1. `mubVpMatrix` — off-axis, obie projekcje
2. `mubViewXDir/YDir/ZDir` — kierunki, zależne od oka
3. `mubProjectionData` — **nie tylko VP**: to 1/m[0], 1/m[5], m[8]/m[0], m[9]/m[5]
4. `mubViewMatrix` — przesunięta pozycja oka
5. `mubCameraPos` — przesunięta o ±IPD/2
6. `mubStableVpMatrix` + `mubOldStableVpMatrix` — TAA per oko
7. Ten sam VP w `LinesBuffer` i `EditorVoxDynamicBuffer`

Nie trzeba zmieniać: `mubPlayerPos`, `mubFrameParams`, `mubTransformMatrix`,
wszystkich buforów immutable i updatable.

## Uwaga o `mubProjectionData`

To pole to nie tylko VP — zawiera odwrotności elementów diagonalnych i składowe
poza przekątnej. Off-axis zmienia zarówno `m[0][0]`, jak i `m[0][2]`/`m[1][2]`, więc
`z = m[8]/m[0]` i `w = m[9]/m[5]` też się zmienią. Trzeba je przeliczyć, nie
kopiować.
