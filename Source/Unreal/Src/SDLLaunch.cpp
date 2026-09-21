#include "SDL2/SDL.h"
#ifdef PLATFORM_WIN32
#include <windows.h>
#endif
#ifdef PLATFORM_PSVITA
#include <vitasdk.h>
#include <vitaGL.h>
#include <unistd.h>
#endif
#ifdef __PSP__
#include <pspkernel.h>
#include <psppower.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pspsdk.h>
#include <psputils.h>
#include <exception>
#include <new>
extern void PspLogSync();
#endif

#include "Engine.h"

extern CORE_API FGlobalPlatform GTempPlatform;
extern DLL_IMPORT UBOOL GTickDue;
extern "C" {HINSTANCE hInstance;}
extern "C" {char GCC_HIDDEN THIS_PACKAGE[64]="Launch";}

// FExecHook.
class FExecHook : public FExec
{
	UBOOL Exec( const char* Cmd, FOutputDevice* Out )
	{
		return 0;
	}
};

FExecHook GLocalHook;
DLL_EXPORT FExec* GThisExecHook = &GLocalHook;

#ifdef PLATFORM_PSVITA

//
// PSVita-specific globals.
//

#define MAX_PATH 1024
#define SYSTEM_PATH "data/unreal/System/"

// 200MB libc heap, 512K main thread stack, 16MB for loading game DLLs
// the rest goes to vitaGL
extern "C" { SceUInt32 sceUserMainThreadStackSize = 512 * 1024; }
extern "C" { unsigned int _pthread_stack_default_user = 512 * 1024; }
extern "C" { unsigned int _newlib_heap_size_user = 200 * 1024 * 1024; }
#define VGL_MEM_THRESHOLD ( 4 * 1024 * 1024 )

static char GRootPath[MAX_PATH] = "app0:/";

//
// PSVita-specific functions.
//

static bool FindRootPath( char* Out, int OutLen )
{
	static const char *Drives[] = { "uma0", "imc0", "ux0" };

	// check if an unreal folder exists on one of the drives
	// default to the last one (ux0)
	for ( unsigned int i = 0; i < sizeof(Drives) / sizeof(*Drives); ++i )
	{
		snprintf( Out, OutLen, "%s:/" SYSTEM_PATH, Drives[i] );
		SceUID Dir = sceIoDopen( Out );
		if ( Dir >= 0 )
		{
			sceIoDclose( Dir );
			return true;
		}
	}

	// not found
	return false;
}

static INT PowerCallback( INT NotifyID, INT NotifyCnt, INT PowerInfo, void* Common )
{
	if ( PowerInfo & ( SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_APP_RESUMING ) )
	{
		debugf( "PowerCallback: resuming..." );
		appHandleSuspendResume( false );
	}
	else if ( PowerInfo & ( SCE_POWER_CB_BUTTON_PS_PRESS | SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND ) )
	{
		debugf( "PowerCallback: suspending..." );
		appHandleSuspendResume( true );
	}

	return 0;
}

static INT CallbackThread( DWORD Argc, void* Argv )
{
	const INT CbID = sceKernelCreateCallback( "Power Callback", 0, PowerCallback, nullptr );
	scePowerRegisterCallback( CbID );
	while( true )
		sceKernelDelayThreadCB( 10000000 );
	return 0;
}

[[noreturn]] static void EarlyError( const char* Msg )
{
	fprintf( stderr, "FATAL ERROR: %s\n", Msg );
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "Fatal Error", Msg, nullptr );
	sceKernelExitProcess( 0 );
	abort();
}

static void PlatformPreInit()
{
	sceTouchSetSamplingState( SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP );
	scePowerSetArmClockFrequency( 444 );
	scePowerSetBusClockFrequency( 222 );
	scePowerSetGpuClockFrequency( 222 );
	scePowerSetGpuXbarClockFrequency( 166 );
	sceSysmoduleLoadModule( SCE_SYSMODULE_NET );

	if ( !FindRootPath( GRootPath, sizeof(GRootPath) ) )
		EarlyError( "Could not find Unreal directory" );

	if ( chdir( GRootPath ) < 0 )
		EarlyError( "Could not chdir to Unreal directory" );

	SceUID Th = sceKernelCreateThread( "CallbackThread", CallbackThread, 0x10000100, 0x10000, 0, 0, nullptr );
	if( Th >= 0 )
		sceKernelStartThread( Th, 0, nullptr );
	
	vglSetParamBufferSize(6 * 1024 * 1024);
	vglSetCircularPoolSize(3 * 1024 * 1024);
	vglInitWithCustomThreshold( 0, 960, 544, VGL_MEM_THRESHOLD, 0, 0, 0, SCE_GXM_MULTISAMPLE_4X );
}

#elif defined(__PSP__)

//
// PSP-specific globals.
//

#define MAX_PATH 1024

// UE1's renderer recurses deeply, and FOutputDevice::Logf drops a 4KB TempStr
// buffer on the stack at the bottom of that recursion. The PSP's default main
// thread stack is nowhere near enough -- it manifests as "CPU Jump to 00000000"
// with the PC inside Logf. The Vita port raises its stack for the same reason.
// Declared extern "C" so the name is not mangled; pspsdk looks it up by symbol.
// 4MB. Package loading recurses: Entry -> UnrealI -> IpDrv, each level a large
// ULinkerLoad constructor, and FOutputDevice::Logf puts a 4KB buffer on the
// stack at every level. Hardware dies at UnrealI's import 324 of 722
// ('ClientBeaconReceiver'), which is exactly where the nested IpDrv load
// begins. 1MB was not enough.
extern "C" { unsigned int sce_newlib_stack_kb_size = 4096; }

// sce_newlib_heap_kb_size is a weak symbol in the SDK, so without this we get
// whatever pspsdk's default is rather than an explicit choice. A negative value
// means "all available memory except this many KB", leaving a little for the
// allocations pspgl and the kernel make outside the newlib heap.
// Negative means "all available memory except this many KB", leaving a little
// for allocations pspgl and the kernel make outside the newlib heap.
//
// MEASURED: this yields heap 23.6MB + kernel free 0.76MB = ~24.4MB total, which
// is a 32MB machine's user partition -- the PSP-2000's extra 32MB is NOT being
// granted despite MEMSIZE=1 in PARAM.SFO. Unreal exhausts this and dies in
// appMalloc's check(Ptr). Getting the extra RAM is the main open problem.
extern "C" { int sce_newlib_heap_kb_size = -1024; }

// The engine expects to run from inside System/, exactly as Unreal.exe does on
// PC -- Unreal.ini's search paths are written relative to it ("..\System\*.u",
// "..\Maps\*.unr"). Pointing this at the game root instead makes every package
// lookup miss and InitEngine() fail in LoadClass. The Vita build does the same
// thing via its SYSTEM_PATH ending in "/System/".
#define SYSTEM_PATH "PSP/GAME/Unreal/System/"
static char GRootPath[MAX_PATH] = "ms0:/" SYSTEM_PATH;

// NOTE: a pspDebugInstallErrorHandler() crash handler was tried here and does
// NOT work. It pulls in sceKernelRegisterDefaultExceptionHandler and
// sceKernelRegisterSubIntrHandler, which are kernel-mode only, and a user-mode
// EBOOT importing them is refused by the loader with 8002013C ("The game could
// not be started"). Catching MIPS exceptions would need a kernel-mode module.

[[noreturn]] static void PspEarlyError( const char* Msg )
{
	fprintf( stderr, "FATAL ERROR: %s\n", Msg );
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "Fatal Error", Msg, nullptr );
	sceKernelExitGame();
	abort();
}

//
// Catch-alls for the ways a PSP process can die without writing anything.
// pspDebugInstallErrorHandler is not usable here (it imports kernel-mode
// symbols, so the EBOOT is rejected with 8002013C), which leaves the C++
// runtime's own exits as the only ones we can still get a line out of:
//   - an appThrowf the toolchain cannot unwind lands in terminate()
//   - a failed appMalloc lands in the new-handler
//   - anything calling exit()/abort() lands in the atexit hook
// Without these, all three look identical to a hardware fault: black screen.
//
static void PspOnTerminate()
{
	debugf( "PSPDEATH: std::terminate -- unhandled/unwindable exception" );
	PspLogSync();
	sceKernelExitGame();
}

static void PspOnBadAlloc()
{
	debugf( "PSPDEATH: operator new failed (out of memory)" );
	PspLogSync();
	sceKernelExitGame();
}

static void PspOnExit()
{
	debugf( "PSPDEATH: exit()/abort() reached" );
	PspLogSync();
}

void PlatformPreInit()
{
	// UE1 divides by zero and underflows freely -- normalising a zero vector,
	// dividing by a zero delta -- which is harmless when the FPU quietly
	// produces inf/NaN. The PSP's FPU traps on inexact/underflow/divide-by-zero
	// instead, so those become fatal exceptions. Mask them.
	pspSdkDisableFPUExceptions();

	std::set_terminate( PspOnTerminate );
	std::set_new_handler( PspOnBadAlloc );
	atexit( PspOnExit );

	// The PSP starts homebrew at full speed only if asked; the default is
	// 222MHz, and Unreal needs every cycle available.
	scePowerSetClockFrequency( 333, 333, 166 );

	// The working directory at launch is not guaranteed to be the EBOOT's own
	// directory, so pin it explicitly before the engine starts resolving
	// relative paths like "System/Unreal.ini".
	if ( chdir( GRootPath ) < 0 )
		PspEarlyError( "Could not chdir to the Unreal directory" );
}

#else

void PlatformPreInit()
{

}

#endif


//
// Handle an error.
//
void HandleError()
{
	GIsGuarded=0;
	GIsCriticalError=1;
	debugf( NAME_Exit, "Shutting down after catching exception" );
	GObj.ShutdownAfterError();
	debugf( NAME_Exit, "Exiting due to exception" );
	GErrorHist[ARRAY_COUNT(GErrorHist)-1]=0;
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, LocalizeError("Critical"), GErrorHist, SDL_GetKeyboardFocus() );
}

//
// Initialize.
//
UEngine* InitEngine()
{
	guard(InitEngine);

	// Platform init.
	appInit();
	GDynMem.Init( 65536 );

	// Init subsystems.
	GSceneMem.Init( 32768 );

	// First-run menu.
	UBOOL FirstRun=0;
	GetConfigBool( "FirstRun", "FirstRun", FirstRun );

	// Create the global engine object.
	UClass* EngineClass;
	if( !GIsEditor )
	{
		// Create game engine.
		EngineClass = GObj.LoadClass( UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}
	else if( ParseParam( appCmdLine(),"MAKE" ) )
	{
		// Create editor engine.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_DisallowFiles | LOAD_KeepImports, NULL );
	}
	else
	{
		// Editor.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}

	// Init engine.
	UEngine* Engine = ConstructClassObject<UEngine>( EngineClass );
	Engine->Init();

	return Engine;

	unguard;
}

//
// Unreal's main message loop.  All windows in Unreal receive messages
// somewhere below this function on the stack.
//
void MainLoop( UEngine* Engine )
{
	guard(MainLoop);

	GIsRunning = 1;
	DOUBLE OldTime = appSeconds();
	while( GIsRunning && !GIsRequestingExit )
	{
		// Update the world.
		DOUBLE NewTime = appSeconds();
#ifdef __PSP__
		// TEMPORARY DIAGNOSTIC -- if these keep printing while Lock() has
		// stopped, the main loop is alive but nothing is being drawn. If they
		// stop too, the loop itself is blocked inside Tick().
		{
			static INT TickCount = 0;
			if( TickCount < 3 || ( TickCount % 10 ) == 0 )
				debugf( NAME_Log, "PSPDIAG: Tick #%d dt=%f", TickCount, (FLOAT)(NewTime - OldTime) );
			++TickCount;
		}
#endif
		Engine->Tick( NewTime - OldTime );
		OldTime = NewTime;

		// Enforce optional maximum tick rate.
		INT MaxTickRate = Engine->GetMaxTickRate();
#ifdef __PSP__
		// UGameEngine::GetMaxTickRate() returns 0 in single player -- it only
		// caps for network games -- so the limiter below never runs. Frame
		// times here swing between ~37ms and ~200ms, which reads as very uneven
		// even though the fast stretches are fine. Capping trades the peaks for
		// a steadier rate and, just as importantly, gives the simulation a
		// consistent DeltaSeconds.
		//
		// Note this cannot speed up the slow stretches; it only stops the fast
		// ones running away. Set [PSP] MaxFPS=0 in Unreal.ini to disable.
		{
			static INT PspMaxFPS = -1;
			if( PspMaxFPS < 0 )
			{
				PspMaxFPS = 20;
				GetConfigInt( "PSP", "MaxFPS", PspMaxFPS );
				debugf( NAME_Log, "PSPPERF: frame cap = %d fps", PspMaxFPS );
			}
			if( PspMaxFPS > 0 )
				MaxTickRate = PspMaxFPS;
		}
#endif
		if( MaxTickRate )
		{
			DOUBLE Delta = (1.0/MaxTickRate) - (appSeconds()-OldTime);
			if( Delta > 0.0 )
				appSleep( Delta );
		}
	}
	GIsRunning = 0;
	unguard;
}

//
// Exit the engine.
//
void ExitEngine( UEngine* Engine )
{
	guard(ExitEngine);

	GObj.Exit();
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit(1);
	appDumpAllocs( &GTempPlatform );

	unguard;
}

#ifdef PLATFORM_WIN32
INT WINAPI WinMain( HINSTANCE hInInstance, HINSTANCE hPrevInstance, char* InCmdLine, INT nCmdShow )
#elif defined(__PSP__)
// On PSP the entry point goes through SDL2main, where SDL_main.h does
// `#define main SDL_main` and declares `extern "C" int SDL_main(int, char*[])`.
// Declaring const char** here would define a differently-mangled symbol that
// SDL2main's own main() cannot find, so match SDL's signature exactly.
int main( int argc, char** argv )
#else
int main( int argc, const char** argv )
#endif
{
#ifdef PLATFORM_WIN32
	hInstance = hInInstance;
#else
	hInstance = NULL;
	// Remember arguments since we don't have GetCommandLine().
	appSetCmdLine( argc, (const char**)argv );
	PlatformPreInit();
#endif

	GIsStarted = 1;

	// Set package name.
	appStrcpy( THIS_PACKAGE, appPackage() );

	// Init mode.
	GIsServer = 1;
	GIsClient = !ParseParam(appCmdLine(),"SERVER") && !ParseParam(appCmdLine(),"MAKE");
	GIsEditor = ParseParam(appCmdLine(),"EDITOR") || ParseParam(appCmdLine(),"MAKE");

	// Init windowing.
	appChdir( appBaseDir() );

	// Init log.
	// TODO: GLog
	GExecHook = GThisExecHook;

	// Begin.
#ifndef _DEBUG
	try
	{
#endif
		// Start main loop.
		GIsGuarded=1;
		GSystem = &GTempPlatform;
		UEngine* Engine = InitEngine();
		if( !GIsRequestingExit )
			MainLoop( Engine );
		ExitEngine( Engine );
		GIsGuarded=0;
#ifndef _DEBUG
	}
	catch( ... )
	{
		// Crashed.
		try {HandleError();} catch( ... ) {}
	}
#endif

	// Shut down.
	GExecHook=NULL;
	appExit();
	GIsStarted = 0;
	return 0;
}
