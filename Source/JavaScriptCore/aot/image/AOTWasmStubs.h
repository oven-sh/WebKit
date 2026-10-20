/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTRelocation.h"
#include <expected>
#include <wtf/Vector.h>

namespace JSC {

namespace Wasm {
class RTT;
struct ModuleInformation;
}

namespace AOT {

class BuildContext;

using WasmStubCode = std::expected<Vector<uint8_t>, LinkFailure>;

WasmStubCode generateWasmThrowStub(BuildContext&);
WasmStubCode generateWasmImportStub(const Wasm::ModuleInformation&, unsigned importIndex, BuildContext&);
WasmStubCode generateWasmJSEntrypoint(const Wasm::RTT&, uint32_t calleeSlot, BuildContext&);
WasmStubCode generateWasmInterpreterStub(uint32_t calleeSlot, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)
