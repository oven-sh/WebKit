/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "StructureInlines.h"
#include "VM.h"

namespace JSC { namespace AOT {

constexpr ObjectMutation mutationOf(SlotExitReason reason)
{
    switch (reason) {
    case SlotExitReason::Delete:
        return ObjectMutation::DeleteField;
    case SlotExitReason::Redefinition:
        return ObjectMutation::RedefineProperty;
    case SlotExitReason::StoreOfOtherType:
        return ObjectMutation::StoreOfOtherType;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

[[noreturn]] void crashOnImpossibleMutation(VM&, const Object&, ObjectMutation);

ALWAYS_INLINE void validateMutation(VM& vm, const Object& object, ObjectMutation mutation)
{
    if (object.layout().impossibleMutations().contains(mutation) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]]
        crashOnImpossibleMutation(vm, object, mutation);
}

[[noreturn]] void crashOnImpossibleMutation(VM&, const Object&, PropertyName, ObjectMutation);

ALWAYS_INLINE void validateMutation(VM& vm, const Object& object, PropertyName name, ObjectMutation mutation)
{
    if (object.layout().impossibleMutations().contains(mutation) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]]
        crashOnImpossibleMutation(vm, object, name, mutation);
}

constexpr uint16_t flagOfFieldsThatCannot(ObjectMutation mutation)
{
    if (mutation == ObjectMutation::DeleteField)
        return fieldIsNeverDeleted;
    if (mutation == ObjectMutation::RedefineProperty)
        return fieldIsNeverRedefined;
    if (mutation == ObjectMutation::StoreOfOtherType)
        return fieldIsNeverStoredWithOtherType;
    return 0;
}

[[noreturn]] void crashOnImpossibleMutation(VM&, const Object&, const FieldData&, ObjectMutation);

ALWAYS_INLINE void validateMutation(VM& vm, const Object& object, const FieldData& field, ObjectMutation mutation)
{
    validateMutation(vm, object, mutation);
    if (field.flags & flagOfFieldsThatCannot(mutation) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]]
        crashOnImpossibleMutation(vm, object, field, mutation);
}

enum class FrameThatAsks : bool { IsTheTopFrame, IsAboveTheTopFrame };
bool isAskedByCodeThatInspectorBrought(VM&, const Instance&, FrameThatAsks = FrameThatAsks::IsTheTopFrame);
bool isAskedByCodeThatInspectorBrought(VM&, const Object&);
bool refusesMutation(VM&, const Object&, ObjectMutation);

[[nodiscard]] ALWAYS_INLINE bool admitsMutation(VM& vm, const Object& object, ObjectMutation mutation)
{
    if (object.layout().impossibleMutations().contains(mutation) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]] {
        if (refusesMutation(vm, object, mutation))
            return false;
        crashOnImpossibleMutation(vm, object, mutation);
    }
    return true;
}

[[nodiscard]] ALWAYS_INLINE bool admitsMutation(VM& vm, const Object& object, const FieldData& field, ObjectMutation mutation)
{
    if (!admitsMutation(vm, object, mutation))
        return false;
    if (field.flags & flagOfFieldsThatCannot(mutation) && !vm.aotState().nativeTestChangesObjectsUnseen) [[unlikely]] {
        if (refusesMutation(vm, object, mutation))
            return false;
        crashOnImpossibleMutation(vm, object, field, mutation);
    }
    return true;
}

struct FieldProperty {
    explicit operator bool() const { return !!valueOrAccessor; }

    JSValue valueOrAccessor;
    unsigned attributes { 0 };
    bool isInSlot { false };
};

UniquedStringImpl* propertyNameOf(VM&, const Object&, const FieldData&);
FieldProperty findFieldProperty(VM&, const Object&, const FieldData&);
bool livesInExtraProperties(VM&, const Object&, const FieldData&);
bool tryCreateField(VM&, State&, Object&, const FieldData&, JSValue);
void putProperty(VM&, JSObject* holder, PropertyName, Object::LeavingProperty);
void removeFieldFromKeyOrder(VM&, State&, Object&, const FieldData&);
void didChangeOwnProperties(VM&, Object&, VM::StructureChainIntegrityEvent, UniquedStringImpl* nameOrNull);
bool interceptsIndexedAccesses(const Object&);
void didChangeIndexedProperties(VM&, Object&, bool interceptedIndexedAccessesBefore);

ALWAYS_INLINE bool isExtraKey(State& state, const Object& object, UniquedStringImpl* name)
{
    if (PropertyName(name).isPrivateName()) [[unlikely]]
        return false;
    return !(object.orderWord() & orderWordHasFieldOutsideSlot) || !object.fieldNamed(state, name);
}

void forEachKeyAsCreated(VM& vm, State& state, const Object& object, const Invocable<void(uint16_t)> auto& fieldFunctor, const auto& extraKeyFunctor)
{
    uint32_t numberOfFields = object.layout().numberOfFields;
    JSObject* extra = object.extraProperties();
    std::span<const uint16_t> entries = object.keyOrder(state);
    std::span<const RefPtr<UniquedStringImpl>> namedKeys;
    if (object.hasKeyOrderOfItsOwn())
        namedKeys = object.compiledStructure().keysNamedByOrder();
    ExtraKeyCounter positioned(numberOfFields);
    uint32_t numberOfKeysListed = 0;
    size_t next = 0;
    UniquedStringImpl* lastKeyInFront = nullptr;
    auto listFieldsUpToNextExtraKey = [&] {
        lastKeyInFront = nullptr;
        while (next < entries.size()) {
            uint16_t entry = entries[next++];
            if (entry < numberOfFields) {
                fieldFunctor(entry);
                continue;
            }
            if (!namedKeys.empty()) {
                lastKeyInFront = namedKeys[entry - numberOfFields].get();
                return;
            }
            positioned.add(entry);
            if (positioned.count() > numberOfKeysListed)
                return;
        }
    };
    listFieldsUpToNextExtraKey();
    if (extra) {
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            if (!isExtraKey(state, object, entry.key()))
                return true;
            extraKeyFunctor(entry);
            if (++numberOfKeysListed == positioned.count() || entry.key() == lastKeyInFront)
                listFieldsUpToNextExtraKey();
            return true;
        });
    }
    for (uint16_t entry : entries.subspan(next)) {
        if (entry < numberOfFields)
            fieldFunctor(entry);
    }
}

enum class KeysListed : uint8_t { Strings = 1, Symbols = 2, StringsAndSymbols = 3 };

void forEachOwnKey(VM& vm, State& state, const Object& object, KeysListed listed, DontEnumPropertiesMode mode, const Invocable<void(UniquedStringImpl*)> auto& functor)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = object.layout();
    std::span<const FieldData> fields = layout.fields();
    auto isIncluded = [&](unsigned attributes) ALWAYS_INLINE_LAMBDA {
        return mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum);
    };
    bool hasSymbolKeys = layout.hasFieldsWithSymbolKeys;
    auto listKeys = [&](bool symbols) {
        forEachKeyAsCreated(vm, state, object, [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (field.isPrivate() || !(symbols ? layouts.hasSymbolKey(field) : layouts.hasStringKey(field)))
                return;
            unsigned attributes = field.attributes();
            if (!object.isInSlot(field)) {
                FieldProperty property = findFieldProperty(vm, object, field);
                if (!property)
                    return;
                attributes = property.attributes;
            }
            if (isIncluded(attributes))
                functor(layouts.nameOf(vm, field));
        }, [&](const auto& entry) ALWAYS_INLINE_LAMBDA {
            bool isSymbol = entry.key()->isSymbol();
            hasSymbolKeys |= isSymbol;
            if (isSymbol == symbols && isIncluded(entry.attributes()))
                functor(entry.key());
        });
    };
    if (static_cast<uint8_t>(listed) & static_cast<uint8_t>(KeysListed::Strings))
        listKeys(false);
    else
        hasSymbolKeys |= object.hasExtraProperties();
    if (hasSymbolKeys && static_cast<uint8_t>(listed) & static_cast<uint8_t>(KeysListed::Symbols))
        listKeys(true);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
