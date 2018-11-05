#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL/SDL.h>

#undef main

// Define windows size
#define W 640
#define H 480

static SDL_Surface* surface = NULL;


int main()
{
    surface = SDL_SetVideoMode(W, H, 32, 0);

    SDL_EnableKeyRepeat(150, 30);
    SDL_ShowCursor(SDL_DISABLE);

    for(;;)
    {
        SDL_LockSurface(surface);
        SDL_UnlockSurface(surface);
        SDL_Flip(surface);

        // Keyboard events
        SDL_Event ev;
        while(SDL_PollEvent(&ev))
        {
            switch(ev.type)
            {
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    switch(ev.key.keysym.sym)
                    {
                        case 'q': goto done;
                    }
                    break;
                case SDL_QUIT:
                    goto done;
            }
        }
    }

done:
    SDL_Quit();
    return 0;
}