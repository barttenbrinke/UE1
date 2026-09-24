#include <stdlib.h>

#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#ifndef PSP_NO_EFX  // PSP OpenAL implements EFX but ships no efx.h header
#include "AL/efx.h"
#include "AL/efx-presets.h"
#endif
#include "xmp.h"

#include "NOpenALDrvPrivate.h"

#ifdef __PSP__
#include <pspaudio.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <malloc.h>
//
// Pre-rendered music on a hardware channel. The tracker mixer (17 channels
// for the intro's flyby.umx) is CPU the PSP does not have, so install-psp.sh
// renders every .umx to Music/<song>.wav (11025 Hz mono 16-bit, xmp) and this
// streams it: a high-priority thread reads a block, upsamples it 4x with
// linear interpolation to the channel's 44100 Hz, and hands it to
// sceAudioOutputBlocking, which the hardware mixes with OpenAL's channel for
// free. Loops at end of file. Song sections are ignored (the file is the
// whole module in order). Without the file, libxmp plays as before.
//
enum { PSP_MUS_FRAMES = 2048, PSP_MUS_OUTRATE = 44100 };
static SceUID GPspMusFd = -1, GPspMusThread = -1;
static int    GPspMusChan = -1, GPspMusRatio = 4, GPspMusDataStart = 0;
static volatile int GPspMusRun = 0, GPspMusPlaying = 0, GPspMusVol = 0, GPspMusRewind = 0;
static short* GPspMusSrc = NULL;
static short* GPspMusOut = NULL;
static UBOOL  GPspMusStreaming = 0;

static int PspMusThreadProc( SceSize, void* )
{
	short Last = 0;
	while( GPspMusRun )
	{
		if( !GPspMusPlaying )
		{
			sceKernelDelayThread( 20000 );
			continue;
		}
		if( GPspMusRewind )
		{
			GPspMusRewind = 0;
			sceIoLseek32( GPspMusFd, GPspMusDataStart, PSP_SEEK_SET );
		}
		const int Want = PSP_MUS_FRAMES * 2;
		int Got = sceIoRead( GPspMusFd, GPspMusSrc, Want );
		if( Got < 0 ) Got = 0;
		if( Got < Want )
		{
			// End of file: wrap and top the block up from the start.
			sceIoLseek32( GPspMusFd, GPspMusDataStart, PSP_SEEK_SET );
			int More = sceIoRead( GPspMusFd, (BYTE*)GPspMusSrc + Got, Want - Got );
			if( More > 0 ) Got += More;
			if( Got < Want ) appMemset( (BYTE*)GPspMusSrc + Got, 0, Want - Got );
		}
		short* O = GPspMusOut;
		for( int i = 0; i < PSP_MUS_FRAMES; ++i )
		{
			const int S = GPspMusSrc[i];
			for( int k = 1; k <= GPspMusRatio; ++k )
				*O++ = (short)( Last + ( S - Last ) * k / GPspMusRatio );
			Last = (short)S;
		}
		sceAudioOutputBlocking( GPspMusChan, GPspMusVol, GPspMusOut );
	}
	return 0;
}

static void PspMusClose()
{
	if( GPspMusThread >= 0 )
	{
		GPspMusRun = 0;
		sceKernelWaitThreadEnd( GPspMusThread, NULL );
		sceKernelDeleteThread( GPspMusThread );
		GPspMusThread = -1;
	}
	if( GPspMusChan >= 0 ) { sceAudioChRelease( GPspMusChan ); GPspMusChan = -1; }
	if( GPspMusFd >= 0 )   { sceIoClose( GPspMusFd ); GPspMusFd = -1; }
	GPspMusStreaming = 0;
	GPspMusPlaying   = 0;
}

// Music/<Name>.wav next to System/. Returns 1 and takes over playback when the
// file exists and is 16-bit mono PCM at a rate that divides 44100.
static UBOOL PspMusOpen( const char* Name, INT Volume255 )
{
	PspMusClose();
	GPspMusVol = Volume255 * PSP_AUDIO_VOLUME_MAX / 255;   // set here too; the fade path updates it later
	char Path[300];
	appStrncpy( Path, appBaseDir(), sizeof(Path) );            // ".../Unreal/System/"
	INT L = appStrlen( Path );
	if( L > 7 && appStricmp( Path + L - 7, "System/" ) == 0 ) Path[L-7] = 0;
	appStrncat( Path, "Music/", sizeof(Path) - 1 );
	appStrncat( Path, Name, sizeof(Path) - 1 );
	appStrncat( Path, ".wav", sizeof(Path) - 1 );
	SceUID Fd = sceIoOpen( Path, PSP_O_RDONLY, 0777 );
	if( Fd < 0 )
		return 0;

	// RIFF walk for "fmt " and "data".
	static BYTE Hdr[512] __attribute__((aligned(64)));
	int N = sceIoRead( Fd, Hdr, sizeof(Hdr) );
	if( N < 44 || appMemcmp( Hdr, "RIFF", 4 ) || appMemcmp( Hdr + 8, "WAVE", 4 ) ) { sceIoClose( Fd ); return 0; }
	int Rate = 0, Channels = 0, Bits = 0, DataAt = 0, O = 12;
	while( O + 8 <= N )
	{
		DWORD Len; appMemcpy( &Len, Hdr + O + 4, 4 );
		if( !appMemcmp( Hdr + O, "fmt ", 4 ) )
		{
			_WORD w; appMemcpy( &w, Hdr + O + 10, 2 ); Channels = w;
			DWORD d; appMemcpy( &d, Hdr + O + 12, 4 ); Rate = d;
			appMemcpy( &w, Hdr + O + 22, 2 ); Bits = w;
		}
		else if( !appMemcmp( Hdr + O, "data", 4 ) ) { DataAt = O + 8; break; }
		O += 8 + ( ( Len + 1 ) & ~1 );
	}
	if( !DataAt || Channels != 1 || Bits != 16 || Rate <= 0 || ( PSP_MUS_OUTRATE % Rate ) != 0 )
	{
		debugf( NAME_Warning, "PSPMUSIC: %s is not 16-bit mono PCM at a rate dividing 44100 (ch %i bits %i rate %i)", Path, Channels, Bits, Rate );
		sceIoClose( Fd );
		return 0;
	}
	GPspMusRatio     = PSP_MUS_OUTRATE / Rate;
	GPspMusDataStart = DataAt;
	sceIoLseek32( Fd, DataAt, PSP_SEEK_SET );
	if( !GPspMusSrc ) GPspMusSrc = (short*)memalign( 64, PSP_MUS_FRAMES * 2 );
	if( !GPspMusOut ) GPspMusOut = (short*)memalign( 64, PSP_MUS_FRAMES * 2 * 8 );   // room for ratio up to 8
	GPspMusChan = sceAudioChReserve( PSP_AUDIO_NEXT_CHANNEL, PSP_MUS_FRAMES * GPspMusRatio, PSP_AUDIO_FORMAT_MONO );
	if( !GPspMusSrc || !GPspMusOut || GPspMusChan < 0 )
	{
		debugf( NAME_Warning, "PSPMUSIC: no hardware channel (%i)", GPspMusChan );
		GPspMusChan = -1; sceIoClose( Fd ); return 0;
	}
	GPspMusFd  = Fd;
	GPspMusRun = 1; GPspMusPlaying = 0; GPspMusRewind = 0;
	// Above the main thread (which SDLLaunch lowers further) so long frames
	// never starve it; the block output blocks, so it costs nothing otherwise.
	GPspMusThread = sceKernelCreateThread( "psp_music", PspMusThreadProc, 0x10, 16 * 1024, THREAD_ATTR_USER, NULL );
	if( GPspMusThread < 0 || sceKernelStartThread( GPspMusThread, 0, NULL ) < 0 )
	{
		debugf( NAME_Warning, "PSPMUSIC: thread failed (%i)", GPspMusThread );
		GPspMusThread = -1; PspMusClose(); return 0;
	}
	GPspMusStreaming = 1;
	debugf( NAME_Log, "PSPMUSIC: streaming %s (%i Hz x%i) on hardware channel %i", Path, Rate, GPspMusRatio, GPspMusChan );
	return 1;
}
#endif
#include "UnRender.h"

/*-----------------------------------------------------------------------------
	Global implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_PACKAGE(NOpenALDrv);
IMPLEMENT_CLASS(UNOpenALAudioSubsystem);

/*-----------------------------------------------------------------------------
	UNOpenALAudioSubsystem implementation.
-----------------------------------------------------------------------------*/

void UNOpenALAudioSubsystem::InternalClassInitializer( UClass* Class )
{
	guardSlow(UNOpenALAudioSubsystem::InternalClassInitializer);
	new(Class, "DeviceName",         RF_Public)UStringProperty( CPP_PROPERTY( DeviceName         ), "Audio", CPF_Config, sizeof(DeviceName)-1 );
	new(Class, "OutputRate",         RF_Public)UIntProperty   ( CPP_PROPERTY( OutputRate         ), "Audio", CPF_Config );
	new(Class, "MusicVolume",        RF_Public)UByteProperty  ( CPP_PROPERTY( MusicVolume        ), "Audio", CPF_Config );
	new(Class, "SoundVolume",        RF_Public)UByteProperty  ( CPP_PROPERTY( SoundVolume        ), "Audio", CPF_Config );
	new(Class, "MasterVolume",       RF_Public)UByteProperty  ( CPP_PROPERTY( MasterVolume       ), "Audio", CPF_Config );
	new(Class, "AmbientFactor",      RF_Public)UFloatProperty ( CPP_PROPERTY( AmbientFactor      ), "Audio", CPF_Config );
	new(Class, "DopplerFactor",      RF_Public)UFloatProperty ( CPP_PROPERTY( DopplerFactor      ), "Audio", CPF_Config );
	new(Class, "UseReverb",          RF_Public)UBoolProperty  ( CPP_PROPERTY( UseReverb          ), "Audio", CPF_Config );
	new(Class, "UseHRTF",            RF_Public)UBoolProperty  ( CPP_PROPERTY( UseHRTF            ), "Audio", CPF_Config );
	new(Class, "MusicInterpolation", RF_Public)UByteProperty  ( CPP_PROPERTY( MusicInterpolation ), "Audio", CPF_Config );
	new(Class, "MusicRate",          RF_Public)UIntProperty   ( CPP_PROPERTY( MusicRate          ), "Audio", CPF_Config );
	new(Class, "MusicMono",          RF_Public)UBoolProperty  ( CPP_PROPERTY( MusicMono          ), "Audio", CPF_Config );
	unguardSlow;
}

UNOpenALAudioSubsystem::UNOpenALAudioSubsystem()
{
	OutputRate = DEFAULT_OUTPUT_RATE;
	MasterVolume = 255;
	SoundVolume = 127;
	MusicVolume = 63;
	AmbientFactor = 0.6f;
	DopplerFactor = 0.01f;
	UseHRTF = true;
	UseReverb = true;
	MusicInterpolation = XMP_INTERP_LINEAR;
#ifdef __PSP__
	// libxmp mixes on the CPU; a 333MHz MIPS cannot spare 22kHz stereo with
	// filters. 11kHz mono, nearest, no DSP is "cheap tracker" quality.
	MusicRate = 11025;
	MusicMono = 1;
#else
	MusicRate = 0;      // 0 = the device rate
	MusicMono = 0;
#endif
}

UBOOL UNOpenALAudioSubsystem::Init()
{
	guard(UNOpenALAudioSubsystem::Init)

	Viewport = NULL;
	Device = NULL;
	if( DeviceName[0] )
		Device = alcOpenDevice( DeviceName );
	if( !Device )
		Device = alcOpenDevice( NULL );
	if( !Device )
	{
		debugf( NAME_Warning, "Could not open AL device: %04x", alcGetError( NULL ) );
		return false;
	}

	if( OutputRate <= 0 )
		OutputRate = DEFAULT_OUTPUT_RATE;
	
	if( DopplerFactor < 0.f )
		DopplerFactor = 0.f;

	AmbientFactor = Clamp( AmbientFactor, 0.f, 1.f );

	if( MusicInterpolation > XMP_INTERP_SPLINE )
		MusicInterpolation = XMP_INTERP_SPLINE;
	if( MusicRate <= 0 )
		MusicRate = OutputRate;

#ifdef PSP_NO_EFX
	// PSP's alext.h has no ALC_SOFT_HRTF, and HRTF is meaningless on the
	// hardware's stereo output regardless.
	const ALint AttrList[] = {
		ALC_FREQUENCY, OutputRate,
		0
	};
#else
	const ALint AttrList[] = {
		ALC_FREQUENCY, OutputRate,
		ALC_SOFT_HRTF, UseHRTF,
		0
	};
#endif

	Ctx = alcCreateContext( Device, AttrList );
	if( !Ctx )
	{
		debugf( NAME_Warning, "Could not create AL context: %04x", alcGetError( Device ) );
		alcCloseDevice( Device );
		Device = NULL;
		return false;
	}

	alcMakeContextCurrent( Ctx );

	alDistanceModel( AL_LINEAR_DISTANCE_CLAMPED );
	alDopplerFactor( Max( 0.f, DopplerFactor ) );
#ifndef PSP_NO_EFX
	alListenerf( AL_METERS_PER_UNIT, DISTANCE_SCALE );   // EFX listener property
#endif
	alListenerf( AL_GAIN, MasterVolume / 255.f );

	alGenSources( MAX_SOURCES, Sources );

	alGenSources( 1, &MusicSource	);
	alSourcei( MusicSource, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( MusicSource, AL_POSITION, 0.f, 0.f, 0.f );
	alSourcef( MusicSource, AL_ROLLOFF_FACTOR, 0.f );
	alSourcef( MusicSource, AL_GAIN, MusicVolume / 255.f );

	alGenBuffers( ARRAY_COUNT( MusicBuffers ), MusicBuffers );
	for( INT i = 0; i < ARRAY_COUNT( MusicBuffers ); ++i )
	{
		alBufferData( MusicBuffers[i], MusicMono ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, MusicBufferData, sizeof( MusicBufferData ), MusicRate );
		FreeMusicBuffers[i] = MusicBuffers[i];
	}
	NumFreeMusicBuffers = NUM_MUSIC_BUFFERS;

	if( UseReverb )
	{
#ifndef PSP_NO_EFX  // reverb effect + slot creation
		alGenEffects( 1, &ReverbEffect );
		InitReverbEffect();
		alGenAuxiliaryEffectSlots( 1, &ReverbSlot );
		alAuxiliaryEffectSloti( ReverbSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL );
#endif
		ReverbOn = false;
	}

	for( INT i = 0; i < MAX_SOURCES; ++i )
		Voices[i].Buffer = INVALID_BUFFER;

	MusicCtx = xmp_create_context();
	xmp_set_player( MusicCtx, XMP_PLAYER_INTERP, MusicInterpolation );
#ifdef __PSP__
	xmp_set_player( MusicCtx, XMP_PLAYER_DSP, 0 );   // no lowpass filtering
#endif

	// Set ourselves up as the audio subsystem.
	USound::Audio = this;
	UMusic::Audio = this;

	// Spawn music streaming thread.
	StartMusicThread();

	return true;

	unguard;
}

void UNOpenALAudioSubsystem::Destroy()
{
	guard(UNOpenALAudioSubsystem::Destroy)

	StopMusicThread();

	USound::Audio = NULL;
	UMusic::Audio = NULL;

	if( MusicCtx )
	{
		xmp_end_player( MusicCtx );
		xmp_free_context( MusicCtx );
		MusicCtx = NULL;
	}

	if (UseReverb)
	{
#ifndef PSP_NO_EFX  // reverb teardown (Destroy)
		alDeleteAuxiliaryEffectSlots(1, &ReverbSlot);
		alDeleteEffects(1, &ReverbEffect);
#endif
	}

	if( Ctx )
	{
		// If we have a context, we probably have everything else. Kill it.
		SetViewport( NULL ); // This will also stop all sounds.
		alDeleteBuffers( Buffers.Num(), &Buffers(0) );
		alDeleteBuffers( ARRAY_COUNT( MusicBuffers ), MusicBuffers );
		alDeleteSources( MAX_SOURCES, Sources );
		alDeleteSources( 1, &MusicSource );
		alcMakeContextCurrent( NULL );
		alcDestroyContext( Ctx );
		Ctx = NULL;
		Buffers.Empty();
	}

	if( Device )
	{
		// We might still have a device even if we haven't created a context.
		alcCloseDevice( Device );
		Device = NULL;
	}

	Super::Destroy();

	unguard;
}

void UNOpenALAudioSubsystem::ShutdownAfterError()
{
	guard(UNOpenALAudioSubsystem::Destroy)

	StopMusicThread();

	USound::Audio = NULL;
	UMusic::Audio = NULL;

	// Shutdown contexts without touching anything else.
	if( MusicCtx )
	{
		xmp_free_context( MusicCtx );
		MusicCtx = NULL;
		Music = NULL;
	}
	if (UseReverb)
	{
#ifndef PSP_NO_EFX  // reverb teardown (ShutdownAfterError)
		alDeleteAuxiliaryEffectSlots(1, &ReverbSlot);
		alDeleteEffects(1, &ReverbEffect);
#endif
	}
	if( Ctx )
	{
		alcMakeContextCurrent( NULL );
		alcDestroyContext( Ctx );
		Ctx = NULL;
	}
	if( Device )
	{
		alcCloseDevice( Device );
		Device = NULL;
	}

	Super::ShutdownAfterError();

	unguard;
}

void UNOpenALAudioSubsystem::PostEditChange()
{
	guard(UNOpenALAudioSubsystem::PostEditChange)

	Super::PostEditChange();

	FScopedLock Lock( MusicMutex );

	if( DopplerFactor < 0.f )
		DopplerFactor = 0.f;
	AmbientFactor = Clamp( AmbientFactor, 0.f, 1.f );
	MusicInterpolation = Clamp( MusicInterpolation, (BYTE)0, (BYTE)XMP_INTERP_SPLINE );

	if( Ctx )
	{
		alListenerf( AL_GAIN, MasterVolume / 255.f );
		alDopplerFactor( DopplerFactor );
		alSourcef( MusicSource, AL_GAIN, Max(MusicFade, 0.f) * MusicVolume / 255.f );
#ifdef __PSP__
			GPspMusVol = (int)( Max(MusicFade, 0.f) * MusicVolume / 255.f * PSP_AUDIO_VOLUME_MAX );
#endif
		// Voice volumes will be updated in Update().
	}

	if( MusicCtx )
	{
		xmp_set_player( MusicCtx, XMP_PLAYER_INTERP, MusicInterpolation );
	}

	unguard;
}

void UNOpenALAudioSubsystem::SetViewport( UViewport* InViewport )
{
	guard(UNOpenALAudioSubsystem::SetViewport)

	// Stop all sounds before viewport change.
	for( INT i = 0; i < MAX_SOURCES; ++i )
		StopVoice( i );

	// Stop and free music if the viewport has changed.
	if( InViewport != Viewport )
	{
		if( Music )
		{
			UnregisterMusic( Music );
			Music = NULL;
		}
	}

	Viewport = InViewport;

	unguard;
}

void UNOpenALAudioSubsystem::RegisterMusic( UMusic* Music )
{
	guard(UNOpenALAudioSubsystem::RegisterMusic)

	FScopedLock Lock( MusicMutex );

	if( Music->Handle || !Music->Data.Num() )
		return;

#ifdef __PSP__
	if( PspMusOpen( Music->GetName(), MusicVolume ) )
	{
		Music->Handle = (void*)2;
		MusicIsLoaded = true;
		return;
	}
#endif
	INT Err = xmp_load_module_from_memory( MusicCtx, &Music->Data(0), Music->Data.Num() );
	if( Err < 0 )
	{
		debugf( NAME_Warning, "Couldn't load music `%s`: %d", Music->GetName(), Err );
		return;
	}

	Err = xmp_start_player( MusicCtx, MusicRate, MusicMono ? XMP_FORMAT_MONO : 0 );
	if( Err < 0 )
	{
		xmp_release_module( MusicCtx );
		debugf( NAME_Warning, "Couldn't start player on `%s`: %d", Music->GetName(), Err );
		return;
	}

	Music->Handle = (void*)1;
	MusicIsLoaded = true;

	unguard;
}

void UNOpenALAudioSubsystem::UnregisterMusic( UMusic* Music )
{
	guard(UNOpenALAudioSubsystem::UnregisterMusic)

	FScopedLock Lock( MusicMutex );

	StopMusic();
	ClearMusicBuffers();
#ifdef __PSP__
	if( GPspMusStreaming )
	{
		PspMusClose();
		MusicIsLoaded = false;
		return;
	}
#endif
	if( MusicCtx )
	{
		xmp_end_player( MusicCtx );
		if( MusicIsLoaded )
			xmp_release_module( MusicCtx );
	}

	MusicIsLoaded = false;

	unguard;
}

void UNOpenALAudioSubsystem::RegisterSound( USound* Sound )
{
	guard(UNOpenALAudioSubsystem::RegisterSound)

	if( Sound->Handle )
		return;

	check( Sound->Data.Num() );

	FWaveModInfo WaveInfo;
	if( !WaveInfo.ReadWaveInfo( Sound->Data ) )
	{
		debugf( NAME_Warning, "Sound %s is not a valid WAV file", Sound->GetName() );
		return;
	}

	ALuint Buf = 0;
	alGenBuffers( 1, &Buf );
	Buffers.AddItem( Buf );

	ALenum Format = AL_FORMAT_MONO8;
	if( *WaveInfo.pChannels == 2 )
	{
		if( *WaveInfo.pBitsPerSample == 16 )
			Format = AL_FORMAT_STEREO16;
		else
			Format = AL_FORMAT_STEREO8;
	}
	else
	{
		if( *WaveInfo.pBitsPerSample == 16 )
			Format = AL_FORMAT_MONO16;
		else
			Format = AL_FORMAT_MONO8;
	}

	alBufferData( Buf, Format, (const void*)WaveInfo.SampleDataStart, WaveInfo.SampleDataSize, *WaveInfo.pSamplesPerSec );

	Sound->Handle = (void*)Buf;
	Sound->Looping = ( WaveInfo.SampleLoopsNum != 0 ); // the only indication of looping in this version of UE1

	if( !GIsEditor )
		Sound->Data.Empty();

	unguard;
}

void UNOpenALAudioSubsystem::UnregisterSound( USound* Sound )
{
	guard(UNOpenALAudioSubsystem::UnregisterSound)

	check( Sound );

	if( Sound->Handle )
	{
		ALuint Buf = (ALuint)Sound->Handle;
		check( alIsBuffer( Buf ) );

		for( INT i = 0; i < MAX_SOURCES; ++i )
		{
			if( Voices[i].Sound == Sound )
				StopVoice( i );
		}

		Buffers.RemoveItem( Buf );
		alDeleteBuffers( 1, &Buf );

		Sound->Handle = NULL;
	}

	unguard;
}

void UNOpenALAudioSubsystem::UpdateVoice( INT Num, const ENVoiceOp Op )
{
	guard(UNOpenALAudioSubsystem::UpdateVoice)

	FNVoice& Voice = Voices[Num];

	// Swap position and velocity into AL space.
	FVector ALVelocity;
	ALVelocity.X = Voice.Velocity.X;
	ALVelocity.Y = Voice.Velocity.Y;
	ALVelocity.Z = -Voice.Velocity.Z;

	// If the source is close enough to the listener, don't spatialize it.
	DWORD SourceRelative;
	FVector ALLocation;
	if( FDistSquared( ListenerCoords.Origin, Voice.Location ) < Square( Voice.Radius * DESPATIALIZE_FACTOR ) )
	{
		SourceRelative = AL_TRUE;
		ALLocation.X = 0.f;
		ALLocation.Y = 0.f;
		ALLocation.Z = 0.f;
		// If the source is ON the listener, also reset the velocity so we don't doppler our own voice.
		if( Viewport && Viewport->Actor && Voice.Actor == Viewport->Actor )
		{
			ALVelocity.X = 0.f;
			ALVelocity.Y = 0.f;
			ALVelocity.Z = 0.f;
		}
	}
	else
	{
		SourceRelative = AL_FALSE;
		ALLocation.X = Voice.Location.X;
		ALLocation.Y = Voice.Location.Y;
		ALLocation.Z = -Voice.Location.Z;
	}

	// Set up AL source.
	ALuint Source = Sources[Num];
	alSourcei( Source, AL_SOURCE_RELATIVE, SourceRelative );
	alSourcef( Source, AL_GAIN, Voice.Volume * ( SoundVolume / 255.f ) );
	alSourcef( Source, AL_PITCH, Voice.Pitch );
	alSourcef( Source, AL_MAX_DISTANCE, Voice.Radius );
	alSourcef( Source, AL_REFERENCE_DISTANCE, Voice.Radius * DESPATIALIZE_FACTOR );
	alSourcef( Source, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR );
	alSourcefv( Source, AL_POSITION, &ALLocation.X );
	alSourcefv( Source, AL_VELOCITY, &ALVelocity.X );
	alSourcei( Source, AL_LOOPING, Voice.Looping );

	if( Voice.BufferChanged )
	{
		Voice.BufferChanged = false;
		INT State = AL_STOPPED;
		alGetSourcei( Source, AL_SOURCE_STATE, &State );
		if( State == AL_PLAYING || State == AL_PAUSED )
			alSourceStop( Source );
		alSourcei( Source, AL_BUFFER, Voice.Buffer );
		if( State == AL_PLAYING || State == AL_PAUSED )
		{
			alSourcePlay( Source );
			if( State == AL_PAUSED )
				alSourcePause( Source );
		}
	}

	if( UseReverb && Op == NVOP_Play )
#ifndef PSP_NO_EFX  // per-source reverb send
		alSource3i( Source, AL_AUXILIARY_SEND_FILTER, (ALint)ReverbSlot, 0, AL_FILTER_NULL );
#endif

	// Play or stop if needed.
	switch( Op )
	{
		case ENVoiceOp::NVOP_Play:  alSourcePlay( Source ); break;
		case ENVoiceOp::NVOP_Pause: alSourcePause( Source ); break;
		case ENVoiceOp::NVOP_Stop:  alSourceStop( Source ); break;
		default: break;
	}

	unguard;
}

UBOOL UNOpenALAudioSubsystem::PlaySound( AActor* Actor, INT Id, USound* Sound, FVector Location, FLOAT Volume, FLOAT Radius, FLOAT Pitch )
{
	guard(UNOpenALAudioSubsystem::PlaySound)

	if( !Viewport )
		return false;

	// Allocate a new slot if requested.
	if( SOUND_SLOT_IS( Id, SLOT_None ) )
		Id = 16 * --NextId;

	FLOAT Priority = GetVoicePriority( Location, Volume, Radius );
	FLOAT MaxPriority = Priority;
	FNVoice* Voice = NULL;
	for( INT i = 0; i < MAX_SOURCES; ++i )
	{
		FNVoice* V = &Voices[i];
		if( ( V->Id & ~1 ) == ( Id & ~1 ) )
		{
			// Skip if not interruptable.
			if( Id & 1 )
				return false;
			StopVoice( i );
			Voice = V;
			break;
		}
		else if( V->Priority <= MaxPriority )
		{
			MaxPriority = V->Priority;
			Voice = V;
		}
	}

	// If we ran out of voices or the sound is too low priority, bail.
	if( !Voice || !Sound || !Sound->Handle )
		return false;

	ALuint Buf = (ALuint)Sound->Handle;
	check( alIsBuffer( Buf ) );

	Voice->Id = Id;
	Voice->BufferChanged = ( Buf != Voice->Buffer );
	Voice->Buffer = Buf;
	Voice->Location = Location;
	Voice->Velocity = Actor ? Actor->Velocity : FVector();
	Voice->Volume = Clamp( Volume, 0.f, 1.f );
	Voice->Radius = Radius;
	Voice->Pitch = Pitch;
	Voice->Priority = Priority;
	Voice->Actor = Actor;
	Voice->Looping = Sound->Looping;
	Voice->Sound = Sound;

	// Start the voice.
	UpdateVoice( Voice - Voices, NVOP_Play );

	return true;

	unguard;
}

void UNOpenALAudioSubsystem::NoteDestroy( AActor* Actor )
{
	guard(UNOpenALAudioSubsystem::NoteDestroy)

	check(Actor);
	check(Actor->IsValid());
	for( INT i = 0; i < MAX_SOURCES; ++i)
	{
		if( Voices[i].Actor == Actor )
		{
			if( SOUND_SLOT_IS( Voices[i].Id, SLOT_Ambient ) )
			{
				// Stop ambient sound when actor dies.
				StopVoice( i );
			}
			else
			{
				// Unbind regular sounds from actors.
				Voices[i].Actor = NULL;
			}
		}
	}

	unguard;
}

void UNOpenALAudioSubsystem::StopVoice( INT Num )
{
	guard(UNOpenALAudioSubsystem::StopVoice)

	FNVoice& Voice = Voices[Num];

	alSourcei( Sources[Num], AL_LOOPING, AL_FALSE );
	alSourceStop( Sources[Num] );

	if( Voice.Buffer != INVALID_BUFFER )
	{
		Voice.Buffer = INVALID_BUFFER;
		alSourcei( Sources[Num], AL_BUFFER, 0 );
	}

	Voice.Priority = 0.f;
	Voice.Sound = NULL;
	Voice.Actor = NULL;
	Voice.Looping = false;
	Voice.Id = 0;

	unguard;
}

void UNOpenALAudioSubsystem::PlayMusic()
{
#ifdef PSP_NO_MUSIC
	return;   // built with -DPSP_NO_MUSIC=ON
#endif
	guard(UNOpenALAudioSubsystem::PlayMusic)

	FScopedLock Lock( MusicMutex );

#ifdef __PSP__
	if( GPspMusStreaming )
	{
		GPspMusRewind  = 1;   // sections are not rendered separately; restart the loop
		GPspMusPlaying = 1;
		MusicIsPlaying = true;
		return;
	}
#endif
	alSourceStop(MusicSource);
	ClearMusicBuffers();
	xmp_set_position( MusicCtx, MusicSection );

	ALint State = 0;
	alGetSourcei( MusicSource, AL_SOURCE_STATE, &State );
	if( State != AL_PLAYING )
		alSourcePlay( MusicSource );

	MusicIsPlaying = true;

	unguard;
}

void UNOpenALAudioSubsystem::StopMusic()
{
	guard(UNOpenALAudioSubsystem::StopMusic)

	FScopedLock Lock( MusicMutex );

	MusicIsPlaying = false;
#ifdef __PSP__
	GPspMusPlaying = 0;
#endif
	alSourceStop( MusicSource );

	unguard;
}

void UNOpenALAudioSubsystem::Update( FPointRegion Region, FCoords& Listener )
{
	guard(UNOpenALAudioSubsystem::Update)

	if( !Viewport || !Viewport->IsRealtime() )
		return;

	// Update AL listener position, velocity and orientation.
	FVector ALPosition;
	ALPosition.X = Listener.Origin.X;
	ALPosition.Y = Listener.Origin.Y;
	ALPosition.Z = -Listener.Origin.Z;
	FVector ALVelocity;
	if( Viewport->Actor )
	{
		ALVelocity.X = Viewport->Actor->Velocity.X;
		ALVelocity.Y = Viewport->Actor->Velocity.Y;
		ALVelocity.Z = -Viewport->Actor->Velocity.Z;
	}
	FLOAT ALOrientation[] = {
		+Listener.ZAxis.X,
		+Listener.ZAxis.Y,
		-Listener.ZAxis.Z,
		-Listener.YAxis.X,
		-Listener.YAxis.Y,
		+Listener.YAxis.Z,
	};
	alListenerfv( AL_POSITION, &ALPosition.X );
	alListenerfv( AL_VELOCITY, &ALVelocity.X );
	alListenerfv( AL_ORIENTATION, ALOrientation );
	ListenerCoords = Listener;

	if( UseReverb )
		UpdateReverb( Region );

	// Start new ambient sounds if needed.
	if( Viewport->Actor && Viewport->Actor->XLevel )
	{
		for( INT i = 0; i < Viewport->Actor->XLevel->Num(); i++ )
		{
			AActor* Actor = Viewport->Actor->XLevel->Actors(i);
			if( !Actor || !Actor->IsValid() )
				continue;

			const FLOAT DistSq = FDistSquared( Viewport->Actor->Location, Actor->Location );
			const FLOAT AmbRad = Square( Actor->WorldSoundRadius() );
			if( !Actor->AmbientSound || DistSq > AmbRad )
				continue;

			// See if it's already playing.
			INT Id = AMBIENT_SOUND_ID( Actor->GetIndex() );
			INT AmbientNum;
			for( AmbientNum = 0; AmbientNum < MAX_SOURCES; ++AmbientNum )
			{
				if( Voices[AmbientNum].Id == Id )
					break;
			}

			// If not, start it.
			if( AmbientNum == MAX_SOURCES )
			{
				FLOAT Vol = AmbientFactor * Actor->SoundVolume / 255.f;
				FLOAT Rad = Actor->WorldSoundRadius();
				FLOAT Pitch = Actor->SoundPitch / 64.f;
				PlaySound( Actor, Id, Actor->AmbientSound, Actor->Location, Vol, Rad, Pitch );
			}
		}
	}

	// Update active ambient sounds.
	for( INT VoiceNum = 0; VoiceNum < MAX_SOURCES; ++VoiceNum )
	{
		FNVoice& Voice = Voices[VoiceNum];
		if( !Voice.Id || !Voice.Sound || Voice.Buffer == INVALID_BUFFER || !SOUND_SLOT_IS( Voice.Id, SLOT_Ambient ) )
			continue;

		check( Voice.Actor );

		const FLOAT DistSq = FDistSquared( Viewport->Actor->Location, Voice.Actor->Location );
		const FLOAT AmbRad = Square( Voice.Actor->WorldSoundRadius() );
		if( Voice.Sound != Voice.Actor->AmbientSound || DistSq > AmbRad )
		{
			// Sound changed or went out of range.
			StopVoice( VoiceNum );
		}
		else
		{
			// Update parameters. These will be applied in the loop below.
			Voice.Radius = Voice.Actor->WorldSoundRadius();
			Voice.Pitch = Voice.Actor->SoundPitch / 64.f;
			Voice.Volume = AmbientFactor * Voice.Actor->SoundVolume / 255.f;
			if( Voice.Actor->LightType != LT_None )
				Voice.Volume *= Voice.Actor->LightBrightness / 255.f;
		}
	}

	// Update all active voices.
	for( INT VoiceNum = 0; VoiceNum < MAX_SOURCES; ++VoiceNum )
	{
		FNVoice& Voice = Voices[VoiceNum];
		if( !Voice.Id || !Voice.Sound || Voice.Buffer == INVALID_BUFFER )
			continue;

		ALuint Source = Sources[VoiceNum];
		ALint SourceState;
		alGetSourcei( Source, AL_SOURCE_STATE, &SourceState );
		if( SourceState == AL_STOPPED )
		{
			// Voice has finished playing.
			StopVoice( VoiceNum );
		}
		else if( SourceState == AL_PLAYING )
		{
			// Voice is playing, update its location and priority.
			if( Voice.Actor && Voice.Actor->IsValid() )
			{
				Voice.Location = Voice.Actor->Location;
				Voice.Velocity = Voice.Actor->Velocity;
			}
			Voice.Priority = GetVoicePriority( Voice.Location, Voice.Volume, Voice.Radius );
			// Update AL source.
			UpdateVoice( VoiceNum );
		}
	}

	// Update music.
	DOUBLE DeltaTime = appSeconds() - MusicTime;
	MusicTime += DeltaTime;
	DeltaTime = Clamp( DeltaTime, (DOUBLE)0.0, (DOUBLE)1.0 );
	if( Viewport->Actor && Viewport->Actor->Transition != MTRAN_None )
	{
		// Track is changing.
		UBOOL MusicChanged = Music != Viewport->Actor->Song;
		if( Music )
		{
			// Already playing something, figure out if we're ready to change.
			UBOOL MusicDone = false;
			if( MusicSection == 255 )
			{
				MusicDone = true;
			}
			else if( Viewport->Actor->Transition == MTRAN_Fade )
			{
				MusicFade -= DeltaTime;
				MusicDone = ( MusicFade < -2.f / 1000.f );
			}
			else if( Viewport->Actor->Transition == MTRAN_SlowFade )
			{
				MusicFade -= DeltaTime * 0.2;
				MusicDone = ( MusicFade < 0.2f * -2.f / 1000.f );
			}
			else if( Viewport->Actor->Transition == MTRAN_FastFade )
			{
				MusicFade -= DeltaTime * 3.0;
				MusicDone = ( MusicFade < 3.0f * -2.f / 1000.f );
			}
			else
			{
				MusicDone = true;
			}

			MusicMutex.Lock();

			if( MusicDone )
			{
				if( Music && MusicChanged )
					UnregisterMusic( Music );
				Music = NULL;
			}
			else
			{
				alSourcef( MusicSource, AL_GAIN, Max(MusicFade, 0.f) * MusicVolume / 255.f );
#ifdef __PSP__
			GPspMusVol = (int)( Max(MusicFade, 0.f) * MusicVolume / 255.f * PSP_AUDIO_VOLUME_MAX );
#endif
			}

			MusicMutex.Unlock();
		}

		if( Music == NULL )
		{
			FScopedLock Lock( MusicMutex );
			MusicFade = 1.f;
			alSourcef( MusicSource, AL_GAIN, Max(MusicFade, 0.f) * MusicVolume / 255.f );
#ifdef __PSP__
			GPspMusVol = (int)( Max(MusicFade, 0.f) * MusicVolume / 255.f * PSP_AUDIO_VOLUME_MAX );
#endif
			Music = Viewport->Actor->Song;
			MusicSection = Viewport->Actor->SongSection;
			if( Music )
			{
				if( MusicChanged )
					RegisterMusic( Music );
				if( MusicSection != 255 )
					PlayMusic();
				else
					StopMusic();
			}
			Viewport->Actor->Transition = MTRAN_None;
		}
	}

	unguard;
}

void UNOpenALAudioSubsystem::UpdateMusicBuffers()
{
#ifdef PSP_NO_MUSIC
	return;   // built with -DPSP_NO_MUSIC=ON
#endif
	guard(UNOpenALAudioSubsystem::UpdateMusicBuffers)

	FScopedLock Lock( MusicMutex );

	// First dequeue the buffers that are done playing and put them into the buffer pool
	ALint BuffersProcessed = 0;
	ALint BuffersQueued = 0;
	ALint State = AL_STOPPED;
	alGetSourcei( MusicSource, AL_BUFFERS_PROCESSED, &BuffersProcessed );
	alGetSourcei( MusicSource, AL_BUFFERS_QUEUED, &BuffersQueued );
	alGetSourcei( MusicSource, AL_SOURCE_STATE, &State );
	if( BuffersProcessed > 0 && NumFreeMusicBuffers < NUM_MUSIC_BUFFERS )
	{
		const INT NumToUnqueue = Min( NUM_MUSIC_BUFFERS - NumFreeMusicBuffers, BuffersProcessed );
		alSourceUnqueueBuffers( MusicSource, NumToUnqueue, &FreeMusicBuffers[NumFreeMusicBuffers] );
		NumFreeMusicBuffers += NumToUnqueue;
	}

	if( !Music || !MusicIsPlaying || MusicSection == 255 || !MusicCtx )
		return;
#ifdef __PSP__
	if( GPspMusStreaming )
		return;
#endif

	// If music is playing, render and queue more buffers if available
	while( BuffersQueued < NUM_MUSIC_BUFFERS && NumFreeMusicBuffers )
	{
		if( xmp_play_buffer( MusicCtx, MusicBufferData, sizeof( MusicBufferData ), 0 ) < 0 )
			break;
		alBufferData( FreeMusicBuffers[NumFreeMusicBuffers - 1], MusicMono ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, MusicBufferData, sizeof( MusicBufferData ), MusicRate );
		alSourceQueueBuffers( MusicSource, 1, &FreeMusicBuffers[NumFreeMusicBuffers - 1] );
		--NumFreeMusicBuffers;
		++BuffersQueued;
	}

	// If it stopped because it ran out of buffers, restart it
	if( BuffersQueued > 0 && ( State == AL_INITIAL || State == AL_STOPPED ) )
		alSourcePlay( MusicSource );

	unguard;
}

void UNOpenALAudioSubsystem::ClearMusicBuffers()
{
	guard(UNOpenALAudioSubsystem::ClearMusicBuffers)

	FScopedLock Lock( MusicMutex );

	appMemset( (void*)MusicBufferData, 0, sizeof(MusicBufferData) );

	ALint BuffersProcessed = 0;
	alGetSourcei( MusicSource, AL_BUFFERS_PROCESSED, &BuffersProcessed );
	if( BuffersProcessed > 0 && NumFreeMusicBuffers < NUM_MUSIC_BUFFERS )
	{
		const INT NumToUnqueue = Min( NUM_MUSIC_BUFFERS - NumFreeMusicBuffers, BuffersProcessed );
		alSourceUnqueueBuffers( MusicSource, NumToUnqueue, &FreeMusicBuffers[NumFreeMusicBuffers] );
		NumFreeMusicBuffers += NumToUnqueue;
	}

	for( INT i = 0; i < NumFreeMusicBuffers; ++i )
		alBufferData( FreeMusicBuffers[i], MusicMono ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, MusicBufferData, sizeof( MusicBufferData ), MusicRate );

	unguard;
}

void UNOpenALAudioSubsystem::UpdateReverb( FPointRegion& Region )
{
#ifdef PSP_NO_EFX
	// No efx.h on PSP, so reverb is compiled out entirely. Positional audio
	// and music are unaffected.
	(void)Region;
#else
	guard(UNOpenALAudioSubsystem::UpdateReverb)

	const UBOOL bNewReverb = ( Viewport->Actor && Viewport->Actor->Region.Zone && Viewport->Actor->Region.Zone->bReverbZone );

	if( !bNewReverb && ReverbOn )
	{
		// Just turn it off.
		ReverbOn = false;
		ReverbZone = NULL;
		// TODO: is there a better way to toggle this?
		alAuxiliaryEffectSloti( ReverbSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL );
		return;
	}

	ReverbOn = bNewReverb;

	if( ReverbOn )
	{
		AZoneInfo* NewReverbZone = Viewport->Actor->Region.Zone;
		if( NewReverbZone != ReverbZone )
		{
			// Reverb zone changed.
			ReverbZone = NewReverbZone;
			// Unbind effect, change parameters, then bind again.
			alAuxiliaryEffectSloti( ReverbSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL );
			alEffectf( ReverbEffect, AL_EAXREVERB_GAIN, ReverbZone->MasterGain / 255.f );
			// TODO: figure out how to convert Galaxy Audio reverb to this
			alAuxiliaryEffectSloti( ReverbSlot, AL_EFFECTSLOT_EFFECT, ReverbEffect );
		}
	}

	unguard;
#endif
}

void UNOpenALAudioSubsystem::InitReverbEffect()
{
	guard(UNOpenALAudioSubsystem::InitReverbEffect)

#ifndef PSP_NO_EFX  // reverb parameter block
	EFXEAXREVERBPROPERTIES Reverb = EFX_REVERB_PRESET_GENERIC;
	alEffecti( ReverbEffect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB );
	alEffectf( ReverbEffect, AL_EAXREVERB_DENSITY, Reverb.flDensity );
	alEffectf( ReverbEffect, AL_EAXREVERB_DIFFUSION, Reverb.flDiffusion );
	alEffectf( ReverbEffect, AL_EAXREVERB_GAIN, Reverb.flGain );
	alEffectf( ReverbEffect, AL_EAXREVERB_GAINHF, Reverb.flGainHF );
	alEffectf( ReverbEffect, AL_EAXREVERB_GAINLF, Reverb.flGainLF );
	alEffectf( ReverbEffect, AL_EAXREVERB_DECAY_TIME, Reverb.flDecayTime );
	alEffectf( ReverbEffect, AL_EAXREVERB_DECAY_HFRATIO, Reverb.flDecayHFRatio );
	alEffectf( ReverbEffect, AL_EAXREVERB_DECAY_LFRATIO, Reverb.flDecayLFRatio );
	alEffectf( ReverbEffect, AL_EAXREVERB_REFLECTIONS_GAIN, Reverb.flReflectionsGain);
	alEffectf( ReverbEffect, AL_EAXREVERB_REFLECTIONS_DELAY, Reverb.flReflectionsDelay );
	alEffectfv( ReverbEffect, AL_EAXREVERB_REFLECTIONS_PAN, Reverb.flReflectionsPan );
	alEffectf( ReverbEffect, AL_EAXREVERB_LATE_REVERB_GAIN, Reverb.flLateReverbGain );
	alEffectf( ReverbEffect, AL_EAXREVERB_LATE_REVERB_DELAY, Reverb.flLateReverbDelay );
	alEffectfv( ReverbEffect, AL_EAXREVERB_LATE_REVERB_PAN, Reverb.flLateReverbPan );
	alEffectf( ReverbEffect, AL_EAXREVERB_ECHO_TIME, Reverb.flEchoTime );
	alEffectf( ReverbEffect, AL_EAXREVERB_ECHO_DEPTH, Reverb.flEchoDepth );
	alEffectf( ReverbEffect, AL_EAXREVERB_MODULATION_TIME, Reverb.flModulationTime );
	alEffectf( ReverbEffect, AL_EAXREVERB_MODULATION_DEPTH, Reverb.flModulationDepth );
	alEffectf( ReverbEffect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, Reverb.flAirAbsorptionGainHF );
	alEffectf( ReverbEffect, AL_EAXREVERB_HFREFERENCE, Reverb.flHFReference );
	alEffectf( ReverbEffect, AL_EAXREVERB_LFREFERENCE, Reverb.flLFReference );
	alEffectf( ReverbEffect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, Reverb.flRoomRolloffFactor );
	alEffecti( ReverbEffect, AL_EAXREVERB_DECAY_HFLIMIT, Reverb.iDecayHFLimit );
#endif

	unguard;
}

UBOOL UNOpenALAudioSubsystem::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UNOpenALAudioSubsystem::Exec)

	if( ParseCommand( &Cmd, "MusicOrder") )
	{
		if( Music && MusicCtx )
		{
			FScopedLock Lock( MusicMutex );
			INT Pos = atoi( Cmd );
			Out->Logf( "Set music position to %d", Pos );
			xmp_set_position( MusicCtx, Pos );
			MusicSection = Pos;
			return true;
		}
	}
	else if( ParseCommand( &Cmd, "MusicInterp" ) )
	{
		FScopedLock Lock( MusicMutex );
		MusicInterpolation = Clamp( atoi( Cmd ), 0, XMP_INTERP_SPLINE );
		if( MusicCtx )
			xmp_set_player( MusicCtx, XMP_PLAYER_INTERP, MusicInterpolation );
		return true;
	}

	return false;

	unguard;
}

void UNOpenALAudioSubsystem::StartMusicThread()
{
	guard(UNOpenALAudioSubsystem::StartMusicThread)

	// This isn't an atomic because we only set it before the thread starts and before we wait on it to join.
	MusicThreadRunning = true;

	MusicThread = appThreadSpawn( MusicThreadProc, (void*)this, "MusicThread", true, nullptr );
	check(MusicThread);

	unguard;
}

void UNOpenALAudioSubsystem::StopMusicThread()
{
	guard(UNOpenALAudioSubsystem::StopMusicThread)

	if( MusicThread )
	{
		MusicThreadRunning = false;
		appThreadJoin( MusicThread );
		MusicThread = nullptr;
	}

	unguard;
}

#ifdef PLATFORM_WIN32
DWORD __stdcall UNOpenALAudioSubsystem::MusicThreadProc( void* Audio )
#else
void* UNOpenALAudioSubsystem::MusicThreadProc( void* Audio )
#endif
{
	UNOpenALAudioSubsystem* This = (UNOpenALAudioSubsystem*)Audio;

	while( This->MusicThreadRunning )
	{
		This->UpdateMusicBuffers();
		appSleep( 0.1f );
	}

	return (THREAD_RET)0;
}
