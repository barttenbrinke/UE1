/*=============================================================================
	UnGame.cpp: Unreal game engine.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "EnginePrivate.h"
#include "UnRender.h"
#include "UnNet.h"

/*-----------------------------------------------------------------------------
	Object class implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(UGameEngine);

/*-----------------------------------------------------------------------------
	Temporary.
-----------------------------------------------------------------------------*/

void UGameEngine::PaintProgress()
{
	guard(PaintProgress);

	FVector LoadFog(0,.1,.25);
	FVector LoadScale(.2,.2,.2);
	UViewport* Viewport=Client->Viewports(0);
	Exchange(Viewport->Actor->FlashFog,LoadFog);
	Exchange(Viewport->Actor->FlashScale,LoadScale);
	Draw( Viewport, NULL, NULL );
	Exchange(Viewport->Actor->FlashFog,LoadFog);
	Exchange(Viewport->Actor->FlashScale,LoadScale);

	unguard;
}

INT UGameEngine::ChallengeResponse( INT Challenge )
{
	guard(UGameEngine::ChallengeResponse);
	return (Challenge*237) ^ (0x93fe92Ce) ^ (Challenge>>16) ^ (Challenge<<16);
	unguard;
}

/*-----------------------------------------------------------------------------
	Game init and exit.
-----------------------------------------------------------------------------*/

//
// Construct the game engine.
//
UGameEngine::UGameEngine()
: LastURL("")
{}

//
// Class creator.
//
void UGameEngine::InternalClassInitializer( UClass* Class )
{
	guard(UGameEngine::InternalClassInitializer);
	if( appStricmp(Class->GetName(),"GameEngine")==0 )
	{
		(new(Class,"ServerActors",  RF_Public)UStringProperty( CPP_PROPERTY(ServerActors  ), "Settings", CPF_Config, 96 ))->ArrayDim=16;
		(new(Class,"ServerPackages",RF_Public)UStringProperty( CPP_PROPERTY(ServerPackages), "Settings", CPF_Config, 96 ))->ArrayDim=16;
	}
	unguard;
}

//
// Initialize the game engine.
//
void UGameEngine::Init()
{
	guard(UGameEngine::Init);
	check(sizeof(*this)==GetClass()->GetPropertiesSize());

	// Call base.
	UEngine::Init();

	// Init variables.
	GLevel = NULL;

	// Delete temporary files in cache.
	appCleanFileCache();

	// If not a dedicated server.
	if( GIsClient )
	{	
		// Init client.
		UClass* ClientClass = GObj.LoadClass( UClient::StaticClass, NULL, "ini:Engine.Engine.ViewportManager", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
		Client = ConstructClassObject<UClient>( ClientClass );
		Client->Init( this );

		// Init rendering.
		UClass* RenderClass = GObj.LoadClass( URenderBase::StaticClass, NULL, "ini:Engine.Engine.Render", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
		Render = ConstructClassObject<URenderBase>( RenderClass );
		Render->Init( this );
	}

	// Load the entry level.
	char Error256[256];
	if( Client )
	{
		if( !LoadMap( FURL("Entry"), NULL, Error256 ) )
			appErrorf( LocalizeError("LoadEntry"), Error256 );
		Exchange( GLevel, GEntry );
	}

	// Create default URL.
	FURL DefaultURL;
	DefaultURL.GetConfigOptions( "DefaultPlayer" );

	// Enter initial world.
	char AutoURL[4096]="";
	const char* Tmp = appCmdLine();
	if
	(	!ParseToken( Tmp, AutoURL, ARRAY_COUNT(AutoURL), 0 )
	||	AutoURL[0]=='-' )
		appStrcpy( AutoURL, *FURL::DefaultLocalMap );
	FURL URL( &DefaultURL, AutoURL, TRAVEL_Partial );
	if( !URL.Valid )
		appErrorf( LocalizeError("InvalidUrl"), AutoURL );
	UBOOL Success = Browse( FURL(&LastURL,AutoURL,TRAVEL_Partial), Error256 );

	// If waiting for a network connection, go into the starting level.
	if( !Success && !Error256[0] && appStricmp( AutoURL, *FURL::DefaultLocalMap )!=0 )
		Success = Browse( FURL(&LastURL,*FURL::DefaultLocalMap,TRAVEL_Partial), Error256 );

	// Handle failure.
	if( !Success )
		appErrorf( LocalizeError("FailedBrowse"), AutoURL, Error256 );

	// Open initial Viewport.
	if( Client )
	{
		UViewport* Viewport = Client->NewViewport( GLevel, NAME_None );
		char Error256[256];
		if( !GLevel->SpawnPlayActor( Viewport, ROLE_SimulatedProxy, URL, "", Error256 ) )
			appErrorf( Error256 );
		Viewport->Input->Init( Viewport, GSystem );
		Viewport->OpenWindow( NULL, 0, Client->ViewportX, Client->ViewportY, INDEX_NONE, INDEX_NONE );
		if( Audio )
			Audio->SetViewport( Viewport );
		if( GPendingLevel )
		{
			// Reprint connecting message.
			char Msg1[256], Msg2[256];
			appSprintf( Msg1, "Connecting (F10 Cancels):" );
			appSprintf( Msg2, "unreal://%s/%s", *URL.Host, *URL.Map );
			SetProgress( Msg1, Msg2, 60.0 );
		}
	}
	debugf( NAME_Init, "Game engine initialized" );
	unguard;
}

//
// Game exit.
//
void UGameEngine::Destroy()
{
	guard(UGameEngine::Destroy);

	// Game exit.
	if( GPendingLevel )
		CancelPending();
	GLevel = NULL;
	debugf( NAME_Exit, "Game engine shut down" );

	UEngine::Destroy();
	unguard;
}

//
// Progress text.
//
void UGameEngine::SetProgress( const char* Str1, const char* Str2, FLOAT Seconds )
{
	guard(UGameEngine::SetProgress);
	if( Client && Client->Viewports.Num() )
	{
		APlayerPawn* Actor = Client->Viewports(0)->Actor;
		if( Seconds==-1.0 )
		{
			// Upgrade message.
			Actor->eventShowUpgradeMenu();
		}
		appStrncpy( Actor->ProgressMessage, Str1, ARRAY_COUNT(Actor->ProgressMessage) );
		appStrncpy( Actor->ProgressMessageTwo, Str2, ARRAY_COUNT(Actor->ProgressMessageTwo) );
		Actor->ProgressTimeOut = Actor->Level->TimeSeconds + Seconds;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Command line executor.
-----------------------------------------------------------------------------*/

//
// This always going to be the last exec handler in the chain. It
// handles passing the command to all other global handlers.
//
UBOOL UGameEngine::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UGameEngine::Exec);
	const char *Str = Cmd;
	if( ParseCommand( &Str, "OPEN" ) )
	{
		char Error256[256];
		if( !Browse( FURL(&LastURL,Str,TRAVEL_Partial), Error256 ) && Error256[0] )
			Out->Logf( "Open failed: %s", Error256 );
		return 1;
	}
	else if( ParseCommand( &Str, "START" ) )
	{
		char Error256[256];
		if( !Browse( FURL(&LastURL,Str,TRAVEL_Absolute), Error256 ) && Error256[0] )
			Out->Logf( "Start failed: %s", Error256 );
		return 1;
	}
	else if( ParseCommand(&Str,"SAVEGAME") )
	{
		if( !GIsEditor && appIsDigit(Str[0]) && Str[1]==0 )
			SaveGame( appAtoi(Str) );
		return 1;
	}
	else if( ParseCommand( &Cmd, "CANCEL" ) )
	{
		if( GPendingLevel )
			SetProgress( "Cancelled Connect Attempt", "", 2.0 );
		else
			SetProgress( "", "", 0.0 );
		CancelPending();
		return 1;
	}
	else if( GLevel && GLevel->Exec( Cmd, Out ) )
	{
		return 1;
	}
	else if( UEngine::Exec( Cmd, Out ) )
	{
		return 1;
	}
	else return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Serialization.
-----------------------------------------------------------------------------*/

//
// Serializer.
//
void UGameEngine::Serialize( FArchive& Ar )
{
	guard(UGameEngine::Serialize);
	UEngine::Serialize(Ar);

	Ar << GLevel << GEntry << GPendingLevel;

	unguardobj;
}

/*-----------------------------------------------------------------------------
	Game entering.
-----------------------------------------------------------------------------*/

//
// Cancel pending level.
//
void UGameEngine::CancelPending()
{
	guard(UGameEngine::CancelPending);
	if( GPendingLevel )
	{
		delete GPendingLevel;
		GPendingLevel = NULL;
	}
	unguard;
}

//
// Match Viewports to actors.
//
static void MatchViewportsToActors( UClient* Client, ULevel* Level, const FURL& URL )
{
	guard(MatchViewportsToActors);
	for( INT i=0; i<Client->Viewports.Num(); i++ )
	{
		char Error256[256]="";
		UViewport* Viewport = Client->Viewports(i);
		debugf( NAME_Log, "Spawning new actor for Viewport %s", Viewport->GetName() );
		if( !Level->SpawnPlayActor( Viewport, ROLE_SimulatedProxy, URL, Viewport->TravelItems, Error256 ) )
			appErrorf( Error256 );
		Viewport->TravelItems = "";
	}
	unguard;
}

//
// Browse to a specified URL, relative to the current one.
//
UBOOL UGameEngine::Browse( FURL URL, char* Error256 )
{
	guard(UGameEngine::Browse);
	check(Error256);
	Error256[0]=0;
	const char* Option;

	// Crack the URL.
	FString UrlStr;
	const char* StringURL=NULL;
	guard(Message);
	URL.String(UrlStr);
	StringURL = *UrlStr;
	debugf( "Browse: %s", StringURL );
	unguard;
	if( !URL.Valid )
	{
		// Unknown URL.
		guard(UnknownURL);
		appSprintf( Error256, LocalizeError("InvalidUrl"), StringURL );
		unguard;
		return 0;
	}
	else if( URL.HasOption("failed") )
	{
		// Handle failure URL.
		guard(FailedURL);
		debugf( NAME_Log, LocalizeError("AbortToEntry") );
		GLevel = GEntry;
		GLevel->GetLevelInfo()->LevelAction = LEVACT_None;
		check(Client && Client->Viewports.Num());
		MatchViewportsToActors( Client, GLevel, URL );
		if( Audio && Client->Viewports.Num() )
			Audio->SetViewport( Client->Viewports(0) );
		GObj.CollectGarbage( GSystem, RF_Intrinsic );
		unguard;
		return 1;
	}
	else if( URL.HasOption("pop") )
	{
		// Pop the hub.
		guard(PopURL);
		if( GLevel && GLevel->GetLevelInfo()->HubStackLevel>0 )
		{
			char Filename[256], SavedPortal[256];
			appSprintf( Filename, "%s\\Game%i.usa", GSys->SavePath, GLevel->GetLevelInfo()->HubStackLevel-1 );   // URL (see the load= branch)
			appStrcpy( SavedPortal, *URL.Portal );
			URL = FURL( &URL, Filename, TRAVEL_Partial );
			URL.Portal = SavedPortal;
		}
		else return 0;
		unguard;
	}
	else if( URL.HasOption("restart") )
	{
		// Handle restarting.
		guard(RestartURL);
		URL = LastURL;
		unguard;
	}
	else if( (Option=URL.GetOption("load=",NULL))!=NULL )
	{
		// Handle restarting.
		guard(LoadURL);
		char Temp[256], Error256[256];
		// URL, not a file path: FURL takes a forward slash as a host separator, so keep the backslash (the PSP file layer converts it)
		appSprintf( Temp, "%s\\Save%i.usa?load", GSys->SavePath, appAtoi(Option) );
		if( LoadMap(FURL(&LastURL,Temp,TRAVEL_Partial),NULL,Error256) )
		{
			// Copy the hub stack.
			INT i;
			for( i=0; i<GLevel->GetLevelInfo()->HubStackLevel; i++ )
			{
				char Src[256], Dest[256];
				appSprintf( Src, "%s" PATH_SEPARATOR "Save%i%i.usa", PATH(GSys->SavePath), appAtoi(Option), i );
				appSprintf( Dest, "%s" PATH_SEPARATOR "Game%i.usa", PATH(GSys->SavePath), i );
				appCopyFile( Src, Dest );
			}
			while( 1 )
			{
				appSprintf( Temp, "%s" PATH_SEPARATOR "Game%i.usa", PATH(GSys->SavePath), i++ );
				if( appFSize(Temp)<=0 )
					break;
				appUnlink( Temp );
			}
			LastURL = GLevel->URL;
			return 1;
		}
		else return 0;
		unguard;
	}

	// Handle normal URL's.
	if( URL.IsLocalInternal() )
	{
		// Local map file.
		guard(LocalMapURL);
#ifdef __PSP__
		// Two full levels never fit the PSP heap at once: LoadMap keeps the
		// old level alive until the new one is in, and the intro map plus
		// Vortex2 ran the heap dry (appMalloc returned NULL inside UPolys and
		// the load died at a null pointer). So hop through the small,
		// already-resident Entry level first: that shuts the old level down
		// and lets garbage collection free it before the real load starts.
		if( GLevel && GLevel != GEntry && appStricmp( *URL.Map, "Entry" ) != 0 )
		{
			debugf( NAME_Log, "PSP: releasing %s before loading %s", GLevel->GetPathName(), *URL.Map );
			if( !LoadMap( FURL( &LastURL, "Entry", TRAVEL_Partial ), NULL, Error256 ) )
				return 0;
			GObj.CollectGarbage( GSystem, RF_Intrinsic );
		}
#endif
		return LoadMap( URL, NULL, Error256 )!=NULL;
		unguard;
	}
	else if( URL.IsInternal() && GIsClient && (!GLevel || !GLevel->NetDriver || GLevel->NetDriver->ServerConnection) )
	{
		// Network URL.
		guard(NetworkURL);
		if( GPendingLevel )
			CancelPending();
		char Msg1[256], Msg2[256];
		appSprintf( Msg1, "Connecting (F10 Cancels):" );
		appSprintf( Msg2, "unreal://%s/%s", *URL.Host, *URL.Map );
		SetProgress( Msg1, Msg2, 60.0 );
		GPendingLevel = new UPendingLevel( this, URL );
		if( !GPendingLevel->NetDriver )
		{
			SetProgress( "Networking Failed", GPendingLevel->Error256, 6.0 );
			delete GPendingLevel;
			GPendingLevel = NULL;
		}
		return 0;
		unguard;
	}
	else if( URL.IsInternal() )
	{
		// Invalid.
		guard(InvalidURL);
		appSprintf( Error256, LocalizeError("ServerOpen") );
		unguard;
		return 0;
	}
	else
	{
		// External URL.
		guard(ExternalURL);
		appLaunchURL( StringURL, "", Error256 );
		unguard;
		return 0;
	}
	unguard;
}

//
// Load a map.
//
#ifdef __PSP__
// Texture texels, sound samples, mesh render data and music bytes are
// dropped after loading and re-read from the package on demand. A save
// game serialises the level package's own objects, so anything embedded in
// the map (MyLevel textures, level sounds) would be written empty and be
// unrecoverable from the save. Bring them back first.
static void PspReloadFreedForSave( UObject* Package )
{
	guard(PspReloadFreedForSave);
	INT Reloaded = 0;
	for( TObjectIterator<UObject> It; It; ++It )
	{
		if( !It->IsIn( Package ) || !It->GetLinker() )
			continue;
		UBOOL Empty = 0;
		if( UTexture* T = Cast<UTexture>( *It ) )
			Empty = T->Mips.Num() && T->USize > 0 && !T->Mips(0).DataArray.Num();
		else if( USound* Snd = Cast<USound>( *It ) )
			Empty = Snd->OriginalSize > 0 && !Snd->Data.Num();
		else if( UMesh* M = Cast<UMesh>( *It ) )
			Empty = M->FrameVerts > 0 && !M->Verts.Num();
		else if( UMusic* Mu = Cast<UMusic>( *It ) )
			Empty = Mu->OriginalSize > 0 && !Mu->Data.Num();
		if( Empty && appReloadObject( *It ) )
			++Reloaded;
	}
	if( Reloaded )
		debugf( NAME_Log, "PSPPERF: reloaded %i freed objects of %s before saving", Reloaded, Package->GetName() );
	unguard;
}
#endif

#ifdef __PSP__
// Lazily freed mesh and texture data comes back from the memory stick the
// first time it is drawn: dropping into a new room stalls the frame for the
// reads. Now that a level loads at 17-30 MB, bring the level's own meshes
// and textures back right after the load while the heap stays under
// [PSP] PrefetchHeapMB. Sounds stay lazy (small reads).
static UBOOL PspPrefetchTexture( UTexture* T, INT& Count, INT Limit )
{
	for( INT Hop = 0; T && Hop < 64; T = T->AnimNext, ++Hop )
	{
		if( !T->Mips.Num() || T->Mips(0).DataArray.Num() || !T->GetLinker() )
			continue;
		if( appPspHeapUsedKB() >= Limit )
			return 0;
		if( PspEnsureTexels( T ) )
			++Count;
	}
	return 1;
}
static void PspPrefetchLevel( ULevel* Level )
{
	guard(PspPrefetchLevel);
	static INT LimitMB = -1;
	if( LimitMB < 0 ) { LimitMB = 30; GetConfigInt( "PSP", "PrefetchHeapMB", LimitMB ); }
	if( !Level || LimitMB <= 0 )
		return;
	const INT Limit = LimitMB * 1024;
	const INT Start = appPspHeapUsedKB();
	INT Meshes = 0, Textures = 0; UBOOL Room = 1;
	for( INT i = 0; i < Level->Num() && Room; i++ )
	{
		AActor* A = Level->Actors(i);
		if( !A || !A->Mesh || A->Mesh->Verts.Num() )
			continue;
		if( appPspHeapUsedKB() >= Limit ) { Room = 0; break; }
		if( A->Mesh->PspPrefetch() )
			++Meshes;
	}
	// then every other mesh: bots, weapons and projectiles are spawned by
	// class later, so the actor list above covers only pickups and decor
	for( TObjectIterator<UMesh> It; It && Room; ++It )
	{
		if( It->Verts.Num() || It->FrameVerts <= 0 )
			continue;
		if( appPspHeapUsedKB() >= Limit ) { Room = 0; break; }
		if( It->PspPrefetch() )
			++Meshes;
	}
	const INT AfterMeshes = appPspHeapUsedKB();
	if( Room && Level->Model && Level->Model->Surfs )
		for( INT i = 0; i < Level->Model->Surfs->Num() && Room; i++ )
			Room = PspPrefetchTexture( Level->Model->Surfs->Element(i).Texture, Textures, Limit );
	for( INT i = 0; i < Level->Num() && Room; i++ )
	{
		AActor* A = Level->Actors(i);
		if( !A ) continue;
		Room = PspPrefetchTexture( A->Texture, Textures, Limit ) && PspPrefetchTexture( A->Skin, Textures, Limit );
		if( Room && A->Mesh )
			for( INT j = 0; j < A->Mesh->Textures.Num() && Room; j++ )
				Room = PspPrefetchTexture( A->Mesh->Textures(j), Textures, Limit );
	}
	debugf( NAME_Log, "PSPPERF: prefetched %i meshes (%i KB) and %i textures (%i KB)%s; heap used %i KB, limit %i MB",
		Meshes, AfterMeshes - Start, Textures, appPspHeapUsedKB() - AfterMeshes, Room ? "" : " -- stopped at the limit", appPspHeapUsedKB(), LimitMB );
	unguard;
}
#endif

#ifdef __PSP__
static FLOAT  GPspAutoWalkPending = 0.f, GPspAutoWalkDelay = 0.f;   // -AUTOWALK waiting for -WALKDELAY
static DOUBLE GPspAutoWalkT0 = 0.0;                                   // when the walk began (slow-frame log)
#endif
ULevel* UGameEngine::LoadMap( const FURL& URL, UPendingLevel* Pending, char* Error256 )
{
	guard(UGameEngine::LoadMap);
	check(!GIsEditor);
	Error256[0]=0;
	FString Str;
	URL.String(Str);
	debugf( NAME_Log, "LoadMap: %s", *Str );
#ifdef __PSP__
	const DOUBLE PspLoadStart = appSeconds();
	appPspLoadCacheBegin();   // packages read whole while the load runs (UnFile.cpp)
	// [PSP] MemDump=1 lists every object class with its memory after the
	// load, while memory is still available to print with.
	extern CORE_API INT GPspFileRefills, GPspFileRefillBytes, GPspFileSeeks, GPspFileReopens;
	extern CORE_API SQWORD GPspFileIoCycles;
	struct FPspLoadTimer { DOUBLE T0; const char* Map; UGameEngine* Engine;
		INT Refills0 = GPspFileRefills, Bytes0 = GPspFileRefillBytes, Seeks0 = GPspFileSeeks, Reopens0 = GPspFileReopens; SQWORD Io0 = GPspFileIoCycles;
	~FPspLoadTimer()
	{
		ULevel* L = Engine->GLevel; INT NullActors = 0;
		PspPrefetchLevel( L );
		// Sounds: samples are deferred at load and fetched on first play, and
		// a first play mid-action (the Vortex Rikers earthquake gibbing the
		// corpses) stalled whole frames. Register the smallest deferred
		// sounds now, up to [PSP] SoundPrefetchKB (keep it under
		// SoundBudgetKB or the budget evicts what was just fetched).
		if( Engine->Audio )
		{
			static INT PrefetchKB = -1;
			if( PrefetchKB < 0 ) { PrefetchKB = 1536; GetConfigInt( "PSP", "SoundPrefetchKB", PrefetchKB ); }
			TArray<USound*> Deferred;
			for( TObjectIterator<USound> It; It; ++It )
				if( !It->Data.Num() && It->GetLinker() && It->OriginalSize > 0 )
					Deferred.AddItem( *It );
			for( INT i = 0; i < Deferred.Num(); i++ ) for( INT j = i + 1; j < Deferred.Num(); j++ ) if( Deferred(j)->OriginalSize < Deferred(i)->OriginalSize ) Exchange( Deferred(i), Deferred(j) );
			INT KB = 0, N = 0; const DOUBLE T1 = appSeconds();
			for( INT i = 0; i < Deferred.Num() && PrefetchKB > 0; i++ )
			{
				if( KB + Deferred(i)->OriginalSize / 1024 > PrefetchKB ) break;
				// Re-read the object: USound::Serialize registers it on the way
				// (RegisterSound itself needs the samples, which are not here).
				if( appReloadObject( Deferred(i) ) && Deferred(i)->Handle )
				{
					KB += Deferred(i)->OriginalSize / 1024; ++N;
				}
			}
			debugf( NAME_Log, "PSPPERF: prefetched %i of %i deferred sounds (%i KB of %i) in %.1f s", N, Deferred.Num(), KB, PrefetchKB, (FLOAT)( appSeconds() - T1 ) );
		}
		INT CacheFiles = 0, CacheKB = 0; appPspLoadCacheEnd( CacheFiles, CacheKB );
		{
			// Sound census: how much a sound prefetch at load would have to bring in.
			INT NSnd = 0, NDeferred = 0, KBDeferred = 0, KBResident = 0;
			for( TObjectIterator<USound> It; It; ++It )
			{
				++NSnd;
				if( It->Data.Num() ) KBResident += It->Data.Num() / 1024;
				else if( It->GetLinker() ) { ++NDeferred; KBDeferred += It->OriginalSize / 1024; }
			}
			debugf( NAME_Log, "PSPSND: after load %i sounds: %i deferred (%i KB in packages), %i KB resident", NSnd, NDeferred, KBDeferred, KBResident );
		}
		{
			INT Walk = 0, Delay = 0; Parse( appCmdLine(), "AUTOWALK=", Walk ); Parse( appCmdLine(), "WALKDELAY=", Delay );
			if( Walk > 0 && appStrstr( Map, "?load" ) ) { GPspAutoWalkPending = (FLOAT)Walk; GPspAutoWalkDelay = (FLOAT)Delay; debugf( NAME_Log, "PSPTEST: autowalk %i s after %i s", Walk, Delay ); }
		}
		if( L ) for( INT i = 0; i < L->Num(); ++i ) if( !L->Actors(i) ) ++NullActors;
		debugf( NAME_Log, "PSPPERF: LoadMap %s took %.1f s; %s; actors %i (%i null); stick: %i refills %i KB, %.1f s in read/seek/open, %i seeks, %i reopens; %i files (%i KB) read whole",
			Map, (FLOAT)( appSeconds() - T0 ), appPspHeapState(), L ? L->Num() : 0, NullActors,
			GPspFileRefills - Refills0, ( GPspFileRefillBytes - Bytes0 ) / 1024, (FLOAT)( GSecondsPerCycle * (DOUBLE)( GPspFileIoCycles - Io0 ) ), GPspFileSeeks - Seeks0, GPspFileReopens - Reopens0, CacheFiles, CacheKB );
		{
			char Top[768]; appPspStickReport( Top, 768 );   // consumed here so the frame report shows play-time traffic only
			debugf( NAME_Log, "PSPPERF: LoadMap stick by file: %s", Top );
		}
		// -IOCHECK: exercise the file layer's read paths against each other.
		if( ParseParam( appCmdLine(), "IOCHECK" ) )
		{
			appPspIoCheck( "../Sounds/Ambmodern.uax" );
			appPspIoCheck( "../Textures/PlayrShp.utx" );
			appPspIoCheck( "../System/UnrealI.u" );
		}
		// -TEXCRC: checksum every texture and mesh the level load left
		// resident, so a hardware log diffs against an emulator log of the
		// same map (the emulator's sceIoRead is a memcpy; the stick is DMA).
		if( ParseParam( appCmdLine(), "TEXCRC" ) )
		{
			INT NTex = 0, NMesh = 0;
			for( TObjectIterator<UTexture> It; It; ++It )
			{
				if( It->Mips.Num() && It->Mips(0).DataArray.Num() )
				{
					debugf( NAME_Log, "PSPDATACRC: tex %s %i crc %08x", It->GetPathName(), It->Mips(0).DataArray.Num(), (DWORD)appMemCrc( &It->Mips(0).DataArray(0), It->Mips(0).DataArray.Num() ) );
					++NTex;
				}
			}
			for( TObjectIterator<UMesh> It; It; ++It )
			{
				if( It->Verts.Num() && It->Tris.Num() )
				{
					debugf( NAME_Log, "PSPDATACRC: mesh %s verts %i crc %08x tris %i crc %08x", It->GetPathName(), It->Verts.Num(), (DWORD)appMemCrc( (BYTE*)&It->Verts(0), It->Verts.Num() * sizeof(FMeshVert) ), It->Tris.Num(), (DWORD)appMemCrc( (BYTE*)&It->Tris(0), It->Tris.Num() * sizeof(FMeshTri) ) );
					++NMesh;
				}
			}
			debugf( NAME_Log, "PSPDATACRC: %i textures, %i meshes resident after load", NTex, NMesh );
		}
		INT Dump = 0; GetConfigInt( "PSP", "MemDump", Dump );
		if( Dump )
		{
			GObj.Exec( "OBJ LIST", GSystem );
			// OBJ LIST reports serialised sizes; this is what the instances
			// themselves occupy (PropertiesSize per object), by class.
			struct FClassBytes { UClass* Class; INT Bytes; INT Count; };
			TArray<FClassBytes> Tally;
			INT Total = 0, Objects = 0;
			for( FObjectIterator It; It; ++It )
			{
				UClass* C = It->GetClass(); const INT B = C ? C->PropertiesSize : 0;
				Total += B; ++Objects;
				INT k; for( k=0; k<Tally.Num(); k++ ) if( Tally(k).Class == C ) break;
				if( k == Tally.Num() ) { FClassBytes N; N.Class = C; N.Bytes = 0; N.Count = 0; Tally.AddItem( N ); }
				Tally(k).Bytes += B; Tally(k).Count++;
			}
			for( INT i=0; i<Tally.Num(); i++ ) for( INT j=i+1; j<Tally.Num(); j++ ) if( Tally(j).Bytes > Tally(i).Bytes ) Exchange( Tally(i), Tally(j) );
			debugf( NAME_Log, "PSPMEM: %i objects, %i KB of instance memory (PropertiesSize)", Objects, Total / 1024 );
			appPspDumpBigBlocks( 30 );
			for( INT i=0; i<Min(Tally.Num(),20); i++ )
				debugf( NAME_Log, "PSPMEM:   %-28s %6i objects %6i KB", Tally(i).Class ? Tally(i).Class->GetName() : "?", Tally(i).Count, Tally(i).Bytes / 1024 );
		}
	} } PspLoadTimer = { PspLoadStart, *Str, this };
#endif

	// Remember current level's stack level.
	INT SavedHubStackLevel = GLevel ? GLevel->GetLevelInfo()->HubStackLevel : 0;

	// Display loading screen.
	guard(LoadingScreen);
	if( Client && Client->Viewports.Num() && GLevel )
	{
		GLevel->GetLevelInfo()->LevelAction = LEVACT_Loading;
		PaintProgress();
		if( Audio )
			Audio->SetViewport( Client->Viewports(0) );
		GLevel->GetLevelInfo()->LevelAction = LEVACT_None;
	}
	unguard;

	// Verify that we can load all packages we need.
	FGuid* Guid = NULL;
	UObject* MapParent = NULL;
	guard(VerifyPackages);
	try
	{
		if( Pending )
		{
			UNetConnection* Connection = Pending->NetDriver->ServerConnection;
			for( INT i=0; i<Connection->Driver->Map.Num(); i++ )
				GObj.GetPackageLinker( Connection->Driver->Map(i).Parent, NULL, LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL, &Connection->Driver->Map(i).Guid );
			if( Connection->Driver->Map.Num() )
			{
				MapParent = Connection->Driver->Map(0).Parent;
				Guid = &Connection->Driver->Map(0).Guid;
			}
		}
		LoadObject<ULevel>( MapParent, "MyLevel", PATH(*URL.Map), LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL );
	}
	catch( char* Error )
	{
		// Safely failed loading.
		appStrcpy( Error256, Error );
		SetProgress( "Failed To Load Map", Error, 6.0 );
		return NULL;
	}
	unguard;

	// Dissociate Viewport actors.
	guard(DissociateViewports);
	if( Client )
	{
		for( INT i=0; i<Client->Viewports.Num(); i++ )
		{
			APlayerPawn* Actor          = Client->Viewports(i)->Actor;
			ULevel*      Level          = Actor->XLevel;
			Actor->Player               = NULL;
			Client->Viewports(i)->Actor = NULL;
			Level->DestroyActor( Actor );
		}
	}
	unguard;

	// Clean up game state.
	guard(ExitLevel);
	if( GLevel )
	{
		// Shut down.
		GObj.ResetLoaders( GLevel->GetParent() );
		if( GLevel->BrushTracker )
		{
			GLevel->BrushTracker->Exit();
			delete GLevel->BrushTracker;
			GLevel->BrushTracker = NULL;
		}
		if( GLevel->NetDriver )
		{
			delete GLevel->NetDriver;
			GLevel->NetDriver = NULL;
		}
		if( URL.HasOption("push") )
		{
			// Save the current level sans players actors.
			GLevel->CleanupDestroyed( 1 );
			char Filename[256];
			appSprintf( Filename, "%s" PATH_SEPARATOR "Game%i.usa", PATH(GSys->SavePath), SavedHubStackLevel );
#ifdef __PSP__
			PspReloadFreedForSave( GLevel->GetParent() );
#endif
			GObj.SavePackage( GLevel->GetParent(), GLevel, 0, Filename );
		}
		GLevel = NULL;
#ifdef __PSP__
		// The old level is otherwise collected only after the new one has
		// loaded (end of LoadMap), so a save load or restart held two levels
		// at once: 6.6 MB over the level's own size on Rrajigar Mine. Normal
		// travel passes through Entry and is unaffected.
		GObj.CollectGarbage( GSystem, RF_Intrinsic );
		debugf( NAME_Log, "PSPPERF: old level released before load; %s", appPspHeapState() );
#endif
	}
	unguard;

	// Load all packages we need.
	guard(LoadLevel);
	if( MapParent && Guid )
		GObj.GetPackageLinker( MapParent, NULL, LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL, Guid );
	GLevel = LoadObject<ULevel>( MapParent, "MyLevel", PATH(*URL.Map), LOAD_KeepImports | LOAD_NoFail, NULL );
	check(!GLevel->NetDriver);
	unguard;

	// Setup network package info.
	if( Pending )
	{
		if( Pending->LonePlayer )
		{
			Pending = NULL;
		}
		else
		{
			Pending->NetDriver->ServerConnection->Driver->Map.Compute();
		}
	}

	// Verify classes.
	guard(VerifyClasses);
	VERIFY_CLASS_OFFSET( A, Actor,       Owner         );
	VERIFY_CLASS_OFFSET( A, Actor,       TimerCounter  );
	VERIFY_CLASS_OFFSET( A, PlayerPawn,  Player        );
	VERIFY_CLASS_OFFSET( A, PlayerPawn,  MaxStepHeight );
	unguard;

	// Get LevelInfo.
	check(GLevel);
	ALevelInfo* Info = GLevel->GetLevelInfo();
	appStrcpy( Info->ComputerName, GComputerName );

	// Handle pushing.
	guard(ProcessHubStack);
	Info->HubStackLevel
	=	URL.HasOption("load") ? Info->HubStackLevel
	:	URL.HasOption("push") ? SavedHubStackLevel+1
	:	URL.HasOption("pop" ) ? Max(SavedHubStackLevel-1,0)
	:	URL.HasOption("peer") ? SavedHubStackLevel
	:	                        0;
	unguard;

	// Handle pending level.
	guard(ActivatePending);
	if( Pending )
	{
		check(Pending==GPendingLevel);

		// Hook network driver up to level.
		GLevel->NetDriver         = Pending->NetDriver;
		GLevel->NetDriver->Notify = GLevel;

		// Setup level.
		GLevel->GetLevelInfo()->NetMode    = NM_Client;
		GLevel->GetLevelInfo()->bInternet  = GLevel->NetDriver->IsInternet();
	}
	else check(!GLevel->NetDriver);
	unguard;

	// Set level info.
	guard(InitLevel);
	if( !URL.GetOption("load",NULL) )
		GLevel->URL = URL;
	appStrncpy( Info->EngineVersion, "1.0", ARRAY_COUNT(Info->EngineVersion) );
	GLevel->Engine = this;
	unguard;

	// Purge unused objects and flush caches.
	guard(Cleanup);
	Flush();
	GObj.CollectGarbage( GSystem, RF_Intrinsic );
	unguard;

	// Init collision.
	GLevel->SetActorCollision( 1 );

	// Setup zone distance table for sound damping.
	guard(SetupZoneTable);
	QWORD OldConvConn[64];
	QWORD ConvConn[64];
	INT i, j;
	for( i=0; i<64; i++ )
	{
		for ( INT j=0; j<64; j++ )
		{
			OldConvConn[i] = GLevel->Model->Nodes->Zones[i].Connectivity;
			if( i == j )
				GLevel->ZoneDist[i][j] = 0;
			else
				GLevel->ZoneDist[i][j] = 255;
		}
	}
	for( i=1; i<64; i++ )
	{
		for( j=0; j<64; j++ )
			for( INT k=0; k<64; k++ )
				if( (GLevel->ZoneDist[j][k] > i) && ((OldConvConn[j] & ((QWORD)1 << k)) != 0) )
					GLevel->ZoneDist[j][k] = i;
		for( j=0; j<64; j++ )
			ConvConn[j] = 0;
		for( j=0; j<64; j++ )
			for( INT k=0; k<64; k++ )
				if( (OldConvConn[j] & ((QWORD)1 << k)) != 0 )
					ConvConn[j] = ConvConn[j] | OldConvConn[k];
		for( j=0; j<64; j++ )
			OldConvConn[j] = ConvConn[j];
	}
	unguard;

	// Init the game info.
	char Options[1024]="";
	char Error256[256]="";
	char GameClassName[256]="";
	guard(InitGameInfo);
	for( INT i=0; i<URL.Op.Num(); i++ )
	{
		appStrcat( Options, "?" );
		appStrcat( Options, *URL.Op(i) );
		Parse( *URL.Op(i), "GAME=", GameClassName, ARRAY_COUNT(GameClassName) );
	}
	if( GLevel->IsServer() && !Info->Game )
	{
		// Get the GameInfo class.
		UClass* GameClass=NULL;
		if( !GameClassName[0] )
		{
			GameClass=Info->DefaultGameType;
			if( !GameClass )
				GameClass = GObj.LoadClass( AGameInfo::StaticClass, NULL, Client ? "ini:Engine.Engine.DefaultGame" : "ini:Engine.Engine.DefaultServerGame", NULL, LOAD_NoFail | LOAD_KeepImports, GLevel->GetSandbox() );
		}
		else GameClass = GObj.LoadClass( AGameInfo::StaticClass, NULL, GameClassName, NULL, LOAD_NoFail | LOAD_KeepImports, GLevel->GetSandbox() );

		// Spawn the GameInfo.
		debugf( NAME_Log, "Game class is '%s'", GameClass->GetName() );
		Info->Game = (AGameInfo*)GLevel->SpawnActor( GameClass );
		check(Info->Game!=NULL);
	}
	unguard;

	// Listen for clients.
	guard(Listen);
	if( !Client || URL.HasOption("Listen") )
	{
		char Error256[256];
		if( !GLevel->Listen( Error256 ) )
			appErrorf( LocalizeError("ServerListen"), Error256 );
	}
	unguard;

	// Init detail.
	Info->bHighDetailMode = 1;
	if
	(	Client
	&&	Client->Viewports.Num()
	&&	Client->Viewports(0)->RenDev
	&&	!Client->Viewports(0)->RenDev->HighDetailActors )
		Info->bHighDetailMode = 0;

	// Init level gameplay info.
	guard(BeginPlay);
	GLevel->iFirstDynamicActor = 0;
	if( !Info->bBegunPlay )
	{
		// Lock the level.
		debugf( NAME_Log, "Bringing %s up for play...", GLevel->GetFullName() );
		GLevel->TimeSeconds = 0;
		GLevel->GetLevelInfo()->TimeSeconds = 0;

		// Init touching actors.
		INT i;
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				for( INT j=0; j<ARRAY_COUNT(GLevel->Actors(i)->Touching); j++ )
					GLevel->Actors(i)->Touching[j] = NULL;

		// Handle network issues.
		if( !GLevel->IsServer() )
		{
			// Kill off actors that aren't interesting to the client.
			for( INT i=0; i<GLevel->Num(); i++ )
			{
				AActor* Actor = GLevel->Actors(i);
				if( Actor )
				{
					if( Actor->bStatic || Actor->bNoDelete )
						Exchange( Actor->Role, Actor->RemoteRole );
					else
						GLevel->DestroyActor( Actor );
				}
			}
		}

		// Init scripting.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->InitExecution();

		// Enable actor script calls.
		Info->bBegunPlay = 1;
		Info->bStartup = 1;

		// Init the game.
		if( Info->Game )
			Info->Game->eventInitGame( Options, Error256 );

		// Send PreBeginPlay.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventPreBeginPlay();

		// Set BeginPlay.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventBeginPlay();

		// Set zones.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->SetActorZone( GLevel->Actors(i), 1, 1 );

		// Post begin play.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventPostBeginPlay();

		// Begin scripting.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventSetInitialState();

		// Find bases
		for( i=0; i<GLevel->Num(); i++ )
		{
			if( GLevel->Actors(i) && !GLevel->Actors(i)->Base && GLevel->Actors(i)->bCollideWorld 
				 && (GLevel->Actors(i)->IsA(ADecoration::StaticClass) || GLevel->Actors(i)->IsA(AInventory::StaticClass) || GLevel->Actors(i)->IsA(APawn::StaticClass)) 
				 &&	((GLevel->Actors(i)->Physics == PHYS_None) || (GLevel->Actors(i)->Physics == PHYS_Rotating)) )
			{
				 GLevel->Actors(i)->FindBase();
				 if ( GLevel->Actors(i)->Base == Info )
					 GLevel->Actors(i)->SetBase(NULL, 0);
			}
		}
		Info->bStartup = 0;
	}
	unguard;

	// Rearrange actors: static first, then others.
	guard(Rearrange);
	TArray<AActor*> Actors;
	Actors.AddItem(GLevel->Element(0));
	Actors.AddItem(GLevel->Element(1));
	INT i;
	for( i=2; i<GLevel->Num(); i++ )
		if( GLevel->Element(i) && GLevel->Element(i)->bStatic )
			Actors.AddItem( GLevel->Element(i) );
	GLevel->iFirstDynamicActor=Actors.Num();
	for( i=2; i<GLevel->Num(); i++ )
		if( GLevel->Element(i) && !GLevel->Element(i)->bStatic )
			Actors.AddItem( GLevel->Element(i) );
	GLevel->Empty();
	GLevel->Add( Actors.Num() );
	for( i=0; i<Actors.Num(); i++ )
		GLevel->Element(i) = Actors(i);
	unguard;

	// Cleanup profiling.
#if DO_SLOW_GUARD
	guard(CleanupProfiling);
	for( TObjectIterator<UFunction> It; It; ++It )
		It->Calls = It->Cycles=0;
	GTicks=1;
	unguard;
#endif

	// Client init.
	guard(ClientInit);
	if( Client )
	{
		// Match Viewports to actors.
		MatchViewportsToActors( Client, GLevel->IsServer() ? GLevel : GEntry, URL );

		// Reset input.
		for( INT i=0; i<Client->Viewports.Num(); i++ )
			Client->Viewports(i)->Input->ResetInput();

		// Init brush tracker.
		GLevel->BrushTracker = GNewBrushTracker( GLevel );

		// Set up audio.
		if( Audio && Client->Viewports.Num()>0 )
			Audio->SetViewport( Client->Viewports(0) );
	}
	unguard;

	// Init detail.
	GLevel->DetailChange( Info->bHighDetailMode );

	// Remember the URL.
	guard(RememberURL);
	LastURL = URL;
	unguard;

	// Successfully started local level.
	return GLevel;
	unguard;
}

/*-----------------------------------------------------------------------------
	Game Viewport functions.
-----------------------------------------------------------------------------*/

//
// Draw a global view.
//
void UGameEngine::Draw( UViewport* Viewport, BYTE* HitData, INT* HitSize )
{
	guard(UGameEngine::Draw);

	// Get view location.
	AActor*      ViewActor    = Viewport->Actor;
	FVector      ViewLocation = ViewActor->Location;
	FRotator     ViewRotation = ViewActor->Rotation;
	Viewport->Actor->eventPlayerCalcView( ViewActor, ViewLocation, ViewRotation );
	check(ViewActor);

	// See if viewer is inside world.
	DWORD LockFlags=0;
	FCheckResult Hit;
	if( !GLevel->Model->PointCheck(Hit,NULL,ViewLocation,FVector(0,0,0),0) )
		LockFlags |= LOCKR_ClearScreen;

	// Lock the Viewport.
	check(Render);
	FPlane FlashScale = Client->ScreenFlashes ? 0.5*Viewport->Actor->FlashScale : FVector(0.5,0.5,0.5);
	FPlane FlashFog   = Client->ScreenFlashes ? Viewport->Actor->FlashFog : FVector(0,0,0);
	FlashScale.X = Clamp( FlashScale.X, 0.f, 1.f );
	FlashScale.Y = Clamp( FlashScale.Y, 0.f, 1.f );
	FlashScale.Z = Clamp( FlashScale.Z, 0.f, 1.f );
	FlashFog.X   = Clamp( FlashFog.X  , 0.f, 1.f );
	FlashFog.Y   = Clamp( FlashFog.Y  , 0.f, 1.f );
	FlashFog.Z   = Clamp( FlashFog.Z  , 0.f, 1.f );
	if( !Viewport->Lock(FlashScale,FlashFog,FPlane(0,0,0,0),LockFlags,HitData,HitSize) )
	{
		debugf( NAME_Warning, "Couldn't lock Viewport for drawing" );
		return;
	}

	// Setup rendering coords.
	FMemMark SceneMark(GSceneMem);
	FSceneNode* Frame = Render->CreateMasterFrame( Viewport, ViewLocation, ViewRotation, NULL );

	// Update level audio.
	if( Audio )
	{
		uclock(GLevel->AudioTickCycles);
		const DWORD PspC0 = appCycles();
		Audio->Update( ViewActor->Region, Frame->Coords );
		GPspAudioCycles += (DWORD)( appCycles() - PspC0 );
		uunclock(GLevel->AudioTickCycles);
	}
	FMemMark MemMark(GMem);
	FMemMark DynMark(GDynMem);

	// Render.
	Render->PreRender( Frame );
	if( Viewport->Console )
		Viewport->Console->PreRender( Frame );
	Viewport->Canvas->Update( Frame );
	Viewport->Actor->eventPreRender( Viewport->Canvas );
	if( Frame->X>0 && Frame->Y>0 )
		Render->DrawWorld( Frame );
	Viewport->RenDev->EndFlash();
	Viewport->Actor->eventPostRender( Viewport->Canvas );
	if( Viewport->Console )
		Viewport->Console->PostRender( Frame );
	Render->PostRender( Frame );

	// Done.
	Viewport->Unlock( 1 );
	MemMark.Pop();
	DynMark.Pop();
	SceneMark.Pop();

	unguard;
}

void ExportTravel( FOutputDevice& Out, AActor* Actor )
{
	guard(ExportTravel);
	check(Actor);
	if( !Actor->bTravel )
		return;
	Out.Logf( "Class=%s Name=%s\r\n{\r\n", Actor->GetClass()->GetPathName(), Actor->GetName() );
	for( TFieldIterator<UProperty> It(Actor->GetClass()); It; ++It )
	{
		for( INT Index=0; Index<It->ArrayDim; Index++ )
		{
			char Value[1024];
			if
			(	(It->PropertyFlags & CPF_Travel)
			&&	It->ExportText( Index, Value, (BYTE*)Actor, &Actor->GetClass()->Defaults(0), 0 ) )
			{
				Out.Log( It->GetName() );
				if( It->ArrayDim!=1 )
					Out.Logf( "[%i]", Index );
				Out.Log( "=" );
				UObjectProperty* Ref = Cast<UObjectProperty>( *It );
				if( Ref && Ref->PropertyClass->IsChildOf(AActor::StaticClass) )
				{
					UObject* Obj = *(UObject**)( (BYTE*)Actor + It->Offset + Index*It->GetElementSize() );
					Out.Logf( "%s\r\n", Obj ? Obj->GetName() : "None" );
				}
				Out.Logf( "%s\r\n", Value );
			}
		}
	}
	Out.Logf( "}\r\n" );
	unguard;
}

//
// Jumping viewport.
//
void UGameEngine::SetClientTravel( UPlayer* Player, const char* NextURL, UBOOL bURL, UBOOL bItems, ETravelType TravelType )
{
	guard(UGameEngine::SetClientTravel);
	if( !Player && Client && Client->Viewports.Num() )
	{
		Player = Client->Viewports(0);
	}
	if( Player )
	{
		if( NextURL && appStricmp(NextURL,"?RESTART")==0 && Player->Actor && Player->Actor->CarryInfo )
		{
			// Automatically carry items the player had at start.
			Player->TravelItems = Player->Actor->CarryInfo->Text;
		}
		else if( bItems )
		{
			// Export items and self.
			FStringOut CarryInfo;
			ExportTravel( CarryInfo, Player->Actor );
			for( AActor* Inv=Player->Actor->Inventory; Inv; Inv=Inv->Inventory )
				ExportTravel( CarryInfo, Inv );
			Player->TravelItems = CarryInfo;
		}
		if( bURL && Cast<UViewport>(Player) )
		{
			// Set next URL.
			Cast<UViewport>(Player)->TravelURL = NextURL;
			Cast<UViewport>(Player)->TravelType = TravelType;
		}
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Tick.
-----------------------------------------------------------------------------*/

//
// Get tick rate limitor.
//
INT UGameEngine::GetMaxTickRate()
{
	guard(UEngine::GetMaxTickRate);
	if( GLevel && GLevel->NetDriver && !GLevel->NetDriver->ServerConnection )
		return GLevel->NetDriver->MaxTicksPerSecond;
	else
		return 0;
	unguard;
}

//
// Update everything.
//
void UGameEngine::Tick( FLOAT DeltaSeconds )
{
	guard(UGameEngine::Tick);
	INT LocalTickCycles=0;
	uclock(LocalTickCycles);

	// If all viewports closed, time to exit.
	if( Client && Client->Viewports.Num()==0 )
	{
		debugf("All Windows Closed");
		appRequestExit();
		return;
	}

	// If game is paused, release the cursor.
	static UBOOL WasPaused=1;
	if( Client && Client->CaptureMouse && Client->Viewports.Num()==1 && GLevel && !Client->FullscreenViewport )
	{
		UBOOL IsPaused = (GLevel->GetLevelInfo()->Pauser[0]!=0) || (Client->Viewports(0)->Actor->bShowMenu);
		if( IsPaused && !WasPaused )
			Client->Viewports(0)->SetMouseCapture( 0, 0 );
		else if( WasPaused && !IsPaused )
			Client->Viewports(0)->SetMouseCapture( 1, 1, 1 );
		WasPaused = IsPaused;
	}
	else WasPaused=0;

	// Update subsystems.
	GObj.Tick();				
	GCache.Tick();

	// Update the level.
	guard(TickLevel);
	GameCycles=0;
	uclock(GameCycles);
	{
		const DWORD PspC0 = appCycles();
		if( GLevel )
			GLevel->Tick( LEVELTICK_All, DeltaSeconds );
		GPspTickCycles += (DWORD)( appCycles() - PspC0 );
	}
	if( Client && Client->Viewports.Num() && Client->Viewports(0)->Actor->XLevel!=GLevel )
		Client->Viewports(0)->Actor->XLevel->Tick( LEVELTICK_All, DeltaSeconds );
	uunclock(GameCycles);
	unguard;

	// Handle server travelling.
	guard(ServerTravel);
#ifdef __PSP__
	// -MAPCYCLE=secs (test hook): walk a fixed map list through the game's
	// own server travel, to compare the heap after repeated loads of the
	// same map (leaks across level changes).
	{
		static INT   Cycle = -1; static FLOAT Left = 0.f; static INT Index = 0;
		static const char* Maps[] = { "DmRadikus.unr?Game=UnrealI.DeathMatchGame", "NyLeve.unr", "Dig.unr", "Chizra.unr", "SkyTown.unr" };
		if( Cycle < 0 ) { Cycle = 0; Parse( appCmdLine(), "MAPCYCLE=", Cycle ); Left = (FLOAT)Cycle; }
		if( Cycle > 0 && GLevel && !*GLevel->GetLevelInfo()->NextURL )
		{
			Left -= DeltaSeconds;
			if( Left <= 0.f )
			{
				appStrcpy( GLevel->GetLevelInfo()->NextURL, Maps[Index++ % ARRAY_COUNT(Maps)] );
				GLevel->GetLevelInfo()->NextSwitchCountdown = 0.f;
				Left = (FLOAT)Cycle;
			}
		}
	}
	// -LOAD=N: load save slot N as soon as the entry level is up, then
	// -AUTOWALK=secs holds the stick full forward for that long once the
	// saved level is running. Together they replay a crash from a save made
	// just before it, without anyone at the controls.
	{
		static INT Slot = -2; static UBOOL Done = 0;
		if( Slot == -2 ) { Slot = -1; if( !Parse( appCmdLine(), "LOAD=", Slot ) ) Slot = -1; }   // slot 0 is a real slot
		if( Slot >= 0 && !Done && GLevel )
		{
			Done = 1;
			char Cmd[64]; appSprintf( Cmd, "START ?load=%i", Slot );
			debugf( NAME_Log, "PSPTEST: %s", Cmd );
			Exec( Cmd, GSystem );
		}
		// -WALKDELAY=secs: let the post-load spike (first uploads, reloads)
		// settle before the walk starts, so the event lands in a clean interval.
		if( GPspAutoWalkPending > 0.f )
		{
			GPspAutoWalkDelay -= Min( DeltaSeconds, 0.1f );   // the tick after a load carries the whole load time
			if( GPspAutoWalkDelay <= 0.f ) { GPspAutoWalkLeft = GPspAutoWalkPending; GPspAutoWalkPending = 0.f; GPspAutoWalkT0 = appSeconds(); debugf( NAME_Log, "PSPTEST: autowalk starts" ); }
		}
		// Every frame slower than 60 ms while the walk runs (and 20 s after): real
		// wall-clock time, not the engine's clamped DeltaSeconds.
		{
			static DOUBLE LastT = 0.0; const DOUBLE Now = appSeconds();
			extern CORE_API INT GPspFileRefills, GPspFileRefillBytes, GPspFileReopens, GPspFileSeeks; extern CORE_API SQWORD GPspFileIoCycles;
			static INT Script0 = 0, ActorT0 = 0, Move0 = 0, NumMoves0 = 0, Spawn0 = 0, See0 = 0, Path0 = 0;
			static SQWORD Io0 = 0; static DWORD Pre0 = 0, RelC0 = 0, SndC0 = 0, TickC0 = 0, DrawC0 = 0, AudC0 = 0; static INT Refills0 = 0, Bytes0 = 0, Preloads0 = 0, Reloads0 = 0, Uploads0 = 0, Sounds0 = 0, Mesh0 = 0, Reopens0 = 0, Seeks0 = 0;
			if( LastT > 0.0 && GPspAutoWalkT0 > 0.0 && Now - GPspAutoWalkT0 < 60.0 && Now - LastT > 0.06 && GLevel )
			{
				for( INT i = 0; i < GLevel->Num(); i++ )
				{
					APlayerPawn* P = Cast<APlayerPawn>( GLevel->Element(i) );
					if( P && P->Player )
					{
						char Top[256] = "";
#ifdef PSP_KEEP_UCLOCK
						extern ENGINE_API void PspTickTop( char* Out, INT Max ); PspTickTop( Top, 256 );
#endif
						debugf( NAME_Log, "PSPTEST: slow frame %.0f ms at t+%.1f s, pawn (%.0f,%.0f,%.0f) physics %i | stick %.0f ms (%i reads %i KB, %i reopens %i seeks) linker %.0f ms (%i preloads) reloads %i (%.0f ms) uploads %i sounds %i (%.0f ms in RegisterSound) meshreload +%i KB | tick %.0f ms draw %.0f ms audio %.0f ms | script %.0f actors %.0f move %.0f (%i) spawn %.0f see %.0f path %.0f ms | top: %s",
							(FLOAT)( ( Now - LastT ) * 1000.0 ), (FLOAT)( Now - GPspAutoWalkT0 ), P->Location.X, P->Location.Y, P->Location.Z, (INT)P->Physics,
							(FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspFileIoCycles - Io0 ) ), GPspFileRefills - Refills0, ( GPspFileRefillBytes - Bytes0 ) / 1024, GPspFileReopens - Reopens0, GPspFileSeeks - Seeks0,
							(FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspPreloadCycles - Pre0 ) ), GPspCtrPreloads - Preloads0,
							GPspCtrReloads - Reloads0, (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspReloadCycles - RelC0 ) ), GPspCtrUploads - Uploads0, GPspCtrSounds - Sounds0, (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspSoundRegCycles - SndC0 ) ), GPspMeshReloadKB - Mesh0,
							(FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspTickCycles - TickC0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspDrawCycles - DrawC0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GPspAudioCycles - AudC0 ) ),
							(FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GScriptCycles - Script0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GLevel->ActorTickCycles - ActorT0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GLevel->MoveCycles - Move0 ) ), GLevel->NumMoves - NumMoves0,
							(FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GLevel->Spawning - Spawn0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GLevel->SeePlayer - See0 ) ), (FLOAT)( GSecondsPerCycle * 1000.0 * (DOUBLE)( GLevel->FindPathCycles - Path0 ) ), Top );
						break;
					}
				}
			}
			LastT = Now;
			Io0 = GPspFileIoCycles; Refills0 = GPspFileRefills; Bytes0 = GPspFileRefillBytes; Pre0 = GPspPreloadCycles; Preloads0 = GPspCtrPreloads;
			Reloads0 = GPspCtrReloads; RelC0 = GPspReloadCycles; Uploads0 = GPspCtrUploads; Sounds0 = GPspCtrSounds; Mesh0 = GPspMeshReloadKB; Reopens0 = GPspFileReopens; Seeks0 = GPspFileSeeks;
			SndC0 = GPspSoundRegCycles; TickC0 = GPspTickCycles; DrawC0 = GPspDrawCycles; AudC0 = GPspAudioCycles;
			if( GLevel ) { Script0 = GScriptCycles; ActorT0 = GLevel->ActorTickCycles; Move0 = GLevel->MoveCycles; NumMoves0 = GLevel->NumMoves; Spawn0 = GLevel->Spawning; See0 = GLevel->SeePlayer; Path0 = GLevel->FindPathCycles; }
		}
		if( GPspAutoWalkLeft > 0.f )
		{
			GPspAutoWalkLeft -= Min( DeltaSeconds, 0.1f );
			static FLOAT Trace = 0.f; Trace += DeltaSeconds;
			if( Trace >= 1.f && GLevel )
			{
				Trace = 0.f;
				for( INT i = 0; i < GLevel->Num(); i++ )
				{
					APlayerPawn* P = Cast<APlayerPawn>( GLevel->Element(i) );
					if( P && P->Player )
					{
						ALevelInfo* LI = GLevel->GetLevelInfo();
						debugf( NAME_Log, "PSPTEST: pawn at (%.0f,%.0f,%.0f) vel %.0f physics %i state %s pauser '%s' timedil %.2f showmenu %i", P->Location.X, P->Location.Y, P->Location.Z, P->Velocity.Size(),
							(INT)P->Physics, P->GetMainFrame() && P->GetMainFrame()->StateNode ? P->GetMainFrame()->StateNode->GetName() : "?", *LI->Pauser, LI->TimeDilation, (INT)P->bShowMenu );
						break;
					}
				}
			}
			// Raw driver cost in the middle of the walk (t+4 s and t+7 s).
			static INT Bench = 0;
			if( ( Bench == 0 && appSeconds() - GPspAutoWalkT0 > 4.0 ) || ( Bench == 1 && appSeconds() - GPspAutoWalkT0 > 7.0 ) )
			{
				++Bench; appPspIoBench( "../Sounds/Ambmodern.uax" ); appPspIoBench( "../System/UnrealI.u" );
			}
			if( GPspAutoWalkLeft <= 0.f ) debugf( NAME_Log, "PSPTEST: autowalk finished" );
		}
	}
	// -SAVETEST=secs (test hook): save to slot 9 after N seconds in a map,
	// load it back at 2N. Exercises the menu's SaveGame / ?load= paths.
	{
		static INT Test = -1; static FLOAT T = 0.f; static INT Stage = 0;
		if( Test < 0 ) { Test = 0; Parse( appCmdLine(), "SAVETEST=", Test ); }
		if( Test > 0 && GLevel && Stage < 2 )
		{
			T += DeltaSeconds;
			if( Stage == 0 && T >= Test )       { Stage = 1; debugf( NAME_Log, "PSPTEST: SAVEGAME 9" ); Exec( "SAVEGAME 9", GSystem ); }
			else if( Stage == 1 && T >= 2 * Test ) { Stage = 2; debugf( NAME_Log, "PSPTEST: START ?load=9" ); Exec( "START ?load=9", GSystem ); }
		}
	}
#endif
	if( GLevel && *GLevel->GetLevelInfo()->NextURL )
	{
		if( (GLevel->GetLevelInfo()->NextSwitchCountdown-=DeltaSeconds) <= 0.0 )
		{
			// Travel to new level, and exit.
			TArray<FString> TravelNames;
			TArray<FString>	TravelItems;
			for( INT i=0; i<GLevel->Num(); i++ )
			{
				APlayerPawn* P = Cast<APlayerPawn>( GLevel->Element(i) );
				if( P && P->Player )
				{
					P->Player->TravelItems="";
					if( Cast<UNetConnection>(P->Player) )
						SetClientTravel( P->Player, GLevel->GetLevelInfo()->NextURL, 1, GLevel->GetLevelInfo()->bNextItems, TRAVEL_Relative );
					if( Cast<UViewport>( P->Player ) )
						Cast<UViewport>( P->Player )->TravelURL = "";
					new(TravelNames)FString(P->PlayerName);
					new(TravelItems)FString(P->Player->TravelItems);
				}
			}
			debugf( "Server switch level: %s", GLevel->GetLevelInfo()->NextURL );
			char Error256[256];
			Browse( FURL(&LastURL,GLevel->GetLevelInfo()->NextURL,TRAVEL_Relative), Error256 );
			*GLevel->GetLevelInfo()->NextURL = 0;
			GLevel->TravelNames = TravelNames;
			GLevel->TravelItems = TravelItems;
			return;
		}
	}
	unguard;

	// Handle client travelling.
	guard(ClientTravel);
	if( Client && Client->Viewports.Num() && Client->Viewports(0)->TravelURL!="" )
	{
		// Travel to new level, and exit.
		FString NextURL = Client->Viewports(0)->TravelURL;
		Client->Viewports(0)->TravelURL="";
		char Error256[256];
		Browse( FURL(&LastURL,*NextURL,Client->Viewports(0)->TravelType), Error256 );
		return;
	}
	unguard;

	// Update the pending level.
	guard(TickPending);
	if( GPendingLevel )
	{
		GPendingLevel->Tick( DeltaSeconds );
		if( GPendingLevel && GPendingLevel->Error256[0] )
		{
			// Pending connect failed.
			guard(PendingFailed);
			FString Str;
			GPendingLevel->URL.String( Str );
			debugf( NAME_Log, LocalizeError("Pending"), *Str, GPendingLevel->Error256 );
			delete GPendingLevel;
			GPendingLevel = NULL;
			//!!should convey this failure to the user by an in-game message.
			unguard;
		}
		else if( GPendingLevel->Success && !GPendingLevel->FilesNeeded && !GPendingLevel->SentJoin )
		{
			// Attempt to load the map.
			char Error256[256];
			guard(AttemptLoadPending);
			LoadMap( GPendingLevel->URL, GPendingLevel, Error256 );
			if( Error256[0] )
			{
				//!!report the error.
			}
			else if( !GPendingLevel->LonePlayer )
			{
				GPendingLevel->SentJoin = 1;
				GPendingLevel->NetDriver->ServerConnection->Logf( "JOIN" );
				GPendingLevel->NetDriver->ServerConnection->FlushNet();
				GPendingLevel->NetDriver = NULL;
				GLevel->GetLevelInfo()->LevelAction = LEVACT_Connecting;
				GEntry->GetLevelInfo()->LevelAction = LEVACT_Connecting;
			}
			unguard;

			// Kill the pending level.
			guard(KillPending);
			delete GPendingLevel;
			GPendingLevel = NULL;
			unguard;
		}
	}
	unguard;

	// Render everything.
	guard(ClientTick);
	INT LocalClientCycles=0;
	if( Client )
	{
		uclock(LocalClientCycles);
		const DWORD PspC0 = appCycles();
		Client->Tick();
		GPspDrawCycles += (DWORD)( appCycles() - PspC0 );
		uunclock(LocalClientCycles);
	}
	ClientCycles=LocalClientCycles;
	unguard;

	uunclock(LocalTickCycles);
	TickCycles=LocalTickCycles;
	GTicks++;
	unguard;
}

/*-----------------------------------------------------------------------------
	Saving the game.
-----------------------------------------------------------------------------*/

//
// Save the current game state to a file.
//
void UGameEngine::SaveGame( INT Position )
{
	guard(UGameEngine::SaveGame);
	char Filename[256];
	appMkdir( PATH(GSys->SavePath) );
	appSprintf( Filename, "%s" PATH_SEPARATOR "Save%i.usa", PATH(GSys->SavePath), Position );
	GLevel->GetLevelInfo()->LevelAction=LEVACT_Saving;
	PaintProgress();
	GSystem->BeginSlowTask( LocalizeProgress("Saving"), 1, 0 );
	if( GLevel->BrushTracker )
	{
		GLevel->BrushTracker->Exit();
		delete GLevel->BrushTracker;
	}
	GLevel->CleanupDestroyed( 1 );
#ifdef __PSP__
	PspReloadFreedForSave( GLevel->GetParent() );
#endif
	if( GObj.SavePackage( GLevel->GetParent(), GLevel, 0, Filename ) )
	{
		// Copy the hub stack.
		INT i;
		for( i=0; i<GLevel->GetLevelInfo()->HubStackLevel; i++ )
		{
			char Src[256], Dest[256];
			appSprintf( Src, "%s" PATH_SEPARATOR "Game%i.usa", PATH(GSys->SavePath), i );
			appSprintf( Dest, "%s" PATH_SEPARATOR "Save%i%i.usa", PATH(GSys->SavePath), Position, i );
			appCopyFile( Src, Dest );
		}
		while( 1 )
		{
			appSprintf( Filename, "%s" PATH_SEPARATOR "Save%i%i.usa", PATH(GSys->SavePath), Position, i++ );
			if( appFSize(Filename)<=0 )
				break;
			appUnlink( Filename );
		}
	}
	for( INT i=0; i<GLevel->Num(); i++ )
		if( Cast<AMover>(GLevel->Actors(i)) )
			Cast<AMover>(GLevel->Actors(i))->SavedPos = FVector(-1,-1,-1);
	GLevel->BrushTracker = GNewBrushTracker( GLevel );
	GSystem->EndSlowTask();
	GLevel->GetLevelInfo()->LevelAction=LEVACT_None;

	unguard;
}

/*-----------------------------------------------------------------------------
	Mouse feedback.
-----------------------------------------------------------------------------*/

//
// Mouse delta while dragging.
//
void UGameEngine::MouseDelta( UViewport* Viewport, DWORD ClickFlags, FLOAT DX, FLOAT DY )
{
	guard(UGameEngine::MouseDelta);
	if( (ClickFlags & MOUSE_FirstHit) && Client && Client->Viewports.Num()==1 && GLevel && !Client->FullscreenViewport && GLevel->GetLevelInfo()->Pauser[0]==0 && !Viewport->Actor->bShowMenu )
	{
		Viewport->SetMouseCapture( 1, 1, 1 );
	}
	else if( (ClickFlags & MOUSE_LastRelease) && !Client->CaptureMouse )
	{
		Viewport->SetMouseCapture( 0, 0 );
	}
	unguard;
}

//
// Absolute mouse position.
//
void UGameEngine::MousePosition( UViewport* Viewport, DWORD ClickFlags, FLOAT X, FLOAT Y )
{
	guard(UGameEngine::MousePosition);
	unguard;
}

//
// Mouse clicking.
//
void UGameEngine::Click( UViewport* Viewport, DWORD ClickFlags, FLOAT X, FLOAT Y )
{
	guard(UGameEngine::Click);
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
