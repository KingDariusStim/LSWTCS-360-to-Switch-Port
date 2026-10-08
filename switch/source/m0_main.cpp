// M0 (compile check) entry point: proves that an NRO holding all recompiled game code links and
// loads. It does not run guest code; it prints the process budget and the size of the function
// table to sdmc:/lswtcs/trace.log. Replaced by the real runtime entry in M2.
#include <switch.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdint>
#include "ppc_config.h"
#include "ppc_context.h"

static FILE* g_log;
#define LOG(...) do { printf(__VA_ARGS__); if (g_log) { fprintf(g_log, __VA_ARGS__); fflush(g_log); } consoleUpdate(NULL); } while (0)

static uint64_t info_u64(uint32_t id) { uint64_t v = 0; svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0); return v; }

int main(int, char**) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    mkdir("sdmc:/lswtcs", 0777);
    g_log = fopen("sdmc:/lswtcs/trace.log", "w");
    LOG("hello from LSWTCS (M0 compile check)\n");
    LOG("  TotalMemory %lu MB, UsedMemory %lu MB\n",
        (unsigned long)(info_u64(InfoType_TotalMemorySize) >> 20), (unsigned long)(info_u64(InfoType_UsedMemorySize) >> 20));
    // Count the function table so the linker keeps every recompiled function (gc-sections).
    size_t n = 0, nonnull = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host || n == 0; ++m, ++n) if (m->host) ++nonnull;
    LOG("  PPCFuncMappings: %zu entries (%zu with code); first host fn %p\n", n, nonnull, (void*)PPCFuncMappings[1].host);
    LOG("DONE. Press + to exit (auto-exit in 10 s).\n");
    if (g_log) fclose(g_log);
    g_log = nullptr;
    uint64_t t0 = armGetSystemTick();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (armTicksToNs(armGetSystemTick() - t0) > 10000000000ull) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
