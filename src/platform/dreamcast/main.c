/* Native KallistiOS frontend. Video is a linear RGB565 PVR texture. */
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba/internal/gba/input.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <dc/sound/stream.h>

#define TEXTURE_SIZE 256
#define SAMPLE_RATE 32768
#define AUDIO_FRAMES 8192
#define STREAM_BYTES 8192

KOS_INIT_FLAGS(INIT_DEFAULT);

/* The core and snd_stream_poll run on the same thread. No audio callback
 * accesses the core, and no locks or framebuffer copies are needed. */
static color_t pixels[TEXTURE_SIZE * TEXTURE_SIZE] __attribute__((aligned(32)));
static int16_t audioRing[AUDIO_FRAMES][2];
static int16_t audioOutput[AUDIO_FRAMES][2] __attribute__((aligned(32)));
static unsigned audioRead, audioWrite, audioCount;

static void logMessage(struct mLogger* logger, int category, enum mLogLevel level,
                       const char* format, va_list args) {
	(void) logger;
	/* The core's fallback logger prints every level. Console forwarding over
	 * dc-load is expensive, so keep normal runs to warnings and errors. */
	if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) return;
	printf("%s: ", mLogCategoryName(category));
	vprintf(format, args);
	printf("\n");
}

static struct mLogger logger = { .log = logMessage };

static void* audioCallback(snd_stream_hnd_t handle, int requested, int* received) {
	(void) handle;
	if (requested > AUDIO_FRAMES) {
		requested = AUDIO_FRAMES;
	}
	memset(audioOutput, 0, requested * sizeof(audioOutput[0]));
	for (int i = 0; i < requested && audioCount; ++i) {
		audioOutput[i][0] = audioRing[audioRead][0];
		audioOutput[i][1] = audioRing[audioRead][1];
		audioRead = (audioRead + 1) % AUDIO_FRAMES;
		--audioCount;
	}
	*received = requested;
	return audioOutput;
}

static void collectAudio(struct mCore* core) {
	struct blip_t* left = core->getAudioChannel(core, 0);
	struct blip_t* right = core->getAudioChannel(core, 1);
	int16_t samples[1024][2];
	int count;
	while ((count = blip_samples_avail(left)) > 0) {
		if (count > blip_samples_avail(right)) {
			count = blip_samples_avail(right);
		}
		if (!count) {
			break;
		}
		if (count > 1024) {
			count = 1024;
		}
		blip_read_samples(left, &samples[0][0], count, 1);
		blip_read_samples(right, &samples[0][1], count, 1);
		for (int i = 0; i < count; ++i) {
			if (audioCount == AUDIO_FRAMES) {
				audioRead = (audioRead + 1) % AUDIO_FRAMES;
				--audioCount;
			}
			audioRing[audioWrite][0] = samples[i][0];
			audioRing[audioWrite][1] = samples[i][1];
			audioWrite = (audioWrite + 1) % AUDIO_FRAMES;
			++audioCount;
		}
	}
}

static bool updateInput(struct mCore* core) {
	maple_device_t* device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
	cont_state_t* state = device ? maple_dev_status(device) : NULL;
	uint32_t keys = 0;
	if (state) {
		uint32_t buttons = state->buttons;
		if ((buttons & CONT_RESET_BUTTONS) == CONT_RESET_BUTTONS) {
			return false;
		}
		if (buttons & CONT_A) keys |= 1 << GBA_KEY_A;
		if (buttons & CONT_B) keys |= 1 << GBA_KEY_B;
		if (buttons & CONT_X) keys |= 1 << GBA_KEY_SELECT;
		if (buttons & CONT_START) keys |= 1 << GBA_KEY_START;
		if (buttons & CONT_DPAD_UP) keys |= 1 << GBA_KEY_UP;
		if (buttons & CONT_DPAD_DOWN) keys |= 1 << GBA_KEY_DOWN;
		if (buttons & CONT_DPAD_LEFT) keys |= 1 << GBA_KEY_LEFT;
		if (buttons & CONT_DPAD_RIGHT) keys |= 1 << GBA_KEY_RIGHT;
		if (state->ltrig > 32) keys |= 1 << GBA_KEY_L;
		if (state->rtrig > 32) keys |= 1 << GBA_KEY_R;
	}
	core->setKeys(core, keys);
	return true;
}

static void present(pvr_ptr_t texture, const pvr_poly_hdr_t* header,
                    unsigned width, unsigned height) {
	/* Finish the preceding render before overwriting its texture. The padded
	 * stride lets KOS upload directly with store queues, without swizzling. */
	pvr_wait_ready();
	pvr_txr_load(pixels, texture, TEXTURE_SIZE * height * sizeof(color_t));
	pvr_scene_begin();
	pvr_list_begin(PVR_LIST_OP_POLY);
	pvr_prim(header, sizeof(*header));
	float scale = 640.0f / width;
	if (height * scale > 480.0f) scale = 480.0f / height;
	float x = (640.0f - width * scale) / 2;
	float y = (480.0f - height * scale) / 2;
	pvr_vertex_t vertex = {0};
	vertex.flags = PVR_CMD_VERTEX;
	vertex.z = 1.0f;
	vertex.argb = 0xFFFFFFFF;
	vertex.x = x;
	vertex.y = y;
	pvr_prim(&vertex, sizeof(vertex));
	vertex.x = x + width * scale;
	vertex.u = (float) width / TEXTURE_SIZE;
	pvr_prim(&vertex, sizeof(vertex));
	vertex.x = x;
	vertex.y = y + height * scale;
	vertex.u = 0;
	vertex.v = (float) height / TEXTURE_SIZE;
	pvr_prim(&vertex, sizeof(vertex));
	vertex.x = x + width * scale;
	vertex.u = (float) width / TEXTURE_SIZE;
	vertex.flags = PVR_CMD_VERTEX_EOL;
	pvr_prim(&vertex, sizeof(vertex));
	pvr_list_finish();
	pvr_scene_finish();
}

int main(int argc, char** argv) {
	(void) argc;
	(void) argv;
	setvbuf(stdout, NULL, _IONBF, 0);
	mLogSetDefaultLogger(&logger);
	static const char* const roots[] = { "/rd", "/pc", "/cd" };
	const char* root = NULL;
	char path[32];
	struct mCore* core = NULL;
	for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
		snprintf(path, sizeof(path), "%s/test.gba", roots[i]);
		core = mCoreFind(path);
		if (core) {
			root = roots[i];
			break;
		}
	}
	if (!core) {
		printf("mgba-dc: no supported ROM at /rd/test.gba, /pc/test.gba or /cd/test.gba\n");
		return 1;
	}
	if (!core->init(core)) {
		printf("mgba-dc: core initialization failed\n");
		free(core);
		return 1;
	}
	bool hostMedia = !strcmp(root, "/pc");
	mCoreConfigSetDreamcastMediaRoot(root);
	mCoreInitConfig(core, "dreamcast");
	mCoreConfigSetDefaultIntValue(&core->config, "volume", 0x100);
	mCoreConfigSetDefaultIntValue(&core->config, "useBios", 1);
	char biosPath[32];
	snprintf(biosPath, sizeof(biosPath), "%s/gba_bios.bin", root);
	mCoreConfigSetDefaultValue(&core->config, "bios", biosPath);
	mCoreLoadConfig(core);
	int result = 1;
	bool videoInitialized = false, audioInitialized = false;
	pvr_ptr_t texture = NULL;
	snd_stream_hnd_t sound = SND_STREAM_INVALID;
	if (!mCoreLoadFile(core, path)) {
		printf("mgba-dc: failed to load %s\n", path);
		goto cleanup;
	}
	/* Romdisk and disc media are read-only. Host-backed saves retain the .sav
	 * convention; VMU storage is not implemented yet. */
	if (hostMedia) mCoreAutoloadSave(core);
	unsigned width, height;
	core->desiredVideoDimensions(core, &width, &height);
	if (!width || !height || width > TEXTURE_SIZE || height > TEXTURE_SIZE) {
		printf("mgba-dc: unsupported video dimensions\n");
		goto cleanup;
	}
	core->setVideoBuffer(core, pixels, TEXTURE_SIZE);
	core->setAudioBufferSize(core, 2048);
	core->reset(core);
	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);
	vid_set_mode(DM_640x480, PM_RGB565);
	if (pvr_init_defaults() < 0) goto cleanup;
	videoInitialized = true;
	pvr_set_bg_color(0, 0, 0);
	texture = pvr_mem_malloc(sizeof(pixels));
	if (!texture) goto cleanup;
	pvr_poly_cxt_t context;
	pvr_poly_hdr_t header;
	pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
	                PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
	                TEXTURE_SIZE, TEXTURE_SIZE, texture, PVR_FILTER_NONE);
	pvr_poly_compile(&header, &context);
	if (snd_stream_init_ex(2, STREAM_BYTES) < 0) goto cleanup;
	audioInitialized = true;
	sound = snd_stream_alloc(audioCallback, STREAM_BYTES);
	if (sound == SND_STREAM_INVALID) goto cleanup;
	snd_stream_start(sound, SAMPLE_RATE, 1);
	printf("mgba-dc: native PVR/Maple/AICA frontend, ROM: %s\n", path);
	while (updateInput(core)) {
		core->runFrame(core);
		collectAudio(core);
		snd_stream_poll(sound);
		present(texture, &header, width, height);
	}
	result = 0;
cleanup:
	if (result) printf("mgba-dc: startup failed\n");
	if (sound != SND_STREAM_INVALID) {
		snd_stream_stop(sound);
		snd_stream_destroy(sound);
	}
	if (audioInitialized) snd_stream_shutdown();
	if (videoInitialized) {
		pvr_wait_ready();
		if (texture) pvr_mem_free(texture);
		pvr_shutdown();
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return result;
}
