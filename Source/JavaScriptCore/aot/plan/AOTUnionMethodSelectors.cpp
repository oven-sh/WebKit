/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlan.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTNumberMethodSelectors.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isRawFormOfNumberInUnionSelectors(Representation form)
{
    return form == Representation::Int32 || form == Representation::Double;
}

static void noteReceiversInUnionSelectors(const Plan& plan, TypeArena& types, MethodFamily& family)
{
    Vector<LayoutID> receivers;
    for (const MethodTableRow& row : family.rows) {
        if (!plan.shapes().shapesOf(row.layout).isEmpty())
            receivers.append(row.layout);
    }
    if (!receivers.isEmpty())
        family.receivers = types.at(types.forLayouts(receivers.span())).layouts;
}

static void noteTypesOfArgumentsInUnionSelectors(const Program& program, const LinkTables& linked, const CallingConventions& conventions, TypeArena& types, MethodFamily& family)
{
    family.typesOfArguments.fill(TypeID(), maxArgumentsOfTableCall);
    for (unsigned argument = 0; argument < maxArgumentsOfTableCall; ++argument) {
        TypeID taken;
        bool everyMethodTakesItRaw = true;
        for (FunctionID method : family.methods) {
            const Function& function = program.at(method);
            if (argument >= function.numberOfPositionalParameters())
                continue;
            unsigned entryParameter = function.firstParameterIndex() + argument;
            std::span<const LinkedParameter> parameters = linked.parametersOf(method);
            TypeID proven = entryParameter < parameters.size() ? parameters[entryParameter].proven : TypeID();
            if (!proven || conventions.hasParameterGuard(method) || !isRawFormOfNumberInUnionSelectors(conventions.parameterRepresentation(program, { method, CodeRole::Body }, entryParameter))) {
                everyMethodTakesItRaw = false;
                break;
            }
            taken = taken ? types.unionOf(taken, proven) : proven;
        }
        if (everyMethodTakesItRaw && taken && types.at(taken).isNumber())
            family.typesOfArguments[argument] = taken;
    }
}

static void noteResultInUnionSelectors(const Program& program, const LinkTables& linked, const CallingConventions& conventions, TypeArena& types, const MethodFamily& family, MethodSelector& typed)
{
    FormsOfSelector forms = formsOfSelector(program, linked, conventions, family);
    if (family.entersLookup() || !forms.everyRowIsProven || forms.returnsNothing || !isRawFormOfNumberInUnionSelectors(forms.result))
        return;
    TypeID returned;
    for (FunctionID method : family.methods)
        returned = returned ? types.unionOf(returned, linked.function(method).result) : linked.function(method).result;
    if (!returned || !types.at(returned).isNumber())
        return;
    typed.result = forms.result;
    typed.typeOfResult = returned;
}

void finishMethodTableForUnions(Program& program, const Facts& facts, Plan& plan)
{
    const CallingConventions& conventions = plan.conventions();
    if (conventions.methodFamilies().empty())
        return;
    const LinkTables& linked = facts.linked();
    bool testsReceivers = facts.mechanisms().contains(Mechanism::Guesses);
    bool hasTypedSelectors = facts.mechanisms().contains(Mechanism::TypedOperations);
    if (!testsReceivers && !hasTypedSelectors)
        return;

    MethodTablePlan table;
    table.families.append(conventions.methodFamilies());
    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (MethodFamily& family : table.families) {
        if (testsReceivers)
            noteReceiversInUnionSelectors(plan, types, family);
        if (hasTypedSelectors)
            noteTypesOfArgumentsInUnionSelectors(program, linked, conventions, types, family);
    }

    for (const MethodSelector& boxed : conventions.methodSelectors()) {
        const MethodFamily& family = table.families[boxed.family];
        uint32_t numberOfUnits = 1 + static_cast<uint32_t>(family.methods.size());
        MethodSelector selector = boxed;
        selector.firstUnit = table.numberOfUnits;
        table.selectors.append(selector);
        table.numberOfUnits += numberOfUnits;
        if (!hasTypedSelectors)
            continue;

        MethodSelector typed = selector;
        typed.isTyped = true;
        typed.firstUnit = table.numberOfUnits;
        noteResultInUnionSelectors(program, linked, conventions, types, family, typed);
        bool takesRawArgument = false;
        for (uint32_t argument = 0; argument < typed.numberOfArguments; ++argument)
            takesRawArgument |= !!family.typesOfArguments[argument];
        if (!takesRawArgument && !typed.typeOfResult)
            continue;
        table.selectors.append(typed);
        table.numberOfUnits += numberOfUnits;
    }
    plan.setMethodTable(WTF::move(table));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
