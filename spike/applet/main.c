/* SPIKE stage 0 (throwaway): a minimal program that takes the place of the "Game Notes" system applet
 * (USA title 0004003000009302, AppID 0x113) through /luma/titles/<id>/{exheader.bin,code.bin}.
 * It answers APT like an applet, shows a line on the top screen and exits on A.
 * Every step's result is appended to sdmc:/3ds/nintendo-dev-agent/applet-trace.log so a failure can be read
 * afterwards even when nothing is visible. */
#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TRACE_PATH "sdmc:/3ds/nintendo-dev-agent/applet-trace.log"

extern u32 __apt_appid;

/* small fixed heaps: the applet lives in the SYSTEM memory region and must not ask for "everything left" */
u32 __ctru_heap_size = 0x80000;
u32 __ctru_linear_heap_size = 0x80000;

static bool g_sd = false;

static void trace(const char *fmt, ...)
{
    if (!g_sd) return;
    FILE *f = fopen(TRACE_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void __appInit(void)
{
    Result r;
    srvInit();
    fsInit();
    r = archiveMountSdmc();
    g_sd = R_SUCCEEDED(r);
    trace("---- applet start");
    trace("archiveMountSdmc = 0x%08lX", (unsigned long)r);
    __apt_appid = 0x113; /* Game Notes */
    r = aptInit();
    trace("aptInit(appid 0x%03lX) = 0x%08lX", (unsigned long)__apt_appid, (unsigned long)r);
    r = hidInit();
    trace("hidInit = 0x%08lX", (unsigned long)r);
}

void __appExit(void)
{
    trace("appExit");
    hidExit();
    aptExit();
    archiveUnmountAll();
    fsExit();
    srvExit();
}

int main(void)
{
    trace("main");
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("Nintendo Dev Agent - applet test\n\n");
    printf("Running as the Game Notes applet\n(AppID 0x113). The game is suspended.\n\n");
    printf("Press A to exit.\n");
    trace("screen ready");

    u32 frames = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_A) {
            trace("A pressed after %lu frames", (unsigned long)frames);
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        frames++;
    }
    trace("leaving main (aptMainLoop returned or A)");
    gfxExit();
    return 0;
}
