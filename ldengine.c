#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <SDL2/SDL.h>

// Define windows size
#define W  640 // width of "game" screen when mini map active
#define W2 640 // Width of the screen
#define H  480 // Height of the screen

// Define vision constants
#define EyeHeight   6           // Camera height from floor when standing
#define DuckHeight  2.5         // And when crouching
#define HeadMargin  1           // How much room there is above camera before the head hits the ceiling
#define KneeHeight  2           // How tall obstacles the player can simply walk over without jumping
// Never >= 180º
#define hfov        (1.0 * 0.73f * H/W)     // Affects the horizontal field of vision
#define vfov        (1.0 * 0.2f)            // Affects the vertical field of vision

/********************************************* CONFIGS *********************************************/

#define TextureMapping      1
#define DepthShading        0
#define LightMapping        1
#define VisibilityTracking  1
#define SplitScreen         0

/********************************************* UTILITY *********************************************/
/* Math functions get some min, max, vectors cross products, etc.                                  */ 
/***************************************************************************************************/

#define min(a, b) (((a) < (b)) ? (a) : (b))             // min: choose smaller of two scalars.
#define max(a, b) (((a) > (b)) ? (a) : (b))             // max: choose greater of two scalars.
#define abs(a) ((a) < 0 ? -(a) : (a))                   // abs: No sign value
#define clamp(a, mi, ma) min(max(a, mi), ma)            // clamp: Clamp a value into set range.
#define sign(v) (((v) > 0) - ((v) < 0))                 // sign: Return the sign of a value (-1, 0 or 1)
#define vxs(x0, y0, x1, y1) ((x0) * (y1) - (x1) * (y0)) // vxs: Vector cross product.

// Overlap: Determine whether the two number ranges overlap.
#define Overlap(a0, a1, b0, b1) (min(a0, a1) <= max(b0, b1) && min(b0, b1) <= max(a0, a1))

// IntersectBox: Determine whether tow 2D-boxes intersect.
#define IntersectBox(x0, y0, x1, y1, x2, y2, x3, y3) (Overlap(x0, x1, x2, x3) && Overlap(y0, y1, y2, y3))

// PointSide: Determine wich side of a line the point is on. Return value: <0, =0 or >0
#define PointSide(px, py, x0, y0, x1, y1) sign(vxs((x1) - (x0), (y1) - (y0), (px) - (x0), (py) - (y0)))

// Intersect: Calculate the point of intersection between two lines.
#define Intersect(x1, y1, x2, y2, x3, y3, x4, y4) ((struct vec2d){                                                                        \
    vxs(vxs(x1, y1, x2, y2), (x1) - (x2), vxs(x3, y3, x4, y4), (x3) - (x4)) / vxs((x1) - (x2), (y1) - (y2), (x3) - (x4), (y3) - (y4)), \
    vxs(vxs(x1, y1, x2, y2), (y1) - (y2), vxs(x3, y3, x4, y4), (y3) - (y4)) / vxs((x1) - (x2), (y1) - (y2), (x3) - (x4), (y3) - (y4))})

// Hard-coded limits
#define MaxVertices 100     // Maximum number of vertices in a map
#define MaxEdges    100     // Maximum number of edges in a sector
#define MaxQueue    32      // Maximum number of pending portal renders

static SDL_Surface *surface = NULL;

#if TextureMapping
typedef int Texture[1014][1024];

struct TextureSet
{
    Texture texture;
    Texture normalmap;
    Texture lightmap;
    Texture lightmap_diffuseonly;
};
#endif

struct vec2d
{
    float x;
    float y;
};

struct vec3d
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
    unsigned short nPoints;
#if VisibilityTracking
    int visible;
#endif    
#if TextureMapping
    struct TextureSet *floortexture;
    struct TextureSet *ceiltexture;
    struct TextureSet *uppertextures;
    struct TextureSet *lowertextures;
#endif
} *sectors = NULL;

static unsigned NumSectors = 0;

#if VisibilityTracking
#define MaxVisibleSectors   256

struct vec2d VisibleFloorsBegins[MaxVisibleSectors][W];
struct vec2d VisibleFloorEnds[MaxVisibleSectors][W];
char VisibleFloors[MaxVisibleSectors][W];

struct vec2d VisibleCeilBegins[MaxVisibleSectors][W];
struct vec2d VisibleCeilEnds[MaxVisibleSectors][W];
char VisibleCeils[MaxVisibleSectors][W];

unsigned NumVisibleSectors = 0;
#endif

// Player: location of the player
static struct player
{
    struct vec3d where; // Current position
    struct vec3d velocity;
    float angle;
    float angleSin;
    float angleCos;
    float yaw;
    unsigned char sector; // Current vector
} player;

#if LightMapping
static struct light
{
    struct vec3d where;
    struct vec3d light;
    unsigned char sector;
} * lights = NULL;

static unsigned NumLights = 0;
#endif

static void LoadData(void)
{
    FILE *fp = fopen("map.txt", "rt");

    if (!fp)
    {
        perror("map.txt");
        exit(1);
    }

    char buf[256];
    char word[256];
    char *ptr;

    struct vec2d vertex[MaxVertices];
    struct vec2d *vertexptr = vertex;

    float x, y, angle, number, numbers[MaxEdges];

    int n, m;

    while (fgets(buf, sizeof(buf), fp))
    {
        switch (sscanf(ptr = buf, "%32s%n", word, &n) == 1 ? word[0] : '\0')
        {
        case 'v':
            for (sscanf(ptr += n, "%f%n", &y, &n); sscanf(ptr += n, "%f%n", &x, &n) == 1;)
            {

                if(vertexptr >= vertex + MaxVertices)
                {
                    fprintf(stderr, "ERROR: Too many vertices, limit is %u\n", MaxVertices);
                    exit(2);
                }

                *vertexptr++ = (struct vec2d) {x, y};
            }
            break;
        case 's':
            sectors = realloc(sectors, ++NumSectors * sizeof(*sectors));
            struct sector *sect = &sectors[NumSectors - 1];
            sscanf(ptr += n, "%f%f%n", &sect->floor, &sect->ceil, &n);
            for (m = 0; sscanf(ptr += n, "%32s%n", word, &n) == 1 && word[0] != '#';)
            {
                if(m >= MaxEdges)
                {
                    fprintf(stderr, "ERROR: Too many edges in sector %u. Limit is %u\n", NumSectors-1, MaxEdges);
                    exit(2);
                }

                numbers[m++] = word[0] == 'x' ? -1 : strtof(word, 0);
            }

            sect->nPoints = m /= 2;
            sect->neighbors = malloc((m) * sizeof(*sect->neighbors));
            sect->vertex = malloc((m + 1) * sizeof(*sect->vertex));
#if VisibilityTracking
            sect->visible = 0;
#endif

            for (n = 0; n < m; ++n)
            {
                sect->neighbors[n] = numbers[m + n];
            }

            for (n = 0; n < m; ++n)
            {
                int v = numbers[n];
                if(v >= vertexptr - vertex)
                {
                    fprintf(stderr, "ERROR: Invalid vertex number %d in sector %u; only have %u\n", v, NumSectors-1, (int)(vertexptr - vertex));
                    exit(2);
                }
                sect->vertex[n + 1] = vertex[v]; // TODO: Range checking
            }

            sect->vertex[0] = sect->vertex[m];
            break;
#if LightMapping
        case 'l':
            lights = realloc(lights, ++NumLights * sizeof(*lights));
            struct light* light = &lights[NumLights-1];
            sscanf(ptr += n, "%f %f %f %f %f %f %f", &light->where.x, &light->where.z, &light->where.y, &number,
                                                     &light->light.x, &light->light.y, &light->light.z);
            light->sector = (int)number;
            break;
#endif
        case 'p':
            sscanf(ptr += n, "%f %f %f %f", &x, &y, &angle, &number);
            player = (struct player)
            {
                {x, y, 0}, {0, 0, 0}, angle, 0, 0, 0, number
            };

            player.where.z = sectors[player.sector].floor + EyeHeight;
            player.angleSin = sinf(player.angle);
            player.angleCos = cosf(player.angle);
        }
    }

    fclose(fp);
}

static void UnloadData(void)
{
    for(unsigned a = 0; a < NumSectors; ++a)
    {
        free(sectors[a].vertex);
        free(sectors[a].neighbors);
    }

    free(sectors);
    sectors = NULL;
    NumSectors = 0;
}

static int IntersetLineSegments(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    return IntersectBox(x0,y0,x1,y1,x2,y2,x3,y3)
        && abs(PointSide(x2,y2,x0,y0,x1,y1) + PointSide(x3,y3,x0,y0,x1,y1)) != 2
        && abs(PointSide(x0,y0,x2,y2,x3,y3) + PointSide(x1,y1,x2,y2,x3,y3)) != 2;
}

struct Scaler
{
    int result;
    int bop;
    int fd;
    int ca;
    int cache;
};

#define Scaler_Init(a,b,c,d,f) \
    { d + (b-1-a) * (f-d) / (c-a), ((f<d) ^ (c<a)) ? -1 : 1, \
      abs(f-d), abs(c-a), (int)((b-1-a)* abs(f-d)) % abs(c-a) }

static int Scaler_Next(struct Scaler* i)
{
    for(i->cache += i->fd; i->cache >= i->ca; i->cache -= i->ca)
    {
        i->result += i->bop;
    }

    return i->result;
}
#if TextureMapping
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int LoadTexture(void)
{
    int initialized = 0;
    int fd = open("ldengine_textures.bin", O_RDWR | O_CREAT, 0664);

    if(lseek(fd, 0, SEEK_END) == 0)
    {
InitializeTextures:;
        // Initialize by loading textures
        #define LoadTexture(filename, name) \
            Texture* name = NULL; \
                do { \
                    FILE* fp = fopen(filename, "rb"); \
                    if(!fp) perror(filename); else { \
                        name = malloc (sizeof(*name));\
                        fseek(fp, 0x11, SEEK_SET); \
                        for(unsigned y=0; y<1024; ++y)\
                            for(unsigned x=0; x<1024; ++x)\
                            {\
                                int r = fgetc(fp), g = fgetc(fp), b = fgetc(fp); \
                                (*name)[x][y] = r * 65536 + g*256 + b; \
                            }\
                            fclose(fp); } \
                    } while(0)    

        #define UnloadTexture(name) free (name)   

        Texture dummyLightmap;
        memset(&dummyLightmap, 0, sizeof(dummyLightmap));

        LoadTexture("wall2.ppm", WallTexture);
        LoadTexture("wall2_norm.ppm", WallNormal);
        LoadTexture("wall3.ppm", WallTexture2);
        LoadTexture("wall3_norm.ppm", WallNormal2);
        LoadTexture("floor2.ppm", FloorTexture);
        LoadTexture("floor2_norm.ppm", FloorNormal);
        LoadTexture("ceil2.ppm", CeilTexture);
        LoadTexture("ceil2_norm.ppm", CeiltNormal);

        #define SafeWrite(fd, buf, amount) do { \
            const char* source = (const char*)(buf); \
            long remain = (amount); \
            while(remain > 0) { \
                long result = write(fd, source, remain); \
                if(result >= 0) { remain -= result; source += result; } \
                else if(errno == EAGAIN || errno == EINTR) continue; \
                else break; \
            } \
            if(remain > 0) perror("write"); \
        } while(0)

        #define PutTextureSet(txtname, normname) do { \
            SafeWrite(fd, txtname, sizeof(Texture)); \
            SafeWrite(fd, normname, sizeof(Texture)); \
            SafeWrite(fd, &dummyLightmap, sizeof(Texture)); \
            SafeWrite(fd, &dummyLightmap, sizeof(Texture)); } while(0)

        printf("Initializing textures...");
        lseek(fd, 0, SEEK_SET);

        for(unsigned n = 0; n<NumSectors; ++n)
        {
            for(int s=printf("%d/%d", n+1, NumSectors); s--;)
            {
                putchar('\b');
            }

            fflush(stdout);

            PutTextureSet(FloorTexture, FloorNormal);
            PutTextureSet(CeilTexture, CeiltNormal);

            for(unsigned w=0; w<sectors[n].nPoints; ++w)
            {
                PutTextureSet(WallTexture, WallNormal);
            }

            for(unsigned w=0; w<sectors[n].nPoints; ++w)
            {
                PutTextureSet(WallNormal2, WallNormal2);
            }
        }
        
        ftruncate(fd, lseek(fd, 0, SEEK_CUR));
        printf("\n"); fflush(stdout);

        UnloadTexture(WallTexture);
        UnloadTexture(WallNormal);
        UnloadTexture(WallTexture2);
        UnloadTexture(WallNormal2);
        UnloadTexture(FloorTexture);
        UnloadTexture(FloorNormal);
        UnloadTexture(CeilTexture);
        UnloadTexture(CeiltNormal);

        #undef UnloadTexture
        #undef LoadTexture
        initialized = 1;
    }

    off_t filesize = lseek(fd, 0 ,SEEK_END);
    char* texturedata = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(!texturedata) perror("mmap");

    printf("Loading textures\n");
    off_t pos = 0;
    for(unsigned n = 0; n<NumSectors; ++n)
    {
        sectors[n].floortexture = (void*)(texturedata + pos); pos += sizeof(struct TextureSet);
        sectors[n].ceiltexture = (void*)(texturedata + pos); pos += sizeof(struct TextureSet);

        unsigned w = sectors[n].nPoints;
        sectors[n].uppertextures = (void*)(texturedata + pos); pos+=sizeof(struct TextureSet) * w;
        sectors[n].lowertextures = (void*)(texturedata + pos); pos+=sizeof(struct TextureSet) * w;
    }

    printf("done, %llu bytes mmapped out of %llu\n", (unsigned long long)pos, (unsigned long long) filesize);

    if(pos != filesize)
    {
        printf(" -- Wrong filesize! Let's try that again.\n");
        munmap(texturedata, filesize);
        goto InitializeTextures;
    }

    return initialized;
}

// HERE 
#endif

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
        pix[y1*W+x] = top;
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
            && IntersectBox(px, py, px+dx, py+dy, vert[s+0].x, vert[s+0].y, vert[s+1].x, vert[s+1].y)
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

        if(renderedSectors[now.sectorno] & 0x21) continue; // Odd = still rendering, 0x20 = give up
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
            if(tz1 <= 0 && tz2 <= 0) continue;

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
            } // for ends

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

static SDL_Window *window = NULL;

int main()
{
    LoadData();

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL failed to initialise: %s\n", SDL_GetError());
        return 1;
   }

    window = SDL_CreateWindow("SDL Doom", /* Title of the SDL window */
 			    SDL_WINDOWPOS_UNDEFINED, /* Position x of the window */
 			    SDL_WINDOWPOS_UNDEFINED, /* Position y of the window */
 			    W, /* Width of the window in pixels */
 			    H, /* Height of the window in pixels */
 			    SDL_WINDOW_OPENGL); /* Additional flag(s) */

    if (window == NULL) {
        fprintf(stderr, "SDL window failed to initialise: %s\n", SDL_GetError());
        return 1;
    }

    surface = SDL_GetWindowSurface(window);

    SDL_ShowCursor(SDL_DISABLE);

    int wasd[4] = { 0, 0, 0, 0 };
    int ground = 0;
    int falling = 1;
    int moving = 0;
    int ducking = 0;

    float yaw = 0;

    for (;;)
    {
        // Vertical collision detection
        float eyeheight = ducking ? DuckHeight : EyeHeight;

        ground = !falling;

        if(falling)
        {
            player.velocity.z -= 0.05f; // Gravity
            
            float nextz = player.where.z + player.velocity.z;
            
            if(player.velocity.z < 0 && nextz < sectors[player.sector].floor + eyeheight) // When going down
            {
                // Fix to ground
                player.where.z = sectors[player.sector].floor + eyeheight;
                player.velocity.z = 0;
                falling = 0;
                ground = 1;
            }
            else if(player.velocity.z > 0 && nextz > sectors[player.sector].ceil) // Go up!
            {
                // Prevent jumping
                player.velocity.z = 0;
                falling = 1;
            }

            if(falling)
            {
                player.where.z += player.velocity.z;
                moving = 1;
            }
        }

        // Horizontal collision detection
        if(moving)
        {
            float px = player.where.x;
            float py = player.where.y;
            float dx = player.velocity.x;
            float dy = player.velocity.y;

            const struct sector* const sect = &sectors[player.sector];
            const struct vec2d* const vert = sect->vertex;

            // Check if the player is about to cross one of the sector's edges
            for(unsigned s = 0; s < sect->nPoints; ++s)
            {
                if(IntersectBox(px, py, px+dx, py+dy, vert[s+0].x, vert[s+0].y, vert[s+1].x,vert[s+1].y)
                && PointSide(px+dx, py+dy, vert[s+0].x, vert[s+0].y, vert[s+1].x, vert[s+1].y) < 0)
                {
                    // Check where the hole is
                    float hole_low  = sect->neighbors[s] < 0 ? 9e9 : max(sect->floor, sectors[sect->neighbors[s]].floor);
                    float hole_high = sect->neighbors[s] < 0 ? -9e9 : min(sect->ceil, sectors[sect->neighbors[s]].ceil);

                    // Check whether we're bumping into a wall
                    if(hole_high < player.where.z + HeadMargin || hole_low > player.where.z - eyeheight +KneeHeight)
                    {
                        // Bumps into a wall! Slide along the wall
                        float xd = vert[s+1].x - vert[s+0].x;
                        float yd = vert[s+1].y - vert[s+0].y;

                        dx = xd * (dx*xd + yd*dy) / (xd*xd + yd*yd);
                        dy = yd * (dx*xd + yd*dy) / (xd*xd + yd*yd);
                        moving = 0;
                    }
                }
            }

            MovePlayer(dx, dy);
            falling = 1;
        }

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
                    case 'w': wasd[0] = ev.type == SDL_KEYDOWN; break;
                    case 's': wasd[1] = ev.type == SDL_KEYDOWN; break;
                    case 'a': wasd[2] = ev.type == SDL_KEYDOWN; break;
                    case 'd': wasd[3] = ev.type == SDL_KEYDOWN; break;
                    case 'q':
                        goto done;
                    case ' ': // Jump
                        if(ground)
                        {
                            player.velocity.z += 0.5;
                            falling = 1;
                        }
                    break;
                    case SDLK_LCTRL: // DUCK
                    case SDLK_RCTRL:
                        ducking = ev.type == SDL_KEYDOWN;
                        falling = 1;
                    break;
                    default: break;
                }
                break;
            case SDL_QUIT:
                goto done;
            }

            // Mouse aiming
            int x,y;
            SDL_GetRelativeMouseState(&x, &y);
            player.angle += x * 0.03f;
            yaw = clamp(yaw - y * 0.05f, -5, 5);
            player.yaw = yaw - player.velocity.z * 0.5f;
            MovePlayer(0,0);

            float move_vec[2] = { 0.f, 0.f };
            if(wasd[0])
            {
                move_vec[0] += player.angleCos * 0.2f;
                move_vec[1] += player.angleSin * 0.2f;
            }

            if(wasd[1])
            {
                move_vec[0] -= player.angleCos * 0.2f;
                move_vec[1] -= player.angleSin * 0.2f;
            }

            if(wasd[2])
            {
                move_vec[0] += player.angleSin * 0.2f;
                move_vec[1] -= player.angleCos * 0.2f;
            }

            if(wasd[3])
            {
                move_vec[0] -= player.angleSin * 0.2f;
                move_vec[1] += player.angleCos * 0.2f;
            }

            int pushing = wasd[0] || wasd[1] || wasd[2] || wasd[3];
            float acceleration = pushing ? 0.4 : 0.2;

            player.velocity.x = player.velocity.x * (1-acceleration) + move_vec[0] * acceleration;
            player.velocity.y = player.velocity.y * (1-acceleration) + move_vec[1] * acceleration;

            if(pushing)
                moving = 1;
				
        }

        SDL_LockSurface(surface);
        DrawScreen();
        SDL_UnlockSurface(surface);
        SDL_UpdateWindowSurface(window);
		SDL_Delay(10);
    }

done:
    UnloadData();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}