/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkWrittenDescriptors.h"

#include "AOTAccessPlan.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnown.h"
#include <array>
#include <optional>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class MemberOfDescriptor : uint8_t { Enumerable, Configurable, Value, Writable, Get, Set };
constexpr unsigned numberOfMembersOfDescriptor = 6;

struct OperandsOfDescriptorCall {
    Reference object;
    Reference key;
    Reference descriptor;
    unsigned argumentOfObject { 0 };
    unsigned argumentOfDescriptor { 0 };
};

struct PartsOfWrittenDescriptor {
    OptionSet<PropertyDescriptorFlags> flags;
    Reference value;
    Reference getter;
    Reference setter;
};

} // anonymous namespace

static PropertyKeyID keyOfMemberOfDescriptor(const Program& program, MemberOfDescriptor member)
{
    switch (member) {
    case MemberOfDescriptor::Enumerable:
        return program.keyOf(BuiltinNames::enumerable);
    case MemberOfDescriptor::Configurable:
        return program.keyOf(BuiltinNames::configurable);
    case MemberOfDescriptor::Value:
        return program.keyOf(BuiltinNames::value);
    case MemberOfDescriptor::Writable:
        return program.keyOf(BuiltinNames::writable);
    case MemberOfDescriptor::Get:
        return program.keyOf(BuiltinNames::get);
    case MemberOfDescriptor::Set:
        return program.keyOf(BuiltinNames::set);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<MemberOfDescriptor> memberOfDescriptorUnder(const Program& program, PropertyKeyID key)
{
    for (unsigned index = 0; index < numberOfMembersOfDescriptor; ++index) {
        if (key == keyOfMemberOfDescriptor(program, static_cast<MemberOfDescriptor>(index)))
            return static_cast<MemberOfDescriptor>(index);
    }
    return std::nullopt;
}

static std::optional<unsigned> argumentOfDescriptorCall(const std::optional<BuiltinOperand>& operand)
{
    if (!operand || operand->kind() != BuiltinOperand::Kind::Argument || operand->argumentIndex() >= numberOfArgumentsOfDescriptorCall)
        return std::nullopt;
    return operand->argumentIndex();
}

static bool descriptorCallReturnsOperand(const BuiltinTable& builtins, const BuiltinOverload& overload, BuiltinOperand operand)
{
    if (!overload.result.type || !builtins.contains(overload.result.type))
        return false;
    const BuiltinType& result = builtins.at(overload.result.type);
    if (!result.flags.isEmpty() || !result.primitives.isEmpty() || !result.stringLiterals.empty() || result.parts.size() != 1)
        return false;
    const BuiltinTypePart& part = result.parts.front();
    return part.kind == BuiltinTypePartKind::Operand && part.operand == operand;
}

static std::optional<OperandsOfDescriptorCall> operandsOfDescriptorCall(const BuiltinTable& builtins, const BuiltinOverload& overload, std::span<const Reference> arguments)
{
    if (overload.mode != BuiltinCallMode::Call || overload.flow.propertyActions.size() != 1)
        return std::nullopt;
    const BuiltinPropertyAction& action = overload.flow.propertyActions.front();
    if (action.kind != BuiltinPropertyActionKind::TakesDescriptor || action.key.kind != BuiltinKeyKind::Operand || action.value.kind != BuiltinPlaceKind::Operand)
        return std::nullopt;

    std::optional<unsigned> object = argumentOfDescriptorCall(action.object);
    std::optional<unsigned> key = argumentOfDescriptorCall(action.key.operand);
    std::optional<unsigned> descriptor = argumentOfDescriptorCall(action.value.operand);
    if (!object || !key || !descriptor || *object == *key || *object == *descriptor || *key == *descriptor)
        return std::nullopt;
    if (arguments.size() != numberOfArgumentsOfDescriptorCall || !descriptorCallReturnsOperand(builtins, overload, action.object))
        return std::nullopt;
    return OperandsOfDescriptorCall { arguments[*object], arguments[*key], arguments[*descriptor], *object, *descriptor };
}

static std::optional<bool> truthinessInDescriptors(Walker& walker, TypeID type)
{
    TypingContext typing = walker.typing();
    if (walker.types().at(narrowByTruthiness(typing, type, EdgeSide::NotTaken)).isNever())
        return true;
    if (walker.types().at(narrowByTruthiness(typing, type, EdgeSide::Taken)).isNever())
        return false;
    return std::nullopt;
}

static Reference rootOfObjectInWrittenDescriptors(const Walker& walker, Reference object)
{
    return object && object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : object;
}

static std::optional<Vector<PropertyOfFreshObject, 8>> propertiesDefinedBehindAllocationInWrittenDescriptors(Walker& walker, const BodyValue& call, ValueID written, Reference descriptor)
{
    static constexpr OpcodeSet storesIntoSlots { Opcode::PutSlot, Opcode::PutSlotOrAdd };
    static_assert(OperandIndex::PutSlot::object == OperandIndex::PutSlotOrAdd::object && OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
    const Code& code = walker.code();
    const Program& program = walker.program();
    Reference root = rootOfObjectInWrittenDescriptors(walker, descriptor);
    if (walker.currentBodyIndex().onlyUsingValue(written) != call.id() || !root || !root.isValue() || code.isConstant(root.id()) || !walker.state().isFresh(root))
        return std::nullopt;
    const Operation& made = code.at(root.id());
    if (made.opcode != Opcode::NewObject || made.block() != walker.currentBlock() || made.prototypeOperand() || program.at(made.layout()).kind != LayoutKind::Record)
        return std::nullopt;

    Vector<ValueID, 16> behindAllocation;
    ValueID last = code.lastOperationOf(walker.currentBlock());
    for (; last && last != root.id(); last = code.at(last).previous)
        behindAllocation.append(last);
    if (!last)
        return std::nullopt;

    Vector<PropertyOfFreshObject, 8> properties;
    auto addPropertyIn = [&](FieldIndex field) {
        FieldReference reference { made.layout(), field };
        Reference value = walker.state().slot(root, reference);
        if (value && value.isRecord() && code.at(value.id()).opcode == Opcode::Absent)
            return false;
        if (value && !properties.containsIf([&](const PropertyOfFreshObject& property) { return property.field == reference; }))
            properties.append({ reference, value });
        return !!value;
    };
    for (FieldIndex listed : made.fields()) {
        if (!addPropertyIn(listed))
            return std::nullopt;
    }
    for (ValueID id : behindAllocation | std::views::reverse) {
        const Operation& operation = code.at(id);
        bool namesCell = false;
        bool namesCellAsObjectAlone = true;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operand || code.isConstant(operand) || underlyingCellOf(code, operand) != root.id())
                return;
            namesCell = true;
            namesCellAsObjectAlone &= operandIndex == OperandIndex::PutSlot::object;
        });
        if (!namesCell || underlyingCellOf(code, id) == root.id())
            continue;
        if (!storesIntoSlots.contains(operation.opcode) || !namesCellAsObjectAlone)
            return std::nullopt;
        std::optional<FieldIndex> field = walker.plan().slots().findField(program, made.layout(), operation.key());
        if (!field || !addPropertyIn(*field))
            return std::nullopt;
    }
    return properties;
}

std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfLiteralWrittenForCall(Walker& walker, const BodyValue& call, ValueID written, Reference literal)
{
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfFreshObject(walker, literal);
    if (!properties)
        properties = propertiesDefinedBehindAllocationInWrittenDescriptors(walker, call, written, literal);
    if (!properties)
        return std::nullopt;
    if (!(walker.state().cellFacts(literal) | walker.state().cellFacts(rootOfObjectInWrittenDescriptors(walker, literal))).contains(CellFact::HasLayoutPrototype))
        return std::nullopt;
    return properties;
}

bool literalsFindNothingUnder(const Program& program, PropertyKeyID name)
{
    return name && !isOnObjectPrototype(program.nameClassOf(name)) && isClosedKey(program, program.builtins().canonicalID(Builtins::ObjectPrototype), name);
}

static std::optional<PartsOfWrittenDescriptor> partsOfDescriptorWith(Walker& walker, const Vector<PropertyOfFreshObject, 8>& properties)
{
    using Flag = PropertyDescriptorFlags;
    const Program& program = walker.program();
    PartsOfWrittenDescriptor read;
    std::array<bool, numberOfMembersOfDescriptor> isOwnMember { };
    for (const PropertyOfFreshObject& property : properties) {
        PropertyKeyID key = walker.plan().slots().keyOf(program, property.field);
        std::optional<MemberOfDescriptor> member = key ? memberOfDescriptorUnder(program, key) : std::nullopt;
        if (!member || walker.plan().slot(property.field.layout, property.field.field).holdsAccessor)
            return std::nullopt;
        TypeID typeOfValue = walker.typeOf(property.value);
        if (!typeOfValue)
            return std::nullopt;
        Type type = walker.types().at(typeOfValue);
        if (!type.isJavaScriptValue() || type.primitives & (Primitive::Absent | Primitive::Unspecified))
            return std::nullopt;

        auto addAttribute = [&](Flag isPresent, Flag isSet) {
            std::optional<bool> truthiness = truthinessInDescriptors(walker, typeOfValue);
            read.flags.add(isPresent);
            read.flags.set(isSet, truthiness.value_or(false));
            return truthiness.has_value();
        };
        auto addPart = [&](Flag isPresent, Reference& part) {
            read.flags.add(isPresent);
            part = property.value;
            return true;
        };
        bool isRead = false;
        switch (*member) {
        case MemberOfDescriptor::Enumerable:
            isRead = addAttribute(Flag::HasEnumerable, Flag::IsEnumerable);
            break;
        case MemberOfDescriptor::Configurable:
            isRead = addAttribute(Flag::HasConfigurable, Flag::IsConfigurable);
            break;
        case MemberOfDescriptor::Value:
            isRead = addPart(Flag::HasValue, read.value);
            break;
        case MemberOfDescriptor::Writable:
            isRead = addAttribute(Flag::HasWritable, Flag::IsWritable);
            break;
        case MemberOfDescriptor::Get:
            isRead = addPart(Flag::HasGet, read.getter);
            break;
        case MemberOfDescriptor::Set:
            isRead = addPart(Flag::HasSet, read.setter);
            break;
        }
        if (!isRead)
            return std::nullopt;
        isOwnMember[static_cast<unsigned>(*member)] = true;
    }
    if (read.flags.containsAny({ Flag::HasGet, Flag::HasSet }) && read.flags.containsAny({ Flag::HasValue, Flag::HasWritable }))
        return std::nullopt;

    for (unsigned index = 0; index < numberOfMembersOfDescriptor; ++index) {
        if (!isOwnMember[index] && !literalsFindNothingUnder(program, keyOfMemberOfDescriptor(program, static_cast<MemberOfDescriptor>(index))))
            return std::nullopt;
    }
    return read;
}

static std::optional<PartsOfWrittenDescriptor> readDescriptorWrittenOut(Walker& walker, const BodyValue& call, ValueID written, Reference descriptor)
{
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfLiteralWrittenForCall(walker, call, written, descriptor);
    return properties ? partsOfDescriptorWith(walker, *properties) : std::nullopt;
}

static std::optional<Vector<PropertyOfFreshObject, 8>> nestedLiteralProperties(Walker& walker, Reference holder, Reference literal)
{
    static constexpr OpcodeSet storesIntoSlots { Opcode::PutSlot, Opcode::PutSlotOrAdd };
    const Code& code = walker.code();
    const Program& program = walker.program();
    Reference root = rootOfObjectInWrittenDescriptors(walker, literal);
    Reference rootOfHolder = rootOfObjectInWrittenDescriptors(walker, holder);
    if (!root || !root.isValue() || code.isConstant(root.id()) || !rootOfHolder || !rootOfHolder.isValue() || root == rootOfHolder || !walker.state().isFresh(rootOfHolder))
        return std::nullopt;
    const Operation& made = code.at(root.id());
    if (made.opcode != Opcode::NewObject || made.block() != walker.currentBlock() || made.prototypeOperand() || program.at(made.layout()).kind != LayoutKind::Record)
        return std::nullopt;
    if (made.fields().size() != made.valuesOperands().size() || !walker.fillersOfAllocation(root).empty())
        return std::nullopt;

    Vector<ValueID, 16> afterAllocation;
    ValueID last = code.lastOperationOf(walker.currentBlock());
    for (; last && last != root.id(); last = code.at(last).previous)
        afterAllocation.append(last);
    if (!last)
        return std::nullopt;

    Vector<PropertyOfFreshObject, 8> properties;
    auto define = [&](FieldIndex field, ValueID stored) {
        if (!stored)
            return false;
        while (conversionsOfForm.contains(code.at(stored).opcode))
            stored = code.at(stored).operand(0);
        Reference value = code.isConstant(stored) ? Reference::constant(stored) : Reference::value(stored);
        TypeID type = !isClosureLeftUnmade(walker, value) ? walker.typeOf(value) : TypeID();
        if (!type || (walker.types().at(type).primitives & Primitive::Absent))
            return false;
        FieldReference reference { made.layout(), field };
        for (PropertyOfFreshObject& property : properties) {
            if (property.field == reference) {
                property.value = value;
                return true;
            }
        }
        properties.append({ reference, value });
        return true;
    };
    for (size_t index = 0; index < made.fields().size(); ++index) {
        if (!define(made.fields()[index], made.valuesOperands()[index]))
            return std::nullopt;
    }
    unsigned timesStoredIntoHolder = 0;
    for (ValueID id : afterAllocation | std::views::reverse) {
        const Operation& operation = code.at(id);
        bool isNamedAsObject = false;
        bool isNamedAsValue = false;
        bool isNamedOtherwise = false;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operand || code.isConstant(operand) || underlyingCellOf(code, operand) != root.id())
                return;
            if (operandIndex == OperandIndex::PutSlot::object)
                isNamedAsObject = true;
            else if (operandIndex == OperandIndex::PutSlot::value)
                isNamedAsValue = true;
            else
                isNamedOtherwise = true;
        });
        if ((!isNamedAsObject && !isNamedAsValue && !isNamedOtherwise) || underlyingCellOf(code, id) == root.id())
            continue;
        if (!storesIntoSlots.contains(operation.opcode) || isNamedOtherwise || isNamedAsObject == isNamedAsValue)
            return std::nullopt;
        if (isNamedAsValue) {
            if (underlyingCellOf(code, operation.objectOperand()) != rootOfHolder.id() || timesStoredIntoHolder++)
                return std::nullopt;
            continue;
        }
        std::optional<FieldIndex> field = walker.plan().slots().findField(program, made.layout(), operation.key());
        if (!field || !define(*field, operation.valueOperand()))
            return std::nullopt;
    }
    if (timesStoredIntoHolder != 1)
        return std::nullopt;
    return properties;
}

static std::optional<BuiltinOverloadID> findUniqueBuiltinOverload(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return std::nullopt;
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return std::nullopt;
    Type type = types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = table.canonicalID(named);
    BuiltinTypeContext context { walker.program(), types };
    auto typeOfOperand = [&](ValueID operand) {
        Reference built = walker.referenceOf(operand);
        return built ? walker.typeOf(built) : TypeID();
    };
    BuiltinOperandTypes passed;
    if (ValueID thisValue = call.thisValueOperand())
        passed.receiver = typeOfOperand(thisValue);
    for (ValueID argument : call.argumentsOperands())
        passed.arguments.append(typeOfOperand(argument));
    if (!isCallableBuiltin(table, builtin) || hasFeatureGuard(table, builtin) || requiresWalkLowering(table, builtin) || !rowsSpeakOfCallOnReceiver(context, builtin, passed.receiver))
        return std::nullopt;

    Vector<BuiltinOverloadID, 4> rows;
    for (BuiltinOverloadID overload : table.overloadIDsOf(builtin)) {
        if (table.at(overload).mode == BuiltinCallMode::Call)
            rows.append(overload);
    }
    if (rows.size() > 1)
        rows = overloadsReachedBy(context, builtin, BuiltinCallMode::Call, passed);
    if (rows.size() != 1)
        return std::nullopt;
    return rows[0];
}

uint32_t objectCreateDescriptorCount(Walker& walker, const BodyValue& objectCreateCall)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID map = objectCreateCall.argumentsOperands()[1];
    if (!index.isParameter(map) && index.opcodeOf(map) == Opcode::FinishObject)
        map = index.value(map).operand(0);
    if (index.isParameter(map) || index.opcodeOf(map) != Opcode::NewObject)
        return 0;
    return index.value(map).as<Opcode::NewObject>().fields().size() + index.useCount(map) - 1;
}

constinit const OpcodeSet opcodesThatExpandObjectCreateDescriptorsTakes { Opcode::Call };

static bool callsBuiltinInWrittenDescriptors(Walker& walker, const BodyValue& call, BuiltinID expected)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    const BuiltinTable& table = walker.program().builtins();
    return named && table.canonicalID(named) == table.canonicalID(expected);
}

static Reference keyConstantInWrittenDescriptors(Walker& walker, PropertyKeyID key, Origin origin)
{
    const PropertyKey& record = walker.program().at(key);
    if (record.kind == PropertyKeyKind::Text) {
        Draft name(Opcode::String, origin);
        name.setString(record.text);
        return walker.constant(WTF::move(name));
    }
    Draft symbol(Opcode::KeyConstant, origin);
    symbol.setKey(key);
    return walker.constant(WTF::move(symbol));
}

bool expandObjectCreateDescriptorsOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis) || call.argumentsOperands().size() != objectCreateArgumentCount)
        return false;
    if (!callsBuiltinInWrittenDescriptors(walker, call, Builtins::ObjectCreate) || !findUniqueBuiltinOverload(walker, call))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    const Program& program = walker.program();
    Reference prototype = written.argumentsOperands()[0];
    Reference map = written.argumentsOperands()[1];
    TypeID provenOfPrototype = prototype ? walker.typeOf(prototype) : TypeID();
    if (!provenOfPrototype || !map)
        return false;
    Type typeOfPrototype = walker.types().at(provenOfPrototype);
    if (!typeOfPrototype.isJavaScriptValue() || typeOfPrototype.isNever() || (typeOfPrototype.primitives & (Primitive::Absent | Primitive::Unspecified)))
        return false;
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfLiteralWrittenForCall(walker, call, call.argumentsOperands()[1], map);
    if (!properties)
        return false;

    struct Definition {
        PropertyKeyID key;
        PartsOfWrittenDescriptor parts;
    };
    Vector<Definition, 8> names;
    Vector<Definition, 8> symbols;
    for (const PropertyOfFreshObject& property : *properties) {
        PropertyKeyID key = walker.plan().slots().keyOf(program, property.field);
        if (!key || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto || walker.plan().slot(property.field.layout, property.field.field).holdsAccessor)
            return false;
        PropertyKeyKind kind = program.at(key).kind;
        bool isSymbol = kind == PropertyKeyKind::WellKnownSymbol || kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::RegisteredSymbol;
        if (!isSymbol && kind != PropertyKeyKind::Text)
            return false;
        if (rootOfObjectInWrittenDescriptors(walker, property.value) == rootOfObjectInWrittenDescriptors(walker, prototype))
            return false;
        std::optional<Vector<PropertyOfFreshObject, 8>> members = nestedLiteralProperties(walker, map, property.value);
        std::optional<PartsOfWrittenDescriptor> parts = members ? partsOfDescriptorWith(walker, *members) : std::nullopt;
        if (!parts)
            return false;
        (isSymbol ? symbols : names).append({ key, *parts });
    }

    Origin origin = written.origin();
    origin.isTailCall = false;
    Draft creation(Opcode::Call, origin);
    walker.chooseConservativeFields(creation);
    creation.setCalleeOperand(written.calleeOperand());
    if (written.thisValueOperand())
        creation.setThisValueOperand(written.thisValueOperand());
    creation.setArgumentsOperands(written.argumentsOperands().first(1));
    creation.setCalleeName(written.calleeName());
    creation.setExpectedResult(written.expectedResult());
    creation.expected = written.expected;
    creation.inputValueGiven = written.inputValueGiven;
    walker.count(StatisticID::ObjectCreateDescriptorsExpanded);
    if (!callBuiltinForwarded(walker, call, creation)) {
        if (Reference result = walker.build(WTF::move(creation)))
            walker.bind(call.id(), result);
    }

    for (const Vector<Definition, 8>* group : { &names, &symbols }) {
        for (const Definition& listed : *group) {
            Reference object = walker.referenceOf(call.id());
            if (!object || walker.isUnreachable() || walker.hasFailed())
                return true;
            Draft definition(Opcode::DefineOwnProperty, origin);
            walker.chooseConservativeFields(definition);
            definition.setObjectOperand(object);
            definition.setComputedKeyOperand(keyConstantInWrittenDescriptors(walker, listed.key, origin));
            if (listed.parts.value)
                definition.setValueOperand(listed.parts.value);
            if (listed.parts.getter)
                definition.setGetterOperand(listed.parts.getter);
            if (listed.parts.setter)
                definition.setSetterOperand(listed.parts.setter);
            definition.setDescriptorFlags(listed.parts.flags);
            walker.build(WTF::move(definition));
        }
    }
    return true;
}

constinit const OpcodeSet opcodesThatExpandObjectCreateAtDefinitionTakes { Opcode::ElemDefine, Opcode::PropDefine };

bool expandObjectCreateAtDefinitionOfItsOpcodes(Walker& walker, const BodyValue& definition)
{
    if (walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID map = definition.objectOperand();
    BlockID inputBlock = index.blockOf(definition.id());
    if (!map || index.isParameter(map) || index.blockOf(map) != inputBlock || index.useCount(map) != 2)
        return false;
    if (index.opcodeOf(map) != Opcode::NewObject || index.value(map).as<Opcode::NewObject>().prototypeOperand() || !index.value(map).as<Opcode::NewObject>().fields().empty())
        return false;

    BodyBlock block = index.block(inputBlock);
    uint32_t firstValue = block.firstValue().index();
    uint32_t endOfBlock = block.endOfValues().index();
    uint32_t next = definition.id().index() + 1;
    if (next >= endOfBlock || index.opcodeOf(ValueID(next)) != Opcode::FinishObject || index.value(ValueID(next)).operand(0) != map || !index.hasOneUse(ValueID(next)))
        return false;
    ValueID argument(next++);
    if (next >= endOfBlock || index.opcodeOf(ValueID(next)) != Opcode::Call || next - firstValue + 1 > walker.endOfStretchBeingWalked())
        return false;
    BodyValue call = index.value(ValueID(next));
    if (call.argumentsOperands().size() != objectCreateArgumentCount || call.argumentsOperands()[1] != argument || !callsBuiltinInWrittenDescriptors(walker, call, Builtins::ObjectCreate))
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = table.canonicalID(Builtins::ObjectCreate);
    if (!isCallableBuiltin(table, builtin) || hasFeatureGuard(table, builtin) || requiresWalkLowering(table, builtin))
        return false;
    Reference madeMap = walker.referenceOf(map);
    if (!madeMap || !madeMap.isValue() || !walker.state().isFresh(rootOfObjectInWrittenDescriptors(walker, madeMap)))
        return false;

    Draft writtenDefinition = walker.draftOf(definition);
    if (walker.hasFailed())
        return false;
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Reference key;
    if (definition.opcode() == Opcode::ElemDefine) {
        key = writtenDefinition.propertyOperand();
        TypeID provenOfKey = key ? walker.typeOf(key) : TypeID();
        if (!provenOfKey)
            return false;
        Type typeOfKey = walker.types().at(provenOfKey);
        if (!typeOfKey.isJavaScriptValue() || typeOfKey.isNever() || !typeOfKey.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol))
            return false;
    } else {
        PropertyKeyKind kind = walker.program().at(definition.key()).kind;
        if (kind != PropertyKeyKind::Text && kind != PropertyKeyKind::WellKnownSymbol && kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol)
            return false;
    }
    Reference prototype = walker.referenceOf(call.argumentsOperands()[0]);
    TypeID provenOfPrototype = prototype ? walker.typeOf(prototype) : TypeID();
    if (!provenOfPrototype)
        return false;
    Type typeOfPrototype = walker.types().at(provenOfPrototype);
    if (!typeOfPrototype.isJavaScriptValue() || typeOfPrototype.isNever() || (typeOfPrototype.primitives & (Primitive::Absent | Primitive::Unspecified)))
        return false;
    Reference descriptor = writtenDefinition.valueOperand();
    if (!descriptor || rootOfObjectInWrittenDescriptors(walker, descriptor) == rootOfObjectInWrittenDescriptors(walker, prototype))
        return false;
    std::optional<PartsOfWrittenDescriptor> parts = readDescriptorWrittenOut(walker, definition, definition.valueOperand(), descriptor);
    if (!parts)
        return false;

    walker.bind(argument, madeMap);
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return true;
    if (!key)
        key = keyConstantInWrittenDescriptors(walker, definition.key(), origin);
    Draft creation(Opcode::Call, origin);
    walker.chooseConservativeFields(creation);
    creation.setCalleeOperand(written.calleeOperand());
    if (written.thisValueOperand())
        creation.setThisValueOperand(written.thisValueOperand());
    creation.setArgumentsOperands(written.argumentsOperands().first(1));
    creation.setCalleeName(written.calleeName());
    creation.setExpectedResult(written.expectedResult());
    creation.expected = written.expected;
    creation.inputValueGiven = written.inputValueGiven;
    walker.count(StatisticID::ObjectCreateDescriptorsExpanded);
    if (!callBuiltinForwarded(walker, call, creation)) {
        if (Reference result = walker.build(WTF::move(creation)))
            walker.bind(call.id(), result);
    }
    Reference object = walker.referenceOf(call.id());
    if (object && !walker.isUnreachable() && !walker.hasFailed()) {
        Draft property(Opcode::DefineOwnProperty, origin);
        walker.chooseConservativeFields(property);
        property.setObjectOperand(object);
        property.setComputedKeyOperand(key);
        if (parts->value)
            property.setValueOperand(parts->value);
        if (parts->getter)
            property.setGetterOperand(parts->getter);
        if (parts->setter)
            property.setSetterOperand(parts->setter);
        property.setDescriptorFlags(parts->flags);
        walker.build(WTF::move(property));
    }
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), definition.id()))
        walker.leaveAnythingAt(callee);
    uint32_t placeAfterCall = next - firstValue + 1;
    walker.walkStretch(inputBlock, placeAfterCall, placeAfterCall);
    return true;
}

bool buildDefinitionByWrittenDescriptorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis) || call.argumentsOperands().size() != numberOfArgumentsOfDescriptorCall)
        return false;
    std::optional<BuiltinOverloadID> row = findUniqueBuiltinOverload(walker, call);
    if (!row)
        return false;
    const BuiltinTable& builtins = walker.program().builtins();
    const BuiltinOverload& overload = builtins.at(*row);
    if (overload.flow.propertyActions.size() != 1)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    std::optional<OperandsOfDescriptorCall> operands = operandsOfDescriptorCall(builtins, overload, written.argumentsOperands());
    if (!operands || !operands->object || !operands->key || !operands->descriptor)
        return false;

    TypeID provenOfKey = walker.typeOf(operands->key);
    TypeID provenOfObject = walker.typeOf(operands->object);
    if (!provenOfKey || !provenOfObject)
        return false;
    Type typeOfKey = walker.types().at(provenOfKey);
    Type typeOfObject = walker.types().at(provenOfObject);
    if (!typeOfKey.isJavaScriptValue() || typeOfKey.isNever() || (typeOfKey.primitives & (Primitive::Absent | Primitive::Unspecified)))
        return false;
    if (!typeOfObject.isJavaScriptValue() || !typeOfObject.hasObjects())
        return false;
    if (typeOfKey.hasObjects() && typeOfObject.primitives)
        return false;
    if (rootOfObjectInWrittenDescriptors(walker, operands->object) == rootOfObjectInWrittenDescriptors(walker, operands->descriptor))
        return false;
    std::optional<PartsOfWrittenDescriptor> descriptor = readDescriptorWrittenOut(walker, call, call.argumentsOperands()[operands->argumentOfDescriptor], operands->descriptor);
    if (!descriptor)
        return false;

    Origin origin = written.origin();
    origin.isTailCall = false;
    Reference key = operands->key;
    if (!typeOfKey.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol)) {
        Draft conversion(Opcode::ToPropertyKey, origin);
        walker.chooseConservativeFields(conversion);
        conversion.setValueOperand(key);
        key = walker.build(WTF::move(conversion));
        if (!key)
            return true;
    }

    constexpr OptionSet<PropertyDescriptorFlags> saysYesToEverything {
        PropertyDescriptorFlags::HasValue, PropertyDescriptorFlags::HasWritable, PropertyDescriptorFlags::IsWritable, PropertyDescriptorFlags::HasEnumerable,
        PropertyDescriptorFlags::IsEnumerable, PropertyDescriptorFlags::HasConfigurable, PropertyDescriptorFlags::IsConfigurable
    };
    bool definesPlainField = descriptor->flags == saysYesToEverything && descriptor->value && !typeOfObject.primitives && walker.has(Mechanism::TypedOperations);
    PropertyKeyID name = definesPlainField ? nameThatKeyIs(walker, key) : PropertyKeyID();
    if (name && definePlainFieldInSlot(walker, call, call.argumentsOperands()[operands->argumentOfObject], operands->object, name, descriptor->value)) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            walker.bind(call.id(), operands->object);
        return true;
    }
    bool writesSlotInLine = name && fieldCreatedInSlotBy(AccessContext { walker.program(), WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() }, {
        provenOfObject, walker.typeOf(key), walker.typeOf(descriptor->value), TypeID(), TypeID(), descriptor->flags
    });
    if (name && !writesSlotInLine) {
        Draft field(Opcode::PropDefine, origin);
        walker.chooseConservativeFields(field);
        field.setObjectOperand(operands->object);
        field.setKey(name);
        field.setValueOperand(descriptor->value);
        walker.build(WTF::move(field));
        if (!walker.isUnreachable() && !walker.hasFailed())
            walker.bind(call.id(), operands->object);
        return true;
    }

    Draft definition(Opcode::DefineOwnProperty, origin);
    walker.chooseConservativeFields(definition);
    definition.expected = written.expected;
    definition.inputValueGiven = written.inputValueGiven;
    definition.setObjectOperand(operands->object);
    definition.setComputedKeyOperand(key);
    if (descriptor->value)
        definition.setValueOperand(descriptor->value);
    if (descriptor->getter)
        definition.setGetterOperand(descriptor->getter);
    if (descriptor->setter)
        definition.setSetterOperand(descriptor->setter);
    definition.setDescriptorFlags(descriptor->flags);
    if (Reference object = walker.build(WTF::move(definition)))
        walker.bind(call.id(), object);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
