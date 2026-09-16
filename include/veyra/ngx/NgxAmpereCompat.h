#pragma once

#include <windows.h>
#include <d3d12.h>

#include "veyra/Result.h"

namespace veyra::ngx {

// Opt-in RTX 30 (Ampere) capability rewrite for a locally loaded, unmodified
// NVIDIA NGX runtime. Scoped exactly like the NR adapter's experimental route:
// only the target module's own GetProcAddress import of
// nvapi64!nvapi_QueryInterface is intercepted; the driver entry point, other
// modules and the OS stay untouched. A successful architecture query for the
// selected Ampere adapter (0x170 family) is rewritten to Ada (0x1B0)
// (NrArchitecturePolicy). Fork extension following the dlssg_for_sm86 route;
// experimental, local-only, fail-closed.
bool installNgxAmpereCompat(HMODULE runtimeModule, ID3D12Device* device,
                            const char* tag, Status& status);
void restoreNgxAmpereCompat(const char* tag);
bool ngxAmpereCompatInstalled();

} // namespace veyra::ngx
