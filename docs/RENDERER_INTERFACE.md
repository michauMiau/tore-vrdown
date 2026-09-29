# Mapa interfejsu renderera Teardown

Silnik nazywa swoje bufory i trzyma nazwy jako stringi w obrazie. Każda ma
dokładnie jedną referencję `lea [rip+disp]`, co daje miejsce tworzenia i rozmiar
w `[rsp+0x24]`. To jedyna metoda, która nie zgaduje.

## Bufory (12)

| rva nazwy | nazwa | rozmiar | slot |
|---|---|---|---|
| `0x98AD38` | `Renderer::OutcomposeBuffer` | — | — |
| `0x99BEC8` | `Renderer::BarrelUpdatebleBuffer` | — | — |
| `0x99BEE8` | `Renderer::SceneImmutableBuffer` | `0x150` = 336 B | `+0x9C0` |
| `0x99BF08` | `Renderer::SceneDynamicBuffer` | `0x1F4` = 500 B | `+0xB90` |
| `0x99BF28` | `Renderer::SceneUpdatableBuffer` | `0x1F4` = 500 B | `+0xBA0` |
| `0x99BF48` | `Renderer::ExposurePerObjectBuffer` | — | — |
| `0x99BF70` | `Renderer::PostUpdatableBuffer` | — | — |
| `0x99CA50` | `Renderer::mTestVisibilityLightFlaresBuffer` | — | — |
| `0x99CA80` | `Renderer::mLensDirtAlphaBuffer` | — | — |
| `0x9E92F6` | `Renderer::EditorVoxImmutableBuffer` | — | — |
| `0x9E9326` | `Renderer::EditorVoxDynamicBuffer` | — | — |
| `0x9E934E` | `Renderer::mGBuffer` | — | — |

## Pola w shaderach (110 nazw `mub*`)

`mub` = **multi-unstructured-buffer**, struktura HLSL.

### Kamera i projekcja

```
mubVpMatrix
mubVpInvMatrix
mubMvpMatrix
mubCameraPos
mubNearFar
mubInvFar
mubTransformMatrix
mubCubeMapMatrix
mubIsoForward
mubOutlineVpMatrix
mubOutlineCameraPos
mubOutlineIsoForward
mubOldStableVpMatrix      <- TAA
mubOldViewMatrix          <- TAA
mubOldModelMatrix
mubModelMatrix
```

`mubOldStableVpMatrix` i `mubOldViewMatrix` potwierdzają TAA. Stereo będzie
potrzebować historii per-eye — te dwa pola trzeba mnożyć per oko.

### Oświetlenie

```
mubDiffuseLights[instanceId]   .lightTransform[0..2]   .radius   .range
                              .unshadowed   .area   .halfLength
                              .fogIterations .fogScale
mubLightDir
mubAmbientColor
mubAmbientParams
mubHdrParams
```

`lightTransform[3]` to macierz 3×4 na świat, `.w` to pozycja — z tego shader
liczy diffuse bez osobnego bufora światła.

### Obraz

```
mubPixelSize          mubNativePixelSize
mubSceneBrightness    mubExposureParams  (rg = minmax, ba = override)
mubColorBalance       mubHdrParams
mubDofParams          (r=focusMin g=focusMax b=focusScale a=far)
mubMaxRadiusRndPixelSize
mubBloomParams        mubHudBlurParams
mubOutlineColor       mubOutlineCameraPos
mubIsOpenGLTexSpace   mubNeedTexCoordFlip
```

### Scena

```
mubVoxelSize          mubVoxUIntData     mubChunksNum
mubVolMatrix          mubVolInvMatrix    mubMaxValue
mubPalette            mubMultColor
mubFogParams          mubFogColor        mubFogType
mubFogHeightOffset    mubApplyFog
mubEditorParams       (r=near g=far ba=pixelSize)
mubFrameParams
```

`mubIsOpenGLTexSpace` i `mubNeedTexCoordFlip` to te same pola dla obu backendów —
potwierdza, że ścieżka DX12 i OpenGL idzie tą samą strukturą.

### Obiekty i debug

```
mubBones              mubGenericShaderFlag
mubAmount             mubColor
mubConstantColor      mubCubeMapColor
mubOffset             mubParams   mubParams2
mubAlpha              mubLineWidthAlpha
mubEmissiveGlassCount mubFoamParams
mubBoundaryColor      mubBoundaryVisibleDistance
mubBindlessTexHandle  mubParticleSystemParams
```

## Co to daje dla stereo

Bufor ma 500 bajtów, a shader zna 110 nazw pól. Stereo wymaga:

1. **`mubVpMatrix` i `mubVpInvMatrix`** — zastąpić per oko off-axis
2. **`mubCameraPos`** — przesunąć o ±IPD/2 wzdłuż osi X kamery
3. **`mubOldStableVpMatrix` i `mubOldViewMatrix`** — TAA wymaga historii per oko,
   inaczej obraz będzie miał ghosting
4. **`mubDofParams.a` (far) i `mubInvFar`** — zgodne, bo z tej samej projekcji

Pozostałe pola nie zależą od oka.

## Dlaczego wcześniejsze szukanie nie działało

`SceneDynamicBuffer` jest `StructuredBuffer` wypełniany przez CPU i czytany przez
GPU. Nazwa bufora jest w obrazie, rozmiar jest w kodzie tworzenia, a struktura
jest w tekstach shaderów. Wszystkie trzy rzeczy były dostępne od początku — szukałem
macierzy w pamięci zamiast użyć nazw, które silnik sam wystawia.
