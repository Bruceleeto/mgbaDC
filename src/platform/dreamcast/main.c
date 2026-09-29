/* Native KallistiOS frontend. Video is a linear RGB565 PVR texture. */
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba/core/profile.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#ifdef DC_JIT
#include <mgba/internal/arm/jit.h>
#endif

#include <mgba-util/vfs.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <dc/sound/stream.h>
#ifdef M_PROFILE
#include <dc/perfctr.h>
#endif

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
static uint64_t profileWait, profileUpload, profileSubmit;

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

#ifdef M_PROFILE
#define PROFILE_CYCLES_PER_MS 200000.0

/* Low words of performance counters PRFC0 and PRFC1 (SH7750 PMCTR1L/PMCTR2L). */
static uint32_t profileCycles(void) {
	return *(volatile uint32_t*) 0xFF100008;
}

static uint32_t profileStallCycles(void) {
	return *(volatile uint32_t*) 0xFF100010;
}
#endif

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

static void pushAudio(int16_t left, int16_t right) {
	if (audioCount == AUDIO_FRAMES) {
		audioRead = (audioRead + 1) % AUDIO_FRAMES;
		--audioCount;
	}
	audioRing[audioWrite][0] = left;
	audioRing[audioWrite][1] = right;
	audioWrite = (audioWrite + 1) % AUDIO_FRAMES;
	++audioCount;
}

/* The core mixes at SAMPLE_RATE unless the game changes SOUNDBIAS's
 * resolution: then its samples come through blip_buf instead. */
static void directAudio(void* context, const struct mStereoSample* samples, int count) {
	(void) context;
	for (int i = 0; i < count; ++i) {
		pushAudio(samples[i].left, samples[i].right);
	}
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
			pushAudio(samples[i][0], samples[i][1]);
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
	uint64_t start = timer_us_gettime64();
	pvr_wait_ready();
	uint64_t ready = timer_us_gettime64();
	pvr_txr_load(pixels, texture, TEXTURE_SIZE * height * sizeof(color_t));
	uint64_t uploaded = timer_us_gettime64();
	profileWait += ready - start;
	profileUpload += uploaded - ready;
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
	profileSubmit += timer_us_gettime64() - uploaded;
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
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
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
#ifdef DC_JIT
	if (!ARMJITInit(core->cpu)) {
		printf("mgba-dc: JIT init failed, using the interpreter\n");
	}
#endif
	core->reset(core);
	/* Benchmarking: start from a savestate when the media carries one. */
	char statePath[32];
	snprintf(statePath, sizeof(statePath), "%s/test.ss", root);
	struct VFile* stateFile = VFileOpen(statePath, O_RDONLY);
	if (stateFile) {
		if (mCoreLoadStateNamed(core, stateFile, 0)) printf("mgba-dc: started from %s\n", statePath);
		stateFile->close(stateFile);
	}
	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);
	if (core->platform(core) == mPLATFORM_GBA) {
		struct GBAAudio* gbaAudio = &((struct GBA*) core->board)->audio;
		gbaAudio->directOutput = directAudio;
		gbaAudio->directInterval = GBA_ARM7TDMI_FREQUENCY / SAMPLE_RATE;
	}
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
#ifdef M_PROFILE
	/* PRFC0 counts CPU cycles for KOS's ns timer; the low word alone is fine
	 * for the profiler's short, wrapping intervals. PRFC1 alternates between
	 * instruction- and data-cache miss stall cycles, one per report. */
	if (!perf_cntr_timer_enabled()) perf_cntr_timer_enable();
	static const struct {
		perf_cntr_event_t mode;
		const char* label;
	} profileStalls[] = {
		{ PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE, "I$ stall" },
		{ PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, "D$ stall" },
	};
	unsigned profileStall = 0;
	perf_cntr_start(PRFC1, profileStalls[profileStall].mode, PMCR_COUNT_CPU_CYCLES);
	mProfileClock = profileCycles;
	mProfileEvents = profileStallCycles;
#endif
	uint64_t profileStart = timer_us_gettime64();
	uint64_t profileInput = 0, profileCore = 0, profileAudio = 0;
	unsigned profileFrames = 0;
	while (true) {
		uint64_t start = timer_us_gettime64();
		if (!updateInput(core)) break;
		uint64_t inputDone = timer_us_gettime64();
		core->runFrame(core);
		uint64_t coreDone = timer_us_gettime64();
		collectAudio(core);
		snd_stream_poll(sound);
		uint64_t audioDone = timer_us_gettime64();
		present(texture, &header, width, height);
		uint64_t now = timer_us_gettime64();
		profileInput += inputDone - start;
		profileCore += coreDone - inputDone;
		profileAudio += audioDone - coreDone;
		++profileFrames;
		if (now - profileStart >= 1000000) {
			double divisor = profileFrames * 1000.0;
			printf("FPS %.1f | ms/frame: input %.2f core %.2f audio %.2f PVR-wait %.2f upload %.2f submit %.2f\n",
			       profileFrames * 1000000.0 / (now - profileStart),
			       profileInput / divisor, profileCore / divisor, profileAudio / divisor,
			       profileWait / divisor, profileUpload / divisor, profileSubmit / divisor);
#ifdef DC_JIT
			struct ARMJITStats jitStats;
			ARMJITGetStats(core->cpu, &jitStats);
			printf("  JIT: %u blocks, %u insns, %u code bytes, %u flushes, %u invalidations, %u fallback steps\n",
			       (unsigned) jitStats.blocksCompiled, (unsigned) jitStats.guestInsnsCompiled,
			       (unsigned) jitStats.codeBytes, (unsigned) jitStats.flushes,
			       (unsigned) jitStats.invalidations, (unsigned) jitStats.fallbackSteps);
#ifdef M_ARM_JIT_FASTMEM
			{
				extern uint32_t fastmem_count;
				static uint32_t lastFaults;
				printf("  fastmem: %u faults/frame\n", (unsigned) ((fastmem_count - lastFaults) / profileFrames));
				lastFaults = fastmem_count;
			}
#else
			printf("  fastmem: off\n");
#endif
#endif
#ifdef M_PROFILE
			printf("  -- %s column: %% of each section's cycles stalled on %s misses --\n",
			       profileStalls[profileStall].label,
			       profileStall ? "data cache" : "instruction cache");
			mProfilePrint(profileCore / divisor, profileFrames, PROFILE_CYCLES_PER_MS,
			              profileStalls[profileStall].label);
			profileStall = (profileStall + 1) % (sizeof(profileStalls) / sizeof(profileStalls[0]));
			perf_cntr_start(PRFC1, profileStalls[profileStall].mode, PMCR_COUNT_CPU_CYCLES);
#endif
			profileStart = timer_us_gettime64();
			profileFrames = 0;
			profileInput = profileCore = profileAudio = 0;
			profileWait = profileUpload = profileSubmit = 0;
		}
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
