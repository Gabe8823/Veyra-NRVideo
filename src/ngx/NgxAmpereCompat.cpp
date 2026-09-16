#include "veyra/ngx/NgxAmpereCompat.h"

#include <atomic>
#include <cstring>
#include <format>

#include "veyra/Log.h"
#include "veyra/ngx/NrArchitecturePolicy.h"

namespace veyra::ngx {

namespace {

// Independent implementation against the NVAPI public query ABI, mirroring the
// proven scoped NR route (DlssNrRuntimeAdapter.cpp). The hook lives only in the
// target runtime's import table and is restored before any caller code runs.
using QueryNvApi = void* (__cdecl*)(unsigned);
struct NvArchInfo { unsigned version, architecture, implementation, revision; };
using GetNvArch = int (__cdecl*)(void*, NvArchInfo*);
struct LogicalGpuData {
    unsigned version;
    void* osAdapterId;
    unsigned physicalGpuCount;
    void* physicalGpuHandles[64];
    unsigned reserved[8];
};
struct AmpereContext {
    HMODULE nvapi = nullptr;
    QueryNvApi query = nullptr;
    GetNvArch getArch = nullptr;
    decltype(&GetProcAddress) getProc = nullptr;
    void* selected[64]{};
    unsigned count = 0;
    void** iatSlot = nullptr;
    void* originalGetProcAddress = nullptr;
    std::atomic_uint64_t lookups{0}, queries{0}, rewrites{0};
};
AmpereContext g_ampere;
constexpr unsigned kArchQuery = 0xD8265D24u;

int __cdecl AmpereGetArch(void* gpu, NvArchInfo* info)
{
    g_ampere.queries.fetch_add(1, std::memory_order_relaxed);
    const int result = g_ampere.getArch(gpu, info);
    if (result != 0 || !info) return result;
    // Unknown handles are never treated as the selected adapter.
    bool selected = false;
    for (unsigned i = 0; i < g_ampere.count; ++i) selected |= gpu == g_ampere.selected[i];
    if (rewriteNrAmpereArchitecture(info->architecture, selected, result)) {
        g_ampere.rewrites.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
void* __cdecl AmpereQuery(unsigned id)
{
    if (id == kArchQuery) return reinterpret_cast<void*>(&AmpereGetArch);
    return g_ampere.query(id);
}
FARPROC WINAPI AmpereGetProc(HMODULE module, LPCSTR name)
{
    // GetProcAddress also accepts integer ordinals.
    if (module == g_ampere.nvapi && reinterpret_cast<uintptr_t>(name) > 0xFFFF &&
        std::strcmp(name, "nvapi_QueryInterface") == 0) {
        g_ampere.lookups.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&AmpereQuery);
    }
    return g_ampere.getProc(module, name);
}

// Walks the in-memory PE import table of `module` and returns the IAT slot
// (address of the thunk function pointer) importing `functionName` from a
// KERNEL32 or API-set DLL.
void** FindImportedFunctionSlot(HMODULE module, const char* functionName)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return nullptr;
    }

    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        const bool isKernel32 = _stricmp(dllName, "kernel32.dll") == 0;
        const bool isApiSet = _strnicmp(dllName, "api-ms-", 7) == 0 || _strnicmp(dllName, "ext-ms-", 7) == 0;
        if (!isKernel32 && !isApiSet) {
            continue;
        }

        const ULONGLONG lookupRva = descriptor->OriginalFirstThunk != 0 ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        if (lookupRva == 0) {
            continue;
        }
        const auto* lookup = reinterpret_cast<const ULONGLONG*>(base + lookupRva);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (size_t i = 0; lookup[i] != 0; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup[i])) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + static_cast<size_t>(lookup[i]));
            if (strcmp(reinterpret_cast<const char*>(byName->Name), functionName) == 0) {
                return reinterpret_cast<void**>(&iat[i].u1.Function);
            }
        }
    }
    return nullptr;
}

bool ExchangeImport(void** slot, void* replacement, void*& previous)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    previous = InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    if (VirtualProtect(slot, sizeof(void*), old, &ignored)) return true;
    // The page is still writable. Roll back before reporting failure.
    InterlockedExchangePointer(slot, previous);
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return false;
}

void ClearAmpereContext()
{
    if (g_ampere.nvapi) FreeLibrary(g_ampere.nvapi);
    g_ampere.nvapi = nullptr;
    g_ampere.query = nullptr;
    g_ampere.getArch = nullptr;
    g_ampere.getProc = nullptr;
    g_ampere.count = 0;
    g_ampere.iatSlot = nullptr;
    g_ampere.originalGetProcAddress = nullptr;
}

} // namespace

bool installNgxAmpereCompat(HMODULE runtimeModule, ID3D12Device* device,
                            const char* tag, Status& status)
{
    if (!runtimeModule || !device || g_ampere.iatSlot || g_ampere.nvapi) {
        status = Status::InvalidArgument;
        veyra::log::error(tag, "ampere-compat: invalid install state");
        return false;
    }
    auto fail = [&](const char* reason) {
        veyra::log::error(tag, reason);
        ClearAmpereContext();
        status = Status::DeviceFailure;
        return false;
    };
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return fail("system directory lookup failed");
    const std::wstring path = std::wstring(system) + L"\\nvapi64.dll";
    g_ampere.nvapi = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_ampere.nvapi) return fail("system NVAPI unavailable");
    g_ampere.query = reinterpret_cast<QueryNvApi>(GetProcAddress(g_ampere.nvapi, "nvapi_QueryInterface"));
    if (!g_ampere.query) return fail("NVAPI query entry missing");
    const auto init = reinterpret_cast<int(__cdecl*)()>(g_ampere.query(0x0150E828u));
    const auto enumerate = reinterpret_cast<int(__cdecl*)(void**, unsigned*)>(g_ampere.query(0x48B3EA59u));
    const auto getLogical = reinterpret_cast<int(__cdecl*)(void*, LogicalGpuData*)>(g_ampere.query(0x842B066Eu));
    g_ampere.getArch = reinterpret_cast<GetNvArch>(g_ampere.query(kArchQuery));
    if (!init || !enumerate || !getLogical || !g_ampere.getArch) return fail("required NVAPI adapter/architecture query missing");
    const int initResult = init();
    if (initResult != 0) {
        veyra::log::error(tag, std::format("NvAPI_Initialize={}", initResult));
        return fail("NVAPI initialization failed");
    }
    void* logical[64]{};
    unsigned count = 0;
    const int enumResult = enumerate(logical, &count);
    if (enumResult != 0 || count > 64) {
        veyra::log::error(tag, std::format("EnumLogicalGPUs={} count={}", enumResult, count));
        return fail("GPU enumeration failed");
    }
    const LUID target = device->GetAdapterLuid();
    for (unsigned i = 0; i < count; ++i) {
        LUID luid{};
        LogicalGpuData data{};
        data.version = sizeof(data) | (1u << 16);
        data.osAdapterId = &luid;
        const int result = getLogical(logical[i], &data);
        if (result != 0) {
            veyra::log::warn(tag, std::format("GetLogicalGpuInfo={}", result));
            continue;
        }
        if (luid.LowPart != target.LowPart || luid.HighPart != target.HighPart) continue;
        if (data.physicalGpuCount == 0 || data.physicalGpuCount > 64) return fail("invalid selected physical GPU count");
        g_ampere.count = data.physicalGpuCount;
        for (unsigned j = 0; j < data.physicalGpuCount; ++j) {
            g_ampere.selected[j] = data.physicalGpuHandles[j];
            NvArchInfo arch{sizeof(NvArchInfo) | (1u << 16), 0, 0, 0};
            const int resultArch = g_ampere.getArch(g_ampere.selected[j], &arch);
            veyra::log::info(tag, std::format("selected LUID={:X}:{:X} archResult={} architecture=0x{:X} rewrite={}",
                target.HighPart, target.LowPart, resultArch, arch.architecture,
                (arch.architecture & 0xFFFFFFF0u) == 0x170u));
            if (resultArch != 0) return fail("selected adapter architecture unavailable");
            const unsigned family = arch.architecture & 0xFFFFFFF0u;
            if (family != 0x170u && family != 0x190u && family < 0x1A0u) return fail("this experimental route requires RTX30 or newer; RTX20 is unverified");
        }
        break;
    }
    if (!g_ampere.count) return fail("cannot match NVAPI adapter to selected D3D12 LUID");
    auto slot = FindImportedFunctionSlot(runtimeModule, "GetProcAddress");
    if (!slot) return fail("runtime GetProcAddress import missing; refusing wider driver hooks");
    g_ampere.getProc = reinterpret_cast<decltype(&GetProcAddress)>(*slot);
    g_ampere.lookups = 0;
    g_ampere.queries = 0;
    g_ampere.rewrites = 0;
    if (!ExchangeImport(slot, reinterpret_cast<void*>(&AmpereGetProc), g_ampere.originalGetProcAddress)) {
        return fail("runtime import install failed and rolled back");
    }
    g_ampere.iatSlot = slot;
    veyra::log::info(tag, "scoped runtime NVAPI lookup installed; system entry point unchanged");
    return true;
}

void restoreNgxAmpereCompat(const char* tag)
{
    if (!g_ampere.iatSlot) return;
    void* previous = nullptr;
    const bool restored = ExchangeImport(g_ampere.iatSlot, g_ampere.originalGetProcAddress, previous);
    veyra::log::info(tag, std::format("restore={} lookups={} queries={} rewrites={}",
        restored, g_ampere.lookups.load(), g_ampere.queries.load(), g_ampere.rewrites.load()));
    ClearAmpereContext();
}

bool ngxAmpereCompatInstalled()
{
    return g_ampere.iatSlot != nullptr;
}

} // namespace veyra::ngx
