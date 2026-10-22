/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNarrowConstantVariables.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTThreads.h"
#include "AOTWholeLiterals.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The value is unknown where the store does not store a primitive constant.
struct Store {
    VariableID variable;
    KnownValue value;
    std::optional<Vector<KnownValue, 8>> list;
};

enum class StoredValues : uint8_t { None, OneConstant, Other };

struct Contents {
    StoredValues stored { StoredValues::None };
    KnownValue constant;
};

} // anonymous namespace

static bool readsSeeOnlyStoredValue(const Variable& variable)
{
    switch (variable.initialization) {
    case VariableInitialization::Uninitialized:
        return true;
    case VariableInitialization::Undefined:
        return variable.flow.initialization == InitializationState::StoredBeforeEveryRead;
    case VariableInitialization::Function:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A static field is a property of its class too, which has more ways to change than a store.
static bool mayHoldOneConstant(const Variable& variable)
{
    return !variable.owner
        && variable.flow.writers == WriterState::AllKnown
        && variable.flow.stores == StoreCount::AtMostOnce
        && !variable.declared.flags.contains(PositionFlags::IsExpected)
        && readsSeeOnlyStoredValue(variable);
}

static void collectStores(const Program& program, const FlowResults& flow, FunctionID id, Vector<Store>& stores)
{
    const Function& function = program.functions[id.index()];
    forEachListedValue(program, id, { Opcode::PutModuleVariable, Opcode::ValidateFirstRun }, [&](BlockID, uint32_t, ValueID, const Value& value) {
        if (!mayHoldOneConstant(program.at(value.variable())))
            return;
        // The type of what a store stores has to lie inside the type of the variable, so the type has to say the constant.
        KnownValue stored;
        std::optional<Vector<KnownValue, 8>> list;
        if (value.opcode == Opcode::PutModuleVariable) {
            stored = knownValueOfType(program, program.typeOf(function, value.valueOperand()));
            list = findConstantsOfWholeLiteral(program, flow, function, value.valueOperand());
        }
        stores.append(Store { value.variable(), stored.isObject() ? KnownValue() : stored, WTF::move(list) });
    });
}

static void addStoredValue(Contents& contents, KnownValue stored)
{
    if (!stored.isKnown() || (contents.stored == StoredValues::OneConstant && contents.constant != stored)) {
        contents.stored = StoredValues::Other;
        return;
    }
    if (contents.stored == StoredValues::None)
        contents = { StoredValues::OneConstant, stored };
}

// A location with a dead zone holds `absent` until its store, and one that is born undefined holds that.
static uint16_t primitivesBeforeStore(const Variable& variable, const Type& declared)
{
    uint16_t kept = declared.primitives & Primitive::Absent;
    if (variable.initialization == VariableInitialization::Undefined)
        kept |= declared.primitives & Primitive::Undefined;
    return kept;
}

// A second store in the text may be the one that runs.
static ConstantLists collectListsStoredAlone(const Program& program, Vector<Store>& stores)
{
    Vector<uint8_t> numberOfStores(program.variables.size());
    numberOfStores.fill(0);
    for (const Store& store : stores)
        numberOfStores[store.variable.index()] = std::min(numberOfStores[store.variable.index()] + 1, 2);

    ConstantLists lists;
    for (Store& store : stores) {
        if (store.list && numberOfStores[store.variable.index()] == 1)
            lists.entries.append({ store.variable, Vector<KnownValue>(store.list->span()) });
    }
    std::ranges::sort(lists.entries, { }, &ConstantLists::Entry::variable);
    return lists;
}

ConstantLists collectConstantLists(const Program& program, const FlowResults& flow, BuildContext& build)
{
    Vector<Store> stores = collectInParallel<Store>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Store>& found) {
        collectStores(program, flow, FunctionID(index), found);
    });
    return collectListsStoredAlone(program, stores);
}

ConstantLists narrowConstantVariables(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<Store> stores = collectInParallel<Store>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Store>& found) {
        collectStores(program, flow, FunctionID(index), found);
    });

    Vector<Contents> contentsOfVariable(program.variables.size());
    for (const Store& store : stores)
        addStoredValue(contentsOfVariable[store.variable.index()], store.value);

    for (unsigned index = 0; index < contentsOfVariable.size(); ++index) {
        const Contents& contents = contentsOfVariable[index];
        if (contents.stored != StoredValues::OneConstant)
            continue;

        VariableID id(index);
        const Variable& variable = program.at(id);
        const Type& declared = program.at(variable.declared.type);
        Type constant = typeOfKnownPrimitive(contents.constant);
        if (!constant.isSubsetOf(declared, program))
            continue;
        TypeID narrowed = editor.types().intern(constant.withPrimitives(primitivesBeforeStore(variable, declared)));
        if (narrowed != variable.declared.type)
            editor.at(id).declared.type = narrowed;
    }
    return collectListsStoredAlone(program, stores);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
