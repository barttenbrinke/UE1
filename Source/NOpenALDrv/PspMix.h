/*=============================================================================
	PspMix.h: a small integer sound mixer for the PSP.

	openal-soft 1.6 (the SDK's) spent 25-57% of the main CPU on a handful of
	voices, above the game thread in priority, and starved every Memory Stick
	wait behind it. This replaces it: a voice table in uncached memory that
	the game writes and a mixer reads, one block (1024 stereo frames at 44100
	Hz) at a time. The Media Engine mixes when it is up (inside the music
	render loop, see NOpenALDrv.cpp); otherwise a blocking CPU thread does it
	right before handing each block to the hardware. Either way the main CPU
	never polls: the output thread sleeps in sceAudioOutputBlocking.

	PspMixBlock() runs on the Media Engine as well: no syscalls, no libc, no
	floating point (the ME has no VFPU and its FPU is best left alone), and
	every pointer it follows is an uncached alias.
=============================================================================*/
#pragma once

#include <psptypes.h>

enum
{
	PSP_MIX_VOICES = 32,      // 31 effect voices + the music stream
	PSP_MIX_FRAMES = 1024,    // per block: 23 ms at 44100 Hz
	PSP_MIX_BLOCKS = 4,       // ring depth; the mixer stays one block behind the consumer
	PSP_MIX_RATE   = 44100,
};

enum
{
	PSP_MIXF_PLAYING = 1,
	PSP_MIXF_LOOP    = 2,
	PSP_MIXF_16BIT   = 4,
	PSP_MIXF_STEREO  = 8,
	PSP_MIXF_PAUSED  = 16,
};

// One voice. The game (CPU) owns everything but Pos while a voice plays;
// the mixer advances Pos and clears PLAYING at the end of a non-looping
// sample, or swaps in Next if one is queued (music streaming), bumping Done.
struct FPspMixVoice
{
	volatile u32 Data;        // uncached address of the samples, 0 = none
	volatile u32 Frames;      // sample frames in Data
	volatile u32 Pos;         // 16.16 frame position
	volatile u32 Step;        // 16.16 source frames per output frame
	volatile u32 GainL;       // Q12: 4096 = unity
	volatile u32 GainR;
	volatile u32 Flags;       // PSP_MIXF_*
	volatile u32 Done;        // buffers finished (streaming)
	volatile u32 NextData;    // queued follow-on buffer (streaming), 0 = none
	volatile u32 NextFrames;
	volatile u32 NextFlags;   // format bits for Next
	volatile u32 Ended;       // mixer -> CPU: the sample ran out (non-looping, nothing queued); cleared by the CPU on (re)start
	volatile u32 CurFmt;      // format bits of the buffer being mixed: set by the CPU at start, by the mixer on a Next swap
};
// Ownership: the CPU writes Flags, Step, GainL/R, Data/Frames/Next* and clears
// Ended when it starts a voice; the mixer writes Pos, Done, Ended and, on a
// streaming swap, Data/Frames/NextData/CurFmt. The mixer never writes Flags:
// the first version wrote the whole word back after each block and a stop
// issued by the CPU in that window was lost, leaving a looping voice that
// nobody owned any more (the shouts that never ended).

struct FPspMixShared
{
	volatile u32 Enable;      // 1: the Media Engine mixes; 0: the CPU output thread does
	volatile u32 Write;       // blocks mixed
	volatile u32 Read;        // blocks played
	volatile u32 Ring;        // uncached address of PSP_MIX_BLOCKS blocks of PSP_MIX_FRAMES stereo int16
	volatile u32 Blocks;      // blocks mixed by the ME in total (log)
	volatile u32 Underruns;   // blocks the output thread had to fill with silence (ME late)
	volatile u32 MeBusyUs;    // unused for now
	volatile u32 Pad;
	FPspMixVoice Voices[PSP_MIX_VOICES];
};

extern FPspMixShared* GPspMix;   // uncached alias; NULL until PspMixInit

UBOOL PspMixInit();
void  PspMixShutdown();
void  PspMixBlock( FPspMixShared* Mix, short* Out, int* Acc );
