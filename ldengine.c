#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL/SDL.h>

#undef main

// Define windows size
#define W 640
#define H 480

// Define vision constants
#define EyeHeight   6           // Camera height from floor when standing
#define DuckHeight  2.5         // And when crouching
#define HeadMargin  1           // How much room there is above camera before the head hits the ceiling
#define KneeHeight  2           // How tall obstacles the player can simply walk over without jumping
#define hfov        (0.73f * H) // Affects the horizontal field of vision
#define vfov        (0.2f * H)  // Affects the vertical field of vision

static SDL_Surface *surface = NULL;

static struct vec2d
{
    float x;
    float y;
};

static struct vec3d
{
    float x;
    float y;
    float z;
};

// Sector: Floor and ceiling height; list of edge vertices and neighbors
static struct sector
{
    float floor;
    float ceil;
    struct vec2d *vertex;
    signed char *neighbors;
    unsigned int nPoints;
} *sectors = NULL;

static unsigned NumSectors = 0;

// Player: location of the player
static struct player
{
    struct vec3d where; // Current position
    struct vec3d velocity;
    float angle;
    float angleSin;
    float angleCos;
    float yaw;
    unsigned sector; // Current vector
} player;

/********************************************* UTILITY *********************************************
 * Math functions get some min, max, vectors cross products, etc.
 ***************************************************************************************************/

#define min(a, b) (((a) < (b)) ? (a) : (b));            // min: choose smaller of two scalars.
#define max(a, b) (((a) > (b)) ? (a) : (b));            // max: choose greater of two scalars.
#define clamp(a, mi, ma) min(max(a, mi), ma)            // clamp: Clamp a value into set range.
#define vxs(x0, y0, x1, y1) ((x0) * (y1) - (x1) * (y0)) // vxs: Vector cross product.

// Overlap: Determine whether the two number ranges overlap.
#define Overlap(a0, a1, b0, b1) (min(a0, a1) <= max(b0, b1) && min(b0, b1) <= max(a0, a1))

// IntersectBox: Determine whether tow 2D-boxes intersect.
#define InsersectBox(x0, y0, x1, y1, x2, y2, x3, y3) (Overlap(x0, x1, x2, x3) && Overlap(y0, y1, y2, y3))

// PointSide: Determine wich side of a line the point is on. Return value: <0, =0 or >0
#define PointSide(px, py, x0, y0, x1, y1) vxs((x1) - (x0), (y1) - (y0), (px) - (x0), (py) - (y0))

// Intersect: Calculate the point of intersection between two lines.
#define Intersect(x1, y1, x2, y2, x3, y3, x4, y4) ((struct xy){                                                                        \
    vxs(vxs(x1, y1, x2, y2), (x1) - (x2), vxs(x3, y3, x4, y4), (x3) - (x4)) / vxs((x1) - (x2), (y1) - (y2), (x3) - (x4), (y3) - (y4)), \
    vxs(vxs(x1, y1, x2, y2), (y1) - (y2), vxs(x3, y3, x4, y4), (y3) - (y4)) / vxs((x1) - (x2), (y1) - (y2), (x3) - (x4), (y3) - (y4))})

static void LoadData()
{
    FILE *fp = fopen("map-clear.txt", "rt");

    if (!fp)
    {
        perror("map-clear.txt");
        exit(1);
    }

    char buf[256];
    char word[256];
    char *ptr;

    struct vec2d *vert = NULL, v;

    int n, m, NumVertices = 0;

    while (fgets(buf, sizeof(buf), fp))
    {
        switch (sscanf(ptr = buf, "%32s%n", word, &n) == 1 ? word[0] : '\0')
        {
        case 'v':
            for (sscanf(ptr += n, "%f%n", &v.y, &n); sscanf(ptr += n, "%f%n", &v.x, &n) == 1;)
            {
                vert = realloc(vert, ++NumVertices * sizeof(*vert));
                vert[NumVertices - 1] = v;
            }
            break;
        case 's':
            sectors = realloc(sectors, ++NumSectors * sizeof(*sectors));
            struct sector *sect = &sectors[NumSectors - 1];
            int *num = NULL;
            sscanf(ptr += n, "%f%f%n", &sect->floor, &sect->ceil, &n);
            for (m = 0; sscanf(ptr += n, "%32s%n", word, &n) == 1 && word[0] != '#';)
            {
                num = realloc(num, ++m * sizeof(*num));
                num[m - 1] = word[0] == 'x' ? -1 : atoi(word);
            }

            sect->nPoints = m /= 2;
            sect->neighbors = malloc((m) * sizeof(*sect->neighbors));
            sect->vertex = malloc((m + 1) * sizeof(*sect->vertex));

            for (n = 0; n < m; ++n)
            {
                sect->neighbors[n] = num[m + n];
            }

            for (n = 0; n < m; ++n)
            {
                sect->vertex[n + 1] = vert[num[n]]; // TODO: Range checking
            }

            sect->vertex[0] = sect->vertex[m];
            free(num);
            break;
        case 'p':;
            float angle;
            sscanf(ptr += n, "%f %f %f %d", &v.x, &v.y, &angle, &n);
            player = (struct player)
            {
                {v.x, v.y, 0}, {0, 0, 0}, angle, 0, 0, 0, n
            };

            player.where.z = sectors[player.sector].floor + EyeHeight;
        }
    }

    fclose(fp);
    free(vert);
}

int main()
{
    surface = SDL_SetVideoMode(W, H, 32, 0);

    SDL_EnableKeyRepeat(150, 30);
    SDL_ShowCursor(SDL_DISABLE);

    for (;;)
    {
        SDL_LockSurface(surface);
        SDL_UnlockSurface(surface);
        SDL_Flip(surface);

        // Keyboard events
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                switch (ev.key.keysym.sym)
                {
                case 'q':
                    goto done;
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