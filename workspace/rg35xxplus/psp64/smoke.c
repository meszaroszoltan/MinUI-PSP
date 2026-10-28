// headless smoke test for ppsspp_libretro.so, mirrors the parts of minarch PSP relies on
// usage: smoke <core.so> <rom> <system_dir> <save_dir> <sw|gl> <frames> <default.cfg> <options.txt> <frame.bmp> <picture|toptext>
//   sw: SET_HW_RENDER is refused so the core falls back to its software renderer (XRGB8888 frames)
//   gl: GLES2 context (Mesa surfaceless) + fbo + glReadPixels, same contract as minarch's HW_* code
//   picture: expects a busy frame (eg. demos/cube.pbp)
//   toptext: expects debug text in the top left corner and nothing else, catches flipped frames (eg. demos/controller.pbp)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "libretro.h"

// _exit: the core's threads are still running, exit() would abort in their destructors
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); _exit(1); } while (0)

static int use_gl = 0;
static const char* system_dir;
static const char* save_dir;
static int pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

// core options
static FILE* options_file;
static int option_count = 0;
#define MAX_VARS 64
static char var_keys[MAX_VARS][128];
static char var_values[MAX_VARS][128];
static int var_count = 0;

// hw
static struct retro_hw_render_callback hw_cb;
static int hw_enabled = 0;
static EGLDisplay dpy;
static EGLContext ctx;
static GLuint fbo, tex, rb;
static int fbo_w, fbo_h;
static uint8_t* readback;

// frames
static uint16_t* frame;
static unsigned frame_w, frame_h;
static int frames = 0, hw_frames = 0, sw_frames = 0, dupes = 0;
static long convert_mismatches = 0;

static void log_cb(enum retro_log_level level, const char* fmt, ...) {
	if (level<RETRO_LOG_INFO) return;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static void loadConfig(const char* path) {
	FILE* file = fopen(path, "r");
	if (!file) FAIL("can't open %s", path);
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		char* key = line;
		if (*key=='-') key++; // locked
		if (strncmp(key, "ppsspp_", 7)) continue;
		char* eq = strstr(key, " = ");
		if (!eq || var_count>=MAX_VARS) continue;
		*eq = '\0';
		char* value = eq + 3;
		value[strcspn(value, "\r\n")] = '\0';
		strcpy(var_keys[var_count], key);
		strcpy(var_values[var_count], value);
		var_count += 1;
	}
	fclose(file);
	printf("smoke: %i ppsspp options from %s\n", var_count, path);
}

static void dumpOptions(const struct retro_core_option_definition* defs) {
	for (; defs && defs->key; defs++) {
		fprintf(options_file, "%s\t", defs->key);
		for (int i=0; i<RETRO_NUM_CORE_OPTION_VALUES_MAX && defs->values[i].value; i++) {
			fprintf(options_file, "%s%s", i ? "|" : "", defs->values[i].value);
		}
		fprintf(options_file, "\n");
		option_count += 1;
	}
	fflush(options_file);
}

static uintptr_t getCurrentFramebuffer(void) { return fbo; }
static retro_proc_address_t getProcAddress(const char* sym) { return (retro_proc_address_t)eglGetProcAddress(sym); }

static bool environment_callback(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = system_dir; return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = save_dir; return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = log_cb; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		int format = *(enum retro_pixel_format*)data;
		if (format!=RETRO_PIXEL_FORMAT_XRGB8888 && format!=RETRO_PIXEL_FORMAT_RGB565) return false;
		pixel_format = format;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		if (!use_gl) return false;
		*(unsigned*)data = RETRO_HW_CONTEXT_OPENGL; // same answer as minarch
		return true;
	case RETRO_ENVIRONMENT_SET_HW_RENDER: {
		struct retro_hw_render_callback* cb = data;
		printf("smoke: SET_HW_RENDER type %i (%u.%u) depth %i stencil %i bottom_left_origin %i\n", cb->context_type, cb->version_major, cb->version_minor, cb->depth, cb->stencil, cb->bottom_left_origin);
		if (!use_gl) return false;
		if (cb->context_type!=RETRO_HW_CONTEXT_OPENGLES2 && cb->context_type!=RETRO_HW_CONTEXT_OPENGLES3) return false;
		cb->get_current_framebuffer = getCurrentFramebuffer;
		cb->get_proc_address = getProcAddress;
		hw_cb = *cb;
		hw_enabled = 1;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT: return use_gl;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 1; return true; // same as minarch
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: dumpOptions(data); return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: dumpOptions(((const struct retro_core_options_intl*)data)->us); return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* var = data;
		var->value = NULL;
		for (int i=0; i<var_count; i++) {
			if (!strcmp(var->key, var_keys[i])) {
				var->value = var_values[i];
				return true;
			}
		}
		return false;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		return true;
	default: return false;
	}
}

static void frameRealloc(unsigned w, unsigned h) {
	static unsigned size = 0;
	if (w*h<=size) return;
	free(frame);
	size = w*h;
	frame = malloc(size*2);
}

static void video_callback(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data) { dupes++; return; }
	frames++;
	frameRealloc(width,height);

	if (data==RETRO_HW_FRAME_BUFFER_VALID) {
		if (!hw_enabled) FAIL("hw frame without hw render");
		hw_frames++;
		if (width>fbo_w) width = fbo_w;
		if (height>fbo_h) height = fbo_h;

		// same as minarch HW_readback
		GLint fb, align;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		glGetIntegerv(GL_PACK_ALIGNMENT, &align);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(0,0, width,height, GL_RGBA,GL_UNSIGNED_BYTE, readback);
		glPixelStorei(GL_PACK_ALIGNMENT, align);
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
		GLenum err = glGetError();
		if (err) FAIL("glReadPixels error 0x%x", err);

		uint16_t* dst = frame;
		for (int y=0; y<height; y++) {
			const uint8_t* src = readback + (hw_cb.bottom_left_origin ? height-1-y : y) * width * 4;
			for (int x=0; x<width; x++) {
				*dst++ = ((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3);
				src += 4;
			}
		}
	}
	else if (pixel_format==RETRO_PIXEL_FORMAT_XRGB8888) {
		sw_frames++;
		// same as minarch buffer_downsample, checked against a plain per-channel reference
		const uint32_t* input = data;
		uint16_t* output = frame;
		size_t extra = pitch / sizeof(uint32_t) - width;
		for (int y=0; y<height; y++) {
			for (int x=0; x<width; x++) {
				uint32_t p = *input;
				*output =  (p & 0xF80000) >> 8;
				*output |= (p & 0xFC00) >> 5;
				*output |= (p & 0xF8) >> 3;
				uint8_t r = p >> 16, g = p >> 8, b = p;
				uint16_t ref = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
				if (*output!=ref) convert_mismatches++;
				input++;
				output++;
			}
			input += extra;
		}
	}
	else {
		sw_frames++;
		for (int y=0; y<height; y++) memcpy(frame + y*width, (const uint8_t*)data + y*pitch, width*2);
	}
	frame_w = width;
	frame_h = height;
}
static void audio_callback(int16_t left, int16_t right) {}
static size_t audio_batch_callback(const int16_t* data, size_t count) { return count; }
static void input_poll_callback(void) {}
static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) { return 0; }

static void glInit(int w, int h) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, NULL, NULL)) FAIL("eglInitialize 0x%x", eglGetError());
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint config_attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE }; // surfaceless has no window configs
	EGLConfig config;
	EGLint count;
	if (!eglChooseConfig(dpy, config_attrs, &config, 1, &count) || !count) FAIL("eglChooseConfig 0x%x", eglGetError());
	int major = hw_cb.context_type==RETRO_HW_CONTEXT_OPENGLES3 ? 3 : 2;
	EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, major, EGL_NONE };
	ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
	if (!ctx) FAIL("eglCreateContext 0x%x", eglGetError());
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) FAIL("eglMakeCurrent 0x%x", eglGetError());
	printf("smoke: GL_VERSION: %s GL_RENDERER: %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

	// same as minarch HW_init + HW_allocFBO
	const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
	int packed = extensions && strstr(extensions, "GL_OES_packed_depth_stencil")!=NULL;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	if (hw_cb.depth || hw_cb.stencil) glGenRenderbuffers(1, &rb);

	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (rb) {
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, packed ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16, w, h);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
		if (packed) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
	}
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status!=GL_FRAMEBUFFER_COMPLETE) FAIL("incomplete framebuffer 0x%x", status);
	fbo_w = w;
	fbo_h = h;
	readback = malloc(w*h*4);
	printf("smoke: fbo %ix%i packed %i\n", w,h, packed);

	hw_cb.context_reset();
}

static void writeBMP(const char* path) {
	FILE* file = fopen(path, "wb");
	if (!file) FAIL("can't write %s", path);
	int row = (frame_w*3 + 3) & ~3;
	uint32_t size = 54 + row*frame_h;
	uint8_t header[54] = {'B','M'};
	*(uint32_t*)(header+2) = size;
	*(uint32_t*)(header+10) = 54;
	*(uint32_t*)(header+14) = 40;
	*(int32_t*)(header+18) = frame_w;
	*(int32_t*)(header+22) = frame_h;
	*(uint16_t*)(header+26) = 1;
	*(uint16_t*)(header+28) = 24;
	fwrite(header, 1, 54, file);
	uint8_t* line = calloc(1, row);
	for (int y=frame_h-1; y>=0; y--) {
		for (int x=0; x<frame_w; x++) {
			uint16_t p = frame[y*frame_w + x];
			line[x*3+0] = (p & 0x1F) << 3;
			line[x*3+1] = ((p >> 5) & 0x3F) << 2;
			line[x*3+2] = (p >> 11) << 3;
		}
		fwrite(line, 1, row, file);
	}
	free(line);
	fclose(file);
}

int main(int argc, char* argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0); // keep output if the core aborts
	if (argc<11) FAIL("usage: %s <core.so> <rom> <system_dir> <save_dir> <sw|gl> <frames> <default.cfg> <options.txt> <frame.bmp> <picture|toptext>", argv[0]);
	const char* core_path = argv[1];
	const char* rom_path = argv[2];
	system_dir = argv[3];
	save_dir = argv[4];
	use_gl = !strcmp(argv[5], "gl");
	int run_frames = atoi(argv[6]);
	loadConfig(argv[7]);
	options_file = fopen(argv[8], "w");
	const char* bmp_path = argv[9];
	int toptext = !strcmp(argv[10], "toptext");

	void* core = dlopen(core_path, RTLD_LAZY);
	if (!core) FAIL("dlopen: %s", dlerror());
	#define SYM(name) typeof(&name) p_##name = dlsym(core, #name); if (!p_##name) FAIL("missing %s", #name)
	SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
	SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init); SYM(retro_deinit); SYM(retro_get_system_info);
	SYM(retro_get_system_av_info); SYM(retro_load_game); SYM(retro_unload_game); SYM(retro_run);
	SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize);

	p_retro_set_environment(environment_callback);
	p_retro_set_video_refresh(video_callback);
	p_retro_set_audio_sample(audio_callback);
	p_retro_set_audio_sample_batch(audio_batch_callback);
	p_retro_set_input_poll(input_poll_callback);
	p_retro_set_input_state(input_state_callback);
	if (!option_count) FAIL("core reported no options");

	p_retro_init();
	struct retro_system_info info = {0};
	p_retro_get_system_info(&info);
	printf("smoke: %s %s (%s)\n", info.library_name, info.library_version, info.valid_extensions);

	struct retro_game_info game = { .path = rom_path };
	if (!p_retro_load_game(&game)) FAIL("retro_load_game");
	if (pixel_format!=RETRO_PIXEL_FORMAT_XRGB8888) FAIL("expected XRGB8888, got %i", pixel_format);

	struct retro_system_av_info av = {0};
	p_retro_get_system_av_info(&av);
	printf("smoke: geometry %ux%u (max %ux%u) fps %f\n", av.geometry.base_width, av.geometry.base_height, av.geometry.max_width, av.geometry.max_height, av.timing.fps);

	if (use_gl) {
		if (!hw_enabled) FAIL("core did not request a GLES context");
		glInit(av.geometry.max_width, av.geometry.max_height);
	}
	else if (hw_enabled) FAIL("hw render enabled in sw mode");

	for (int i=0; i<run_frames; i++) {
		p_retro_run();
		if (i==run_frames/2) {
			size_t size = p_retro_serialize_size();
			void* state = calloc(1, size);
			if (!size || !p_retro_serialize(state, size)) FAIL("retro_serialize (%zu)", size);
			if (!p_retro_unserialize(state, size)) FAIL("retro_unserialize");
			free(state);
			printf("smoke: save state %zu bytes ok\n", size);
		}
	}

	printf("smoke: frames %i (hw %i sw %i) dupes %i last %ux%u\n", frames, hw_frames, sw_frames, dupes, frame_w, frame_h);
	if (!frames) FAIL("no frames");
	if (use_gl && !hw_frames) FAIL("no hw frames");
	if (!use_gl && !sw_frames) FAIL("no sw frames");
	if (frame_w!=av.geometry.base_width || frame_h!=av.geometry.base_height) FAIL("frame %ux%u != geometry %ux%u", frame_w,frame_h, av.geometry.base_width,av.geometry.base_height);
	if (convert_mismatches) FAIL("8888 to 565 conversion mismatched reference on %li pixels", convert_mismatches);

	int lit = 0, colors = 0, max_x = 0, max_y = 0, min_x = frame_w, min_y = frame_h;
	static uint8_t seen[65536];
	for (int y=0; y<frame_h; y++) {
		for (int x=0; x<frame_w; x++) {
			uint16_t p = frame[y*frame_w + x];
			if (!seen[p]) { seen[p] = 1; colors++; }
			if (!p) continue;
			lit++;
			if (x<min_x) min_x = x;
			if (x>max_x) max_x = x;
			if (y<min_y) min_y = y;
			if (y>max_y) max_y = y;
		}
	}
	writeBMP(bmp_path);
	printf("smoke: lit pixels %i/%u colors %i bounds %i,%i-%i,%i -> %s\n", lit, frame_w*frame_h, colors, min_x,min_y, max_x,max_y, bmp_path);
	if (toptext) {
		// a line of 8px debug text near the top left (controller.pbp prints on its second line), a flipped frame puts it at the bottom
		if (lit<50) FAIL("no text");
		if (max_y>=frame_h/4 || min_x>2) FAIL("text not in the top left corner, frame flipped?");
	}
	else if (lit < frame_w*frame_h/100 || colors<4) FAIL("frame looks blank");

	if (use_gl) hw_cb.context_destroy();
	p_retro_unload_game();
	p_retro_deinit();
	printf("PASS %s\n", use_gl ? "gl" : "sw");
	return 0;
}
