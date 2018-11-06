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

#define min(a, b) (((a) < (b)) ? (a) : (b))            // min: choose smaller of two scalars.
#define max(a, b) (((a) > (b)) ? (a) : (b))            // max: choose greater of two scalars.
#define clamp(a, mi, ma) min(max(a, mi), ma)            // clamp: Clamp a value into set range.
#define vxs(x0, y0, x1, y1) ((x0) * (y1) - (x1) * (y0)) // vxs: Vector cross product.

// Overlap: Determine whether the two number ranges overlap.
#define Overlap(a0, a1, b0, b1) (min(a0, a1) <= max(b0, b1) && min(b0, b1) <= max(a0, a1))

// IntersectBox: Determine whether tow 2D-boxes intersect.
#define InsersectBox(x0, y0, x1, y1, x2, y2, x3, y3) (Overlap(x0, x1, x2, x3) && Overlap(y0, y1, y2, y3))

// PointSide: Determine wich side of a line the point is on. Return value: <0, =0 or >0
#define PointSide(px, py, x0, y0, x1, y1) vxs((x1) - (x0), (y1) - (y0), (px) - (x0), (py) - (y0))

// Intersect: Calculate the point of intersection between two lines.
#define Intersect(x1, y1, x2, y2, x3, y3, x4, y4) ((struct vec2d){                                                                        \
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

static void UnloadData()
{
    for(unsigned a = 0; a < NumSectors; ++a)
    {
        free(sectors[a].vertex);
    }

    for(unsigned a = 0; a < NumSectors; ++a)
    {
        free(sectors[a].neighbors);
    }

    sectors = NULL;

    NumSectors = 0;
}

// viline: Draw a vertical line on screen, with a different color pixel in top and bottom
static void vline(int x, int y1, int y2, int top, int middle, int bottom)
{
    int *pix = (int *) surface->pixels;
    y1 = clamp(y1, 0, (H-1));
    y2 = clamp(y2, 0, (H-1));

    if(y2 == y1)
    {
        pix[y1*W+x] = middle;
    }
    else if(y2 > y1)
    {
        pix[y1*W+x] = middle;
        for(int y = y1+1; y < y2; ++y)
        {
            pix[y*W+x] = middle;
        }

        pix[y2*W+x] = bottom;
    }
}

// MovePlayer: Moves the player by (dx,dy) in the map, and also updates ther anglesin, anglecos and sector
// properties.
static void MovePlayer(float dx, float dy)
{
    float px = player.where.x, py = player.where.y;

    const struct sector* const sect = &sectors[player.sector];
    const struct vec2d* const vert = sect->vertex;

    for(unsigned s = 0; s < sect->nPoints; ++s)
    {
        if(sect->neighbors[s] >= 0
            && InsersectBox(px, py, px+dx, py+dy, vert[s+0].x, vert[s+0].y, vert[s+1].x, vert[s+1].y)
            && PointSide(px+dx, py+dy, vert[s+0].x, vert[s+0].y, vert[s+1].x, vert[s+1].y) < 0)
        {
            player.sector = sect->neighbors[s];
            break;
        }
    }

    player.where.x += dx;
    player.where.y += dy;
    player.angleSin = sinf(player.angle);
    player.angleCos = cosf(player.angle);
}

static void DrawScreen()
{
    enum
    {
        MaxQueue = 32 // Maximum number of pending portal renders
    };

    struct item
    {
        int sectorno;
        int sx1;
        int sx2;
    };

    struct item queue[MaxQueue];
    struct item *head = queue;
    struct item *tail = queue;

    int ytop[W] = {0};
    int ybottom[W];
    int renderedSectors[NumSectors];

    for(unsigned x=0; x<W; ++x)
    {
        ybottom[x] = H-1;
    }

    for(unsigned n=0; n<NumSectors; ++n)
    {
        renderedSectors[n] = 0;
    }

    *head = (struct item) { player.sector, 0, W-1 };

    if(++head == queue+MaxQueue)
    {
        head = queue;
    }

    do{
        // pick a sector and slice from queue to draw
        const struct item now = *tail;

        if(++tail == queue+MaxQueue)
        {
            tail = queue;
        }

        if(renderedSectors[now.sectorno] & 0x21) continue; // 0dd = still rendering, 0x20 = give up
        ++renderedSectors[now.sectorno];

        const struct sector* const sect = &sectors[now.sectorno];

        // Render each wall of this sector that is facing towards player.
        for(unsigned s = 0; s < sect->nPoints; ++s)
        {
            // Acquire the x,y coordinates of the two endpoints(vertices) ot this edge of the sector.
            float vx1 = sect->vertex[s+0].x - player.where.x;
            float vy1 = sect->vertex[s+0].y - player.where.y;
            float vx2 = sect->vertex[s+1].x - player.where.x;
            float vy2 = sect->vertex[s+1].y - player.where.y;

            // Rotate them around the player's view
            float pcos = player.angleCos;
            float psin = player.angleSin;

            float tx1 = vx1 * psin - vy1 * pcos;
            float tz1 = vx1 * pcos + vy1 * psin;
            float tx2 = vx2 * psin - vy2 * pcos;
            float tz2 = vx2 * pcos + vy2 * psin;

            // Is the wall at least partially in fron of the player?
            if(tz1 <= 0 || tz2 <= 0) continue;

            // If it's partially behaind the player, clip it against player's view frustrum
            if(tz1 <= 0 || tz2 <= 0)
            {
                float nearz = 1e-4f;
                float farz = 5;
                float nearside = 1e-5f;
                float farside = 20.f;

                // Find an intersection between the wall and the approximate edges of player's view
                struct vec2d i1 = Intersect(tx1, tz1, tx2, tz2, -nearside, nearz, -farside, farz);
                struct vec2d i2 = Intersect(tx1, tz1, tx2, tz2, nearside, nearz, farside, farz);

                if(tz1 < nearz)
                {
                    if(i1.y > 0)
                    {
                        tx1 = i1.x;
                        tz1 = i1.y;
                    }
                    else
                    {
                        tx1 = i2.x;
                        tz1 = i2.y;
                    }
                }

                if(tz2 < nearz)
                {
                    if(i1.y > 0)
                    {
                        tx2 = i1.x;
                        tz2 = i1.y;
                    }
                    else
                    {
                        tx2 = i2.x;
                        tz2 = i2.y;
                    }
                }
            }

            // Perspective transformation
            float xscale1 = hfov / tz1;
            float yscale1 = vfov / tz1;
            float xscale2 = hfov / tz2;
            float yscale2 = vfov / tz2;

            int x1 = W / 2 - (int)(tx1 * xscale1);
            int x2 = W / 2 - (int)(tx2 * xscale2);

            if(x1 >= x2 || x2 < now.sx1 || x1 > now.sx2) continue; // only render if it's visible

            // Acquire the floor and ceiling heights, relative to where the player's view is
            float yceil = sect->ceil - player.where.z;
            float yfloor = sect->floor - player.where.z;

            // Check the edge type: neighbor = -1 means wall, other = boundary between two sectors
            int neighbor = sect->neighbors[s];
            float nyceil = 0;
            float nyfloor = 0;

            if(neighbor >= 0) // Is another sector showing through this portal?
            {
                nyceil = sectors[neighbor].ceil - player.where.z;
                nyfloor = sectors[neighbor].floor - player.where.z;
            }

            // Project our ceiling and floor heights nito screen coordinates (Y)
            #define Yaw(y,z) (y + z * player.yaw)

            int y1a = H / 2 - (int)(Yaw(yceil, tz1) * yscale1);
            int y1b = H / 2 - (int)(Yaw(yfloor, tz1) * yscale1);
            int y2a = H / 2 - (int)(Yaw(yceil, tz2) * yscale2);
            int y2b = H / 2 - (int)(Yaw(yfloor, tz2) * yscale2);

            // The same for the neighboring sector
            int ny1a = H / 2 - (int)(Yaw(nyceil,tz1) * yscale1);
            int ny1b = H / 2 - (int)(Yaw(nyfloor,tz1) * yscale1);
            int ny2a = H / 2 - (int)(Yaw(nyceil,tz2) * yscale2);
            int ny2b = H / 2 - (int)(Yaw(nyfloor,tz2) * yscale2);

            // Render the wall
            int beginx = max(x1, now.sx1);
            int endx = min(x2, now.sx2);

            for(int x = beginx; x <= endx; ++x)
            {
                // Calculate the Z coordinate for this point (Only used for lighting)
                int z = ((x - x1) * (tz2-tz1) / (x2-x1) + tz1) * 8;

                // Acquire the Y coordinates for our ceiling and floor for this X coordinate. Clamp them.
                int ya = (x-x1) * (y2a - y1a) / (x2-x1) + y1a;
                int yb = (x-x1) * (y2b - y1b) / (x2-x1) + y1b;
                int cya = clamp(ya, ytop[x], ybottom[x]); // top
                int cyb = clamp(yb, ytop[x], ybottom[x]); // bottom

                // Render ceiling: everything above this sector's ceiling height
                vline(x, ytop[x], cya-1, 0x111111, 0x222222, 0x111111);

                // Render floor: everything below this sector's floor height
                vline(x, cyb+1, ybottom[x], 0x0000FF, 0x0000AA, 0x0000FF);

                // Is there another sector behind this edge?
                if(neighbor >= 0)
                {
                    int nya = (x-x1) * (ny2a - ny1a) / (x2-x1) + ny1a;
                    int nyb = (x-x1) * (ny2b - ny1b) / (x2-x1) + ny1b;
                    int cnya = clamp(nya, ytop[x], ybottom[x]); // top
                    int cnyb = clamp(nyb, ytop[x], ybottom[x]); // bottom

                    // If our ceiling is higher than ther ceiling, render upper wall
                    unsigned r1 = 0x010101 * (255 - z);
                    unsigned r2 = 0x040007 * (31 - z/8);
                    vline(x, cya, cnya-1, 0, x==x1 || x == x2 ? 0 : r1, 0); //Between our and their ceiling
                    ytop[x] = clamp(max(cya, cnya), ytop[x], H-1); // Shrink the remaining window below these ceiling;

                    // If our floor is lower than ther floor, render bottom wall
                    vline(x, cnyb+1, cyb, 0, x == x1 || x == x2 ? 0 : r2, 0); // Between their and our floor
                    ybottom[x] = clamp(min(cyb, cnyb), 0, ybottom[x]); // Shrink the remaining window above these floor
                }
                else
                {
                    // NO NEIGHBOR!!!! Render wall from top to bottom
                    unsigned r = 0x010101 * (255-z);
                    vline(x, cya, cyb, 0, x==x1 || x == x2 ? 0 : r, 0);
                }
            }

            // Shedule the neighboring sector for rendering within the window formed by this wall
            if(neighbor >= 0 && endx >= beginx && (head+MaxQueue+1-tail)%MaxQueue)
            {
                *head = (struct item) { neighbor, beginx, endx };
                if(++head == queue+MaxQueue)
                {
                    head = queue;
                }
            }
        }  // for ends

        ++renderedSectors[now.sectorno];
    } while(head != tail); // render any other queued sectors
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