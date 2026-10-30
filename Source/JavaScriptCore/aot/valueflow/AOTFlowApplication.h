/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactList.h"
#include "AOTFlowResults.h"
#include "AOTTypeArena.h"
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowPositionUpdate {
    TypeID type;
    bool isProven { false };
    bool replacesExpectation { false };
    UnprovenCause cause { UnprovenCause::Unknown };
    Witness witness { noWitness };

    bool isEmpty() const { return !type && !isProven; }
};

struct FlowAccessorField {
    OptionSet<FieldForm> form;
    CompactList<FunctionID> getters;
    CompactList<FunctionID> setters;

    bool isAccessor() const { return form.contains(FieldForm::IsAccessor); }
};

struct FlowFieldUpdate {
    FlowPositionUpdate position;
    FieldFlow flow;
    FlowAccessorField accessor;
    bool undefinedAtStartIsNeverRead { false };
};

// An allocation lists, for a field, a value that may lie outside the type of the field. Nothing tests it there.
struct FlowListedValue {
    uint32_t field { 0 };
    FunctionID function;
    ValueID allocation;
    uint16_t primitivesOutside { 0 };
};

struct FlowAppendedField {
    PropertyKeyID key;
    FlowPositionUpdate position;
    FieldFlow flow;
    FlowAccessorField accessor;
    bool isFilledByCreators { false };
    // For a field that holds a value: the form in which every definition creates the property.
    OptionSet<FieldForm> formOfValue;
};

struct FlowLayoutUpdate {
    LayoutFlow flow;
    ClassFlow classFlow;
    Vector<FlowFieldUpdate> fields;
    Vector<FlowAppendedField> appendedFields;
    Vector<FlowListedValue> valuesListedWithOtherTypes;
    Vector<Vector<PropertyKeyID>> literalKeyOrders;
    bool objectsAreDictionaries { false };
    bool objectsArePrototypesOfCreatedObjects { false };
    bool isHandedToObjectCreateAlone { false };
    bool createsFieldsOnlyInLiterals { false };
};

struct FlowVariableUpdate {
    FlowPositionUpdate position;
    LocationFlow flow;
};

struct FlowSignatureUpdate {
    Vector<FlowPositionUpdate> entryParameters;
    Vector<FlowPositionUpdate, 1> results;
    OptionSet<FunctionCapabilities> provenCapabilities;
    PropertyKeyID keyThatNamesFunction;
};

struct FlowValueProof {
    ValueID value;
    TypeID type;
    // The type is what flow holds of the value, and flow follows what a check turns away. Otherwise it was cut out of the type of the value as read, which may be owed to a check.
    bool holdsInEveryVersion { false };
};

// A prediction, not a proof: a test guards whatever is made of it.
struct FlowValueExpectation {
    ValueID value;
    TypeID type;
};

struct FlowClaimUpdate {
    ValueID claim;
    bool isRedundant { false };
    TypeID arriving;
    bool mayFail { false };
    // Of an answer: see FlowValueProof.
    bool holdsInEveryVersion { false };
};

struct FlowAccessStamp {
    ValueID value;
    LayoutSetID receivers;
};

struct FlowCallStamp {
    ValueID value;
    CalleeSetID callees;
};

struct FlowTailCallUpdate {
    ValueID call;
    bool claimsAreRedundant { false };
};

struct FlowAllocationStamp {
    ValueID value;
    AllocationSiteID site;
    AllocationClassID allocationClass;
    TypeID elements;
    bool elementsAreProven { false };
    ArrayStorage arrayStorage { ArrayStorage::Other };
};

struct FlowSymbolConstant {
    ValueID call;
    PropertyKeyID key;
};

struct FlowDefinedValue {
    ValueID definition;
    Value value;
};

struct FlowMemberKey {
    ValueID definition;
    PropertyKeyID key;
};

struct FlowBodyUpdate {
    CompactList<FlowValueProof> proofs;
    CompactList<FlowValueExpectation> expectations;
    CompactList<ValueID> readsAfterInitialization;
    CompactList<ValueID> opensOfUndecidedStorage;
    CompactList<FlowClaimUpdate> claims;
    CompactList<FlowAccessStamp> accesses;
    CompactList<FlowCallStamp> calls;
    CompactList<FlowTailCallUpdate> tailCalls;
    CompactList<FlowAllocationStamp> allocations;
    CompactList<FlowSymbolConstant> symbolConstants;
    CompactList<FlowMemberKey> keysOfMembers;
    CompactList<ValueID> namesTakenFromOneKey;
    Vector<FlowDefinedValue> definedValues;
};
static_assert(sizeof(FlowBodyUpdate) <= 112);

template<typename Entry, typename Member>
const Entry* findByValue(std::span<const Entry> ascendingEntries, ValueID value, Member Entry::* member)
{
    auto found = std::ranges::lower_bound(ascendingEntries, value, { }, member);
    if (found == ascendingEntries.end() || (*found).*member != value)
        return nullptr;
    return &*found;
}

struct FlowAccessSiteNumbering {
    Vector<uint32_t> firstSiteOfFunction;
    Vector<ValueID> valueOfSite;

    uint32_t numberOfSites() const { return firstSiteOfFunction.isEmpty() ? 0 : firstSiteOfFunction.last(); }
    AccessSiteID siteOf(FunctionID functionAsRead, uint32_t ordinal) const { return AccessSiteID(firstSiteOfFunction[functionAsRead.index()] + ordinal); }
    uint32_t numberOfSitesOf(FunctionID functionAsRead) const { return firstSiteOfFunction[functionAsRead.index() + 1] - firstSiteOfFunction[functionAsRead.index()]; }
    AccessSiteID siteOfValue(FunctionID functionAsRead, ValueID value) const
    {
        uint32_t first = firstSiteOfFunction[functionAsRead.index()];
        std::span<const ValueID> values = valueOfSite.subspan(first, numberOfSitesOf(functionAsRead));
        auto found = std::ranges::lower_bound(values, value);
        return found != values.end() && *found == value ? AccessSiteID(first + static_cast<uint32_t>(found - values.begin())) : AccessSiteID();
    }
};

constexpr bool hasAccessSite(Opcode opcode) { return PayloadWord::accessSite(opcode).has_value(); }

void forEachAccessSiteValue(const Function& function, NOESCAPE const Invocable<void(ValueID, uint32_t ordinal)> auto& functor)
{
    uint32_t ordinal = 0;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (hasAccessSite(function.values[index].opcode))
            functor(ValueID(index), ordinal++);
    }
}

struct FlowApplication {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowApplication);

    uint32_t functionCountAsRead { 0 };
    FlowAccessSiteNumbering accessSites;
    Vector<FlowLayoutUpdate> layouts;
    Vector<FlowVariableUpdate> variables;
    Vector<FlowSignatureUpdate> signatures;
    Vector<FlowBodyUpdate> bodies;
    bool hasDictionaries { false };
    Vector<std::pair<TypeID, TypeID>> typesBehindDictionaries;
    Vector<std::pair<TypeID, TypeID>> typesTestedBehindDictionaries;
    BitVector typesNamingDictionaries;
    BitVector layoutSetsNamingDictionaries;
};

TypeID typeBehindDictionaries(std::span<const std::pair<TypeID, TypeID>> ascendingTypes, TypeID);

struct DictionaryTypeRewriter {
    TypeID typeBehind(TypeID id) const { return application.typesNamingDictionaries.get(id.index()) ? typeBehindDictionaries(application.typesBehindDictionaries.span(), id) : id; }
    TypeID typeTestedBehind(TypeID id) const { return application.typesNamingDictionaries.get(id.index()) ? typeBehindDictionaries(application.typesTestedBehindDictionaries.span(), id) : id; }
    void operator()(TypeID& id) const { id = typeBehind(id); }
    void operator()(auto&) const { }

    const FlowApplication& application;
};

LayoutID layoutOfPlainLiteral(const Program&, const Value&);

void remapTypes(FlowPositionUpdate&, const TypeRemap&);
void remapTypes(FlowLayoutUpdate&, const TypeRemap&);
void remapTypes(FlowSignatureUpdate&, const TypeRemap&);
void remapTypes(FlowBodyUpdate&, const TypeRemap&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
