// Throwaway spike: does dswifi's SERVER side (listen/accept) survive a real session on this
// DSi, at the same time as libfat SD access, in DSi-enhanced mode? Not part of the product.
//
// Does NOT use WFC_CONNECT (the console's system-saved Wi-Fi settings): on real hardware that
// was empty (never configured for online play) and Wifi_InitDefault failed outright. Scans and
// asks for a password on-screen instead -- the same reason KirovAir/TwilightBoxart (a real,
// published DSi homebrew doing SD+wifi together) has its own network picker rather than relying
// on firmware settings. Saved to sd:/ so it is only typed once.
//
// Wi-Fi bring-up pattern (WIFI_ATTEMPT_DSI_MODE, cothread_yield_irq while waiting, never
// swiWaitForVBlank -- that starves the lwIP cothread and DHCP hangs forever) and the sd:/
// prefix are lifted from BlocksDS's own examples/dswifi/connection_info and
// examples/filesystem/all_filesystems -- not guessed.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <fat.h>
#include <nds.h>
#include <dswifi9.h>

#define PORT 6464
#define WIFI_CFG_PATH "sd:/nsd-bridge-wifi.cfg"
#define SCAN_SLOTS 32

static PrintConsole g_top;

static void die_press_start(void)
{
    printf("\nPress START to exit");
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysHeld() & KEY_START)
            break;
    }
}

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

/* ---- saved network (typed once, reused after) ---- */

struct saved_net
{
    char ssid[33];
    char key[64];
};

static bool load_saved_net(struct saved_net *out)
{
    FILE *f = fopen(WIFI_CFG_PATH, "rb");
    if (!f)
        return false;
    memset(out, 0, sizeof(*out));
    bool ok = fgets(out->ssid, sizeof(out->ssid), f) != NULL &&
              fgets(out->key, sizeof(out->key), f) != NULL;
    fclose(f);
    if (!ok)
        return false;
    out->ssid[strcspn(out->ssid, "\r\n")] = '\0';
    out->key[strcspn(out->key, "\r\n")] = '\0';
    return out->ssid[0] != '\0';
}

static void save_net(const char *ssid, const char *key)
{
    FILE *f = fopen(WIFI_CFG_PATH, "wb");
    if (!f)
        return; // not fatal: just means typing it again next time
    fprintf(f, "%s\n%s\n", ssid, key);
    fclose(f);
}

/* ---- on-screen keyboard, echoed (masked) to the top screen ---- */

static void read_line(const char *prompt, char *buf, size_t buf_size, bool mask)
{
    printf("%s\n", prompt);
    size_t len = 0;
    buf[0] = '\0';

    keyboardDemoInit();
    keyboardShow();

    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        int key = keyboardUpdate();

        if (key == DVK_ENTER)
            break;
        if (key == DVK_BACKSPACE)
        {
            if (len > 0)
            {
                len--;
                printf("\b \b");
            }
            continue;
        }
        if (key > 0 && key < 128 && len < buf_size - 1)
        {
            buf[len++] = (char)key;
            buf[len] = '\0';
            printf(mask ? "*" : "%c", key);
        }
    }
    printf("\n");

    keyboardHide();
}

/* ---- scan + pick (simple button menu, no keyboard needed) ---- */

static int scan_and_pick(Wifi_AccessPoint *chosen)
{
    printf("Scanning...\n");
    Wifi_ScanMode();
    for (int i = 0; i < 2 * 60; i++) // ~2 s
        cothread_yield_irq(IRQ_VBLANK);

    static Wifi_AccessPoint list[SCAN_SLOTS];
    int total = Wifi_GetNumAP();
    int n = 0;
    for (int i = 0; i < total && n < SCAN_SLOTS; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid_len == 0)
            continue;
        list[n++] = ap;
    }

    if (n == 0)
    {
        printf("No networks found.\n");
        return -1;
    }

    int sel = 0;
    while (1)
    {
        consoleClear();
        printf("NSD Bridge -- pick a network\n");
        printf("(UP/DOWN, A to choose)\n\n");
        for (int i = 0; i < n; i++)
        {
            char ssid[33];
            int l = list[i].ssid_len < 32 ? list[i].ssid_len : 32;
            memcpy(ssid, list[i].ssid, l);
            ssid[l] = '\0';
            printf("%s%2d. %-24s %s\n", i == sel ? "> " : "  ", i, ssid,
                   (list[i].flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA)) ? "[locked]" : "[open]");
        }

        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        int down = keysDown();
        if (down & KEY_UP)
            sel = (sel - 1 + n) % n;
        if (down & KEY_DOWN)
            sel = (sel + 1) % n;
        if (down & KEY_A)
        {
            *chosen = list[sel];
            consoleClear();
            return 0;
        }
    }
}

/* ---- join + wait (the DHCP-hang lesson: cothread_yield_irq, never swiWaitForVBlank) ---- */

static bool wait_for_association(int seconds)
{
    int deadline = seconds * 60;
    for (int frame = 0; frame < deadline; frame++)
    {
        cothread_yield_irq(IRQ_VBLANK);
        int status = Wifi_AssocStatus();
        if (frame % 60 == 0)
            printf(".");
        if (status == ASSOCSTATUS_ASSOCIATED)
            return true;
        if (status == ASSOCSTATUS_CANNOTCONNECT)
            return false;
    }
    return false;
}

static bool connect_wifi(void)
{
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE))
    {
        printf("Wifi_InitDefault FAILED\n");
        return false;
    }

    struct saved_net saved;
    char key[64] = { 0 };
    Wifi_AccessPoint ap;
    bool have_ap = false;

    if (load_saved_net(&saved))
    {
        printf("Trying saved network:\n%s\n", saved.ssid);
        Wifi_ScanMode();
        for (int i = 0; i < 2 * 60; i++)
            cothread_yield_irq(IRQ_VBLANK);
        int total = Wifi_GetNumAP();
        for (int i = 0; i < total; i++)
        {
            Wifi_AccessPoint a;
            if (Wifi_GetAPData(i, &a) != WIFI_RETURN_OK)
                continue;
            if (a.ssid_len == strlen(saved.ssid) && memcmp(a.ssid, saved.ssid, a.ssid_len) == 0)
            {
                ap = a;
                have_ap = true;
                strcpy(key, saved.key);
                break;
            }
        }
        if (!have_ap)
            printf("(not in range right now)\n");
    }

    if (!have_ap)
    {
        if (scan_and_pick(&ap) != 0)
            return false;
        if (ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA))
            read_line("Password:", key, sizeof(key), true);
    }

    printf("Connecting");
    int rc = (ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA))
                 ? Wifi_ConnectSecureAP(&ap, key, strlen(key))
                 : Wifi_ConnectOpenAP(&ap);
    if (rc != 0)
    {
        printf("\nWifi_Connect...AP() FAILED (%d)\n", rc);
        return false;
    }

    if (!wait_for_association(20))
    {
        printf("\nAssociation FAILED/timed out\n");
        return false;
    }

    printf("\nConnected. DSi mode: %d\n", isDSiMode());
    struct in_addr gw = { 0 }, mask = { 0 }, dns1 = { 0 }, dns2 = { 0 };
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    printf("IP: %s\n", inet_ntoa(ip));

    if (!have_ap || strlen(key) > 0)
    {
        char ssid[33] = { 0 };
        int l = ap.ssid_len < 32 ? ap.ssid_len : 32;
        memcpy(ssid, ap.ssid, l);
        save_net(ssid, key);
    }

    return true;
}

/* ---- the actual thing being tested: a TCP server, alongside all of the above ---- */

static int wait_and_accept(int listen_fd)
{
    for (;;)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysHeld() & KEY_START)
            return -2;

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd >= 0)
        {
            printf("client: %s\n", inet_ntoa(peer.sin_addr));
            return fd;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            printf("accept() error %d\n", errno);
            return -1;
        }
    }
}

static void echo_session(int fd)
{
    char buf[256];
    int n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        printf("recv %d bytes\n", n);
        if (send(fd, buf, n, 0) != n)
        {
            printf("send() short/failed\n");
            break;
        }
    }
    printf(n == 0 ? "client closed\n" : "recv() error %d\n", n == 0 ? 0 : errno);
    closesocket(fd);
}

int main(int argc, char *argv[])
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&g_top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleSelect(&g_top);

    printf("NSD Bridge -- DSi spike\n");
    printf("wifi + SD, at the same time\n\n");

    bool sd_ok = sd_smoke_test();
    bool net_ok = connect_wifi();

    if (!net_ok)
    {
        printf("\nSD %s, WiFi FAILED\n", sd_ok ? "OK" : "FAILED");
        die_press_start();
        return 0;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0)
    {
        printf("bind/listen FAILED (%d)\n", errno);
        die_press_start();
        return 0;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    printf("\nSD %s. Listening on\nport %d.\n", sd_ok ? "OK" : "FAILED", PORT);
    printf("(START to exit)\n\n");

    for (;;)
    {
        int fd = wait_and_accept(listen_fd);
        if (fd == -2)
            break;
        if (fd < 0)
            continue;
        echo_session(fd);
    }

    closesocket(listen_fd);
    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    return 0;
}
