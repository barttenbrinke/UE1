/*=============================================================================
	PspMix.cpp: the PSP sound mixer (see PspMix.h).
=============================================================================*/
#include "Engine.h"
#ifdef __PSP__
#include <pspaudio.h>
#include <pspthreadman.h>
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>
#include "PspMix.h"

FPspMixShared* GPspMix = NULL;

static void*  GPspMixSharedRaw = NULL;
static void*  GPspMixRingRaw   = NULL;
static int    GPspMixChan      = -1;
static SceUID GPspMixThread    = -1;
static volatile int GPspMixRun = 0;
static int    GPspMixAcc[PSP_MIX_FRAMES * 2] __attribute__((aligned(64)));   // CPU mixing accumulator
static short  GPspMixSilence[PSP_MIX_FRAMES * 2] __attribute__((aligned(64)));

//
// Mix one block. Nearest-plus-linear resampling in 16.16, gains in Q12,
// 32-bit accumulation, one clamp at the end. Runs on the ME or the CPU.
//
void PspMixBlock( FPspMixShared* Mix, short* Out, int* Acc )
{
	for( int i = 0; i < PSP_MIX_FRAMES * 2; ++i )
		Acc[i] = 0;
	for( int v = 0; v < PSP_MIX_VOICES; ++v )
	{
		FPspMixVoice& V = Mix->Voices[v];
		const u32 Flags = V.Flags;
		if( !( Flags & PSP_MIXF_PLAYING ) || ( Flags & PSP_MIXF_PAUSED ) || V.Ended || !V.Data || !V.Frames )
			continue;
		u32 Fmt    = V.CurFmt;
		u32 Data   = V.Data;
		u32 Frames = V.Frames;
		u32 Pos    = V.Pos;
		const u32 Step  = V.Step;
		const int GainL = (int)V.GainL, GainR = (int)V.GainR;
		int i = 0;
		while( i < PSP_MIX_FRAMES )
		{
			u32 Idx = Pos >> 16;
			if( Idx >= Frames )
			{
				if( Flags & PSP_MIXF_LOOP )
				{
					Pos -= Frames << 16;
					if( ( Pos >> 16 ) >= Frames ) Pos = 0;
					continue;
				}
				if( V.NextData )
				{
					// Streaming: the next buffer follows without a gap.
					Data = V.NextData; Frames = V.NextFrames; Fmt = V.NextFlags & ( PSP_MIXF_16BIT | PSP_MIXF_STEREO );
					V.Data = Data; V.Frames = Frames; V.CurFmt = Fmt; V.NextData = 0;
					V.Done = V.Done + 1;
					Pos -= Idx << 16;
					continue;
				}
				V.Ended = 1;
				V.Done = V.Done + 1;
				break;
			}
			const u32 Idx1 = ( Idx + 1 < Frames ) ? Idx + 1 : Idx;
			const int Frac = (int)( ( Pos >> 8 ) & 0xff );
			int L, R;
			if( Fmt & PSP_MIXF_STEREO )
			{
				if( Fmt & PSP_MIXF_16BIT )
				{
					const short* S = (const short*)Data;
					const int L0 = S[Idx*2], L1 = S[Idx1*2], R0 = S[Idx*2+1], R1 = S[Idx1*2+1];
					L = L0 + ( ( ( L1 - L0 ) * Frac ) >> 8 );
					R = R0 + ( ( ( R1 - R0 ) * Frac ) >> 8 );
				}
				else
				{
					const unsigned char* S = (const unsigned char*)Data;
					const int L0 = ( (int)S[Idx*2] - 128 ) << 8, L1 = ( (int)S[Idx1*2] - 128 ) << 8;
					const int R0 = ( (int)S[Idx*2+1] - 128 ) << 8, R1 = ( (int)S[Idx1*2+1] - 128 ) << 8;
					L = L0 + ( ( ( L1 - L0 ) * Frac ) >> 8 );
					R = R0 + ( ( ( R1 - R0 ) * Frac ) >> 8 );
				}
			}
			else
			{
				int S0, S1;
				if( Fmt & PSP_MIXF_16BIT )
				{
					const short* S = (const short*)Data;
					S0 = S[Idx]; S1 = S[Idx1];
				}
				else
				{
					const unsigned char* S = (const unsigned char*)Data;
					S0 = ( (int)S[Idx] - 128 ) << 8; S1 = ( (int)S[Idx1] - 128 ) << 8;
				}
				L = R = S0 + ( ( ( S1 - S0 ) * Frac ) >> 8 );
			}
			Acc[i*2]   += ( L * GainL ) >> 12;
			Acc[i*2+1] += ( R * GainR ) >> 12;
			Pos += Step;
			++i;
		}
		V.Pos = Pos;   // never Flags: see the ownership note in PspMix.h
	}
	for( int i = 0; i < PSP_MIX_FRAMES * 2; ++i )
	{
		int S = Acc[i];
		if( S > 32767 ) S = 32767; else if( S < -32768 ) S = -32768;
		Out[i] = (short)S;
	}
}

//
// Output thread: feeds the hardware channel from the ring. In ME mode it
// only waits for blocks; in CPU mode it mixes the block it is about to
// hand over. sceAudioOutputBlocking blocks until the channel takes the
// block, so this thread costs nothing between blocks.
//
static int PspMixThreadProc( SceSize, void* )
{
	FPspMixShared* Mix = GPspMix;
	while( GPspMixRun )
	{
		if( Mix->Enable )
		{
			// Give the ME a moment if it is late; then keep time with silence.
			int Waited = 0;
			while( Mix->Write == Mix->Read && Waited < 12 ) { sceKernelDelayThread( 1000 ); ++Waited; }
			if( Mix->Write == Mix->Read )
			{
				Mix->Underruns = Mix->Underruns + 1;
				sceAudioOutputBlocking( GPspMixChan, PSP_AUDIO_VOLUME_MAX, GPspMixSilence );
				continue;
			}
			short* Block = (short*)( Mix->Ring + ( Mix->Read % PSP_MIX_BLOCKS ) * PSP_MIX_FRAMES * 4 );
			sceAudioOutputBlocking( GPspMixChan, PSP_AUDIO_VOLUME_MAX, Block );
			Mix->Read = Mix->Read + 1;
		}
		else
		{
			short* Block = (short*)( Mix->Ring + ( Mix->Read % PSP_MIX_BLOCKS ) * PSP_MIX_FRAMES * 4 );
			PspMixBlock( Mix, Block, GPspMixAcc );
			sceAudioOutputBlocking( GPspMixChan, PSP_AUDIO_VOLUME_MAX, Block );
			Mix->Read = Mix->Read + 1;
			Mix->Write = Mix->Read;
		}
	}
	return 0;
}

UBOOL PspMixInit()
{
	if( GPspMix )
		return 1;
	const u32 SharedBytes = ( sizeof(FPspMixShared) + 63 ) & ~63;
	const u32 RingBytes   = PSP_MIX_BLOCKS * PSP_MIX_FRAMES * 4;
	GPspMixSharedRaw = memalign( 64, SharedBytes );
	GPspMixRingRaw   = memalign( 64, RingBytes );
	if( !GPspMixSharedRaw || !GPspMixRingRaw )
	{
		debugf( NAME_Warning, "PSPMIX: no memory" );
		return 0;
	}
	memset( GPspMixSharedRaw, 0, SharedBytes );
	memset( GPspMixRingRaw, 0, RingBytes );
	memset( GPspMixSilence, 0, sizeof(GPspMixSilence) );
	sceKernelDcacheWritebackInvalidateRange( GPspMixSharedRaw, SharedBytes );
	sceKernelDcacheWritebackInvalidateRange( GPspMixRingRaw, RingBytes );
	sceKernelDcacheWritebackInvalidateRange( GPspMixSilence, sizeof(GPspMixSilence) );
	GPspMix = (FPspMixShared*)( (u32)GPspMixSharedRaw | 0x40000000 );
	GPspMix->Ring = (u32)GPspMixRingRaw | 0x40000000;
	GPspMixChan = sceAudioChReserve( PSP_AUDIO_NEXT_CHANNEL, PSP_MIX_FRAMES, PSP_AUDIO_FORMAT_STEREO );
	if( GPspMixChan < 0 )
	{
		debugf( NAME_Warning, "PSPMIX: no hardware channel (%i)", GPspMixChan );
		GPspMix = NULL;
		return 0;
	}
	GPspMixRun = 1;
	// Above the game thread (36) so a block is never late, below the Memory
	// Stick driver (16) and the music streamer (16); it sleeps in the kernel
	// between blocks either way.
	GPspMixThread = sceKernelCreateThread( "psp_mix", PspMixThreadProc, 18, 16 * 1024, THREAD_ATTR_USER, NULL );
	if( GPspMixThread < 0 || sceKernelStartThread( GPspMixThread, 0, NULL ) < 0 )
	{
		debugf( NAME_Warning, "PSPMIX: thread failed (%i)", GPspMixThread );
		GPspMixRun = 0; sceAudioChRelease( GPspMixChan ); GPspMixChan = -1; GPspMix = NULL;
		return 0;
	}
	debugf( NAME_Log, "PSPMIX: up: %i voices, %i blocks of %i frames at %i Hz, hardware channel %i", (int)PSP_MIX_VOICES, (int)PSP_MIX_BLOCKS, (int)PSP_MIX_FRAMES, (int)PSP_MIX_RATE, GPspMixChan );
	return 1;
}

void PspMixShutdown()
{
	if( !GPspMix )
		return;
	GPspMix->Enable = 0;
	for( int v = 0; v < PSP_MIX_VOICES; ++v ) GPspMix->Voices[v].Flags = 0;
	if( GPspMixThread >= 0 )
	{
		GPspMixRun = 0;
		sceKernelWaitThreadEnd( GPspMixThread, NULL );
		sceKernelDeleteThread( GPspMixThread );
		GPspMixThread = -1;
	}
	if( GPspMixChan >= 0 ) { sceAudioChRelease( GPspMixChan ); GPspMixChan = -1; }
	GPspMix = NULL;
	// The shared block and ring are left allocated: the Media Engine loop may
	// still hold their addresses (it is never stopped).
}
#endif
