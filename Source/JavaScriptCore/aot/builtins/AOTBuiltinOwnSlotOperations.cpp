/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOwnSlots.h"
#include "AOTBuiltinTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <span>

namespace JSC { namespace AOT {

// The table has the names and not the attributes.
static Structure* structureAfterAddingOwnSlots(VM& vm, const BuiltinTable& table, Structure* structure, std::span<const BuiltinName> names, JSType typeOfInstances)
{
    for (size_t position = 0; structure && position < names.size(); ++position) {
        Identifier name = table.identifierOf(vm, names[position]);
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (!isValidOffset(offset)) {
            attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
            structure = Structure::addPropertyTransition(vm, structure, name, attributes, offset);
        }
        if (structure->isDictionary() || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            return nullptr;
        if (offset != propertyOffsetOfOwnSlot(typeOfInstances, static_cast<uint8_t>(position)))
            return nullptr;
    }
    return structure;
}

void publishStructureOfOwnSlot(Instance& instance, BuiltinOverloadID id)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    const BuiltinOverloadSummary& overload = table.summaryOf(id);
    RELEASE_ASSERT(overload.kind == BuiltinImplementationKind::OwnSlot);
    const BuiltinOwnSlot& slot = overload.ownSlot;
    RELEASE_ASSERT(slot.structure.index() < BuiltinWords::numberOfOwnSlotStructures);
    uint32_t& published = instance.builtins.ownSlotStructureIDs[slot.structure.index()];
    if (published)
        return;
    instance.builtins.ownSlotStructuresLookedFor |= 1u << slot.structure.index();

    const BuiltinRealmStructure& description = table.at(slot.structure);
    BuiltinClassID builtinClass = table.receiverClassOf(overload.builtin);
    RELEASE_ASSERT(builtinClass && description.names.size() == slot.offset + 1u);
    Structure* structure = structureAfterAddingOwnSlots(*instance.vm, table, description.findInitialStructure(instance.globalObject), description.names, table.at(builtinClass).type);
    if (!structure) {
        published = structureIDOfNoInstance;
        return;
    }
    // The number of a dead Structure may be given to another one.
    instance.state->immortalCells->add(structure);
    published = structure->id().bits();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindOwnSlotStructure, void, (Instance* instance, uint32_t overload))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTFindOwnSlotStructure);
    publishStructureOfOwnSlot(*instance, BuiltinOverloadID(overload));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
