/* Fixed-tic demo replay: compare complete indexed frames and player state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WR_REPLAY_ENGINE
#define WR_REPLAY_ENGINE "../engine.c"
#endif
#include WR_REPLAY_ENGINE

static int replay_step;
void wr_print(const char *s) { fputs(s, stderr); }
void *wr_malloc(int n) { return malloc(n); }
void wr_free(void *p) { free(p); }
void *wr_open(const char *n, const char *m) { return fopen(n, m); }
void wr_close(void *p) { fclose(p); }
int wr_read(void *p, void *b, int n) { return (int)fread(b, 1, n, p); }
int wr_write(void *p, const void *b, int n) { return (int)fwrite(b, 1, n, p); }
int wr_seek(void *p, int n, int o) { return fseek(p, n, o); }
int wr_tell(void *p) { return (int)ftell(p); }
int wr_eof(void *p) { return feof(p); }
char *wr_getenv(const char *n) { (void)n; return "."; }
void wr_gettime(int *s, int *u)
{
    *s = replay_step / 35;
    *u = (int)((long long)(replay_step % 35) * 1000000 / 35);
}
void wr_exit(int code) { exit(code ? code : 2); }

static unsigned hash(const unsigned char *p, int n)
{
    unsigned value = 2166136261u;
    for (int i = 0; i < n; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}

int main(void)
{
    char *args[] = {"doom", "-playdemo", "demo1"};
    wr_engine_init(3, args);
    /* Test clock: render every recorded tic, independent of host speed. */
    singletics = true;
    for (replay_step = 1; replay_step <= 1500; ++replay_step) {
        wr_engine_step();
        player_t *p = &players[consoleplayer];
        printf("%d %d %08x %08x %d %d %u %d %d\n", replay_step, gametic,
               hash(wr_engine_frame(), 320*200), hash(wr_engine_palette(), 768),
               p->mo ? p->mo->x : 0, p->mo ? p->mo->y : 0,
               p->mo ? p->mo->angle : 0, p->health, p->readyweapon);
    }
    return 0;
}
