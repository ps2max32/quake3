/* Sound: ioquake3's mixer (client/snd_dma.c, snd_mix.c) into a ring here,
   22050 Hz 16-bit stereo, and from it into maxsdk's PCM stream (mx/snd.h:
   audsrv, which plays it at 48 kHz). SNDDMA_Submit sends what the mixer has
   painted since the last call (up to s_paintedtime), as far as the stream
   has room; SNDDMA_GetDMAPos reports what has been sent as played. The
   sound plays the stream's ring later (about 100 ms).

   S_GetSoundtime starts each mix at the reported position plus
   s_mixPreStep: sys_ps2.c sets that to 0, so the mix starts where the
   sending stopped, and s_mixahead to 0.14 s, more than the stream's ring
   takes. */

#include <mx/snd.h>

#include "../client/snd_local.h"

#define RATE 22050
#define FRAMES 8192 /* the ring: 0.37 s, a power of two */

static short ring[FRAMES * 2] __attribute__((aligned(64)));
static int sent; /* frames sent to the stream, as s_paintedtime counts them */

qboolean SNDDMA_Init(void)
{
	if (mx_snd_init() < 0 || mx_snd_stream_format(RATE, 16, 2) < 0) {
		Com_Printf("SNDDMA_Init: no audsrv\n");
		return qfalse;
	}
	dma.channels = 2;
	dma.samplebits = 16;
	dma.isfloat = 0;
	dma.speed = RATE;
	dma.samples = FRAMES * 2;
	dma.fullsamples = FRAMES;
	dma.submission_chunk = 1;
	dma.buffer = (byte *)ring;
	sent = 0;
	Com_Printf("Sound: %d Hz 16-bit stereo through audsrv\n", RATE);
	return qtrue;
}

int SNDDMA_GetDMAPos(void)
{
	return (sent * 2) & (FRAMES * 2 - 1);
}

void SNDDMA_BeginPainting(void)
{
}

void SNDDMA_Submit(void)
{
	if (s_paintedtime < sent) /* snd_dma.c chopped its counters */
		sent = s_paintedtime;
	int n = s_paintedtime - sent;
	int room = (int)(mx_snd_stream_room() / 4);
	if (n > room)
		n = room;
	while (n > 0) {
		int at = sent & (FRAMES - 1);
		int k = FRAMES - at < n ? FRAMES - at : n;
		int took = (int)(mx_snd_stream_queue(ring + at * 2, (u32)k * 4) / 4);
		sent += took;
		n -= took;
		if (took < k)
			break;
	}
}

void SNDDMA_Shutdown(void)
{
	mx_snd_stream_stop();
}

void SNDDMA_StartCapture(void) {}
int SNDDMA_AvailableCaptureSamples(void) { return 0; }
void SNDDMA_Capture(int samples, byte *data) {}
void SNDDMA_StopCapture(void) {}
void SNDDMA_MasterGain(float val) {}
