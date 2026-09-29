// Isolation test: identical console+SD code to dsi-wifi-sd, but built with the STANDARD ARM7
// (no dswifi). If this boots fine via TWiLight Menu++/nds-bootstrap where dsi-wifi-sd went to a
// white screen, the custom dswifi ARM7 binary is the culprit.

#include <stdio.h>
#include <string.h>

#include <fat.h>
#include <nds.h>

static PrintConsole g_top, g_bot;

static bool sd_smoke_test(void)
{
    if (!fatInitDefault())
    {
        printf("fatInitDefault() FAILED\n");
        return false;
    }

    const char *path = "sd:/nsd-bridge-spike-test.txt";
    const char *content = "NSD Bridge DSi spike was here\n";

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        printf("SD write open FAILED\n");
        return false;
    }
    size_t written = fwrite(content, 1, strlen(content), f);
    fclose(f);
    if (written != strlen(content))
    {
        printf("SD write FAILED (%u bytes)\n", (unsigned)written);
        return false;
    }

    f = fopen(path, "rb");
    if (!f)
    {
        printf("SD read open FAILED\n");
        return false;
    }
    char buf[64] = { 0 };
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (got != strlen(content) || strcmp(buf, content) != 0)
    {
        printf("SD readback MISMATCH\n");
        return false;
    }

    printf("SD write+read: OK\n");
    return true;
}

int main(int argc, char *argv[])
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&g_top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&g_bot, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    consoleSelect(&g_top);

    printf("NSD Bridge -- DSi spike\n");
    printf("(no wifi this time)\n\n");
    printf("DSi mode: %d\n", isDSiMode());

    sd_smoke_test();

    printf("\nPress START to exit");
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysHeld() & KEY_START)
            break;
    }

    return 0;
}
