/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMemoryLocation.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFamilyTables.h"
#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

class SingleAccessCollector {
public:
    void reads(const MemoryLocation& location) { collect(read, location); }
    void writes(const MemoryLocation& location) { collect(written, location); }
    void readsAllHeaps() { isDisqualified = true; }
    void throws() { isDisqualified = true; }
    void collects() { isDisqualified = true; }
    void suspends() { isDisqualified = true; }
    void terminates() { isDisqualified = true; }
    void unknownCode() { isDisqualified = true; }

    std::optional<MemoryLocation> read;
    std::optional<MemoryLocation> written;
    bool isDisqualified { false };

private:
    void collect(std::optional<MemoryLocation>& access, const MemoryLocation& location)
    {
        if (access || location.isEntireHeap()) {
            isDisqualified = true;
            return;
        }
        access = location;
    }
};

enum class CreationStore : uint8_t { NotCounted, Counted };

} // anonymous namespace

static std::optional<uint32_t> findStoredOperand(const Value& value)
{
    if (std::optional<uint8_t> stored = storedOf(value.opcode))
        return *stored;

    std::optional<uint32_t> retained;
    for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
        if (!operandUsesOf(value.opcode, index).contains(OperandUse::Retains))
            continue;
        if (retained)
            return std::nullopt;
        retained = index;
    }
    return retained;
}

static std::optional<uint32_t> findBaseOperand(const Value& value, std::optional<uint32_t> storedOperand)
{
    if (std::optional<uint8_t> receiver = receiverOf(value.opcode))
        return *receiver;

    if (storedOperand) {
        if (retainedInOf(value.opcode, *storedOperand) != RetainedIn::Operand)
            return std::nullopt;
        return retainingOperandOf(value.opcode, *storedOperand);
    }

    std::optional<uint32_t> base;
    for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
        if (operandKindOf(value.opcode, index) == OperandKind::Guard || !value.operand(index))
            continue;
        if (base)
            return std::nullopt;
        base = index;
    }
    return base;
}

MemoryAccess memoryAccessOf(const Function&, const Value& value)
{
    if (effectRuleOf(value.opcode) != EffectRule::Declared)
        return { };
    if (value.opcode == Opcode::GetSlotSetter)
        return { };

    SingleAccessCollector collector;
    forEachDeclaredEffect(value, collector);
    if (collector.isDisqualified || collector.read.has_value() == collector.written.has_value())
        return { };
    if (mayRunOperandCode(value.opcode))
        return { };

    bool isRead = collector.read.has_value();
    MemoryLocation location = isRead ? *collector.read : *collector.written;
    if (!heapsHoldingValues.contains(location.heap()) || !hasSingleValueLocations(location.heap()))
        return { };

    std::optional<uint32_t> storedOperand;
    ValueID stored;
    if (isRead) {
        if (resultRuleOf(value.opcode) != ResultRule::Declared)
            return { };
    } else {
        storedOperand = findStoredOperand(value);
        if (!storedOperand)
            return { };
        stored = value.operand(*storedOperand);
        if (!stored)
            return { };
    }

    ValueID base;
    if (perObjectHeaps.contains(location.heap())) {
        std::optional<uint32_t> baseOperand = findBaseOperand(value, storedOperand);
        if (!baseOperand)
            return { };
        base = value.operand(*baseOperand);
        if (!base)
            return { };
    }

    return { isRead ? MemoryAccess::Kind::Read : MemoryAccess::Kind::Write, location, base, stored };
}

static LocationStability stabilityOfFlow(const LocationFlow& flow, CreationStore creationStore)
{
    LocationStability fixed = flow.isUnchangedDuringActivation ? LocationStability::UnchangedDuringActivation : LocationStability::ChangedByOwnStores;
    switch (flow.stores) {
    case StoreCount::Unknown:
        return LocationStability::Unknown;
    case StoreCount::Never:
        return flow.initialization != InitializationState::Unknown ? fixed : LocationStability::Unknown;
    case StoreCount::AtMostOnce:
        break;
    }
    switch (flow.initialization) {
    case InitializationState::Unknown:
        return LocationStability::Unknown;
    case InitializationState::StoredBeforeEveryRead:
        return fixed;
    case InitializationState::SetAtCreation:
        return creationStore == CreationStore::Counted ? fixed : LocationStability::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LocationStability weakerOf(LocationStability a, LocationStability b)
{
    switch (a) {
    case LocationStability::Unknown:
        return LocationStability::Unknown;
    case LocationStability::ChangedByOwnStores:
        return b == LocationStability::Unknown ? LocationStability::Unknown : LocationStability::ChangedByOwnStores;
    case LocationStability::UnchangedDuringActivation:
        return b;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LocationStability stabilityOfField(const Program& program, FieldReference field)
{
    return stabilityOfFlow(flowOf(program, field).location, CreationStore::Counted);
}

static LocationStability stabilityOfObjectField(const Program& program, const Value& access, PropertyKeyID key)
{
    if (!Value::opcodesWithReceivers.contains(access.opcode) || !access.receivers())
        return LocationStability::Unknown;

    const LayoutSet& receivers = program.at(access.receivers());
    if (!receivers.isComplete || receivers.members.isEmpty())
        return LocationStability::Unknown;

    const FamilyMember& member = program.familyTables().memberOf(access.receivers(), key);
    if (member.withoutField && !program.at(member.withoutField).members.isEmpty())
        return LocationStability::Unknown;
    if (member.hasOneDeclaration)
        return stabilityOfField(program, member.declaration);

    LocationStability stability = LocationStability::UnchangedDuringActivation;
    for (LayoutID receiver : receivers.members) {
        std::optional<uint32_t> field = program.at(receiver).findField(key);
        stability = weakerOf(stability, field ? stabilityOfField(program, { receiver, *field }) : LocationStability::Unknown);
        if (stability == LocationStability::Unknown)
            break;
    }
    return stability;
}

LocationStability stabilityOf(const Program& program, const Value& access, const MemoryAccess& memoryAccess)
{
    const MemoryLocation& location = memoryAccess.location;
    if (!memoryAccess || location.isEntireHeap())
        return LocationStability::Unknown;

    switch (location.heap()) {
    case AbstractHeap::ModuleVariables:
        return stabilityOfFlow(program.at(location.variable()).flow, CreationStore::NotCounted);
    case AbstractHeap::EnvironmentFields: {
        FieldReference reference { location.environmentLayout(), location.environmentFieldIndex() };
        const LocationFlow& flow = flowOf(program, reference).location;
        if (fieldAt(program, reference).flags.contains(FieldFlags::IsImmutable) && flow.initialization == InitializationState::StoredBeforeEveryRead)
            return LocationStability::ChangedByOwnStores;
        return stabilityOfFlow(flow, CreationStore::Counted);
    }
    case AbstractHeap::ObjectFields:
        return stabilityOfObjectField(program, access, location.key());
    case AbstractHeap::ShapeWords:
    case AbstractHeap::OrderWords:
    case AbstractHeap::MemberGuards:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::NamedProperties:
    case AbstractHeap::Elements:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::ValidationState:
        return LocationStability::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
