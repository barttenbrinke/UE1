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

- [x] Texture corruption "here and there" (walls, HUD icons, fonts; also
      in the emulator). Cause: `TArray(INT InNum=0)` calls Realloc for
      zero elements and `appRealloc(NULL,0)` returns newlib's malloc(0)
      block, so an EMPTY mip array still has a non-NULL Data pointer.
      `UTexture::GetInfo` set `DataPtr = &DataArray(0)` unconditionally;
      for a texture whose texels were skipped at load that pointed at a
      16-byte block, the render device saw "data present", never reloaded,
      and uploaded the heap behind it. Fixed: GetInfo hands out NULL for an
      empty array (PSP only), the read-and-drop fallbacks in the texture,
      sound and mesh serialisers release their scratch allocations, and
      `-TEXCRC` logs a checksum of every base level handed to the GE plus
      the resident texture/mesh data after a load (`PSPTEXCRC`,
      `PSPDATACRC`; `-IOCHECK` cross-checks the file layer's read paths on
      the device -- all clean). Emulator and card upload checksums now
      match. Side finding: every default-constructed TArray costs a
      malloc(0) chunk; returning NULL from `appRealloc(NULL,0)` would save
      ~16 bytes per empty array (thousands) but any `&Arr(0)` on an empty
      array would then be NULL -- untested, left alone.
      The dynamic-texture copy ring (three slots -> 512 KB byte ring) was
      a real but separate hazard and stays.
- [ ] Vortex Rikers collapsing-floor sequence (save slot 0, 2026-09-28),
      replayed on the card with `-LOAD=0 -WALKDELAY=15 -AUTOWALK=12`.
      What happens: the trigger tilts the floor, an Earthquake actor
      throws the corpses in the corridor around, they gib into chunks
      (CreatureChunks, Thigh, MaleHead, Arm1, Leg1, Stomach), 18
      ExplosionChain actors fire, and every one of those needs meshes,
      textures and sounds that were skipped at load. Four costs stack:
      (a) The corridor view itself runs at 12 fps before anything happens:
      ~800 mesh polys/frame (decorations, corpses), mesh pipeline 21 ms +
      occlusion 22 ms + polyv 11 ms + illum 9 ms per frame. General
      mesh-heavy-view cost; the lower deck runs 27 fps with 0 mesh polys.
      (b) DONE: openal-soft (the SDK's 1.6.372) is gone from the PSP
      build. Its mixer thread sat at priority 22 above the game thread's
      36 and took 25-57% of the CPU with 4-6 voices playing (7% in a bot
      match), stretching every stick wait and frame behind it. Replaced
      by PspMix (NOpenALDrv/PspMix.*): a 32-voice integer mixer (16.16
      resampling with linear interpolation, Q12 gains, 1024-frame stereo
      blocks at 44100 Hz, ring of 4) whose voice table lives in uncached
      memory. The Media Engine mixes the blocks inside the existing music
      render loop; when the ME is off (PPSSPP, -MUSICME=0) a blocking CPU
      thread (priority 18) mixes each block just before
      sceAudioOutputBlocking. PspAL.cpp is a thin OpenAL shim over it (the
      driver's AL calls are unchanged): linear-clamped distance model and
      constant-power stereo pan on the CPU per source change, no doppler,
      no effects, streaming queue with gapless Next hand-off for the CPU
      music path. Result: the audio thread no longer registers in the CPU
      accounting, mid-play stick reads back to 2 ms, the corridor view
      12 -> 17 fps standing still, the collapse replay has no frame over
      230 ms with 16 voices mixing, 0 underruns. -MAXVOICES/-RESAMPLER/
      -OUTPUTRATE are moot now (the shim ignores the last two).
      Not judged by ear yet: panning width, loudness vs. music, looping
      ambient seams.
      (c) Mid-play reloads (first-play sounds, first-draw textures,
      chunk meshes) at ~60 ms per stick read because of (b): the raw
      driver does 2 ms (`-IOCHECK`, `PSPIOBENCH` mid-walk). DONE:
      appReloadObject raises the main thread to priority 20 for the
      reload (`[PSP] ReloadPriority`, `-RELOADPRIO=`): stall stick share
      1.2 s -> 0.1-0.2 s. DONE: the smallest deferred sounds are reloaded
      (appReloadObject; USound::Serialize registers them) after each
      level load up to `[PSP] SoundPrefetchKB` (1536; keep under
      SoundBudgetKB), ~1 s per load; this level has 157 deferred sounds /
      5.4 MB, so the budget covers ~70 of them.
      (d) Timer bunching: ULevel::Tick clamped the step at 0.4 s, so a
      stalled frame let all 18 ExplosionChain delays (0.3-0.9 s) expire
      together: one 2 s frame (HurtRadius = VisibleCollidingActors, a
      line check per actor in radius, ~35 ms per explosion), which
      stalled the next frame, which bunched the next batch. DONE:
      `[PSP] MaxDeltaMs` (200; 100 made the sequence run in slow motion
      and the player crossed the pit before it opened).
      Result on the card build under PSPLink: worst frame 4.2 s -> 1.4 s,
      but 14 frames over 250 ms remain (some are the USB log itself).
      Next levers: the mixer (b), then the mesh pipeline (a).
      Tools: `PSPTEST: slow frame` lines (every frame >60 ms while the walk
      runs: deltas of stick ms/reads/reopens/seeks, linker preload ms,
      reloads, uploads, sounds + RegisterSound ms, mesh reload KB, tick /
      draw / audio ms, level script/actor/move/spawn counters and the top
      six actor classes by tick time -- PSP_KEEP_UCLOCK builds only),
      per-file stick attribution on the frame report and the LoadMap
      line (`appPspStickReport`), `PSPIO:` lines for kernel reads over 10
      ms, `-IOCHECK` / `appPspIoBench` raw driver cost, `PSPSND: after
      load` sound census, `-MAXVOICES=`, `-OUTPUTRATE=`, `-RESAMPLER=`.
- [ ] Crash replay from a save (2026-09-28): `-LOAD=N` loads save slot N
      (0 is valid) as soon as the entry level is up; `-AUTOWALK=secs` then
      holds the stick full forward (NSDLDrv injects SDL's -32767 on LEFTY
      while the engine counts `GPspAutoWalkLeft` down, 0.1 s per tick at
      most: the tick after a load carries the whole load time), after
      `-WALKDELAY=secs` of standing still so the post-load spike (first
      draw: ~40 reloads, 150 uploads, 1.6 s) is out of the way.
      Workflow: save in-game facing the spot, `scratchpad/pull-save.sh N`
      copies `Save/SaveN.usa` (plus hub files) over PSPLink into the PPSSPP
      Save folder, replay in the emulator for logic and on the PSP under
      PSPLink for timing (PPSSPP is several times faster and capped at 20
      fps: frame-rate problems do not show there).
- [ ] Level load time, remaining half: on the card an Entry load is now
      7.3 s of which 4.0 s is stick I/O, DmRadikus 14.3 s / 7.7 s. The
      `-REFILLKB` A/B (see the done list) showed bytes moved, not read
      count, set the I/O time, so the next lever is reading less: the
      loader still pulls ~5.5 MB (Entry) / 12.5 MB (DmRadikus) through
      the windows for objects it needs. The non-I/O half is the linker
      and serialisers themselves; bulk-serialising POD arrays (FColor is
      read as four separate bytes, FMeshVert as one int) is the follow-up.

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
      MaxFPS 30: mean 24.0 fps (worst 13.6) vs 17.8 at the 20 cap, same
      match. The mixer thread (openal-soft) was then 23% of the CPU with
      64 sources; `[PSP] MaxVoices` (default 16, UE1 drops the lowest
      priority sound when voices run out) took it to 7.5% and the match
      to 27.3 fps mean, worst 14.8. Sixteen voices not yet judged by
      ear (ambient-heavy levels may lose sounds).
      Level load time (Entry 19 s, DmRadikus 23 s on the card). The load
      line now reports stick traffic: an Entry load made 6274 reads with
      5710 seeks for ~5 MB of package data -- the loader visits objects on
      demand and jumps around the 37 MB UnrealI.u, so one 16 KB window
      thrashed, and every texel/sample/mesh byte was read only to be
      dropped. Three changes: files under `[PSP] LoadCacheMB` (2) are
      read whole while a load runs (under a `LoadCacheHeapMB` 30 guard;
      the first version at 8 MB / 38 MB pushed the emulator into
      out-of-memory), sixteen LRU read windows per file during loads (one
      while playing), and the texture, sound and mesh serialisers now
      Skip() past data they would drop instead of reading it
      (FArchive::Skip, a seek in the file loader). Emulator counts:
      DmRadikus 7764 reads / 38.6 MB -> 3775 reads / 16.7 MB; Entry 6274
      / 17 MB -> 1930 / 6.8 MB. On the card: Entry 19.6 s -> 7.3 s,
      DmRadikus 28.5 s -> 15.1 s, four-bot match steady at 29 fps.
      `-REFILLKB` A/B on the card (DmRadikus load): 1 KB 14.3 s, 2 KB
      15.1, 4 KB 16.8, 8 KB 19.1, 16 KB 24.2 -- the stick time tracks
      bytes moved, not read count, so the first refill after a seek is
      now 1 KB (PSP_FILE_MINREFILL), doubling while reads stay sequential.
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
