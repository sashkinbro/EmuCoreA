// Actual PPSSPP registry HLE dispatch and bounded registry-section serialization.
#include "Common/Log/LogManager.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeMap.h"
#include "Core/Config.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceReg.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr u32 NAME = 0x08801000;
constexpr u32 OUTPUT = 0x08801200;
constexpr u32 VALUE = 0x08801300;
constexpr u32 OPEN_REGISTRY = 0x92E41280;
constexpr u32 OPEN_CATEGORY = 0x1D8A762E;
constexpr u32 CREATE_KEY = 0x57641A81;
constexpr u32 SET_VALUE = 0x17768E14;
constexpr u32 GET_INFO = 0xD4475AA8;
constexpr u32 GET_VALUE = 0x28A8E98A;
constexpr u32 GET_BY_NAME = 0x30BE0259;

u32 Call(u32 nid, std::initializer_list<u32> args) {
    const HLEFunction *function = GetHLEFunc("sceReg", nid);
    if (!function || !function->func) {
        std::fprintf(stderr, "Missing real registry HLE NID %08x\n", nid);
        return ~0U;
    }
    int index = MIPS_REG_A0;
    for (u32 arg : args) currentMIPS->r[index++] = arg;
    function->func();
    return currentMIPS->r[MIPS_REG_V0];
}

u32 Name(const char *name) {
    Memory::MemcpyUnchecked(NAME, name, (u32)std::strlen(name) + 1);
    return NAME;
}

bool Check(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int Open() {
    __RegInit();
    if (Call(OPEN_REGISTRY, {0, 1, OUTPUT}) != 0 ||
        Call(OPEN_CATEGORY, {0, Name("/DATA/FONT/PROPERTY/INFO0"), 1, OUTPUT}) != 0)
        return -1;
    return (int)Memory::ReadUnchecked_U32(OUTPUT);
}

bool Create(int category, const char *name, u32 type = 2) {
    return Check(Call(CREATE_KEY, {(u32)category, Name(name), type, 4}) == 0, "create key");
}

bool Write(int category, const char *name, const void *bytes, u32 size) {
    Memory::MemcpyUnchecked(VALUE, bytes, size);
    return Check(Call(SET_VALUE, {(u32)category, Name(name), VALUE, size}) == 0, "write value");
}

bool WriteInt(int category, const char *name, u32 value) {
    return Write(category, name, &value, sizeof(value));
}

u32 Handle(int category, const char *name) {
    if (Call(GET_INFO, {(u32)category, Name(name), OUTPUT, OUTPUT + 4, OUTPUT + 8}) != 0)
        return ~0U;
    return Memory::ReadUnchecked_U32(OUTPUT);
}

bool ReadInt(int category, const char *name, u32 expected) {
    Memory::WriteUnchecked_U32(0xDEADBEEF, VALUE);
    return Check(Call(GET_BY_NAME, {(u32)category, Name(name), VALUE, 4}) == 0 &&
        Memory::ReadUnchecked_U32(VALUE) == expected, "read restored integer by name");
}

bool ReadHandle(int category, u32 handle, u32 expected) {
    Memory::WriteUnchecked_U32(0xDEADBEEF, VALUE);
    return Check(Call(GET_VALUE, {(u32)category, handle, VALUE, 4}) == 0 &&
        Memory::ReadUnchecked_U32(VALUE) == expected, "previous key handle still refers to its original key");
}

bool ReadBytes(int category, const char *name, const void *expected, u32 size) {
    std::memset(Memory::GetPointerWriteUnchecked(VALUE), 0xA5, size);
    return Check(Call(GET_BY_NAME, {(u32)category, Name(name), VALUE, size}) == 0 &&
        std::memcmp(Memory::GetPointerUnchecked(VALUE), expected, size) == 0, "read restored string/binary bytes");
}

struct RegistryState {
    void DoState(PointerWrap &p) { __RegDoState(p); }
};

// Versions 1-3 contained only registry/category metadata, in this exact layout.
struct LegacyCategory {
    std::string path;
    int mode = 1;
    void DoState(PointerWrap &p) { Do(p, path); Do(p, mode); }
};
struct LegacyState {
    int version;
    void DoState(PointerWrap &p) {
        auto section = p.Section("sceReg", 0, version);
        int mode = 1, count = 1, nextHandle = 1338;
        std::map<int, LegacyCategory> categories;
        categories[1337].path = "/DATA/FONT/PROPERTY/INFO0";
        Do(p, mode);
        Do(p, categories);
        if (version >= 2) Do(p, count);
        if (version >= 3) Do(p, nextHandle);
    }
};

template <typename T>
std::vector<u8> Save(T &state) {
    std::vector<u8> bytes;
    if (CChunkFileReader::MeasureAndSavePtr(state, &bytes) != CChunkFileReader::ERROR_NONE)
        bytes.clear();
    return bytes;
}

bool Load(std::vector<u8> &bytes) {
    RegistryState state;
    std::string error;
    return Check(!bytes.empty() && CChunkFileReader::LoadPtr(bytes.data(), bytes.size(), state, &error) ==
        CChunkFileReader::ERROR_NONE, "bounded registry state loads successfully");
}

bool StableHandles() {
    const int category = Open();
    if (!Check(category >= 0, "open category") || !Create(category, "registry_z") ||
        !WriteInt(category, "registry_z", 0x12345678)) return false;
    const u32 handle = Handle(category, "registry_z");
    if (!Check(handle != ~0U, "get key handle") || !Create(category, "registry_a") ||
        !WriteInt(category, "registry_a", 0x87654321)) return false;
    // A later alphabetically earlier key must never retarget the existing handle.
    return ReadHandle(category, handle, 0x12345678);
}

bool OverlayState(bool clearBeforeLoad) {
    const int category = Open();
    const char text[] = "saved registry string";
    const u8 binary[] = {0x11, 0, 0x22, 0xFF, 0, 0x33};
    if (category < 0 || !Create(category, "registry_z") || !WriteInt(category, "registry_z", 42) ||
        !Create(category, "registry_text", 3) || !Write(category, "registry_text", text, sizeof(text)) ||
        !Create(category, "registry_binary", 4) || !Write(category, "registry_binary", binary, sizeof(binary)) ||
        !WriteInt(category, "h_size", 777)) return false;
    const u32 handle = Handle(category, "registry_z");
    RegistryState state;
    auto snapshot = Save(state);
    if (!Check(!snapshot.empty(), "save registry state") ||
        !WriteInt(category, "registry_z", 99) || !WriteInt(category, "h_size", 999) ||
        !Write(category, "registry_text", "later", 6) || !Write(category, "registry_binary", "later", 6) ||
        !Create(category, "registry_future")) return false;
    // Fresh-session load has no overlay. Same-session load has future writes and cached views.
    if (clearBeforeLoad) { __RegShutdown(); __RegInit(); }
    if (!Load(snapshot)) return false;
    bool ok = ReadInt(category, "registry_z", 42);
    ok = ReadInt(category, "h_size", 777) && ok;
    ok = ReadBytes(category, "registry_text", text, sizeof(text)) && ok;
    ok = ReadBytes(category, "registry_binary", binary, sizeof(binary)) && ok;
    ok = ReadHandle(category, handle, 42) && ok;
    ok = Check(Call(GET_INFO, {(u32)category, Name("registry_future"), OUTPUT, OUTPUT + 4, OUTPUT + 8}) != 0,
        "load discards keys created after the snapshot") && ok;
    return ok;
}

bool LegacyStates() {
    bool ok = true;
    for (int version = 1; version <= 3; ++version) {
        const int category = Open();
        if (category < 0 || !Create(category, "registry_future") || !WriteInt(category, "h_size", 999)) return false;
        LegacyState legacy{version};
        auto snapshot = Save(legacy);
        if (!Load(snapshot)) return false;
        ok = ReadInt(1337, "h_size", 0x288) && ok;
        ok = Check(Call(GET_INFO, {1337, Name("registry_future"), OUTPUT, OUTPUT + 4, OUTPUT + 8}) != 0,
            "legacy load restores static registry instead of retaining future overlay") && ok;
    }
    return ok;
}
}  // namespace

int main() {
    g_logManager.SetAllLogEnable(false);
    g_Config.Init();
    Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;
    if (!Memory::Init(Memory::MemMapSetupFlags::Default)) {
        std::fputs("Unable to map actual PSP memory\n", stderr);
        return 2;
    }
    Register_sceReg();
    int failures = 0;
    const auto run = [&](const char *name, bool result) {
        std::printf("registry %s: %s\n", name, result ? "PASS" : "FAIL");
        if (!result) ++failures;
    };
    run("stable handles", StableHandles());
    run("same-session rollback", OverlayState(false));
    run("fresh-session restoration", OverlayState(true));
    run("legacy state versions 1-3", LegacyStates());
    __RegShutdown();
    HLEShutdown();
    Memory::Shutdown();
    return failures ? 1 : 0;
}
