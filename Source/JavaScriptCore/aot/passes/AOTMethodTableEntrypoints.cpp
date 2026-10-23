/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMethodTableEntrypoints.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCreateEntrypointsInternal.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowApply.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TableParameters {
    ValueID thisValue;
    Vector<ValueID, 4> arguments;
};

} // anonymous namespace

static constexpr uint32_t typedFormBit = 1u << 31;

static uint32_t encodeCountAndForm(MethodSelector selector)
{
    return selector.numberOfArguments | (selector.form == ArgumentForm::Typed ? typedFormBit : 0);
}

static MethodSelector decodeSelector(PropertyKeyID key, uint32_t countAndForm)
{
    return { key, countAndForm & ~typedFormBit, countAndForm & typedFormBit ? ArgumentForm::Typed : ArgumentForm::Boxed };
}

DerivedFunctionKey tableEntrypointKey(FunctionID method, MethodSelector selector)
{
    return { .role = FunctionRole::TableEntrypoint, .target = method, .parameter = encodeCountAndForm(selector), .node = selector.key.index() };
}

DerivedFunctionKey tableLookupKey(MethodSelector selector)
{
    return { .role = FunctionRole::TableLookup, .target = FunctionID(), .parameter = encodeCountAndForm(selector), .node = selector.key.index() };
}

MethodSelector selectorOfTableFunction(const Function& function)
{
    RELEASE_ASSERT(function.role == FunctionRole::TableLookup || function.role == FunctionRole::TableEntrypoint);
    return decodeSelector(PropertyKeyID(function.derivation.node), function.derivation.parameter);
}

static void forEachMethodNamed(const Program& program, PropertyKeyID key, const Invocable<void(const Function&)> auto& functor)
{
    LayoutID last;
    for (LayoutID declarer : program.declarersOfName(key)) {
        if (declarer == last)
            continue;
        last = declarer;
        const ClassFlow& flow = program.at(declarer).classFlow;
        for (const Vector<ClassMember>* members : { &flow.members, &flow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.key == key && member.side == ClassSide::Prototype && member.kind == MethodKind::Method && member.function)
                    functor(program.at(member.function));
            }
        }
    }
}

static bool definesMethod(const Program& program, LayoutID declarer, PropertyKeyID key, FunctionID method)
{
    const ClassFlow& flow = program.at(declarer).classFlow;
    for (const Vector<ClassMember>* members : { &flow.members, &flow.namedMembers }) {
        for (const ClassMember& member : *members) {
            if (member.key == key && member.side == ClassSide::Prototype)
                return member.kind == MethodKind::Method && member.function == method;
        }
    }
    return false;
}

LayoutList receiversOfTableEntrypoint(const Program& program, PropertyKeyID key, FunctionID method)
{
    LayoutList receivers;
    std::span<const LayoutID> layouts = program.layoutsWithName(key);
    std::span<const LayoutID> declarers = program.declarersOfName(key);
    for (size_t index = 0; index < layouts.size(); ++index) {
        if (definesMethod(program, declarers[index], key, method))
            receivers.add(layouts[index]);
    }
    return receivers;
}

Vector<TypeID, 4> parameterTypesOf(const Program& program, TypeArena& types, MethodSelector selector)
{
    Vector<TypeID, 4> result(FillWith { }, selector.numberOfArguments, TypeTable::anything);
    if (selector.form == ArgumentForm::Boxed)
        return result;

    Vector<Type, 4> declared(selector.numberOfArguments);
    forEachMethodNamed(program, selector.key, [&](const Function& method) {
        size_t numberOfPositionalParameters = method.numberOfPositionalParameters();
        for (uint32_t index = 0; index < selector.numberOfArguments; ++index) {
            // An argument that goes into a list, or that the method may find among `arguments`, is any value.
            bool isDeclared = index < numberOfPositionalParameters && !method.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest });
            declared[index] = declared[index].unionWith(isDeclared ? program.at(method.parameters[index].type) : Type::anything(), types);
        }
    });
    for (uint32_t index = 0; index < selector.numberOfArguments; ++index) {
        if (!declared[index].isNever())
            result[index] = types.intern(declared[index]);
    }
    return result;
}

// An entrypoint passes on what it was given, undefined for what it was not given. The method tests a parameter whose type it only expects. Any other has to hold whatever the selector lets through.
bool canHaveTableEntrypoint(const Program& program, TypeArena& types, FunctionID method, MethodSelector selector)
{
    if (!canCallWithoutClosure(program, method))
        return false;
    return canHaveTableEntrypoint(program, types, method, parameterTypesOf(program, types, selector).span());
}

bool canHaveTableEntrypoint(const Program& program, TypeArena& types, FunctionID method, std::span<const TypeID> given)
{
    if (!canCallWithoutClosure(program, method))
        return false;
    const Vector<DeclaredType>& parameters = program.at(method).parameters;
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index].flags.contains(PositionFlags::IsExpected))
            continue;
        Type passed = index < given.size() ? types.at(given[index]) : Type::fromPrimitives(Primitive::Undefined);
        if (!passed.isSubsetOf(types.at(parameters[index].type), program))
            return false;
    }
    return true;
}

MethodSelector selectorOf(const Program& program, const Value& tableCall)
{
    RELEASE_ASSERT(tableCall.opcode == Opcode::CallTable);
    return selectorOfTableFunction(program.at(tableCall.function()));
}

static void declareTableConvention(const Program& program, Function& function, TypeArena& types, MethodSelector selector)
{
    function.flags.add(FunctionFlags::HasThis);
    function.thisType = DeclaredType { .type = types.intern(Type::object()) };
    function.length = selector.numberOfArguments;
    for (TypeID parameter : parameterTypesOf(program, types, selector))
        function.parameters.append(DeclaredType { .type = parameter });
    function.results.append(DeclaredType { .type = TypeTable::anything });
}

static TableParameters addEntryBlock(IRBuilder& builder)
{
    const Function& function = builder.function();
    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);

    TableParameters parameters;
    parameters.thisValue = builder.addParameter(block, function.thisType.type);
    for (const DeclaredType& parameter : function.parameters)
        parameters.arguments.append(builder.addParameter(block, parameter.type));
    return parameters;
}

Function buildTableEntrypoint(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& method = program.at(key.target);
    RELEASE_ASSERT(canHaveTableEntrypoint(program, inputs.types, key.target, decodeSelector(PropertyKeyID(key.node), key.parameter)));

    Function entry = createDerivedHeader(method, key.target, FunctionRole::TableEntrypoint, key.parameter);
    declareTableConvention(program, entry, inputs.types, decodeSelector(PropertyKeyID(key.node), key.parameter));
    entry.creator = CreatorRecord::Entrypoint { .target = key.target, .arguments = ArgumentPassing::Registers };

    AnalysisContext context { inputs.build, program, &inputs.flow, nullptr };
    IRBuilder builder(context, entry, inputs.types);
    builder.setOrigin({ method.position, 0 });
    TableParameters parameters = addEntryBlock(builder);

    // What the method expects of `this` says nothing about who calls it. The rows that may hold the entrypoint do.
    ValueID receiver = parameters.thisValue;
    LayoutList receivers = receiversOfTableEntrypoint(program, PropertyKeyID(key.node), key.target);
    if (method.has(FunctionFlags::HasThis) && !receivers.isEmpty()) {
        TypeID instances = inputs.types.intern(Type::forLayoutSet(inputs.types.completeLayoutSet(receivers.span())));
        if (inputs.build.validates())
            builder.append(makeValidateType(receiver, instances));
        receiver = builder.append(makeAssumeType(receiver, instances));
    }

    ValueID result = appendDirectCall(builder, {
        .callee = key.target,
        .thisValue = method.has(FunctionFlags::HasThis) ? receiver : ValueID(),
        .arguments = parameters.arguments.span(),
    });
    emitReturnOfEntrypointCall(builder, result);
    return entry;
}

Function buildTableLookup(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    PropertyKeyID name(key.node);

    Function lookup;
    lookup.kind = FunctionKind::Plain;
    lookup.role = FunctionRole::TableLookup;
    lookup.capabilities = capabilitiesOf(FunctionRole::TableLookup);
    lookup.creator = CreatorRecord::TableLookup { .key = name };
    lookup.derivation = Derivation { .source = FunctionID(), .parameter = key.parameter, .node = key.node };
    declareTableConvention(inputs.program, lookup, inputs.types, decodeSelector(name, key.parameter));

    AnalysisContext context { inputs.build, inputs.program, &inputs.flow, nullptr };
    IRBuilder builder(context, lookup, inputs.types);
    TableParameters parameters = addEntryBlock(builder);

    Value read = IRBuilder::make(Opcode::PropGet, { parameters.thisValue });
    read.setKey(name);
    ValueID callee = builder.append(WTF::move(read));

    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(callee);
    call.setThisValueOperand(parameters.thisValue);
    call.setArgumentsOperands(parameters.arguments.span());
    call.setCalleeKey(name);
    call.setIsNeverInlined(true);
    builder.emitReturn(builder.append(WTF::move(call)));
    return lookup;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
