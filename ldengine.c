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
typedef int Texture[1024][1024];

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
    unsigned short nPoints;
    signed char *neighbors;
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

struct vec2d VisibleFloorBegins[MaxVisibleSectors][W];
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

static int IntersectLineSegments(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
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

/*static void LT(char *filename, Texture* name)
{
    //Texture* name = NULL; 
                do { 
                    FILE* fp = fopen(filename, "rb"); 
                    if(!fp) perror(filename); else { 
                        name = malloc (sizeof(Texture));
                        fseek(fp, 0x11, SEEK_SET); 
                        for(unsigned y=0; y<1024; ++y)
                            for(unsigned x=0; x<1024; ++x)
                            {
                                int r = fgetc(fp), g = fgetc(fp), b = fgetc(fp); 
                                (*name)[x][y] = r * 65536 + g*256 + b; 
                            }
                            fclose(fp); } 
                    } while(0);    
}*/

static int LoadTexture(void)
{
    int initialized = 0;
    int fd = open("ldengine_textures.bin", O_RDWR | O_CREAT, 0644);

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

        //#define LoadTexture(filename, name) Texture* name = NULL; LT(filename, name)

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
                PutTextureSet(WallTexture2, WallNormal2);
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

#if LightMapping
#define vlen(x,y,z) sqrtf((x)*(x) + (y) * (y) + (z) * (z))
#define vlen2(x0,y0,z0,x1,y1,z1) vlen((x1)-(x0), (y1)-(y0), (z1)-(z0))
#define vdot3(x0,y0,z0,x1,y1,z1) ((x0)*(x1) + (y0)*(y1) + (z0)*(z1))
#define vxs3(x0,y0,z0,x1,y1,z1) (struct vec3d){ vxs(y0,z0,y1,z1), vxs(z0,x0,z1,x1), vxs(x0,y0,x1,y1) }

struct Intersection
{
    struct vec3d where;                 // Map coordinates where the hit happened, x,z = map, y = height
    struct TextureSet* surface;         // Information about the surface that was hit
    struct vec3d normal;                // Perturbet surface normal
    int sample;                         // RGB sample from surface texture and lightmap
    int sectorno;                       
};

static int ClampWithDesaturation(int r, int g, int b)
{
    int luma = r * 299 + g * 587 + b * 114;
    if(luma > 255000)
    {
        r = g = b = 255;
    }
    else if(luma <= 0)
    {
        r=g=b=0;
    }
    else
    {
        double sat = 1000;
        if(r > 255)
        {
            sat = min(sat,(luma-255e3) / (luma-r));
        }
        else if(r < 0)
        {
            sat = min(sat, luma/(double)(luma-r));
        }

        if(g > 255)
        {
            sat = min(sat,(luma-255e3) / (luma-g));
        }
        else if(g < 0)
        {
            sat = min(sat, luma/(double)(luma-g));
        }

        if(b > 255)
        {
            sat = min(sat,(luma-255e3) / (luma-b));
        }
        else if(b < 0)
        {
            sat = min(sat, luma/(double)(luma-b));
        }

        if(sat != 1.0)
        {
            r = (r - luma) * sat/1e3 + luma; 
            r = clamp(r, 0, 255);
           
            g = (g - luma) * sat/1e3 + luma; 
            g = clamp(g, 0, 255);
           
            b = (b - luma) * sat/1e3 + luma; 
            b = clamp(b, 0, 255);
        }
    }

    return r * 65536 + g * 256 + b;
}

static int ApplyLight(int texture, int light)
{
    int tr = (texture >> 16) & 0xFF;
    int tg = (texture >>  8) & 0xFF;
    int tb = (texture >>  0) & 0xFF;
    int lr = ((light >> 16)  & 0xFF);
    int lg = ((light >>  8)  & 0xFF);
    int lb = ((light >>  0)  & 0xFF);
    int r = tr * lr * 2 / 255;
    int g = tg * lg * 2 / 255;
    int b = tb * lb * 2 / 255;

#if 1
    return ClampWithDesaturation(r,g,b);
#else
    return clamp(tr*lr / 255,0,255) * 65536
         + clamp(tg*lg / 255, 0, 255) * 256
         + clamp(tb*lb / 255, 0, 255);
#endif
}

static void PutColor(int* target, struct vec3d color)
{
    *target = ClampWithDesaturation(color.x, color.y, color.z);
}

static void AddColor(int* target, struct vec3d color)
{
    int r = ((*target >> 16) &0xFF) + color.x;
    int g = ((*target >>  8) &0xFF) + color.y;
    int b = ((*target >>  0) &0xFF) + color.z;

    *target = ClampWithDesaturation(r,g,b);
}

static struct vec3d PerturbNormal(struct vec3d normal, struct vec3d tangent, struct vec3d bitangent, int normal_sample)
{
    struct vec3d perturb = 
    { 
        ((normal_sample >> 16) & 0xFF) / 127.5f - 1.f,  
        ((normal_sample >>  8) & 0xFF) / 127.5f - 1.f,  
        ((normal_sample >>  0) & 0xFF) / 127.5f - 1.f   
    };

    return (struct vec3d) 
    {
        normal.x * perturb.z + bitangent.x * perturb.y + tangent.x * perturb.x,
        normal.y * perturb.z + bitangent.y * perturb.y + tangent.y * perturb.x,
        normal.z * perturb.z + bitangent.z * perturb.y + tangent.z * perturb.x
    };  
}

static void GetSectorBoundingBox(int sectorno, struct vec2d* bounding_min, struct vec2d* bounding_max)
{
    const struct sector* sect = &sectors[sectorno];
    for(int s = 0; s < sect->nPoints; ++s)
    {
        bounding_min->x = min(bounding_min->x, sect->vertex[s].x);
        bounding_min->y = min(bounding_min->y, sect->vertex[s].y);
        bounding_max->x = max(bounding_max->x, sect->vertex[s].x);
        bounding_max->y = max(bounding_max->y, sect->vertex[s].y);

    }
}

// Return values:
//  0 = clear path, nothing hit
//  1 = hit, *result indicates where it hit
//  2 = a direct path doesn't lead to this sector
static int IntersectRay(struct vec3d origin, int origin_sectorno, struct vec3d target, int target_sectorno, struct Intersection* result)
{
    unsigned n_rescan = 0;
    int prev_sectorno = -1;

rescan:;
    ++n_rescan;

    struct sector* sect = &sectors[origin_sectorno];

    //Check if this beam hits one of the sector's edges.
    unsigned u=0, v=0, lu=0, lv=0;
    struct vec3d tangent, bitangent;

    for(int s = 0; s < sect->nPoints; ++s)
    {
        float vx1 = sect->vertex[s+0].x;
        float vy1 = sect->vertex[s+0].y;
        float vx2 = sect->vertex[s+1].x;
        float vy2 = sect->vertex[s+1].y;

        if(!IntersectLineSegments(origin.x, origin.z, target.x, target.z, vx1, vy1, vx2, vy2))
            continue;

        // Determine the X and Z coordinates of the wall hit.
        struct vec2d hit = Intersect(origin.x, origin.z, target.x, target.z, vx1, vy1, vx2, vy2);
        float x = hit.x;
        float z = hit.y;

        // Also determine the Y coordinate
        float y = origin.y + ((abs(target.x - origin.x) > abs(target.z-origin.z))
                                ? ((x - origin.x) * (target.y - origin.y) / (target.x - origin.x))
                                : ((z - origin.z) * (target.y - origin.y) / (target.z - origin.z)));

        // Check where the hole is.
        float hole_low = 9e9, hole_high = -9e9;

        if(sect->neighbors[s] >= 0)
        {
            hole_low = max(sect->floor, sectors[sect->neighbors[s]].floor);
            hole_high = min(sect->ceil, sectors[sect->neighbors[s]].ceil);        
        }

        if(y >= hole_low && y <= hole_high)
        {
            // the point fit in between  this hole.
            origin_sectorno = sect->neighbors[s];
            origin.x        = x + (target.x - origin.x)*1e-2;
            origin.y        = y + (target.y - origin.y)*1e-2;
            origin.z        = z + (target.z - origin.z)*1e-2;

            float distance = vlen(target.x - origin.x, target.y - origin.y, target.z - origin.z);

            if(origin_sectorno == prev_sectorno)
            {
                continue;
            }
            if(distance < 1e-3f || origin_sectorno == prev_sectorno)
            {
                goto close_enough;
            }

            prev_sectorno = origin_sectorno;
            goto rescan;
        }

        // Hit the road jack
        if(y < sect->floor)
            goto hit_floor;
        // D:! hit the ceil
        if(y > sect->ceil)
            goto hit_ceil;

        // Oh now hit the wall!
        result->where       = (struct vec3d) { x, y, z};
        result->surface     = (y < hole_low) ? &sect->lowertextures[s] : &sect->uppertextures[s];
        result->sectorno    = origin_sectorno;

        float nx  = vy2-vy1;
        float nz  = vx1-vx2;
        float len = sqrtf(nx*nx + nz*nz);

        result->normal = (struct vec3d){ nx/len, 0, nz/len };

        nx  = vx2 - vx1;
        nz  = vy2 - vy1;
        len = sqrtf(nx * nx + nz * nz);
        tangent = (struct vec3d){ nx/len, 0, nz/len };
        bitangent = (struct vec3d){ 0, 1, 0};

        // Calculate the texture coordinates.
        float dx = vx2 - vx1;
        float dy = vy2 - vy1;

        v = (unsigned)((y - sect->floor) * 1024.0f / (sect->ceil - sect->floor)) % 1024u;
        u = (abs(dx) > abs(dy) ? (unsigned)((x-vx1)*1024/dx)
                               : (unsigned)((z-vy1)*1024/dy)) % 1024u;

        // Lightmap coordinate are the same as texture coordinates.
        lu = u;
        lv = v;

    perturb_normal:;
        int texture_sample = result->surface->texture[v][u];
        int normal_sample  = result->surface->normalmap[v][u];
        int light_sample   = result->surface->lightmap[lv][lu];
        result->sample = ApplyLight(texture_sample, light_sample);
        result->normal = PerturbNormal(result->normal, tangent, bitangent, normal_sample);
        return 1;
    }

    if(target.y > sect->ceil)
    {
        hit_ceil:
            result->where.y = sect->ceil;
            result->surface = sect->ceiltexture;
            result->normal  = (struct vec3d){ 0, -1, 0 };
            tangent         = (struct vec3d){ 1, 0, 0 };
            bitangent       = vxs3(result->normal.x, result->normal.y, result->normal.z, tangent.x, tangent.y, tangent.z); 
        hit_ceil_or_floor:
            // Either the floor or ceiling was hit. Determine X & Z coordinates.
            result->where.x = (result->where.y - origin.y) * (target.x - origin.x) / (target.y - origin.y) + origin.x;
            result->where.z = (result->where.y - origin.y) * (target.z - origin.z) / (target.y - origin.y) + origin.z;

            // Calculate the texture coordinates.
            u = ((unsigned)(result->where.x * 256)) % 1024u;
            v = ((unsigned)(result->where.z * 256)) % 1024u;

            // Calculate the lightmap coordinates.
            struct vec2d bounding_min = { 1e9f, 1e9f };
            struct vec2d bounding_max = { -1e9f, -1e9f };
            GetSectorBoundingBox(origin_sectorno, &bounding_min, &bounding_max);
            lu = ((unsigned)((result->where.x - bounding_min.x) * 1024 / (bounding_max.x - bounding_min.x))) % 1024;
            lv = ((unsigned)((result->where.y - bounding_min.y) * 1024 / (bounding_max.y - bounding_min.y))) % 1024;

            goto perturb_normal;
    }

    if(target.y < sect->floor)
    {
        hit_floor:
            result->where.y = sect->floor;
            result->surface = sect->floortexture;
            result->normal  = (struct vec3d){ 0, 1, 0};
            tangent         = (struct vec3d){ -1, 0, 0};
            bitangent       = vxs3(result->normal.x, result->normal.y, result->normal.z, tangent.x, tangent.y, tangent.z); 
            
            goto hit_ceil_or_floor;
    }

    close_enough:;
        // Is the target in this sector?
        return origin_sectorno == target_sectorno ? 0 : 2;
}

#define narealightcomponents    32
#define area_light_radius       0.4
#define nrandomvectors          128
#define firstround              1
#define maxrounds               100
#define fade_distance_diffuse   10.0
#define fade_distance_radiosity 10.0
#define radiomul                1.0

static struct vec3d tvec[nrandomvectors];
static struct vec3d avec[narealightcomponents];

static void DiffuseLightCalculation(struct vec3d normal, struct vec3d tangent, struct vec3d bitangent,
                                    struct TextureSet* texture, unsigned tx, unsigned ty,
                                    unsigned lx, unsigned ly,  struct vec3d point_in_wall,
                                    unsigned sectorno)
{
    struct vec3d perturbed_normal = PerturbNormal(normal, tangent, bitangent, texture->normalmap[tx][ty]);

    // For each lightsource, check if ther is an obstacle in between this vertex and the lightsource.
    // Calculate the ambient light levels from the fact.
    // This simulates diffuse light.
    struct vec3d color = {0, 0, 0};
    for(unsigned l=0; l<NumLights; ++l)
    {
        const struct light* light = &lights[l];
        struct vec3d source = 
        { 
            point_in_wall.x + normal.x * 1e-5f,
            point_in_wall.y + normal.y * 1e-5f,
            point_in_wall.z + normal.z * 1e-5f
        };

        for(unsigned qa = 0; qa < narealightcomponents; ++qa)
        {
            struct vec3d target  = { light->where.x + avec[qa].x, light->where.y + avec[qa].y, light->where.z + avec[qa].z };
            struct vec3d towards = { target.x - source.x, target.y - source.y, target.z - source.z };
            float len = vlen(towards.x, towards.y, towards.z);
            float invlen = 1.0f / len;
            
            towards.x *= invlen;
            towards.y *= invlen;
            towards.z *= invlen;

            float cosine = vdot3(perturbed_normal.x, perturbed_normal.y, perturbed_normal.z, towards.x, towards.y, towards.z);
            float power  = cosine / (1.f + powf(len / fade_distance_diffuse, 2.0f));
            power /= (float) narealightcomponents;

            if(power > 1e-7f)
            {
                struct Intersection i;
                if(IntersectRay(source, sectorno, target, light->sector, &i) == 0)
                {
                    color.x += light->light.x * power;
                    color.y += light->light.y * power;
                    color.z += light->light.z * power;
                }
            }
        }
    }

    PutColor(&texture->lightmap[lx][ly], color);
}

static void RadiosityCalculation(struct vec3d normal, struct vec3d tangent, struct vec3d bitangent,
                                 struct TextureSet* texture, unsigned tx, unsigned ty,
                                 unsigned lx, unsigned ly, struct vec3d point_in_wall,
                                 unsigned sectorno)
{
    struct vec3d perturbed_normal = PerturbNormal(normal, tangent, bitangent, texture->normalmap[tx][ty]);

    // Shoot rays to each random direction and see what it hits.
    // Take the last round's light value from that location.
    struct vec3d source = 
    {
        point_in_wall.x + normal.x * 1e-3f,
        point_in_wall.y + normal.y * 1e-3f,
        point_in_wall.z + normal.z * 1e-3f
    };

    float basepower = radiomul / nrandomvectors;

    // Apply the set of random vectors to this surface.
    // This produces a set of vectors all pointing away
    // from the wall to random directions.
    struct vec3d color = { 0, 0, 0 };
    for(unsigned qq = 0; qq < nrandomvectors; ++qq)
    {
        struct vec3d rvec = tvec[qq];

        // If the random vector points to the wrong side from the wall, flip it
        if(vdot3(rvec.x, rvec.y, rvec.z, normal.x, normal.y, normal.z) < 0)
        {
            rvec.x = -rvec.x;
            rvec.y = -rvec.y;
            rvec.z = -rvec.z;
        }

        struct vec3d target =
        {
            source.x + rvec.x * 512.f,
            source.y + rvec.y * 512.f,
            source.z + rvec.z * 512.f
        };

        struct Intersection i;
        if(IntersectRay(source, sectorno, target, -1, &i) == 1)
        {
            float cosine = vdot3(perturbed_normal.x, i.normal.x,
                                 perturbed_normal.y, i.normal.y,
                                 perturbed_normal.z, i.normal.z) * basepower;

            float len = vlen(i.where.x - source.x, i.where.y - source.y, i.where.z - source.z);
            float power = abs(cosine) / (1.f + powf(len / fade_distance_radiosity, 2.0f));

            color.x += ((i.sample >> 16) & 0xFF) * power;
            color.y += ((i.sample >>  8) & 0xFF) * power;
            color.z += ((i.sample >>  0) & 0xFF) * power;
        }
    }

    AddColor(&texture->lightmap[lx][ly], color);
}

static void Begin_Radiosity(struct TextureSet* set)
{
    memcpy(&set->lightmap, &set->lightmap_diffuseonly, sizeof(Texture));
}

static double End_Radiosity(struct TextureSet* set, const char* label)
{
    long differences = 0;
    for(unsigned x=0; x < 1024; ++x)
    {
        for(unsigned y = 0; y < 1024; ++y)
        {
            int old = set->lightmap_diffuseonly[x][y];
            int r = (old >> 16) & 0xFF;
            int g = (old >>  8) & 0xFF;
            int b = (old) & 0xFF;

            int new = set->lightmap[x][y];
            r -= (new >> 16) & 0xFF;
            g -= (new >>  8) & 0xFF;
            b -= (new) & 0xFF;

            differences += abs(r) + abs(g) + abs(b);
        }
    }

    double result = differences / (double)(1024 * 1024);
    fprintf(stderr, "Differences in %s: %g\33[K\n", label, result);
    return result;
}

static void End_Diffuse(struct TextureSet* set)
{
    memcpy(&set->lightmap_diffuseonly, &set->lightmap, sizeof(Texture));
}

#ifdef _OPENMP
#include <omp.h>

#define OMP_SCALER_LOOP_BEGIN(a,b,c,d,e,f) do { \
        int this_thread = omp_get_thread_num(), num_threads = omp_get_num_threads(); \
        int my_start = (this_thread  ) * ((c)-(a)) / num_threads + (a); \
        int my_end   = (this_thread+1) * ((c)-(a)) / num_threads + (a); \
        struct Scaler e##int = Scaler_Init(a, my_start, (c)-1, (d) * 32768, (f) * 32768); \
        for(int b = my_start; b < my_end; ++b) \
        { \
            float e = Scaler_Next(&e##int) / 32768.f;

#else
#define OMP_SCALER_LOOP_BEGIN(a,b,c,d,e,f) do { \
        struct Scaler e##int = Scaler_Init(a, a ,(c)-1, (d)*32768, (f)*32768); \
        for(int b = (a); b < (c); ++b) \
        {\
            float e = Scaler_Next(&e##int) / 32768.f; 
#endif

#define OMP_SCALER_LOOP_END() \
        } \
    } while(0)


// Lightmap calculation involes some raytracing.
static void BuildLightmaps(void)
{
    for(unsigned round = firstround; round<=maxrounds; ++round)
    {
        fprintf(stderr, "Lighting calculation, round %u...\n", round);
#ifndef _OPENMP
    fprintf(stderr, "Note: This would probably go faster if you enabled OpenMP in your compiler options. It's -fopenmp in GCC and Clang. \n");
#endif

        // Create unformly distributed random unit vectors
        for(unsigned n = 0; n < nrandomvectors; ++n)
        {
            double u = (rand() % 1000000) / 1e6;
            double v = (rand() % 1000000) / 1e6;
            double theta = 2*3.141592653 * u;
            double phi = acos(2*v-1);
            tvec[n].x = cos(theta) * sin(phi);
            tvec[n].y = sin(theta) * sin(phi);
            tvec[n].z = cos(phi);
        }

        //  A lightsource is represented by a spherical cloud of smaller lightsources around the actual lightsource.
        // The achieves smooth edges for the shadows.
        #define drand(m) ((rand()%1000-500)*5e-2*m)
        for(unsigned qa=0; qa < narealightcomponents; ++qa)
        {
            double len;
            do{
                avec[qa] = (struct vec3d){ drand(100.0), drand(100.0), drand(100.0) };
                len = sqrt(avec[qa].x * avec[qa].x + avec[qa].y * avec[qa].y + avec[qa].z * avec[qa].z);
            } while(len < 1e-3);

            avec[qa].x *= area_light_radius / len;
            avec[qa].y *= area_light_radius / len;
            avec[qa].z *= area_light_radius / len;
        }
        
        #undef drand

        fprintf(stderr, "Note: You can interrupt this program at any time you want. If you wish to resume\n"
                        "      the lightmap calculation at a later date, use the --rebuild commandline option.\n"
                        "      If you have already finished round 1 (diffuse light), and don't wish to do that\n"
                        "      again, change the '#define firstround' value to your liking. Value 1 means\n"
                        "      it starts from beginning, and any value from 2-100 (actual value is not important)\n"
                        "      means to progressively improve the radiosity (cumulative). The current value is %d.\n",
            firstround);

        double total_differences = 0;
        for(unsigned sectorno = 0; sectorno < NumSectors; ++sectorno)
        {
            struct sector* const sect = &sectors[sectorno];
            const struct vec2d* const vert = sect->vertex;

            double sector_differences = 0;

            if(1) // do ceiling and floor
            {
                struct vec2d bounding_min = { 1e9f, 1e9f };
                struct vec2d bounding_max = { -1e9f, -1e9f };

                GetSectorBoundingBox(sectorno, &bounding_min, &bounding_max);
                struct vec3d floornormal    = (struct vec3d){0, 1, 0}; // floor
                struct vec3d floortangent   = (struct vec3d){1, 0, 0};
                struct vec3d floorbitangent = vxs3(floornormal.x, floornormal.y, floornormal.z, floortangent.x, floortangent.y, floortangent.z);
                struct vec3d ceilnormal     = (struct vec3d){0, -1, 0}; // ceiling
                struct vec3d ceiltangent    = (struct vec3d){1, 0, 0};
                struct vec3d ceilbitangent  = vxs3(ceilnormal.x, ceilnormal.y, ceilnormal.z, ceiltangent.x, ceiltangent.y, ceiltangent.z);

                fprintf(stderr, "Bounding box for sector %d/%d: %g,%g - %g,%g\n", sectorno+1, NumSectors, bounding_min.x, 
                        bounding_min.y, bounding_max.x, bounding_max.y);

                // Round 1: Check lightsources
                if(round == 1)
                {
                    struct Scaler txtx_int = Scaler_Init(0,0,1023, bounding_min.x*32768, bounding_max.x*32768);
                    for(unsigned x = 0; x < 1024; ++x)
                    {
                        fprintf(stderr, "- Sector %d ceil&floor, %u/%u diffuse light...\r", sectorno+1, x, 1024);
                        float txtx = Scaler_Next(&txtx_int) / 32768.f;

                        // For better cache locality, fist, do floors and then ceils
                        #pragma omp parallel
                        OMP_SCALER_LOOP_BEGIN(0, y, 1024, bounding_min.y, txty, bounding_max.y);
                            DiffuseLightCalculation(floornormal, floortangent, floorbitangent, sect->floortexture, ((unsigned)(txtx*256)) % 1024,
                            ((unsigned)(txty*256)) % 1024, x, y, (struct vec3d){txtx, sect->floor, txty}, sectorno);
                        OMP_SCALER_LOOP_END();
                        #pragma omp parallel
                        OMP_SCALER_LOOP_BEGIN(0, y, 1024, bounding_min.y, txty, bounding_max.y);
                            DiffuseLightCalculation(ceilnormal, ceiltangent, ceilbitangent, sect->ceiltexture, ((unsigned)(txtx*256)) % 1024,
                            ((unsigned)(txty*256)) % 1024, x, y, (struct vec3d){txtx, sect->ceil, txty}, sectorno);
                        OMP_SCALER_LOOP_END();
                    }

                    fprintf(stderr, "\n");
                    End_Diffuse(sect->floortexture);
                    End_Diffuse(sect->ceiltexture);
                }
                else
                {
                    // Round 2+: Radiosity
                    Begin_Radiosity(sect->floortexture);
                    Begin_Radiosity(sect->ceiltexture);

                    // Calculate radiosity in decreased resolution
                    struct Scaler txtx_int = Scaler_Init(0,0,1023, bounding_min.x*32768, bounding_max.x*32768);
                    for(unsigned x = 0; x < 1024; ++x)
                    {
                        fprintf(stderr, "- Sector %d ceil&floor, %u/%u radiosity...\r", sectorno+1, x, 1024);
                        float txtx = Scaler_Next(&txtx_int) / 32768.f;

                        // For better cache locality, fist, do floors and then ceils
                        #pragma omp parallel
                        OMP_SCALER_LOOP_BEGIN(0, y, 1024, bounding_min.y, txty, bounding_max.y);
                            RadiosityCalculation(floornormal, floortangent, floorbitangent, sect->floortexture, ((unsigned)(txtx*256)) % 1024,
                            ((unsigned)(txty*256)) % 1024, x, y, (struct vec3d){txtx, sect->floor, txty}, sectorno);
                        OMP_SCALER_LOOP_END();
                        #pragma omp parallel
                        OMP_SCALER_LOOP_BEGIN(0, y, 1024, bounding_min.y, txty, bounding_max.y);
                            RadiosityCalculation(ceilnormal, ceiltangent, ceilbitangent, sect->ceiltexture, ((unsigned)(txtx*256)) % 1024,
                            ((unsigned)(txty*256)) % 1024, x, y, (struct vec3d){txtx, sect->ceil, txty}, sectorno);
                        OMP_SCALER_LOOP_END();
                    }

                    char Buf[128];
                    sprintf(Buf, "Sector %u floors", sectorno + 1);
                    sector_differences += End_Radiosity(sect->floortexture, Buf);
                    sprintf(Buf, "Sector %u ceils", sectorno + 1);
                    sector_differences += End_Radiosity(sect->ceiltexture, Buf);
                }
            }

            if(1)
            {
                for(unsigned s=0; s < sect->nPoints; ++s)
                {
                    float xd = vert[s+1].x - vert[s].x;
                    float zd = vert[s+1].y - vert[s].y;
                    float len = vlen(xd, zd, 0);

                    struct vec3d normal     = { -zd/len, 0, xd/len};
                    struct vec3d tangent    = {xd/len, 0, zd/len};
                    struct vec3d bitangent  = {0, 1, 0};

                    float hole_low  = 9e9;
                    float hole_high = -9e9;

                    if(sect->neighbors[s] >= 0)
                    {
                        hole_low  = max(sect->floor, sectors[sect->neighbors[s]].floor);
                        hole_high = min(sect->ceil, sectors[sect->neighbors[s]].ceil);
                    }

                    if(round == 1)
                    {
                        // Round 1: Check lightsources
                        struct Scaler txtx_int = Scaler_Init(0,0,1023, vert[s].x*32768, vert[s+1].x*32768);
                        struct Scaler txtz_int = Scaler_Init(0,0,1023, vert[s].y*32768, vert[s+1].y*32768);

                        for(unsigned x=0; x < 1024; ++x)
                        {
                            float txtx = Scaler_Next(&txtx_int) / 32768.f;
                            float txtz = Scaler_Next(&txtz_int) / 32768.f;

                            fprintf(stderr, "- Sector %u Wall %u/%u %u/%u diffuse light...\r", sectorno+1, s+1, sect->nPoints, x, 1024);
                            #pragma omp parallel
                            OMP_SCALER_LOOP_BEGIN(0,y,1024, sect->ceil, txty, sect->floor);
                                struct TextureSet* texture = &sect->uppertextures[s];

                                if(sect->neighbors[s] >= 0 && txty < hole_high)
                                {
                                    if(txty > hole_low)
                                        continue;
                                    texture = &sect->lowertextures[s];
                                }

                                struct vec3d point_in_wall = { txtx, txty, txtz };
                                DiffuseLightCalculation(normal, tangent, bitangent, texture, x, y, x, y, point_in_wall, sectorno);
                            OMP_SCALER_LOOP_END();
                        }

                        End_Diffuse(&sect->uppertextures[s]);
                        End_Diffuse(&sect->lowertextures[s]);
                    }
                    else
                    {
                        Begin_Radiosity(&sect->uppertextures[s]);
                        Begin_Radiosity(&sect->lowertextures[s]);

                        // Round 2+: Radiosity 
                        struct Scaler txtx_int = Scaler_Init(0,0,1023, vert[s].x*32768, vert[s+1].x*32768);
                        struct Scaler txtz_int = Scaler_Init(0,0,1023, vert[s].y*32768, vert[s+1].y*32768);
                        for(unsigned x=0; x < 1024; ++x)
                        {
                            float txtx = Scaler_Next(&txtx_int) / 32768.f;
                            float txtz = Scaler_Next(&txtz_int) / 32768.f;

                            fprintf(stderr, "- Sector %u Wall %u/%u %u/%u radiosity...\r", sectorno+1, s+1, sect->nPoints, x, 1024);
                            #pragma omp parallel
                            OMP_SCALER_LOOP_BEGIN(0,y,1024, sect->ceil, txty, sect->floor);
                                struct TextureSet* texture = &sect->uppertextures[s];

                                if(sect->neighbors[s] >= 0 && txty < hole_high)
                                {
                                    if(txty > hole_low)
                                        continue;
                                    texture = &sect->lowertextures[s];
                                }

                                struct vec3d point_in_wall = { txtx, txty, txtz };
                                RadiosityCalculation(normal, tangent, bitangent, texture, x, y, x, y, point_in_wall, sectorno);
                            OMP_SCALER_LOOP_END();
                        }

                        char Buf[128];
                        sprintf(Buf, "Sector %u wall %u upper texture", sectorno + 1, s+1);
                        sector_differences += End_Radiosity(&sect->uppertextures[s], Buf);
                        sprintf(Buf, "Sector %u wall %u lower texture", sectorno + 1, s+1);
                        sector_differences += End_Radiosity(&sect->lowertextures[s], Buf);
                    }

                    fprintf(stderr, "\n");
                }
            }

            fprintf(stderr, "Round %u differences in sector %u: %g\n", round, sectorno+1, sector_differences);
            total_differences += sector_differences;
        }

        fprintf(stderr, "Round %u differences total: %g.\n", round, total_differences);
        if(total_differences < 1e-6)
        {
            break;
        }
    }
}
#endif
#endif

// Helper function for the antialiased line algorithm.
#define fpart(x) ((x) < 0 ? 1 - ((x) - floorf(x)) : (x) - floorf(x))
#define rfpart(x) (1 -fpart(x))
static void plot(int x, int y, float opacity, int color)
{
    opacity = powf(opacity, 1/2.2f);
    int *pix = ((int*) surface->pixels) + y * W2 + x;
    int r0 = (*pix >> 16) & 0xFF, r1 = (color >> 16) & 0xFF;
    int g0 = (*pix >>  8) & 0xFF, g1 = (color >>  8) & 0xFF;
    int b0 = (*pix >>  0) & 0xFF, b1 = (color >>  0) & 0xFF;
    int r = max(r0, opacity*r1);
    int g = max(g0, opacity*g1);
    int b = max(b0, opacity*b1);

    *pix = (r << 16) | (g << 8) | b;
}

static void line(float x0, float y0, float x1, float y1, int color)
{
    // Xiaolin Wu's antialased algorithm
    int steep = fabsf(y1-y0) > fabsf(x1-x0);
    if(steep)
    {
        float tmp;
        tmp = x0;
        x0 = y0;
        y0 = tmp;
        tmp = x1;
        x1 = y1;
        y1 = tmp;
    }

    if(x0 > x1)
    {
        float tmp;
        tmp = x0;
        x0 = x1;
        x1 = tmp;
        tmp = y0;
        y0 = y1;
        y1 = tmp;
    }

    float dx = x1-x0, dy = y1-y0, gradient = dy/dx;

    // Handle first endpoint
    int xend = (int)(x0 + 0.5f);
    int yend = y0 + gradient * (xend - x0);
    float xgap = rfpart(x0 +0.5f);
    int xpxl1 = xend;
    int ypxl1 = (int)(yend);

    if(steep)
    {
        plot(ypxl1,   xpxl1, rfpart(yend) * xgap, color);
        plot(ypxl1+1, xpxl1, fpart(yend) * xgap, color);
    }
    else
    {
        plot(xpxl1, ypxl1, rfpart(yend) * xgap, color);
        plot(xpxl1, ypxl1+1, fpart(yend) * xgap, color);
    }

    float intery = yend + gradient;
    xend = (int)(x1 + 0.5f);
    yend = y1 + gradient * (xend - x1);
    xgap = fpart(x1 + 0.5);

    int xpxl2 = xend;
    int ypxl2 = (int)(yend);

    if(steep)
    {
        plot(ypxl2, xpxl2, rfpart(yend) *  xgap, color);
        plot(ypxl2+1, xpxl2, fpart(yend) * xgap, color);
    }
    else
    {
        plot(xpxl2, ypxl2, rfpart(yend) * xgap, color);
        plot(xpxl2, ypxl2+1, fpart(yend) * xgap, color);
    }

    // main loop
    for(int x = xpxl1 +1; x < xpxl2; ++x, intery += gradient)
    {
        if(steep)
        {
            plot((int)(intery), x,  rfpart(intery), color);
            plot((int)(intery)+1, x, fpart(intery), color);
        }
        else
        {
            plot(x, (int)(intery),  rfpart(intery), color);
            plot(x, (int)(intery)+1, fpart(intery), color);
        }
    }
}

// Bloom Postprocess add some bloom to the 2D map image.
static void BloomPostprocess(void)
{
    const int blur_width = W/120;
    const int blur_height = H/90;

    float blur_kernel[blur_height*2+1][blur_width*2+1];
    for(int y =-blur_height; y <= blur_height; ++y)
    {
        for(int x=-blur_width; x<=blur_width; ++x)
       {
           float value = expf(-(x*x+y*y) / (2.f*(0.5f*max(blur_width, blur_height))));
           blur_kernel[y+blur_height][x+blur_width] = value * 0.3f;
       } 
    }

    static int pixels_original[W2*H];
    static struct pixel { float r,g,b,brightness; } img[W2*H];
    memcpy(pixels_original, surface->pixels, sizeof(pixels_original));

    int *pix = ((int*) surface->pixels);

    for(unsigned y = 0; y < H; ++y)
    {
        for(unsigned x = 0; x < W2; ++x)
        {
            int original_pixel = pixels_original[y*W2+x];
            float r = (original_pixel >> 16) & 0xFF;
            float g = (original_pixel >>  8) & 0xFF;
            float b = (original_pixel >>  0) & 0xFF;
            float wanted_br = original_pixel == 0xFFFFFF ? 1
                            : original_pixel == 0x55FF55 ? 0.6
                            : original_pixel == 0xFFAA55 ? 1
                            : 0.1;
            float brightness = powf((r*0.299f + g*0.587f + b*0.114f) / 255.f, 12.f/2.2f);
            brightness = (brightness * 0.2f + wanted_br * 0.3f + max(max(r,g),b) * 0.5f/255.f);
            img[y*W2+x] = (struct pixel){ r,g,b,brightness };
        }
    }

    #pragma omp parallel for schedule(static) collapse(2)
    for(unsigned y=0; y<H; ++y)
    {
#if SplitScreen
        for(unsigned x=W; x<W2; ++x)
#else
        for(unsigned x=0; x<W; ++x)
#endif  
        {
            int ypmin = max(0, (int)y - blur_height);
            int ypmax = min(H-1, (int)y + blur_height);
            int xpmin = max(0, (int)x - blur_width);
            int xpmax = min(W-1, (int)x + blur_width);

            float rsum = img[y*W2+x].r;
            float gsum = img[y*W2+x].g;
            float bsum = img[y*W2+x].b;

            for(int yp = ypmin; yp <= ypmax; ++yp)
            {
                for(int xp = xpmin; xp <= xpmax; ++xp)
                {
                    float r = img[yp*W2+xp].r;
                    float g = img[yp*W2+xp].g;
                    float b = img[yp*W2+xp].b;

                    float brightness = img[yp*W2+xp].brightness;
                    float value = brightness * blur_kernel[yp + blur_height-(int)y][xp + blur_width - (int)x];

                    rsum += r * value;
                    gsum += g * value;
                    bsum += b * value;
                }
            }

                int color = (((int)clamp(rsum,0,255)) << 16)
                          + (((int)clamp(gsum,0,255)) <<  8)
                          + (((int)clamp(bsum,0,255)) <<  0);
                pix[y*W2+x] = color;
        }
    }
}

// Fillpolygon draws a filled polygon - used only in the 2D map rendering
static void fillpolygon(const struct sector* sect, int color)
{
#if SplitScreen
    float square = min(W/20.f/0.8, H/29.f);
    float X      = (W2-W)/20.f;
    float Y      = square;
    float X0     = W+X*1.f;
    float Y0     = (H-28*square)/2;
#else
    float square = min(W/20.f/0.8, H/29.f);
    float X      = square * 0.8;
    float Y      = square;
    float X0     = (W-18*square*0.8)/2;
    float Y0     = (H-28*square)/2;
#endif

    const struct vec2d* const vert = sect->vertex;

    // Find the minimum and maximum Y coordinates
    float miny = 9e9, maxy = -9e9;
    for(unsigned a = 0; a < sect->nPoints; ++a)
    {
        miny = min(miny, 28-vert[a].x);
        maxy = max(maxy, 28-vert[a].x);
    }

    miny = Y0 + miny * Y;
    maxy = Y0 + maxy * Y;

    // Scan each line within this range
    for(int y = max(0, (int)(miny+0.5)); y <= min(H-1, (int)(maxy+0.5)); ++y)
    {
        // Find all intersection points on this scanline
        float intersections[W2];
        unsigned num_intersections = 0;
        for(unsigned a = 0; a < sect->nPoints && num_intersections < W; ++a)
        {
            float x0 = X0 + vert[a].y*X;
            float x1 = X0 + vert[a+1].y*X;
            float y0 = Y0+(28-vert[a].x)*Y;
            float y1 = Y0+(28-vert[a+1].x)*Y;

            if(IntersectBox(x0,y0,x1,y1,0,y,W2-1,y))
            {
                struct vec2d point = Intersect(x0,y0,x1,y1,0,y,W2-1,y);
                if(isnan(point.x) || isnan(point.y))
                    continue;

                // Insert it in intersections[] keeping it sorted.
                // Sorting complexity: n log n
                unsigned begin  = 0;
                unsigned end    = num_intersections;
                unsigned len    = end - begin;

                while(len)
                {
                    unsigned middle = begin + len/2;
                    if(intersections[middle] < point.x)
                    {
                        begin = middle++;
                        len = len - len/2 -1;
                    }
                    else
                    {
                        len /=2;
                    }
                }

                for(unsigned n = num_intersections++; n > begin; --n)
                {
                    intersections[n] = intersections[n-1];
                }

                intersections[begin] = point.x;
            }
        }

        // Draw lines
        for(unsigned a = 0; a+1 < num_intersections; a+=2)
        {
            line(clamp(intersections[a], 0, W2-1), y, clamp(intersections[a+1], 0, W2-1), y, color);
        }
    }
}

static void DrawMap(void)
{
    static unsigned process = ~0u;
    ++process;

    // Render the 2d map on screen
    SDL_LockSurface(surface);
#if SplitScreen
    for(unsigned y = 0; y < H; ++y)
        memset((char*)surface->pixels + (y*W2+W)*4, 0, (W2-W)*4);
#else
    for(unsigned y = 0; y < H; ++y)
        memset((char*)surface->pixels + (y*W2)*4, 0, (W)*4);
#endif    

#if SplitScreen
    float square = min(W/20.f/0.8, H/29.f);
    float X = (W2-W)/20.f;
    float Y = square;
    float X0 = W+X*1.f;
    float Y0 = (H-28*square)/2;
#else
    float square = min(W/20.f/0.8, H/29.f);
    float X = square * 0.8;
    float Y = square;
    float X0 = (W-18*square*0.8)/2;
    float Y0 = (H-28*square)/2;
#endif

    for(float x=0; x <= 18; ++x)
    {
        line(X0+x*X, Y0+0*Y, X0+x*X, Y0+28*Y, 0x002200);
    } 

    for(float y=0; y <= 28; ++y)
    {
        line(X0+0*X, Y0+y*Y, X0+18*X, Y0+y*Y, 0x002200);
    } 

#if VisibilityTracking
    for(unsigned c=0; c<NumSectors; ++c)
    {
        if(sectors[c].visible)
        {
            fillpolygon(&sectors[c], 0x220000);
        }
    }
#endif

    fillpolygon(&sectors[player.sector], 0x440000);

#if VisibilityTracking
    for(unsigned c = 0; c < NumVisibleSectors; ++c)
    {
        for(unsigned x = 0; x < W; ++x)
        {
            if(VisibleFloors[c][x])
            {
                line(clamp(X0 + VisibleFloorBegins[c][x].y*X,0,W2-1), clamp(Y0 + (28-VisibleFloorBegins[c][x].x)*Y, 0,H-1),
                     clamp(X0 + VisibleFloorEnds[c][x].y*X,0,W2-1), clamp(Y0 + (28-VisibleFloorEnds[c][x].x)*Y, 0,H-1), 0x222200);
            }

            if(VisibleCeils[c][x])
            {
                line(clamp(X0 + VisibleCeilBegins[c][x].y*X,0,W2-1), clamp(Y0 + (28-VisibleCeilBegins[c][x].x)*Y, 0,H-1),
                     clamp(X0 + VisibleCeilEnds[c][x].y*X,0,W2-1), clamp(Y0 + (28-VisibleCeilEnds[c][x].x)*Y, 0,H-1), 0x28003A);
            }
        }
    }
#endif  

    for(unsigned c=0; c<NumSectors; ++c)
    {
        unsigned a = c;
        if(a == player.sector && player.sector != (NumSectors-1))
            a = NumSectors -1;
        else if(a == NumSectors-1)
            a = player.sector;
        
        const struct sector* const sect = &sectors[a];
        const struct vec2d* const vert = sect->vertex;

        for(unsigned b = 0; b < sect->nPoints; ++b)
        {
            float x0 = 28-vert[b].x;
            float x1 = 28-vert[b+1].x;
            unsigned vertcolor = a == player.sector ? 0x55FF55
#if VisibilityTracking
                                : sect->visible ? 0x55FF55
#endif
                                : 0x00AA00;

            line( X0 + vert[b].y*X, Y0+x0*Y, X0 + vert[b+1].y*X, Y0+x1*Y, 
                 (a == player.sector)
                 ? (sect->neighbors[b] >= 0 ? 0xFF5533 : 0xFFFFFF)
#if VisibilityTracking
                 : (sect->visible)
                 ? (sect->neighbors[b] >= 0 ? 0xFF3333 : 0xAAAAAA)
#endif
                 : (sect->neighbors[b] >= 0 ? 0x880000 : 0x6A6A6A)
                 );


            line( X0+vert[b].y*X-2, Y0+x0*Y-2, X0+vert[b].y*X+2, Y0+x0*Y-2, vertcolor);
            line( X0+vert[b].y*X-2, Y0+x0*Y-2, X0+vert[b].y*X-2, Y0+x0*Y+2, vertcolor);
            line( X0+vert[b].y*X+2, Y0+x0*Y-2, X0+vert[b].y*X+2, Y0+x0*Y+2, vertcolor);
            line( X0+vert[b].y*X-2, Y0+x0*Y+2, X0+vert[b].y*X+2, Y0+x0*Y+2, vertcolor);
        }
    }

    float c     = player.angleSin;
    float s     = -player.angleCos;
    float px    = player.where.y;
    float tx    = px + c * 0.8f;
    float qx0   = px + s * 0.2f;
    float qx1   = px - s * 0.2f;
    float py    = 28 - player.where.x;
    float ty    = py + s * 0.8f;
    float qy0   = py - c * 0.2f;
    float qy1   = py + c * 0.2f;

    px  = clamp(px, -0.4f, 18.4f);
    tx  = clamp(tx, -0.4f, 18.4f);
    qx0 = clamp(qx0, -0.4f, 18.4f);
    qx1 = clamp(qx1, -0.4f, 18.4f);
    py  = clamp(py, -0.4f, 28.4f);
    ty  = clamp(ty, -0.4f, 28.4f);
    qy0 = clamp(qy0, -0.4f,28.4f);
    qy1 = clamp(qy1, -0.4f,28.4f);

    line(X0 + px*X, Y0 + py *Y, X0 + tx*X, Y0 + ty*Y, 0x5555FF);
    line(X0 + qx0*X, Y0 + qy0*Y, X0 + qx1*X, Y0 + qy1*Y, 0x5555FF);

    BloomPostprocess();

    SDL_UnlockSurface(surface);
}

static int vert_compare(const struct vec2d* a, const struct vec2d* b)
{
    if(a->y != b->y) 
        return (a->y - b->y) * 1e3;

    return (a->x - b->x) * 1e3;
}

// Verify map for consistencies
static void VerifyMap(void)
{
Rescan:
    for(unsigned a = 0; a < NumSectors; ++a)
    {
        const struct sector* const sect = &sectors[a];
        const struct vec2d* const vert = sect->vertex;

        if(vert[0].x != vert[sect->nPoints].x || vert[0].y != vert[sect->nPoints].y)
        {
            fprintf(stderr, "Internal error: Sector %u: Vertexes don't form a loop!\n", a);
        }
    }

    // Verify that each edge that has a neighbor, the neighbor
    // has this same neighbor.
    for(unsigned a = 0; a < NumSectors; ++a)
    {
        const struct sector* sect = &sectors[a];
        const struct vec2d* const vert = sect->vertex;
        for(unsigned b = 0; b < sect->nPoints; ++b)
        {
            if(sect->neighbors[b] >= (int)NumSectors)
            {
                fprintf(stderr, "Sector %u: Contains neighbor %d (too large, number of sectors is %u)\n", a, sect->neighbors[b], NumSectors);
            }

            struct vec2d point1 = vert[b], point2 = vert[b+1];

            int found = 0;
            for(unsigned d = 0; d < NumSectors; ++d)
            {
                const struct sector* const neigh = &sectors[d];
                for(unsigned c = 0; c < neigh->nPoints; ++c)
                {
                    if(neigh->vertex[c+1].x == point1.x
                    && neigh->vertex[c+1].y == point1.y
                    && neigh->vertex[c+0].x == point2.x
                    && neigh->vertex[c+0].y == point2.y)
                   {
                       if(neigh->neighbors[c] != (int)a)
                       {
                            fprintf(stderr, "Sector %d: Neighbor behind line (%g,%g)-(%g,%g) should be %u, %d found instead. Fixing\n",
                                    d, point2.x, point2.y, point1.x, point1.y, a, neigh->neighbors[c]);
                            neigh->neighbors[c] = a;
                            goto Rescan;
                       }

                       if(sect->neighbors[b] != (int)d)
                       {
                            fprintf(stderr, "Sector %u: Neighbor behind line (%g,%g)-(%g,%g) shoul be %u, %d found instead. Fixing \n",
                                    a, point1.x, point1.y, point2.x, point2.y, d, sect->neighbors[b]);
                            sect->neighbors[b] = d;
                            goto Rescan;
                       }
                       else
                       {
                           ++found;
                       }
                   } 
                }
            }

            if(sect->neighbors[b]>=0 && sect->neighbors[b] < (int)NumSectors && found != 1)
            {
                fprintf(stderr, "Sectors %u and its neighbor %d don't share line (%g,%g)-(%g,%g)\n",
                        a, sect->neighbors[b], point1.x, point1.y, point2.x, point2.y);
            }
        }
    }

    // Verify that the vertexes from convex hull.
    for(unsigned a = 0; a < NumSectors; ++a)
    {
        struct sector* sect = &sectors[a];
        const struct vec2d* const vert = sect->vertex;
        for(unsigned b = 0; b < sect->nPoints; ++b)
        {
            unsigned c = (b+1) % sect->nPoints, d = (b+2) & sect->nPoints;
            float x0 = vert[b].x;
            float y0 = vert[b].y;
            float x1 = vert[c].x;
            float y1 = vert[c].y;

            switch(PointSide(vert[d].x, vert[d].y, x0, y0, x1, y1))
            {
                case 0:
                    continue;
                    if(sect->neighbors[b] == sect->neighbors[c])
                        continue;
                    fprintf(stderr, "Sector %u: Edges %u-%u and %u-%u are parallel, but have different neighbors. This would pose problems for collision detections.\n",
                            a, b, c, c, d);
                    break;
                case -1:
                    fprintf(stderr, "Sector %u: Edges %u-%u and %u-%u create a concave turn. This would be rendered wrong.\n",
                            a, b, c, c, d);
                    break;
                default: // All good :D
                    continue;
            }

            fprintf(stderr, "- Splitting sector, using (%g,%g) as anchor", vert[c].x, vert[c].y);

            // Insert and edge between (c) and (e),
            // where e is the nearest point to (c), under the following rules:
            // e cannot be c, c-1 or c+1
            // line (c)-(e) cannot intersect with any edge in this sector
            float nearest_dist          = 1e29f;
            unsigned nearest_point      = ~0u;
            for(unsigned n = (d+1) % sect->nPoints; n != b; n = (n+1) % sect->nPoints)
            {
                float x2 = vert[n].x;
                float y2 = vert[n].y;
                float distx = x2-x1;
                float disty = y2-y1;
                float dist = distx*distx + disty*disty;

                if(dist >= nearest_dist)
                    continue;

                if(PointSide(x2, y2, x0, y0, x1,y1) != 1)
                    continue;

                int ok =1;

                x1 += distx * 1e-4f;
                x2 -= distx * 1e-4f;
                y1 += disty * 1e-4f;
                y2 -= disty * 1e-4f;

                for(unsigned f = 0; f < sect->nPoints; ++f)
                {
                    if(IntersectLineSegments(x1, y1, x2, y2, vert[f].x, vert[f].y, vert[f+1].x, vert[f+1].y))
                    {
                        ok = 0;
                        break;
                    }
                }

                if(!ok)
                    continue;

                // Check whether this split would resolve the original problem
                if(PointSide(x2, y2, vert[d].x, vert[d].y, x1, y1) == 1)
                    dist += 1e6f;
                if(dist >= nearest_dist)
                    continue;

                nearest_dist = dist;
                nearest_point = n;
            }

            if(nearest_point == ~0u)
            {
                fprintf(stderr, " - ERROR: Could not find a vertex to pair with!\n");
                SDL_Delay(200);
                continue;
            }

            unsigned e = nearest_point;
            fprintf(stderr, " and point %u - (%g-%g) as the far point.\n", e, vert[e].x, vert[e].y);

            // Now that we have a chain: a b c d e f g h
            // And we're supposed to split it at "c" and "e", the outcome should be two chains:
            // c d e         (c)
            // e f g h a b c (e)

            struct vec2d* vert1 = malloc(sect->nPoints * sizeof(*vert1));
            struct vec2d* vert2 = malloc(sect->nPoints * sizeof(*vert2));
            signed char* neigh1 = malloc(sect->nPoints * sizeof(*neigh1));
            signed char* neigh2 = malloc(sect->nPoints * sizeof(*neigh2));

            // Create chan 1: from c to e.
            unsigned chain1_length = 0;
            for(unsigned n = 0; n < sect->nPoints; ++n)
            {
                unsigned m = (c + n) % sect->nPoints;
                neigh1[chain1_length] = sect->neighbors[m];
                vert1[chain1_length++] = sect->vertex[m];
                if(m==e)
                {
                    vert1[chain1_length] = vert1[0];
                    break;
                }
            }

            neigh1[chain1_length-1] = NumSectors;

            // Create chanin 2: from e to c.
            unsigned chain2_length = 0;
            for(unsigned n = 0; n < sect->nPoints; ++n)
            {
                unsigned m = (e+n) % sect->nPoints;
                neigh2[chain2_length] = sect->neighbors[m];
                vert2[chain2_length++] = sect->vertex[m];
                if(m == c)
                {
                    vert2[chain2_length] = vert2[0];
                    break;
                } 
            }

            neigh2[chain2_length-1] = a;
            
            // Change sect into using chain1.
            free(sect->vertex);
            sect->vertex =  vert1;
            free(sect->neighbors);
            sect->neighbors = neigh1;
            sect->nPoints = chain1_length;

            // Create another sector that uses chain2.
            sectors = realloc(sectors, ++NumSectors * sizeof(*sectors));
            sect = &sectors[a];
            sectors[NumSectors-1] = (struct sector) { sect->floor, sect->ceil, vert2, chain2_length, neigh2 };

            // The other sector may now have neighbors that think their neighbor is still the old sector.
            // Rescan to fix it.
            goto Rescan;
        }
    }

    printf("%d sectors. \n", NumSectors);
}

#if !TextureMapping
// viline: Draw a vertical line on screen, with a different color pixel in top and bottom
static void vline(int x, int y1, int y2, int top, int middle, int bottom)
{
    int *pix = (int *) surface->pixels;
    y1 = clamp(y1, 0, (H-1));
    y2 = clamp(y2, 0, (H-1));

    if(y2 == y1)
    {
        pix[y1*W2+x] = middle;
    }
    else if(y2 > y1)
    {
        pix[y1*W2+x] = top;
        for(int y = y1+1; y < y2; ++y)
        {
            pix[y*W2+x] = middle;
        }

        pix[y2*W2+x] = bottom;
    }
}
#endif

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
            printf("Player is now in sector %d\n", player.sector);
            break;
        }
    }

    player.where.x += dx;
    player.where.y += dy;
    player.angleSin = sinf(player.angle);
    player.angleCos = cosf(player.angle);
}

#if TextureMapping
static void vline2(int x, int y1, int y2, struct Scaler ty, unsigned txtx, const struct TextureSet* t)
{
    int *pix = (int*)surface->pixels;
    y1 = clamp(y1, 0, H-1);
    y2 = clamp(y2, 0, H-1);
    pix += y1 * W2 + x;

    for(int y = y1; y <= y2; ++y)
    {
        unsigned txty = Scaler_Next(&ty);
#if LightMapping
        *pix = ApplyLight(t->texture[txtx % 1024][txty % 1024], t->lightmap[txtx % 1024][txty % 1024]);
#else
        *pix = t->texture[txtx % 1024][txty % 1024];
#endif
        pix += W2;
    }
}
#endif

static void DrawScreen()
{
    struct item
    {
        short sectorno;
        short sx1;
        short sx2;
    };

    struct item queue[MaxQueue];
    struct item *head = queue;
    struct item *tail = queue;

    short ytop[W] = {0};
    short ybottom[W];
    short renderedSectors[NumSectors];

    for(unsigned x=0; x<W; ++x)
    {
        ybottom[x] = H-1;
    }

    for(unsigned n=0; n<NumSectors; ++n)
    {
        renderedSectors[n] = 0;
    }

#if VisibilityTracking
    for(unsigned n = 0; n < NumSectors; ++n)
    {
        sectors[n].visible = 0;
    }

    memset(VisibleFloors, 0, sizeof(VisibleFloors));
    memset(VisibleCeils, 0, sizeof(VisibleCeils));
    NumVisibleSectors = 0;
#endif

    *head = (struct item) { player.sector, 0, W-1 };

    if(++head == queue+MaxQueue)
    {
        head = queue;
    }

    SDL_LockSurface(surface);

    while(head != tail)
    {
        // pick a sector and slice from queue to draw
        const struct item now = *tail;

        if(++tail == queue+MaxQueue)
        {
            tail = queue;
        }

        if(renderedSectors[now.sectorno] & 0x21) continue; // Odd = still rendering, 0x20 = give up
        ++renderedSectors[now.sectorno];

#if VisibilityTracking
    sectors[now.sectorno].visible = 1;
#endif

        const struct sector* const sect = &sectors[now.sectorno];

#if LightMapping
        struct vec2d bounding_min = { 1e9f, 1e9f };
        struct vec2d bounding_max = { -1e9f, -1e9f };
        GetSectorBoundingBox(now.sectorno, &bounding_min, &bounding_max);
#endif

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

#if TextureMapping
            int u0 = 0, u1 = 1023;
#endif            

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

#if TextureMapping
                struct vec2d org1 = { tx1, tz1 };
                struct vec2d org2 = { tx2, tz2 };
#endif                

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

#if TextureMapping
                if(abs(tx2-tx1) > abs(tz2 - tz1))
                {
                    u0 = (tx1 - org1.x) * 1023 / (org2.x - org1.x);
                    u1 = (tx2 - org1.x) * 1023 / (org2.x - org1.x);
                }
                else
                {
                    u0 = (tz1 - org1.y) * 1023 / (org2.y - org1.y);
                    u1 = (tz2 - org1.y) * 1023 / (org2.y - org1.y);
                }
#endif
            }

            // Perspective transformation
            float xscale1 = (W*hfov) / tz1;
            float yscale1 = (H*vfov) / tz1;
            float xscale2 = (W*hfov) / tz2;
            float yscale2 = (H*vfov) / tz2;

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

#if DepthShading && !TextureMapping
            struct Scaler z_int = Scaler_Init(x1, beginx, x2, tz1*8,tz2*8);
#endif
            struct Scaler ya_int    = Scaler_Init(x1, beginx, x2, y1a, y2a);
            struct Scaler yb_int    = Scaler_Init(x1, beginx, x2, y1b, y2b);
            struct Scaler nya_int   = Scaler_Init(x1, beginx, x2, ny1a, ny2a);
            struct Scaler nyb_int   = Scaler_Init(x1, beginx, x2, ny1b, ny2b);
;
            for(int x = beginx; x <= endx; ++x)
            {
#if TextureMapping
                int txtx = (u0*((x2-x)*tz2) + u1*((x-x1)*tz1)) / ((x2-x)*tz2 + (x-x1)*tz1);
#endif                
#if DepthShading && !TextureMapping
                // Calculate the Z coordinate for this point (Only used for lighting)
                int z = Scaler_Next(&z_int);//((x - x1) * (tz2-tz1) / (x2-x1) + tz1) * 8;
#endif
                // Acquire the Y coordinates for our ceiling and floor for this X coordinate. Clamp them.
                int ya = Scaler_Next(&ya_int); //(x-x1) * (y2a - y1a) / (x2-x1) + y1a;
                int yb = Scaler_Next(&yb_int); //(x-x1) * (y2b - y1b) / (x2-x1) + y1b;
                int cya = clamp(ya, ytop[x], ybottom[x]); // top
                int cyb = clamp(yb, ytop[x], ybottom[x]); // bottom

                // Our perspective calculation produces these two:
                //     screenX = W/2 + -mapX              * (W*hfov) / mapZ
                //     screenY = H/2 + -(mapY + mapZ*yaw) * (H*vfov) / mapZ
                // To translate these coordinates back into mapX, mapY and mapZ...
                //
                // Solving for Z, when we know Y (ceiling height):
                //     screenY - H/2  = -(mapY + mapZ*yaw) * (H*vfov) / mapZ
                //     (screenY - H/2) / (H*vfov) = -(mapY + mapZ*yaw) / mapZ
                //     (H/2 - screenY) / (H*vfov) = (mapY + mapZ*yaw) / mapZ
                //     mapZ = mapY / ((H/2 - screenY) / (H*vfov) - yaw)
                //     mapZ = mapY*H*vfov / (H/2 - screenY - yaw*H*vfov)
                // Solving for X, when we know Z
                //     mapX = mapZ*(W/2 - screenX) / (W*hfov)
                //
                // This calculation is used for visibility tracking
                //   (the visibility cones in the map)
                // and for floor & ceiling texture mapping.
                //

                #define CeilingFloorScreenCoordinatesToMapCoordinates(mapY, screenX, screenY, X, Z) \
                    do { Z = (mapY)*H*vfov / ((H/2 - (screenY)) - player.yaw * H * vfov); \
                         X = (Z) * (W/2 - (screenX)) / (W*hfov); \
                         RelativeMapCoordinatesToAbsoluteOnes(X,Z); } while(0)

                #define RelativeMapCoordinatesToAbsoluteOnes(X,Z) \
                    do { float rtx = (Z) * pcos + (X) * psin; \
                         float rtz = (Z) * psin - (X) * pcos; \
                         X = rtx + player.where.x; Z = rtz + player.where.y; \
                       } while(0) 


#if TextureMapping
                for(int y = ytop[x]; y <= ybottom[x]; ++y)
                {
                    if(y >= cya && y <= cyb)
                    {
                        y = cyb;
                        continue;
                    }

                    float hei = y < cya ? yceil : yfloor;
                    float mapx, mapz;

                    CeilingFloorScreenCoordinatesToMapCoordinates(hei, x, y, mapx, mapz);
                    unsigned txtx = (mapx * 256);
                    unsigned txtz = (mapz * 256);
                    const struct TextureSet* txt = y < cya ? sect->ceiltexture : sect->floortexture;

#if LightMapping
                    unsigned lu = ((unsigned)((mapx - bounding_min.x) * 1024 / (bounding_max.x - bounding_min.x))) % 1024;
                    unsigned lv = ((unsigned)((mapz - bounding_min.y) * 1024 / (bounding_max.y - bounding_min.y))) % 1024;
                    int pel = ApplyLight(txt->texture[txtx % 1024][txtz % 1024], txt->lightmap[lu][lv]);
#else
                    int pel = txt->texture[txtz % 1024][txtx % 1024];
#endif
                    ((int*)surface->pixels)[y*W2+x] = pel;
                }
#else
                // Render ceiling: everything above this sector's ceiling height
                vline(x, ytop[x], cya-1, 0x111111, 0x222222, 0x111111);

                // Render floor: everything below this sector's floor height
                vline(x, cyb+1, ybottom[x], 0x0000FF, 0x0000AA, 0x0000FF);
#endif

#if VisibilityTracking
                {
                    unsigned n = NumVisibleSectors;
                    if(ybottom[x] >= (cyb+1))
                    {
                        float FloorXbegin, FloorZbegin, FloorXend, FloorZend;
                        CeilingFloorScreenCoordinatesToMapCoordinates(yfloor, x, cyb+1, FloorXbegin, FloorZbegin);
                        CeilingFloorScreenCoordinatesToMapCoordinates(yfloor, x, ybottom[x], FloorXend, FloorZend);
                        VisibleFloorBegins[n][x] = (struct vec2d){ FloorXbegin, FloorZbegin };
                        VisibleFloorEnds[n][x] = (struct vec2d) { FloorXend, FloorZend };
                        VisibleFloors[n][x] = 1;
                    }

                    if((cya-1) >= ytop[x])
                    {
                        float CeilXbegin, CeilZbegin, CeilXend, CeilZend;
                        CeilingFloorScreenCoordinatesToMapCoordinates(yceil, x, ytop[x], CeilXbegin, CeilZbegin);
                        CeilingFloorScreenCoordinatesToMapCoordinates(yceil, x, cya-1, CeilXend, CeilZend);
                        VisibleCeilBegins[n][x] = (struct vec2d){ CeilXbegin, CeilZbegin };
                        VisibleCeilEnds[n][x] = (struct vec2d) { CeilXend, CeilZend };
                        VisibleCeils[n][x] = 1;
                    }
                }
#endif
                // Is there another sector behind this edge?
                if(neighbor >= 0)
                {
                    int nya = Scaler_Next(&nya_int); //(x-x1) * (ny2a - ny1a) / (x2-x1) + ny1a;
                    int nyb = Scaler_Next(&nyb_int); //(x-x1) * (ny2b - ny1b) / (x2-x1) + ny1b;
                    int cnya = clamp(nya, ytop[x], ybottom[x]); // top
                    int cnyb = clamp(nyb, ytop[x], ybottom[x]); // bottom

                    // If our ceiling is higher than ther ceiling, render upper wall
#if TextureMapping
                    vline2(x, cya, cnya-1, (struct Scaler)Scaler_Init(ya,cya,yb,0,1023), txtx, &sect->uppertextures[s]);
#else
    #if DepthShading
                    unsigned r1 = 0x010101 * (255 - z);
                    unsigned r2 = 0x040007 * (31 - z/8);
    #else
                    unsigned r1 = 0xAAAAAA;
                    unsigned r2 = 0x7C00D9;                   
    #endif
                    vline(x, cya, cnya-1, 0, x==x1 || x == x2 ? 0 : r1, 0); //Between our and their ceiling
#endif
                    ytop[x] = clamp(max(cya, cnya), ytop[x], H-1); // Shrink the remaining window below these ceiling;

                    // If our floor is lower than ther floor, render bottom wall
#if TextureMapping
                    vline2(x, cnyb+1, cyb,  (struct Scaler)Scaler_Init(ya,cnyb+1,yb,0,1023), txtx, &sect->lowertextures[s]);
#else
                    vline(x, cnyb+1, cyb, 0, x == x1 || x == x2 ? 0 : r2, 0); // Between their and our floor
#endif
                    ybottom[x] = clamp(min(cyb, cnyb), 0, ybottom[x]); // Shrink the remaining window above these floor
                }
                else
                {
                    // NO NEIGHBOR!!!! Render wall from top to bottom
#if TextureMapping
                    vline2(x, cya, cyb, (struct Scaler)Scaler_Init(ya,cya,yb,0,1023), txtx, &sect->uppertextures[s]);
#else
    #if DepthShading
                    unsigned r = 0x010101 * (255-z);
    #else
                    unsigned r = 0xAAAAAA;
    #endif
                    vline(x, cya, cyb, 0, x==x1 || x == x2 ? 0 : r, 0);
#endif                    
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
#if VisibilityTracking
        NumVisibleSectors += 1;
#endif
    } 

    SDL_UnlockSurface(surface);
}

static SDL_Window *window = NULL;

int main(int argc, char** argv)
{
    LoadData();
    VerifyMap();
#if TextureMapping
    int textures_initialized = LoadTexture();
    #if LightMapping
        if(textures_initialized || (argc > 1 && strcmp(argv[1], "--rebuild") == 0))
            BuildLightmaps();
    #endif
#endif

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL failed to initialise: %s\n", SDL_GetError());
        return 1;
   }

    window = SDL_CreateWindow("SDL Doom", /* Title of the SDL window */
 			    SDL_WINDOWPOS_UNDEFINED, /* Position x of the window */
 			    SDL_WINDOWPOS_UNDEFINED, /* Position y of the window */
 			    W2, /* Width of the window in pixels */
 			    H, /* Height of the window in pixels */
 			    SDL_WINDOW_OPENGL); /* Additional flag(s) */

    if (window == NULL) {
        fprintf(stderr, "SDL window failed to initialise: %s\n", SDL_GetError());
        return 1;
    }

    surface = SDL_GetWindowSurface(window);

    SDL_ShowCursor(SDL_DISABLE);

    signal(SIGINT, SIG_DFL);

    FILE* fp = fopen("actions.log", "rb");

    int wasd[4] = { 0, 0, 0, 0 };
    int ground = 0;
    int falling = 1;
    int moving = 0;
    int ducking = 0;
    int map = 0;

    float yaw = 0;

    for (;;)
    {

        DrawScreen();
#if SplitScreen
        DrawMap();
#else
        if(map) 
            DrawMap();
#endif

        /*static unsigned skip = 0;
        if(++skip >= 1)
        {
            skip = 0;
            SDL_Flip(surface);
        }*/

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
                    case SDLK_TAB: map = ev.type == SDL_KEYDOWN; break;
                    default: break;
                }
                break;
            case SDL_QUIT:
                goto done;
            }

            // Mouse aiming
            {
                int x,y;
                SDL_GetRelativeMouseState(&x, &y);
                player.angle += x * 0.03f;
                yaw = clamp(yaw - y * 0.05f, -5, 5);
                player.yaw = yaw - player.velocity.z * 0.5f;
                MovePlayer(0,0);
            }

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

        //SDL_LockSurface(surface);
        //DrawScreen();
        //SDL_UnlockSurface(surface);
        SDL_UpdateWindowSurface(window);
		SDL_Delay(10);
    }

done:
    UnloadData();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}