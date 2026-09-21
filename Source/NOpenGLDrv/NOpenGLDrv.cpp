#include "SDL2/SDL.h"
#ifdef __PSP__
#include <pspsysmem.h>
#include <malloc.h>
#include "glad_psp.h"
#else
#include "glad.h"
#endif

#include "NOpenGLDrvPrivate.h"

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
	}

	unguard;
}

UBOOL UNOpenGLRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	return false;
}

void UNOpenGLRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
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
#ifdef PSP_CLEAR_TEST
	// TEMPORARY DIAGNOSTIC -- is Lock() reached at all? If this never prints,
	// the engine is not rendering and the black screen is a main-loop problem,
	// not a GL one.
	{
		static INT LockCount = 0;
		if( LockCount < 3 || ( LockCount % 10 ) == 0 )
			debugf( NAME_Log, "PSPDIAG: Lock() call #%d flags=0x%08x", LockCount, (unsigned)RenderLockFlags );
		++LockCount;
	}
	// The magenta clear that proved the present path works is retired -- it
	// painted over the scene. Force a black clear every frame instead, so that
	// stale back-buffer contents cannot be mistaken for rendered geometry.
	glClearColor( 0.f, 0.f, 0.f, 1.f );
	ClearBits |= GL_COLOR_BUFFER_BIT;
#endif
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
static FLOAT* GPspVtx     = NULL;   // interleaved u,v,x,y,z for one pass
static INT    GPspVtxMax  = 0;      // capacity in vertices

static UBOOL PspEnsureVtxBuffers( INT Pts )
{
	if( Pts > GPspVtxMax )
	{
		FLOAT* NewRaw = (FLOAT*)realloc( GPspRawUV, Pts * 2 * sizeof(FLOAT) );
		if( !NewRaw ) return 0;
		GPspRawUV = NewRaw;
		// 24 bytes/vertex covers both T2F_V3F (20) and T2F_C4UB_V3F (24).
		FLOAT* NewVtx = (FLOAT*)realloc( GPspVtx, Pts * 24 );
		if( !NewVtx ) return 0;
		GPspVtx = NewVtx;
		GPspVtxMax = Pts;
	}
	return 1;
}

// Emit every polygon of the facet for one pass, reusing the cached raw U/V.
static void PspEmitFacet( FSurfaceFacet& Facet, FLOAT UDot, FLOAT VDot,
                          FLOAT UPan, FLOAT VPan, FLOAT UMult, FLOAT VMult )
{
	INT Base = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT N = Poly->NumPts;
		FLOAT* Out = GPspVtx;
		for( INT i = 0; i < N; ++i )
		{
			const FLOAT* Raw = &GPspRawUV[ ( Base + i ) * 2 ];
			*Out++ = ( Raw[0] - UDot - UPan ) * UMult;
			*Out++ = ( Raw[1] - VDot - VPan ) * VMult;
			const FVector& P = Poly->Pts[i]->Point;
			*Out++ = P.X;
			*Out++ = P.Y;
			*Out++ = P.Z;
		}
		glInterleavedArrays( GL_T2F_V3F, 0, GPspVtx );
		glDrawArrays( GL_TRIANGLE_FAN, 0, N );
		Base += N;
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
	// Same -0.5 texel bias the lightmap pass applies via SetTexture's PanBias.
	const FLOAT LMUPan = LM.Pan.X - 0.5f * LM.UScale;
	const FLOAT LMVPan = LM.Pan.Y - 0.5f * LM.VScale;
	const FLOAT InvUScale = 1.f / LM.UScale;
	const FLOAT InvVScale = 1.f / LM.VScale;

	INT Base = 0;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT N = Poly->NumPts;
		BYTE* Out = (BYTE*)GPspVtx;
		for( INT i = 0; i < N; ++i )
		{
			const FLOAT* Raw = &GPspRawUV[ ( Base + i ) * 2 ];

			FLOAT* T = (FLOAT*)Out;
			T[0] = ( Raw[0] - UDot - UPan ) * UMult;
			T[1] = ( Raw[1] - VDot - VPan ) * VMult;

			// Lightmap texel for this vertex.
			INT iu = appFloor( ( Raw[0] - UDot - LMUPan ) * InvUScale );
			INT iv = appFloor( ( Raw[1] - VDot - LMVPan ) * InvVScale );
			iu = Clamp( iu, 0, LMU - 1 );
			iv = Clamp( iv, 0, LMV - 1 );
			const BYTE* Texel = &LMData[ ( iv * LMU + iu ) * 4 ];

			BYTE* C = Out + 8;
			C[0] = GPspLightLUT[ Texel[2] & 0x7F ];   // BGRA source -> R
			C[1] = GPspLightLUT[ Texel[1] & 0x7F ];   // G
			C[2] = GPspLightLUT[ Texel[0] & 0x7F ];   // B
			C[3] = 255;

			FLOAT* V = (FLOAT*)( Out + 12 );
			const FVector& P = Poly->Pts[i]->Point;
			V[0] = P.X; V[1] = P.Y; V[2] = P.Z;
			Out += 24;
		}
		glInterleavedArrays( GL_T2F_C4UB_V3F, 0, GPspVtx );
		glDrawArrays( GL_TRIANGLE_FAN, 0, N );
		Base += N;
	}
}
#endif

void UNOpenGLRenderDevice::DrawComplexSurfaceSingleTex( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	const FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	const FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;

#ifdef __PSP__
	{
		INT TotalPts = 0;
		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
			TotalPts += Poly->NumPts;

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

			// Base texture.
			SetBlend( Surface.PolyFlags );
			SetTexture( 0, *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
			if( bVertexLit )
			{
				// One pass: GU_TFX_MODULATE gives texture * vertex colour.
				PspEmitFacetLit( Facet, UDot, VDot, TexInfo[0].UPan, TexInfo[0].VPan,
					TexInfo[0].UMult, TexInfo[0].VMult, *Surface.LightMap );
			}
			else
			{
				glColor4f( 1.f, 1.f, 1.f, 1.f );
				PspEmitFacet( Facet, UDot, VDot, TexInfo[0].UPan, TexInfo[0].VPan, TexInfo[0].UMult, TexInfo[0].VMult );
			}

			// Lightmap.
			//
			// BISECT/tunable: lightmaps are the one thing torches actually make
			// expensive -- dynamic lights mark them TF_RealtimeChanged every
			// frame, so each one is both re-uploaded AND costs a second full
			// geometry pass over the surface. Set [PSP] LightMaps=0 in
			// Unreal.ini to drop the pass and measure what it is worth.
			static INT PspLightMaps = -1;
			if( PspLightMaps < 0 )
			{
				PspLightMaps = 1;
				GetConfigInt( "PSP", "LightMaps", PspLightMaps );
				debugf( NAME_Log, "PSPPERF: lightmap pass = %s", PspLightMaps ? "on" : "off" );
			}
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
			return;
		}
		// Allocation failed -- fall through to the portable immediate-mode path.
	}
#endif

	// Draw texture.
#ifdef PSP_CLEAR_TEST
	// TEMPORARY EXPERIMENT -- pspgl buffers immediate-mode geometry into a
	// fixed-size GU display list. UE1 submits a whole Unreal scene per frame in
	// glBegin/glVertex3f form, and PPSSPP crashes walking sequentially off the
	// end of a mapped region (fault address advances by 0x100 between runs),
	// which is what an overrun list would look like. Flushing per surface
	// forces pspgl to submit and start a fresh list.
	glFlush();
#endif
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
	if( Pts > GPspMeshMax )
	{
		BYTE* NewBuf = (BYTE*)realloc( GPspMeshBuf, Pts * 24 );
		if( !NewBuf ) return 0;
		GPspMeshBuf = NewBuf;
		GPspMeshMax = Pts;
	}
	return 1;
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
			glInterleavedArrays( GL_T2F_C4UB_V3F, 0, GPspMeshBuf );
			glDrawArrays( GL_TRIANGLE_FAN, 0, NumPts );
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
				glInterleavedArrays( GL_C4UB_V3F, 0, GPspMeshBuf );
				glDrawArrays( GL_TRIANGLE_FAN, 0, NumPts );
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

	uunclock(TileCycles);
	unguard;
}

void UNOpenGLRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{

}

void UNOpenGLRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )
{

}

void UNOpenGLRenderDevice::EndFlash( )
{
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

}

void UNOpenGLRenderDevice::PopHit( INT Count, UBOOL bForce )
{

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
#ifdef PSP_CLEAR_TEST
		// TEMPORARY DIAGNOSTIC -- the forced clear shows only as a stripe, so
		// print the numbers the viewport is actually working with.
		{
			static INT VpLog = 0;
			if( VpLog < 4 )
			{
				debugf( NAME_Log, "PSPDIAG: viewport size=%dx%d frame X=%d Y=%d XB=%d YB=%d -> glViewport(%d,%d,%d,%d)",
					Viewport->SizeX, Viewport->SizeY,
					Frame->X, Frame->Y, Frame->XB, Frame->YB,
					Frame->XB, Viewport->SizeY - Frame->Y - Frame->YB, Frame->X, Frame->Y );
				++VpLog;
			}
		}
#endif
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
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>(Info.USize));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>(Info.VSize));

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
	}

	glActiveTexture( GL_TEXTURE0 + TMU );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, Bind->Id );
	uunclock(BindCycles);

	if( !OldBind || RealtimeChanged )
	{
		// New texture or it has changed, upload it.
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, ( PolyFlags & PF_Masked ), !OldBind );
		// Set mip filtering if there are mips.
		if( ( PolyFlags & PF_NoSmooth ) || ( NoFiltering && Info.Palette ) ) // TODO: This is set per poly, not per texture.
		{
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ( Info.NumMips > 1 ) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		}
		else
		{
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ( Info.NumMips > 1 ) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR );
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
			++i;
		}
		// 255 alpha on the rest of the palette
		for( ; i < 256; ++i )
			*DstPal++ = *SrcPal++ | ALPHA_MASK;
		// set palette pointer
		glColorTableEXT( GL_TEXTURE_2D, GL_RGBA8, 256, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)Compose );
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
		if( Mip->USize < 8 || Mip->VSize < 8 )
			break;
#endif
		// Convert texture if needed.
		if( Info.Palette )
			ConvertTextureMipI8( Mip, Info.Palette, Masked, UploadBuf, UploadFormat, InternalFormat );
		else
			ConvertTextureMipBGRA7777( Mip, UploadBuf, UploadFormat, InternalFormat );
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
		else if( InternalFormat == GL_COLOR_INDEX8_EXT )
		{
			// EXT_paletted_texture wants internalformat = GL_COLOR_INDEX8_EXT
			// but format = GL_COLOR_INDEX. UE1 passes GL_COLOR_INDEX8_EXT for
			// both, which desktop drivers tolerate and pspgl rejects with
			// GL_INVALID_OPERATION.
			UploadFormat = GL_COLOR_INDEX;
		}
#endif
		// Upload to GL.
#ifdef __PSP__
		++GPspUploadCount;
#endif
		if( NewTexture )
			glTexImage2D( GL_TEXTURE_2D, MipIndex, InternalFormat, Mip->USize, Mip->VSize, 0, UploadFormat, GL_UNSIGNED_BYTE, (void*)UploadBuf );
		else
			glTexSubImage2D( GL_TEXTURE_2D, MipIndex, 0, 0, Mip->USize, Mip->VSize, UploadFormat, GL_UNSIGNED_BYTE, (void*)UploadBuf );
#ifdef PSP_CLEAR_TEST
		// TEMPORARY DIAGNOSTIC -- does pspgl actually accept these uploads?
		// Geometry renders solid black, so either the texels never arrive or
		// the format is being rejected. Report the first few, plus every error.
		{
			static INT UpLog = 0, ErrLog = 0, UpTotal = 0, ErrTotal = 0;
			const GLenum Err = glGetError();
			++UpTotal;
			// Separate budgets so a flood of successes cannot crowd out the
			// first errors. Both capped: Logf costs 4KB of stack and this sits
			// deep in the render recursion.
			UBOOL WantLog;
			if( Err != GL_NO_ERROR ) { ++ErrTotal; WantLog = ( ErrLog++ < 15 ); }
			else                     { WantLog = ( UpLog  <  15 ); }
			// Bounded running totals, so VRAM exhaustion is visible even after
			// the per-line budgets are spent.
			if( ( UpTotal % 250 ) == 0 )
				debugf( NAME_Log, "PSPDIAG: uploads=%d errors=%d", UpTotal, ErrTotal );
			if( WantLog )
			{
				debugf( NAME_Log, "PSPDIAG: upload #%d mip=%d %dx%d int=0x%04x fmt=0x%04x pal=%d err=0x%04x freemem=%u maxblock=%u",
					UpLog, MipIndex, Mip->USize, Mip->VSize, (unsigned)InternalFormat, (unsigned)UploadFormat,
					Info.Palette ? 1 : 0, (unsigned)Err,
					(unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize() );
				++UpLog;
			}
		}
#endif
	}
	uunclock(ImageCycles);

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
