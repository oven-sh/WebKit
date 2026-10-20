/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

void dumpAnalyses(PrintStream&, FunctionAnalyses&, AnalysisSet);

ASCIILiteral sectionNameOf(FunctionAnalysis);
std::optional<FunctionAnalysis> parseSectionName(StringView);

void dumpID(PrintStream&, ValueID);
void dumpID(PrintStream&, BlockID);
void dumpID(PrintStream&, ObjectID);
void dumpPoint(PrintStream&, ProgramPoint);
void dumpEdge(PrintStream&, Edge);
void dumpUse(PrintStream&, const Use&);
void dumpBoolean(PrintStream&, bool);

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

void dumpValues(PrintStream&, std::span<const ValueID>);
void dumpBlocks(PrintStream&, std::span<const BlockID>);
void dumpObjects(PrintStream&, std::span<const ObjectID>);
void dumpObjects(PrintStream&, ObjectSet);

void dumpOperandUses(PrintStream&, OperandUses);
void dumpEscapeReasons(PrintStream&, EscapeReasons);
void dumpProofs(PrintStream&, ProofSet);
void dumpGuardFacts(PrintStream&, const Program&, const GuardFacts&);

void dumpLiveness(PrintStream&, const CFG&, const Liveness&, const BitVector* trackedBlocks);
void dumpUseEffectHistory(PrintStream&, const CFG&, const EscapeAnalysis&, const UseEffectHistory&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
