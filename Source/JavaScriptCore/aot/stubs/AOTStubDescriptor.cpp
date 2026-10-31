/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubDescriptor.h"

#if ENABLE(AOT)

#include <wtf/PrintStream.h>

namespace JSC { namespace AOT {

void dump(PrintStream& out, StubKey key)
{
    if (!key.hasFamily()) {
        out.print("<no stub>"_s);
        return;
    }

    const StubDescriptor& descriptor = descriptorOf(key.family());
    out.print(descriptor.name);
    if (key.form() < descriptor.formNames.size())
        out.print("."_s, descriptor.formNames[key.form()]);
    else if (key.form())
        out.print(".<no form "_s, key.form(), ">"_s);
    if (descriptor.axes.contains(StubAxis::Count))
        out.print(".n"_s, key.count());

    switch (key.callerFrame()) {
    case CallerFrame::HasFrame:
        break;
    case CallerFrame::Frameless:
        out.print(".Frameless"_s);
        break;
    case CallerFrame::StubFrame:
        out.print(".StubFrame"_s);
        break;
    case CallerFrame::KeepsNoRegisters:
        out.print(".KeepsNoRegisters"_s);
        break;
    case CallerFrame::KeepsNoFPRs:
        out.print(".KeepsNoFPRs"_s);
        break;
    }

    if (std::optional<GPRReg> reg = key.operandRegister())
        out.print("("_s, Reg(*reg), ")"_s);
    if (std::optional<uint32_t> immediate = key.immediate())
        out.print("#"_s, *immediate);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
