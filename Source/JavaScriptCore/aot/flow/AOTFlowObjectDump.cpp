/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjects.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static void dumpValue(PrintStream& out, ValueID value)
{
    if (!value) {
        out.print("-"_s);
        return;
    }
    out.print("v"_s, value.index());
}

static void dumpSite(PrintStream& out, const Program& program, const FlowSite& site)
{
    if (site.function.index() >= program.functions.size())
        out.print("host"_s);
    else
        out.print("f"_s, site.function.index());
    out.print(":"_s);

    const FlowSiteDescription& description = site.description;
    switch (description.origin) {
    case FlowSiteOrigin::Operation:
    case FlowSiteOrigin::DataNode:
    case FlowSiteOrigin::BuiltinResult:
    case FlowSiteOrigin::BuiltinConstructor:
        dumpValue(out, description.value);
        break;
    case FlowSiteOrigin::BuiltinElement:
        dumpValue(out, description.value);
        out.print(".element"_s);
        break;
    case FlowSiteOrigin::TypedResult:
        dumpValue(out, description.value);
        out.print(".typed"_s);
        break;
    case FlowSiteOrigin::RuntimeError:
        dumpValue(out, description.value);
        out.print(".error"_s);
        break;
    case FlowSiteOrigin::CoroutineResult:
        out.print("result"_s);
        break;
    case FlowSiteOrigin::RestParameter:
        out.print("rest"_s);
        break;
    }

    if (description.ordinal)
        out.print("."_s, description.ordinal);
}

void FlowObjectTable::dump(PrintStream& out, FlowObjectID object) const
{
    const FlowObject& record = at(object);
    const BuiltinTable& builtins = m_program.builtins();
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        out.print("unknown"_s);
        break;
    case FlowObjectKind::Allocation:
        out.print("L"_s, record.layout.index());
        if (!record.standsForLayout)
            out.print("#"_s, record.allocationClass.index());
        break;
    case FlowObjectKind::Closure:
        out.print("f"_s, record.function.index());
        break;
    case FlowObjectKind::Prototype:
        dump(out, record.owner);
        out.print(".prototype"_s);
        return;
    case FlowObjectKind::Instance:
        out.print(builtins.tokenOf(record.builtinClass), "@"_s);
        dumpSite(out, m_program, at(record.site));
        break;
    case FlowObjectKind::Opaque:
        out.print("opaque"_s);
        if (record.builtinClass)
            out.print("("_s, builtins.tokenOf(record.builtinClass), ")"_s);
        out.print("@"_s);
        dumpSite(out, m_program, at(record.site));
        break;
    case FlowObjectKind::ModuleNamespace:
        out.print("namespace m"_s, record.module.index());
        break;
    case FlowObjectKind::Builtin:
        out.print(builtins.tokenOf(record.builtin));
        break;
    case FlowObjectKind::ModuleBuiltin:
        out.print(builtins.tokenOf(record.builtin), "@m"_s, record.module.index());
        break;
    }

    if (record.instance)
        out.print("@i"_s, record.instance.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
