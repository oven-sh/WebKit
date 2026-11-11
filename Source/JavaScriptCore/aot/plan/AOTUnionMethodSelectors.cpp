/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlan.h"

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTNumberMethodSelectors.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isRawFormOfNumberInUnionSelectors(Representation form)
{
    return form == Representation::Int32 || form == Representation::Double;
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

namespace {

struct RowsByMethodInUnionSelectors {
    BitVector hasGuardedRow;
    Vector<Vector<LayoutID, 2>> receivers;
};

} // anonymous namespace

static RowsByMethodInUnionSelectors sortRowsByMethodInUnionSelectors(const MethodFamily& family)
{
    RowsByMethodInUnionSelectors rows;
    rows.receivers.grow(family.methods.size());
    for (const MethodTableRow& row : family.rows) {
        size_t place = std::ranges::lower_bound(family.methods, row.method) - family.methods.begin();
        rows.receivers[place].append(row.layout);
        if (row.guardedClass)
            rows.hasGuardedRow.set(place);
    }
    return rows;
}

static bool isEnteredDirectlyInUnionSelectors(const Program& program, const Facts& facts, const CallingConventions& conventions, TypeArena& types, const MethodFamily& family, const RowsByMethodInUnionSelectors& rows, const MethodSelector& selector, size_t place)
{
    FunctionID method = family.methods[place];
    const Function& function = program.at(method);
    CodeUnit entered = conventions.directEntryOf(method);
    unsigned numberOfParameters = 1 + selector.numberOfArguments;
    if (function.isCoroutine() || function.numberOfEntryParameters() != numberOfParameters || function.numberOfPositionalParameters() != selector.numberOfArguments || function.entryParameterKind(0) != EntryParameter::This)
        return false;
    if (!conventions.exists(entered) || conventions.numberOfResults(entered) != 1 || !haveSameRegisterRepresentation(conventions.resultRepresentation(entered), selector.result))
        return false;
    if (conventions.stackWordsReservedByCallersOf(program, entered))
        return false;
    if (rows.hasGuardedRow.get(place))
        return false;
    for (unsigned index = 0; index < numberOfParameters; ++index) {
        if (!haveSameRegisterRepresentation(index ? Representation::JSValue : Representation::Cell, conventions.parameterRepresentation(program, entered, index)))
            return false;
        TypeID known = facts.typeOfEntryParameter(method, index).proven;
        if (!known)
            continue;
        TypeID held = facts.typeBelievedOfEntryParameter(types, method, index, known);
        if (!types.at(held).isJavaScriptValue())
            continue;
        if (index) {
            if (!types.at(held).isAnything())
                return false;
            continue;
        }
        if (!types.isSubset(types.forLayouts(rows.receivers[place].span()), held))
            return false;
    }
    return true;
}

void finishMethodTableForUnions(Program& program, const Facts& facts, Plan& plan)
{
    const CallingConventions& conventions = plan.conventions();
    if (conventions.methodFamilies().empty())
        return;
    const LinkTables& linked = facts.linked();
    bool hasTypedSelectors = facts.mechanisms().contains(Mechanism::TypedOperations);
    if (!hasTypedSelectors && !facts.mechanisms().contains(Mechanism::Guesses))
        return;

    MethodTablePlan table;
    table.families.append(conventions.methodFamilies());
    TypeArena types(program.types, ArenaKeeps::InProgram);
    if (hasTypedSelectors) {
        for (MethodFamily& family : table.families)
            noteTypesOfArgumentsInUnionSelectors(program, linked, conventions, types, family);
    }

    uint32_t familyOfRowsByMethod = UINT32_MAX;
    RowsByMethodInUnionSelectors rowsByMethod;
    for (const MethodSelector& boxed : conventions.methodSelectors()) {
        const MethodFamily& family = table.families[boxed.family];
        uint32_t numberOfUnits = 1 + static_cast<uint32_t>(family.methods.size());
        MethodSelector selector = boxed;
        selector.firstUnit = table.numberOfUnits;
        if (familyOfRowsByMethod != boxed.family) {
            familyOfRowsByMethod = boxed.family;
            rowsByMethod = sortRowsByMethodInUnionSelectors(family);
        }
        for (size_t place = 0; place < family.methods.size(); ++place) {
            if (isEnteredDirectlyInUnionSelectors(program, facts, conventions, types, family, rowsByMethod, selector, place))
                selector.methodsEnteredDirectly.set(place);
        }
        selector.everyMethodIsEnteredDirectly = selector.methodsEnteredDirectly.bitCount() == family.methods.size();
        table.selectors.append(selector);
        table.numberOfUnits += numberOfUnits;
        if (!hasTypedSelectors)
            continue;

        MethodSelector typed = selector;
        typed.isTyped = true;
        typed.methodsEnteredDirectly = { };
        typed.everyMethodIsEnteredDirectly = false;
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
