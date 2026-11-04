/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerPlan.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool isSerializedAsJSONStringifyDoes(const BuiltinParameter& parameter)
{
    constexpr Protocols protocolsOfSerialization { Protocol::OwnKeys, Protocol::OwnValues, Protocol::ToJSON, Protocol::ToPrimitive };
    constexpr OperandUses usesOfReaders { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::AppliesTransitively };
    return parameter.uses.contains(OperandUse::AppliesTransitively) && usesOfReaders.containsAll(parameter.uses)
        && parameter.protocols.contains(Protocol::ToJSON) && protocolsOfSerialization.containsAll(parameter.protocols)
        && parameter.roles.contains(BuiltinOperandRole::OnlyBoxedPrimitivesAreConverted) && !parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused);
}

static bool someBuiltinInUseSerializes(const Program& program, const Facts& facts)
{
    const BuiltinTable& table = program.builtins();
    return std::ranges::any_of(facts.builtinUses(), [&](const BuiltinUseRecord& use) {
        return std::ranges::any_of(table.overloadsOf(BuiltinID(use.builtin)), [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.parameters, isSerializedAsJSONStringifyDoes);
        });
    });
}

void planSerializers(CallingConventionParts& parts, const Program& program, const Facts& facts)
{
    Mechanisms mechanisms = withoutMechanismsMissingDependencies(facts.mechanisms());
    if (!mechanisms.contains(Mechanism::BuiltinCalls) || !mechanisms.contains(Mechanism::TypedOperations) || !someBuiltinInUseSerializes(program, facts))
        return;

    auto bodyOf = [&](ModuleID module) {
        FunctionID body = module ? program.at(module).body : FunctionID();
        return body && program.at(body).hasBody() && parts.conventions[body.index()].has(CodeRole::Body) ? body : FunctionID();
    };
    FunctionID bodyOfFirstEntry = program.entries.isEmpty() ? FunctionID() : bodyOf(program.entries[0].module);

    const LinkTables& link = facts.linked();
    size_t numberOfPlaces = std::min<size_t>(program.layouts.size(), CodeUnit::numberOfLayoutsThatSerializersCanName);
    parts.functionsOfSerializers.fill(FunctionID(), numberOfPlaces);
    for (size_t index = 0; index < numberOfPlaces; ++index) {
        const Layout& layout = program.layouts[index];
        if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || !isFoundByShapeTest(layout))
            continue;
        if (index < link.layouts.size() && !link.layouts[index].isAllocated)
            continue;
        FunctionID bodyOfDeclaringModule = bodyOf(layout.module);
        parts.functionsOfSerializers[index] = bodyOfDeclaringModule ? bodyOfDeclaringModule : bodyOfFirstEntry;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
