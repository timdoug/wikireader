/* WikiReader bridge to PureDOOM, GPL-3.0-or-later. */
#include "wr_doom.h"
#include "c33_math.h"
#include "render_cache.h"
#include "profile.h"
#ifdef WR_C33
#define DOOM_ERROR_MESSAGE(text) wr_error(text)
static void benchmark_defaults(void);
#define DOOM_PROFILE_DEFAULTS() benchmark_defaults()
#define DOOM_PROFILE_BOOT(name) wr_profile_boot(name)
#define DOOM_PROFILE_BEGIN(p) do { if (wr_profile_enabled) wr_profile_phase_begin(p); } while (0)
#define DOOM_PROFILE_END(p) do { if (wr_profile_enabled) wr_profile_phase_end(p); } while (0)
#endif
#define DOOM_FIXED_MUL(a, b) wr_fixed_mul(a, b)
#define DOOM_RECIPROCAL(d) wr_reciprocal(d)
#ifdef WR_C33
#define WR_FAST_DATA __attribute__((section(".fastbss")))
#else
#define WR_FAST_DATA
#endif
/* COLORMAP is immutable after initialization. Separate entries keep floor
   and wall lighting from evicting each other. */
static wr_light_cache column_light WR_FAST_DATA, span_light WR_FAST_DATA;
#define DOOM_COLUMN_COLORMAP(p) wr_light_map(&column_light, p)
#define DOOM_SPAN_COLORMAP(p) wr_light_map(&span_light, p)

#ifdef WR_C33
static unsigned flat_pixels[1024] __attribute__((section(".texture_cache")));
static void copy_flat(unsigned *dest, const unsigned *source)
    __attribute__((section(".fastcode"), noinline));
static int cache_large_flat(int width, const unsigned char *top, const unsigned char *bottom)
    __attribute__((section(".fastcode"), noinline));
#else
static unsigned flat_pixels[1024];
#endif
static int flat_lump = -1;

static void copy_flat(unsigned *dest, const unsigned *source)
{
    /* Zone-allocated WAD data and our cache are word aligned. */
    for (unsigned i = 0; i < 1024; ++i) dest[i] = source[i];
}

static int cache_large_flat(int width, const unsigned char *top, const unsigned char *bottom)
{
    unsigned pixels = 0;
    for (int x = 0; x < width; ++x) {
        if (top[x] != 255 && top[x] <= bottom[x]) {
            pixels += bottom[x] - top[x] + 1;
            /* Copying 4 KiB is wasteful for a few short spans. Stop counting
               once this plane covers enough texels to justify admission. */
            if (pixels >= 2048) return 1;
        }
    }
    return 0;
}

static unsigned char *cache_flat(int lump, const unsigned char *source, int width,
                                 const unsigned char *top, const unsigned char *bottom)
{
    /* WAD lump identities are stable; zone allocation addresses may be
       reused between levels. Animated flats select a different lump. */
    if (flat_lump != lump) {
        int (*volatile large)(int, const unsigned char *, const unsigned char *) = cache_large_flat;
        if (!large(width, top, bottom)) return (unsigned char *)source;
        void (*volatile copy)(unsigned *, const unsigned *) = copy_flat;
        copy(flat_pixels, (const unsigned *)source);
        flat_lump = lump;
    }
    return (unsigned char *)flat_pixels;
}
#define DOOM_FLAT_TEXTURE(lump, source, plane) \
    cache_flat(lump, source, (plane)->maxx - (plane)->minx + 1, \
               (plane)->top + (plane)->minx, (plane)->bottom + (plane)->minx)
#ifdef WR_C33
/* Calls use the renderer's function pointers, which can reach internal RAM. */
void R_DrawColumnLow(void) __attribute__((section(".fastcode"), noinline));
void R_DrawSpanLow(void) __attribute__((section(".fastcode"), noinline));
void R_MapPlane(int y, int x1, int x2) __attribute__((section(".fastcode"), noinline));
void R_MakeSpans(int x, int t1, int b1, int t2, int b2)
    __attribute__((section(".fastcode"), noinline));
void R_RenderSegLoop(void) __attribute__((section(".fastcode"), noinline));
unsigned FixedDivUnsigned(unsigned a, unsigned b) __attribute__((section(".fastcode"), noinline));
#endif
#define DOOM_NO_SOUND
#define DOOM_IMPLEMENTATION
#include "vendor/PureDOOM.h"

#ifdef WR_C33
static void benchmark_defaults(void)
{
    if (wr_profile_locked()) { detailLevel = 1; screenblocks = 10; }
}
#endif

static int seek_file(void *handle, int offset, doom_seek_t origin)
{
    return wr_seek(handle, offset, origin);
}

void wr_engine_init(int argc, char **argv)
{
#ifdef WR_C33
    /* Isolate benchmark configuration from the player's saved settings. */
    static char *bench_args[] = { "doom.app", "-warp", "1", "1", "-skill", "2",
                                 "-config", "/doom/bench.cfg" };
    if (wr_profile_locked()) {
        argc = sizeof(bench_args) / sizeof(bench_args[0]); argv = bench_args;
    }
#endif
    column_light.source = span_light.source = 0;
    flat_lump = -1;
    doom_set_print(wr_print);
    doom_set_malloc(wr_malloc, wr_free);
    doom_set_file_io(wr_open, wr_close, wr_read, wr_write,
                     seek_file, wr_tell, wr_eof);
    doom_set_gettime(wr_gettime);
    doom_set_getenv(wr_getenv);
    doom_set_exit(wr_exit);
    doom_set_default_int("screenblocks", 10);
    doom_set_default_int("detaillevel", 1);
    doom_set_default_int("sfx_volume", 0);
    doom_set_default_int("music_volume", 0);
    doom_init(argc, argv, DOOM_FLAG_HIDE_MOUSE_OPTIONS |
              DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS);
    singletics = false;
}

int wr_engine_step(void)
{
    int ticked = !is_wiping_screen;
    doom_force_update();
    return ticked;
}
void wr_engine_key(int key, int down)
{
    static unsigned char mapped[256];
    int original = key;
    if (!down) {
        key = mapped[key] ? mapped[key] : key;
        mapped[original] = 0;
        doom_key_up((doom_key_t)key);
        return;
    }
    if (key == WR_ENTER && messageToPrint && messageNeedsInput) key = 'y';
    /* Supply a save name on devices without a text keyboard. */
    if (key == WR_ENTER && down && saveStringEnter && !savegamestrings[saveSlot][0])
        doom_strcpy(savegamestrings[saveSlot], "WIKIREADER");
    mapped[original] = (unsigned char)key;
    doom_key_down((doom_key_t)key);
}
int wr_engine_menu(void) { return menuactive || gamestate == GS_DEMOSCREEN || demoplayback; }
void wr_engine_weapon(void)
{
    if (menuactive || gamestate != GS_LEVEL || demoplayback) return;
    player_t *p = &players[consoleplayer];
    int next = p->readyweapon;
    do { next = (next + 1) % NUMWEAPONS; } while (!p->weaponowned[next]);
    p->pendingweapon = (weapontype_t)next;
}
void wr_engine_quit(void) { I_Quit(); }
const unsigned char *wr_engine_frame(void) { return screens[0]; }
const unsigned char *wr_engine_palette(void) { return screen_palette; }

void wr_engine_profile(wr_profile_state *s)
{
    player_t *p = &players[consoleplayer];
    s->state = gamestate; s->menu = menuactive; s->demo = demoplayback;
    s->wipe = is_wiping_screen; s->episode = gameepisode; s->map = gamemap;
    s->skill = gameskill + 1; s->detail = detailshift;
    s->width = viewwidth; s->height = viewheight;
    s->gametic = gametic; s->leveltime = leveltime;
    s->x = p->mo ? p->mo->x : 0; s->y = p->mo ? p->mo->y : 0;
    s->angle = p->mo ? p->mo->angle : 0; s->health = p->health;
}
