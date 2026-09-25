/* SPIKE (throwaway): can a Luma3DS plugin (.3gx) running INSIDE a game open a TCP connection?
 * It steals a client session to "soc:U" (Luma's svcControlService), initializes it with its own memory block,
 * connects to NDEV_SPIKE_HOST:NDEV_SPIKE_PORT (this computer) and sends one line per step.
 * Every step's result code is also shown on the game screen (OSD notifications).
 * Nothing is read or written on the SD card. */
#include "3ds.h"
#include "csvc.h"
#include "CTRPluginFramework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef NDEV_SPIKE_HOST
#error "define NDEV_SPIKE_HOST as four comma-separated bytes, e.g. -DNDEV_SPIKE_HOST=192,168,0,10"
#endif
#ifndef NDEV_SPIKE_PORT
#define NDEV_SPIKE_PORT 6465
#endif

namespace CTRPluginFramework
{
    static u8 g_socBuf[0x20000] __attribute__((aligned(0x1000)));
    static Handle g_soc = 0, g_mem = 0;
    static std::string g_report;

    static inline u32 CurProcessId(void) { return 0x20; } /* IPC "process id" translate descriptor */

    static void Step(const char *name, s32 value)
    {
        char line[96];
        snprintf(line, sizeof line, "%s = 0x%08lX", name, (unsigned long)value);
        g_report += line;
        g_report += "\n";
        OSD::Notify(line, value < 0 ? Color::Red : Color::Lime);
    }

    /* soc:U commands, formats taken from libctru (source/services/soc/*.c) */
    static s32 SocInitialize(void)
    {
        u32 *c = getThreadCommandBuffer();
        c[0] = IPC_MakeHeader(0x1, 1, 4);
        c[1] = sizeof g_socBuf;
        c[2] = CurProcessId();
        c[4] = IPC_Desc_SharedHandles(1);
        c[5] = g_mem;
        s32 r = svcSendSyncRequest(g_soc);
        return r != 0 ? r : (s32)c[1];
    }

    static s32 SocSocket(s32 *fd)
    {
        u32 *c = getThreadCommandBuffer();
        c[0] = IPC_MakeHeader(0x2, 3, 2);
        c[1] = 2; /* AF_INET */
        c[2] = 1; /* SOCK_STREAM */
        c[3] = 0;
        c[4] = CurProcessId();
        s32 r = svcSendSyncRequest(g_soc);
        if (r != 0) return r;
        if ((s32)c[1] != 0) return (s32)c[1];
        *fd = (s32)c[2];
        return 0;
    }

    static s32 SocConnect(s32 fd, const u8 ip[4], u16 port)
    {
        u8 addr[8] = {8, 2, (u8)(port >> 8), (u8)port, ip[0], ip[1], ip[2], ip[3]};
        u32 *c = getThreadCommandBuffer();
        c[0] = IPC_MakeHeader(0x6, 2, 4);
        c[1] = (u32)fd;
        c[2] = 8;
        c[3] = CurProcessId();
        c[5] = IPC_Desc_StaticBuffer(8, 0);
        c[6] = (u32)addr;
        s32 r = svcSendSyncRequest(g_soc);
        if (r != 0) return r;
        if ((s32)c[1] != 0) return (s32)c[1];
        return (s32)c[2]; /* the network result (negative = error) */
    }

    static s32 SocSend(s32 fd, const void *buf, u32 len)
    {
        u8 none[8] = {0};
        u32 *c = getThreadCommandBuffer();
        c[0] = IPC_MakeHeader(0x9, 4, 6);
        c[1] = (u32)fd;
        c[2] = len;
        c[3] = 0;
        c[4] = 0;
        c[5] = CurProcessId();
        c[7] = IPC_Desc_StaticBuffer(0, 1);
        c[8] = (u32)none;
        c[9] = IPC_Desc_Buffer(len, IPC_BUFFER_R);
        c[10] = (u32)buf;
        s32 r = svcSendSyncRequest(g_soc);
        if (r != 0) return r;
        if ((s32)c[1] != 0) return (s32)c[1];
        return (s32)c[2]; /* bytes sent, or negative */
    }

    static void SocClose(s32 fd)
    {
        u32 *c = getThreadCommandBuffer();
        c[0] = IPC_MakeHeader(0xB, 1, 2);
        c[1] = (u32)fd;
        c[2] = CurProcessId();
        svcSendSyncRequest(g_soc);
    }

    static void RunNetTest(void)
    {
        static bool running = false;
        if (running) return;
        running = true;
        const u8 ip[4] = {NDEV_SPIKE_HOST};
        g_report.clear();

        char title[64];
        snprintf(title, sizeof title, "ndev spike in title %016llX", (unsigned long long)Process::GetTitleID());
        g_report += title;
        g_report += "\n";
        OSD::Notify(title);

        s32 r;
        if (g_soc == 0)
        {
            r = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &g_soc, "soc:U");
            Step("1 steal soc:U session", r);
            if (r != 0) { g_soc = 0; running = false; return; }
            r = svcCreateMemoryBlock(&g_mem, (u32)g_socBuf, sizeof g_socBuf, (MemPerm)0, (MemPerm)3);
            Step("2 create memory block", r);
            if (r != 0) { running = false; return; }
            r = SocInitialize();
            Step("3 soc:U Initialize", r);
            if (r != 0) { running = false; return; }
        }
        s32 fd = -1;
        r = SocSocket(&fd);
        Step("4 socket()", r);
        if (r == 0)
        {
            r = SocConnect(fd, ip, NDEV_SPIKE_PORT);
            Step("5 connect()", r);
            if (r == 0)
            {
                g_report += "(connected)\n";
                s32 sent = SocSend(fd, g_report.data(), g_report.size());
                Step("6 send()", sent);
            }
            SocClose(fd);
        }
        running = false;
    }

    static void NetTestMenu(MenuEntry *entry)
    {
        (void)entry;
        RunNetTest();
    }

    void PatchProcess(FwkSettings &settings) { (void)settings; }
    void OnProcessExit(void) {}

    int main(void)
    {
        PluginMenu *menu = new PluginMenu("ndev spike", 0, 0, 1, "Network test for the Nintendo Dev MCP.");
        menu->SynchronizeWithFrame(true);
        menu->Append(new MenuEntry("Network test (soc:U)", nullptr, NetTestMenu, "Connects to the computer and sends the step results."));
        RunNetTest();
        menu->Run();
        delete menu;
        return 0;
    }
}
