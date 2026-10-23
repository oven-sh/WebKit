/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCopy.h"

#if ENABLE(AOT)

#include "AOTObject.h"
#include "AOTVector.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

struct CopiedProperties {
    bool tryAppend(VM& vm, UniquedStringImpl* name, JSValue value)
    {
        if (name == vm.propertyNames->underscoreProto.impl())
            return false;
        names.append(name);
        values.appendWithCrashOnOverflow(value);
        return true;
    }

    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
};

bool takesPlainPropertiesDirectly(Structure* target)
{
    return target->typeInfo().type() == FinalObjectType
        && !target->isCompiledStructure()
        && !target->mayHaveImmutableProperties()
        && !target->mayBePrototype()
        && !target->isWatchingReplacement()
        && !target->isDictionary()
        && target->isStructureExtensible()
        && !target->hasPolyProto();
}

static bool canDefineByStoring(VM& vm, Structure* target, const CopiedProperties& copied)
{
    if (!target->hasReadOnlyOrGetterSetterPropertiesExcludingProto() && !target->hasNonEnumerableProperties() && !target->hasNonConfigurableProperties())
        return true;
    for (UniquedStringImpl* name : copied.names) {
        unsigned attributes;
        if (isValidOffset(target->get(vm, name, attributes)) && attributes)
            return false;
    }
    return true;
}

static bool tryStore(VM& vm, JSObject* target, CopiedProperties& copied, TargetPrototypes prototypes)
{
    bool canStore = prototypes == TargetPrototypes::MayIntercept ? target->canPerformFastPutInlineExcludingProto() : canDefineByStoring(vm, target->structure(), copied);
    if (!canStore)
        return false;
    if (!copied.names.isEmpty())
        target->putOwnDataPropertyBatching(vm, copied.names.mutableSpan().data(), copied.values.data(), copied.names.size());
    return true;
}

bool tryCopyDataProperties(Instance*, VM& vm, JSObject* target, JSObject* source, TargetPrototypes prototypes)
{
    Structure* sourceStructure = source->structure();
    if (!takesPlainPropertiesDirectly(target->structure()))
        return false;
    if (sourceStructure->isDictionary() || !sourceStructure->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(sourceStructure->indexingType()))
        return false;
    if (sourceStructure->hasNonReifiedStaticProperties())
        return false;

    CopiedProperties copied;
    bool canCopy = true;
    EnsureStillAliveScope sourceStructureScope(sourceStructure);
    sourceStructure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if ((entry.attributes() & PropertyAttribute::DontEnum) || PropertyName(entry.key()).isPrivateName())
            return true;
        canCopy = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue) && copied.tryAppend(vm, entry.key(), source->getDirect(entry.offset()));
        return canCopy;
    });
    return canCopy && tryStore(vm, target, copied, prototypes);
}

bool tryCopyRecordDataProperties(Instance*, VM& vm, JSObject* target, JSObject* object)
{
    Object* record = compiledObjectOrNull(object);
    if (!record || !takesPlainPropertiesDirectly(target->structure()))
        return false;

    CopiedProperties copied;
    bool canCopy = true;
    bool hasOnlyFields = record->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        canCopy = copied.tryAppend(vm, &name, value);
        return canCopy;
    }, Object::SymbolKeys::Include);
    return hasOnlyFields && canCopy && tryStore(vm, target, copied, TargetPrototypes::Ignored);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
