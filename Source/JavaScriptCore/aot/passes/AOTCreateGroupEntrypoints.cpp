/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateEntrypointsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EntryParameters {
    ValueID closure;
    ValueID thisValue;
    Vector<ValueID, 8> arguments;
};

} // anonymous namespace

static TypeID unionOf(const Program& program, TypeArena& types, const CallGroupPosition& position)
{
    Type joined = Type::never();
    for (TypeID type : position.types)
        joined = joined.unionWith(program.at(type), types);
    if (position.isOmittedAtSomeSite)
        joined = joined.unionWith(program.at(TypeTable::undefined), types);
    return types.intern(joined);
}

static TypeID closureTypeOf(TypeArena& types, CalleeSetID members)
{
    Type closure = Type::never();
    closure.functions = SortedSet<FunctionID>(types.at(members).functions.span());
    return types.intern(closure);
}

static TypeID returnedTypeOf(const Program& program, const FlowResults& flow, TypeArena& types, FunctionID version, EntrypointResultTest resultTest)
{
    const Function& function = program.at(version);
    const DeclaredType& declared = function.results[0];
    if (resultTest == EntrypointResultTest::LeftToReceiver)
        return declared.type;
    switch (resultCheckOf(flow, version)) {
    case ResultCheck::ByCallee:
        return declared.type;
    case ResultCheck::ByCallers: {
        TypingContext written { program, function, types, TypeLevel::Written };
        return types.intern(written.typeOf(declared).withoutProofs());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool areGroupTypesInside(const Program& program, const GroupSignature& signature, const Function& callee)
{
    auto isInside = [&](TypeID type, const DeclaredType& declared) {
        return program.at(type).isSubsetOf(program.at(declared.type), program);
    };

    if (callee.has(FunctionFlags::HasThis) && !isInside(signature.thisType, callee.thisType))
        return false;
    for (unsigned index = 0; index < callee.numberOfPositionalParameters(); ++index) {
        if (!isInside(signature.parameters[index], callee.parameters[index]))
            return false;
    }
    return true;
}

static CallGroupPosition collectReturnedTypes(const Program& program, const FlowResults& flow, TypeArena& types, const CallGroups& groups, const GroupSignature& signature)
{
    CallGroupPosition returned;
    for (FunctionID member : types.at(signature.members).functions) {
        EntrypointResultTest resultTest = groups.leavesResultTestToReceiver(member) ? EntrypointResultTest::LeftToReceiver : EntrypointResultTest::MadeByEntrypoint;
        FunctionID callee = versionToEnter(program, flow, member, signature.members);
        returned.types.add(returnedTypeOf(program, flow, types, callee, resultTest));
        if (areGroupTypesInside(program, signature, program.at(callee)))
            continue;

        // To convert an argument, appendDirectCall() enters the generic version and joins the result of an async function with a rejected promise.
        if (FunctionID genericVersion = flow.function(callee).genericVersion)
            returned.types.add(returnedTypeOf(program, flow, types, genericVersion, resultTest));
        if (rejectsOnFailedConversion(program, callee))
            returned.types.add(types.intern(Type::builtinClass(BuiltinClasses::Promise)));
    }
    return returned;
}

GroupSignature createGroupSignature(const Program& program, const FlowResults& flow, TypeArena& types, const CallGroups& groups, const CallGroup& group)
{
    GroupSignature signature;
    signature.members = types.intern(CalleeSet { .functions = group.members, .builtins = { }, .isComplete = true });
    if (group.someMemberTakesThis)
        signature.thisType = unionOf(program, types, group.thisValue);
    for (const CallGroupPosition& argument : group.arguments)
        signature.parameters.append(unionOf(program, types, argument));
    signature.result = unionOf(program, types, collectReturnedTypes(program, flow, types, groups, signature));
    return signature;
}

static Function createHeader(TypeArena& types, const Function& target, FunctionID targetID, const GroupSignature& signature)
{
    Function entry = createDerivedHeader(target, targetID, FunctionRole::GroupEntrypoint, 0);
    entry.captures.append(DeclaredType { .type = closureTypeOf(types, signature.members) });
    if (signature.thisType) {
        entry.flags.add(FunctionFlags::HasThis);
        entry.thisType = DeclaredType { .type = signature.thisType };
    }
    for (TypeID parameter : signature.parameters)
        entry.parameters.append(DeclaredType { .type = parameter });
    entry.results.append(DeclaredType { .type = signature.result });
    entry.creator = CreatorRecord::Entrypoint { .target = targetID, .group = signature.members, .arguments = ArgumentPassing::Registers };
    return entry;
}

static EntryParameters addEntryBlock(IRBuilder& builder)
{
    const Function& entry = builder.function();
    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);

    EntryParameters parameters;
    parameters.closure = builder.addParameter(block, entry.captures[0].type);
    if (entry.has(FunctionFlags::HasThis))
        parameters.thisValue = builder.addParameter(block, entry.thisType.type);
    for (const DeclaredType& parameter : entry.parameters)
        parameters.arguments.append(builder.addParameter(block, parameter.type));
    return parameters;
}

Function createGroupEntrypoint(const AnalysisContext& context, TypeArena& types, FunctionID targetID, const GroupSignature& signature, EntrypointResultTest resultTest)
{
    RELEASE_ASSERT(context.flow);
    const Program& program = context.program;
    const FlowResults& flow = *context.flow;
    const Function& target = program.at(targetID);

    Function entry = createHeader(types, target, targetID, signature);
    IRBuilder builder(context, entry, types);
    builder.setOrigin({ target.position, 0 });
    EntryParameters parameters = addEntryBlock(builder);
    TypeID closureType = types.intern(Type::forFunction(targetID));
    if (context.build.validates())
        builder.append(makeValidateType(parameters.closure, closureType));
    ValueID closure = builder.append(makeAssumeType(parameters.closure, closureType));

    FunctionID callee = versionToEnter(program, flow, targetID, signature.members);
    if (!program.at(callee).hasBody()) {
        builder.emitUnreachable();
        return entry;
    }

    Vector<ValueID, 8> captures = appendCaptures(builder, targetID, closure);
    size_t numberOfArguments = std::min<size_t>(parameters.arguments.size(), target.numberOfPositionalParameters());
    ValueID result = appendDirectCall(builder, {
        .callee = callee,
        .captures = captures.span(),
        .thisValue = target.has(FunctionFlags::HasThis) ? parameters.thisValue : ValueID(),
        .calleeValue = target.has(FunctionFlags::TakesCallee) ? closure : ValueID(),
        .arguments = parameters.arguments.span().first(numberOfArguments),
        .resultCheck = resultCheckOf(flow, callee),
    });
    emitReturnOfEntrypointCall(builder, flow, callee, result, resultTest);
    return entry;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
