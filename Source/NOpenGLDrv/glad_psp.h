#ifndef GLAD_PSP_H
#define GLAD_PSP_H

//
// PSP stand-in for glad.
//
// glad resolves every GL entry point at runtime via SDL_GL_GetProcAddress.
// The PSP's libGL (pspgl) is a *static* library with no runtime symbol table,
// so gladLoadGLLoader() resolves nothing and returns 0 -- which is what made
// UNOpenGLRenderDevice::Init() bail out with "Could not load GL" and trip the
// `check(RenDev)` assertion in NSDLViewport.
//
// The GL functions are linked in directly, so there is nothing to load. Pull in
// the real pspgl headers and supply only the handful of glad-specific symbols
// NOpenGLDrv actually references.
//

#include <GL/gl.h>
#include <GL/glext.h>

// pspgl's GL_EXTENSIONS string advertises GL_EXT_paletted_texture -- the PSP
// GPU has a hardware CLUT, so 8-bit indexed textures are a real win here -- but
// it does NOT advertise multitexture, texture_env_combine or BGRA.
// NOpenGLDrv checks each of these and disables the corresponding path when
// absent, so report them honestly rather than optimistically.
// pspgl advertises GL_EXT_paletted_texture and the PSP GPU has a hardware CLUT.
// This was briefly disabled while chasing the black screen, but that turned out
// to be a sized-internalformat problem, not a palette one. It is back on, and
// now it matters: with 32-bit RGBA uploads pspgl runs out of texture memory
// (GL_OUT_OF_MEMORY) after a few thousand mips. 8-bit indexed is a 4x saving.
#define GLAD_GL_EXT_paletted_texture    1
#define GLAD_GL_EXT_bgra                0
#define GLAD_GL_ARB_multitexture        0
#define GLAD_GL_EXT_texture_env_combine 0

// Claim 1.1. pspgl's headers define GL_VERSION_1_2 and _1_3, but the features
// NOpenGLDrv gates on GL_CHECK_VER(1,2) (BGRA texture upload) are not actually
// implemented, so claiming 1.2 would enable a path that then fails at runtime.
struct gladGLversionStruct { int major, minor; };
static struct gladGLversionStruct GLVersion = { 1, 1 };

// Templated so it accepts &SDL_GL_GetProcAddress without a cast; C++ will not
// implicitly convert a function pointer to void*.
template<typename LoaderT>
static inline int gladLoadGLLoader( LoaderT ) { return 1; }

// pspgl *implements* glColorTableEXT (verified in libGL.a's symbol table) but
// only declares it behind header guards that are not active here, so declare it
// ourselves. This is the paletted-texture upload path, which is worth having:
// the PSP GPU does 8-bit CLUT textures in hardware.
extern "C" void glColorTableEXT( GLenum target, GLenum internalFormat,
                                 GLsizei width, GLenum format, GLenum type,
                                 const GLvoid* table );

// pspgl declares glActiveTexture/glMultiTexCoord2f in its headers but ships no
// implementation of either (confirmed against libGL.a). GLAD_GL_ARB_multitexture
// is reported as 0 above, so UNOpenGLRenderDevice::Init() clears UseMultiTexture
// and never calls them -- but they still have to resolve at link time, so
// glad_psp.cpp defines them as no-ops.

#ifndef GL_MAX_TEXTURE_UNITS_ARB
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#endif
#ifndef GL_COLOR_INDEX8_EXT
#define GL_COLOR_INDEX8_EXT 0x80E5
#endif

#endif // GLAD_PSP_H
