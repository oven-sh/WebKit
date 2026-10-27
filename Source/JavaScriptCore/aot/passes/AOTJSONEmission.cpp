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
#include "AOTKnownValue.h"
#include "AOTMethodDispatch.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSelectTypedResultsNodes.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// A field that is redefined, or stored with a value its slot cannot hold, moves to the extra properties and keeps its place in the key order.
static constexpr OptionSet<ObjectMutation> mutationsCreatingExtraProperties {
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::StoreOfOtherType,
};

// Reading a provisional callee builds it, and a recursive node would wait for the function that is being built.
static std::optional<DerivedSignature> signatureOfProvisionalCallee(Emission& emission, FunctionID callee)
{
    if (!ProvisionalFunctions::contains(callee))
        return std::nullopt;
    DerivedFunctionKey key = emission.inputs().functions.keyOf(callee);
    switch (key.role) {
    case FunctionRole::Serializer:
        return signatureOfSerializer(emission.inputs());
    case FunctionRole::Parser:
        return signatureOfParser(emission.inputs(), describeNode(emission.program(), nodeOfKey(key)));
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

EmissionInputs::EmissionInputs(FunctionPassContext& context)
    : program(context.program)
    , flow(context.inputs.flow)
    , types(context.types)
    , strings(context.strings)
    , build(context.build)
    , functions { &context, context.inputs.derivedFunctions }
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
    , functions { nullptr, &inputs.derivedFunctions }
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
    function.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    for (TypeID type : signature.parameters) {
        function.parameters.append(DeclaredType(Proven::byPosition(type)));
        function.derivation.parameterMap.append(ParameterOrigin { });
    }
    for (TypeID type : signature.results)
        function.results.append(DeclaredType(Proven::byPosition(type)));

    IRBuilder builder(inputs.analysisContext(), function, inputs.types);
    head.entry = builder.addBlock();
    for (TypeID type : signature.parameters)
        head.parameters.append(builder.addParameter(head.entry, type));
    return head;
}

ValueID Emission::appendResultAt(ValueID results, uint32_t index)
{
    Value result = make<&Value::setIndex>(Opcode::ResultAt, { results }, index);
    const Value& producer = function().at(results);
    std::optional<DerivedSignature> signature;
    if (producer.opcode == Opcode::CallDirect)
        signature = signatureOfProvisionalCallee(*this, producer.function());
    if (!signature)
        return m_builder.append(WTF::move(result));
    return m_builder.appendWithType(WTF::move(result), signature->results[index]);
}

void Emission::emitReturnOf(std::initializer_list<ValueID> values)
{
    RELEASE_ASSERT(values.size());
    m_builder.emitReturn(values.size() == 1 ? *values.begin() : m_builder.append(Opcode::Results, values));
}

ValueID Emission::appendCallOf(FunctionID function, std::initializer_list<ValueID> arguments)
{
    Value call = make<&Value::setFunction, &Value::setArgumentsOperands>(Opcode::CallDirect, { }, function, std::span<const ValueID> { arguments.begin(), arguments.size() });
    std::optional<DerivedSignature> signature = signatureOfProvisionalCallee(*this, function);
    if (!signature)
        return m_builder.append(WTF::move(call));
    return m_builder.appendWithType(WTF::move(call), signature->results.size() == 1 ? signature->results[0] : TypeTable::undefined);
}

ValueID Emission::appendHelperCall(BuiltinID helper, std::initializer_list<ValueID> arguments)
{
    BuiltinTableIDRange<BuiltinOverloadID> overloads = program().builtins().overloadIDsOf(helper);
    RELEASE_ASSERT(overloads.size() == 1);
    return m_builder.append(makeBuiltinCall(overloads.at(0), BuiltinCallForm::Whole, { { }, Vector<ValueID, 4>(arguments) }, { }));
}

Emission::TestedObject Emission::emitShapeTest(ValueID value, std::span<const LayoutID> layouts, BlockID notTaken, Expected<BlockFrequency> whenTaken)
{
    LayoutSetID tested = layoutSetOf(layouts);
    ValueID object = emitTypeTest(value, TypeTable::object, notTaken);
    const Type& type = typeOf(object);
    if (type.hasOnlyLayouts() && isSubsetOf(program().at(type.layouts), program().at(tested)))
        return { object, { } };
    return { object, emitProvenBranch(m_builder.append(make<&Value::setLayouts>(Opcode::ShapeTest, { object }, tested)), notTaken, whenTaken) };
}

ValueID Emission::emitKeyOrderTest(const TestedObject& object, LayoutID layout, std::span<const uint32_t> fields, BlockID notTaken)
{
    Vector<ValueID, 3> guards;
    if (object.guard)
        guards.append(object.guard);

    Value keyOrderTest = make<&Value::setLayout, &Value::setFields>(Opcode::HasKeyOrder, { object.object, object.guard }, layout, fields);
    guards.append(emitProvenBranch(m_builder.append(WTF::move(keyOrderTest)), notTaken));

    if (!receiverStateOf(program(), layout).impossibleMutations.containsAll(mutationsCreatingExtraProperties))
        guards.append(emitProvenBranch(m_builder.append(Opcode::HasNoExtraProperties, { object.object, object.guard }), notTaken));
    return m_builder.appendAllGuards(guards.span());
}

ValueID Emission::appendFieldRead(const TestedObject& object, LayoutID layout, uint32_t field, Opcode opcode)
{
    Value read = IRBuilder::make(opcode, { object.object, object.guard });
    read.setKey(program().at(layout).fields[field].key);
    read.setReceivers(layoutSetOf(singleElementSpan(layout)));
    read.setLevel(object.guard ? KnowledgeLevel::Expected : KnowledgeLevel::Proven);
    return m_builder.append(WTF::move(read));
}

ValueID Emission::emitProvenBranch(ValueID test, BlockID notTaken, Expected<BlockFrequency> whenTaken)
{
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, 0, whenTaken, function().at(notTaken).frequency);
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

// An array may hold any plain data, so contents that are proven narrower are not taken on trust.
static bool provenContentsAdmitPlainData(const Program& program, const Type& fieldType, TypeArena& types)
{
    Type plainData = Type::fromPrimitives(Primitive::Null | Primitive::Boolean | Primitive::Number | Primitive::String)
        .unionWith(Type::builtinClass(BuiltinClasses::Array), types)
        .unionWith(Type::builtinClass(BuiltinClasses::Object), types);
    return std::ranges::all_of(fieldType.classes, [&](const ClassPart& part) {
        return !part.contentsAreProven || std::ranges::all_of(part.writtenContents, [&](TypeID contents) {
            return plainData.isSubsetOf(program.at(contents).withoutClassProofs(), program);
        });
    });
}

TypeID Emission::fieldTypeToTest(TypeID fieldType)
{
    const Type& type = program().at(fieldType);
    if (!provenContentsAdmitPlainData(program(), type, m_inputs.types))
        return fieldType;
    return m_inputs.types.intern(type.withoutClassProofs());
}

ValueID Emission::emitRequireFieldType(ValueID value, TypeID fieldType, BlockID notTaken)
{
    TypeID testedType = fieldTypeToTest(fieldType);
    ValueID tested = emitRequireStoredType(value, testedType, notTaken);
    if (testedType == fieldType || typeOf(tested).isSubsetOf(program().at(fieldType), program()))
        return tested;
    if (m_inputs.build.validates())
        m_builder.append(makeValidateType(tested, fieldType));
    return m_builder.append(makeAssumeType(tested, Proven::byConstruction(fieldType)));
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
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(condition, BlockFrequency::Normal, function().at(otherwise).frequency);
    m_builder.moveToEndOf(blocks.notTaken);
    m_builder.emitJump(otherwise);
    m_builder.moveToEndOf(blocks.taken);
}

void Emission::emitJumpIf(ValueID condition, BlockID target)
{
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(condition, function().at(target).frequency, BlockFrequency::Normal);
    m_builder.moveToEndOf(blocks.taken);
    m_builder.emitJump(target);
    m_builder.moveToEndOf(blocks.notTaken);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
