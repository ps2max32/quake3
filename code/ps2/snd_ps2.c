/* Sound: ioquake3's mixer (client/snd_dma.c, snd_mix.c) into a ring here,
   22050 Hz 16-bit stereo, and from it into maxsdk's PCM stream (mx/snd.h:
   mxsnd, which plays it at 48 kHz). SNDDMA_Submit sends what the mixer has
   painted since the last call (up to s_paintedtime), as far as the stream
   has room; SNDDMA_GetDMAPos reports what has been sent as played. The
   sound plays the stream's ring later (about 100 ms).

   S_GetSoundtime starts each mix at the reported position plus
   s_mixPreStep: sys_ps2.c sets that to 0, so the mix starts where the
   sending stopped, and s_mixahead to 0.14 s, more than the stream's ring
   takes.

   s_ps2hw 1 (the default; below): no mixer. The sounds are SPU ADPCM in SPU
   RAM, played by the SPU2's voices; the music goes into the stream as it
   is. */

#include <mx/snd.h>

#include "../client/snd_local.h"
#include "../client/snd_codec.h"

#define RATE 22050
#define FRAMES 8192 /* the ring: 0.37 s, a power of two */

static short ring[FRAMES * 2] __attribute__((aligned(64)));
static int sent;    /* frames sent to the stream, as s_paintedtime counts them */
static int painted; /* frames the mixer has painted into the ring */

static void hw_reset(void);
static void hw_free_all(void);

qboolean SNDDMA_Init(void)
{
	ps2_sndhw = Cvar_Get("s_ps2hw", "1", CVAR_ARCHIVE | CVAR_LATCH)->integer != 0;
	if (mx_snd_init() < 0 || mx_snd_stream_format(RATE, 16, 2) < 0) {
		Com_Printf("SNDDMA_Init: no sound module\n");
		return qfalse;
	}
	hw_reset();
	dma.channels = 2;
	dma.samplebits = 16;
	dma.isfloat = 0;
	dma.speed = RATE;
	dma.samples = FRAMES * 2;
	dma.fullsamples = FRAMES;
	dma.submission_chunk = 1;
	dma.buffer = (byte *)ring;
	sent = painted = 0;
	if (ps2_sndhw)
		Com_Printf("Sound: the SPU2's voices (s_ps2hw 1), music through mxsnd's stream\n");
	else
		Com_Printf("Sound: %d Hz 16-bit stereo through mxsnd\n", RATE);
	return qtrue;
}

/* Sends ring[sent .. painted) as far as the stream has room. */
static void send(void)
{
	int n = painted - sent;
	if (n <= 0)
		return;
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

/* What has been sent counts as played. S_Update_ mixes only when this has
   moved since its last call (and then calls SNDDMA_Submit), and before that
   S_GetSoundtime sets s_paintedtime back to it: so what was painted and not
   taken (the stream's ring full) is remembered here (painted) and sent
   now, or the position would stay and the sound stop for good. */
int SNDDMA_GetDMAPos(void)
{
	send();
	return (sent * 2) & (FRAMES * 2 - 1);
}

void SNDDMA_BeginPainting(void)
{
}

void SNDDMA_Submit(void)
{
	if (s_paintedtime < sent) /* snd_dma.c chopped its counters */
		sent = s_paintedtime;
	painted = s_paintedtime;
	send();
}

void SNDDMA_Shutdown(void)
{
	if (ps2_sndhw) {
		S_PS2_StopAll();
		hw_free_all();
	}
	mx_snd_stream_stop();
}

void SNDDMA_StartCapture(void) {}
int SNDDMA_AvailableCaptureSamples(void) { return 0; }
void SNDDMA_Capture(int samples, byte *data) {}
void SNDDMA_StopCapture(void) {}
void SNDDMA_MasterGain(float val) {}

/* ---- the SPU2's voices (s_ps2hw 1) ---------------------------------------------

   snd_dma.c keeps its channels (who plays what, how loud on each side, the
   looping sounds merged by sound) and S_PS2_Update takes them to mxsnd's
   channels once a frame, in place of S_PaintChannels: a channel started
   since the last frame is played, the others get their volumes, a looping
   sound not asked for again is stopped. A sound is kept here as ADPCM at
   its own rate (the voice's pitch plays it; a third and a half of its 16-bit
   samples, pageable) and copied into SPU RAM when it first plays, a second
   time with the loop's flags when it first loops: a game's sounds are about
   twice SPU RAM's 1.8 MiB. When SPU RAM is full the sounds used longest ago
   leave it. */

int ps2_sndhw;

_Static_assert(sizeof(struct mx_snd_sample) == sizeof(((sfx_t *)0)->ps2Sample[0]), "sfx_t's ps2Sample");
#define SAMPLE(sfx, loop) ((struct mx_snd_sample *)(sfx)->ps2Sample[loop])

#define HW_SFX 4096
static sfx_t *hw_all[HW_SFX];    /* the sounds with their ADPCM here */
static sfx_t *hw_loaded[HW_SFX]; /* those with a sample in SPU RAM */
static int hw_nall, hw_nloaded;
static unsigned hw_bytes, hw_all_bytes, hw_encode_ms, hw_evicted, hw_file_ms, hw_spu_ms, hw_ups, hw_packed;

static struct {
	int chan;        /* mxsnd's channel, 0: none */
	int left, right; /* its volumes */
} hw_ch[MAX_CHANNELS];
#define HW_LOOPS 32
static struct {
	sfx_t *sfx;
	int chan, left, right, seen;
	unsigned rate;
} hw_loop[HW_LOOPS];
static int hw_rate, hw_width, hw_channels; /* the stream's format */

static void hw_reset(void)
{
	Com_Memset(hw_ch, 0, sizeof hw_ch);
	Com_Memset(hw_loop, 0, sizeof hw_loop);
	hw_rate = hw_width = hw_channels = 0;
}

/* the sound used longest ago leaves SPU RAM (its ADPCM stays here); qfalse:
   none is left but keep */
static qboolean hw_evict(const sfx_t *keep)
{
	int best = -1;
	for (int i = 0; i < hw_nloaded; i++)
		if (hw_loaded[i] != keep && (best < 0 || hw_loaded[i]->lastTimeUsed < hw_loaded[best]->lastTimeUsed))
			best = i;
	if (best < 0)
		return qfalse;
	sfx_t *sfx = hw_loaded[best];
	for (int k = 0; k < 2; k++)
		if (SAMPLE(sfx, k)->spu) {
			hw_bytes -= SAMPLE(sfx, k)->size;
			mx_snd_free(SAMPLE(sfx, k));
			Com_Memset(SAMPLE(sfx, k), 0, sizeof(struct mx_snd_sample));
		}
	hw_evicted++;
	hw_loaded[best] = hw_loaded[--hw_nloaded];
	return qtrue;
}

/* sfx's sample in SPU RAM, played once or looping (the same ADPCM with the
   loop's flags in its first and last blocks), or NULL */
static struct mx_snd_sample *hw_sample(sfx_t *sfx, int loop)
{
	struct mx_snd_sample *s = SAMPLE(sfx, loop);
	if (s->spu)
		return s;
	if (!sfx->ps2Adpcm || sfx->ps2Bytes < 16)
		return NULL;
	int t0 = Sys_Milliseconds();
	byte *a = sfx->ps2Adpcm;
	byte first = a[1], last = a[sfx->ps2Bytes - 15];
	if (loop)
		a[1] = first | 4, a[sfx->ps2Bytes - 15] = (last & ~3) | 3;
	qboolean ok;
	while (!(ok = mx_snd_load(s, a, (u32)sfx->ps2Bytes, (u32)sfx->ps2Rate) == 0) && hw_evict(sfx))
		;
	a[1] = first, a[sfx->ps2Bytes - 15] = last;
	hw_spu_ms += (unsigned)(Sys_Milliseconds() - t0);
	if (!ok) {
		Com_Memset(s, 0, sizeof *s);
		return NULL;
	}
	hw_ups++;
	hw_bytes += (unsigned)sfx->ps2Bytes;
	int in = 0;
	for (int i = 0; i < hw_nloaded; i++)
		in |= hw_loaded[i] == sfx;
	if (!in && hw_nloaded < HW_SFX)
		hw_loaded[hw_nloaded++] = sfx;
	return s;
}

/* The sound's ADPCM: <name>.adp of the PS2's sound pak (tools/ps2snd.py:
   "ADP1", the rate, the samples, then the blocks), else its file encoded
   here (a second of sound takes about 60 ms). */
#define ADP_HEAD 12
qboolean S_PS2_LoadSound(sfx_t *sfx)
{
	char name[MAX_QPATH];
	byte *buf = NULL;
	int rate = 0, n = 0, bytes = 0;

	sfx->lastTimeUsed = Com_Milliseconds() + 1;
	sfx->soundData = NULL;
	if (hw_nall == HW_SFX)
		return qfalse;
	int t0 = Sys_Milliseconds();
	COM_StripExtension(sfx->soundName, name, sizeof name);
	Q_strcat(name, sizeof name, ".adp");
	void *file;
	int len = FS_ReadFile(name, &file);
	if (file) {
		const byte *f = file;
		if (len > ADP_HEAD + 16 && !memcmp(f, "ADP1", 4) && !((len - ADP_HEAD) & 15)) {
			memcpy(&rate, f + 4, 4), memcpy(&n, f + 8, 4);
			bytes = len - ADP_HEAD;
			if ((buf = malloc((size_t)bytes)) != NULL)
				memcpy(buf, f + ADP_HEAD, (size_t)bytes);
			hw_packed++;
		}
		FS_FreeFile(file);
		hw_file_ms += (unsigned)(Sys_Milliseconds() - t0);
	}
	if (!buf) {
		snd_info_t info;
		byte *data = S_CodecLoad(sfx->soundName, &info);
		int t1 = Sys_Milliseconds();
		hw_file_ms += (unsigned)(t1 - t0);
		if (!data)
			return qfalse;
		n = info.samples, rate = info.rate;
		int nc = info.channels;
		if (n > 0 && nc > 0 && rate > 0 && (buf = malloc(MX_SND_ADPCM_SIZE((size_t)n))) != NULL) {
			short *pcm = Hunk_AllocateTempMemory(n * sizeof(short));
			const byte *src = data + info.dataofs;
			for (int i = 0; i < n; i++) {
				int v = 0;
				for (int c = 0; c < nc; c++)
					v += info.width == 2 ? ((const short *)src)[i * nc + c] : ((int)src[i * nc + c] - 128) << 8;
				pcm[i] = (short)(v / nc);
			}
			bytes = (int)mx_snd_encode(pcm, (u32)n, buf, 0);
			Hunk_FreeTempMemory(pcm);
		}
		Hunk_FreeTempMemory(data);
		hw_encode_ms += (unsigned)(Sys_Milliseconds() - t1);
	}
	if (!buf || rate <= 0)
		return qfalse;
	sfx->ps2Adpcm = buf, sfx->ps2Bytes = bytes, sfx->ps2Rate = rate;
	Com_Memset(sfx->ps2Sample, 0, sizeof sfx->ps2Sample);
	/* its length as the channels count it: in dma.speed's samples */
	sfx->soundLength = (int)((long long)n * dma.speed / rate) + 1;
	sfx->soundChannels = 1;
	sfx->soundCompressionMethod = 0;
	hw_all[hw_nall++] = sfx;
	hw_all_bytes += (unsigned)bytes;
	return qtrue;
}

/* the sound system ends: the sounds' memory here and in SPU RAM */
static void hw_free_all(void)
{
	for (int i = 0; i < hw_nall; i++) {
		sfx_t *sfx = hw_all[i];
		for (int k = 0; k < 2; k++)
			if (SAMPLE(sfx, k)->spu)
				mx_snd_free(SAMPLE(sfx, k));
		Com_Memset(sfx->ps2Sample, 0, sizeof sfx->ps2Sample);
		free(sfx->ps2Adpcm);
		sfx->ps2Adpcm = NULL, sfx->ps2Bytes = 0;
		sfx->inMemory = qfalse;
	}
	hw_nall = hw_nloaded = 0;
	hw_bytes = hw_all_bytes = 0;
}

static int hw_vol(int v, float scale)
{
	int r = (int)((float)v * scale);
	return r < 0 ? 0 : r > MX_SND_VOL_MAX ? MX_SND_VOL_MAX : r;
}

/* once in a while, when more was loaded: what the sounds take and cost */
static void hw_report(void)
{
	static unsigned told, frames;
	if (++frames % 256 || hw_ups + (unsigned)hw_nall == told)
		return;
	told = hw_ups + (unsigned)hw_nall;
	Com_Printf("Sound: %d sounds, %u KB (%u from the pak); files %u ms, encoding %u ms; SPU RAM: %d in %u KB, "
	           "%u copies in %u ms, %u dropped\n", hw_nall, hw_all_bytes / 1024, hw_packed, hw_file_ms, hw_encode_ms,
	           hw_nloaded, hw_bytes / 1024, hw_ups, hw_spu_ms, hw_evicted);
}

void S_PS2_Update(void)
{
	hw_report();
	/* the channels' clock: real time in dma.speed's samples */
	s_soundtime = s_paintedtime = (int)((long long)Sys_Milliseconds() * dma.speed / 1000);
	/* the mixer's full scale is 255 times s_volume */
	float scale = s_muted->integer ? 0.0f : s_volume->value * (float)MX_SND_VOL_MAX / 255.0f;

	channel_t *ch = s_channels;
	for (int i = 0; i < MAX_CHANNELS; i++, ch++) {
		sfx_t *sfx = ch->thesfx;
		if (!sfx)
			continue;
		int left = hw_vol(ch->leftvol, scale), right = hw_vol(ch->rightvol, scale);
		if (ch->startSample == START_SAMPLE_IMMEDIATE) {
			/* started since the last frame; what this channel played before ends */
			if (hw_ch[i].chan)
				mx_snd_ch_stop(hw_ch[i].chan);
			hw_ch[i].chan = 0;
			if (!sfx->inMemory && !sfx->defaultSound)
				S_memoryLoad(sfx);
			const struct mx_snd_sample *sm = hw_sample(sfx, 0);
			if (sm)
				hw_ch[i].chan = mx_snd_ch_play(sm, (u32)sfx->ps2Rate, left, right, ch->fullVolume ? 200 : 128);
			hw_ch[i].left = left, hw_ch[i].right = right;
			ch->startSample = s_paintedtime;
			continue;
		}
		/* over (an eighth of a second later: the voice ends by itself) */
		if (ch->startSample + sfx->soundLength + dma.speed / 8 <= s_paintedtime) {
			S_ChannelFree(ch);
			hw_ch[i].chan = 0;
			continue;
		}
		if (hw_ch[i].chan && (left != hw_ch[i].left || right != hw_ch[i].right)) {
			if (mx_snd_ch_set(hw_ch[i].chan, (u32)sfx->ps2Rate, left, right) < 0)
				hw_ch[i].chan = 0;
			hw_ch[i].left = left, hw_ch[i].right = right;
		}
	}

	/* the looping sounds asked for this frame (S_AddLoopSounds) */
	for (int k = 0; k < HW_LOOPS; k++)
		hw_loop[k].seen = 0;
	ch = loop_channels;
	for (int i = 0; i < numLoopChannels; i++, ch++) {
		sfx_t *sfx = ch->thesfx;
		if (!sfx || sfx->defaultSound)
			continue;
		int left = hw_vol(ch->leftvol, scale), right = hw_vol(ch->rightvol, scale);
		int at = -1, room = -1;
		for (int k = 0; k < HW_LOOPS && at < 0; k++) {
			if (hw_loop[k].sfx == sfx && !hw_loop[k].seen)
				at = k;
			else if (!hw_loop[k].sfx && room < 0)
				room = k;
		}
		if (at < 0 && room < 0)
			continue;
		if (!sfx->inMemory)
			S_memoryLoad(sfx);
		const struct mx_snd_sample *sm = hw_sample(sfx, 1);
		if (!sm)
			continue;
		unsigned rate = (unsigned)sfx->ps2Rate;
		if (ch->doppler && ch->dopplerScale > 0.0f)
			rate = (unsigned)((float)rate * ch->dopplerScale);
		if (at >= 0 && mx_snd_ch_playing(hw_loop[at].chan)) {
			if (left != hw_loop[at].left || right != hw_loop[at].right || rate != hw_loop[at].rate)
				mx_snd_ch_set(hw_loop[at].chan, rate, left, right);
		} else {
			if (at < 0)
				at = room;
			hw_loop[at].sfx = sfx;
			hw_loop[at].chan = mx_snd_ch_play(sm, rate, left, right, 64);
		}
		hw_loop[at].left = left, hw_loop[at].right = right, hw_loop[at].rate = rate;
		hw_loop[at].seen = 1;
	}
	for (int k = 0; k < HW_LOOPS; k++)
		if (hw_loop[k].sfx && !hw_loop[k].seen) {
			if (hw_loop[k].chan)
				mx_snd_ch_stop(hw_loop[k].chan);
			hw_loop[k].sfx = NULL, hw_loop[k].chan = 0;
		}
	mx_snd_commit();
}

void S_PS2_StopAll(void)
{
	for (int i = 0; i < MAX_CHANNELS; i++)
		if (hw_ch[i].chan)
			mx_snd_ch_stop(hw_ch[i].chan);
	for (int k = 0; k < HW_LOOPS; k++)
		if (hw_loop[k].chan)
			mx_snd_ch_stop(hw_loop[k].chan);
	hw_reset();
	mx_snd_stream_stop();
	mx_snd_commit();

}

/* the stream in the music's format; the bytes it takes now */
int S_PS2_StreamRoom(int rate, int width, int channels)
{
	if (rate != hw_rate || width != hw_width || channels != hw_channels) {
		if (mx_snd_stream_format((u32)rate, width * 8, channels) < 0)
			return 0;
		hw_rate = rate, hw_width = width, hw_channels = channels;
	}
	return (int)mx_snd_stream_room();
}

void S_PS2_RawSamples(int samples, int width, int channels, const byte *data, float volume)
{
	if (width != hw_width || channels != hw_channels)
		return; /* S_PS2_StreamRoom sets the format: the music's only */
	mx_snd_stream_volume(hw_vol(MX_SND_VOL_MAX, volume));
	mx_snd_stream_queue(data, (u32)(samples * width * channels));
}
