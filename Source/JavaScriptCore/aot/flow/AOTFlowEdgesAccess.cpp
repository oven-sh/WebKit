/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool hasBooleanResult(FlowAccessKind kind)
{
    return kind == FlowAccessKind::Test || kind == FlowAccessKind::Delete;
}

static FlowSlotAccess createContentsLoad(ValueID origin, FlowLocalID collection, ContentsSlot contentsSlot, FlowLocalID result)
{
    return {
        .kind = FlowAccessKind::Load,
        .origin = origin,
        .receiver = collection,
        .contentsSlot = contentsSlot,
        .value = result,
    };
}

FlowPropertyAccessShape FlowSummaryBuilder::propertyAccessShapeOf(const Value& value) const
{
    FlowPropertyAccessShape shape;

    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::GetOwnProperty:
        shape.kind = FlowAccessKind::Load;
        shape.receiver = value.objectOperand();
        break;
    case Opcode::GetByValWithThis:
        shape.kind = FlowAccessKind::Load;
        shape.receiver = value.objectOperand();
        shape.thisValue = value.thisValueOperand();
        break;
    case Opcode::PropSet:
    case Opcode::ElemSet:
        shape.kind = FlowAccessKind::Store;
        shape.receiver = value.objectOperand();
        shape.value = value.valueOperand();
        break;
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
        shape.kind = FlowAccessKind::Define;
        shape.receiver = value.objectOperand();
        shape.value = value.valueOperand();
        break;
    case Opcode::PropHas:
    case Opcode::ElemHas:
        shape.kind = FlowAccessKind::Test;
        shape.receiver = value.objectOperand();
        break;
    case Opcode::ScopeHasBinding:
        shape.kind = FlowAccessKind::Test;
        shape.receiver = value.scopeOperand();
        break;
    case Opcode::PropDelete:
    case Opcode::ElemDelete:
        shape.kind = FlowAccessKind::Delete;
        shape.receiver = value.objectOperand();
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    shape.flags.set(FlowAccessFlag::IsInCycle, isInCycle());
    shape.flags.set(FlowAccessFlag::IsOwnOnly, value.opcode == Opcode::GetOwnProperty);
    bool isStore = shape.kind == FlowAccessKind::Store || shape.kind == FlowAccessKind::Define;
    shape.flags.set(FlowAccessFlag::NamesReceivers, isStore && !shape.thisValue && Value::opcodesWithReceivers.contains(value.opcode));
    if (Value::opcodesWithIsFreeName.contains(value.opcode))
        shape.flags.set(FlowAccessFlag::IsFreeName, value.isFreeName());
    if (Value::opcodesWithIsForTypeOf.contains(value.opcode))
        shape.flags.set(FlowAccessFlag::IsForTypeOf, value.isForTypeOf());
    if (Value::opcodesWithMode.contains(value.opcode))
        shape.flags.set(FlowAccessFlag::IsSloppy, value.mode() == ECMAMode::Sloppy);
    return shape;
}

void FlowSummaryBuilder::addPropertyAccess(ValueID id, const Value& value)
{
    FlowPropertyAccessShape shape = propertyAccessShapeOf(value);
    addNamedAccess(id, shape, value.key());
    if (hasBooleanResult(shape.kind))
        seedTypeAsRead(id);

    if (value.opcode != Opcode::ScopeHasBinding)
        return;

    FlowLocalID unscopables = m_assembler.addTemporary();
    FlowLocalID unscopablesObject = m_assembler.addTemporary();
    FlowPropertyAccess load {
        .kind = FlowAccessKind::Load,
        .flags = shape.flags,
        .origin = id,
        .receiver = localOf(shape.receiver),
        .key = m_program.keyOf(BuiltinNames::unscopablesSymbol),
        .value = unscopables,
        .thrown = thrownTarget(),
    };
    m_assembler.add(load);
    filter(unscopables, unscopablesObject, TypeTable::object);

    load.receiver = unscopablesObject;
    load.key = value.key();
    load.value = m_assembler.addTemporary();
    m_assembler.add(load);
}

void FlowSummaryBuilder::addNamedAccess(ValueID origin, const FlowPropertyAccessShape& shape, PropertyKeyID key)
{
    bool isLoad = shape.kind == FlowAccessKind::Load;
    m_assembler.add(FlowPropertyAccess {
        .kind = shape.kind,
        .use = isLoad ? readUseOf(origin) : FlowReadUse::Value,
        .flags = shape.flags,
        .origin = origin,
        .receiver = localAtUse(shape.receiver),
        .key = key,
        .value = isLoad ? localOf(origin) : localAtUse(shape.value),
        .thisValue = localOf(shape.thisValue),
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addComputedAccess(ValueID origin, const FlowPropertyAccessShape& shape, ValueID key)
{
    bool isLoad = shape.kind == FlowAccessKind::Load;
    m_assembler.add(FlowElementAccess {
        .kind = shape.kind,
        .use = isLoad ? readUseOf(origin) : FlowReadUse::Value,
        .flags = shape.flags,
        .origin = origin,
        .receiver = localAtUse(shape.receiver),
        .key = localOf(key),
        .value = isLoad ? localOf(origin) : localAtUse(shape.value),
        .thisValue = localOf(shape.thisValue),
        .thrown = thrownTarget(),
    });
}

bool FlowSummaryBuilder::isLengthOf(ValueID array, ValueID value) const
{
    const Value& read = m_function.at(m_identities->identityOf(value));
    return read.opcode == Opcode::PropGet && !read.isFreeName() && read.key() == m_program.keyOf(BuiltinNames::length) && m_identities->identityOf(read.objectOperand()) == array;
}

// A key that is no number compares false, and names no index.
bool FlowSummaryBuilder::findsKeyWithinLength(Edge edge, ValueID array, ValueID key) const
{
    const Terminator& branch = m_function.at(edge.from).terminator;
    if (edge.isHandlerEdge() || branch.kind != TerminatorKind::Branch)
        return false;

    const Value& comparison = m_function.at(branch.conditionOperand());
    bool isTaken = !edge.targetIndex;
    bool keyIsOnLeft;
    switch (comparison.opcode) {
    case Opcode::Less:
    case Opcode::LessEq:
        keyIsOnLeft = isTaken;
        break;
    case Opcode::Greater:
    case Opcode::GreaterEq:
        keyIsOnLeft = !isTaken;
        break;
    default:
        return false;
    }
    ValueID smaller = comparison.operand(keyIsOnLeft ? 0 : 1);
    ValueID larger = comparison.operand(keyIsOnLeft ? 1 : 0);
    return m_identities->identityOf(smaller) == key && isLengthOf(array, larger);
}

// An array that never gets shorter has every index that was once below its length.
bool FlowSummaryBuilder::isWithinLengthOf(ValueID receiver, ValueID key)
{
    if (!m_identities)
        m_identities = makeUnique<ValueIdentities>(m_function, m_cfg);
    ValueID array = m_identities->identityOf(receiver);
    if (isLengthOf(array, key))
        return true;

    if (!m_dominators)
        m_dominators = makeUnique<Dominators>(m_cfg, false);
    ValueID comparedKey = m_identities->identityOf(key);
    for (BlockID block = m_block; block != entryBlock; block = m_dominators->idom(block)) {
        auto edges = m_cfg.incomingEdges(block);
        if (edges.size() == 1 && findsKeyWithinLength(edges[0], array, comparedKey))
            return true;
    }
    return false;
}

void FlowSummaryBuilder::addElementAccess(ValueID id, const Value& value)
{
    FlowPropertyAccessShape shape = propertyAccessShapeOf(value);
    if (shape.kind == FlowAccessKind::Store || shape.kind == FlowAccessKind::Define)
        shape.flags.set(FlowAccessFlag::IsWithinLength, isWithinLengthOf(shape.receiver, value.propertyOperand()));
    addComputedAccess(id, shape, value.propertyOperand());
    if (hasBooleanResult(shape.kind))
        seedTypeAsRead(id);
}

void FlowSummaryBuilder::addAccessorDefinition(ValueID id, const Value& value)
{
    FlowPropertyAccessShape shape;
    shape.kind = value.opcode == Opcode::DefineGetter ? FlowAccessKind::DefineGetter : FlowAccessKind::DefineSetter;
    shape.flags.set(FlowAccessFlag::IsInCycle, isInCycle());
    shape.receiver = value.objectOperand();
    shape.value = value.functionOperand();

    if (PropertyKeyID key = value.key()) {
        addNamedAccess(id, shape, key);
        return;
    }
    addComputedAccess(id, shape, value.computedKeyOperand());
}

void FlowSummaryBuilder::addSetFunctionName(ValueID id, const Value& value)
{
    FlowLocalID name = m_assembler.addTemporary();
    m_assembler.seed(FlowSeed::typeAsRead(name, TypeTable::string));

    OptionSet<FlowAccessFlag> flags;
    flags.set(FlowAccessFlag::IsInCycle, isInCycle());
    m_assembler.add(FlowPropertyAccess {
        .kind = FlowAccessKind::Define,
        .flags = flags,
        .origin = id,
        .receiver = localOf(value.functionOperand()),
        .key = m_program.keyOf(BuiltinNames::name),
        .value = name,
        .thrown = thrownTarget(),
    });
}

void FlowSummaryBuilder::addSetLiteralPrototype(ValueID id, const Value& value)
{
    m_assembler.add(FlowPrototypeAccess { FlowAccessKind::Store, id, localOf(value.objectOperand()), localOf(value.prototypeOperand()) });
}

void FlowSummaryBuilder::addCopyDataProperties(ValueID id, const Value& value)
{
    m_assembler.add(FlowPropertyCopy {
        id,
        localOf(value.sourceOperand()),
        localOf(value.targetOperand()),
        m_assembler.addList(localsOf(value.excludedOperands()).span()),
        thrownTarget(),
    });
}

void FlowSummaryBuilder::addGetSuperBase(ValueID id, const Value& value)
{
    FlowLocalID home;
    switch (value.homeKind()) {
    case HomeObjectKind::Object:
        home = localOf(value.homeOperand());
        break;
    case HomeObjectKind::ClassPrototype:
        home = m_assembler.addTemporary();
        addPrototypePropertyRead(id, value.homeOperand(), home);
        break;
    }
    m_assembler.add(FlowPrototypeAccess { FlowAccessKind::Load, id, home, localOf(id) });
}

void FlowSummaryBuilder::addSuperAccess(ValueID id, const Value& value)
{
    FlowPropertyAccessShape shape;
    shape.flags.set(FlowAccessFlag::IsInCycle, isInCycle());
    shape.receiver = value.baseOperand();
    shape.thisValue = value.receiverOperand();

    switch (value.opcode) {
    case Opcode::SuperGet:
        shape.kind = FlowAccessKind::Load;
        break;
    case Opcode::SuperSet:
        shape.kind = FlowAccessKind::Store;
        shape.flags.set(FlowAccessFlag::IsSloppy, value.mode() == ECMAMode::Sloppy);
        shape.value = value.valueOperand();
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (PropertyKeyID key = value.key()) {
        addNamedAccess(id, shape, key);
        return;
    }
    addComputedAccess(id, shape, value.computedKeyOperand());
}

void FlowSummaryBuilder::addPrivateAccess(ValueID id, const Value& value)
{
    FlowFieldAccess access {
        .isInCycle = isInCycle(),
        .origin = id,
        .receiver = localOf(value.objectOperand()),
        .layout = value.layout(),
        .field = value.field(),
        .thrown = thrownTarget(),
    };

    switch (value.opcode) {
    case Opcode::PrivateGet:
        access.kind = FlowAccessKind::Load;
        access.use = readUseOf(id);
        access.value = localOf(id);
        break;
    case Opcode::PrivateSet:
        access.kind = FlowAccessKind::Store;
        access.value = localOf(value.valueOperand());
        break;
    case Opcode::PrivateDefine:
        access.kind = FlowAccessKind::Define;
        access.value = localOf(value.valueOperand());
        break;
    case Opcode::PrivateHas:
        access.kind = FlowAccessKind::Test;
        seedTypeAsRead(id);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_assembler.add(access);
}

void FlowSummaryBuilder::addInternalFieldAccess(ValueID id, const Value& value)
{
    FlowSlotAccess access {
        .origin = id,
        .receiver = localOf(value.objectOperand()),
        .internalFieldIndex = value.index(),
    };

    if (value.opcode == Opcode::GetInternalField) {
        access.kind = FlowAccessKind::Load;
        access.value = localOf(id);
        m_assembler.add(access);
        return;
    }
    RELEASE_ASSERT(value.opcode == Opcode::PutInternalField);
    access.kind = FlowAccessKind::Store;
    access.value = localOf(value.valueOperand());
    m_assembler.add(access);
    m_assembler.copy(access.value, localOf(id));
}

void FlowSummaryBuilder::addElementSlotAccess(ValueID id, const Value& value)
{
    FlowSlotAccess access {
        .origin = id,
        .receiver = localOf(value.arrayOperand()),
        .contentsSlot = ContentsSlot::Element,
    };

    switch (value.opcode) {
    case Opcode::GetContiguousIndex:
        access.kind = FlowAccessKind::Load;
        access.value = localOf(id);
        break;
    case Opcode::TryPutIndex:
        // It takes any index that the storage has room for.
        access.mayLeaveHole = true;
        [[fallthrough]];
    case Opcode::TryPutByValDirect:
        seedTypeAsRead(id);
        [[fallthrough]];
    case Opcode::PutContiguousIndex:
    case Opcode::PutDoubleIndex:
        access.kind = FlowAccessKind::Store;
        access.value = localOf(value.valueOperand());
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_assembler.add(access);
}

void FlowSummaryBuilder::addCollectionNext(ValueID id, const Value& value)
{
    BitVector coveredOperands;
    // The step that finds no entry answers undefined.
    auto addContentsResult = [&](FlowLocalID collection, ContentsSlot slot, FlowLocalID result) {
        m_assembler.add(createContentsLoad(id, collection, slot, result));
        seedPrimitives(result, Primitive::Undefined);
    };

    if (value.opcode == Opcode::SetNext) {
        FlowLocalID set = localOf(value.setOperand());
        const Vector<FlowLocalID, 4>& results = resultLocalsOf(id, numberOfEnumerators<SetNextResult>);
        addContentsResult(set, ContentsSlot::Element, results[static_cast<uint32_t>(SetNextResult::Element)]);
        coveredOperands.set(OperandIndex::SetNext::set);
    } else {
        RELEASE_ASSERT(value.opcode == Opcode::MapNext);
        FlowLocalID map = localOf(value.mapOperand());
        const Vector<FlowLocalID, 4>& results = resultLocalsOf(id, numberOfEnumerators<MapNextResult>);
        addContentsResult(map, ContentsSlot::Key, results[static_cast<uint32_t>(MapNextResult::Key)]);
        addContentsResult(map, ContentsSlot::Value, results[static_cast<uint32_t>(MapNextResult::Value)]);
        coveredOperands.set(OperandIndex::MapNext::map);
    }
    addOperandUsesFromSchema(id, value, coveredOperands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
