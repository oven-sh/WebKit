/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinMembers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPlainURL.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSplitUnions.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class UnbuiltClass : bool { URL, SearchParams };

struct ReadOfMember {
    ValueID read;
    BuiltinID member;
};

struct CallOfMember {
    ValueID call;
    BuiltinID member;
    ValueID readOfMember;
    ValueID readOfSearchParams;
};

struct UnbuiltObject {
    UnbuiltClass builtinClass;
    ValueID construction;
    Vector<ValueID, 8> valuesLeftWithoutUsers;
    Vector<ValueID, 4> testsThatPass;
    Vector<ReadOfMember, 4> reads;
    Vector<CallOfMember, 4> calls;
    Vector<ValueID, 2> walks;
};

struct Parts {
    ValueID objectOrUndefined;
    ValueID text;
    ValueID packed;
    Value construction;
};

struct ReplayedCall {
    std::optional<Value> readOfSearchParams;
    std::optional<Value> readOfMember;
    Value call;
};

} // anonymous namespace

static BuiltinID memberOfInstances(FunctionPassContext& context, BuiltinClassID builtinClass, PropertyKeyID key)
{
    MemberAnswer answer = findBuiltinInstanceMember({ context.program, &context.inputs.flow }, builtinClass, key, Access::Read);
    if (answer.kind != MemberKind::Builtin || !answer.needsNoTest())
        return { };
    return context.program.builtins().canonicalID(answer.builtin);
}

static BuiltinID getterReadFrom(FunctionPassContext& context, const Value& read, ValueID receiver, BuiltinClassID builtinClass)
{
    const Program& program = context.program;
    if (read.opcode == Opcode::PropGet)
        return read.objectOperand() == receiver ? memberOfInstances(context, builtinClass, read.key()) : BuiltinID();
    if (read.opcode != Opcode::CallBuiltin || read.builtinCallForm() != BuiltinCallForm::Whole || read.receiverOperand() != receiver || !read.argumentsOperands().empty())
        return { };
    const BuiltinOverload& overload = program.builtins().at(read.overload());
    return overload.mode == BuiltinCallMode::Get ? program.builtins().canonicalID(overload.builtin) : BuiltinID();
}

static bool forEachUseThroughCasts(FunctionPassContext& context, ValueID object, Vector<ValueID, 8>& casts, NOESCAPE const Invocable<bool(const Use&, ValueID used)> auto& visit)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, 4> worklist { object };
    while (!worklist.isEmpty()) {
        ValueID current = worklist.takeLast();
        for (const Use& use : useLists.usesOf(current)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            if (isCastWithoutTest(function.at(use.user).opcode) && !use.operandIndex) {
                casts.append(use.user);
                worklist.append(use.user);
                continue;
            }
            if (!visit(use, current))
                return false;
        }
    }
    return true;
}

static bool walksPairs(BuiltinID member)
{
    return member == Builtins::URLSearchParamsPrototypeEntries || member == Builtins::URLSearchParamsPrototypeKeys || member == Builtins::URLSearchParamsPrototypeValues;
}

static std::optional<size_t> numberOfArgumentsOf(BuiltinID member, UnbuiltClass builtinClass)
{
    if (member == Builtins::URLSearchParamsPrototypeGet || member == Builtins::URLSearchParamsPrototypeHas)
        return 1;
    if (builtinClass == UnbuiltClass::SearchParams && (member == Builtins::URLSearchParamsPrototypeToString || walksPairs(member)))
        return 0;
    return std::nullopt;
}

static bool comesFrom(const Function& function, ValueID value, ValueID source)
{
    while (value != source && isCastWithoutTest(function.at(value).opcode))
        value = function.at(value).operand(0);
    return value == source;
}

static bool isOnlyWalked(FunctionPassContext& context, ValueID iterable, UnbuiltObject& object)
{
    return forEachUseThroughCasts(context, iterable, object.valuesLeftWithoutUsers, [&](const Use& use, ValueID) {
        return context.function.at(use.user).opcode == Opcode::IteratorOpen;
    });
}

static bool addCall(FunctionPassContext& context, const CallOfMember& call, UnbuiltObject& object)
{
    object.calls.append(call);
    return !walksPairs(call.member) || isOnlyWalked(context, call.call, object);
}

static bool collectCallsOfMember(FunctionPassContext& context, ValueID readOfMember, BuiltinID member, ValueID searchParams, UnbuiltObject& object)
{
    const Function& function = context.function;
    size_t numberOfArguments = *numberOfArgumentsOf(member, object.builtinClass);
    return forEachUseThroughCasts(context, readOfMember, object.valuesLeftWithoutUsers, [&](const Use& use, ValueID) {
        const Value& call = function.at(use.user);
        if (call.opcode != Opcode::Call || use.operandIndex || call.argumentsOperands().size() != numberOfArguments || !call.thisValueOperand())
            return false;
        if (!comesFrom(function, call.thisValueOperand(), searchParams))
            return false;
        if (numberOfArguments && comesFrom(function, call.argumentsOperands()[0], searchParams))
            return false;
        return addCall(context, { use.user, member, readOfMember, object.builtinClass == UnbuiltClass::URL ? searchParams : ValueID() }, object);
    });
}

static bool collectUsesOfSearchParams(FunctionPassContext& context, ValueID searchParams, UnbuiltObject& object)
{
    const Program& program = context.program;
    const Function& function = context.function;
    bool isWhole = object.builtinClass == UnbuiltClass::SearchParams;
    return forEachUseThroughCasts(context, searchParams, object.valuesLeftWithoutUsers, [&](const Use& use, ValueID used) {
        const Value& user = function.at(use.user);
        if (user.opcode == Opcode::IsPristine) {
            object.testsThatPass.append(use.user);
            return true;
        }
        if (user.opcode == Opcode::IteratorOpen && isWhole) {
            object.walks.append(use.user);
            return true;
        }
        if (user.opcode == Opcode::Call) {
            const Value& callee = function.at(sameObjectThroughCasts(function, user.calleeOperand()));
            return use.operandIndex == 1 && callee.opcode == Opcode::PropGet && comesFrom(function, callee.objectOperand(), searchParams)
                && numberOfArgumentsOf(memberOfInstances(context, BuiltinClasses::URLSearchParams, callee.key()), object.builtinClass);
        }
        if (isWhole && getterReadFrom(context, user, used, BuiltinClasses::URLSearchParams) == Builtins::URLSearchParamsPrototypeSize) {
            object.reads.append({ use.user, Builtins::URLSearchParamsPrototypeSize });
            return true;
        }
        if (user.opcode == Opcode::PropGet) {
            BuiltinID member = memberOfInstances(context, BuiltinClasses::URLSearchParams, user.key());
            if (user.objectOperand() != used || !numberOfArgumentsOf(member, object.builtinClass))
                return false;
            object.valuesLeftWithoutUsers.append(use.user);
            return collectCallsOfMember(context, use.user, member, searchParams, object);
        }
        if (user.opcode != Opcode::CallBuiltin || user.builtinCallForm() != BuiltinCallForm::Whole || user.receiverOperand() != used || std::ranges::contains(user.argumentsOperands(), used))
            return false;
        const BuiltinOverload& overload = program.builtins().at(user.overload());
        BuiltinID member = program.builtins().canonicalID(overload.builtin);
        if (overload.mode != BuiltinCallMode::Call || numberOfArgumentsOf(member, object.builtinClass) != user.argumentsOperands().size())
            return false;
        return addCall(context, { use.user, member, { }, isWhole ? ValueID() : searchParams }, object);
    });
}

static BuiltinID constructedBuiltin(FunctionPassContext& context, const Value& value)
{
    const Program& program = context.program;
    if (value.opcode == Opcode::Construct) {
        BuiltinID callee = singleBuiltinOf(program.typeOf(context.function, value.calleeOperand()));
        return callee && !value.newTargetOperand() ? program.builtins().canonicalID(callee) : BuiltinID();
    }
    if (value.opcode != Opcode::CallBuiltin || (value.builtinCallForm() != BuiltinCallForm::Whole && value.builtinCallForm() != BuiltinCallForm::AnyReceiver))
        return { };
    const BuiltinOverload& overload = program.builtins().at(value.overload());
    return overload.mode == BuiltinCallMode::Construct ? program.builtins().canonicalID(overload.builtin) : BuiltinID();
}

static bool isRequiredTailCall(FunctionPassContext& context, ValueID call)
{
    const Terminator& terminator = context.function.at(context.analyses.useLists().blockOf(call)).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call && terminator.requirement() == TailCallRequirement::Required;
}

static bool collectUsesOfURL(FunctionPassContext& context, UnbuiltObject& object)
{
    return forEachUseThroughCasts(context, object.construction, object.valuesLeftWithoutUsers, [&](const Use& use, ValueID used) {
        const Value& read = context.function.at(use.user);
        if (read.opcode == Opcode::IsPristine) {
            object.testsThatPass.append(use.user);
            return true;
        }
        BuiltinID member = getterReadFrom(context, read, used, BuiltinClasses::URL);
        if (member == Builtins::URLPrototypePathname || member == Builtins::URLPrototypeSearch) {
            object.reads.append({ use.user, member });
            return true;
        }
        if (member != Builtins::URLPrototypeSearchParams)
            return false;
        object.valuesLeftWithoutUsers.append(use.user);
        return collectUsesOfSearchParams(context, use.user, object);
    });
}

static std::optional<UnbuiltObject> matchUnbuiltObject(FunctionPassContext& context, ValueID id)
{
    const Value& construction = context.function.at(id);
    BuiltinID constructed = constructedBuiltin(context, construction);
    size_t numberOfArguments = construction.opcode == Opcode::Construct || construction.opcode == Opcode::CallBuiltin ? construction.argumentsOperands().size() : 0;
    bool isURL = constructed == Builtins::URL && numberOfArguments && numberOfArguments <= 2;
    bool isSearchParams = constructed == Builtins::URLSearchParams && numberOfArguments == 1;
    if (!isURL && !isSearchParams)
        return std::nullopt;

    UnbuiltObject object { isURL ? UnbuiltClass::URL : UnbuiltClass::SearchParams, id, { }, { }, { }, { }, { } };
    if (!(isURL ? collectUsesOfURL(context, object) : collectUsesOfSearchParams(context, id, object)))
        return std::nullopt;
    if (object.reads.isEmpty() && object.calls.isEmpty() && object.walks.isEmpty())
        return std::nullopt;
    bool hasRequiredTailCall = std::ranges::any_of(object.reads, [&](const ReadOfMember& read) {
        return isRequiredTailCall(context, read.read);
    }) || std::ranges::any_of(object.calls, [&](const CallOfMember& call) {
        return isRequiredTailCall(context, call.call);
    });
    if (hasRequiredTailCall)
        return std::nullopt;
    return object;
}

static ValueID appendCallOfHelper(FunctionPassContext& context, IRBuilder& builder, BuiltinID helper, std::initializer_list<ValueID> arguments)
{
    BuiltinOverloadID overload = *context.program.builtins().overloadIDsOf(helper).begin();
    return builder.append(makeBuiltinCall(overload, BuiltinCallForm::Whole, { { }, arguments }, { }));
}

static ValueID appendScan(FunctionPassContext& context, IRBuilder& builder, UnbuiltClass builtinClass, ValueID text, ValueID base)
{
    if (builtinClass == UnbuiltClass::SearchParams)
        return appendCallOfHelper(context, builder, Builtins::CompilerHelpersScanPlainQuery, { text });
    if (!base)
        return appendCallOfHelper(context, builder, Builtins::CompilerHelpersScanPlainURL, { text });
    const Value& baseValue = context.function.at(base);
    if (baseValue.opcode == Opcode::String && isPlainBaseURL(byteCast<Latin1Character>(context.program.strings[baseValue.string()])))
        return appendCallOfHelper(context, builder, Builtins::CompilerHelpersScanPlainURLWithPlainBase, { text });
    return appendCallOfHelper(context, builder, Builtins::CompilerHelpersScanPlainURLWithBase, { text, base });
}

static ValueID appendEmptyString(FunctionPassContext& context, IRBuilder& builder)
{
    Value constant = IRBuilder::make(Opcode::String);
    constant.setString(context.program.strings.emptyString());
    return builder.append(WTF::move(constant));
}

static ValueID appendShifted(IRBuilder& builder, ValueID packed, unsigned shift)
{
    return shift ? builder.append(Opcode::RShift, { packed, builder.appendInteger(shift) }) : packed;
}

static ValueID appendBits(IRBuilder& builder, ValueID packed, unsigned shift, int64_t mask)
{
    return builder.append(Opcode::BitAnd, { appendShifted(builder, packed, shift), builder.appendInteger(mask) });
}

static ValueID appendOffset(IRBuilder& builder, ValueID packed, unsigned shift)
{
    return appendBits(builder, packed, shift, PlainURL::maxOffset);
}

static ValueID appendRange(IRBuilder& builder, ValueID text, ValueID range)
{
    return builder.append(Opcode::Substring, { text, appendShifted(builder, range, PlainURL::bitsOfOffset), appendOffset(builder, range, 0) });
}

static IRBuilder::Expansion beginExpansionOf(FunctionPassContext& context, IRBuilder& builder, ValueID value)
{
    BlockID block = context.analyses.useLists().blockOf(value);
    const Terminator& terminator = context.function.at(block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == value)
        builder.demoteReturnCall(block);
    return builder.beginExpansion(context.analyses.useLists().pointOf(value));
}

static Parts expandConstruction(FunctionPassContext& context, IRBuilder& builder, const UnbuiltObject& unbuilt)
{
    Function& function = context.function;
    ValueID id = unbuilt.construction;
    Value construction = function.at(id);
    TypeID typeOfObject = construction.type;
    ValueID text = construction.argumentsOperands()[0];
    ValueID base = construction.argumentsOperands().size() > 1 ? construction.argumentsOperands()[1] : ValueID();

    IRBuilder::Expansion expansion = beginExpansionOf(context, builder, id);
    function.edit(id, ValueEdit::Type).type = context.types.unionOf(TypeTable::undefined, typeOfObject);
    function.edit(id, ValueEdit::Type).setExpected({ });
    ValueID textParameter = builder.addParameter(expansion.continuation, TypeTable::string);
    ValueID packedParameter = builder.addParameter(expansion.continuation, TypeTable::int32);
    expansion.results.append(textParameter);
    expansion.results.append(packedParameter);

    BlockID makesObject = builder.addBlock(BlockFrequency::Unlikely);
    ValueID string = text;
    if (!context.program.typeOf(function, text).isAnyString()) {
        IRBuilder::ProvenBranch isString = builder.emitBranchWithProof(builder.appendTypeTest(text, TypeTable::string), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        builder.moveToEndOf(isString.notTaken);
        builder.emitJump(makesObject);
        builder.moveToEndOf(isString.taken);
        string = isString.provenOnTaken;
    }
    ValueID packed = appendScan(context, builder, unbuilt.builtinClass, string, base);
    IRBuilder::BranchBlocks isPlain = builder.emitBranch(builder.append(Opcode::GreaterEq, { packed, builder.appendInteger(0) }), BlockFrequency::Normal, BlockFrequency::Unlikely);
    builder.moveToEndOf(isPlain.notTaken);
    builder.emitJump(makesObject);
    builder.moveToEndOf(isPlain.taken);
    ValueID plainResults[] { builder.appendUndefined(), string, packed };
    builder.endExpansion(expansion, plainResults);

    builder.moveToEndOf(makesObject);
    ValueID object = builder.append(Value(construction));
    ValueID objectResults[] { object, appendEmptyString(context, builder), builder.appendInteger(PlainURL::notPlain) };
    builder.endExpansion(expansion, objectResults);
    return { id, textParameter, packedParameter, WTF::move(construction) };
}

static ValueID appendWithFirstOperand(IRBuilder& builder, const Value& original, ValueID operand)
{
    Value copy = original;
    copy.setOperand(0, operand);
    return builder.append(WTF::move(copy));
}

static IRBuilder::ProvenBranch branchOnUnbuilt(IRBuilder& builder, const Parts& parts)
{
    return builder.emitBranchWithProof(builder.appendTypeTest(parts.objectOrUndefined, TypeTable::undefined), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
}

static void expandRead(FunctionPassContext& context, IRBuilder& builder, const Parts& parts, const ReadOfMember& read)
{
    Value original = context.function.at(read.read);
    IRBuilder::Expansion expansion = beginExpansionOf(context, builder, read.read);
    IRBuilder::ProvenBranch isUnbuilt = branchOnUnbuilt(builder, parts);

    builder.moveToEndOf(isUnbuilt.notTaken);
    builder.endExpansion(expansion, appendWithFirstOperand(builder, original, isUnbuilt.provenOnNotTaken));

    builder.moveToEndOf(isUnbuilt.taken);
    if (read.member == Builtins::URLSearchParamsPrototypeSize) {
        builder.endExpansion(expansion, appendShifted(builder, parts.packed, PlainURL::shiftOfPairCount));
        return;
    }
    ValueID endOfPath = appendOffset(builder, parts.packed, PlainURL::bitsOfOffset);
    if (read.member == Builtins::URLPrototypePathname) {
        builder.endExpansion(expansion, builder.append(Opcode::Substring, { parts.text, appendShifted(builder, parts.packed, 2 * PlainURL::bitsOfOffset), endOfPath }));
        return;
    }
    ValueID endOfQuery = appendOffset(builder, parts.packed, 0);
    ValueID hasQuery = builder.append(Opcode::Less, { builder.append(Opcode::Add, { endOfPath, builder.appendInteger(1) }), endOfQuery });
    IRBuilder::BranchBlocks branch = builder.emitBranch(hasQuery);
    builder.moveToEndOf(branch.taken);
    builder.endExpansion(expansion, builder.append(Opcode::Substring, { parts.text, endOfPath, endOfQuery }));
    builder.moveToEndOf(branch.notTaken);
    builder.endExpansion(expansion, appendEmptyString(context, builder));
}

static ValueID appendReplayed(IRBuilder& builder, const ReplayedCall& replayed, ValueID object)
{
    ValueID searchParams = replayed.readOfSearchParams ? appendWithFirstOperand(builder, *replayed.readOfSearchParams, object) : object;
    if (!replayed.readOfMember)
        return appendWithFirstOperand(builder, replayed.call, searchParams);
    Value call = replayed.call;
    call.setOperand(0, appendWithFirstOperand(builder, *replayed.readOfMember, searchParams));
    call.setOperand(1, searchParams);
    return builder.append(WTF::move(call));
}

static ValueID appendPairs(FunctionPassContext& context, IRBuilder& builder, const Parts& parts, BuiltinID member)
{
    BuiltinID helper = member == Builtins::URLSearchParamsPrototypeKeys ? Builtins::CompilerHelpersPlainQueryKeys
        : member == Builtins::URLSearchParamsPrototypeValues ? Builtins::CompilerHelpersPlainQueryValues : Builtins::CompilerHelpersPlainQueryEntries;
    return appendCallOfHelper(context, builder, helper, { parts.text, parts.packed });
}

static void endWithEither(FunctionPassContext& context, IRBuilder& builder, const IRBuilder::Expansion& expansion, ValueID merged, ValueID result)
{
    Function& function = context.function;
    function.edit(merged, ValueEdit::Type).type = context.types.unionOf(function.at(merged).type, function.at(result).type);
    function.edit(merged, ValueEdit::Type).setExpected({ });
    builder.endExpansion(expansion, result);
}

static void expandSearch(FunctionPassContext& context, IRBuilder& builder, const Parts& parts, const ReplayedCall& replayed, const CallOfMember& site, const IRBuilder::Expansion& expansion)
{
    ValueID name = replayed.call.argumentsOperands()[0];
    bool answersValue = site.member == Builtins::URLSearchParamsPrototypeGet;
    ValueID found = site.readOfSearchParams
        ? appendCallOfHelper(context, builder, Builtins::CompilerHelpersFindPlainQueryValue, { parts.text, parts.packed, name })
        : appendCallOfHelper(context, builder, Builtins::CompilerHelpersFindInPlainQuery, { parts.text, name });
    IRBuilder::BranchBlocks isFound = builder.emitBranch(builder.append(Opcode::GreaterEq, { found, builder.appendInteger(0) }));
    builder.moveToEndOf(isFound.taken);
    builder.endExpansion(expansion, answersValue ? appendRange(builder, parts.text, found) : builder.appendBoolean(true));

    builder.moveToEndOf(isFound.notTaken);
    IRBuilder::BranchBlocks isAbsent = builder.emitBranch(builder.append(Opcode::StrictEq, { found, builder.appendInteger(PlainURL::absent) }), BlockFrequency::Normal, BlockFrequency::Unlikely);
    builder.moveToEndOf(isAbsent.taken);
    builder.endExpansion(expansion, answersValue ? builder.append(Opcode::Null, { }) : builder.appendBoolean(false));

    builder.moveToEndOf(isAbsent.notTaken);
    builder.endExpansion(expansion, appendReplayed(builder, replayed, builder.append(Value(parts.construction))));
}

static void expandCall(FunctionPassContext& context, IRBuilder& builder, const Parts& parts, const ReplayedCall& replayed, const CallOfMember& site)
{
    IRBuilder::Expansion expansion = beginExpansionOf(context, builder, site.call);
    IRBuilder::ProvenBranch isUnbuilt = branchOnUnbuilt(builder, parts);

    builder.moveToEndOf(isUnbuilt.notTaken);
    builder.endExpansion(expansion, appendReplayed(builder, replayed, isUnbuilt.provenOnNotTaken));

    builder.moveToEndOf(isUnbuilt.taken);
    if (walksPairs(site.member))
        endWithEither(context, builder, expansion, site.call, appendPairs(context, builder, parts, site.member));
    else if (site.member == Builtins::URLSearchParamsPrototypeToString)
        builder.endExpansion(expansion, builder.append(Opcode::Substring, { parts.text, appendBits(builder, parts.packed, PlainURL::bitsOfOffset, 1), appendOffset(builder, parts.packed, 0) }));
    else
        expandSearch(context, builder, parts, replayed, site, expansion);
}

static void pointWalksAt(FunctionPassContext& context, ValueID iterable)
{
    Function& function = context.function;
    Vector<ValueID, 8> casts;
    Vector<ValueID, 2> walks;
    forEachUseThroughCasts(context, iterable, casts, [&](const Use& use, ValueID) {
        walks.append(use.user);
        return true;
    });
    for (ValueID walk : walks)
        function.edit(walk, ValueEdit::Definition).setOperand(0, iterable);
    if (!casts.isEmpty())
        context.analyses.didChangeValues();
}

static void expandWalk(FunctionPassContext& context, IRBuilder& builder, const Parts& parts, ValueID walk)
{
    Function& function = context.function;
    Value placeholder = IRBuilder::make(Opcode::Upcast, { parts.objectOrUndefined });
    placeholder.setCastType(function.at(parts.objectOrUndefined).type);
    ValueID iterable = builder.insertBefore(context.analyses.useLists().pointOf(walk), WTF::move(placeholder));
    builder.applyInsertions();
    function.edit(walk, ValueEdit::Definition).setOperand(0, iterable);
    context.analyses.didChangeValues();

    IRBuilder::Expansion expansion = beginExpansionOf(context, builder, iterable);
    IRBuilder::ProvenBranch isUnbuilt = branchOnUnbuilt(builder, parts);
    function.edit(iterable, ValueEdit::Type).type = function.at(isUnbuilt.provenOnNotTaken).type;
    builder.moveToEndOf(isUnbuilt.notTaken);
    builder.endExpansion(expansion, isUnbuilt.provenOnNotTaken);
    builder.moveToEndOf(isUnbuilt.taken);
    endWithEither(context, builder, expansion, iterable, appendPairs(context, builder, parts, Builtins::URLSearchParamsPrototypeEntries));
}

unsigned leaveURLsUnbuilt(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<UnbuiltObject> objects;
    for (const ValueSite& site : context.sitesIn(SiteList::ConstructionsAndBuiltinCalls)) {
        if (function.at(site.block).frequency != BlockFrequency::Normal)
            continue;
        if (std::optional<UnbuiltObject> object = matchUnbuiltObject(context, site.value))
            objects.append(WTF::move(*object));
    }
    if (objects.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const UnbuiltObject& object : objects) {
        auto copyOf = [&](ValueID value) {
            return value ? std::optional(function.at(value)) : std::nullopt;
        };
        Vector<ReplayedCall, 4> replayedCalls;
        for (const CallOfMember& call : object.calls) {
            replayedCalls.append({ copyOf(call.readOfSearchParams), copyOf(call.readOfMember), function.at(call.call) });
            if (walksPairs(call.member))
                pointWalksAt(context, call.call);
        }

        Parts parts = expandConstruction(context, builder, object);
        for (const ReadOfMember& read : object.reads)
            expandRead(context, builder, parts, read);
        for (size_t index = 0; index < object.calls.size(); ++index)
            expandCall(context, builder, parts, replayedCalls[index], object.calls[index]);
        for (ValueID walk : object.walks)
            expandWalk(context, builder, parts, walk);
        for (ValueID value : object.valuesLeftWithoutUsers)
            becomeUndefined(function, value);
        for (ValueID test : object.testsThatPass) {
            Value& value = function.edit(test);
            value.become(Opcode::Boolean);
            value.setBoolean(true);
            value.type = resultTypeOf(context.program, function, value, context.types);
        }
        context.analyses.didChangeValues();
    }
    context.analyses.didChangeControlFlow();
    context.build.statistics().count(StatisticID::URLsLeftUnbuilt, objects.size());
    return static_cast<unsigned>(objects.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
