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

// `node.accept(v)` where nothing is known of `node`: whatever has a row under the name is expected, and one test of shapes asks for all of them (callThroughMethodTableBehindTest() of walk/AOTWalkUnionCalls.cpp).
// A family grows with the program and a call asks at every copy, so the set is made once for the build.
static void noteReceiversInUnionSelectors(const Plan& plan, TypeArena& types, MethodFamily& family)
{
    Vector<LayoutID> receivers;
    // The test asks for shape numbers.
    for (const MethodTableRow& row : family.rows) {
        if (!plan.shapes().shapesOf(row.layout).isEmpty())
            receivers.append(row.layout);
    }
    if (!receivers.isEmpty())
        family.receivers = types.at(types.forLayouts(receivers.span())).layouts;
}

// `shape.scale(2.5)`. Argument i crosses raw where the code that a row leads to takes it raw: each method that names the parameter is entered at its Body, which takes a number that Link has proven.
// What the selector takes there is all that those methods take. A method that does not name the parameter asks nothing of it.
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

// The typed selector returns as every method of the family does. Only where nothing puts the lookup into the table: what a lookup finds returns anything.
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

// planProgram(), behind the guards: which unit a row leads to hangs on them. AOTPlan.cpp declares this.
// THE BOXED SELECTOR OF A FAMILY AND A COUNT STAYS AS IT IS, and CallingConventions::findMethodSelector() answers it: the emitter takes the table for a closed call, whose record is a boxed value, by that one alone.
// The typed one stands right behind it, and only a CallTable that the walk has built names it.
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
