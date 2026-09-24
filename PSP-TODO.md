# Unreal (1998) on PSP-2000: performance TODO

State on 2026-09-24: card build renders correctly, 18-19 fps in typical
intro scenes, 11-13 fps in the heaviest castle views (20 fps cap). All
numbers below are from the real PSP over PSPLink; PPSSPP timings do not
transfer (its recompiler makes CPU work nearly free).

Where a heavy frame goes (ms, castle view): BSP occlusion ~33 (traversal
13, clip 7, span buffer 7, edge raster 5, bounds 1.4), surface drawing ~15,
meshes 9-12 (driver vertex building 4-5, lighting 1.5, keyframe lerp 0.1).

## Simple, to do next

- [ ] Music volume/effects balance after the louder defaults
      (SoundVolume 255, MusicVolume 180).
- [x] Per-thread CPU shares read on the PSP (2026-09-24): main 55-65%,
      OpenAL mixer 9-20%, rest ~3%. Main was blocked ~9 ms/frame in the
      vblank wait (SwapInterval=1) and 0.2 ms in the GE finish: the GE is
      not a bottleneck. The EAX reverb was most of the mixer (9% -> 2%
      without it). Both are now off by default in the card ini
      (`SwapInterval=0`, `UseReverb=False`), each worth ~6% mean fps;
      revert either in Unreal.ini if tearing or dry rooms bother you.
- [ ] Remaining main-thread idle (~14% with vsync off): find it. Candidates:
      Memory Stick reads on the main thread (lazy asset loads, texture
      package reloads), pspgl waiting for a free display list, the input
      thread. Add timers around sceIoRead/appFread and pspgl's submit.
- [ ] OpenAL mixer on the Media Engine: no longer worth it (2% without
      reverb). Reverb on the ME would be, if the reverb is wanted back.

- [ ] Level load time (hardware only): the load profile had 8.6k window
      refills for 27 MB, i.e. ~3 KB per Memory Stick read, because the
      first refill after a seek starts at 2 KB. Per-read latency likely
      dominates. `-REFILLKB=N` (2..16) and the new "LoadMap ... took" log
      line give the A/B; bulk-serialising POD arrays (FColor is read as
      four separate bytes, FMeshVert as one int) is the follow-up.

## Bigger, in order of expected payoff

- [ ] Hot/cold data layout for BSP nodes / points / vertex pool
      (64-byte lines, 2-way 16 KB D-cache). Only if the prefetch result
      shows the misses are the cost.
- [x] Media Engine music (2026-09-24): libxmp renders the module on the ME
      (mcidclan's me-core bridge; `[PSP] MusicME`, `MusicMERate`,
      `MusicMEStereo`), 44.1 kHz stereo straight into the hardware channel,
      interactive sections via a command word. The ME sits mostly idle, the
      frame rate is unchanged. The WAV renders are now only a fallback
      (installer: `PSP_WAV_MUSIC=1`); the Music/*.wav on the card can go.
      Still to check by ear in a level: section changes, fades.
- [ ] Media Engine, next candidates now that the bridge works: OpenAL's
      software mixer (audioOutput thread, priority 22 on the main CPU), or
      the mesh pass once its CPU share is understood.
- [~] Botmatch "out of memory" (2026-09-24 evening): package texture
      texels are now dropped as they load (UTexture::Serialize; the driver
      re-reads them via appReloadObject for the first upload) and sound
      samples are freed once OpenAL holds them. Measured over PSPLink with
      `./Unreal.prx DmRadikus.unr?Game=UnrealI.DeathMatchGame`: the map now
      LOADS on the PSP (22.9 s) and renders at 19.6 fps -- with 138 KB of
      heap free, so in-game allocations can still fail. The PSP's heap
      reaches ~44 MB. The object list after load accounts for only ~12 MB
      (meshes 6.6, level geometry 1.6; the "Sound" line reports original
      sizes); ~30 MB sit outside UObjects: OpenAL sample copies, the music
      module, GCache (CacheSizeMegs), memory stacks, file windows, pspgl,
      allocator overhead. Next: measure those by disabling subsystems
      ([Engine.Engine] AudioDevice= empty in the emulator), then trim
      the biggest. `[PSP] MemDump=1` lists objects after each load.

## Rejected on hardware numbers (do not retry)

- No software occlusion at all (frustum + Z-buffer, the Quake-port way):
  13.4 fps mean / 7.1 worst vs 16.3 / 10.8. Surface drawing doubles and
  mesh work more than doubles (actors behind walls are no longer culled).
  UE1 has no PVS; the span buffer earns its 30 ms.
- `-Os`: 15.5 mean / 9.4 worst vs 16.3 / 10.8; occlusion +13%, mesh +25%.
  The 16 KB I-cache does not make smaller code faster here. Stay on -O2.
- `-mno-check-zero-division`: 16.1 vs 16.3, noise. Left on (harmless).

- Software prefetch (`cache 0x1e` fills for the child nodes and a node's
  points one step ahead): 16.2 vs 16.3 fps mean, 10.6 vs 10.8 worst. The
  traversal's misses are not hidden by one-step-ahead fills. Kept behind
  `[PSP] Prefetch` (default 0) / `-PREFETCH=N`.
- Skipping edge raster + span buffer for polygons under 64 or 256 px^2:
  raster+span fell 614 -> 587/534 ms per 100 frames but surface drawing
  rose 759 -> 945/1049; worst window 10.8 -> 9.3/9.0 fps. Same verdict as
  PPSSPP gave. Kept behind `[PSP] OccludeMinSize` (default 0).

- VFPU for the occlusion clip transform: arithmetic 2.1x faster, pass
  unchanged (memory-bound). Kept as a verified building block.
- GE vertex morphing for mesh animation: the keyframe interpolation it
  would replace is 0.1 ms/frame of the 9-12 ms mesh cost. The cost is
  per-triangle setup, per-vertex lighting and the driver's vertex
  building, none of which the GE morph unit touches.
- CurvedSurfaces=False: no measurable gain, angular actors.

## Done (see git log on psp-port)

- Single-precision math everywhere (no soft doubles), 333 MHz clock,
  hardware palettes, facet/mesh batching, mapped pspgl VBO vertex ring,
  pre-rendered music on a hardware channel, adaptive file window,
  lightmap vertex lighting, gamma/light curve, texture budget/eviction.
