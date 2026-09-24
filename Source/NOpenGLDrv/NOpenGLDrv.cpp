#include "SDL2/SDL.h"
#ifdef __PSP__
#include <pspsysmem.h>
#include <malloc.h>
#include "glad_psp.h"
#else
#include "glad.h"
#endif

#include "NOpenGLDrvPrivate.h"

#ifdef PSP_PSPLINK
// PSPLink-only diagnostic. Linked with -Wl,--wrap=glDrawArrays, so every
// glDrawArrays from our code lands here. "-HALTDRAW=N" on the command line
// freezes the main thread right BEFORE the Nth draw (after N-1 have been
// issued), so pspgl's static display-list array can be pulled out of PSP
// memory with pspsh's savemem and decoded on the host. Draw 14 is the one
// that first fills a 512-word list and hands it to the GE.
#include <pspthreadman.h>
extern "C" void __real_glDrawArrays( GLenum Mode, GLint First, GLsizei Count );
extern "C" void __wrap_glDrawArrays( GLenum Mode, GLint First, GLsizei Count )
{
	static INT Halt = -1, N = 0;
	if( Halt < 0 )
	{
		Halt = 0;
		Parse( appCmdLine(), "HALTDRAW=", Halt );
	}
	++N;
	if( Halt && N == Halt )
	{
		printf( "PSPHALT: frozen before draw %d (mode=%d first=%d count=%d); dump pspgl dlists now\n", N, (int)Mode, (int)First, (int)Count );
		fflush( stdout );
		for( ;; )
			sceKernelDelayThread( 1000000 );
	}
	__real_glDrawArrays( Mode, First, Count );
}
#endif

#ifdef PSP_PSPLINK
// GE queue diagnostics, also PSPLink-only (-Wl,--wrap on the sceGe calls).
//   -NOGE    never hand a list to the GE (draws are built but never run)
//   -SYNCGE  wait for each list to finish right after queueing it, so a
//            GE-triggered fault lands on the main thread at a known draw
//   -DUMPGE  print the first four lists word by word as they are queued
#include <pspge.h>
extern "C" int __real_sceGeListEnQueue( const void* List, void* Stall, int CbId, PspGeListArgs* Arg );
extern "C" int __real_sceGeListSync( int Qid, int Mode );
extern "C" int __real_sceGeDrawSync( int Mode );
static INT GPspNoGe = -1, GPspSyncGe = 0, GPspDumpGe = 0, GPspGeLists = 0;
static void PspGeSwitches()
{
	if( GPspNoGe >= 0 ) return;
	GPspNoGe   = ParseParam( appCmdLine(), "NOGE" );
	GPspSyncGe = ParseParam( appCmdLine(), "SYNCGE" );
	GPspDumpGe = ParseParam( appCmdLine(), "DUMPGE" );
}
extern "C" int __wrap_sceGeListEnQueue( const void* List, void* Stall, int CbId, PspGeListArgs* Arg )
{
	PspGeSwitches();
	++GPspGeLists;
	const DWORD* W = (const DWORD*)List;
	INT Len = ( (const DWORD*)Stall - W ) + 1;
	printf( "PSPGE: enqueue #%d list=%p len=%d\n", GPspGeLists, List, Len );
	fflush( stdout );
	if( GPspDumpGe )
	{
		// Every list, raw, to the host BEFORE the GE runs it, so the one that
		// kills the console can still be decoded (decode_dlist.py --file).
		static FILE* F = NULL;
		if( !F ) F = fopen( "host0:/lists.bin", "wb" );
		if( F )
		{
			DWORD Hdr[4] = { 0x5453494c, (DWORD)GPspGeLists, (DWORD)Len, (DWORD)(size_t)List };
			fwrite( Hdr, sizeof(Hdr), 1, F );
			fwrite( W, 4, Len, F );
			fflush( F );
		}
	}
	if( GPspNoGe )
		return 0x7000 + GPspGeLists;
	int Q = __real_sceGeListEnQueue( List, Stall, CbId, Arg );
	if( GPspSyncGe )
	{
		int R = __real_sceGeListSync( Q, 0 );
		printf( "PSPGE: list #%d done (sync=%d)\n", GPspGeLists, R );
		fflush( stdout );
	}
	return Q;
}
extern "C" int __wrap_sceGeListSync( int Qid, int Mode )
{
	PspGeSwitches();
	return GPspNoGe ? 0 : __real_sceGeListSync( Qid, Mode );
}
extern "C" int __wrap_sceGeDrawSync( int Mode )
{
	PspGeSwitches();
	return GPspNoGe ? 0 : __real_sceGeDrawSync( Mode );
}
#endif

#ifdef __PSP__
//
// Vertex data for the GE must stay untouched until the GE has actually read it.
// pspgl does not copy client arrays -- it hands the pointer to the hardware and
// flushes the range with sceKernelDcacheWritebackInvalidateRange -- and the GE
// runs asynchronously, so a single scratch buffer that every poly overwrites
// (worse, one moved by realloc) is read back as garbage a few draws later.
// PPSSPP consumes draws synchronously and never shows this.
//
// So hand out each draw its own slice of a ring, and only reuse the ring once
// glFinish says the GE is done with it. One sync per wrap rather than per draw.
//
enum { PSP_VTX_RING_BYTES = 512 * 1024 };
static void PspFlushBatch();   // mesh triangle batch, defined with the emitters below
static INT   GPspBatchDraws = 0, GPspBatchPolys = 0, GPspFacetDraws = 0;   // per report interval
static INT GPspLastUploadedMips = 0;   // levels the last UploadTexture really sent (chain may be cut short)
static INT GPspLastUploadBytes  = 0;   // image bytes the last UploadTexture handed to pspgl
static INT GPspTexBytes         = 0;   // image bytes pspgl holds for every cached texture
static INT GPspTexBudget        = -1;  // [PSP] TextureBudgetMB, resolved on first use
static INT GPspUploadFailed     = 0;
static INT GPspUpFirst = 0, GPspUpRealtime = 0, GPspUpBig = 0;   // per report interval
static INT GPspUpBytes = 0;                                       // bytes handed to GL per interval
#include <pspsysmem.h>
// Heap picture for the log: newlib arena in use / free, plus what the kernel
// still has outside the heap. Cheap; used in periodic reports and on failures.
// 8x8 mid-grey, 256 bytes: what a texture gets when pspgl could not take the
// real image, so the GE always has memory to sample.
static DWORD GPspPlaceholder[64];
static UBOOL PspUploadPlaceholder()
{
	if( !GPspPlaceholder[0] )
		for( INT i=0; i<64; ++i ) GPspPlaceholder[i] = 0xff808080;
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void*)GPspPlaceholder );
	return glGetError() == GL_NO_ERROR;
}
static const char* PspHeapStr()
{
	static char Buf[96];
	struct mallinfo M = mallinfo();
	appSprintf( Buf, "heap used %iKB free %iKB (arena %iKB), kernel free %iKB",
		M.uordblks / 1024, M.fordblks / 1024, M.arena / 1024, sceKernelTotalFreeMemSize() / 1024 );
	return Buf;
}
static BYTE* GPspVtxRing    = NULL;
static INT   GPspVtxRingPos = 0;
static FLOAT* GPspVtx     = NULL;   // current slice; valid until the next claim

// Claim Bytes of ring space for a draw that is about to be issued.
static void* PspClaimVtx( INT Bytes )
{
	Bytes = ( Bytes + 63 ) & ~63;           // keep every slice 64-byte aligned
	if( Bytes > PSP_VTX_RING_BYTES )
		return NULL;                        // absurdly large poly; skip it

	if( !GPspVtxRing )
	{
		// Static, not memalign: by the time the renderer starts, the heap top is
		// past 0x0A000000 (the PSP-2000's extra 32MB), and the GE cannot fetch
		// vertices from there. Module data sits at 0x08Exxxxx, which it can.
		static BYTE GPspVtxRingStore[PSP_VTX_RING_BYTES] __attribute__((aligned(64)));
		GPspVtxRing = GPspVtxRingStore;
		if( !GPspVtxRing )
			return NULL;
		GPspVtxRingPos = 0;
	}

	if( GPspVtxRingPos + Bytes > PSP_VTX_RING_BYTES )
	{
		// Wrapping would overwrite data the GE may still be reading.
		glFinish();
		GPspVtxRingPos = 0;
	}

	void* Slice = GPspVtxRing + GPspVtxRingPos;
	GPspVtxRingPos += Bytes;
	return Slice;
}
#endif

/*-----------------------------------------------------------------------------
	Global implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_PACKAGE(NOpenGLDrv);
IMPLEMENT_CLASS(UNOpenGLRenderDevice);

/*-----------------------------------------------------------------------------
	UNOpenGLRenderDevice implementation.
-----------------------------------------------------------------------------*/

// from XOpenGLDrv:
// PF_Masked requires index 0 to be transparent, but is set on the polygon instead of the texture,
// so we potentially need two copies of any palettized texture in the cache
// unlike in newer unreal versions the low cache bits are actually used, so we have use one of the
// actually unused higher bits for this purpose, thereby breaking 64-bit compatibility for now
#define MASKED_TEXTURE_TAG (1ULL << 60)

// FColor is adjusted for endianness
#define ALPHA_MASK 0xff000000

// lightmaps are 0-127
#define LIGHTMAP_SCALE 2

// and it also would be nice to overbright them
#define LIGHTMAP_OVERBRIGHT 1.4f

#ifdef __PSP__
// Frame counter, kept for throttling experiments.
//
// NOTE: throttling TF_RealtimeChanged here was tried and made things visibly
// WORSE. The GL upload is not the expensive part -- the cost is CPU-side:
// UFireTexture regenerating procedural fire (Fire/Src/UnFractal.cpp) and, more
// significantly, UE1 recomputing lightmaps for dynamic lights
// (Render/Src/UnLight.cpp:1924 sets TF_RealtimeChanged on the lightmap).
// Skipping uploads only added judder. Attack the CPU side instead.
static DWORD GPspFrameCount = 0;
static DWORD GPspUploadCount = 0;
static DWORD GPspUploadLast  = 0;
// Signed: the per-frame counters are 32-bit INT and can come back negative if
// a uclock/uunclock pair is unbalanced on some path. Accumulating those into an
// unsigned type produced nonsense like "18446744949882880ms".
static SQWORD GPspAccBind = 0, GPspAccImage = 0, GPspAccComplex = 0;
static SQWORD GPspAccGouraud = 0, GPspAccTile = 0;
// The engine's own renderer stats, to break down what is left over.
static SQWORD GPspAccIllum = 0, GPspAccOcclusion = 0, GPspAccMesh = 0, GPspAccPolyV = 0;
static INT GPspAccClip = 0, GPspAccRaster = 0, GPspAccSpan = 0;   // OccludeBsp's own sub-timers
static INT GPspAccMeshFrame = 0;                                     // UMesh::GetFrame (keyframe lerp + transform)
#endif

#define GL_CHECK_EXT(ext) GLAD_GL_ ## ext
#define GL_CHECK_VER(maj, min) (((maj) * 10 + (min)) <= (GLVersion.major * 10 + GLVersion.minor))

void UNOpenGLRenderDevice::InternalClassInitializer( UClass* Class )
{
	guardSlow(UNOpenGLRenderDevice::InternalClassInitializer);
	new(Class, "NoFiltering",         RF_Public)UBoolProperty( CPP_PROPERTY(NoFiltering),         "Options", CPF_Config );
	new(Class, "UseHwPalette",        RF_Public)UBoolProperty( CPP_PROPERTY(UseHwPalette),        "Options", CPF_Config );
	new(Class, "UseBGRA",             RF_Public)UBoolProperty( CPP_PROPERTY(UseBGRA),             "Options", CPF_Config );
	new(Class, "DetailTextures",      RF_Public)UBoolProperty( CPP_PROPERTY(DetailTextures),      "Options", CPF_Config );
	new(Class, "UseMultiTexture",     RF_Public)UBoolProperty( CPP_PROPERTY(UseMultiTexture),     "Options", CPF_Config );
	new(Class, "AutoFOV",             RF_Public)UBoolProperty( CPP_PROPERTY(AutoFOV),             "Options", CPF_Config );
	new(Class, "UseWindowBrightness", RF_Public)UBoolProperty( CPP_PROPERTY(UseWindowBrightness), "Options", CPF_Config );
	new(Class, "SwapInterval",        RF_Public)UIntProperty ( CPP_PROPERTY(SwapInterval),        "Options", CPF_Config );
	unguardSlow;
}

UNOpenGLRenderDevice::UNOpenGLRenderDevice()
{
	NoFiltering = false;
	UseHwPalette = true;
	UseBGRA = true;
	DetailTextures = true;
	UseMultiTexture = true;
	AutoFOV = true;
	UseWindowBrightness = true;
	CurrentBrightness = -1.f;
	SwapInterval = 1;
}

UBOOL UNOpenGLRenderDevice::Init( UViewport* InViewport )
{
	guard(UNOpenGLRenderDevice::Init)

	if( !gladLoadGLLoader( &SDL_GL_GetProcAddress ) )
	{
		debugf( NAME_Warning, "Could not load GL: %s", SDL_GetError() );
		return false;
	}

	// Startup diagnostics: which SDL video driver and GL implementation we
	// actually got, plus the real heap headroom. Off by default -- the heap
	// probe allocates and frees up to 64MB. Build with -DPSP_DIAGNOSTICS=ON.
#ifdef PSP_DIAGNOSTICS
	{
		const char* VideoDrv = SDL_GetCurrentVideoDriver();
		debugf( NAME_Log, "PSPDIAG: SDL video driver = %s", VideoDrv ? VideoDrv : "(null)" );
		const GLubyte* Vendor   = glGetString( GL_VENDOR );
		const GLubyte* Renderer = glGetString( GL_RENDERER );
		const GLubyte* Version  = glGetString( GL_VERSION );
		debugf( NAME_Log, "PSPDIAG: GL_VENDOR   = %s", Vendor   ? (const char*)Vendor   : "(null)" );
		debugf( NAME_Log, "PSPDIAG: GL_RENDERER = %s", Renderer ? (const char*)Renderer : "(null)" );
		debugf( NAME_Log, "PSPDIAG: GL_VERSION  = %s", Version  ? (const char*)Version  : "(null)" );
		SDL_Window* Wnd = InViewport ? (SDL_Window*)InViewport->GetWindow() : NULL;
		if( Wnd )
		{
			int WW = 0, WH = 0;
			SDL_GetWindowSize( Wnd, &WW, &WH );
			debugf( NAME_Log, "PSPDIAG: window = %dx%d flags=0x%08x", WW, WH, (unsigned)SDL_GetWindowFlags( Wnd ) );
		}
		else
		{
			debugf( NAME_Log, "PSPDIAG: viewport has no SDL window" );
		}
		debugf( NAME_Log, "PSPDIAG: last SDL error = '%s'", SDL_GetError() );
		// How much heap do we actually have? appMalloc's check(Ptr) fires when
		// malloc returns NULL, so the real budget decides whether Unreal's
		// texture set can fit at all.
		{
			struct mallinfo mi = mallinfo();
			debugf( NAME_Log, "PSPDIAG: heap arena=%u used=%u free=%u | kernel free=%u max block=%u",
				(unsigned)mi.arena, (unsigned)mi.uordblks, (unsigned)mi.fordblks,
				(unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize() );

			// mallinfo().arena is only what newlib has sbrk'd so far, NOT the
			// ceiling -- it grows on demand. Probe the real headroom by taking
			// 1MB chunks until malloc fails, then give them all back.
			{
				enum { MAXCHUNKS = 64 };
				void* Chunks[MAXCHUNKS];
				INT n = 0;
				while( n < MAXCHUNKS )
				{
					Chunks[n] = malloc( 1024 * 1024 );
					if( !Chunks[n] ) break;
					++n;
				}
				for( INT i = 0; i < n; ++i )
					free( Chunks[i] );
				debugf( NAME_Log, "PSPDIAG: headroom = %d MB still allocatable (probe cap %d)", n, (int)MAXCHUNKS );
			}
		}
	}
#endif // PSP_DIAGNOSTICS

	SupportsFogMaps = true;
	SupportsDistanceFog = true;

	UpdateSwapInterval();

	if( UseHwPalette && !GL_CHECK_EXT( EXT_paletted_texture ) )
	{
		debugf( NAME_Warning, "EXT_paletted_texture not available, disabling UseHwPalette" );
		UseHwPalette = false;
	}

	if( UseBGRA && !GL_CHECK_VER( 1, 2 ) && !GL_CHECK_EXT( EXT_bgra ) )
	{
		debugf( NAME_Warning, "EXT_bgra not available, disabling UseBGRA" );
		UseBGRA = false;
	}

	if( UseMultiTexture && ( !GL_CHECK_EXT( ARB_multitexture ) || !GL_CHECK_EXT( EXT_texture_env_combine ) ) )
	{
		debugf( NAME_Warning, "ARB_multitexture or EXT_texture_env_combine is not available, disabling UseMultiTexture" );
		UseMultiTexture = false;
	}

	if( UseMultiTexture )
	{
		GLint TMUnits;
		glGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &TMUnits );
		if ( TMUnits < 4 )
		{
			debugf( NAME_Warning, "Not enough texture units (%i, expected 4), disabling UseMultiTexture", TMUnits );
			UseMultiTexture = false;
		}
	}

	debugf( NAME_Log, "Got OpenGL %d.%d", GLVersion.major, GLVersion.minor );

	EnsureComposeSize( 256 * 256 * 4 );
	verify( Compose );

	// Set modelview matrix to flip stuff into our coordinate system.
	const FLOAT Matrix[16] =
	{
		+1, +0, +0, +0,
		+0, -1, +0, +0,
		+0, +0, -1, +0,
		+0, +0, +0, +1,
	};
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	glMultMatrixf( Matrix );

	// Set permanent state.
	glEnable( GL_DEPTH_TEST );
	glShadeModel( GL_SMOOTH );
	glAlphaFunc( GL_GREATER, 0.5 );
	glDisable( GL_ALPHA_TEST );
	glDepthMask( GL_TRUE );
	glBlendFunc( GL_ONE, GL_ZERO );
	glEnable( GL_BLEND );
	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	CurrentPolyFlags = PF_Occlude;
	Viewport = InViewport;

	return true;
	unguard;
}

void UNOpenGLRenderDevice::Exit()
{
	guard(UNOpenGLRenderDevice::Exit);

	debugf( NAME_Log, "Shutting down OpenGL renderer" );

	Flush();

	if( Compose )
	{
		appFree( Compose );
		Compose = NULL;
	}
	ComposeSize = 0;

	unguard;
}

void UNOpenGLRenderDevice::PostEditChange()
{
	guard(UNOpenGLRenderDevice::PostEditChange)

	Super::PostEditChange();

	UpdateSwapInterval();

	unguard;
}

void UNOpenGLRenderDevice::Flush()
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLRenderDevice::Flush);

	if( TexAlloc.Num() )
	{
		debugf( NAME_Log, "Flushing %d/%d textures", TexAlloc.Num(), BindMap.Size() );
		for ( INT i = 0; i < MaxTexUnits; ++i )
		{
			ResetTexture( i );
		}
		glFinish();
		glDeleteTextures( TexAlloc.Num(), &TexAlloc(0) );
		TexAlloc.Empty();
		BindMap.Empty();
#ifdef __PSP__
		GPspTexBytes = 0;
#endif
	}

	unguard;
}

#ifdef __PSP__
// pspgl keeps one copy of every texture image (VRAM when it fits, otherwise
// the heap) and UE1's driver never lets go of a texture until the level
// changes. Unreal's own data already fills ~27MB of the 64MB console, so the
// intro flyby ran the heap dry after ~40 frames (845 uploads, ~15MB). Keep
// resident images under [PSP] TextureBudgetMB by dropping the least recently
// bound textures; anything bound this frame is never touched. pspgl defers the
// actual free until the GE has finished with the image.
void UNOpenGLRenderDevice::PspEvictTextures()
{
	guard(UNOpenGLRenderDevice::PspEvictTextures);
	if( GPspTexBudget < 0 )
	{
		INT MB = 8;
		GetConfigInt( "PSP", "TextureBudgetMB", MB );
		GPspTexBudget = MB * 1024 * 1024;
		debugf( NAME_Log, "PSPPERF: texture budget %i MB", MB );
	}
	if( GPspTexBytes <= GPspTexBudget )
		return;
	const INT Target = GPspTexBudget - GPspTexBudget / 4;
	INT Evicted = 0, Freed = 0;
	while( GPspTexBytes > Target )
	{
		INT   Best      = -1;
		DWORD BestFrame = GPspFrameCount;
		for( INT i = 0; i < BindMap.Size(); ++i )
			if( BindMap[i].LastFrame < BestFrame )
			{
				BestFrame = BindMap[i].LastFrame;
				Best      = i;
			}
		if( Best < 0 )
			break;   // everything left was bound this frame
		const QWORD Key  = BindMap.KeyAt( Best );
		FCachedTexture T = BindMap[Best];
		for( INT t = 0; t < MaxTexUnits; ++t )
			if( TexInfo[t].CurrentCacheID == Key )
				TexInfo[t].CurrentCacheID = 0;
		glDeleteTextures( 1, &T.Id );
		TexAlloc.RemoveItem( T.Id );
		BindMap.RemoveAt( Best );
		GPspTexBytes -= T.Bytes;
		Freed        += T.Bytes;
		++Evicted;
	}
	static INT Reports = 0;
	if( ++Reports <= 10 || ( Reports % 50 ) == 0 )
		debugf( NAME_Log, "PSPPERF: evicted %i textures (%i KB), %i KB resident, %i cached; %s",
			Evicted, Freed / 1024, GPspTexBytes / 1024, BindMap.Size(), PspHeapStr() );
	unguard;
}
#endif

UBOOL UNOpenGLRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	return false;
}

void UNOpenGLRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLRenderDevice::Lock);

	BindCycles = ImageCycles = ComplexCycles = GouraudCycles = TileCycles = 0;

#ifdef __PSP__
	// Cheap always-on frame timing: one log line per 100 frames is negligible,
	// and guessing at "feels smoother" has already cost us a wrong fix.
	++GPspFrameCount;
	{
		static DOUBLE LastReport = 0.0;
		const DOUBLE Now = appSeconds();
		if( LastReport == 0.0 )
			LastReport = Now;
		else if( ( GPspFrameCount % 100 ) == 0 )
		{
			const DOUBLE Elapsed = Now - LastReport;
			// Report uploads alongside the timing: if the slow intervals are
			// the upload-heavy ones, texture streaming is the stall, not the
			// rendering itself.
			// Split the interval: how much was the render device, how much
			// was engine CPU work we cannot see from in here.
			const DOUBLE Bind    = GSecondsPerCycle * (DOUBLE)GPspAccBind;
			const DOUBLE Image   = GSecondsPerCycle * (DOUBLE)GPspAccImage;
			const DOUBLE Complex = GSecondsPerCycle * (DOUBLE)GPspAccComplex;
			const DOUBLE Gouraud = GSecondsPerCycle * (DOUBLE)GPspAccGouraud;
			const DOUBLE Tile    = GSecondsPerCycle * (DOUBLE)GPspAccTile;
			const DOUBLE RendDev = Bind + Image + Complex + Gouraud + Tile;
			debugf( NAME_Log, "PSPPERF: 100 frames in %.2fs = %.1f fps (%.0f ms/frame) | uploads=%u (%u new)",
				(FLOAT)Elapsed, (FLOAT)( 100.0 / Max( Elapsed, (DOUBLE)0.001 ) ),
				(FLOAT)( Elapsed * 10.0 ),
				(unsigned)GPspUploadCount, (unsigned)( GPspUploadCount - GPspUploadLast ) );
			debugf( NAME_Log, "PSPPERF:   textures %i KB resident in %i cached, %i upload failures; %s", GPspTexBytes / 1024, BindMap.Size(), GPspUploadFailed, PspHeapStr() );
			debugf( NAME_Log, "PSPPERF:   uploads: %i first-time, %i realtime re-uploads, %i of them >=256x256, %i KB moved, hw palette %s",
				GPspUpFirst, GPspUpRealtime, GPspUpBig, GPspUpBytes / 1024, UseHwPalette ? "on" : "off" );
			GPspUpFirst = GPspUpRealtime = GPspUpBig = GPspUpBytes = 0;
			{
				extern CORE_API INT GPspFileRefills, GPspFileRefillBytes;
				static INT LastRefills = 0, LastBytes = 0;
				debugf( NAME_Log, "PSPPERF:   memory stick: %i window refills, %i KB read", GPspFileRefills - LastRefills, ( GPspFileRefillBytes - LastBytes ) / 1024 );
				LastRefills = GPspFileRefills; LastBytes = GPspFileRefillBytes;
			}
			debugf( NAME_Log, "PSPPERF:   draws: %i mesh polys in %i batches, %i facet passes", GPspBatchPolys, GPspBatchDraws, GPspFacetDraws );
			GPspBatchPolys = GPspBatchDraws = GPspFacetDraws = 0;
			{
				// Once per run, when the heap gets tight: UE1's own per-class
				// memory table, so an out-of-memory has a suspect list next to it.
				static UBOOL ObjListDumped = 0;
				struct mallinfo M = mallinfo();
				if( !ObjListDumped && M.fordblks < 1024 * 1024 && sceKernelTotalFreeMemSize() < 2 * 1024 * 1024 )
				{
					ObjListDumped = 1;
					debugf( NAME_Log, "PSPPERF: heap tight (%i KB free) -- object memory by class:", M.fordblks / 1024 );
					GObj.Exec( "OBJ LIST", GSystem );
				}
			}
			debugf( NAME_Log, "PSPPERF:   renddev %.1f%% | bind %.0fms image %.0fms complex %.0fms gouraud %.0fms tile %.0fms | ENGINE %.0fms (%.1f%%)",
				(FLOAT)( 100.0 * RendDev / Max( Elapsed, (DOUBLE)0.001 ) ),
				(FLOAT)(Bind*1000), (FLOAT)(Image*1000), (FLOAT)(Complex*1000),
				(FLOAT)(Gouraud*1000), (FLOAT)(Tile*1000),
				(FLOAT)( ( Elapsed - RendDev ) * 1000 ),
				(FLOAT)( 100.0 * ( Elapsed - RendDev ) / Max( Elapsed, (DOUBLE)0.001 ) ) );
			debugf( NAME_Log, "PSPPERF:   engine: illum %.0fms occlusion %.0fms mesh %.0fms polyv %.0fms",
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccIllum),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccOcclusion),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccMesh),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccPolyV) );
			debugf( NAME_Log, "PSPPERF:   occlusion: clip %.0fms raster %.0fms span %.0fms (rest is traversal/bounds); mesh getframe %.0fms",
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccClip),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccRaster),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccSpan),
				(FLOAT)(GSecondsPerCycle*1000*(DOUBLE)GPspAccMeshFrame) );
			GPspAccClip = GPspAccRaster = GPspAccSpan = GPspAccMeshFrame = 0;
			GPspAccBind = GPspAccImage = GPspAccComplex = GPspAccGouraud = GPspAccTile = 0;
			GPspAccIllum = GPspAccOcclusion = GPspAccMesh = GPspAccPolyV = 0;
			GPspUploadLast = GPspUploadCount;
			LastReport = Now;
		}
	}
#endif

	glClearColor( ScreenClear.X, ScreenClear.Y, ScreenClear.Z, ScreenClear.W );
	glClearDepth( 1.0 );
	glDepthFunc( GL_LEQUAL );

	if( UseWindowBrightness )
	{
		FLOAT TargetBrightness = CurrentBrightness;
		if ( Viewport && Viewport->Client )
			TargetBrightness = Viewport->Client->Brightness;
		else if ( CurrentBrightness < 0.f )
			TargetBrightness = 0.5f;
		if ( CurrentBrightness != TargetBrightness )
		{
			CurrentBrightness = TargetBrightness;
			const FLOAT Gamma = 0.5 + 1.5 * CurrentBrightness;
			SDL_Window* Window = (SDL_Window*)Viewport->GetWindow();
			SDL_SetWindowBrightness( Window, Gamma );
		}
	}

	SetBlend( PF_Occlude );

	GLbitfield ClearBits = GL_DEPTH_BUFFER_BIT;
	if( RenderLockFlags & LOCKR_ClearScreen )
		ClearBits |= GL_COLOR_BUFFER_BIT;
	glClear( ClearBits );

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	if( AutoFOV && Viewport && Viewport->Actor && Viewport->Actor->DesiredFOV == 90.0f )
	{
		const FLOAT Aspect = (FLOAT)Viewport->SizeX / (FLOAT)Viewport->SizeY;
		const FLOAT Fov = (FLOAT)( appAtan( appTan( 90.0 * PI / 360.0 ) * ( Aspect / ( 4.0 / 3.0 ) ) ) * 360.0 ) / PI;
		Viewport->Actor->DesiredFOV = Fov;
	}

	unguard;
}

void UNOpenGLRenderDevice::Unlock( UBOOL Blit )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLRenderDevice::Unlock);

	glFlush();

#ifdef __PSP__
	// Accumulate the render device's own cycle counters before Lock() resets
	// them next frame. These cover only the render device; whatever is left
	// over versus wall-clock frame time is engine CPU work (lighting,
	// occlusion, UnrealScript).
	GPspAccBind    += (INT)BindCycles;
	GPspAccImage   += (INT)ImageCycles;
	GPspAccComplex += (INT)ComplexCycles;
	GPspAccGouraud += (INT)GouraudCycles;
	GPspAccTile    += (INT)TileCycles;
	GPspAccIllum     += (INT)GStat.IllumTime;
	GPspAccOcclusion += (INT)GStat.OcclusionTime;
	GPspAccMesh      += (INT)GStat.MeshTime;
	GPspAccPolyV     += (INT)GStat.PolyVTime;
	GPspAccClip      += (INT)GStat.ClipTime;
	GPspAccRaster    += (INT)GStat.RasterTime;
	GPspAccSpan      += (INT)GStat.SpanTime;
	GPspAccMeshFrame += (INT)GStat.MeshGetFrameTime;
#endif

	unguard;
}

void UNOpenGLRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UNOpenGLRenderDevice::DrawComplexSurface);

	check(Surface.Texture);

	SetSceneNode( Frame );

	uclock(ComplexCycles);

	if( UseMultiTexture )
	{
		// Draw with multitexture.
		DrawComplexSurfaceMultiTex( Frame, Surface, Facet );
	}
	else
	{
		// Draw with single texture unit.
		DrawComplexSurfaceSingleTex(Frame, Surface, Facet);
	}

	uunclock(ComplexCycles);

	unguard;
}

void UNOpenGLRenderDevice::DrawComplexSurfaceMultiTex( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	const FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	const FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;

	SetBlend( Surface.PolyFlags );
	SetTexture( 0, *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.0 );

	if( Surface.LightMap )
	{
		SetTexture( 1, *Surface.LightMap, 0, -0.5f );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
		glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
		glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
	}

	if( Surface.DetailTexture && DetailTextures )
	{
		SetTexture( 2, *Surface.DetailTexture, 0, 0.f );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
		glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
		glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
	}

	if( Surface.FogMap )
	{
		SetTexture( 3, *Surface.FogMap, 0, -0.5f );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
		glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
	}

	glColor4f( 1.f, 1.f, 1.f, 1.f );
	for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
	{
		glBegin( GL_TRIANGLE_FAN );
		for( INT i=0; i<Poly->NumPts; i++ )
		{
			const FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
			const FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
			for( INT t=0; t<MaxTexUnits; ++t)
			{
				if( TexInfo[t].CurrentCacheID != 0 )
				{
					glMultiTexCoord2f( GL_TEXTURE0+t, (U-UDot-TexInfo[t].UPan)*TexInfo[t].UMult, (V-VDot-TexInfo[t].VPan)*TexInfo[t].VMult );
				}
			}
			glVertex3fv( &Poly->Pts[i]->Point.X );
		}
		glEnd();
	}

	for( INT t=1; t<MaxTexUnits; ++t)
	{
		ResetTexture( t );
	}
}


#ifdef __PSP__
//
// PSP fast path for complex (BSP) surfaces.
//
// Profiling showed DrawComplexSurface at 58-60% of frame time on the slow
// stretches, dwarfing texture uploads (3%). Two costs stand out:
//
//  1. Every vertex went through glBegin/glTexCoord2f/glVertex3f -- two calls
//     per vertex through pspgl's immediate-mode buffering. glInterleavedArrays
//     (GL_T2F_V3F) + glDrawArrays maps directly onto the GU's native vertex
//     format instead, which is pspgl's fast path.
//  2. Without multitexture each surface is drawn up to four times (base,
//     lightmap, detail, fog) and every pass recomputed the same two
//     texture-space dot products per vertex. They only need computing once.
//
static FLOAT* GPspRawUV   = NULL;   // raw u,v per vertex, shared by all passes
static INT    GPspVtxMax  = 0;      // capacity in vertices

static UBOOL PspEnsureVtxBuffers( INT Pts )
{
	if( Pts > GPspVtxMax )
	{
		// GPspRawUV is CPU-only scratch: the GE never sees it, so realloc is fine.
		FLOAT* NewRaw = (FLOAT*)realloc( GPspRawUV, Pts * 2 * sizeof(FLOAT) );
		if( !NewRaw ) return 0;
		GPspRawUV = NewRaw;
		GPspVtxMax = Pts;
	}
	return 1;
}

//
// pspgl's glInterleavedArrays does not reliably disable the client arrays that
// a format leaves out: after a T2F_C4UB_V3F draw, GL_COLOR_ARRAY can stay
// enabled holding a stale pointer, and the next draw then asks the kernel to
// flush that array's cache range. On hardware that arrives as
// sceKernelDcacheWritebackInvalidateRange(NULL, 0) and the kernel answers with
// a syscall exception; PPSSPP ignores the bad range, so it only ever fails on
// the console. Set (and unset) every array explicitly.
//

//
// pspgl faults on a 3-vertex GL_TRIANGLE_FAN on real hardware. Traced on a
// PSP-2000: every fan with 4 or 5 vertices draws fine, and the very first
// 3-vertex fan takes the console down inside glDrawArrays. PPSSPP draws it
// happily, which is why it survived every emulator run.
//
// A 3-vertex fan is exactly one triangle, so GL_TRIANGLES is an identical
// primitive without the bug.
//
static inline void PspDrawFan( INT N )
{
	glDrawArrays( N == 3 ? GL_TRIANGLES : GL_TRIANGLE_FAN, 0, N );
}


static UBOOL PspSetArrays( UBOOL bTex, UBOOL bColor, const void* Base, INT Stride )
{
	const BYTE* P = (const BYTE*)Base;
	INT Off = 0;

	// pspgl's __pspgl_cache_arrays walks every *enabled* array and flushes its
	// cache range, so one stale array with a null pointer takes the process
	// down. Leave nothing enabled that we are not about to fill, and refuse to
	// draw at all if our own buffer never got allocated.
	glDisableClientState( GL_NORMAL_ARRAY );
	if( !Base )
	{
		glDisableClientState( GL_VERTEX_ARRAY );
		glDisableClientState( GL_TEXTURE_COORD_ARRAY );
		glDisableClientState( GL_COLOR_ARRAY );
		return 0;
	}

	if( bTex )
	{
		glEnableClientState( GL_TEXTURE_COORD_ARRAY );
		glTexCoordPointer( 2, GL_FLOAT, Stride, P );
		Off += 2 * sizeof(FLOAT);
	}
	else glDisableClientState( GL_TEXTURE_COORD_ARRAY );

	if( bColor )
	{
		glEnableClientState( GL_COLOR_ARRAY );
		glColorPointer( 4, GL_UNSIGNED_BYTE, Stride, P + Off );
		Off += 4;
	}
	else glDisableClientState( GL_COLOR_ARRAY );

	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, Stride, P + Off );
	return 1;
}

// Every glDrawArrays costs pspgl a malloc, a vertex copy and a D-cache flush
// syscall -- ~67us on hardware, and the intro issued 210 draws per frame, 55%
// of them single mesh triangles from DrawGouraudPolygon. So: (1) mesh polys
// with the same texture, flags and frame are appended to one triangle list
// and drawn together (flushed by every other entry point), and (2) each BSP
// facet pass is one triangle list instead of one fan per poly. pspgl copies
// the vertices at draw time, so these buffers need not outlive the call.
enum { PSP_BATCH_MAX_VERTS = 3072, PSP_POLY_MAX = 128 };
static BYTE  GPspBatchBuf[PSP_BATCH_MAX_VERTS * 24] __attribute__((aligned(64)));
static INT   GPspBatchVerts = 0;
static UBOOL GPspBatchOpen  = 0;
static QWORD GPspBatchTex   = 0;
static DWORD GPspBatchFlags = 0;
static const FSceneNode* GPspBatchFrame = NULL;

static void PspFlushBatch()
{
	if( GPspBatchVerts > 0 && PspSetArrays( 1, 1, GPspBatchBuf, 24 ) )
	{
		glDrawArrays( GL_TRIANGLES, 0, GPspBatchVerts );
		++GPspBatchDraws;
	}
	GPspBatchVerts = 0;
	GPspBatchOpen  = 0;
}

// Room for N vertices in the batch, flushing first if it is full (the GL
// state is the batch's, so it stays open). NULL only if N cannot ever fit.
static inline BYTE* PspBatchReserve( INT N )
{
	if( N > PSP_BATCH_MAX_VERTS )
		return NULL;
	if( GPspBatchVerts + N > PSP_BATCH_MAX_VERTS )
	{
		PspFlushBatch();
		GPspBatchOpen = 1;
	}
	BYTE* P = GPspBatchBuf + GPspBatchVerts * 24;
	GPspBatchVerts += N;
	return P;
}

// Fan -> triangle list copy: vertices 0,i,i+1 for i in 1..N-2, Stride bytes each.
static inline BYTE* PspFanToTris( BYTE* Out, const BYTE* Fan, INT N, INT Stride )
{
	for( INT i = 1; i < N - 1; ++i )
	{
		appMemcpy( Out,            Fan,                  Stride );
		appMemcpy( Out + Stride,   Fan + i * Stride,     Stride );
		appMemcpy( Out + 2*Stride, Fan + (i+1) * Stride, Stride );
		Out += 3 * Stride;
	}
	return Out;
}

// Emit every polygon of the facet for one pass, reusing the cached raw U/V.
static void PspEmitFacet( FSurfaceFacet& Facet, FLOAT UDot, FLOAT VDot,
                          FLOAT UPan, FLOAT VPan, FLOAT UMult, FLOAT VMult )
{
	INT Tris = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		if( Poly->NumPts >= 3 && Poly->NumPts <= PSP_POLY_MAX )
			Tris += Poly->NumPts - 2;
	if( !Tris )
		return;
	BYTE* Buf = (BYTE*)PspClaimVtx( Tris * 3 * 20 );
	if( !Buf )
		return;
	static BYTE Fan[PSP_POLY_MAX * 20];
	BYTE* Out  = Buf;
	INT   Base = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT N = Poly->NumPts;
		if( N < 3 || N > PSP_POLY_MAX )
		{
			Base += N;
			continue;
		}
		FLOAT* F = (FLOAT*)Fan;
		for( INT i = 0; i < N; ++i )
		{
			const FLOAT* Raw = &GPspRawUV[ ( Base + i ) * 2 ];
			*F++ = ( Raw[0] - UDot - UPan ) * UMult;
			*F++ = ( Raw[1] - VDot - VPan ) * VMult;
			const FVector& P = Poly->Pts[i]->Point;
			*F++ = P.X;
			*F++ = P.Y;
			*F++ = P.Z;
		}
		Out = PspFanToTris( Out, Fan, N, 20 );
		Base += N;
	}
	if( PspSetArrays( 1, 0, Buf, 20 ) )
	{
		glDrawArrays( GL_TRIANGLES, 0, Tris * 3 );
		++GPspFacetDraws;
	}
}

//
// Single-pass lit emitter.
//
// The PSP GE has exactly one texture unit (sceGuTexFunc takes no stage), so a
// lit surface normally costs two full geometry passes: base texture, then the
// lightmap modulated over it. A bisect showed that second pass is what keeps
// the intro off its 20fps cap.
//
// But GU_TFX_MODULATE computes Cv = Ct * Cf -- texture times the diffuse
// fragment colour -- for free. So sample the lightmap once per *vertex* and
// feed it as vertex colour, and the hardware does base*light in one pass.
//
// Tradeoff: lighting is interpolated across the polygon instead of per texel,
// so gradients on large surfaces are coarser. Controlled by [PSP] VertexLight.
//
// Lightmaps are BGRA7777: 4 bytes/texel, each 0-127.
//
// Brightness: the two-pass path blends the lightmap with
// glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR), i.e. src*dst + dst*src = 2*base*light
// -- a built-in x2 overbright. Plain GU_TFX_MODULATE (Ct*Cf) has no such
// doubling, so folding the lightmap into vertex colour naively comes out
// exactly half as bright.
//
// To match, a raw texel i (0-127) must become vertex colour 4*i, saturating at
// 255. [PSP] LightScale scales that in percent -- the PSP screen is dim, so
// leaning above 100 is reasonable. A 128-entry LUT keeps it to one lookup per
// channel per vertex (no multiply or divide in the inner loop).
// Base-texture gamma.
//
// Raising LightScale alone eventually saturates the lightmap LUT at 255, which
// flattens already-bright surfaces while dark ones keep lifting. Gamma on the
// base texture instead lifts darks and midtones while approaching 255
// asymptotically, so highlights keep their detail.
//
// Applied to the PALETTE (256 entries) at upload time rather than per texel,
// and only once per texture, so it costs nothing at runtime.
//   [PSP] Gamma=130   ; percent; 100 = off, higher = brighter
static BYTE  GPspGammaLUT[256];
static UBOOL GPspGammaActive = 0;
static INT   GPspGammaInit = 0;

static void PspInitGamma()
{
	if( GPspGammaInit ) return;
	GPspGammaInit = 1;
	INT GammaPercent = 130;
	GetConfigInt( "PSP", "Gamma", GammaPercent );
	GammaPercent = Clamp( GammaPercent, 50, 400 );
	GPspGammaActive = ( GammaPercent != 100 );
	const FLOAT Exponent = 100.f / (FLOAT)GammaPercent;
	for( INT i = 0; i < 256; ++i )
	{
		const FLOAT N = (FLOAT)i / 255.f;
		GPspGammaLUT[i] = (BYTE)Clamp( appRound( 255.f * appPow( N, Exponent ) ), 0, 255 );
	}
	debugf( NAME_Log, "PSPPERF: base texture gamma = %d%%", GammaPercent );
}

static BYTE GPspLightLUT[128];
static INT  GPspLightLUTScale = -1;

static void PspBuildLightLUT( INT ScalePercent )
{
	for( INT i = 0; i < 128; ++i )
		GPspLightLUT[i] = (BYTE)Clamp( ( i * 4 * ScalePercent ) / 100, 0, 255 );
	GPspLightLUTScale = ScalePercent;
}

static void PspEmitFacetLit( FSurfaceFacet& Facet, FLOAT UDot, FLOAT VDot,
                             FLOAT UPan, FLOAT VPan, FLOAT UMult, FLOAT VMult,
                             const FTextureInfo& LM )
{
	const BYTE* LMData = (const BYTE*)LM.Mips[0]->DataPtr;
	const INT   LMU    = LM.Mips[0]->USize;
	const INT   LMV    = LM.Mips[0]->VSize;
	const FLOAT LMUPan = LM.Pan.X - 0.5f * LM.UScale;
	const FLOAT LMVPan = LM.Pan.Y - 0.5f * LM.VScale;
	const FLOAT InvUScale = 1.f / LM.UScale;
	const FLOAT InvVScale = 1.f / LM.VScale;

	INT Tris = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		if( Poly->NumPts >= 3 && Poly->NumPts <= PSP_POLY_MAX )
			Tris += Poly->NumPts - 2;
	if( !Tris )
		return;
	BYTE* Buf = (BYTE*)PspClaimVtx( Tris * 3 * 24 );
	if( !Buf )
		return;
	static BYTE Fan[PSP_POLY_MAX * 24];
	BYTE* Out  = Buf;
	INT   Base = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT N = Poly->NumPts;
		if( N < 3 || N > PSP_POLY_MAX )
		{
			Base += N;
			continue;
		}
		BYTE* V = Fan;
		for( INT i = 0; i < N; ++i )
		{
			const FLOAT* Raw = &GPspRawUV[ ( Base + i ) * 2 ];

			FLOAT* T = (FLOAT*)V;
			T[0] = ( Raw[0] - UDot - UPan ) * UMult;
			T[1] = ( Raw[1] - VDot - VPan ) * VMult;

			INT iu = appFloor( ( Raw[0] - UDot - LMUPan ) * InvUScale );
			INT iv = appFloor( ( Raw[1] - VDot - LMVPan ) * InvVScale );
			iu = Clamp( iu, 0, LMU - 1 );
			iv = Clamp( iv, 0, LMV - 1 );
			const BYTE* Texel = &LMData[ ( iv * LMU + iu ) * 4 ];

			BYTE* C = V + 8;
			C[0] = GPspLightLUT[ Texel[2] & 0x7F ];   // BGRA source -> R
			C[1] = GPspLightLUT[ Texel[1] & 0x7F ];   // G
			C[2] = GPspLightLUT[ Texel[0] & 0x7F ];   // B
			C[3] = 255;

			FLOAT* P3 = (FLOAT*)( V + 12 );
			const FVector& P = Poly->Pts[i]->Point;
			P3[0] = P.X; P3[1] = P.Y; P3[2] = P.Z;
			V += 24;
		}
		Out = PspFanToTris( Out, Fan, N, 24 );
		Base += N;
	}
	if( PspSetArrays( 1, 1, Buf, 24 ) )
	{
		glDrawArrays( GL_TRIANGLES, 0, Tris * 3 );
		++GPspFacetDraws;
	}
}

// Base pass into the shared triangle batch (see PspFlushBatch): consecutive
// surfaces with the same texture and flags -- which is how the engine hands
// them over, sorted by texture -- become one draw instead of one per facet.
// LM == NULL draws unlit (white vertex colour, modulate leaves the texture).
static void PspBatchFacet( FSurfaceFacet& Facet, FLOAT UDot, FLOAT VDot,
                           FLOAT UPan, FLOAT VPan, FLOAT UMult, FLOAT VMult,
                           const FTextureInfo* LM )
{
	const BYTE* LMData = NULL; INT LMU = 1, LMV = 1; FLOAT LMUPan = 0, LMVPan = 0, InvUScale = 0, InvVScale = 0;
	if( LM )
	{
		LMData = (const BYTE*)LM->Mips[0]->DataPtr; LMU = LM->Mips[0]->USize; LMV = LM->Mips[0]->VSize;
		LMUPan = LM->Pan.X - 0.5f * LM->UScale; LMVPan = LM->Pan.Y - 0.5f * LM->VScale;
		InvUScale = 1.f / LM->UScale; InvVScale = 1.f / LM->VScale;
	}
	static BYTE Fan[PSP_POLY_MAX * 24];
	INT Base = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT N = Poly->NumPts;
		if( N < 3 || N > PSP_POLY_MAX )
		{
			Base += N;
			continue;
		}
		BYTE* V = Fan;
		for( INT i = 0; i < N; ++i )
		{
			const FLOAT* Raw = &GPspRawUV[ ( Base + i ) * 2 ];
			FLOAT* T = (FLOAT*)V;
			T[0] = ( Raw[0] - UDot - UPan ) * UMult;
			T[1] = ( Raw[1] - VDot - VPan ) * VMult;
			BYTE* C = V + 8;
			if( LMData )
			{
				INT iu = appFloor( ( Raw[0] - UDot - LMUPan ) * InvUScale );
				INT iv = appFloor( ( Raw[1] - VDot - LMVPan ) * InvVScale );
				iu = Clamp( iu, 0, LMU - 1 );
				iv = Clamp( iv, 0, LMV - 1 );
				const BYTE* Texel = &LMData[ ( iv * LMU + iu ) * 4 ];
				C[0] = GPspLightLUT[ Texel[2] & 0x7F ];
				C[1] = GPspLightLUT[ Texel[1] & 0x7F ];
				C[2] = GPspLightLUT[ Texel[0] & 0x7F ];
			}
			else
				C[0] = C[1] = C[2] = 255;
			C[3] = 255;
			FLOAT* P3 = (FLOAT*)( V + 12 );
			const FVector& P = Poly->Pts[i]->Point;
			P3[0] = P.X; P3[1] = P.Y; P3[2] = P.Z;
			V += 24;
		}
		BYTE* Out = PspBatchReserve( 3 * ( N - 2 ) );
		if( Out )
			PspFanToTris( Out, Fan, N, 24 );
		Base += N;
	}
	++GPspFacetDraws;   // counts facets submitted, no longer draws
}

#endif

void UNOpenGLRenderDevice::DrawComplexSurfaceSingleTex( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	const FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	const FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;

#ifdef __PSP__
	{
		// BISECT: [PSP] VertexArrays=0 falls back to the stock immediate-mode
		// renderer (glBegin/glVertex3f). Slower, but it isolates whether
		// pspgl's client-array path is what dies on hardware -- every array
		// draw goes through glDrawArrays, and both emitters fail at the same
		// surface regardless of which one is used.
		static INT PspVertexArrays = -1;
		if( PspVertexArrays < 0 )
		{
			PspVertexArrays = 1;
			GetConfigInt( "PSP", "VertexArrays", PspVertexArrays );
			debugf( NAME_Log, "PSPPERF: vertex arrays = %s", PspVertexArrays ? "on" : "off" );
		}

		INT TotalPts = 0;
		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
			TotalPts += Poly->NumPts;
		if( !PspVertexArrays )
			TotalPts = 0;   // fall through to the immediate-mode path below

		if( TotalPts > 0 && PspEnsureVtxBuffers( TotalPts ) )
		{
			// Cache the texture-space dot products once for the whole facet.
			INT Base = 0;
			for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
			{
				for( INT i = 0; i < Poly->NumPts; ++i )
				{
					const FVector& P = Poly->Pts[i]->Point;
					GPspRawUV[ ( Base + i ) * 2 + 0 ] = Facet.MapCoords.XAxis | P;
					GPspRawUV[ ( Base + i ) * 2 + 1 ] = Facet.MapCoords.YAxis | P;
				}
				Base += Poly->NumPts;
			}

			// Can we fold the lightmap into vertex colour and save a whole
			// geometry pass? Needs an unpalettised lightmap with readable mip
			// data (lightmaps are BGRA7777, so Palette is NULL).
			static INT PspVertexLight = -1;
			if( PspVertexLight < 0 )
			{
				PspVertexLight = 0;
				GetConfigInt( "PSP", "VertexLight", PspVertexLight );
				INT Scale = 125;   // lean bright: the PSP panel is dim
				GetConfigInt( "PSP", "LightScale", Scale );
				PspBuildLightLUT( Clamp( Scale, 10, 400 ) );
				debugf( NAME_Log, "PSPPERF: per-vertex lighting = %s, LightScale = %d%%",
					PspVertexLight ? "on" : "off", Scale );
			}
			const UBOOL bVertexLit =
				PspVertexLight && Surface.LightMap && !Surface.LightMap->Palette &&
				Surface.LightMap->Mips[0] && Surface.LightMap->Mips[0]->DataPtr;

			static INT PspLightMaps = -1;
			if( PspLightMaps < 0 )
			{
				PspLightMaps = 1;
				GetConfigInt( "PSP", "LightMaps", PspLightMaps );
				debugf( NAME_Log, "PSPPERF: lightmap pass = %s", PspLightMaps ? "on" : "off" );
			}
			// Base texture, into the shared batch (same key rules as the mesh
			// path: texture, flags, frame; a realtime texture change reopens).
			{
				const UBOOL Realtime = ( Surface.Texture->TextureFlags & TF_RealtimeChanged );
				if( GPspBatchOpen && ( Surface.Texture->CacheID != GPspBatchTex || Surface.PolyFlags != GPspBatchFlags || Frame != GPspBatchFrame || Realtime ) )
					PspFlushBatch();
				if( !GPspBatchOpen )
				{
					SetBlend( Surface.PolyFlags );
					SetTexture( 0, *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
					ResetTexture( 1 ); ResetTexture( 2 ); ResetTexture( 3 );
					GPspBatchOpen  = 1;
					GPspBatchTex   = Surface.Texture->CacheID;
					GPspBatchFlags = Surface.PolyFlags;
					GPspBatchFrame = Frame;
				}
				// Pan and scale from THIS surface's texture info: SetTexture is
				// skipped while the batch stays open, so TexInfo[0] would carry the
				// previous surface's pan (textures visibly sliding/zooming).
				const FTextureInfo& BT = *Surface.Texture;
				const FLOAT BUMult = 1.f / ( BT.UScale * (FLOAT)Max( 8, BT.USize ) );
				const FLOAT BVMult = 1.f / ( BT.VScale * (FLOAT)Max( 8, BT.VSize ) );
				PspBatchFacet( Facet, UDot, VDot, BT.Pan.X, BT.Pan.Y, BUMult, BVMult,
					bVertexLit ? Surface.LightMap : NULL );
			}
			// Any extra pass below draws over this surface with GL_EQUAL, so the
			// batch (which now holds this surface) must be on the GE first.
			const UBOOL bExtraPass = ( Surface.LightMap && PspLightMaps && !bVertexLit ) || ( Surface.DetailTexture && DetailTextures ) || Surface.FogMap;
			if( bExtraPass )
				PspFlushBatch();

			// Lightmap.
			//
			// BISECT/tunable: lightmaps are the one thing torches actually make
			// expensive -- dynamic lights mark them TF_RealtimeChanged every
			// frame, so each one is both re-uploaded AND costs a second full
			// geometry pass over the surface. Set [PSP] LightMaps=0 in
			// Unreal.ini to drop the pass and measure what it is worth.
			if( Surface.LightMap && PspLightMaps && !bVertexLit )
			{
				SetBlend( PF_Modulated );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_EQUAL );
				SetTexture( 0, *Surface.LightMap, 0, -0.5 );
				glColor4f( 1.f, 1.f, 1.f, 1.f );
				PspEmitFacet( Facet, UDot, VDot, TexInfo[0].UPan, TexInfo[0].VPan, TexInfo[0].UMult, TexInfo[0].VMult );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_LEQUAL );
			}

			// Detail texture.
			if( Surface.DetailTexture && DetailTextures )
			{
				SetBlend( PF_Modulated );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_EQUAL );
				SetTexture( 0, *Surface.DetailTexture, 0, 0.f );
				PspEmitFacet( Facet, UDot, VDot, TexInfo[0].UPan, TexInfo[0].VPan, TexInfo[0].UMult, TexInfo[0].VMult );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_LEQUAL );
			}

			// Fog.
			if( Surface.FogMap )
			{
				SetBlend( PF_Highlighted );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_EQUAL );
				SetTexture( 0, *Surface.FogMap, 0, -0.5 );
				PspEmitFacet( Facet, UDot, VDot, TexInfo[0].UPan, TexInfo[0].VPan, TexInfo[0].UMult, TexInfo[0].VMult );
				if( Surface.PolyFlags & PF_Masked )
					glDepthFunc( GL_LEQUAL );
			}

			glDisableClientState( GL_VERTEX_ARRAY );
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisableClientState( GL_COLOR_ARRAY );
			return;
		}
		// Allocation failed -- fall through to the portable immediate-mode path.
	}
#endif

	// Draw texture.
	SetBlend( Surface.PolyFlags );
	SetTexture( 0, *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
	glColor4f( 1.f, 1.f, 1.f, 1.f );
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		glBegin( GL_TRIANGLE_FAN );
		for( INT i = 0; i < Poly->NumPts; i++ )
		{
			const FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
			const FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
			glTexCoord2f( (U-UDot-TexInfo[0].UPan)*TexInfo[0].UMult, (V-VDot-TexInfo[0].VPan)*TexInfo[0].VMult );
			glVertex3f( Poly->Pts[i]->Point.X, Poly->Pts[i]->Point.Y, Poly->Pts[i]->Point.Z );
		}
		glEnd();
	}

	// Draw lightmap.
	if( Surface.LightMap )
	{
		SetBlend( PF_Modulated );
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_EQUAL );
		SetTexture( 0, *Surface.LightMap, 0, -0.5 );
		glColor4f( 1.f, 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i = 0; i < Poly->NumPts; i++ )
			{
				const FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
				const FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
				glTexCoord2f( (U-UDot-TexInfo[0].UPan)*TexInfo[0].UMult, (V-VDot-TexInfo[0].VPan)*TexInfo[0].VMult );
				glVertex3f( Poly->Pts[i]->Point.X, Poly->Pts[i]->Point.Y, Poly->Pts[i]->Point.Z );
			}
			glEnd();
		}
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_LEQUAL );
	}

	// Draw detail texture overlaid.
	if( Surface.DetailTexture && DetailTextures )
	{
		SetBlend( PF_Modulated );
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_EQUAL );
		SetTexture( 0, *Surface.DetailTexture, 0, 0.f );

		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i = 0; i < Poly->NumPts; i++ )
			{
				const FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
				const FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
				glTexCoord2f( (U-UDot-TexInfo[0].UPan)*TexInfo[0].UMult, (V-VDot-TexInfo[0].VPan)*TexInfo[0].VMult );
				glVertex3f( Poly->Pts[i]->Point.X, Poly->Pts[i]->Point.Y, Poly->Pts[i]->Point.Z );
			}
			glEnd();
		}
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_LEQUAL );
	}


	// Draw fog.
	if( Surface.FogMap )
	{
		SetBlend( PF_Highlighted );
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_EQUAL );
		SetTexture( 0, *Surface.FogMap, 0, -0.5 );
		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i = 0; i < Poly->NumPts; i++ )
			{
				const FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
				const FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
				glTexCoord2f( (U-UDot-TexInfo[0].UPan)*TexInfo[0].UMult, (V-VDot-TexInfo[0].VPan)*TexInfo[0].VMult );
				glVertex3f( Poly->Pts[i]->Point.X, Poly->Pts[i]->Point.Y, Poly->Pts[i]->Point.Z );
			}
			glEnd();
		}
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_LEQUAL );
	}
}


#ifdef __PSP__
//
// Gouraud (actor mesh) fast path.
//
// GL_T2F_C4UB_V3F is exactly the GU's native vertex layout
// (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF), so pspgl can hand it
// straight to the hardware. The original path cost up to three calls per
// vertex (glColor4f + glTexCoord2f + glVertex3f).
//
// Stride 24: 2 floats uv, 4 bytes rgba, 3 floats xyz.
// Fog pass uses GL_C4UB_V3F, stride 16.
//
static BYTE* GPspMeshBuf = NULL;
static INT   GPspMeshMax = 0;

static UBOOL PspEnsureMeshBuffer( INT Pts )
{
	// Refusing degenerate counts here guards both mesh draw paths: a zero-length
	// vertex array makes pspgl flush a zero-byte cache range, which the kernel
	// rejects with a syscall exception. Callers fall back to the immediate-mode
	// path, which handles an empty primitive harmlessly.
	if( Pts < 3 )
		return 0;
	// Storage now comes from the GE-safe ring, claimed per draw; see
	// PspClaimVtx. Reusing one realloc'd buffer let the GE read vertices we
	// had already overwritten.
	GPspMeshBuf = (BYTE*)PspClaimVtx( Pts * 24 );
	GPspMeshMax = Pts;
	return GPspMeshBuf != NULL;
}

static inline BYTE PspToByte( FLOAT V )
{
	const INT I = appRound( V * 255.f );
	return (BYTE)Clamp( I, 0, 255 );
}
#endif

void UNOpenGLRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer )
{
		guard(UNOpenGLRenderDevice::DrawGouraudPolygon);

#ifdef __PSP__
		// Batch path (see PspFlushBatch). Polys needing the separate fog pass
		// keep the stock two-draw path below.
		const UBOOL bFogPass = ( (PolyFlags & (PF_RenderFog|PF_Translucent|PF_Modulated)) == PF_RenderFog );
		if( !bFogPass && NumPts >= 3 && NumPts <= PSP_POLY_MAX )
		{
			uclock(GouraudCycles);
			const UBOOL Modulated = ( PolyFlags & PF_Modulated );
			const UBOOL Realtime  = ( Texture.TextureFlags & TF_RealtimeChanged );
			if( GPspBatchOpen && ( Texture.CacheID != GPspBatchTex || PolyFlags != GPspBatchFlags || Frame != GPspBatchFrame || Realtime ) )
				PspFlushBatch();
			if( !GPspBatchOpen )
			{
				SetSceneNode( Frame );
				SetBlend( PolyFlags );
				SetTexture( 0, Texture, ( PolyFlags & PF_Masked ), 0 );
				ResetTexture( 1 );
				ResetTexture( 2 );
				ResetTexture( 3 );
				GPspBatchOpen  = 1;
				GPspBatchTex   = Texture.CacheID;
				GPspBatchFlags = PolyFlags;
				GPspBatchFrame = Frame;
			}
			const INT Need = 3 * ( NumPts - 2 );
			if( GPspBatchVerts + Need > PSP_BATCH_MAX_VERTS )
			{
				PspFlushBatch();
				GPspBatchOpen = 1;   // GL state is still the batch's
			}
			static BYTE Fan[PSP_POLY_MAX * 24];
			BYTE* V = Fan;
			for( INT i = 0; i < NumPts; ++i )
			{
				FTransTexture* P = Pts[i];
				FLOAT* T = (FLOAT*)V;
				T[0] = P->U * TexInfo[0].UMult;
				T[1] = P->V * TexInfo[0].VMult;
				BYTE* C = V + 8;
				if( Modulated )
					C[0] = C[1] = C[2] = 255;
				else
				{
					C[0] = PspToByte( P->Light.X );
					C[1] = PspToByte( P->Light.Y );
					C[2] = PspToByte( P->Light.Z );
				}
				C[3] = 255;
				FLOAT* P3 = (FLOAT*)( V + 12 );
				P3[0] = P->Point.X; P3[1] = P->Point.Y; P3[2] = P->Point.Z;
				V += 24;
			}
			PspFanToTris( GPspBatchBuf + GPspBatchVerts * 24, Fan, NumPts, 24 );
			GPspBatchVerts += Need;
			++GPspBatchPolys;
			uunclock(GouraudCycles);
			return;
		}
		PspFlushBatch();
#endif

		SetSceneNode( Frame );
		uclock(GouraudCycles);
		SetBlend( PolyFlags );
		SetTexture( 0, Texture, ( PolyFlags & PF_Masked ), 0 );
		ResetTexture( 1 );
		ResetTexture( 2 );
		ResetTexture( 3 );

		const UBOOL IsModulated = ( PolyFlags & PF_Modulated );

		if( IsModulated )
			glColor4f( 1.f, 1.f, 1.f, 1.f );

#ifdef __PSP__
		if( PspEnsureMeshBuffer( NumPts ) )
		{
			BYTE* Out = GPspMeshBuf;
			for( INT i=0; i<NumPts; i++ )
			{
				FTransTexture* P = Pts[i];
				FLOAT* T = (FLOAT*)Out;
				T[0] = P->U * TexInfo[0].UMult;
				T[1] = P->V * TexInfo[0].VMult;
				BYTE* C = Out + 8;
				if( IsModulated )
				{
					C[0] = C[1] = C[2] = C[3] = 255;
				}
				else
				{
					C[0] = PspToByte( P->Light.X );
					C[1] = PspToByte( P->Light.Y );
					C[2] = PspToByte( P->Light.Z );
					C[3] = 255;
				}
				FLOAT* V = (FLOAT*)( Out + 12 );
				V[0] = P->Point.X;
				V[1] = P->Point.Y;
				V[2] = P->Point.Z;
				Out += 24;
			}
			if( PspSetArrays( 1, 1, GPspMeshBuf, 24 ) )
				PspDrawFan( NumPts );
		}
		else
#endif
		{
		glBegin( GL_TRIANGLE_FAN  );
		for( INT i=0; i<NumPts; i++ )
		{
			FTransTexture* P = Pts[i];
			if( !IsModulated )
				glColor4f( P->Light.X, P->Light.Y, P->Light.Z, 1.f );
			glTexCoord2f( P->U*TexInfo[0].UMult, P->V*TexInfo[0].VMult );
			glVertex3f( P->Point.X, P->Point.Y, P->Point.Z );
		}
		glEnd();
		}

		if( (PolyFlags & (PF_RenderFog|PF_Translucent|PF_Modulated)) == PF_RenderFog )
		{
			ResetTexture( 0 );
			SetBlend( PF_Highlighted );
#ifdef __PSP__
			if( PspEnsureMeshBuffer( NumPts ) )
			{
				BYTE* Out = GPspMeshBuf;   // C4UB_V3F: stride 16
				for( INT i = 0; i < NumPts; i++ )
				{
					FTransTexture* P = Pts[i];
					Out[0] = PspToByte( P->Fog.X );
					Out[1] = PspToByte( P->Fog.Y );
					Out[2] = PspToByte( P->Fog.Z );
					Out[3] = PspToByte( P->Fog.W );
					FLOAT* V = (FLOAT*)( Out + 4 );
					V[0] = P->Point.X;
					V[1] = P->Point.Y;
					V[2] = P->Point.Z;
					Out += 16;
				}
				if( PspSetArrays( 0, 1, GPspMeshBuf, 16 ) )
					PspDrawFan( NumPts );
			}
			else
#endif
			{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i = 0; i < NumPts; i++ )
			{
				FTransTexture* P = Pts[i];
				glColor4f( P->Fog.X, P->Fog.Y, P->Fog.Z, P->Fog.W );
				glVertex3f( P->Point.X, P->Point.Y, P->Point.Z );
			}
			glEnd();
			}
		}

		uunclock(GouraudCycles);
		unguard;
}

void UNOpenGLRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLRenderDevice::DrawTile);

	SetSceneNode( Frame );
	uclock(TileCycles);
	SetBlend( PolyFlags );
	SetTexture( 0, Texture, ( PolyFlags & PF_Masked ), 0.f );
	ResetTexture( 1 );
	ResetTexture( 2 );
	ResetTexture( 3 );

	if( PolyFlags & PF_Modulated )
		glColor4f( 1.f, 1.f, 1.f, 1.f );
	else
		glColor4f( Light.X, Light.Y, Light.Z, 1.f );

#ifdef __PSP__
	// Canvas tiles are drawn with the depth test OFF on PSP. UE1 draws the
	// whole canvas at Z=1.0: the menu panel first, its text over it. Desktop
	// GL passes the equal depth (GL_LEQUAL); the GE does not, so menu text,
	// HUD icons and the intro title were rejected while the panel showed.
	// Found by bisecting with [PSP] TileDepthTest=0, which stays as a switch
	// along with TileAlphaTest and a log of the first masked tiles.
	{
		static INT TileAlpha = -1, TileDepth = 0, TileLog = 0;
		if( TileAlpha < 0 )
		{
			TileAlpha = 1;
			GetConfigInt( "PSP", "TileAlphaTest", TileAlpha );
			GetConfigInt( "PSP", "TileDepthTest", TileDepth );
			debugf( NAME_Log, "PSPTILE: alpha test %s, depth test %s", TileAlpha ? "on" : "OFF", TileDepth ? "on" : "OFF" );
		}
		if( !TileAlpha ) glDisable( GL_ALPHA_TEST );
		if( !TileDepth ) glDisable( GL_DEPTH_TEST );
		if( ( PolyFlags & PF_Masked ) && Y > 40.f && TileLog < 24 )   // skip the HUD line at the top; the menu is what is missing
		{
			++TileLog;
			debugf( NAME_Log, "PSPTILE: masked id=%08X%08X %ix%i mips=%i pal=%i rt=%i | XY %.0f,%.0f size %.0fx%.0f Z=%.2f | UV %.1f,%.1f + %.1f,%.1f mult %.5f,%.5f | flags %08X light %.2f,%.2f,%.2f",
				(unsigned)( Texture.CacheID >> 32 ), (unsigned)Texture.CacheID, Texture.USize, Texture.VSize, Texture.NumMips, Texture.Palette ? 1 : 0,
				( Texture.TextureFlags & TF_Realtime ) ? 1 : 0,
				X, Y, XL, YL, Z, U, V, UL, VL, TexInfo[0].UMult, TexInfo[0].VMult, (unsigned)PolyFlags, Light.X, Light.Y, Light.Z );
		}
	}
#endif

	glBegin( GL_TRIANGLE_FAN );
		glTexCoord2f( (U   )*TexInfo[0].UMult, (V   )*TexInfo[0].VMult );
		glVertex3f( RFX2*Z*(X   -Frame->FX2), RFY2*Z*(Y   -Frame->FY2), Z );
		glTexCoord2f( (U+UL)*TexInfo[0].UMult, (V   )*TexInfo[0].VMult );
		glVertex3f( RFX2*Z*(X+XL-Frame->FX2), RFY2*Z*(Y   -Frame->FY2), Z );
		glTexCoord2f( (U+UL)*TexInfo[0].UMult, (V+VL)*TexInfo[0].VMult );
		glVertex3f( RFX2*Z*(X+XL-Frame->FX2), RFY2*Z*(Y+YL-Frame->FY2), Z );
		glTexCoord2f( (U   )*TexInfo[0].UMult, (V+VL)*TexInfo[0].VMult );
		glVertex3f( RFX2*Z*(X   -Frame->FX2), RFY2*Z*(Y+YL-Frame->FY2), Z );
	glEnd();

#ifdef __PSP__
	glEnable( GL_DEPTH_TEST );   // undo the TileDepthTest bisect for the next draw
#endif
	uunclock(TileCycles);
	unguard;
}

void UNOpenGLRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{
#ifdef __PSP__
	PspFlushBatch();
#endif

}

void UNOpenGLRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )
{
#ifdef __PSP__
	PspFlushBatch();
#endif

}

void UNOpenGLRenderDevice::EndFlash( )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLESRenderDevice::EndFlash);

	if( ColorMod == FPlane( 0.f, 0.f, 0.f, 0.f ) )
		return;

	ResetTexture( 0 );
	ResetTexture( 1 );
	ResetTexture( 2 );
	ResetTexture( 3 );
	SetBlend( PF_Highlighted );

	const FLOAT Z = 1.f;
	const FLOAT RFX2 = RProjZ;
	const FLOAT RFY2 = RProjZ * Aspect;

	glDisable( GL_DEPTH_TEST );

	glColor4fv( &ColorMod.R );
	glBegin( GL_TRIANGLE_FAN );
		glVertex3f( RFX2 * -Z, RFY2 * -Z, Z );
		glVertex3f( RFX2 * +Z, RFY2 * -Z, Z );
		glVertex3f( RFX2 * +Z, RFY2 * +Z, Z );
		glVertex3f( RFX2 * -Z, RFY2 * +Z, Z );
	glEnd();

	glEnable( GL_DEPTH_TEST );

	unguard;
}

void UNOpenGLRenderDevice::PushHit( const BYTE* Data, INT Count )
{
#ifdef __PSP__
	PspFlushBatch();
#endif

}

void UNOpenGLRenderDevice::PopHit( INT Count, UBOOL bForce )
{
#ifdef __PSP__
	PspFlushBatch();
#endif

}

void UNOpenGLRenderDevice::GetStats( char* Result )
{
	guard(UNOpenGLRenderDevice::GetStats)

//	if( Result ) *Result = '\0';
	appSprintf
	(
		Result,
		"OpenGL stats: Bind=%04.1f Image=%04.1f Complex=%04.1f Gouraud=%04.1f Tile=%04.1f",
		GSecondsPerCycle*1000 * BindCycles,
		GSecondsPerCycle*1000 * ImageCycles,
		GSecondsPerCycle*1000 * ComplexCycles,
		GSecondsPerCycle*1000 * GouraudCycles,
		GSecondsPerCycle*1000 * TileCycles
	);

	unguard;
}

void UNOpenGLRenderDevice::ReadPixels( FColor* Pixels )
{
	guard(UNOpenGLRenderDevice::ReadPixels);

	glPixelStorei( GL_UNPACK_ALIGNMENT, 0 );
	glReadPixels( 0, 0, Viewport->SizeX, Viewport->SizeY, GL_RGBA, GL_UNSIGNED_BYTE, (void*)Pixels );

	// Swap RGBA -> BGRA and flip vertically.
	for( INT i=0; i<Viewport->SizeY/2; i++ )
	{
		for( INT j=0; j<Viewport->SizeX; j++ )
		{
			Exchange( Pixels[j+i*Viewport->SizeX].R, Pixels[j+(Viewport->SizeY-1-i)*Viewport->SizeX].B );
			Exchange( Pixels[j+i*Viewport->SizeX].G, Pixels[j+(Viewport->SizeY-1-i)*Viewport->SizeX].G );
			Exchange( Pixels[j+i*Viewport->SizeX].B, Pixels[j+(Viewport->SizeY-1-i)*Viewport->SizeX].R );
		}
	}

	unguard;
}

void UNOpenGLRenderDevice::ClearZ( FSceneNode* Frame )
{
#ifdef __PSP__
	PspFlushBatch();
#endif
	guard(UNOpenGLRenderDevice::ClearZ);

	SetBlend( PF_Occlude );
	glClear( GL_DEPTH_BUFFER_BIT );

	unguard;
}

void UNOpenGLRenderDevice::SetSceneNode( FSceneNode* Frame )
{
	guard(UNOpenGLRenderDevice::SetSceneNode);

	check(Viewport);

	if( !Frame )
	{
		// invalidate current saved data
		CurrentSceneNode.X = -1;
		CurrentSceneNode.FX = -1.f;
		CurrentSceneNode.SizeX = -1;
		return;
	}

	if( Frame->X != CurrentSceneNode.X || Frame->Y != CurrentSceneNode.Y ||
			Frame->XB != CurrentSceneNode.XB || Frame->YB != CurrentSceneNode.YB ||
			Viewport->SizeX != CurrentSceneNode.SizeX || Viewport->SizeY != CurrentSceneNode.SizeY )
	{
		glViewport( Frame->XB, Viewport->SizeY - Frame->Y - Frame->YB, Frame->X, Frame->Y );
		CurrentSceneNode.X = Frame->X;
		CurrentSceneNode.Y = Frame->Y;
		CurrentSceneNode.XB = Frame->XB;
		CurrentSceneNode.YB = Frame->YB;
		CurrentSceneNode.SizeX = Viewport->SizeX;
		CurrentSceneNode.SizeY = Viewport->SizeY;
	}

	if( Frame->FX != CurrentSceneNode.FX || Frame->FY != CurrentSceneNode.FY ||
			Viewport->Actor->FovAngle != CurrentSceneNode.FovAngle )
	{
		RProjZ = appTan( Viewport->Actor->FovAngle * PI / 360.0 );
		Aspect = Frame->FY / Frame->FX;
		RFX2 = 2.0f * RProjZ / Frame->FX;
		RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;
		glMatrixMode( GL_PROJECTION );
		glLoadIdentity();
		glFrustum( -RProjZ, +RProjZ, -Aspect * RProjZ, +Aspect * RProjZ, 1.0, 65336.0 );
		CurrentSceneNode.FX = Frame->FX;
		CurrentSceneNode.FY = Frame->FY;
		CurrentSceneNode.FovAngle = Viewport->Actor->FovAngle;
	}

	unguard;
}

void UNOpenGLRenderDevice::SetBlend( DWORD PolyFlags, UBOOL InverseOrder )
{
	guard(UNOpenGLRenderDevice::SetBlend);

	// Adjust PolyFlags according to Unreal's precedence rules.
	if( !(PolyFlags & (PF_Translucent|PF_Modulated)) )
		PolyFlags |= PF_Occlude;
	else if( PolyFlags & PF_Translucent )
		PolyFlags &= ~PF_Masked;

	// Detect changes in the blending modes.
	DWORD Xor = CurrentPolyFlags ^ PolyFlags;
	if( Xor & (PF_Translucent|PF_Modulated|PF_Invisible|PF_Occlude|PF_Masked|PF_Highlighted) )
	{
		if( Xor&(PF_Translucent|PF_Modulated|PF_Highlighted) )
		{
			glEnable( GL_BLEND );
			if( PolyFlags & PF_Translucent )
			{
				glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_COLOR );
			}
			else if( PolyFlags & PF_Modulated )
			{
				glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );
			}
			else if( PolyFlags & PF_Highlighted )
			{
				glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
			}
			else
			{
				glDisable( GL_BLEND );
				glBlendFunc( GL_ONE, GL_ZERO );
			}
		}
		if( Xor & PF_Invisible )
		{
			UBOOL Show = !( PolyFlags & PF_Invisible );
			glColorMask( Show, Show, Show, Show );
		}
		if( Xor & PF_Occlude )
		{
			glDepthMask( (PolyFlags & PF_Occlude) != 0 );
		}
		if( Xor & PF_Masked )
		{
			if( PolyFlags & PF_Masked )
				glEnable( GL_ALPHA_TEST );
			else
				glDisable( GL_ALPHA_TEST );
		}
	}

	CurrentPolyFlags = PolyFlags;

	unguard;
}

void UNOpenGLRenderDevice::ResetTexture( INT TMU )
{
	guard(UNOpenGLRenderDevice::ResetTexture);

	if( TexInfo[TMU].CurrentCacheID != 0 )
	{
		uclock(BindCycles);
		glActiveTexture( GL_TEXTURE0 + TMU );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glDisable( GL_TEXTURE_2D );
		TexInfo[TMU].CurrentCacheID = 0;
		uunclock(BindCycles);
	}

	unguard;
}

void UNOpenGLRenderDevice::SetTexture( INT TMU, FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias )
{
	guard(UNOpenGLRenderDevice::SetTexture);

	// Set panning.
	FTexInfo& Tex = TexInfo[TMU];
	Tex.UPan      = Info.Pan.X + PanBias*Info.UScale;
	Tex.VPan      = Info.Pan.Y + PanBias*Info.VScale;

	// Account for all the impact on scale normalization.
#ifdef __PSP__
	// UploadTexture pads a sub-8 base mip up to 8x8 (see there). Normalised
	// UV = texel-space * 1/(Scale*Size), so dividing by the PADDED size maps
	// the real USize texels onto exactly the top-left fraction of the padded
	// texture where they were placed. For textures already >= 8 this is the
	// identity.
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>( Max( 8, Info.USize ) ));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>( Max( 8, Info.VSize ) ));
#else
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>(Info.USize));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>(Info.VSize));
#endif

	// Find in cache.
	QWORD NewCacheID = Info.CacheID;
	if( ( PolyFlags & PF_Masked ) && Info.Palette )
		NewCacheID |= MASKED_TEXTURE_TAG;
	UBOOL RealtimeChanged = ( Info.TextureFlags & TF_RealtimeChanged );
	if( NewCacheID == Tex.CurrentCacheID && !RealtimeChanged )
		return;

	// Make current.
	uclock(BindCycles);
	Tex.CurrentCacheID = NewCacheID;
	FCachedTexture* Bind = BindMap.Find( NewCacheID );
	FCachedTexture* OldBind = Bind;
	if( !Bind )
	{
		// New texture.
		Bind = BindMap.Add( NewCacheID, FCachedTexture() );
		glGenTextures( 1, &Bind->Id );
		TexAlloc.AddItem( Bind->Id );
#ifdef __PSP__
		Bind->Bytes = 0;
#endif
	}
#ifdef __PSP__
	Bind->LastFrame = GPspFrameCount;
#endif

	glActiveTexture( GL_TEXTURE0 + TMU );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, Bind->Id );
	uunclock(BindCycles);

	if( !OldBind || RealtimeChanged )
	{
		// New texture or it has changed, upload it.
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, ( PolyFlags & PF_Masked ), !OldBind );
#ifdef __PSP__
		if( !OldBind )
		{
			Bind->Bytes   = GPspLastUploadBytes;
			GPspTexBytes += Bind->Bytes;
			PspEvictTextures();
			Bind = BindMap.Find( NewCacheID );   // Pairs may have moved
		}
#endif
		// Set mip filtering if there are mips.
#ifdef __PSP__
		// Mip filtering only over levels that were really uploaded; the fill
		// in UploadTexture makes the rest valid but they are placeholders.
		const UBOOL HasMips = GPspLastUploadedMips > 1;
#else
		const UBOOL HasMips = Info.NumMips > 1;
#endif
		if( ( PolyFlags & PF_NoSmooth ) || ( NoFiltering && Info.Palette ) ) // TODO: This is set per poly, not per texture.
		{
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, HasMips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		}
		else
		{
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, HasMips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		}
	}

	unguard;
}

void UNOpenGLRenderDevice::EnsureComposeSize( const DWORD NewSize )
{
	if( NewSize > ComposeSize )
	{
		ComposeSize = NewSize;
		Compose = (BYTE*)appRealloc( Compose, NewSize, "GLComposeBuf" );
	}
	verify( Compose );
}

void UNOpenGLRenderDevice::ConvertTextureMipI8( const FMipmap* Mip, const FColor* Palette, const UBOOL Masked, BYTE*& UploadBuf, GLenum& UploadFormat, GLenum& InternalFormat )
{
	// 8-bit indexed. We have to fix the alpha component since it's mostly garbage.
	DWORD i;
	if( UseHwPalette )
	{
		// GL has support for palettized textures, use it. Still have to fix the alpha.
#ifdef __PSP__
		// Same gamma treatment as the expanded path below: brighten the
		// palette, not the texels. The PSP GE applies the CLUT in hardware,
		// so this is the only place the correction can go.
		PspInitGamma();
		static FColor GPspHwGammaPal[256];
		if( GPspGammaActive )
		{
			for( INT k = 0; k < 256; ++k )
			{
				GPspHwGammaPal[k].R = GPspGammaLUT[ Palette[k].R ];
				GPspHwGammaPal[k].G = GPspGammaLUT[ Palette[k].G ];
				GPspHwGammaPal[k].B = GPspGammaLUT[ Palette[k].B ];
				GPspHwGammaPal[k].A = Palette[k].A;
			}
			Palette = GPspHwGammaPal;
		}
#endif
		const DWORD* SrcPal = (const DWORD*)Palette;
		EnsureComposeSize( 256 * 4 );
		DWORD* DstPal = (DWORD*)Compose;
		UploadBuf = Mip->DataPtr;
		InternalFormat = GL_COLOR_INDEX8_EXT;
		UploadFormat = GL_COLOR_INDEX8_EXT;
		i = 0;
		// index 0 is transparent in masked textures
		if( Masked )
		{
			*DstPal++ = 0;
			++SrcPal;   // was missing: every other entry shifted by one, so
			++i;        // masked text drew in entry 0's colour (black) -- invisible
		}
		// 255 alpha on the rest of the palette
		for( ; i < 256; ++i )
			*DstPal++ = *SrcPal++ | ALPHA_MASK;
		// set palette pointer
#ifdef __PSP__
		// pspgl insists internalformat == format for colour tables (and for
		// glTexImage2D); GL_RGBA8 is rejected with GL_INVALID_OPERATION.
		glColorTableEXT( GL_TEXTURE_2D, GL_RGBA, 256, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)Compose );
#else
		glColorTableEXT( GL_TEXTURE_2D, GL_RGBA8, 256, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)Compose );
#endif
	}
	else
	{
		// No support for palettized textures. Expand to RGBA8888 and fix alpha.
		const BYTE* Src = (const BYTE*)Mip->DataPtr;
		const DWORD* Pal = (const DWORD*)Palette;
		const DWORD Count = Mip->USize * Mip->VSize;
#ifdef __PSP__
		// Gamma-correct the palette once, then expand as usual. Alpha is left
		// alone -- it carries the mask, not brightness.
		PspInitGamma();
		static FColor GPspGammaPal[256];
		if( GPspGammaActive )
		{
			for( INT k = 0; k < 256; ++k )
			{
				GPspGammaPal[k].R = GPspGammaLUT[ Palette[k].R ];
				GPspGammaPal[k].G = GPspGammaLUT[ Palette[k].G ];
				GPspGammaPal[k].B = GPspGammaLUT[ Palette[k].B ];
				GPspGammaPal[k].A = Palette[k].A;
			}
			Palette = GPspGammaPal;
			Pal = (const DWORD*)GPspGammaPal;
		}
#endif
		EnsureComposeSize( Count * 4 );
		DWORD* Dst = (DWORD*)Compose;
		UploadBuf = Compose;
		UploadFormat = GL_RGBA;
		InternalFormat = GL_RGBA8;
		if( Masked )
		{
			// index 0 is transparent
#if __INTEL_BYTE_ORDER__
			for( i = 0; i < Count; ++i, ++Src )
				*Dst++ = *Src ? ( Pal[*Src] | ALPHA_MASK ) : 0;
#else
			for( i = 0; i < Count; ++i, ++Src )
			{
				FColor Color = Palette[*Src];
				Color.A = *Src ? 255 : 0;
				*Dst++ = (Color.R << 24) | (Color.G << 16) | (Color.B << 8) | Color.A;
			}
#endif
		}
		else
		{
			// index 0 is whatever
#if __INTEL_BYTE_ORDER__
			for( i = 0; i < Count; ++i )
				*Dst++ = ( Pal[*Src++] | ALPHA_MASK );
#else
			for( i = 0; i < Count; ++i, ++Src )
			{
				FColor Color = Palette[*Src];
				Color.A = 255;
				*Dst++ = (Color.R << 24) | (Color.G << 16) | (Color.B << 8) | Color.A;
			}
#endif
		}
	}
}

void UNOpenGLRenderDevice::ConvertTextureMipBGRA7777( const FMipmap* Mip, BYTE*& UploadBuf, GLenum& UploadFormat, GLenum& InternalFormat )
{
	// BGRA8888. This is actually a BGRA7777 lightmap, so we need to scale it.
	const BYTE* Src = (const BYTE*)Mip->DataPtr;
	const DWORD Count = Mip->USize * Mip->VSize;
	EnsureComposeSize( Count * 4 );
	BYTE* Dst = (BYTE*)Compose;
	UploadBuf = Compose;
	InternalFormat = GL_RGBA8;
	if( UseBGRA )
	{
		UploadFormat = GL_BGRA;
		for( DWORD i = 0; i < Count; ++i )
		{
			*Dst++ = (*Src++) << 1;
			*Dst++ = (*Src++) << 1;
			*Dst++ = (*Src++) << 1;
			*Dst++ = (*Src++) << 1;
		}
	}
	else
	{
		// Swap BGRA -> RGBA
		UploadFormat = GL_RGBA;
		for( DWORD i = 0; i < Count; ++i, Src += 4 )
		{
			*Dst++ = Src[2] << 1;
			*Dst++ = Src[1] << 1;
			*Dst++ = Src[0] << 1;
			*Dst++ = Src[3] << 1;
		}
	}
}

#ifdef __PSP__
//
// Ported from the Vita build (NOpenGLESDrv.cpp): dynamic textures must not be
// re-uploaded into the same memory the GPU may still be reading. vitaGL keeps
// a pointer to the caller's data rather than copying it, so that port
// allocates three buffers per dynamic texture and cycles through them on every
// upload. pspgl hands the GE pointers the same way -- it flushes the range
// with sceKernelDcacheWritebackInvalidateRange rather than taking a copy -- so
// a single shared compose buffer is overwritten while the previous frame's
// draw is still reading it.
//
// "Dynamic" is the Vita's test: TF_Realtime, or no palette (lightmaps and fog
// maps are BGRA7777 and unpalettised), so it is always 4 bytes per pixel here.
//
enum { PSP_DYN_TEX_BUFS = 3 };
static BYTE* GPspDynTex[PSP_DYN_TEX_BUFS] = { NULL, NULL, NULL };
static INT   GPspDynTexSize = 0;
static INT   GPspDynTexCur  = 0;

static BYTE* PspRotateDynTex( const BYTE* Src, INT Bytes )
{
	if( Bytes <= 0 )
		return NULL;
	if( Bytes > GPspDynTexSize )
	{
		for( INT i = 0; i < PSP_DYN_TEX_BUFS; ++i )
		{
			BYTE* New = (BYTE*)realloc( GPspDynTex[i], Bytes );
			if( !New )
				return NULL;
			GPspDynTex[i] = New;
		}
		GPspDynTexSize = Bytes;
	}
	BYTE* Dst = GPspDynTex[GPspDynTexCur];
	GPspDynTexCur = ( GPspDynTexCur + 1 ) % PSP_DYN_TEX_BUFS;
	appMemcpy( Dst, Src, Bytes );
	return Dst;
}
#endif

void UNOpenGLRenderDevice::UploadTexture( FTextureInfo& Info, UBOOL Masked, UBOOL NewTexture )
{
	guard(UNOpenGLRenderDevice::UploadTexture);

	if( !Info.Mips[0] )
	{
		debugf( NAME_Warning, "Encountered texture with invalid mips!" );
		return;
	}

#ifdef PSP_NO_TEXTURES
	// BISECT: skip all texture uploads. CONFIRMED -- with this on, the engine
	// runs happily for hundreds of frames, so the crash is in pspgl's texture
	// upload, not in geometry or the display list.
	return;
#endif

	// Upload all mips.
	INT UploadedMips = 0;
#ifdef __PSP__
	// What the last successful upload looked like, and the base level's size,
	// for the mip-chain fill after the loop (see there).
	BYTE*  LastBuf = NULL; INT LastW = 0, LastH = 0; GLenum LastFmt = 0, LastIF = 0;
	INT    BaseW = 0, BaseH = 0;
	GPspLastUploadBytes = 0;
	if( NewTexture ) ++GPspUpFirst; else ++GPspUpRealtime;
	if( Info.Mips[0] && Info.Mips[0]->USize * Info.Mips[0]->VSize >= 256 * 256 ) ++GPspUpBig;
#endif
	uclock(ImageCycles);
	for( INT MipIndex = 0; MipIndex < Info.NumMips; ++MipIndex )
	{
		const FMipmap* Mip = Info.Mips[MipIndex];
		BYTE* UploadBuf;
		GLenum UploadFormat;
		GLenum InternalFormat;
		if( !Mip || !Mip->DataPtr )
			break;
#ifdef __PSP__
		// The PSP GPU swizzles textures in 8x8 pixel blocks, and pspgl cannot
		// handle a mip smaller than one block: it overruns its buffer while
		// swizzling, which surfaced as PPSSPP segfaulting inside its own
		// emulation thread walking sequentially off the end of mapped memory.
		//
		// Bisected: skipping mips below 8x8 gives 3000+ uploads with zero GL
		// errors and a stable frame loop. At 4x4 pspgl instead reports
		// GL_INVALID_* for ~84% of uploads; below that it corrupts memory.
		// UE1 supplies full chains down to 1x1, so the small levels must go.
		// Sub-8 mips need care in BOTH directions. Uploading one raw corrupts
		// the heap: pspgl sizes its allocation from USize*VSize but swizzles in
		// whole 8x8 blocks, so a 2x2 upload writes past the buffer and the
		// next free() dies (PPSSPP crash in _free_r). Skipping one leaves the
		// texture with no data when the BASE mip is already small -- a 64x4
		// lightmap logged uploaded=0, and 354 of them in one immediate-mode
		// frame -- and the GE then reads garbage at draw time.
		//
		// So: skip the small tail of the chain as before, but PAD the base
		// mip up to 8x8 with edge replication and upload that. SetTexture
		// scales UMult/VMult by the same padded size, so the real texels land
		// exactly where the UVs expect them. This is the Vita port's approach.
		INT   UpW  = Mip->USize;
		INT   UpH  = Mip->VSize;
		UBOOL bPad = 0;
		if( Mip->USize < 8 || Mip->VSize < 8 )
		{
			if( MipIndex > 0 )
				break;
			bPad = 1;
			UpW  = Max( 8, Mip->USize );
			UpH  = Max( 8, Mip->VSize );
		}
#endif
		// Convert texture if needed.
		if( Info.Palette )
			ConvertTextureMipI8( Mip, Info.Palette, Masked, UploadBuf, UploadFormat, InternalFormat );
		else
			ConvertTextureMipBGRA7777( Mip, UploadBuf, UploadFormat, InternalFormat );
#ifdef __PSP__
		if( bPad )
		{
			// Tightly packed at USize stride, 4 bytes/pixel from the RGBA
			// converters or 1 byte/pixel for a hardware-palette (index8) upload.
			// Copy into the top-left of an UpW x UpH image and replicate the
			// last column and row outward, so clamped/filtered sampling at the
			// edge sees the edge texel rather than black.
			static BYTE* PadBuf = NULL;
			static INT   PadCap = 0;
			const INT Bpp  = ( UploadFormat == GL_COLOR_INDEX8_EXT || UploadFormat == GL_COLOR_INDEX ) ? 1 : 4;
			const INT Need = UpW * UpH * Bpp;
			if( Need > PadCap )
			{
				BYTE* N = (BYTE*)realloc( PadBuf, Need );
				if( N ) { PadBuf = N; PadCap = Need; }
			}
			if( PadBuf && PadCap >= Need )
			{
				const INT SrcW = Mip->USize, SrcH = Mip->VSize;
				for( INT y = 0; y < UpH; ++y )
				{
					const INT sy = Min( y, SrcH - 1 );
					if( Bpp == 4 )
					{
						const DWORD* SrcRow = (const DWORD*)UploadBuf + sy * SrcW;
						DWORD*       DstRow = (DWORD*)PadBuf + y * UpW;
						for( INT x = 0; x < UpW; ++x )
							DstRow[x] = SrcRow[ Min( x, SrcW - 1 ) ];
					}
					else
					{
						const BYTE* SrcRow = UploadBuf + sy * SrcW;
						BYTE*       DstRow = PadBuf + y * UpW;
						for( INT x = 0; x < UpW; ++x )
							DstRow[x] = SrcRow[ Min( x, SrcW - 1 ) ];
					}
				}
				UploadBuf = PadBuf;
			}
			else
			{
				UpW = Mip->USize; UpH = Mip->VSize;   // allocation failed: upload raw
			}
		}

		// See PspRotateDynTex: give the GE its own copy of anything that gets
		// re-uploaded, so the next upload does not overwrite it mid-draw.
		if( ( Info.TextureFlags & TF_Realtime ) || !Info.Palette )
		{
			BYTE* Rotated = PspRotateDynTex( UploadBuf, UpW * UpH * 4 );
			if( Rotated )
				UploadBuf = Rotated;
		}
#endif
#ifdef __PSP__
		// pspgl identifies as "OpenGL ES-CM 1.1" and enforces the GLES1 rule
		// that internalformat must be an unsized enum *equal* to format. The
		// converters return the sized GL_RGBA8 (0x8058), which pspgl rejects
		// with GL_INVALID_OPERATION -- so every upload failed and all geometry
		// rendered solid black. Rewrite only the sized RGB/RGBA enums; the
		// paletted path already sets internal == format (GL_COLOR_INDEX8_EXT)
		// and must not be touched.
		if( InternalFormat == GL_RGBA8 )
			InternalFormat = GL_RGBA;
		else if( InternalFormat == GL_RGB8 )
			InternalFormat = GL_RGB;
		// Indexed uploads keep format == internalformat == GL_COLOR_INDEX8_EXT:
		// that is what pspgl's format table matches on (see its glTexImage2D,
		// "if (format != internalformat) goto out_error"). The earlier rewrite
		// to GL_COLOR_INDEX here made every paletted upload fail.
#endif
		// Upload to GL.
#ifdef __PSP__
		++GPspUploadCount;
#endif
		if( NewTexture )
			glTexImage2D( GL_TEXTURE_2D, MipIndex, InternalFormat, UpW, UpH, 0, UploadFormat, GL_UNSIGNED_BYTE, (void*)UploadBuf );
		else
			glTexSubImage2D( GL_TEXTURE_2D, MipIndex, 0, 0, UpW, UpH, UploadFormat, GL_UNSIGNED_BYTE, (void*)UploadBuf );
#ifdef __PSP__
		if( NewTexture && glGetError() != GL_NO_ERROR )
		{
			// pspgl failed the upload -- GL_OUT_OF_MEMORY once the heap is
			// full -- and leaves the texture object with NO image: every level
			// pointer 0. Drawing with it makes the GE fetch texels from address
			// 0 and hangs the console (found by decoding the killing display
			// list under PSPLink). Give it an 8x8 placeholder instead, which is
			// 256 bytes and will fit where 256KB did not.
			if( PspUploadPlaceholder() )
			{
				UploadedMips = 1;
				LastBuf = (BYTE*)GPspPlaceholder; LastW = LastH = 8; LastFmt = GL_RGBA; LastIF = GL_RGBA;
				BaseW = BaseH = 8;
				GPspLastUploadBytes = 8 * 8 * 4;
			}
			if( ++GPspUploadFailed <= 20 || ( GPspUploadFailed % 100 ) == 0 )
				debugf( NAME_Log, "PSPTEX: upload FAILED (%ix%i level %i), placeholder used; %i failures, %i KB resident; %s",
					UpW, UpH, MipIndex, GPspUploadFailed, GPspTexBytes / 1024, PspHeapStr() );
			break;
		}
#endif
		++UploadedMips;
#ifdef __PSP__
		GPspUpBytes += UpW * UpH * ( ( UploadFormat == GL_COLOR_INDEX8_EXT || UploadFormat == GL_COLOR_INDEX ) ? 1 : 4 );
		LastBuf = UploadBuf; LastW = UpW; LastH = UpH; LastFmt = UploadFormat; LastIF = InternalFormat;
		if( NewTexture )
			GPspLastUploadBytes += UpW * UpH * ( ( UploadFormat == GL_COLOR_INDEX8_EXT || UploadFormat == GL_COLOR_INDEX ) ? 1 : 4 );
		if( MipIndex == 0 ) { BaseW = UpW; BaseH = UpH; }
		if( bPad )
			break;   // a padded base mip stands alone; its sub-mips would be sub-8 too
#endif
	}
	uunclock(ImageCycles);

#ifdef __PSP__
	// We stop uploading at 8x8 (pspgl overruns swizzling anything smaller), so
	// the mip chain is TRUNCATED. Without capping GL_TEXTURE_MAX_LEVEL the
	// texture is incomplete and therefore unsamplable -- which is why anything
	// relying on mipmapped sampling came out blank.
#ifdef __PSP__
	// A texture that uploaded nothing (no base mip data) would leave pspgl's
	// object empty and the GE reading address 0 at draw time; same cure as a
	// failed upload.
	if( UploadedMips == 0 && NewTexture && PspUploadPlaceholder() )
	{
		UploadedMips = 1;
		LastBuf = (BYTE*)GPspPlaceholder; LastW = LastH = 8; LastFmt = GL_RGBA; LastIF = GL_RGBA;
		BaseW = BaseH = 8;
		GPspLastUploadBytes = 8 * 8 * 4;
		debugf( NAME_Log, "PSPTEX: %ix%i uploaded no mips, placeholder used", Info.Mips[0]->USize, Info.Mips[0]->VSize );
	}
#endif
	// THE hardware killer, found with PSPLink by decoding the display list
	// that took the console down. pspgl tells the GE how many mip levels a
	// texture has from the BASE level's size alone (max(log2 w, log2 h) + 1)
	// and points every level we did not upload at address 0. It ignores
	// GL_TEXTURE_MAX_LEVEL and its completeness check is compiled out. We
	// deliberately stop the chain early (sub-8 mips, see above), so with a
	// mipmap min filter the first small textured polygon makes the GE fetch
	// texels from physical address 0. PPSSPP reads zeros there; the real PSP
	// hangs its bus and powers off. So give every level the GE may sample a
	// valid image: repeat the smallest real level down to the announced end.
	// Placeholders only; they are never re-uploaded for realtime textures.
	if( NewTexture && UploadedMips > 0 && LastBuf )
	{
		INT MaxLvl = 0;
		while( ( 1 << ( MaxLvl + 1 ) ) <= Max( BaseW, BaseH ) )
			++MaxLvl;
		// 8x8 placeholders, never the last level's full size: a lone 256x256
		// level would otherwise get eight 256KB copies (2MB) and blow the heap.
		// Every real level is >= 8x8, so LastBuf holds at least one 8x8 block;
		// with a mipmap filter the GE only reaches these when the texture is
		// under 8 pixels on screen, and single-level textures never sample them.
		const INT FillBpp = ( LastFmt == GL_COLOR_INDEX8_EXT || LastFmt == GL_COLOR_INDEX ) ? 1 : 4;
		for( INT L = UploadedMips; L <= MaxLvl; ++L )
		{
			glTexImage2D( GL_TEXTURE_2D, L, LastIF, 8, 8, 0, LastFmt, GL_UNSIGNED_BYTE, (void*)LastBuf );
			if( glGetError() != GL_NO_ERROR )
			{
				debugf( NAME_Log, "PSPTEX: mip fill FAILED at level %i", L );
				break;
			}
			GPspLastUploadBytes += 8 * 8 * FillBpp;
		}
	}
	GPspLastUploadedMips = UploadedMips;
#endif

	unguard;
}

void UNOpenGLRenderDevice::UpdateSwapInterval()
{
	guard(UNOpenGLRenderDevice::UpdateSwapInterval);

	if( SwapInterval < -1 )
	{
		SwapInterval = -1;
	}

	if( SDL_GL_SetSwapInterval( SwapInterval ) < 0 )
	{
		debugf( NAME_Warning, "Failed to set swap interval %d: %s", SwapInterval, SDL_GetError() );
		if( SwapInterval < 0 )
		{
			// Adaptive VSync not supported, try normal VSync.
			SwapInterval = 1;
			UpdateSwapInterval();
		}
	}

	unguard;
}
