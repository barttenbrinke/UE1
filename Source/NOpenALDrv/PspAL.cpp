/*=============================================================================
	PspAL.cpp: the slice of the OpenAL API that NOpenALDrv uses, on top of
	PspMix. Sources map 1:1 onto mixer voices; buffers are 64-byte aligned
	copies of the PCM written back for the Media Engine; the 3D maths (linear
	clamped distance model, constant-power stereo pan) runs here on the CPU
	whenever a source or the listener changes, which is a few dozen times a
	frame. No doppler, no effects.
=============================================================================*/
#include "Engine.h"
#ifdef __PSP__
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "PspMix.h"

enum { PSPAL_BUFFERS = 512, PSPAL_QUEUE = 8 };

struct FPspALBuffer
{
	void*  Raw;        // memalign'd PCM (cached address)
	u32    Uncached;   // what the mixer reads
	u32    Frames;
	u32    Rate;
	u32    Flags;      // PSP_MIXF_16BIT / STEREO
	int    Used;
};
struct FPspALSource
{
	int    Used;
	float  Gain, Pitch, MaxDist, RefDist, Rolloff;
	float  Pos[3], Vel[3];
	int    Relative, Looping;
	ALuint Buffer;             // static buffer, 0 = none
	int    Started;            // ever played (AL_INITIAL vs AL_STOPPED)
	ALuint Queue[PSPAL_QUEUE]; // streaming queue; Queue[0] is the playing one
	int    QNum;
	u32    DoneSeen;           // voice Done count already accounted
	int    Processed;          // finished queue entries not yet unqueued
};

static FPspALBuffer GBuffers[PSPAL_BUFFERS];      // ids 1..PSPAL_BUFFERS-1
static FPspALSource GSources[PSP_MIX_VOICES + 1]; // ids 1..PSP_MIX_VOICES, voice = id-1
static float GListenerPos[3] = { 0, 0, 0 }, GListenerVel[3] = { 0, 0, 0 };
static float GListenerAt[3] = { 0, 0, -1 }, GListenerUp[3] = { 0, 1, 0 };
static float GListenerGain = 1.f;
static ALenum GError = AL_NO_ERROR;
static int   GDistanceModel = AL_LINEAR_DISTANCE_CLAMPED;
static struct ALCdevice_struct { int Dummy; } GDevice;
static struct ALCcontext_struct { int Dummy; } GContext;

static inline FPspALSource* Src( ALuint Id ) { return ( Id >= 1 && Id <= PSP_MIX_VOICES && GSources[Id].Used ) ? &GSources[Id] : NULL; }
static inline FPspMixVoice* Voice( ALuint Id ) { return GPspMix ? &GPspMix->Voices[Id - 1] : NULL; }
static inline FPspALBuffer* Buf( ALuint Id ) { return ( Id >= 1 && Id < PSPAL_BUFFERS && GBuffers[Id].Used ) ? &GBuffers[Id] : NULL; }

// Gains and step for a source from its state and the listener's.
static void PspALCommit( ALuint Id )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id );
	if( !S || !V )
		return;
	float Gain = S->Gain * GListenerGain;
	float PanL = 0.7071f, PanR = 0.7071f;
	if( !S->Relative )
	{
		const float Dx = S->Pos[0] - GListenerPos[0], Dy = S->Pos[1] - GListenerPos[1], Dz = S->Pos[2] - GListenerPos[2];
		float D = sqrtf( Dx*Dx + Dy*Dy + Dz*Dz );
		if( GDistanceModel != AL_NONE && S->MaxDist > S->RefDist )
		{
			float Dc = D < S->RefDist ? S->RefDist : ( D > S->MaxDist ? S->MaxDist : D );
			float Att = 1.f - S->Rolloff * ( Dc - S->RefDist ) / ( S->MaxDist - S->RefDist );
			Gain *= Att < 0.f ? 0.f : ( Att > 1.f ? 1.f : Att );
		}
		if( D > 1.f )
		{
			// right = at x up
			const float Rx = GListenerAt[1]*GListenerUp[2] - GListenerAt[2]*GListenerUp[1];
			const float Ry = GListenerAt[2]*GListenerUp[0] - GListenerAt[0]*GListenerUp[2];
			const float Rz = GListenerAt[0]*GListenerUp[1] - GListenerAt[1]*GListenerUp[0];
			const float Rl = sqrtf( Rx*Rx + Ry*Ry + Rz*Rz );
			float Right = Rl > 0.f ? ( Dx*Rx + Dy*Ry + Dz*Rz ) / ( Rl * D ) : 0.f;
			if( Right < -1.f ) Right = -1.f; else if( Right > 1.f ) Right = 1.f;
			PanL = sqrtf( ( 1.f - Right ) * 0.5f );
			PanR = sqrtf( ( 1.f + Right ) * 0.5f );
		}
	}
	if( Gain < 0.f ) Gain = 0.f;
	int GL = (int)( Gain * PanL * 4096.f + 0.5f ), GR = (int)( Gain * PanR * 4096.f + 0.5f );
	if( GL > 8192 ) GL = 8192; if( GR > 8192 ) GR = 8192;
	V->GainL = (u32)GL; V->GainR = (u32)GR;
	const FPspALBuffer* B = Buf( S->QNum ? S->Queue[0] : S->Buffer );
	const float Rate = B ? (float)B->Rate : (float)PSP_MIX_RATE;
	float Step = Rate * ( S->Pitch > 0.f ? S->Pitch : 1.f ) / (float)PSP_MIX_RATE;
	if( Step < 0.01f ) Step = 0.01f; else if( Step > 16.f ) Step = 16.f;
	V->Step = (u32)( Step * 65536.f );
	u32 F = V->Flags;
	F = S->Looping ? ( F | PSP_MIXF_LOOP ) : ( F & ~PSP_MIXF_LOOP );
	V->Flags = F;
}

// Load a buffer into the voice and start it.
static void PspALStart( ALuint Id, ALuint BufId )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id ); FPspALBuffer* B = Buf( BufId );
	if( !S || !V || !B )
		return;
	V->Flags = 0;
	V->Data = B->Uncached; V->Frames = B->Frames; V->Pos = 0; V->NextData = 0;
	PspALCommit( Id );
	V->Flags = PSP_MIXF_PLAYING | B->Flags | ( S->Looping ? PSP_MIXF_LOOP : 0 );
	S->Started = 1;
}

// Streaming: account finished buffers and keep Next topped up.
static void PspALService( ALuint Id )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id );
	if( !S || !V || !S->QNum )
		return;
	while( V->Done != S->DoneSeen && S->Processed < S->QNum )
	{
		S->DoneSeen = S->DoneSeen + 1;
		++S->Processed;
	}
	// The mixer plays Queue[Processed]; offer Queue[Processed+1] as Next.
	if( ( V->Flags & PSP_MIXF_PLAYING ) && !V->NextData && S->Processed + 1 < S->QNum )
	{
		FPspALBuffer* N = Buf( S->Queue[S->Processed + 1] );
		if( N ) { V->NextFrames = N->Frames; V->NextFlags = N->Flags; V->NextData = N->Uncached; }
	}
}

extern "C" {

// ---- ALC ------------------------------------------------------------------
ALCdevice*  ALC_APIENTRY alcOpenDevice( const ALCchar* ) { return PspMixInit() ? &GDevice : NULL; }
ALCboolean  ALC_APIENTRY alcCloseDevice( ALCdevice* ) { PspMixShutdown(); return ALC_TRUE; }
ALCcontext* ALC_APIENTRY alcCreateContext( ALCdevice* D, const ALCint* ) { return D ? &GContext : NULL; }
ALCboolean  ALC_APIENTRY alcMakeContextCurrent( ALCcontext* ) { return ALC_TRUE; }
void        ALC_APIENTRY alcDestroyContext( ALCcontext* ) {}
ALCcontext* ALC_APIENTRY alcGetCurrentContext( void ) { return &GContext; }
void        ALC_APIENTRY alcProcessContext( ALCcontext* ) {}
void        ALC_APIENTRY alcSuspendContext( ALCcontext* ) {}
ALCenum     ALC_APIENTRY alcGetError( ALCdevice* ) { return ALC_NO_ERROR; }
ALCboolean  ALC_APIENTRY alcIsExtensionPresent( ALCdevice*, const ALCchar* ) { return ALC_FALSE; }
const ALCchar* ALC_APIENTRY alcGetString( ALCdevice*, ALCenum Param ) { return Param == ALC_DEVICE_SPECIFIER || Param == ALC_DEFAULT_DEVICE_SPECIFIER ? "PspMix" : ""; }
void        ALC_APIENTRY alcGetIntegerv( ALCdevice*, ALCenum Param, ALCsizei Size, ALCint* Data )
{
	if( !Data || Size < 1 ) return;
	switch( Param )
	{
		case ALC_FREQUENCY:      *Data = PSP_MIX_RATE; break;
		case ALC_MONO_SOURCES:   *Data = PSP_MIX_VOICES - 1; break;
		case ALC_STEREO_SOURCES: *Data = 1; break;
		case ALC_MAJOR_VERSION:  *Data = 1; break;
		case ALC_MINOR_VERSION:  *Data = 1; break;
		default: *Data = 0; break;
	}
}

// ---- state / errors --------------------------------------------------------
ALenum      AL_APIENTRY alGetError( void ) { ALenum E = GError; GError = AL_NO_ERROR; return E; }
ALint       AL_APIENTRY alGetInteger( ALenum Param ) { return Param == AL_DISTANCE_MODEL ? GDistanceModel : 0; }
const ALchar* AL_APIENTRY alGetString( ALenum Param ) { return Param == AL_VERSION ? "1.1 PspMix" : ( Param == AL_RENDERER ? "PspMix" : ( Param == AL_VENDOR ? "UE1 PSP" : "" ) ); }
ALboolean   AL_APIENTRY alIsExtensionPresent( const ALchar* ) { return AL_FALSE; }
void*       AL_APIENTRY alGetProcAddress( const ALchar* ) { return NULL; }
ALenum      AL_APIENTRY alGetEnumValue( const ALchar* ) { return 0; }
void        AL_APIENTRY alDistanceModel( ALenum Model ) { GDistanceModel = Model; }
void        AL_APIENTRY alDopplerFactor( ALfloat ) {}
void        AL_APIENTRY alDopplerVelocity( ALfloat ) {}
void        AL_APIENTRY alSpeedOfSound( ALfloat ) {}

// ---- listener --------------------------------------------------------------
static void PspALCommitAll() { for( ALuint i = 1; i <= PSP_MIX_VOICES; ++i ) if( GSources[i].Used ) PspALCommit( i ); }
void AL_APIENTRY alListenerf( ALenum Param, ALfloat Value ) { if( Param == AL_GAIN ) { GListenerGain = Value; PspALCommitAll(); } }
void AL_APIENTRY alListener3f( ALenum Param, ALfloat A, ALfloat B, ALfloat C ) { ALfloat V[3] = { A, B, C }; alListenerfv( Param, V ); }
void AL_APIENTRY alListenerfv( ALenum Param, const ALfloat* V )
{
	if( !V ) return;
	switch( Param )
	{
		case AL_POSITION:    GListenerPos[0] = V[0]; GListenerPos[1] = V[1]; GListenerPos[2] = V[2]; PspALCommitAll(); break;
		case AL_VELOCITY:    GListenerVel[0] = V[0]; GListenerVel[1] = V[1]; GListenerVel[2] = V[2]; break;
		case AL_ORIENTATION: GListenerAt[0] = V[0]; GListenerAt[1] = V[1]; GListenerAt[2] = V[2]; GListenerUp[0] = V[3]; GListenerUp[1] = V[4]; GListenerUp[2] = V[5]; PspALCommitAll(); break;
		default: break;
	}
}

// ---- buffers ---------------------------------------------------------------
void AL_APIENTRY alGenBuffers( ALsizei N, ALuint* Out )
{
	for( ALsizei k = 0; k < N; ++k )
	{
		Out[k] = 0;
		for( ALuint i = 1; i < PSPAL_BUFFERS; ++i )
			if( !GBuffers[i].Used ) { memset( &GBuffers[i], 0, sizeof(GBuffers[i]) ); GBuffers[i].Used = 1; Out[k] = i; break; }
		if( !Out[k] ) GError = AL_OUT_OF_MEMORY;
	}
}
void AL_APIENTRY alDeleteBuffers( ALsizei N, const ALuint* Ids )
{
	for( ALsizei k = 0; k < N; ++k )
	{
		FPspALBuffer* B = Buf( Ids[k] );
		if( !B ) continue;
		// Detach from any voice still pointing at it.
		for( ALuint s = 1; s <= PSP_MIX_VOICES; ++s )
		{
			FPspMixVoice* V = Voice( s );
			if( V && ( V->Data == B->Uncached || V->NextData == B->Uncached ) ) { V->Flags = 0; V->Data = 0; V->NextData = 0; }
			if( GSources[s].Used && GSources[s].Buffer == Ids[k] ) GSources[s].Buffer = 0;
		}
		if( B->Raw ) free( B->Raw );
		memset( B, 0, sizeof(*B) );
	}
}
ALboolean AL_APIENTRY alIsBuffer( ALuint Id ) { return Buf( Id ) ? AL_TRUE : AL_FALSE; }
void AL_APIENTRY alBufferData( ALuint Id, ALenum Format, const ALvoid* Data, ALsizei Size, ALsizei Freq )
{
	FPspALBuffer* B = Buf( Id );
	if( !B || !Data || Size <= 0 ) { GError = AL_INVALID_VALUE; return; }
	u32 Flags = 0, BytesPerFrame = 1;
	switch( Format )
	{
		case AL_FORMAT_MONO8:    Flags = 0; BytesPerFrame = 1; break;
		case AL_FORMAT_MONO16:   Flags = PSP_MIXF_16BIT; BytesPerFrame = 2; break;
		case AL_FORMAT_STEREO8:  Flags = PSP_MIXF_STEREO; BytesPerFrame = 2; break;
		case AL_FORMAT_STEREO16: Flags = PSP_MIXF_STEREO | PSP_MIXF_16BIT; BytesPerFrame = 4; break;
		default: GError = AL_INVALID_ENUM; return;
	}
	void* Raw = memalign( 64, ( Size + 63 ) & ~63 );
	if( !Raw ) { GError = AL_OUT_OF_MEMORY; return; }
	memcpy( Raw, Data, Size );
	sceKernelDcacheWritebackRange( Raw, ( Size + 63 ) & ~63 );
	// Any voice on the old data stops: the memory goes away.
	if( B->Raw )
	{
		for( ALuint s = 1; s <= PSP_MIX_VOICES; ++s )
		{
			FPspMixVoice* V = Voice( s );
			if( V && ( V->Data == B->Uncached || V->NextData == B->Uncached ) ) { V->Flags = 0; V->Data = 0; V->NextData = 0; }
		}
		free( B->Raw );
	}
	B->Raw = Raw; B->Uncached = (u32)Raw | 0x40000000; B->Frames = (u32)Size / BytesPerFrame; B->Rate = (u32)Freq; B->Flags = Flags;
}
void AL_APIENTRY alGetBufferi( ALuint Id, ALenum Param, ALint* Value )
{
	FPspALBuffer* B = Buf( Id ); if( !B || !Value ) return;
	switch( Param )
	{
		case AL_FREQUENCY: *Value = (ALint)B->Rate; break;
		case AL_BITS:      *Value = ( B->Flags & PSP_MIXF_16BIT ) ? 16 : 8; break;
		case AL_CHANNELS:  *Value = ( B->Flags & PSP_MIXF_STEREO ) ? 2 : 1; break;
		case AL_SIZE:      *Value = (ALint)( B->Frames * ( ( B->Flags & PSP_MIXF_16BIT ) ? 2 : 1 ) * ( ( B->Flags & PSP_MIXF_STEREO ) ? 2 : 1 ) ); break;
		default: *Value = 0; break;
	}
}

// ---- sources ---------------------------------------------------------------
void AL_APIENTRY alGenSources( ALsizei N, ALuint* Out )
{
	for( ALsizei k = 0; k < N; ++k )
	{
		Out[k] = 0;
		for( ALuint i = 1; i <= PSP_MIX_VOICES; ++i )
			if( !GSources[i].Used )
			{
				FPspALSource& S = GSources[i]; memset( &S, 0, sizeof(S) );
				S.Used = 1; S.Gain = 1.f; S.Pitch = 1.f; S.MaxDist = 1e9f; S.RefDist = 1.f; S.Rolloff = 1.f;
				if( FPspMixVoice* V = Voice( i ) ) { V->Flags = 0; V->Data = 0; V->NextData = 0; V->Done = 0; }
				Out[k] = i; break;
			}
		if( !Out[k] ) GError = AL_OUT_OF_MEMORY;
	}
}
void AL_APIENTRY alDeleteSources( ALsizei N, const ALuint* Ids )
{
	for( ALsizei k = 0; k < N; ++k )
		if( FPspALSource* S = Src( Ids[k] ) )
		{
			if( FPspMixVoice* V = Voice( Ids[k] ) ) { V->Flags = 0; V->Data = 0; V->NextData = 0; }
			memset( S, 0, sizeof(*S) );
		}
}
void AL_APIENTRY alSourcef( ALuint Id, ALenum Param, ALfloat Value )
{
	FPspALSource* S = Src( Id ); if( !S ) return;
	switch( Param )
	{
		case AL_GAIN:               S->Gain = Value; break;
		case AL_PITCH:              S->Pitch = Value; break;
		case AL_MAX_DISTANCE:       S->MaxDist = Value; break;
		case AL_REFERENCE_DISTANCE: S->RefDist = Value; break;
		case AL_ROLLOFF_FACTOR:     S->Rolloff = Value; break;
		default: return;
	}
	PspALCommit( Id );
}
void AL_APIENTRY alSource3f( ALuint Id, ALenum Param, ALfloat A, ALfloat B, ALfloat C ) { ALfloat V[3] = { A, B, C }; alSourcefv( Id, Param, V ); }
void AL_APIENTRY alSourcefv( ALuint Id, ALenum Param, const ALfloat* V )
{
	FPspALSource* S = Src( Id ); if( !S || !V ) return;
	switch( Param )
	{
		case AL_POSITION: S->Pos[0] = V[0]; S->Pos[1] = V[1]; S->Pos[2] = V[2]; PspALCommit( Id ); break;
		case AL_VELOCITY: S->Vel[0] = V[0]; S->Vel[1] = V[1]; S->Vel[2] = V[2]; break;
		default: alSourcef( Id, Param, V[0] ); break;
	}
}
void AL_APIENTRY alSourcei( ALuint Id, ALenum Param, ALint Value )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id ); if( !S || !V ) return;
	switch( Param )
	{
		case AL_SOURCE_RELATIVE: S->Relative = Value != 0; PspALCommit( Id ); break;
		case AL_LOOPING:         S->Looping = Value != 0; PspALCommit( Id ); break;
		case AL_BUFFER:
			S->Buffer = (ALuint)Value; S->QNum = 0; S->Processed = 0;
			if( !Value ) { V->Flags = 0; V->Data = 0; V->NextData = 0; }
			break;
		default: break;
	}
}
void AL_APIENTRY alSource3i( ALuint, ALenum, ALint, ALint, ALint ) {}
void AL_APIENTRY alGetSourcef( ALuint Id, ALenum Param, ALfloat* Value )
{
	FPspALSource* S = Src( Id ); if( !S || !Value ) return;
	switch( Param )
	{
		case AL_GAIN:  *Value = S->Gain; break;
		case AL_PITCH: *Value = S->Pitch; break;
		default: *Value = 0.f; break;
	}
}
void AL_APIENTRY alGetSourcei( ALuint Id, ALenum Param, ALint* Value )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id ); if( !S || !V || !Value ) return;
	switch( Param )
	{
		case AL_SOURCE_STATE:
			PspALService( Id );
			if( V->Flags & PSP_MIXF_PLAYING ) *Value = ( V->Flags & PSP_MIXF_PAUSED ) ? AL_PAUSED : AL_PLAYING;
			else *Value = S->Started ? AL_STOPPED : AL_INITIAL;
			break;
		case AL_BUFFER:            *Value = (ALint)( S->QNum ? S->Queue[0] : S->Buffer ); break;
		case AL_BUFFERS_QUEUED:    PspALService( Id ); *Value = S->QNum; break;
		case AL_BUFFERS_PROCESSED: PspALService( Id ); *Value = S->Processed; break;
		case AL_SOURCE_RELATIVE:   *Value = S->Relative; break;
		case AL_LOOPING:           *Value = S->Looping; break;
		default: *Value = 0; break;
	}
}
void AL_APIENTRY alSourcePlay( ALuint Id )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id ); if( !S || !V ) return;
	if( V->Flags & PSP_MIXF_PAUSED ) { V->Flags = V->Flags & ~PSP_MIXF_PAUSED; return; }
	if( S->QNum )
	{
		// Streaming: (re)start from the first unprocessed buffer.
		if( S->Processed >= S->QNum ) return;
		PspALStart( Id, S->Queue[S->Processed] );
		S->DoneSeen = V->Done;
		PspALService( Id );
	}
	else if( S->Buffer )
		PspALStart( Id, S->Buffer );
}
void AL_APIENTRY alSourcePlayv( ALsizei N, const ALuint* Ids ) { for( ALsizei k = 0; k < N; ++k ) alSourcePlay( Ids[k] ); }
void AL_APIENTRY alSourceStop( ALuint Id )
{
	FPspALSource* S = Src( Id ); FPspMixVoice* V = Voice( Id ); if( !S || !V ) return;
	PspALService( Id );
	V->Flags = V->Flags & ~( PSP_MIXF_PLAYING | PSP_MIXF_PAUSED );
	V->NextData = 0;
	if( S->QNum ) { S->Processed = S->QNum; S->DoneSeen = V->Done; }   // everything queued counts as played
}
void AL_APIENTRY alSourceStopv( ALsizei N, const ALuint* Ids ) { for( ALsizei k = 0; k < N; ++k ) alSourceStop( Ids[k] ); }
void AL_APIENTRY alSourcePause( ALuint Id )
{
	FPspMixVoice* V = Voice( Id ); if( !Src( Id ) || !V ) return;
	if( V->Flags & PSP_MIXF_PLAYING ) V->Flags = V->Flags | PSP_MIXF_PAUSED;
}
void AL_APIENTRY alSourceQueueBuffers( ALuint Id, ALsizei N, const ALuint* Ids )
{
	FPspALSource* S = Src( Id ); if( !S ) return;
	for( ALsizei k = 0; k < N; ++k )
	{
		if( S->QNum >= PSPAL_QUEUE || !Buf( Ids[k] ) ) { GError = AL_INVALID_VALUE; return; }
		S->Queue[S->QNum++] = Ids[k];
	}
	S->Buffer = 0;
	PspALService( Id );
}
void AL_APIENTRY alSourceUnqueueBuffers( ALuint Id, ALsizei N, ALuint* Ids )
{
	FPspALSource* S = Src( Id ); if( !S ) return;
	PspALService( Id );
	if( N > S->Processed ) { GError = AL_INVALID_VALUE; N = S->Processed; }
	for( ALsizei k = 0; k < N; ++k ) Ids[k] = S->Queue[k];
	for( int i = N; i < S->QNum; ++i ) S->Queue[i - N] = S->Queue[i];
	S->QNum -= N; S->Processed -= N;
}

} // extern "C"
#endif
