//
// Stubs for GL entry points that pspgl declares but does not implement.
//
// pspgl has no multitexture support at all -- its GL_EXTENSIONS string omits
// GL_ARB_multitexture, and libGL.a contains no glActiveTexture or
// glMultiTexCoord2f. glad_psp.h reports the extension as absent, so
// UNOpenGLRenderDevice::Init() clears UseMultiTexture and these are never
// called at runtime; they exist only so the link resolves.
//
#include <GL/gl.h>

extern "C" void glActiveTexture( GLenum )                    {}
extern "C" void glMultiTexCoord2f( GLenum, GLfloat, GLfloat ) {}
