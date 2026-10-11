// Runs two independent copies of the shipped Android core, without test JNI.
// All networking calls originate in an ELF guest and use the real PSP imports.
#include "Common/CPUDetect.h"
#include "Common/Log/LogManager.h"
#include "Common/Thread/ThreadManager.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/Instance.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr uint32_t ENTRY = 0x08804000, RECORD = 0x08806000;
constexpr uint32_t MAC = RECORD + 0x100, PRODUCT = RECORD + 0x110;
constexpr uint32_t GROUP = RECORD + 0x130, STATE = RECORD + 0x140;
constexpr uint32_t SIZE = RECORD + 0x144, SCAN = RECORD + 0x180;
constexpr uint32_t PEERS = RECORD + 0x280, PEER_MAC = PEERS + 132;
constexpr uint32_t REMOTE_MAC = RECORD + 0x380, REMOTE_PORT = RECORD + 0x388;
constexpr uint32_t TX = RECORD + 0x400, RX = RECORD + 0x440;
constexpr uint32_t DONE = 100, FAILED = 0xdead;

// PSP EABI passes all eight word arguments in a0..a3,t0..t3 (HLE.h PARAM),
// unlike the four-register O32 calling convention used by many MIPS targets.
struct Guest {
    std::vector<uint32_t> code;
    std::map<std::string, size_t> labels;
    struct Fixup { size_t at; std::string label; bool absolute; };
    std::vector<Fixup> fixups;
    std::map<std::string, uint32_t> calls;
    void emit(uint32_t op) { code.push_back(op); }
    void li(int r, uint32_t n) {
        emit(0x3c000000 | r << 16 | n >> 16);
        emit(0x34000000 | r << 21 | r << 16 | (n & 0xffff));
    }
    void move(int d, int s) { emit(s << 21 | d << 11 | 0x21); }
    void lw(int r, int base, int off) { emit(0x8c000000 | base << 21 | r << 16 | (off & 0xffff)); }
    void sw(int r, int base, int off) { emit(0xac000000 | base << 21 | r << 16 | (off & 0xffff)); }
    void store(uint32_t address, uint32_t value) { li(8, address); li(9, value); sw(9, 8, 0); }
    void label(const std::string &name) { labels[name] = code.size(); }
    void branch(uint32_t opcode, int a, int b, const std::string &label) {
        fixups.push_back({code.size(), label, false}); emit(opcode | a << 21 | b << 16); emit(0);
    }
    void jump(const std::string &label) {
        fixups.push_back({code.size(), label, true}); emit(0x08000000); emit(0);
    }
    void call(const std::string &name) { emit(0x0c000000 | ((calls.at(name) >> 2) & 0x03ffffff)); emit(0); }
    void args(const std::vector<uint32_t> &values) {
        for (size_t i = 0; i < values.size(); i++) {
            li(4 + (int)i, values[i]);
        }
    }
    void call(const std::string &name, std::initializer_list<uint32_t> values) { args(values); call(name); }
    void check() { sw(2, 16, 4); branch(0x04000000, 2, 0, "fail"); } // bltz v0
    void expect(int reg, uint32_t value) { li(8, value); branch(0x14000000, reg, 8, "fail"); }
    void stage(int n) { li(8, n); sw(8, 16, 0); }
    void pause() { call("delay", {10000}); }
    void checkPeerMac() {
        li(8, REMOTE_MAC); li(9, PEER_MAC); lw(10, 8, 0); lw(11, 9, 0);
        branch(0x14000000, 10, 11, "fail");
        emit(0x950a0004); emit(0x952b0004); // lhu t2,4(t0); lhu t3,4(t1)
        branch(0x14000000, 10, 11, "fail");
    }
    void payload(uint32_t address, uint32_t magic, int round) {
        store(address, magic); store(address + 4, round);
        store(address + 8, 0x11223344); store(address + 12, 0xaabbccdd);
    }
    void validate(uint32_t magic, int round) {
        li(9, RX); lw(10, 9, 0); expect(10, magic);
        lw(10, 9, 4); expect(10, round);
        lw(10, 9, 8); expect(10, 0x11223344);
        lw(10, 9, 12); expect(10, 0xaabbccdd);
    }
    void stream(const std::string &fn, uint32_t buffer, const std::string &labelName) {
        li(20, 0); label(labelName);
        li(8, 16); emit(8 << 21 | 20 << 16 | 21 << 11 | 0x23); // remaining =16-offset
        li(8, SIZE); sw(21, 8, 0);
        move(4, 19); li(5, buffer); emit(5 << 21 | 20 << 16 | 5 << 11 | 0x21);
        li(6, SIZE); li(7, 1000000); li(8, 0);
        call(fn); check(); li(8, SIZE); lw(21, 8, 0);
        branch(0x10000000, 21, 0, "fail");
        emit(20 << 21 | 21 << 16 | 20 << 11 | 0x21);
        li(8, 16); branch(0x14000000, 20, 8, labelName);
    }
    void finish() {
        stage(DONE); jump("park"); label("fail"); lw(8, 16, 0); sw(8, 16, 8); stage(FAILED);
        label("park"); pause(); jump("park");
        for (const auto &f : fixups) {
            const size_t target = labels.at(f.label);
            if (f.absolute) code[f.at] |= ((ENTRY + (uint32_t)target * 4) >> 2) & 0x03ffffff;
            else code[f.at] |= (uint16_t)((int)target - (int)f.at - 1);
        }
    }
};

std::vector<uint8_t> MakeElf(bool server, int round) {
    constexpr size_t LOAD = 0x100, MOD = 0x1900, IMPORTS = 0x1940;
    constexpr size_t NAMES = 0x1a00, NIDS = 0x1b00, STUBS = 0x1c00;
    constexpr size_t STRINGS = 0x3100, SECTIONS = 0x3200;
    std::vector<uint8_t> bytes(SECTIONS + 4 * 40);
    auto word = [&](size_t off, uint32_t n) { for (int i = 0; i < 4; i++) bytes.at(off + i) = n >> (i * 8); };
    auto half = [&](size_t off, uint16_t n) { bytes.at(off) = n; bytes.at(off + 1) = n >> 8; };
    auto address = [&](size_t off) { return ENTRY + (uint32_t)off - LOAD; };
    bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F'; bytes[4] = 1; bytes[5] = 1; bytes[6] = 1;
    half(16, 2); half(18, 8); word(20, 1); word(24, ENTRY); word(28, 52); word(32, SECTIONS);
    half(40, 52); half(42, 32); half(44, 1); half(46, 40); half(48, 4); half(50, 3);
    word(52, 1); word(56, LOAD); word(60, ENTRY); word(64, MOD);
    word(68, 0x3000); word(72, 0x4000); word(76, 7); word(80, 0x100);
    half(MOD, 0); half(MOD + 2, 0x0100); std::memcpy(&bytes[MOD + 4], "AdhocTransport", 14);
    word(MOD + 44, address(IMPORTS)); word(MOD + 48, address(IMPORTS + 5 * 20));
    Guest g;
    size_t name = NAMES, nid = NIDS, stub = STUBS, import = IMPORTS;
    auto module = [&](const char *library, std::initializer_list<std::pair<const char *, uint32_t>> funcs) {
        std::memcpy(&bytes[name], library, std::strlen(library) + 1);
        word(import, address(name)); half(import + 4, 0x0100); half(import + 6, 0x4001);
        bytes[import + 8] = 5; half(import + 10, (uint16_t)funcs.size());
        word(import + 12, address(nid)); word(import + 16, address(stub));
        for (const auto &f : funcs) { g.calls[f.first] = address(stub); word(nid, f.second); word(stub, 0x03e00008); nid += 4; stub += 8; }
        import += 20; name += std::strlen(library) + 1;
    };
    module("sceNet", {{"netInit", 0x39af39a6}});
    module("sceWlanDrv", {{"mac", 0x0c622081}});
    module("sceNetAdhoc", {{"adhocInit", 0xe1d621d7}, {"pdpCreate", 0x6f92741b}, {"pdpSend", 0xabed3790},
        {"pdpRecv", 0xdfe53e03}, {"ptpListen", 0xe08bdac1}, {"ptpAccept", 0x9df81198},
        {"ptpOpen", 0x877f6d66}, {"ptpConnect", 0xfc6fc07b}, {"ptpSend", 0x4da4c788}, {"ptpRecv", 0x8bea2b3e}});
    module("sceNetAdhocctl", {{"ctlInit", 0xe26f226e}, {"create", 0xec0635c1}, {"scan", 0x08fff7a0},
        {"state", 0x75ecd386}, {"scanInfo", 0x81aee1be}, {"join", 0x5e7f79c9}, {"peers", 0xe162cb14}});
    module("ThreadManForUser", {{"delay", 0xceadeb47}});
    if (stub >= 0x2000) throw std::runtime_error("import stubs overlap result record");
    std::memcpy(&bytes[LOAD + PRODUCT - ENTRY + 4], "EMUTEST01", 9);
    std::memcpy(&bytes[LOAD + GROUP - ENTRY], "EMUTEST1", 8);
    // Keep an aligned guest stack; imported HLE arguments use eight registers.
    g.emit(0x27bdffe0); g.li(16, RECORD); g.stage(1);
    g.call("netInit", {0x20000, 0x20, 0x1000, 0x20, 0x1000}); g.check();
    g.call("mac", {MAC}); g.check(); g.call("adhocInit", {}); g.check();
    g.call("ctlInit", {0x2000, 0x30, PRODUCT}); g.check(); g.stage(2);
    if (server) { g.call("create", {GROUP}); g.check(); }
    else {
        g.label("scanRetry"); g.call("scan", {}); g.check();
        g.label("scanWait"); g.pause(); g.call("state", {STATE}); g.check();
        g.li(8, STATE); g.lw(9, 8, 0); g.branch(0x14000000, 9, 0, "scanWait");
        g.store(SIZE, 256); g.call("scanInfo", {SIZE, SCAN}); g.check();
        g.li(8, SIZE); g.lw(9, 8, 0); g.branch(0x10000000, 9, 0, "scanRetry");
        g.expect(9, 28); // one exact SceNetAdhocctlScanInfoEmu
        g.li(9, SCAN + 8); g.lw(10, 9, 0); g.expect(10, 0x54554d45); // EMUT
        g.lw(10, 9, 4); g.expect(10, 0x31545345); // EST1
        g.call("join", {SCAN}); g.check();
    }
    g.stage(3);
    g.call("pdpCreate", {MAC, 30000, 4096, 0}); g.check(); g.move(17, 2);
    if (server) { g.call("ptpListen", {MAC, 30001, 4096, 1000000, 5, 1, 0}); g.check(); g.move(18, 2); }
    g.label("peerWait"); g.pause(); g.store(SIZE, 152); g.call("peers", {SIZE, PEERS}); g.check();
    g.li(8, SIZE); g.lw(9, 8, 0); g.branch(0x10000000, 9, 0, "peerWait"); g.expect(9, 152);
    g.stage(4);
    if (!server) {
        g.payload(TX, 0x55445051, round);
        g.move(4, 17); g.li(5, PEER_MAC); g.li(6, 30000); g.li(7, TX);
        g.li(8, 16); g.li(9, 1000000); g.li(10, 0);
        g.call("pdpSend"); g.check();
    }
    g.label("udpRecv"); g.pause(); g.store(SIZE, 16);
    g.move(4, 17); g.li(5, REMOTE_MAC); g.li(6, REMOTE_PORT); g.li(7, RX);
    g.li(8, SIZE); g.li(9, 0); g.li(10, 1);
    g.call("pdpRecv"); g.sw(2, 16, 4); g.branch(0x04000000, 2, 0, "udpRecv");
    g.li(8, SIZE); g.lw(9, 8, 0); g.expect(9, 16); g.checkPeerMac();
    g.li(8, REMOTE_PORT); g.emit(0x95090000); g.expect(9, 30000);
    g.validate(server ? 0x55445051 : 0x55445052, round); g.stage(5);
    if (server) {
        g.payload(TX, 0x55445052, round);
        g.move(4, 17); g.li(5, PEER_MAC); g.li(6, 30000); g.li(7, TX);
        g.li(8, 16); g.li(9, 1000000); g.li(10, 0);
        g.call("pdpSend"); g.check();
        g.label("acceptWait"); g.pause(); g.move(4, 18); g.li(5, REMOTE_MAC); g.li(6, REMOTE_PORT); g.li(7, 0);
        g.li(8, 1); g.call("ptpAccept"); g.sw(2, 16, 4);
        g.branch(0x04000000, 2, 0, "acceptWait"); g.move(19, 2); g.checkPeerMac();
        g.li(8, REMOTE_PORT); g.emit(0x95090000); g.expect(9, 30002);
    } else {
        g.call("ptpOpen", {MAC, 30002, PEER_MAC, 30001, 4096, 1000000, 5, 0}); g.check(); g.move(19, 2);
        g.label("connectWait"); g.pause(); g.move(4, 19); g.li(5, 1000000); g.li(6, 1); g.call("ptpConnect");
        g.sw(2, 16, 4); g.branch(0x04000000, 2, 0, "connectWait");
    }
    g.stage(6);
    if (!server) { g.payload(TX, 0x54435051, round); g.stream("ptpSend", TX, "tcpSendRequest"); }
    g.stream("ptpRecv", RX, "tcpRecv"); g.validate(server ? 0x54435051 : 0x54435052, round);
    if (server) { g.payload(TX, 0x54435052, round); g.stream("ptpSend", TX, "tcpSendReply"); }
    g.finish();
    if (LOAD + g.code.size() * 4 >= MOD) throw std::runtime_error("guest code overlaps module info");
    for (size_t i = 0; i < g.code.size(); i++) word(LOAD + i * 4, g.code[i]);
    const char names[] = "\0.text\0.rodata.sceModuleInfo\0.shstrtab\0";
    std::memcpy(&bytes[STRINGS], names, sizeof(names));
    auto section = [&](int i, int nameIndex, int flags, uint32_t addr, int off, int size) {
        size_t h = SECTIONS + i * 40; word(h, nameIndex); word(h + 4, 1); word(h + 8, flags);
        word(h + 12, addr); word(h + 16, off); word(h + 20, size); word(h + 32, 4);
    };
    section(1, 1, 6, ENTRY, LOAD, (int)g.code.size() * 4);
    section(2, 7, 2, address(MOD), MOD, 52);
    section(3, 29, 0, 0, STRINGS, sizeof(names)); word(SECTIONS + 3 * 40 + 4, 3);
    return bytes;
}

bool Exists(const std::string &path) { struct stat st{}; return stat(path.c_str(), &st) == 0; }
void Mark(const std::string &path) { std::ofstream(path) << "ready\n"; }
bool Wait(const std::string &path) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(50);
    while (!Exists(path) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return Exists(path);
}
}

int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || (std::strcmp(argv[1], "server") && std::strcmp(argv[1], "client"))) {
        std::fprintf(stderr, "Usage: %s server|client shared-work-directory [port-offset=10000]\n", argv[0]); return 2;
    }
    const bool server = !std::strcmp(argv[1], "server");
    const std::string root = argv[2], role = argv[1];
    const int offset = argc == 4 ? std::atoi(argv[3]) : 10000;
    if (offset != 0 && offset != 10000) return 2;
    std::ofstream(root + "/" + role + ".pid") << getpid() << '\n';
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_logManager.Init(&g_Config.bEnableLogging, false);
    g_logManager.SetAllLogEnable(true); g_logManager.SetAllLogLevels(LogLevel::LWARNING);
    g_logManager.SetOutputsEnabled(LogOutput::Stdio);
    g_Config.Init(); g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS, false);
    PPSSPP_ID = server ? 1 : 2;
    g_Config.bEnableWlan = true; g_Config.bEnableAdhocServer = server;
    g_Config.sProAdhocServer = "127.0.0.1";
    g_Config.iAdhocServerRelayMode = (int)AdhocServerRelayMode::AlwaysOff;
    g_Config.sMACAddress = "00:11:22:33:44:55"; g_Config.sNickName = role;
    g_Config.iPortOffset = offset; g_Config.bEnableUPnP = false; g_Config.bForcedFirstConnect = false;
    g_Config.iMinTimeout = 0; g_Config.bEnableSound = false; g_Config.bEnableCheats = false;
    g_Config.bAutoSaveSymbolMap = false; g_Config.sReportHost.clear();
    g_Config.memStickDirectory = Path(root); g_Config.internalDataDirectory = Path(root);
    g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);
    std::printf("%s pid=%d loopback=127.0.0.%d portOffset=%d\n", role.c_str(), getpid(), (int)PPSSPP_ID, offset);
    int status = 0;
    for (int round = 1; round <= 2 && !status; round++) {
        const std::string prefix = root + "/round" + std::to_string(round);
        if (!server && !Wait(prefix + ".ready")) { status = 3; break; }
        const auto elf = MakeElf(server, round);
        const std::string filename = root + "/" + role + ".elf";
        std::ofstream file(filename, std::ios::binary); file.write((const char *)elf.data(), elf.size()); file.close();
        CoreParameter p{}; p.cpuCore = CPUCore::INTERPRETER; p.gpuCore = GPUCORE_SOFTWARE;
        p.fileToStart = Path(filename); p.headLess = true; p.enableSound = false; p.loadGameConfigs = false;
        p.renderWidth = p.pixelWidth = 480; p.renderHeight = p.pixelHeight = 272;
        std::string error;
        if (PSP_Init(p, &error) != BootState::Complete) {
            std::fprintf(stderr, "%s boot failed: %s\n", role.c_str(), error.c_str()); status = 4; break;
        }
        coreState = CORE_RUNNING_CPU;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        uint32_t stage = 0, previous = ~0U;
        bool markedReady = false;
        while (std::chrono::steady_clock::now() < deadline) {
            PSP_RunLoopFor(2220000); // at most ~10ms guest time; real network worker needs wall time
            if (coreState == CORE_NEXTFRAME) coreState = CORE_RUNNING_CPU;
            if (coreState != CORE_RUNNING_CPU) { status = 5; break; }
            stage = Memory::ReadUnchecked_U32(RECORD);
            if (stage != previous) {
                std::printf("%s round=%d stage=%u lastHle=%08x\n", role.c_str(), round, stage, Memory::ReadUnchecked_U32(RECORD + 4));
                previous = stage;
            }
            if (server && stage >= 3 && stage < FAILED && !markedReady) { Mark(prefix + ".ready"); markedReady = true; }
            if (stage == DONE || stage == FAILED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const uint32_t lastHle = Memory::ReadUnchecked_U32(RECORD + 4);
        if (stage != DONE || status) {
            std::fprintf(stderr, "%s round=%d FAIL stage=%u failedPhase=%u lastHle=%08x pc=%08x coreState=%d\n",
                role.c_str(), round, stage, Memory::ReadUnchecked_U32(RECORD + 8), lastHle, currentMIPS->pc, (int)coreState); status = 6;
        } else {
            std::printf("%s round=%d discovery/PDP/PTP nonce+MAC+port PASS\n", role.c_str(), round);
            Mark(prefix + "." + role + ".done");
            // Keep the PRO server alive until the client has consumed its TCP reply.
            if (server && !Wait(prefix + ".client.done")) status = 7;
        }
        PSP_Shutdown(true);
        if (server) Mark(prefix + ".stopped");
        else if (!status && !Wait(prefix + ".stopped")) status = 8;
    }
    g_threadManager.Teardown(); g_logManager.Shutdown();
    Mark(root + "/" + role + ".exit-" + std::to_string(status));
    return status;
}
