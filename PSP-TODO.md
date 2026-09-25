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
- [~] Botmatch "out of memory": mesh render data (Verts/Tris/Connects/
      VertLinks) and sound uploads are now lazy, like the texture texels:
      dropped at load, re-read from the package on first draw / first
      play. Emulator DmRadikus load: heap 44.0 -> 32.0 MB (was 140 KB from
      the ceiling on the PSP). `[PSP] FreeMeshData/FreeSoundData/
      FreeTextureData` turn each off. Boot crash of the first texture
      version (procedural textures reading freed source texels) fixed by
      PspEnsureTexels + TF_PspPinned. Verified on the PSP (card 7846f886):
      flyby clean, DmRadikus loads with heap 39.4 MB after load / 41.1 MB
      running (43 KB free) -- the emulator's 32 MB does not include the
      Media Engine music (module bytes + libxmp's copy, ~7 MB). Next:
      UMusic::Data freed after xmp_load (built, untested), then an A/B on
      the PSP with `-MUSICME=0` to size the rest of the music cost.
      XMB boot crash of 7846f886 with lazy loading on: NOT the lazy
      loading itself -- my low-memory probe called mallinfo() on every
      large allocation and newlib's bin walk raced the mixer thread
      (fixed in 1b130a3, probe now opt-in). Second boot crash of the
      staged build: the same probe read the command line before appInit()
      had one (NULL deref in ParseParam; fixed). Silent sound effects with
      UseReverb=False: with EFX compiled out the reverb `if` had an empty
      body and swallowed the play/stop switch (fixed, verified by ear and
      by the `-SNDLOG` trace). Card now runs df97bdfe with the lazy-on
      ini (LightGamma 210). The level-one "big room" crash on the old
      card build was most likely the mallinfo race (any large allocation
      while the mixer thread allocates); the card writes no log, so it
      can only be confirmed by not recurring.
      Botmatch memory, reproduced on PPSSPP (DmRadikus, 4 bots, 5 min):
      the heap grows after load as the match touches content -- mesh
      render data reloaded by GetFrame (+2.6 MB, never freed again),
      texture uploads (+1.3 MB, capped by TextureBudgetMB), sounds
      uploaded on first play (+1.2 MB). Now capped: `[PSP] MeshBudgetKB`
      (default 1024; meshes not drawn for 2 s are dropped, least recently
      drawn first) and `[PSP] SoundBudgetKB` (default 2048; least
      recently played sounds are unregistered). Five-minute result:
      34.0 -> 37.2 MB used (was 39.8 MB). The 100-frame report line now
      carries arena/kernel-free/texture/mesh-reload figures; `-MALLINFO`
      adds allocator used/free (emulator only, see the race above).
      `[Audio] LowSoundQuality` (8-bit, half rate at load) exists as a
      further lever, off by default. Untested on hardware: a DM botmatch
      under PSPLink (set InitialBots in the card ini, the URL takes no
      bot option) to get the real load figure with the ME music; the
      emulator's 32 MB after load excludes it.
      Later levels on PPSSPP (idle at the start): Chizra and SkyTown
      failed to load at 43-45 MB. A table of live blocks >= 64 KB
      (printed after LoadMap with MemDump=1 and at out-of-memory) showed
      ~300 uniform 64-75 KB blocks: UDatabase::Serialize restores the
      editor's capacity (DbMax, grown by "256 + a quarter") for every
      brush Polys and BSP table -- 19 MB of slack on Chizra. Allocating
      the stored count instead (UnModel.cpp) gave, after load:
      Chizra 44.8 -> 29.3 MB, SkyTown 42.9 -> 29.5, Dig 31.5 -> 26.4,
      NyLeve 27.6 -> 22.1, DmRadikus 32.0 -> 17.0 (emulator, no ME
      music). Still unexplained: ~6 MB in 21 generic "FArray" blocks
      after a Chizra load; tagging those reallocs is the next diagnostic.
      Level transitions (PPSSPP, `-MAPCYCLE=35`, three rounds over five
      maps): no leak. Entry between maps settles at ~15.5 MB after the
      game packages load once; the per-round creep is resident sounds
      under their budget. The 556 KB "TArray<AActor*>" block is the BSP
      light list (UModel::Lights), static data.
      Save / load (PPSSPP, `-SAVETEST=25` on Dig): SaveGame 9 writes
      ../Save/Save9.usa (4.9 MB), `START ?load=9` reloads it. Two fixes
      on the way: embedded map objects whose data was lazily freed are
      reloaded before SavePackage (else the save holds empty arrays), and
      the old level is garbage-collected before the new package loads
      (a save load or restart held both levels: transient peak 33.6 ->
      31.5 MB on Dig; normal travel goes through Entry anyway). Note the
      load= and hub Game%i.usa strings are URLs: FURL reads a forward
      slash as a host separator, so they keep the backslash and the PSP
      file layer converts it. Untested on hardware: saving to the memory
      stick (appMkdir creates Save/), and the load peak on a big level.
      Hardware (2026-09-25, PSPLink): DmRadikus with 4 bots loads at
      17.0 MB (was 39.4), plays at 16.6 fps mean, arena 27 MB after three
      minutes, no crash. Save/load on the memory stick: the slot writes in
      5 s, loads in 29 s. The first load crashed in pspgl's texture free
      and the morning's exit crash was in _free_r: both after a music
      stop, both gone with -MUSICME=0. Root cause: the Media Engine writes
      libxmp's player state (CPU heap) through its own data cache; a
      64-byte line shared with a neighbouring CPU block is written back
      with stale neighbour bytes. This is also the real cause of the
      "mallinfo race" boot crash (a corrupted bin walked). Fix: libxmp's
      allocations get their own 64-byte aligned, padded blocks via
      --wrap of malloc/calloc/realloc/free (NOpenALDrv.cpp; wrapping
      newlib's _malloc_r instead broke its calloc). mallinfo is safe again
      (appPspHeapUsedKB). Frame dips on the card were memory-stick reads
      inside GetFrame / texture upload (lazy reloads when entering a new
      room): `[PSP] PrefetchHeapMB` (30) reloads the level's meshes and
      textures right after load until the heap reaches the limit (Dig:
      26.4 -> 30.7 MB, +3 s load); MeshBudgetKB default 8192 so they
      stay. Card: c49749ba + lazy ini (InitialBots=4 for the harness).
      To judge by ear/eye: the dips, and the load-from-save peak on a big
      level (arena hit 36.5 MB on Dig).
      Mesh cost on the card (four bots in view, per 100 frames): mesh 3300
      ms of which "sub" 3500 (nested) and tmap 1050. "sub" is curved-surface
      subdivision: the active client section ([NSDLDrv.NSDLClient]) had
      CurvedSurfaces=True while the earlier "no gain" test had flipped the
      WinDrv section. Hardware A/B with it off: mesh 1679 -> 622 ms/100f
      average over the match, sub 1585 -> 0, tmap 519 -> 316. Installer
      now writes CurvedSurfaces=False into the NSDLClient section.
      GE mesh path, first step (DrawMeshTris): the renderer hands the
      driver each mesh's visible triangles per texture/flag set when all
      three vertices are inside the view and past the near plane; the
      driver writes them straight into the batch ring with the vertex
      colour converted once per vertex per call. Clipped, unlit,
      environment-mapped triangles keep the per-polygon path. Verified on
      PPSSPP (clean); hardware numbers pending. Next steps for the GE
      path: indexed draws (dedupe vertex+UV pairs per mesh once), and
      moving the vertex transform to the GE via a per-actor matrix.
      A falling ASMD pickup destroyed itself with "moved without proper
      hashing" (fatal appError, DmRadikus): on PSP the unhash now removes
      the links at the hashed location and warns instead.
      Hardware, four-bot DmRadikus, mean fps 16.6 (curved on) -> 17.0
      (curved off) -> 18.0 (+ triangle list). Then two driver items from
      the per-interval breakdown: canvas tiles were one glBegin each
      (tile 387 -> 74 ms/100f once batched through the vertex ring, with
      "tile" in the batch key since tiles draw with the depth test off),
      and realtime texture re-uploads copied 4 bytes per texel for 8-bit
      fire textures (image 444 -> 129, complex 577 -> 290). Frame rate
      stayed at the 20 fps cap; the main thread went 66% -> 48% busy, so
      the cap is now the limit: MaxFPS=30 is the next A/B.
      Background: the 1998 engine leaned on the PC's virtual memory;
      retail patches later added TLazyArray for mips and sounds, which
      this source snapshot predates.

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
