/* ps2_glimp.c: the GLimp_* functions of renderergl1 on libgl
   (libs/libgl), and the qgl* function pointers: its gl* functions.
   The screen is the TV's (NTSC 640 x 448, PAL 640 x 512), or 1080i
   (1920 x 1080) when the mode is 1920x1080 (r_mode -1, r_customwidth,
   r_customheight: the Graphics menu's video mode, from r_availableModes);
   with beam racing when r_ps2beam is 1 (the menu's Beam Racing), in 24-bit
   colour when r_colorbits is 32 (Color Depth 32 bit: beam racing in the
   TV's mode only); set as the renderer starts (vid_restart). */

#include "../renderergl1/tr_local.h"
#include <mxgl.h>

void (APIENTRYP qglActiveTextureARB)(GLenum texture);
void (APIENTRYP qglClientActiveTextureARB)(GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB)(GLenum target, GLfloat s, GLfloat t);
void (APIENTRYP qglLockArraysEXT)(GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT)(void);

#define GLE(ret, name, ...) name##proc *qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

int qglMajorVersion = 1, qglMinorVersion = 1;
/* coloured lightmaps in effect (r_ps2colorlight 1, not with beam racing):
   tr_image.c keeps them in 8 bits a channel, tr_shade.c gives no stage the
   vertex light's hue (mono lightmaps' stand-in) */
int ps2_colorlight;

/* the stages' colours on VU1 (q3stage.hlsl; libgl's GL_PS2_vertex_program_binary):
   GL 2.0's calls, which the renderer's GL headers do not have */
GLuint glCreateShader(GLenum type);
void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum format, const void *binary,
                    GLsizei length);
GLuint glCreateProgram(void);
void glAttachShader(GLuint prog, GLuint sh);
void glLinkProgram(GLuint prog);
void glGetProgramiv(GLuint prog, GLenum pname, GLint *v);
void glUseProgram(GLuint prog);
GLint glGetUniformLocation(GLuint prog, const char *name);
void glUniform4fv(GLint loc, GLsizei count, const GLfloat *v);
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#define GL_LINK_STATUS 0x8B82
#endif
#ifdef MXGL_VU1CLIP
extern const struct mxgl_vu1_binary q3stagec_vu1;
#define q3stage_vu1 q3stagec_vu1
#else
extern const struct mxgl_vu1_binary q3stage_vu1;
#endif
static GLuint stage_prog;
static GLint stage_mul, stage_add;

static void stage_program_init(void) {
	if (stage_prog)
		return;
	GLuint sh = glCreateShader(GL_VERTEX_SHADER), prog = glCreateProgram();
	GLint ok = 0;
	glShaderBinary(1, &sh, GL_PS2_VU1_BINARY, &q3stage_vu1, sizeof q3stage_vu1);
	glAttachShader(prog, sh);
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		ri.Printf(PRINT_WARNING, "ps2: the stage program did not link\n");
		return;
	}
	stage_prog = prog;
	stage_mul = glGetUniformLocation(prog, "cmul");
	stage_add = glGetUniformLocation(prog, "cadd");
}

/* the draws after: colour = vertex colour * mul + add (q3stage.hlsl), or
   as they are (mul NULL); qfalse if there is no program */
qboolean GL_PS2_StageProgram(const float *mul, const float *add) {
	if (!stage_prog)
		return qfalse;
	if (!mul) {
		glUseProgram(0);
		return qtrue;
	}
	glUseProgram(stage_prog);
	glUniform4fv(stage_mul, 1, mul);
	glUniform4fv(stage_add, 1, add);
	return qtrue;
}
int qglesMajorVersion, qglesMinorVersion;

/* the GL calls of libgl */
void APIENTRY glActiveTextureARB(GLenum t);
void APIENTRY glClientActiveTextureARB(GLenum t);
void APIENTRY glMultiTexCoord2fARB(GLenum t, GLfloat s, GLfloat tt);
void APIENTRY glLockArraysEXT(GLint first, GLsizei count);
void APIENTRY glUnlockArraysEXT(void);

static void set_procs(void) {
#define S(name) qgl##name = gl##name;
	S(BindTexture) S(BlendFunc) S(ClearColor) S(Clear) S(ClearStencil) S(ColorMask)
	S(CullFace) S(DeleteTextures) S(DepthFunc) S(DepthMask) S(Disable) S(DrawArrays)
	S(DrawElements) S(Enable) S(Finish) S(Flush) S(GenTextures) S(GetBooleanv) S(GetError)
	S(GetIntegerv) S(GetString) S(LineWidth) S(PolygonOffset) S(ReadPixels) S(Scissor)
	S(StencilFunc) S(StencilMask) S(StencilOp) S(TexImage2D) S(TexParameterf)
	S(TexParameteri) S(TexSubImage2D) S(Viewport) S(AlphaFunc) S(Color4f) S(ColorPointer)
	S(DisableClientState) S(EnableClientState) S(LoadIdentity) S(LoadMatrixf) S(MatrixMode)
	S(PopMatrix) S(PushMatrix) S(ShadeModel) S(TexCoordPointer) S(TexEnvf) S(Translatef)
	S(VertexPointer) S(ClearDepth) S(DepthRange) S(DrawBuffer) S(PolygonMode)
	S(ArrayElement) S(Begin) S(ClipPlane) S(Color3f) S(Color4ubv) S(End) S(Ortho)
	S(TexCoord2f) S(TexCoord2fv) S(Vertex2f) S(Vertex3f) S(Vertex3fv)
	S(LockArraysEXT) S(UnlockArraysEXT)
#undef S
}

/* every qgl function that libgl does not have: says which, once */
#define GLE(ret, name, ...) \
	static ret APIENTRY missing_##name(__VA_ARGS__) { \
		static int said; \
		if (!said++) ri.Printf(PRINT_ALL, "mxgl: qgl" #name " is not there\n"); \
		return (ret)0; \
	}
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
#undef GLE

void GLimp_Init(qboolean fixedFunction) {
	mxgl_init();
	set_procs();
#define GLE(ret, name, ...) if (!qgl##name) qgl##name = missing_##name;
	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
#undef GLE
	int hires = ri.Cvar_VariableIntegerValue("r_mode") == -1 &&
	            ri.Cvar_VariableIntegerValue("r_customheight") >= 1080;
	/* beam racing (libgl's MXGL_BEAM): twice the room for textures in the
	   TV's mode, at most a frame a field */
	cvar_t *beam = ri.Cvar_Get("r_ps2beam", "0", CVAR_ARCHIVE);
	int flags = (beam->integer ? MXGL_BEAM : 0) |
	            (ri.Cvar_VariableIntegerValue("r_colorbits") == 32 ? MXGL_RGB24 : 0);
	if (mxgl_set_mode((hires ? MXGL_1080I : MXGL_SD) | flags) < 0) {
		ri.Printf(PRINT_ALL, "beam racing: not enough memory\n");
		ri.Cvar_Set("r_ps2beam", "0");
	}
	flags = mxgl_mode_flags();
	ri.Printf(PRINT_ALL, "ps2: %s%s%s\n", hires ? "1080i" : "TV mode", flags & MXGL_BEAM ? ", beam racing" : "",
	          flags & MXGL_RGB24 ? ", 24-bit colour" : "");
	/* coloured light: the lightmaps' products a channel at a time (three
	   passes); with beam racing their brightness */
	ps2_colorlight = ri.Cvar_Get("r_ps2colorlight", "1", CVAR_ARCHIVE | CVAR_LATCH)->integer &&
	                 !(flags & MXGL_BEAM);
	mxgl_color_light(ps2_colorlight);
	stage_program_init();
	/* the textures' top levels left out only while GS memory overflows or
	   the swap file is read (-1: MXGL_TEX_AUTO), or that many always */
	mxgl_texture_quality(ri.Cvar_Get("r_ps2texquality", "-1", CVAR_ARCHIVE)->integer);
	int hi = 120, lo = 80, sw = 30;
	sscanf(ri.Cvar_Get("r_ps2texlimits", "120 80 30", CVAR_ARCHIVE)->string, "%d%*c%d%*c%d", &hi, &lo, &sw);
	mxgl_texture_limits(hi, lo, sw);
	int w, h;
	mxgl_screen(&w, &h);
	glConfig.vidWidth = w;
	glConfig.vidHeight = h;
	glConfig.windowAspect = hires ? 16.0f / 9.0f : 4.0f / 3.0f;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 0;
	glConfig.isFullscreen = qtrue;
	glConfig.displayFrequency = mxgl_mode() == MXGL_PAL ? 50 : 60;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	/* "gamma": the overbright range is GL_COMBINE's RGB_SCALE (GL_PS2_Shift),
	   the GS's modulate up to 2x */
	glConfig.deviceSupportsGamma = qtrue;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;
	/* one unit, though libgl has two (GL_ARB_multitexture, a second pass):
	   tr_shade.c's stages for the PS2 give the lightmap's pass the hue of
	   the vertex light, which a collapsed stage would lose */
	glConfig.numTextureUnits = 1;
	Q_strncpyz(glConfig.vendor_string, (const char *)qglGetString(GL_VENDOR), sizeof glConfig.vendor_string);
	Q_strncpyz(glConfig.renderer_string, (const char *)qglGetString(GL_RENDERER), sizeof glConfig.renderer_string);
	Q_strncpyz(glConfig.version_string, (const char *)qglGetString(GL_VERSION), sizeof glConfig.version_string);
	Q_strncpyz(glConfig.extensions_string, (const char *)qglGetString(GL_EXTENSIONS), sizeof glConfig.extensions_string);
	/* the paletted textures of the PS2's pak (tr_image.c, tools/ps2pak.py) */
	if (strstr(glConfig.extensions_string, "GL_OES_compressed_paletted_texture"))
		qglCompressedTexImage2D = (CompressedTexImage2Dproc *)mxglGetProcAddress("glCompressedTexImage2D");
	/* the Graphics menu's modes: the TV's and 1080i */
	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);
	ri.Cvar_Set("r_availableModes", mxgl_mode() == MXGL_PAL ? "640x512 1920x1080" : "640x448 1920x1080");
	ri.IN_Init(NULL);
}

void GLimp_Shutdown(void) {}

#include "runcfg.h"
void mx_exit(int status) __attribute__((noreturn));


void GLimp_EndFrame(void) {
	static unsigned frames;
	mxgl_end_frame();
	mxgl_frame_done();
	if (PS2_GLSHOT && ++frames == PS2_GLSHOT) {
		ri.Printf(PRINT_ALL, "ps2: rendered frame %u, picture: %d\n", frames,
		          mxgl_screenshot("quake3.tga"));
		ri.Printf(PRINT_ALL, "@@PS2TEST_END@@ status=0\n");
		mx_exit(0);
	}
}

void GLimp_LogComment(char *comment) {}
void GLimp_Minimize(void) {}
void GLimp_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256]) {}
