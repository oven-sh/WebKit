/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include <span>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {


void dumpID(PrintStream&, ValueID);
void dumpID(PrintStream&, BlockID);
void dumpID(PrintStream&, ObjectID);
void dumpPoint(PrintStream&, ProgramPoint);
void dumpEdge(PrintStream&, Edge);
void dumpUse(PrintStream&, const Use&);
void dumpUseLists(PrintStream&, const Function&, const UseLists&);

template<typename T>
void dumpList(PrintStream& out, std::span<const T> items, NOESCAPE const Invocable<void(PrintStream&, const T&)> auto& dumpItem)
{
    out.print("[");
    CommaPrinter separator(" "_s);
    for (const T& item : items) {
        out.print(separator);
        dumpItem(out, item);
    }
    out.print("]");
}

inline void dumpItem(PrintStream& out, ASCIILiteral text) { out.print(text); }
inline void dumpItem(PrintStream& out, uint32_t number) { out.print(number); }
inline void dumpItem(PrintStream& out, bool answer) { out.print(answer ? "yes" : "no"); }
inline void dumpItem(PrintStream& out, ValueID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, BlockID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, ObjectID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, Edge edge) { dumpEdge(out, edge); }
inline void dumpItem(PrintStream& out, ProgramPoint point) { dumpPoint(out, point); }
inline void dumpItem(PrintStream& out, const Use& use) { dumpUse(out, use); }
void dumpItem(PrintStream&, OperandUses);
void dumpItem(PrintStream&, EscapeReasons);

template<typename T>
void dumpItem(PrintStream& out, std::span<const T> items)
{
    dumpList(out, items, [](PrintStream& out, const T& item) { dumpItem(out, item); });
}

template<typename T> void dumpItem(PrintStream& out, const Vector<T>& items) { dumpItem(out, items.span()); }

template<typename... Items> void dumpItems(PrintStream& out, const Items&... items) { (dumpItem(out, items), ...); }
template<typename... Items> void dumpLine(PrintStream& out, const Items&... items) { dumpItems(out, items..., "\n"_s); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
