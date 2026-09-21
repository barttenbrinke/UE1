// Minimal SDL2 + pspgl test: clear the screen to magenta and swap, forever.
// If this shows a full magenta screen, pspgl/SDL presentation is fine and the
// stripe is something Unreal does. If it also shows a stripe, the problem is
// below Unreal entirely (pspgl, SDL's PSP video driver, or PPSSPP).
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* w = SDL_CreateWindow("t", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                     480, 272, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!w) { printf("CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext c = SDL_GL_CreateContext(w);
    if (!c) { printf("CreateContext: %s\n", SDL_GetError()); return 1; }

    printf("vendor=%s\n",   (const char*)glGetString(GL_VENDOR));
    printf("renderer=%s\n", (const char*)glGetString(GL_RENDERER));
    printf("version=%s\n",  (const char*)glGetString(GL_VERSION));

    int f = 0;
    for (;;) {
        // alternate magenta / green so a frozen frame is obvious
        if ((f / 60) & 1) glClearColor(1.f, 0.f, 1.f, 1.f);
        else              glClearColor(0.f, 1.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        SDL_GL_SwapWindow(w);
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        ++f;
    }
    return 0;
}
