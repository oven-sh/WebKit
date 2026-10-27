/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTEffectsCheck.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTResolveCallsCallees.h"
#include "AOTSort.h"
#include "AOTSplitStringWalk.h"
#include "AOTSplitUnions.h"
#include "AOTStringArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "AOTWholeLiterals.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LiteralMethodCall {
    BuiltinID method;
    ValueID receiver;
    Vector<ValueID, 4> arguments;
};

struct SearchOfLiteral {
    ValueID call;
    ValueID searched;
    Vector<ValueID, 8> elements;
    Vector<KnownValue, 8> constants;
    bool answersIndex { false };
};

struct ConcatenationOfLiterals {
    ValueSite call;
    Vector<ValueID, 8> elements;
};

} // anonymous namespace

static constexpr std::array searchMethods { Builtins::ArrayPrototypeIncludes, Builtins::ArrayPrototypeIndexOf };
static constexpr std::array concatenationMethods { Builtins::ArrayPrototypeConcat };

static BuiltinID findMethodImplementedBy(const Program& program, FunctionID libraryFunction, std::span<const NamedBuiltin> methods)
{
    RuntimeFunctionID runtimeFunction = program.at(libraryFunction).enclosingRuntimeFunction;
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, runtimeFunction);
    if (!entry || entry->function != libraryFunction)
        return { };
    const BuiltinTable& builtins = program.builtins();
    for (BuiltinID builtin : builtins.at(runtimeFunction).implements) {
        if (std::ranges::contains(methods, builtins.canonicalID(builtin)))
            return builtins.canonicalID(builtin);
    }
    return { };
}

static std::optional<LiteralMethodCall> findCallOfLibraryFunction(const Program& program, const Function& function, const Value& call, std::span<const NamedBuiltin> methods)
{
    BuiltinID method = findMethodImplementedBy(program, call.function(), methods);
    const Function& target = program.at(call.function());
    std::optional<unsigned> thisParameter = target.thisParameterIndex();
    if (!method || !thisParameter || target.has(FunctionFlags::HasRestParameter) || target.hiddenRestParameterIndex() || target.argumentCountParameterIndex())
        return std::nullopt;

    std::span<const ValueID> operands = call.argumentsOperands();
    Vector<ValueID, 4> arguments(operands.subspan(target.firstParameterIndex(), target.numberOfPositionalParameters()));
    while (!arguments.isEmpty() && program.typeOf(function, arguments.last()).hasOnlyPrimitives(Primitive::Undefined) && function.at(arguments.last()).opcode == Opcode::Undefined)
        arguments.removeLast();
    return LiteralMethodCall { method, operands[*thisParameter], WTF::move(arguments) };
}

static std::optional<LiteralMethodCall> findMethodCall(const Program& program, const Function& function, const Value& value, std::span<const NamedBuiltin> methods)
{
    const BuiltinTable& builtins = program.builtins();
    std::optional<LiteralMethodCall> call;
    switch (value.opcode) {
    case Opcode::CallBuiltin: {
        const BuiltinOverload& overload = builtins.at(value.overload());
        if (value.builtinCallForm() == BuiltinCallForm::Whole && overload.mode == BuiltinCallMode::Call && value.receiverOperand())
            call = LiteralMethodCall { builtins.canonicalID(overload.builtin), value.receiverOperand(), Vector<ValueID, 4>(value.argumentsOperands()) };
        break;
    }
    case Opcode::Call:
        if (BuiltinID callee = singleBuiltinOf(program.typeOf(function, value.calleeOperand())); callee && value.thisValueOperand() && !value.expectedResult())
            call = LiteralMethodCall { builtins.canonicalID(callee), value.thisValueOperand(), Vector<ValueID, 4>(value.argumentsOperands()) };
        break;
    case Opcode::CallDirect:
        return findCallOfLibraryFunction(program, function, value, methods);
    default:
        break;
    }
    if (!call || !std::ranges::contains(methods, call->method))
        return std::nullopt;
    return call;
}

Value makeConstant(KnownValue constant)
{
    Value value;
    switch (constant.kind()) {
    case KnownValue::Kind::Undefined:
        return IRBuilder::make(Opcode::Undefined);
    case KnownValue::Kind::Null:
        return IRBuilder::make(Opcode::Null);
    case KnownValue::Kind::Boolean:
        value = IRBuilder::make(Opcode::Boolean);
        value.setBoolean(constant.asBoolean());
        return value;
    case KnownValue::Kind::Integer:
        value = IRBuilder::make(Opcode::Integer);
        value.setInteger(constant.asInteger());
        return value;
    case KnownValue::Kind::Double:
        value = IRBuilder::make(Opcode::Double);
        value.setNumber(constant.asNumber());
        return value;
    case KnownValue::Kind::String:
        value = IRBuilder::make(Opcode::String);
        value.setString(constant.asString());
        return value;
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void demoteIfReturned(IRBuilder& builder, ProgramPoint call)
{
    const Block& block = builder.function().at(call.block);
    if (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == block.values[call.index])
        builder.demoteReturnCall(call.block);
}

static std::optional<std::span<const ValueID>> findElementsOfLiteralFor(FunctionPassContext& context, ValueID operand, ValueID user)
{
    std::optional<ValueID> definition = findSingleUseDefinition(context, operand, user);
    if (!definition || !context.analyses.useLists().singleUse(*definition))
        return std::nullopt;
    const Value& literal = context.function.at(*definition);
    if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
        return std::nullopt;
    return literal.elementsOperands();
}

static bool isUsedBy(FunctionPassContext& context, ValueID operand, ValueID user, ValueID literal)
{
    std::optional<ValueID> definition = findSingleUseDefinition(context, operand, user);
    return definition == literal && context.analyses.useLists().singleUse(literal);
}

std::optional<Vector<KnownValue, 8>> findConstantsOfListFor(FunctionPassContext& context, ValueID operand, ValueID user)
{
    const Program& program = context.program;
    const Function& function = context.function;
    if (std::optional<Vector<KnownValue, 8>> constants = findConstantsOfWholeList(program, context.inputs.flow, function, operand))
        return constants;

    ValueID id = sameObjectThroughCasts(function, operand);
    const Value& literal = function.at(id);
    if (!isUsedBy(context, operand, user, id))
        return std::nullopt;
    Vector<KnownValue, 8> constants;
    if (literal.opcode == Opcode::Data) {
        const DataNode::Array* list = program.at(literal.data()).root.tryAs<DataNode::Array>();
        if (!list)
            return std::nullopt;
        for (const DataNode& element : list->elements)
            constants.append(knownValueOf(element));
    } else if (literal.opcode == Opcode::NewArray && literal.holes().empty() && !literal.capacityOperand()) {
        for (ValueID element : literal.elementsOperands())
            constants.append(knownValueOf(program, function, element));
    } else
        return std::nullopt;

    bool holdsPrimitives = std::ranges::all_of(constants, [](KnownValue constant) {
        return constant.isKnown() && !constant.isObject();
    });
    if (!holdsPrimitives)
        return std::nullopt;
    return constants;
}

static bool isFoundByStrictEquality(const Program& program, const Function& function, ValueID searched, std::span<const ValueID> elements)
{
    if (!program.typeOf(function, searched).mayBeNaN())
        return true;
    return std::ranges::none_of(elements, [&](ValueID element) {
        return program.typeOf(function, element).mayBeNaN();
    });
}

static std::optional<SearchOfLiteral> matchSearchOfLiteral(FunctionPassContext& context, ValueID id)
{
    const Program& program = context.program;
    const Function& function = context.function;
    std::optional<LiteralMethodCall> call = findMethodCall(program, function, function.at(id), searchMethods);
    if (!call || call->arguments.size() != 1)
        return std::nullopt;

    ValueID searched = call->arguments[0];
    bool answersIndex = call->method == Builtins::ArrayPrototypeIndexOf;
    if (std::optional<Vector<KnownValue, 8>> constants = findConstantsOfListFor(context, call->receiver, id)) {
        bool holdsNaN = std::ranges::any_of(*constants, [](KnownValue constant) {
            return constant.kind() == KnownValue::Kind::Double && std::isnan(constant.asNumber());
        });
        if (constants->size() > context.limit(LimitID::MaxInlineSearchedElements) || (!answersIndex && holdsNaN && program.typeOf(function, searched).mayBeNaN()))
            return std::nullopt;
        return SearchOfLiteral { id, searched, { }, WTF::move(*constants), answersIndex };
    }

    std::optional<std::span<const ValueID>> elements = findElementsOfLiteralFor(context, call->receiver, id);
    if (!elements || elements->size() > context.limit(LimitID::MaxInlineSearchedElements))
        return std::nullopt;
    if (!answersIndex && !isFoundByStrictEquality(program, function, searched, *elements))
        return std::nullopt;
    return SearchOfLiteral { id, searched, Vector<ValueID, 8>(*elements), { }, answersIndex };
}

static void emitComparisons(IRBuilder& builder, const SearchOfLiteral& search, ProgramPoint call)
{
    auto answer = [&](std::optional<size_t> index) {
        if (search.answersIndex)
            return builder.appendInteger(index ? static_cast<int64_t>(*index) : -1);
        return builder.appendBoolean(!!index);
    };

    demoteIfReturned(builder, call);
    IRBuilder::Expansion cut = builder.beginExpansion(call);
    size_t numberOfElements = search.elements.size() + search.constants.size();
    for (size_t index = 0; index < numberOfElements; ++index) {
        ValueID element = search.constants.isEmpty() ? search.elements[index] : builder.append(makeConstant(search.constants[index]));
        ValueID isEqual = builder.append(Opcode::StrictEq, { element, search.searched });
        IRBuilder::BranchBlocks blocks = builder.emitBranch(isEqual);
        builder.moveToEndOf(blocks.taken);
        builder.endExpansion(cut, answer(index));
        builder.moveToEndOf(blocks.notTaken);
    }
    builder.endExpansion(cut, answer(std::nullopt));
}

unsigned simplifySearchesOfLiterals(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<std::pair<ProgramPoint, SearchOfLiteral>> searches;
    for (const ValueSite& site : context.sitesIn(SiteList::ConstantListCalls)) {
        if (std::optional<SearchOfLiteral> search = matchSearchOfLiteral(context, site.value))
            searches.append({ site.point(), WTF::move(*search) });
    }
    if (searches.isEmpty())
        return 0;

    AOT::sort(searches, std::ranges::greater { }, [](const auto& search) {
        return std::pair { search.first.block, search.first.index };
    });
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const auto& [call, search] : searches)
        emitComparisons(builder, search, call);
    builder.setHandler(std::nullopt);
    return static_cast<unsigned>(searches.size());
}

static bool isConstructionOfCollection(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.receiverOperand() || call.argumentsOperands().size() != 1)
        return false;
    const BuiltinTable& builtins = program.builtins();
    const BuiltinOverload& overload = builtins.at(call.overload());
    return overload.mode == BuiltinCallMode::Construct && builtins.canonicalID(overload.builtin) == builtins.at(BuiltinClasses::Set).constructor;
}

unsigned takeConstantListsApart(FunctionPassContext& context)
{
    Function& function = context.function;
    struct Use {
        ValueSite site;
        Vector<KnownValue, 8> constants;
    };
    Vector<Use> uses;
    context.forEachSiteIn(SiteList::ConstantListSpreads, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        ValueID list;
        if (value.opcode == Opcode::AppendSpread)
            list = value.iterableOperand();
        else if (isConstructionOfCollection(context.program, value))
            list = value.argumentsOperands()[0];
        std::optional<Vector<KnownValue, 8>> constants = list ? findConstantsOfListFor(context, list, site.value) : std::nullopt;
        if (!constants || constants->isEmpty() || (value.opcode != Opcode::AppendSpread && isUsedBy(context, list, site.value, sameObjectThroughCasts(function, list))))
            return;
        if (!context.exceeds(LimitID::MaxArrayLiteralBatch, constants->size(), site.value))
            uses.append({ site, WTF::move(*constants) });
    });
    if (uses.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const Use& use : uses) {
        builder.setOrigin(function.at(use.site.value).origin());
        Vector<ValueID, 8> elements;
        for (KnownValue constant : use.constants)
            elements.append(builder.insertBefore(use.site.point(), makeConstant(constant)));

        if (function.at(use.site.value).opcode == Opcode::AppendSpread) {
            ValueID array = function.at(use.site.value).arrayOperand();
            for (ValueID element : elements.span().first(elements.size() - 1))
                builder.insertBefore(use.site.point(), IRBuilder::make(Opcode::AppendElement, { array, element }));
            Value last = IRBuilder::make(Opcode::AppendElement, { array, elements.last() });
            last.setOrigin(function.at(use.site.value).origin());
            last.type = resultTypeOf(context.program, function, last, context.types);
            function.edit(use.site.value) = WTF::move(last);
            continue;
        }
        Value literal = IRBuilder::make(Opcode::NewArray);
        literal.setContents(TypeTable::anything);
        literal.setElementsOperands(elements.span());
        ValueID list = builder.insertBefore(use.site.point(), WTF::move(literal));
        Value& construction = function.edit(use.site.value);
        construction.setOperand(construction.numberOfOperands() - 1, list);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return static_cast<unsigned>(uses.size());
}

static std::optional<ConcatenationOfLiterals> matchConcatenationOfLiterals(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    ValueID id = site.value;
    std::optional<LiteralMethodCall> call = findMethodCall(program, function, function.at(id), concatenationMethods);
    if (!call)
        return std::nullopt;

    std::optional<std::span<const ValueID>> first = findElementsOfLiteralFor(context, call->receiver, id);
    if (!first)
        return std::nullopt;
    ConcatenationOfLiterals match { site, Vector<ValueID, 8>(*first) };
    for (ValueID argument : call->arguments) {
        if (!program.typeOf(function, argument).hasObjects()) {
            match.elements.append(argument);
            continue;
        }
        if (std::ranges::count(call->arguments, argument) != 1 || argument == call->receiver)
            return std::nullopt;
        std::optional<std::span<const ValueID>> elements = findElementsOfLiteralFor(context, argument, id);
        if (!elements)
            return std::nullopt;
        match.elements.append(*elements);
    }
    return match;
}

static TypeID elementTypeOf(const Program& program, const Type& array)
{
    const ClassPart* part = array.findClassPart(BuiltinClasses::Array);
    return part ? part->writtenContentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything;
}

static AllocationSiteID allocationSiteOf(const Value& call)
{
    return call.opcode == Opcode::CallBuiltin ? call.allocationSite() : AllocationSiteID();
}

unsigned simplifyConcatenationsOfLiterals(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<ConcatenationOfLiterals> matches;
    for (const ValueSite& site : context.sitesIn(SiteList::ConstantListCalls)) {
        if (std::optional<ConcatenationOfLiterals> match = matchConcatenationOfLiterals(context, site))
            matches.append(WTF::move(*match));
    }
    if (matches.isEmpty())
        return 0;

    unsigned rewrites = 0;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const ConcatenationOfLiterals& match : matches) {
        ValueID call = match.call.value;
        Value literal = IRBuilder::make(Opcode::NewArray);
        literal.setContents(elementTypeOf(context.program, context.program.typeOf(function, call)));
        literal.setElementsOperands(match.elements.span());
        literal.setAllocationSite(allocationSiteOf(function.at(call)));
        if (!keepsTypeOf(context, call, literal) || context.exceeds(LimitID::MaxArrayLiteralBatch, match.elements.size(), call))
            continue;
        demoteIfReturned(builder, match.call.point());
        replaceKeepingType(context, call, WTF::move(literal));
        ++rewrites;
    }
    if (rewrites)
        context.analyses.didChangeValues();
    return rewrites;
}

static Vector<StringID, 8> splitText(FunctionPassContext& context, StringID text, StringID separator)
{
    Vector<char16_t> textBuffer;
    Vector<char16_t> separatorBuffer;
    StringView whole = context.program.strings.viewOf(text, textBuffer);
    StringView mark = context.program.strings.viewOf(separator, separatorBuffer);

    Vector<StringID, 8> pieces;
    unsigned start = 0;
    for (size_t found = whole.find(mark, 0); found != notFound; found = whole.find(mark, start)) {
        pieces.append(context.strings.intern(whole.substring(start, found - start)));
        start = found + mark.length();
    }
    pieces.append(context.strings.intern(whole.substring(start)));
    return pieces;
}

unsigned simplifySplitsOfConstants(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    const Program& program = context.program;
    Function& function = context.function;
    struct Split {
        ValueSite site;
        Vector<StringID, 8> pieces;
    };
    Vector<Split> splits;
    for (const ValueSite& site : calls) {
        std::optional<SplitString> split = splitStringOf(context, function.at(site.value));
        if (!split)
            continue;
        StringID text = program.typeOf(function, split->string).singleStringLiteral();
        StringID separator = program.typeOf(function, split->separator).singleStringLiteral();
        if (!text || !separator || program.string(text).size() > context.limit(LimitID::MaxFoldedStringBytes))
            continue;
        Vector<StringID, 8> pieces = splitText(context, text, separator);
        if (!context.exceeds(LimitID::MaxArrayLiteralBatch, pieces.size(), site.value))
            splits.append({ site, WTF::move(pieces) });
    }
    if (splits.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<Value> literals;
    for (const Split& split : splits) {
        builder.setOrigin(function.at(split.site.value).origin());
        Vector<ValueID, 8> elements;
        for (StringID piece : split.pieces)
            elements.append(builder.insertBefore(split.site.point(), makeConstant(KnownValue::string(piece))));
        Value literal = IRBuilder::make(Opcode::NewArray);
        literal.setContents(elementTypeOf(program, program.typeOf(function, split.site.value)));
        literal.setElementsOperands(elements.span());
        literal.setAllocationSite(allocationSiteOf(function.at(split.site.value)));
        literals.append(WTF::move(literal));
    }
    builder.applyInsertions();

    unsigned rewrites = 0;
    for (size_t index = 0; index < splits.size(); ++index) {
        if (!keepsTypeOf(context, splits[index].site.value, literals[index]))
            continue;
        replaceKeepingType(context, splits[index].site.value, WTF::move(literals[index]));
        ++rewrites;
    }
    context.analyses.didChangeValues();
    return rewrites;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
