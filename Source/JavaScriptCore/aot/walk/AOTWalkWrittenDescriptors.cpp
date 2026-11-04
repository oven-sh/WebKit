/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `Object.defineProperty(o, "x", { value: 1, enumerable: false })`: the flags become bits, and the value, the getter and the setter operands of one DefineOwnProperty. Nothing names the literal any more, so the sweep drops it.

#include "config.h"
#include "AOTWalkWrittenDescriptors.h"

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
#include <array>
#include <optional>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned numberOfArgumentsOfDescriptorCall = 3;

namespace {

enum class MemberOfDescriptor : uint8_t { Enumerable, Configurable, Value, Writable, Get, Set };
constexpr unsigned numberOfMembersOfDescriptor = 6;

struct OperandsOfDescriptorCall {
    Reference object;
    Reference key;
    Reference descriptor;
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

// DefineOwnProperty answers with the object and throws where the definition is refused. Reflect.defineProperty answers false there.
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
    return OperandsOfDescriptorCall { arguments[*object], arguments[*key], arguments[*descriptor], *descriptor };
}

// Nothing: the type leaves it open.
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
    return object && object.isValue() ? Reference::value(rootOfCellInSweptCode(walker.code(), object.id())) : object;
}

// `{ enumerable: true, get: function () { return a.b; } }` as a file without types has it: an allocation that lists nothing and a definition for each member, which leave an order word that nobody knows.
// And a literal of a layout to which Link has appended a name, of which propertiesOfFreshObject() says nothing.
// The literal is written in the place of the operand, so what lies between its allocation and the call is the evaluation of its members, and each literal is gone through once.
// Every operation there that names the cell gives it another name or stores into a slot of it. Nothing: some other does, or the way from the allocation to the call is not straight.
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
            if (!operand || code.isConstant(operand) || rootOfCellInSweptCode(code, operand) != root.id())
                return;
            namesCell = true;
            namesCellAsObjectAlone &= operandIndex == OperandIndex::PutSlot::object;
        });
        if (!namesCell || rootOfCellInSweptCode(code, id) == root.id())
            continue;
        if (!storesIntoSlots.contains(operation.opcode) || !namesCellAsObjectAlone)
            return std::nullopt;
        std::optional<FieldIndex> field = walker.plan().slots().findField(program, made.layout(), operation.key());
        if (!field || !addPropertyIn(*field))
            return std::nullopt;
    }
    return properties;
}

// While nothing but this code has the literal, its own properties are what its allocation lists, and all of them hold plain values: reading them runs no code, in whatever order.
static std::optional<PartsOfWrittenDescriptor> readDescriptorWrittenOut(Walker& walker, const BodyValue& call, ValueID written, Reference descriptor)
{
    using Flag = PropertyDescriptorFlags;
    const Program& program = walker.program();
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfFreshObject(walker, descriptor);
    if (!properties)
        properties = propertiesDefinedBehindAllocationInWrittenDescriptors(walker, call, written, descriptor);
    if (!properties)
        return std::nullopt;

    // `{ __proto__: p, .. }`: what the literal leaves out is looked for above p.
    if (!(walker.state().factsOfCell(descriptor) | walker.state().factsOfCell(rootOfObjectInWrittenDescriptors(walker, descriptor))).contains(FactOfCell::HasLayoutPrototype))
        return std::nullopt;

    PartsOfWrittenDescriptor read;
    std::array<bool, numberOfMembersOfDescriptor> isOwnMember { };
    for (const PropertyOfFreshObject& property : *properties) {
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
    // The built-in refuses the pair in its own words.
    if (read.flags.containsAny({ Flag::HasGet, Flag::HasSet }) && read.flags.containsAny({ Flag::HasValue, Flag::HasWritable }))
        return std::nullopt;

    // A member that the literal leaves out is looked for above it. Object.prototype has none of the six names, and gets none but what a test run has opened.
    BuiltinID objectPrototype = program.builtins().canonicalID(Builtins::ObjectPrototype);
    for (unsigned index = 0; index < numberOfMembersOfDescriptor; ++index) {
        PropertyKeyID name = keyOfMemberOfDescriptor(program, static_cast<MemberOfDescriptor>(index));
        if (!isOwnMember[index] && (!name || isOnObjectPrototype(program.nameClassOf(name)) || !isClosedKey(program, objectPrototype, name)))
            return std::nullopt;
    }
    return read;
}

// The one row of calls that the call of a built-in goes by, where its callee is proven one built-in whose rows may be believed. Nothing: there is none.
static std::optional<BuiltinOverloadID> findOnlyRowOfCallOfBuiltinInWrittenDescriptors(Walker& walker, const BodyValue& call)
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
    BuiltinTypeContext context { walker.program(), types, &walker.facts() };
    auto typeOfOperand = [&](ValueID operand) {
        Reference built = walker.referenceOf(operand);
        return built ? walker.typeOf(built) : TypeID();
    };
    BuiltinOperandTypes passed;
    if (ValueID thisValue = call.thisValueOperand())
        passed.receiver = typeOfOperand(thisValue);
    for (ValueID argument : call.argumentsOperands())
        passed.arguments.append(typeOfOperand(argument));
    if (!isCallableBuiltin(table, builtin) || hasFeatureGuard(table, builtin) || isCallThatTheWalkMustLower(table, builtin) || !rowsSpeakOfCallOnReceiver(context, builtin, passed.receiver))
        return std::nullopt;

    // The only row of calls speaks of every call, whatever is proven of the operands.
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

bool buildDefinitionByWrittenDescriptorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis) || call.argumentsOperands().size() != numberOfArgumentsOfDescriptorCall)
        return false;
    std::optional<BuiltinOverloadID> row = findOnlyRowOfCallOfBuiltinInWrittenDescriptors(walker, call);
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

    // The built-in refuses what is no object in front of the conversion of the key. A key that is no object becomes a name without running anybody's code, so nobody sees the order.
    TypeID provenOfKey = walker.typeOf(operands->key);
    TypeID provenOfObject = walker.typeOf(operands->object);
    if (!provenOfKey || !provenOfObject)
        return false;
    Type typeOfKey = walker.types().at(provenOfKey);
    Type typeOfObject = walker.types().at(provenOfObject);
    if (!typeOfKey.isJavaScriptValue() || typeOfKey.hasObjects() || typeOfKey.isNever() || (typeOfKey.primitives & (Primitive::Absent | Primitive::Unspecified)))
        return false;
    if (!typeOfObject.isJavaScriptValue() || !typeOfObject.hasObjects())
        return false;
    // The object that takes the property is not the literal that describes it.
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
