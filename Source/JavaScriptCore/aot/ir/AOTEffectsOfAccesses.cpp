/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfAccesses.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTIR.h"
#include "ArrayConventions.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr Range arrayIndexes { 0, MAX_ARRAY_INDEX };

namespace {

class AccessFormBuilder {
public:
    AccessFormBuilder(const Program& program, PropertyKeyID key, LayoutSetID receivers)
        : m_program(program)
        , m_key(key)
        , m_receivers(receivers)
    {
    }

    void reads(const MemoryLocation& location) { m_effects.reads.add(m_program, location); }
    void writes(const MemoryLocation& location) { m_effects.writes.add(m_program, location); }
    void readsAtPayload(AbstractHeap heap) { reads(locationAt(heap)); }
    void writesAtPayload(AbstractHeap heap) { writes(locationAt(heap)); }
    void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    void throws() { m_effects.mayThrow = true; }
    void collects() { m_effects.mayCollect = true; }
    void suspends() { m_effects.maySuspend = true; }
    void terminates() { m_effects.mayBeTerminated = true; }
    void unknownCode() { m_effects.merge(Effects::unknownCode()); }

    const Effects& effects() const LIFETIME_BOUND { return m_effects; }

private:
    MemoryLocation locationAt(AbstractHeap heap) const
    {
        switch (heap) {
        case AbstractHeap::ObjectFields:
            return m_key ? MemoryLocation::objectField(m_key) : MemoryLocation(heap);
        case AbstractHeap::NamedProperties:
            return m_key ? MemoryLocation::namedProperty(m_key) : MemoryLocation(heap);
        case AbstractHeap::ShapeWords:
            return m_receivers ? MemoryLocation::shapeWords(m_receivers) : MemoryLocation(heap);
        case AbstractHeap::ModuleVariables:
        case AbstractHeap::EnvironmentFields:
        case AbstractHeap::OrderWords:
        case AbstractHeap::MemberGuards:
        case AbstractHeap::PrototypeLinks:
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
        case AbstractHeap::CalleesInFrames:
            return heap;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const Program& m_program;
    PropertyKeyID m_key;
    LayoutSetID m_receivers;
    Effects m_effects { Effects::none() };
};

} // anonymous namespace

Effects effectsOfAccessForm(const Program& program, Opcode operation, PropertyKeyID key, LayoutSetID receivers)
{
    AccessFormBuilder builder(program, key, receivers);
    forEachDeclaredEffect(operation, builder);
    return withDeclaredKindOfThrow(operation, builder.effects());
}

bool isArrayIndex(const Type& position)
{
    return position.isInteger() && position.integers.isSubsetOf(arrayIndexes);
}

static constexpr Effects effectsOfLookupByNumberName = [] {
    Effects effects = declaredBoundOf(NamedEffectSet::CreatesString);
    effects.reads.add(AbstractHeap::NamedProperties);
    return effects;
}();

static constexpr Effects effectsOfReadAtArrayIndex = [] {
    Effects effects = Effects::none();
    effects.reads.add(AbstractHeap::ArrayStorage);
    effects.reads.add(AbstractHeap::Elements);
    return effects;
}();

static constexpr Effects effectsOfStoreInsideLength = [] {
    Effects effects = Effects::none();
    effects.reads.add(AbstractHeap::ArrayStorage);
    effects.writes.add(AbstractHeap::Elements);
    return effects;
}();

static constexpr Effects effectsOfLookupByKey = [] {
    Effects effects = declaredBoundOf(NamedEffectSet::CreatesString);
    effects.merge(declaredBoundOf(NamedEffectSet::ReadsObjectGraph));
    effects.reads.add(AbstractHeap::PrototypeLinks);
    return effects;
}();

std::optional<Effects> effectsOfLookupFromBase(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::SuperGet || !value.key())
        return std::nullopt;
    const Type& base = program.typeOf(function, value.operand(OperandIndex::SuperGet::base));
    if (!provenLookupUnderAnyKey(program, base).isComplete)
        return std::nullopt;
    Effects effects = effectsOfLookupByKey;
    effects.mayThrow |= base.mayBeUndefinedOrNull();
    return effects;
}

static_assert(effectsOfLookupByNumberName.isSubsetOf(declaredBoundOf(Opcode::GetArrayElement)));
static_assert(effectsOfLookupByNumberName.isSubsetOf(declaredBoundOf(Opcode::HasArrayElement)));
static_assert(declaredBoundOf(Opcode::PutArrayElement).writes.contains(AbstractHeap::NamedProperties));

static constexpr Effects effectsOfDefinition = declaredBoundOf(Opcode::DefineGetter);
static_assert(!effectsOfDefinition.mayRunOtherCode());

static bool takesAnyNumberAsArrayPosition(Opcode operation)
{
    std::optional<unsigned> position = OperandPosition::position(operation);
    return position && OperandPosition::array(operation) && operandKindOf(operation, *position) == OperandKind::Number;
}

void refineElementEffects(Opcode operation, const Type& position, ElementPosition where, Effects& effects)
{
    if (!takesAnyNumberAsArrayPosition(operation) || (where == ElementPosition::Anywhere && !isArrayIndex(position)))
        return;

    switch (interpretedOf(operation)) {
    case Interpretation::Store:
        if (where == ElementPosition::InsideLength)
            effects = effects.intersectedWith(effectsOfStoreInsideLength);
        else
            effects.writes.remove(AbstractHeap::NamedProperties);
        return;
    case Interpretation::Read:
    case Interpretation::Test:
        effects = effects.intersectedWith(effectsOfReadAtArrayIndex);
        return;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Effects effectsAtPlacedKeys(const Program& program, const Value& value)
{
    Effects effects = Effects::none();
    for (PlacedKey placedKey : value.placedKeys())
        effects.merge(effectsOfAccessForm(program, placedKey.mayAdd ? Opcode::PutSlotOrAdd : Opcode::PutSlot, placedKey.key, value.receivers()));
    return effects;
}

static const Type* findComputedKeyType(const Program& program, const Function& function, const Value& value)
{
    std::optional<unsigned> property = OperandPosition::property(value.opcode);
    return property ? &program.typeOf(function, value.operand(*property)) : nullptr;
}

static void describeAccessOperands(const Program& program, const Function& function, const Value& value, EffectDescription& description)
{
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        describeOperandCode(program, program.typeOf(function, operand), declaredBehaviorOf(value.opcode, index), description);
    });
}

static Effects effectsOfChainWalk(const Program& program, PropertyKeyID key)
{
    Effects walk = Effects::none();
    walk.reads.add(AbstractHeap::PrototypeLinks);
    walk.reads.add(program, key ? MemoryLocation::namedProperty(key) : MemoryLocation(AbstractHeap::NamedProperties));
    walk.reads.add(program, key ? MemoryLocation::objectField(key) : MemoryLocation(AbstractHeap::ObjectFields));
    return walk;
}

static bool mayThrowOnReceiver(const Program& program, const Function& function, const Value& value, const Effects& declared)
{
    std::optional<uint8_t> receiver = receiverOf(value.opcode);
    if (!receiver)
        return declared.mayThrow;
    return program.typeOf(function, value.operand(*receiver)).mayBeUndefinedOrNull();
}

static void describeAccessors(const Program& program, const Value& value, std::span<const FunctionID> accessors, EffectDescription& description)
{
    if (accessors.empty())
        return;

    CalleeList callees = CalleeList::none();
    for (FunctionID accessor : accessors)
        callees.add(accessor);

    bool stores = interpretedOf(value.opcode) == Interpretation::Store;
    describeCallees(program, callees, stores ? BuiltinCallMode::Set : BuiltinCallMode::Get, description);
}

void describeAccess(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description, UntestedStorage untestedStorage)
{
    if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
        Effects effects = effectsAtPlacedKeys(program, value);
        ASSERT(effects.isSubsetOf(declared));
        describeBound(effects, description);
        describeAccessOperands(program, function, value, description);
        return;
    }
    // It answers or it does not, and runs nothing either way.
    if (value.opcode == Opcode::PropGet && value.asksOnlyTables()) {
        Effects lookup = effectsOfChainWalk(program, value.key());
        lookup.reads.add(AbstractHeap::ShapeWords);
        lookup.reads.add(AbstractHeap::RuntimeState);
        description.addOwn(lookup);
        return;
    }

    ProvenAccess storage;
    const ProvenAccess& proven = provenAccessOf(program, function, value, storage, untestedStorage);
    if (!proven.isComplete) {
        describeBound(declared, description);
        if (!declared.mayCallUnknownCode)
            describeAccessOperands(program, function, value, description);
        return;
    }

    if (declared.mayCollect) {
        Effects collects = Effects::none();
        collects.mayCollect = true;
        description.addOwn(collects);
    }

    const Type* computedKey = findComputedKeyType(program, function, value);
    for (Opcode operation : proven.operations) {
        Effects form = effectsOfAccessForm(program, operation, proven.key, proven.receivers);
        if (computedKey)
            refineElementEffects(operation, *computedKey, ElementPosition::Anywhere, form);
        ASSERT(declared.mayCallUnknownCode || (form.reads.isSubsetOf(declared.reads) && form.writes.isSubsetOf(declared.writes)));
        describeBound(form, description);
    }
    if (proven.isLookupByKey)
        description.addOwn(declared.mayCallUnknownCode ? effectsOfLookupByKey : effectsOfLookupByKey.intersectedWith(declared));
    if (proven.definesOwnProperty)
        description.addOwn(effectsOfDefinition);
    describeAccessors(program, value, proven.accessors.span(), description);
    if (!proven.methods.isEmpty() || !proven.builtins.isEmpty() || proven.mayFindNothing || proven.mayFindStoredValue) {
        Effects walk = effectsOfChainWalk(program, proven.key);
        description.addOwn(declared.mayCallUnknownCode ? walk : walk.intersectedWith(declared));
    }
    describeAccessOperands(program, function, value, description);
    if (mayThrowOnReceiver(program, function, value, declared))
        description.addThrow();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
