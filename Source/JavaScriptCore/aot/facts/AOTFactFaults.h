/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <optional>
#include <wtf/text/StringView.h>

namespace WTF {
class PrintStream;
}

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Facts;
class Plan;
class Program;
class ThinSummaries;
struct FactTables;
struct LinkTables;

std::optional<uint32_t> numberOfMisstatedFact(StringView name);
void listMisstatedFacts(WTF::PrintStream&);

void misstateSummaries(const Program&, ThinSummaries&, BuildContext&);
void misstateLinkTables(Program&, const ThinSummaries&, LinkTables& linked, LinkTables& held, BuildContext&);
void misstateFactTables(Program&, LinkTables& linked, FactTables&, BuildContext&);
void misstatePlan(Program&, const Facts&, Plan&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
