/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONEmission.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowApply.h"
#include "AOTJSONParserBuilder.h"
#include "AOTJSONSerializerBuilder.h"
#include "AOTKnownValue.h"
#include "AOTMethodDispatch.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTValidatorBuilder.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// A field that is redefined, or stored with a value its slot cannot hold, moves to the extra properties and keeps its place in the key order.
static constexpr OptionSet<ObjectMutation> mutationsCreatingExtraProperties {
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::StoreOfOtherType,
};

static DerivedSignature signatureOfKey(Emission& emission, const DerivedFunctionKey& key)
{
    switch (key.role) {
    case FunctionRole::Serializer:
        return JSONSerializerBuilder::signatureOf(emission, LayoutID(key.parameter));
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
        return JSONParserBuilder::signatureOf(emission, describeNode(emission.program(), nodeOfKey(key)));
    case FunctionRole::Validator:
        return ValidatorBuilder::signatureOf(emission, describeNode(emission.program(), nodeOfKey(key)));
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Reading a provisional callee builds it, and a recursive node would wait for the function that is being built.
static std::optional<DerivedSignature> signatureOfProvisionalCallee(Emission& emission, FunctionID callee)
{
    if (!ProvisionalFunctions::contains(callee))
        return std::nullopt;
    return signatureOfKey(emission, emission.inputs().functions.keyOf(callee));
}

FunctionID DerivedFunctionRequests::ensure(const DerivedFunctionKey& key) const
{
    return m_pass ? m_pass->derivedFunction(key) : m_functions->ensure(key);
}

DerivedFunctionKey DerivedFunctionRequests::keyOf(FunctionID provisional) const
{
    return (m_pass ? m_pass->inputs.derivedFunctions : m_functions)->keyOf(provisional);
}

EmissionInputs::EmissionInputs(FunctionPassContext& context)
    : program(context.program)
    , flow(context.inputs.flow)
    , types(context.types)
    , strings(context.strings)
    , build(context.build)
    , functions(context)
    , function(context.id)
    , functionAsRead(context.subject().function)
{
}

EmissionInputs::EmissionInputs(DerivedFunctionInputs& inputs, FunctionID subject)
    : program(inputs.program)
    , flow(inputs.flow)
    , types(inputs.types)
    , strings(inputs.strings)
    , build(inputs.build)
    , functions(inputs)
    , function(subject)
    , functionAsRead(subject)
{
}

DerivedFunctionHead createDerivedFunction(const EmissionInputs& inputs, FunctionRole role, CreatorRecord&& creator, const DerivedSignature& signature)
{
    DerivedFunctionHead head;
    Function& function = head.function;
    function.kind = FunctionKind::Plain;
    function.role = role;
    function.capabilities = capabilitiesOf(role);
    function.creator = WTF::move(creator);
    function.length = static_cast<uint32_t>(signature.parameters.size());
    function.thisType = DeclaredType { .type = TypeTable::anything };
    for (TypeID type : signature.parameters) {
        function.parameters.append(DeclaredType { .type = type });
        function.derivation.parameterMap.append(ParameterOrigin { });
    }
    for (TypeID type : signature.results)
        function.results.append(DeclaredType { .type = type });

    IRBuilder builder(inputs.analysisContext(), function, inputs.types);
    head.entry = builder.addBlock();
    for (TypeID type : signature.parameters)
        head.parameters.append(builder.addParameter(head.entry, type));
    return head;
}

Emission::Emission(IRBuilder& builder, const EmissionInputs& inputs)
    : m_builder(builder)
    , m_inputs(inputs)
{
}

const Type& Emission::typeOf(ValueID value) const
{
    return program().typeOf(m_builder.function(), value);
}

TypeID Emission::outcomeType()
{
    return m_inputs.types.intern(Type::integer({ 0, numberOfEnumerators<JSONOutcome> - 1 }));
}

TypeID Emission::cursorType()
{
    return m_inputs.types.intern(Type::forOpaque(OpaqueKind::Cursor));
}

TypeID Emission::keyOrderType()
{
    return m_inputs.types.intern(Type::forOpaque(OpaqueKind::KeyOrder));
}

TypeID Emission::stringBuilderType()
{
    return m_inputs.types.intern(Type::forOpaque(OpaqueKind::StringBuilder));
}

ValueID Emission::appendOutcome(JSONOutcome outcome)
{
    return m_builder.appendInteger(static_cast<int64_t>(outcome));
}

ValueID Emission::appendString(StringID string)
{
    Value value = IRBuilder::make(Opcode::String);
    value.setString(string);
    return m_builder.append(WTF::move(value));
}

ValueID Emission::appendString(StringView text)
{
    return appendString(m_inputs.strings.intern(text));
}

ValueID Emission::appendNull()
{
    return m_builder.append(Opcode::Null, { });
}

ValueID Emission::appendResultAt(ValueID results, uint32_t index)
{
    Value result = IRBuilder::make(Opcode::ResultAt, { results });
    result.setIndex(index);

    const Value& producer = m_builder.function().at(results);
    std::optional<DerivedSignature> signature;
    if (producer.opcode == Opcode::CallDirect)
        signature = signatureOfProvisionalCallee(*this, producer.function());
    if (!signature)
        return m_builder.append(WTF::move(result));
    return m_builder.appendWithType(WTF::move(result), signature->results[index]);
}

ValueID Emission::appendResults(std::initializer_list<ValueID> values)
{
    return m_builder.append(Opcode::Results, values);
}

void Emission::emitReturnOf(std::initializer_list<ValueID> values)
{
    RELEASE_ASSERT(values.size());
    m_builder.emitReturn(values.size() == 1 ? *values.begin() : appendResults(values));
}

ValueID Emission::appendCallOf(FunctionID function, std::initializer_list<ValueID> arguments)
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(function);
    call.setArgumentsOperands(std::span<const ValueID> { arguments.begin(), arguments.size() });

    std::optional<DerivedSignature> signature = signatureOfProvisionalCallee(*this, function);
    if (!signature)
        return m_builder.append(WTF::move(call));
    return m_builder.appendWithType(WTF::move(call), signature->results.size() == 1 ? signature->results[0] : TypeTable::undefined);
}

ValueID Emission::appendHelperCall(BuiltinID helper, std::initializer_list<ValueID> arguments)
{
    BuiltinTableIDRange<BuiltinOverloadID> overloads = program().builtins().overloadIDsOf(helper);
    RELEASE_ASSERT(overloads.size() == 1);
    return appendBuiltinCall(overloads.at(0), { }, std::span<const ValueID> { arguments.begin(), arguments.size() });
}

ValueID Emission::appendBuiltinCall(BuiltinOverloadID overload, ValueID receiver, std::span<const ValueID> arguments)
{
    return m_builder.append(makeBuiltinCall(overload, BuiltinCallForm::Whole, { receiver, Vector<ValueID, 4>(arguments) }, { }));
}

Emission::TestedObject Emission::emitShapeTest(ValueID value, std::span<const LayoutID> layouts, BlockID notTaken, BlockFrequency whenTaken)
{
    LayoutSetID tested = layoutSetOf(layouts);
    ValueID object = emitTypeTest(value, TypeTable::object, notTaken);
    const Type& type = typeOf(object);
    if (type.hasOnlyLayouts(program()) && isSubsetOf(program().at(type.layouts), program().at(tested)))
        return { object, { } };

    Value test = IRBuilder::make(Opcode::ShapeTest, { object });
    test.setLayouts(tested);
    return { object, emitProvenBranch(m_builder.append(WTF::move(test)), notTaken, whenTaken) };
}

ValueID Emission::emitKeyOrderTest(const TestedObject& object, LayoutID layout, std::span<const uint32_t> fields, BlockID notTaken)
{
    Vector<ValueID, 3> guards;
    if (object.guard)
        guards.append(object.guard);

    Value keyOrderTest = IRBuilder::make(Opcode::HasKeyOrder, { object.object, object.guard });
    keyOrderTest.setLayout(layout);
    keyOrderTest.setFields(fields);
    guards.append(emitProvenBranch(m_builder.append(WTF::move(keyOrderTest)), notTaken));

    if (!receiverStateOf(program(), layout).impossibleMutations.containsAll(mutationsCreatingExtraProperties))
        guards.append(emitProvenBranch(m_builder.append(Opcode::HasNoExtraProperties, { object.object, object.guard }), notTaken));
    return m_builder.appendAllGuards(guards.span());
}

ValueID Emission::appendSlotRead(Opcode opcode, const TestedObject& object, LayoutID layout, uint32_t field)
{
    Value read = IRBuilder::make(opcode, { object.object, object.guard });
    read.setKey(program().at(layout).fields[field].key);
    read.setReceivers(layoutSetOf(singleElementSpan(layout)));
    read.setLevel(object.guard ? KnowledgeLevel::Expected : KnowledgeLevel::Proven);
    return m_builder.append(WTF::move(read));
}

ValueID Emission::appendFieldRead(const TestedObject& object, LayoutID layout, uint32_t field)
{
    return appendSlotRead(Opcode::GetSlot, object, layout, field);
}

ValueID Emission::appendFieldReadOrUndefined(const TestedObject& object, LayoutID layout, uint32_t field)
{
    return appendSlotRead(Opcode::GetSlotOrUndefined, object, layout, field);
}

ValueID Emission::emitProvenBranch(ValueID test, BlockID notTaken, BlockFrequency whenTaken)
{
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, 0, whenTaken, m_builder.function().at(notTaken).frequency);
    m_builder.moveToEndOf(branch.notTaken);
    m_builder.emitJump(notTaken);
    m_builder.moveToEndOf(branch.taken);
    return branch.provenOnTaken;
}

ValueID Emission::emitTypeTest(ValueID value, TypeID tested, BlockID notTaken)
{
    switch (evaluateTypeTest(program(), typeOf(value), program().at(tested))) {
    case KnownBoolean::True:
        return value;
    case KnownBoolean::False:
        m_builder.emitJump(notTaken);
        m_builder.moveToEndOf(m_builder.addBlock(BlockFrequency::Rare));
        return value;
    case KnownBoolean::Unknown:
        return emitProvenBranch(m_builder.appendTypeTest(value, tested), notTaken);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID Emission::emitRequireStoredType(ValueID value, TypeID fieldType, BlockID notTaken)
{
    const Type& required = program().at(fieldType);
    if (typeOf(value).isSubsetOf(required, program()))
        return value;
    Type testable = testableStoredTypeOf({ program(), &m_inputs.flow }, typeOf(value), required, m_inputs.types);
    return emitTypeTest(value, m_inputs.types.intern(testable), notTaken);
}

bool Emission::passesOnlyUndefinedIntoStoredType(const Type& value, TypeID fieldType)
{
    return testableStoredTypeOf({ program(), &m_inputs.flow }, value, program().at(fieldType), m_inputs.types).hasOnlyPrimitives(Primitive::Undefined);
}

static Type withoutProofsAboutArrays(const Type& type)
{
    Type result = type;
    for (ClassPart& part : result.classes) {
        if (part.builtinClass != BuiltinClasses::Array)
            continue;
        part.contentsAreProven = false;
        part.provenPristine = { };
        part.arrayStorage = ArrayStorage::Other;
    }
    return result;
}

// An array may hold any plain data, so contents that are proven narrower are not taken on trust.
static bool provenContentsAdmitPlainData(const Program& program, const Type& fieldType, TypeArena& types)
{
    Type plainData = Type::fromPrimitives(Primitive::Null | Primitive::Boolean | Primitive::Number | Primitive::String)
        .unionWith(Type::builtinClass(BuiltinClasses::Array), types)
        .unionWith(Type::builtinClass(BuiltinClasses::Object), types);
    return std::ranges::all_of(fieldType.classes, [&](const ClassPart& part) {
        return !part.contentsAreProven || std::ranges::all_of(part.contents, [&](TypeID contents) {
            return plainData.isSubsetOf(program.at(contents).withoutClassProofs(), program);
        });
    });
}

TypeID Emission::fieldTypeToTest(TypeID fieldType, ProofsByOrigin proofs)
{
    const Type& type = program().at(fieldType);
    if (proofs == ProofsByOrigin::None || !provenContentsAdmitPlainData(program(), type, m_inputs.types))
        return fieldType;
    return m_inputs.types.intern(proofs == ProofsByOrigin::AboutArrays ? withoutProofsAboutArrays(type) : type.withoutClassProofs());
}

ValueID Emission::emitRequireFieldType(ValueID value, TypeID fieldType, ProofsByOrigin proofs, BlockID notTaken)
{
    TypeID testedType = fieldTypeToTest(fieldType, proofs);
    ValueID tested = emitRequireStoredType(value, testedType, notTaken);
    if (testedType == fieldType || typeOf(tested).isSubsetOf(program().at(fieldType), program()))
        return tested;
    if (m_inputs.build.validates())
        m_builder.append(makeValidateType(tested, fieldType));
    return m_builder.append(makeAssumeType(tested, fieldType));
}

void Emission::emitBranchOnOutcome(ValueID outcome, BlockID parsed, BlockID rejected, BlockID unknown)
{
    Vector<std::pair<int64_t, Target>> cases;
    cases.append({ static_cast<int64_t>(JSONOutcome::Parsed), Target { parsed, { }, { } } });
    if (rejected != unknown)
        cases.append({ static_cast<int64_t>(JSONOutcome::Rejected), Target { rejected, { }, { } } });
    m_builder.emitSwitch(outcome, WTF::move(cases), Target { unknown, { }, { } });
}

void Emission::emitJumpUnless(ValueID condition, BlockID otherwise)
{
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(condition, BlockFrequency::Normal, m_builder.function().at(otherwise).frequency);
    m_builder.moveToEndOf(blocks.notTaken);
    m_builder.emitJump(otherwise);
    m_builder.moveToEndOf(blocks.taken);
}

void Emission::emitJumpIf(ValueID condition, BlockID target)
{
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(condition, m_builder.function().at(target).frequency, BlockFrequency::Normal);
    m_builder.moveToEndOf(blocks.taken);
    m_builder.emitJump(target);
    m_builder.moveToEndOf(blocks.notTaken);
}

LayoutSetID Emission::layoutSetOf(std::span<const LayoutID> layouts)
{
    return m_inputs.types.completeLayoutSet(layouts);
}

bool Emission::fitsDeclaredType(ValueID value, const DeclaredType& declared) const
{
    return typeOf(value).isSubsetOf(program().at(declared.type), program());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
