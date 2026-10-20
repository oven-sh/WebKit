/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTSchemaOperandIndexes.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

ValueSetID HeapRule::kinds(uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return m_context.maker().of(members.span());
}

void HeapRule::connect(ValueFlowNodeID from, ValueFlowNodeID to)
{
    if (from && to)
        m_context.connect(from, to);
}

void HeapRule::connect(const ValueFlowOperand& from, ValueFlowNodeID to)
{
    if (to)
        m_context.connect(from, to);
}

void HeapRule::connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to)
{
    ValueFlowOperand masked = from;
    masked.mask = from.mask ? m_context.maker().intersect(from.mask, mask) : mask;
    connect(masked, to);
}

void HeapRule::add(ValueFlowNodeID to, ValueSetID constant)
{
    if (to)
        m_context.add(to, constant);
}

void HeapRule::add(ValueFlowNodeID to, ClimbingSet set)
{
    if (to)
        m_context.add(to, set);
}

void HeapRule::pass(const HeapReceiver& receiver, ValueFlowNodeID to)
{
    if (receiver.operand)
        connect(*receiver.operand, to);
    else
        add(to, receiver.set);
}

HeapKey HeapRule::keyNamed(PropertyKeyID name) const
{
    if (!name)
        return unknownHeapKey;
    return m_program.isIndexLike(name) ? indexHeapKey : HeapKey { HeapKeyClass::Name, name };
}

void HeapRule::read(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    if (m_accessorFunctor) {
        readThroughChain(startOfLookup(receivers), key, { &object, { } }, m_statement.result);
        if (mayBeUnknownObject(receivers))
            readThroughChain(unknownObjectAlone(), key, { &object, { } }, m_statement.result);
        return;
    }

    ValueFlowName name = foundName(key, HeapPropertyPart::Value);
    connect(m_context.lookup(startOfLookup(receivers), name), m_statement.result);
    if (mayBeUnknownObject(receivers))
        connect(m_context.lookup(unknownObjectAlone(), name), m_statement.result);
    mayThrow();
}

// What lies in the object itself, where no accessor can stand and nothing is inherited.
void HeapRule::readOwn(ClimbingSet holders, ValueFlowName name, ValueFlowNodeID result)
{
    ClimbingSet objects = known(holders);
    if (name == prototypeLinkName)
        objects = m_context.restrict(objects, ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    connect(m_context.read(objects, name), result);
    connect(storedThroughUnknownObjectInto(objects, name), result);
    if (!mayBeUnknownObject(holders))
        return;
    add(result, unknownValue());
    if (name == prototypeLinkName)
        letPrototypeObjectsOfTableEscape();
}

void HeapRule::test(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    HeapChain chain = chainFrom(receivers);
    auto noteTest = [&](BuiltinID member, ModuleID) {
        use(member, BuiltinUse::Tested);
    };
    forEachMemberUnder(chain.objects, key, noteTest);
    if (chain.holdsUnknownObject) {
        forEachMemberUnder(holdersFoundThroughUnknownObject(key), key, noteTest);
        noteTrapOfUnknownObject(unknownObjectAlone(), ValueFlowCallKind::Get);
    }
    answerBoolean();
    mayThrow();
}

// What is stored through the unknown object lies in one node for each name. A lookup takes it from there if an object on its chain has escaped.
void HeapRule::write(ClimbingSet objects, ValueFlowName name, const ValueFlowOperand& value)
{
    connect(value, m_context.written(known(objects), name));
    // Native code reaches the built-in objects of the realm without being handed them, and it reads and calls what the program puts there.
    bool writesIntoRealm = false;
    m_context.forEachMember(m_context.restrict(objects, m_universe.builtinObjects()), [&](ValueFlowObjectID object) {
        writesIntoRealm |= m_universe.kindOf(object) == ValueFlowObjectKind::Builtin;
    });
    if (writesIntoRealm)
        connect(value, ValueFlowProgramNodes::escaped());
    if (!mayBeUnknownObject(objects))
        return;
    connect(value, m_context.written(unknownObjectAlone(), name));
    connect(value, ValueFlowProgramNodes::escaped());
}

// Nearly every store goes into objects of the program alone, and then there is nothing to note.
void HeapRule::noteWrittenMembers(ClimbingSet receivers, const HeapKey& key)
{
    if (!m_context.meet(receivers, m_tables.builtinObjectsAndUnknownObject()))
        return;

    ValueSetID holders = ValueSets::nothing;
    ValueFlowName name = ValueFlowName::of(ValueFlowNameKind::EveryMemberWritten);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.under(key.name).holders;
        name = { ValueFlowNameKind::MemberWritten, key.name.index() };
        break;
    case HeapKeyClass::Index:
        break;
    case HeapKeyClass::Unknown:
        holders = m_tables.holdersWithMembers();
        break;
    }
    if (holders != ValueSets::nothing) {
        ClimbingSet written = m_context.restrict(receivers, m_context.maker().unite(holders, m_universe.unknownObjectAlone()));
        if (holdsAnything(written))
            m_context.lookup(written, name);
    }
    applyEventsOfStore(m_statement, m_context, receivers, key.keyClass == HeapKeyClass::Name ? key.name : PropertyKeyID());
}

// A store under an index adds no name to an array, and neither does a store under a name that the layout of the object declares.
void HeapRule::markStore(ClimbingSet receivers, const HeapKey& key, OptionSet<ObjectMutation> otherMarks)
{
    ClimbingSet objects = m_context.restrict(receivers, m_universe.objects());
    m_context.mark(objects, otherMarks | ObjectMutation::StoreAfterCreation);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        objects = m_context.restrict(objects, ValueSets::complement(m_tables.objectsDeclaringField(key.name)));
        break;
    case HeapKeyClass::Index:
        objects = m_context.restrict(objects, ValueSets::complement(m_universe.sureInstancesOf(BuiltinClasses::Array)));
        break;
    case HeapKeyClass::Unknown:
        break;
    }
    if (holdsAnything(objects))
        m_context.mark(objects, ObjectMutation::AddProperty);
}

// A value that is defined where a value lies leaves the property what it was. It makes another kind of property of an accessor, and so does an accessor of whatever was there.
void HeapRule::markDefinition(ClimbingSet receivers, const HeapKey& key, HeapPropertyPart part)
{
    ClimbingSet objects = known(receivers);
    auto mayHave = [&](HeapPropertyPart accessor) {
        ValueFlowName name = foundName(key, accessor);
        return holdsAnything(m_context.valueOf(m_context.read(objects, name))) || storedThroughUnknownObjectInto(objects, name);
    };

    bool redefines = part != HeapPropertyPart::Value || mayBeUnknownObject(receivers) || mayHave(HeapPropertyPart::Getter) || mayHave(HeapPropertyPart::Setter);
    markStore(receivers, key, redefines ? OptionSet<ObjectMutation> { ObjectMutation::RedefineProperty } : OptionSet<ObjectMutation> { });
}

void HeapRule::define(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part, const ValueFlowOperand& value)
{
    ClimbingSet receivers = m_context.valueOf(object);
    write(receivers, writtenName(key, part), value);
    if (part == HeapPropertyPart::Value)
        convertStoredElement(receivers, key, value);
    noteWrittenMembers(receivers, key);
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    markDefinition(receivers, key, part);
    mayThrow();
}

// A typed array keeps a number, which code that the stored object names makes of it.
void HeapRule::convertStoredElement(ClimbingSet receivers, const HeapKey& key, const ValueFlowOperand& value)
{
    if (key.keyClass != HeapKeyClass::Name && m_context.meet(chainFrom(receivers).objects, m_tables.prototypesOfTypedArrays()))
        connect(value, m_context.hub(m_universe.builtinTables().conversionToPrimitive()));
}

void HeapRule::callSetters(ClimbingSet receivers, const HeapKey& key, const ValueFlowOperand& receiver, const ValueFlowOperand& value)
{
    ValueFlowName name = foundName(key, HeapPropertyPart::Setter);
    ClimbingSet setters = m_context.valueOf(m_context.lookup(startOfLookup(receivers), name));
    if (mayBeUnknownObject(receivers))
        setters = m_context.unite(setters, m_context.valueOf(m_context.lookup(unknownObjectAlone(), name)));
    callAccessors(setters, ValueFlowCallKind::Set, { &receiver, { } }, &value, { });
}

// As for a read: only a receiver on whose own chain a setter nobody knows may lie is its `this`.
void HeapRule::callSettersOfReceivers(ClimbingSet start, ValueFlowName name, const ValueFlowOperand& value)
{
    ClimbingSet setters = m_context.valueOf(m_context.lookup(start, name));
    ClimbingSet first;
    ClimbingSet second;
    if (mayBeAccessorNobodyKnows(setters) && splitInTwo(start, first, second)) {
        callSettersOfReceivers(first, name, value);
        callSettersOfReceivers(second, name, value);
        return;
    }
    callAccessors(setters, ValueFlowCallKind::Set, { nullptr, start }, &value, { });
}

void HeapRule::store(const ValueFlowOperand& object, const HeapKey& key, const ValueFlowOperand& value)
{
    ClimbingSet receivers = m_context.valueOf(object);
    write(receivers, writtenName(key, HeapPropertyPart::Value), value);
    convertStoredElement(receivers, key, value);
    noteWrittenMembers(receivers, key);
    markStore(receivers, key, { });
    ValueFlowName setter = foundName(key, HeapPropertyPart::Setter);
    callSettersOfReceivers(startOfLookup(receivers), setter, value);
    if (mayBeUnknownObject(receivers))
        callSettersOfReceivers(unknownObjectAlone(), setter, value);
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    mayThrow();
}

// A lookup that starts at a built-in holder asks for this.
void HeapRule::noteDeletedMembers(ClimbingSet receivers, const HeapKey& key)
{
    ValueSetID holders = ValueSets::nothing;
    ValueFlowName name = ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.under(key.name).holders;
        name = { ValueFlowNameKind::DeletedMember, key.name.index() };
        break;
    case HeapKeyClass::Index:
        return;
    case HeapKeyClass::Unknown:
        holders = m_tables.holdersWithMembers();
        break;
    }
    if (holders == ValueSets::nothing)
        return;
    ClimbingSet deletedFrom = m_context.restrict(receivers, holders);
    if (holdsAnything(deletedFrom))
        add(m_context.written(deletedFrom, name), m_universe.undefinedValue());
    if (mayBeUnknownObject(receivers))
        add(m_context.written(unknownObjectAlone(), name), m_universe.undefinedValue());
}

void HeapRule::remove(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    noteWrittenMembers(receivers, key);
    noteDeletedMembers(receivers, key);
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    m_context.mark(m_context.restrict(receivers, m_universe.objects()), ObjectMutation::DeleteField);
    answerBoolean();
    mayThrow();
}

void HeapRule::runNewObject()
{
    const Value& allocation = operation();
    LayoutID layoutID = allocation.layout();
    const Layout& layout = m_program.at(layoutID);
    ClimbingSet object(only(m_universe.siteObject(m_statement)));

    const ValueFlowOperand& prototype = operand(OperandIndex::NewObject::prototype);
    if (isPassed(prototype))
        connect(prototype, m_context.written(object, prototypeLinkName));

    std::span<const uint32_t> fields = allocation.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        const ValueFlowOperand& value = operand(OperandIndex::NewObject::values + index);
        const Field& field = layout.fields[fields[index]];
        if (field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden }))
            connect(value, field.originLayout ? m_context.nodes().field(field.originLayout, *field.originField) : m_context.nodes().field(layoutID, fields[index]));
        else
            connect(value, m_context.written(object, writtenName(keyNamed(field.key), HeapPropertyPart::Value)));
    }
}

void HeapRule::runNewArray()
{
    ClimbingSet array(only(m_universe.siteObject(m_statement)));
    ValueFlowNodeID contents = m_context.written(array, elementsName);
    for (const ValueFlowOperand& element : m_operands.subspan(OperandIndex::NewArray::elements))
        connect(element, contents);
}

void HeapRule::runCreateArguments(const ValueFlowOperand& count, const ValueFlowOperand& rest, std::span<const ValueFlowOperand> parameters, const ValueFlowOperand* callee)
{
    ClimbingSet arguments(only(m_universe.siteObject(m_statement)));
    ValueFlowNodeID contents = m_context.written(arguments, elementsName);
    readOwn(rest, elementsName, contents);
    for (const ValueFlowOperand& parameter : parameters)
        connect(parameter, contents);

    auto propertyNamed = [&](BuiltinName name) {
        return m_context.written(arguments, ValueFlowName::property(m_program.keyOf(name)));
    };
    connect(count, propertyNamed(BuiltinNames::length));
    if (callee)
        connect(*callee, propertyNamed(BuiltinNames::callee));
    BuiltinID iteratorMethod = m_builtins.findOwnMember(m_builtins.at(BuiltinClasses::Array).prototype, BuiltinNames::iteratorSymbol);
    add(propertyNamed(BuiltinNames::iteratorSymbol), iteratorMethod ? only(m_universe.objectOf(iteratorMethod)) : m_universe.unknownObjectAlone());
}

void HeapRule::runData()
{
    struct Pending {
        const DataNode* node;
        ValueFlowNodeID holder;
    };
    // The sites are numbered in preorder, and a primitive takes a number too.
    Vector<Pending, 32> pending { Pending { &m_program.at(operation().data()).root, { } } };
    for (uint32_t ordinal = 0; !pending.isEmpty(); ++ordinal) {
        auto [node, holder] = pending.takeLast();
        switch (node->kind()) {
        case DataNode::Kind::Array: {
            ClimbingSet array(only(m_universe.siteObject(m_statement, ordinal)));
            add(holder, array);
            ValueFlowNodeID contents = m_context.written(array, elementsName);
            const Vector<DataNode>& items = node->as<DataNode::Array>().elements;
            for (size_t index = items.size(); index--;)
                pending.append({ &items[index], contents });
            break;
        }
        case DataNode::Kind::Object: {
            ClimbingSet object(only(m_universe.siteObject(m_statement, ordinal)));
            add(holder, object);
            const Vector<DataMember>& members = node->as<DataNode::Object>().members;
            for (size_t index = members.size(); index--;)
                pending.append({ &members[index].value, m_context.written(object, writtenName(keyNamed(members[index].key), HeapPropertyPart::Value)) });
            break;
        }
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Hole:
            add(holder, m_universe.undefinedValue());
            break;
        case DataNode::Kind::Null:
            add(holder, kinds(Primitive::Null));
            break;
        case DataNode::Kind::Boolean:
            add(holder, kinds(Primitive::Boolean));
            break;
        case DataNode::Kind::Number:
            add(holder, kinds(Primitive::Number));
            break;
        case DataNode::Kind::String:
            add(holder, kinds(Primitive::String));
            break;
        case DataNode::Kind::BigInt:
            add(holder, kinds(Primitive::BigInt));
            break;
        }
    }
}

// The result is the array, whether or not the bodies take it for an allocation of its own.
void HeapRule::runTruncateNewArray()
{
    const ValueFlowOperand& array = operand(OperandIndex::TruncateNewArray::array);
    connect(array, m_statement.result);
    if (ValueFlowObjectID site = m_universe.siteObject(m_statement))
        readOwn(array, elementsName, m_context.written(ClimbingSet(only(site)), elementsName));
}

void HeapRule::runCreateIteratorResult()
{
    ClimbingSet result(only(m_universe.siteObject(m_statement)));
    connect(operand(OperandIndex::CreateIteratorResult::value), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::value))));
    connect(operand(OperandIndex::CreateIteratorResult::done), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::done))));
}

void HeapRule::runGetByValWithThis()
{
    ClimbingSet holders = m_context.valueOf(operand(OperandIndex::GetByValWithThis::object));
    HeapReceiver receiver { &operand(OperandIndex::GetByValWithThis::thisValue), { } };
    forEachKey(operand(OperandIndex::GetByValWithThis::property), [&](const HeapKey& key) {
        readThroughChain(startOfLookup(holders), key, receiver, m_statement.result);
        if (mayBeUnknownObject(holders))
            readThroughChain(unknownObjectAlone(), key, receiver, m_statement.result);
    });
    mayThrow();
}

// `with (scope)`: whether the scope has the name, and whether the object under Symbol.unscopables hides it.
void HeapRule::runScopeHasBinding()
{
    const ValueFlowOperand& scope = operand(OperandIndex::ScopeHasBinding::scope);
    HeapKey key = keyNamed(operation().key());
    test(scope, key);
    ValueFlowNodeID unscopables = m_context.lookup(m_context.valueOf(scope), ValueFlowName::property(m_program.keyOf(BuiltinNames::unscopablesSymbol)));
    m_context.lookup(m_context.valueOf(unscopables), foundName(key, HeapPropertyPart::Value));
}

void HeapRule::runGetSuperBase()
{
    ClimbingSet homes = m_context.valueOf(operand(OperandIndex::GetSuperBase::home));
    // The operand is the class, and the home object of a method is what the class holds under `prototype`.
    if (operation().homeKind() == HomeObjectKind::ClassPrototype)
        homes = m_context.valueOf(m_context.lookup(homes, ValueFlowName::property(m_program.keyOf(BuiltinNames::prototype))));
    readOwn(homes, prototypeLinkName, m_statement.result);
    add(m_statement.result, prototypesAtCreationOf(homes));
    add(m_statement.result, kinds(Primitive::Null));
}

void HeapRule::runSuperGet()
{
    ClimbingSet bases = m_context.valueOf(operand(OperandIndex::SuperGet::base));
    HeapReceiver receiver { &operand(OperandIndex::SuperGet::receiver), { } };
    forEachKeyOfOperation(OperandIndex::SuperGet::computedKey, [&](const HeapKey& key) {
        readThroughChain(startOfLookup(bases), key, receiver, m_statement.result);
        if (mayBeUnknownObject(bases))
            readThroughChain(unknownObjectAlone(), key, receiver, m_statement.result);
    });
    mayThrow();
}

void HeapRule::runSuperSet()
{
    ClimbingSet bases = m_context.valueOf(operand(OperandIndex::SuperSet::base));
    const ValueFlowOperand& receiver = operand(OperandIndex::SuperSet::receiver);
    const ValueFlowOperand& value = operand(OperandIndex::SuperSet::value);
    forEachKeyOfOperation(OperandIndex::SuperSet::computedKey, [&](const HeapKey& key) {
        define(receiver, key, HeapPropertyPart::Value, value);
        callSetters(bases, key, receiver, value);
    });
}

void HeapRule::runOperation()
{
    namespace At = OperandIndex;
    switch (m_statement.opcode) {
    case Opcode::PropGet:
        read(operand(At::PropGet::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemGet:
        forEachKey(operand(At::ElemGet::property), [&](const HeapKey& key) {
            read(operand(At::ElemGet::object), key);
        });
        mayThrow();
        return;
    case Opcode::PropSet:
        store(operand(At::PropSet::object), keyNamed(operation().key()), operand(At::PropSet::value));
        return;
    case Opcode::ElemSet:
        forEachKey(operand(At::ElemSet::property), [&](const HeapKey& key) {
            store(operand(At::ElemSet::object), key, operand(At::ElemSet::value));
        });
        mayThrow();
        return;
    case Opcode::PropDefine:
        define(operand(At::PropDefine::object), keyNamed(operation().key()), HeapPropertyPart::Value, operand(At::PropDefine::value));
        return;
    case Opcode::ElemDefine:
        forEachKey(operand(At::ElemDefine::property), [&](const HeapKey& key) {
            define(operand(At::ElemDefine::object), key, HeapPropertyPart::Value, operand(At::ElemDefine::value));
        });
        mayThrow();
        return;
    case Opcode::DefineGetter:
        forEachKeyOfOperation(At::DefineGetter::computedKey, [&](const HeapKey& key) {
            define(operand(At::DefineGetter::object), key, HeapPropertyPart::Getter, operand(At::DefineGetter::function));
        });
        return;
    case Opcode::DefineSetter:
        forEachKeyOfOperation(At::DefineSetter::computedKey, [&](const HeapKey& key) {
            define(operand(At::DefineSetter::object), key, HeapPropertyPart::Setter, operand(At::DefineSetter::function));
        });
        return;
    case Opcode::PropHas:
        test(operand(At::PropHas::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemHas:
        forEachKey(operand(At::ElemHas::property), [&](const HeapKey& key) {
            test(operand(At::ElemHas::object), key);
        });
        answerBoolean();
        mayThrow();
        return;
    case Opcode::PropDelete:
        remove(operand(At::PropDelete::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemDelete:
        forEachKey(operand(At::ElemDelete::property), [&](const HeapKey& key) {
            remove(operand(At::ElemDelete::object), key);
        });
        answerBoolean();
        mayThrow();
        return;
    case Opcode::SetFunctionName:
    case Opcode::AppendHole:
        return;
    case Opcode::SetLiteralPrototype:
        changePrototype(m_context.valueOf(operand(At::SetLiteralPrototype::object)), operand(At::SetLiteralPrototype::prototype));
        return;
    case Opcode::CopyDataProperties:
        copyProperties(operand(At::CopyDataProperties::target), operand(At::CopyDataProperties::source), false);
        return;
    case Opcode::ScopeHasBinding:
        runScopeHasBinding();
        return;
    case Opcode::ForInOpen:
    case Opcode::ForInStep:
        // The names of an object are listed without running anything of the program, and a name is a string nobody knows.
        add(m_statement.result, m_universe.maskOf(m_context.maker(), m_program, m_program.at(operation().type)));
        mayThrow();
        return;
    case Opcode::GetSuperBase:
        runGetSuperBase();
        return;
    case Opcode::SuperGet:
        runSuperGet();
        return;
    case Opcode::SuperSet:
        runSuperSet();
        return;
    case Opcode::PrivateGet:
        connect(m_context.nodes().privateField(operation().layout(), operation().field()), m_statement.result);
        mayThrow();
        return;
    case Opcode::PrivateSet:
        connect(operand(At::PrivateSet::value), m_context.nodes().privateField(operation().layout(), operation().field()));
        mayThrow();
        return;
    case Opcode::PrivateDefine:
        connect(operand(At::PrivateDefine::value), m_context.nodes().privateField(operation().layout(), operation().field()));
        mayThrow();
        return;
    case Opcode::PrivateHas:
        answerBoolean();
        mayThrow();
        return;
    case Opcode::NewObject:
        runNewObject();
        return;
    case Opcode::NewArray:
        runNewArray();
        return;
    case Opcode::AppendElement:
        write(m_context.valueOf(operand(At::AppendElement::array)), elementsName, operand(At::AppendElement::value));
        return;
    case Opcode::CreateUnmappedArguments:
        runCreateArguments(operand(At::CreateUnmappedArguments::count), operand(At::CreateUnmappedArguments::rest), m_operands.subspan(At::CreateUnmappedArguments::parameters), nullptr);
        return;
    case Opcode::CreateMappedArguments:
        runCreateArguments(operand(At::CreateMappedArguments::count), operand(At::CreateMappedArguments::rest), m_operands.subspan(At::CreateMappedArguments::parameters), &operand(At::CreateMappedArguments::callee));
        return;
    case Opcode::GetMappedArgument:
        readOwn(operand(At::GetMappedArgument::arguments), elementsName, m_statement.result);
        add(m_statement.result, m_universe.undefinedValue());
        return;
    case Opcode::SetMappedArgument:
        write(m_context.valueOf(operand(At::SetMappedArgument::arguments)), elementsName, operand(At::SetMappedArgument::value));
        return;
    case Opcode::Data:
        runData();
        return;
    case Opcode::CreateIteratorResult:
        runCreateIteratorResult();
        return;
    case Opcode::GetOwnProperty:
        read(operand(At::GetOwnProperty::object), keyNamed(operation().key()));
        return;
    case Opcode::GetByValWithThis:
        runGetByValWithThis();
        return;
    case Opcode::GetContiguousIndex:
        readOwn(operand(At::GetContiguousIndex::array), elementsName, m_statement.result);
        add(m_statement.result, m_universe.undefinedValue());
        return;
    case Opcode::GetDoubleIndex:
        add(m_statement.result, kinds(Primitive::Number));
        return;
    case Opcode::PutContiguousIndex:
        write(m_context.valueOf(operand(At::PutContiguousIndex::array)), elementsName, operand(At::PutContiguousIndex::value));
        return;
    case Opcode::TryPutByValDirect:
        write(m_context.valueOf(operand(At::TryPutByValDirect::array)), elementsName, operand(At::TryPutByValDirect::value));
        answerBoolean();
        return;
    case Opcode::TryPutIndex:
        write(m_context.valueOf(operand(At::TryPutIndex::array)), elementsName, operand(At::TryPutIndex::value));
        answerBoolean();
        return;
    case Opcode::PutDoubleIndex:
    case Opcode::GrowNewArray:
        return;
    case Opcode::TruncateNewArray:
        runTruncateNewArray();
        return;
    case Opcode::GetInternalField:
        readOwn(operand(At::GetInternalField::object), internalFieldsName, m_statement.result);
        add(m_statement.result, m_universe.anyPrimitive());
        return;
    case Opcode::PutInternalField:
        write(m_context.valueOf(operand(At::PutInternalField::object)), internalFieldsName, operand(At::PutInternalField::value));
        connect(operand(At::PutInternalField::value), m_statement.result);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool isHeapOperation(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::PropSet:
    case Opcode::ElemSet:
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
    case Opcode::DefineGetter:
    case Opcode::DefineSetter:
    case Opcode::PropHas:
    case Opcode::ElemHas:
    case Opcode::PropDelete:
    case Opcode::ElemDelete:
    case Opcode::SetFunctionName:
    case Opcode::AppendHole:
    case Opcode::SetLiteralPrototype:
    case Opcode::CopyDataProperties:
    case Opcode::ScopeHasBinding:
    case Opcode::ForInOpen:
    case Opcode::ForInStep:
    case Opcode::GetSuperBase:
    case Opcode::SuperGet:
    case Opcode::SuperSet:
    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine:
    case Opcode::PrivateHas:
    case Opcode::NewObject:
    case Opcode::NewArray:
    case Opcode::AppendElement:
    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments:
    case Opcode::GetMappedArgument:
    case Opcode::SetMappedArgument:
    case Opcode::Data:
    case Opcode::CreateIteratorResult:
    case Opcode::GetOwnProperty:
    case Opcode::GetByValWithThis:
    case Opcode::GetContiguousIndex:
    case Opcode::GetDoubleIndex:
    case Opcode::PutContiguousIndex:
    case Opcode::TryPutByValDirect:
    case Opcode::TryPutIndex:
    case Opcode::PutDoubleIndex:
    case Opcode::GrowNewArray:
    case Opcode::TruncateNewArray:
    case Opcode::GetInternalField:
    case Opcode::PutInternalField:
        return true;
    default:
        return false;
    }
}

// A lookup enters getters and records uses of built-ins, and many operations share it. So even a read does nothing before its function is reached.
void runHeapOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (context.isReached(statement.function))
        HeapRule(statement, context).runOperation();
}

void forEachAccessorRunBy(const ValueFlowStatement& statement, ValueFlowRuleContext& context, const ValueFlowAccessorFunctor& functor)
{
    HeapRule rule(statement, context);
    rule.reportAccessorsTo(functor);
    rule.runOperation();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
