// Exercises the actual HLE handler and kernel shutdown in the linked PPSSPP core.
#include "Common/Log/LogManager.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernelModule.h"
#include "Core/MIPS/MIPS.h"

#include <cstdio>

void Register_SystemCtrlForKernel();

static u32 SetHandler(u32 address) {
    const int index = GetHLEModuleIndex("SystemCtrlForKernel");
    const HLEModule *module = GetHLEModuleByIndex(index);
    for (int i = 0; i < module->numFunctions; ++i) {
        if (module->funcTable[i].ID == 0x1C90BECB) {
            currentMIPS->r[MIPS_REG_A0] = address;
            module->funcTable[i].func();
            return currentMIPS->r[MIPS_REG_V0];
        }
    }
    std::fputs("Missing start-module handler HLE function\n", stderr);
    return ~0U;
}

int main() {
    g_logManager.SetAllLogEnable(false);
    Register_SystemCtrlForKernel();
    if (SetHandler(0x08804000) != 0 || SetHandler(0x08805000) != 0x08804000) {
        std::fputs("Handler registration did not return the previous guest address\n", stderr);
        return 1;
    }
    __KernelModuleShutdown();
    HLEShutdown();
    Register_SystemCtrlForKernel();
    const u32 previous = SetHandler(0);
    if (previous != 0) {
        std::fprintf(stderr, "Kernel shutdown leaked guest handler %08x into the next session\n", previous);
        return 2;
    }
    // Repeated shutdowns must remain safe without an active guest.
    __KernelModuleShutdown();
    __KernelModuleShutdown();
    HLEShutdown();
    std::puts("PPSSPP start-module handler session isolation: PASS");
    return 0;
}
