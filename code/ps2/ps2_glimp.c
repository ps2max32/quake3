/* ps2_glimp.c: the GLimp_* functions of renderergl1 on maxsdk_gl
   (libs/maxsdk_gl), and the qgl* function pointers: its gl* functions. */

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
int qglesMajorVersion, qglesMinorVersion;

/* the GL calls of maxsdk_gl */
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

/* every qgl function that maxsdk_gl does not have: says which, once */
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
	glConfig.vidWidth = MXGL_WIDTH;
	glConfig.vidHeight = MXGL_HEIGHT;
	glConfig.windowAspect = 4.0f / 3.0f;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 0;
	glConfig.isFullscreen = qtrue;
	glConfig.displayFrequency = 60;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	/* "gamma": the overbright range is the GS's modulate (mxgl_ColorShift) */
	glConfig.deviceSupportsGamma = qtrue;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;
	glConfig.numTextureUnits = 1;
	Q_strncpyz(glConfig.vendor_string, (const char *)qglGetString(GL_VENDOR), sizeof glConfig.vendor_string);
	Q_strncpyz(glConfig.renderer_string, (const char *)qglGetString(GL_RENDERER), sizeof glConfig.renderer_string);
	Q_strncpyz(glConfig.version_string, (const char *)qglGetString(GL_VERSION), sizeof glConfig.version_string);
	Q_strncpyz(glConfig.extensions_string, "GL_EXT_compiled_vertex_array", sizeof glConfig.extensions_string);
	ri.Cvar_Get("r_availableModes", "640x448", CVAR_ROM);
	ri.IN_Init(NULL);
}

void GLimp_Shutdown(void) {}

#include "runcfg.h"
void mx_exit(int status) __attribute__((noreturn));

extern int mxgl_show_stats;

void GLimp_EndFrame(void) {
	static unsigned frames;
	static cvar_t *stats;
	if (!stats)
		stats = ri.Cvar_Get("r_ps2stats", "1", CVAR_ARCHIVE);
	mxgl_show_stats = stats->integer;
	mxgl_end_frame();
	mxgl_frame_done();
	if (PS2_GLSHOT && ++frames == PS2_GLSHOT) {
		ri.Printf(PRINT_ALL, "ps2: rendered frame %u, picture: %d\n", frames,
		          mxgl_screenshot("host:quake3.tga"));
		ri.Printf(PRINT_ALL, "@@PS2TEST_END@@ status=0\n");
		mx_exit(0);
	}
}

void GLimp_LogComment(char *comment) {}
void GLimp_Minimize(void) {}
void GLimp_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256]) {}
