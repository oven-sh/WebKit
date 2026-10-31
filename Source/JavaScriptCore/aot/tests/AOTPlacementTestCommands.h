/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include "AOTPlacement.h"
#include "AOTShapeTest.h"
#include "AOTSlotPlan.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestCommand.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <span>
#include "AOTBitVector.h"

namespace JSC { namespace AOT { namespace Typed {

void runPlanSlots(TestCommandContext&);
void runTables(TestCommandContext&);

template<typename Range>
void printList(PrintStream& out, const Range& items, NOESCAPE const auto& printItem)
{
    out.print("[");
    bool isFirst = true;
    for (const auto& item : items) {
        if (!isFirst)
            out.print(" ");
        isFirst = false;
        printItem(item);
    }
    out.print("]");
}

template<typename View>
const View* findView(std::span<const View> views, StringView name)
{
    auto found = std::ranges::find_if(views, [&](const View& view) {
        return name == view.name;
    });
    return found == views.end() ? nullptr : &*found;
}

JS_EXPORT_PRIVATE ASCIILiteral nameOf(FieldAccess::Coverage);
JS_EXPORT_PRIVATE ASCIILiteral nameOf(StaticsRegion);
JS_EXPORT_PRIVATE ASCIILiteral nameOf(MemberGuardKind);
JS_EXPORT_PRIVATE ASCIILiteral nameOf(VariableEncoding);

JS_EXPORT_PRIVATE BitVector collectSetsNamedBySites(const Program&);

JS_EXPORT_PRIVATE void printShapeTest(PrintStream&, const ShapeTest&);
JS_EXPORT_PRIVATE void printSlot(PrintStream&, const PlacedSlot&);
JS_EXPORT_PRIVATE void printRequestRecord(IRPrinter&, const RequestRecord&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
