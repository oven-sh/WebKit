/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectsDescription.h"
#include "AOTFlowResults.h"
#include "AOTFoldConstants.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessElements.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRegExpFlags.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTScratchRealm.h"
#include "AOTStringArena.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "TypeofType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(uint64_t, maxPrimitiveTextBytes, 24);
AOT_THRESHOLD(unsigned, maxOperationsBetweenTextAndPattern, 8);

static std::optional<TypeofType> typeofTypeNamed(std::span<const uint8_t> text)
{
    static constexpr std::pair<ASCIILiteral, TypeofType> names[] = {
        { "undefined"_s, TypeofType::Undefined },
        { "bigint"_s, TypeofType::BigInt },
        { "boolean"_s, TypeofType::Boolean },
        { "number"_s, TypeofType::Number },
        { "string"_s, TypeofType::String },
        { "symbol"_s, TypeofType::Symbol },
        { "object"_s, TypeofType::Object },
        { "function"_s, TypeofType::Function },
    };
    for (auto& [name, type] : names) {
        if (equalSpans(name.span8(), text))
            return type;
    }
    return std::nullopt;
}

static std::optional<Type> typeTestedByTypeOf(const Program& program, TypeofType result, const Type& source)
{
    switch (result) {
    case TypeofType::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case TypeofType::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case TypeofType::Boolean:
        return Type::fromPrimitives(Primitive::Boolean);
    case TypeofType::Number:
        return Type::fromPrimitives(Primitive::Number);
    case TypeofType::String:
        return Type::fromPrimitives(Primitive::String);
    case TypeofType::Symbol:
        return Type::fromPrimitives(Primitive::Symbol);
    case TypeofType::Function:
        return Type::function();
    case TypeofType::Object:
        break;
    }

    // Type::object() includes the callable objects, whose typeof is "function", and no type stands for the others.
    if (!source.overlaps(Type::function(), program))
        return Type::object().withPrimitives(Primitive::Null);
    if (source.withOnlyObjects().isSubsetOf(Type::function(), program))
        return Type::fromPrimitives(Primitive::Null);
    return std::nullopt;
}

namespace {

enum class FoldingRule : uint8_t {
    KnownOperands = 1 << 0,
    SingleValueType = 1 << 1,
    SingleValueBehindCasts = 1 << 2,
    KnownString = 1 << 3,
    OwnRule = 1 << 4,
    MayHaveSeveralResults = 1 << 5,
    MayPassProof = 1 << 6,
};

using FoldingRules = OptionSet<FoldingRule>;

constexpr OpcodeSet opcodesWithOwnFoldingRule {
    Opcode::TypeTest, Opcode::IsClosureOf, Opcode::StrictEq, Opcode::Eq, Opcode::ToBoolean, Opcode::ToString, Opcode::ToPropertyKey,
    Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine, Opcode::SuperGet, Opcode::SuperSet,
    Opcode::CopyDataProperties, Opcode::CallBuiltin, Opcode::HasInPrototypeChain,
};

constexpr FoldingRules foldingRulesOf(Opcode opcode)
{
    FoldingRules ways;
    if (!foldableOpcodes.contains(opcode))
        return ways;
    if (opcodesWithKnownValueRules.contains(opcode))
        ways.add(FoldingRule::KnownOperands);
    if (isCastOrConversion(opcode))
        ways.add(FoldingRule::SingleValueBehindCasts);
    else if (resultRuleOf(opcode) != ResultRule::None && resultRuleOf(opcode) != ResultRule::Allocation && opcode != Opcode::NewClass)
        ways.add(FoldingRule::SingleValueType);
    if (opcodesCreatingKnownStrings.contains(opcode))
        ways.add(FoldingRule::KnownString);
    if (opcodesWithOwnFoldingRule.contains(opcode))
        ways.add(FoldingRule::OwnRule);
    if (ways.isEmpty())
        return ways;
    if (opcodesThatMayHaveSeveralResults.contains(opcode))
        ways.add(FoldingRule::MayHaveSeveralResults);
    if (provesOf(opcode) != Proof::None)
        ways.add(FoldingRule::MayPassProof);
    return ways;
}

constexpr std::array<FoldingRules, numberOfOpcodes> foldingRulesOfOpcodes = [] {
    std::array<FoldingRules, numberOfOpcodes> result { };
    for (Opcode opcode : allMembersOf<Opcode>)
        result[static_cast<unsigned>(opcode)] = foldingRulesOf(opcode);
    return result;
}();

class ConstantFolder {
public:
    explicit ConstantFolder(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
    {
        if (context.analyses.computed().contains(FunctionAnalysis::UseLists))
            m_useLists = &context.analyses.useLists();
    }

    PassOutcome run()
    {
        Vector<ValueID, 16> foldedValues;
        bool didChangeCasts = false;
        for (BlockID blockID : m_context.analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(blockID);
            ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
            for (ValueID id : block.values) {
                Opcode opcode = m_function.at(id).opcode;
                if (foldingRulesOfOpcodes[static_cast<unsigned>(opcode)].isEmpty() || id == tailCall)
                    continue;
                bool wasCast = isCastOrConversion(opcode);
                if (!foldValue(id))
                    continue;
                foldedValues.append(id);
                didChangeCasts |= wasCast || isCastOrConversion(m_function.at(id).opcode);
            }
        }
        m_context.foldingAskedForUses = m_askedForUses;
        if (foldedValues.isEmpty())
            return PassOutcome::Unchanged;

        // Which values are one value is read from the casts, from what the edges pass and from the tests whose branches pass proofs. The last two stay as they are.
        AnalysisSet unchangedAnalyses;
        unchangedAnalyses.set(FunctionAnalysis::Identities, !didChangeCasts);
        m_context.analyses.invalidate(valueAnalyses - unchangedAnalyses);
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, foldedValues.span());
        m_context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Value& definitionOf(ValueID id) const { return m_function.at(definitionThroughUpcasts(m_function, id)); }
    KnownValue knownValueOf(ValueID id) const { return Typed::knownValueOf(m_program, m_function, id); }

    bool foldValue(ValueID id)
    {
        bool didFold = false;
        while (foldOnce(id)) {
            didFold = true;
            // The walk goes on to ask what this value and its users do.
            m_context.analyses.invalidate(FunctionAnalysis::Effects);
        }
        return didFold;
    }

    bool foldOnce(ValueID id)
    {
        const Value& value = m_function.at(id);
        FoldingRules rules = foldingRulesOfOpcodes[static_cast<unsigned>(value.opcode)];
        if (rules.isEmpty())
            return false;
        if (rules.contains(FoldingRule::MayHaveSeveralResults) && numberOfResultsOf(m_program, m_function, value))
            return false;
        if (rules.contains(FoldingRule::MayPassProof) && isConditionOfBranchWithProofs(id))
            return false;
        if (rules.contains(FoldingRule::KnownOperands) && foldKnownOperands(id, value))
            return true;
        if (rules.contains(FoldingRule::SingleValueType) ? foldSingleValueOfOwnType(id, value) : rules.contains(FoldingRule::SingleValueBehindCasts) && foldSingleValueType(id, value))
            return true;
        if (rules.contains(FoldingRule::KnownString) && foldKnownString(id, value))
            return true;
        if (!rules.contains(FoldingRule::OwnRule))
            return false;

        switch (value.opcode) {
        case Opcode::TypeTest:
        case Opcode::IsClosureOf:
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value));
        case Opcode::StrictEq:
        case Opcode::Eq:
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value)) || foldComparisonOfOneClosure(id, value) || foldTypeOfComparison(id, value);
        case Opcode::ToBoolean:
            return foldToBoolean(id, value);
        case Opcode::ToString:
            return foldToString(id, value);
        case Opcode::ToPropertyKey:
            return foldToPropertyKey(id, value);
        case Opcode::ElemGet:
        case Opcode::ElemSet:
        case Opcode::ElemHas:
        case Opcode::ElemDelete:
        case Opcode::ElemDefine:
            return convertToNamedAccess(id, value);
        case Opcode::SuperGet:
        case Opcode::SuperSet:
            return convertToNamedSuperAccess(id, value);
        case Opcode::CopyDataProperties:
            return foldCopyDataProperties(id, value);
        case Opcode::CallBuiltin:
            return foldStringCall(id, value);
        case Opcode::HasInPrototypeChain:
            return foldWalkFromNewObject(id, value);
        default:
            return false;
        }
    }

    // `this instanceof F` in the body of F, behind the allocation: an object that is handed no prototype is born with the prototype object of its class, and no object of the layout ever gets another.
    bool foldWalkFromNewObject(ValueID id, const Value& walk)
    {
        const Value& object = m_function.at(sameObjectThroughCasts(m_function, walk.valueOperand()));
        const Value& prototype = m_function.at(sameObjectThroughCasts(m_function, walk.prototypeOperand()));
        if (object.opcode != Opcode::NewObject || object.prototypeOperand() || prototype.opcode != Opcode::GetClassPrototype)
            return false;
        if (!allocatesOwnLayout(m_program, object) || declaredLayoutOf(m_program, object.layout()) != prototype.layout())
            return false;
        return receiverStateOf(m_program, object.layout()).cannot(ObjectMutation::ChangePrototype) && becomeBoolean(id, KnownBoolean::True);
    }

    // `stream.emit === emit`: the program makes one closure of the function, so whatever is a closure of it is that object.
    bool foldComparisonOfOneClosure(ValueID id, const Value& comparison)
    {
        for (unsigned i = 0; i < 2; ++i) {
            const Type& closure = typeOf(comparison.operand(i));
            FunctionID function = closure.singleFunction();
            if (!function || closure.mayBeUndefinedOrNull() || !m_context.inputs.flow.hasOneClosure(function))
                continue;
            ValueID other = comparison.operand(1 - i);
            const Type& otherType = typeOf(other);
            if (!otherType.isJavaScriptValue() || otherType.withOnlyObjects().singleFunction() != function)
                continue;
            if (otherType.singleFunction() == function && !otherType.mayBeUndefinedOrNull())
                return becomeBoolean(id, KnownBoolean::True);
            Value& test = m_function.edit(id);
            test.become(Opcode::TypeTest);
            test.setValueOperand(other);
            test.setTestedType(m_context.types.intern(Type::forFunction(function)));
            return true;
        }
        return false;
    }

    static bool isPrimitive(KnownValue value) { return value.isKnown() && !value.isObject(); }

    bool foldKnownOperands(ValueID id, const Value& value)
    {
        KnownValue result = evaluateWithKnownOperands(m_program, m_function, value, [&](ValueID operand) {
            return knownValueOf(operand);
        });
        return isPrimitive(result) && becomeConstant(id, result);
    }

    bool isConditionOfBranchWithProofs(ValueID id)
    {
        if (!m_conditionsOfBranchesWithProofs)
            m_conditionsOfBranchesWithProofs = conditionsOfBranchesWithProofs(m_function, m_context.analyses.cfg());
        return m_conditionsOfBranchesWithProofs->get(id.index());
    }

    bool foldSingleValueOfOwnType(ValueID id, const Value& value)
    {
        const Type& type = m_program.types.at(value.type);
        static_assert(std::has_single_bit(Primitive::Undefined) && std::has_single_bit(Primitive::Null) && std::has_single_bit(Primitive::False) && std::has_single_bit(Primitive::True) && std::has_single_bit(Primitive::Integer) && std::has_single_bit(Primitive::LiteralString));
        if (!std::has_single_bit(type.primitives))
            return false;
        KnownValue result = knownValueOfType(m_program, type);
        if (!isPrimitive(result) || !m_context.analyses.isRemovableWhenUnused(id))
            return false;
        return becomeConstant(id, result);
    }

    bool foldSingleValueType(ValueID id, const Value& value)
    {
        if (resultRuleOf(value.opcode) == ResultRule::None)
            return false;
        KnownValue result = knownValueOf(id);
        if (!isPrimitive(result) || !m_context.analyses.isRemovableWhenUnused(id))
            return false;
        return becomeConstant(id, result);
    }

    bool foldKnownString(ValueID id, const Value& value)
    {
        if (!opcodesCreatingKnownStrings.contains(value.opcode))
            return false;

        uint64_t bytes = 0;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            KnownValue operand = knownValueOf(value.operand(index));
            if (!isPrimitive(operand))
                return false;
            bytes += operand.kind() == KnownValue::Kind::String ? m_program.string(operand.asString()).size() : maxPrimitiveTextBytes;
        }
        if (exceedsLimitOfFoldedString(id, bytes))
            return false;

        KnownValue result = evaluateStringOperation(m_program, m_context.strings, m_function, value, [&](ValueID operand) {
            return knownValueOf(operand);
        });
        return result.kind() == KnownValue::Kind::String && becomeConstant(id, result);
    }

    BuiltinID calledStringFunctionOf(const Value& value) const
    {
        if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
            return { };
        const BuiltinTable& table = m_program.builtins();
        const BuiltinOverload& overload = table.at(value.overload());
        BuiltinID function = table.canonicalID(overload.builtin);
        // What such a function answers depends on its operands and on nothing else: no locale, no clock.
        if (overload.mode != BuiltinCallMode::Call)
            return { };
        if (!table.at(function).flags.contains(BuiltinFlag::IsDeterministic) && !formatsNumberInNamedLocale(function, value))
            return { };
        return function;
    }

    // In a locale that the call names, and without options, the text depends on the number and on the data of ICU.
    bool formatsNumberInNamedLocale(BuiltinID function, const Value& value) const
    {
        if (function != Builtins::NumberPrototypeToLocaleString || targetOfThisBuild().platform == BuiltinPlatform::Darwin)
            return false;
        std::span<const ValueID> arguments = value.argumentsOperands();
        if (arguments.empty() || arguments.size() > 2 || knownValueOf(arguments[0]).kind() != KnownValue::Kind::String)
            return false;
        return arguments.size() == 1 || knownValueOf(arguments[1]).kind() == KnownValue::Kind::Undefined;
    }

    bool isPatternOfCreatedRegExp(const Value& user, ValueID text) const
    {
        if (user.opcode != Opcode::CallBuiltin || user.builtinCallForm() != BuiltinCallForm::Whole)
            return false;
        const BuiltinTable& table = m_program.builtins();
        const BuiltinOverload& overload = table.at(user.overload());
        if (overload.mode != BuiltinCallMode::Call && overload.mode != BuiltinCallMode::Construct)
            return false;
        if (table.canonicalID(overload.builtin) != Builtins::RegExp)
            return false;
        std::span<const ValueID> arguments = user.argumentsOperands();
        return !arguments.empty() && arguments[0] == text && std::ranges::count(arguments, text) == 1 && user.receiverOperand() != text;
    }

    // FoldConstantRegExps spells such a text once, as the pattern of the literal that it makes of the call.
    bool feedsOnlyRegExpPatterns(ValueID text, unsigned operationsLeft = maxOperationsBetweenTextAndPattern) const
    {
        m_askedForUses = true;
        std::span<const Use> uses = useLists().usesOf(text);
        if (uses.empty())
            return false;
        return std::ranges::all_of(uses, [&](const Use& use) {
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = m_function.at(use.user);
            if (isPatternOfCreatedRegExp(user, text))
                return true;
            bool makesTextOfOperands = user.opcode == Opcode::Upcast || opcodesCreatingKnownStrings.contains(user.opcode) || calledStringFunctionOf(user);
            return makesTextOfOperands && operationsLeft && feedsOnlyRegExpPatterns(use.user, operationsLeft - 1);
        });
    }

    bool exceedsLimitOfFoldedString(ValueID id, uint64_t bytes)
    {
        if (bytes > m_context.limit(LimitID::MaxFoldedStringBytes) && feedsOnlyRegExpPatterns(id))
            return m_context.exceeds(LimitID::RegExpCandidateTextLength, bytes, id);
        return m_context.exceeds(LimitID::MaxFoldedStringBytes, bytes, id);
    }

    // A literal that nothing but the call uses is as created when the call begins, and nobody sees what the call leaves in it.
    std::optional<RegExpLiteralInType> findLiteralUsedOnlyBy(ValueID user, ValueID regExp) const
    {
        m_askedForUses = true;
        for (;;) {
            std::optional<Use> use = useLists().singleUse(regExp);
            if (!use || use->kind != Use::Kind::Operand || use->user != user)
                return std::nullopt;
            const Value& definition = m_function.at(regExp);
            if (definition.opcode == Opcode::NewRegExp)
                return RegExpLiteralInType { definition.pattern(), definition.regExpFlags().toRaw() };
            if (definition.opcode != Opcode::Upcast && definition.opcode != Opcode::AssumeType)
                return std::nullopt;
            user = regExp;
            regExp = definition.valueOperand();
        }
    }

    TypeID typeOfLiteralAsCreated(RegExpLiteralInType literal)
    {
        Value shared(Opcode::SharedRegExpLiteral);
        shared.setPattern(literal.pattern);
        shared.setRegExpFlags(literal.flags());
        return resultTypeOf(m_program, m_function, shared, m_context.types);
    }

    struct RegExpOperand {
        RegExpLiteralInType literal;
        TypeID typeAtCall;
    };

    std::optional<RegExpOperand> regExpOperandOf(ValueID call, ValueID operand)
    {
        if (std::optional<RegExpLiteralInType> literal = findLiteralUsedOnlyBy(call, operand))
            return RegExpOperand { *literal, typeOfLiteralAsCreated(*literal) };
        // Only built-ins that leave the object as created get a shared literal, and the call is one of them.
        const Value& definition = m_function.at(sameObjectThroughCasts(m_function, operand));
        if (definition.opcode == Opcode::SharedRegExpLiteral) {
            RegExpLiteralInType literal { definition.pattern(), definition.regExpFlags().toRaw() };
            return RegExpOperand { literal, typeOfLiteralAsCreated(literal) };
        }
        // A match without the flags g and y neither depends on lastIndex nor stores it.
        std::optional<RegExpLiteralInType> literal = singleRegExpLiteralAsCreatedOf(typeOf(operand));
        if (!literal || literal->flags().containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }))
            return std::nullopt;
        return RegExpOperand { *literal, m_function.at(operand).type };
    }

    bool foldStringCall(ValueID id, const Value& value)
    {
        BuiltinID function = calledStringFunctionOf(value);
        if (!function)
            return false;
        ValueID receiverOperand = value.receiverOperand();
        KnownValue receiver = receiverOperand ? knownValueOf(receiverOperand) : KnownValue();
        bool takesNumber = function == Builtins::NumberPrototypeToLocaleString;
        if (takesNumber ? !receiver.isNumber() : receiver.kind() != KnownValue::Kind::String)
            return false;

        uint64_t bytes = takesNumber ? maxPrimitiveTextBytes : m_program.string(receiver.asString()).size();
        double largestNumber = 1;
        Vector<ScratchRealm::Operand, 2> arguments;
        Vector<TypeID, 2> argumentTypes;
        for (ValueID argument : value.argumentsOperands()) {
            KnownValue primitive = knownValueOf(argument);
            if (isPrimitive(primitive)) {
                if (primitive.kind() == KnownValue::Kind::String)
                    bytes += m_program.string(primitive.asString()).size();
                if (primitive.isNumber() && primitive.asNumber() > largestNumber)
                    largestNumber = primitive.asNumber();
                arguments.append(ScratchRealm::Operand { primitive, std::nullopt });
                argumentTypes.append(m_function.at(argument).type);
                continue;
            }
            std::optional<RegExpOperand> regExp = regExpOperandOf(id, argument);
            if (!regExp)
                return false;
            bytes += m_program.string(regExp->literal.pattern).size();
            arguments.append(ScratchRealm::Operand { { }, regExp->literal });
            argumentTypes.append(regExp->typeAtCall);
        }

        EffectDescription description;
        describeOverload(m_program, value.overload(), &typeOf(receiverOperand), argumentTypes.span(), description);
        if (description.callsUnknownCode || !description.functions.isEmpty())
            return false;
        static constexpr AbstractHeapSet heapsNobodyObserves = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };
        Effects effects = description.own.onAnyEdge();
        if (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsNobodyObserves | AbstractHeapSet { AbstractHeap::RegExpState }))
            return false;
        // The RegExp of the call is left as it was or is seen by nobody, so what the last match leaves in the constructor is the one trace of a match.
        if (!effects.writes.containsOnly(heapsNobodyObserves) && m_context.inputs.flow.mayReadLegacyRegExpStatics())
            return false;
        if (exceedsLimitOfFoldedString(id, bytes))
            return false;
        // An entry with an error of its own may fail for the length of its answer, which a number among its operands sets: a count, a length.
        bool pads = function == Builtins::StringPrototypePadStart || function == Builtins::StringPrototypePadEnd;
        if (pads || !m_program.builtins().at(value.overload()).onlyPropagatesErrors()) {
            double bytesOfAnswer = static_cast<double>(bytes) * largestNumber;
            if (bytesOfAnswer >= static_cast<double>(std::numeric_limits<uint32_t>::max()) || exceedsLimitOfFoldedString(id, static_cast<uint64_t>(bytesOfAnswer)))
                return false;
        }

        std::optional<ScratchRealm::Answer> answer = m_context.build.scratchRealms().ofCurrentThread().call(m_program, function, receiver, arguments.span());
        if (!answer)
            return false;
        if (answer->text.isNull())
            return becomeConstant(id, answer->primitive);
        if (exceedsLimitOfFoldedString(id, answer->text.sizeInBytes()))
            return false;
        return becomeConstant(id, KnownValue::string(m_context.strings.intern(StringView(answer->text))));
    }

    bool foldToBoolean(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        if (!typeOf(operand).isBoolean())
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value));

        becomeUpcast(m_function, id, operand);
        return true;
    }

    bool foldToString(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        if (!typeOf(operand).isAnyString())
            return false;

        becomeUpcast(m_function, id, operand);
        return true;
    }

    bool foldToPropertyKey(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        const Type& type = typeOf(operand);
        if (type.isNever() || !type.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol))
            return false;

        becomeUpcast(m_function, id, operand);
        return true;
    }

    bool foldTypeOfComparison(ValueID id, const Value& value)
    {
        for (unsigned i = 0; i < 2; ++i) {
            const Value& typeOfOperation = definitionOf(value.operand(i));
            const Value& string = definitionOf(value.operand(1 - i));
            if (typeOfOperation.opcode != Opcode::TypeOf || string.opcode != Opcode::String)
                continue;
            auto expectedTypeof = typeofTypeNamed(m_program.string(string.string()));
            if (!expectedTypeof)
                return becomeConstant(id, KnownValue::boolean(false));

            ValueID operand = typeOfOperation.valueOperand();
            std::optional<Type> tested = typeTestedByTypeOf(m_program, *expectedTypeof, typeOf(operand));
            if (!tested)
                return false;
            Value& test = m_function.edit(id);
            test.become(Opcode::TypeTest);
            test.setValueOperand(operand);
            test.setTestedType(m_context.types.intern(*tested));
            return true;
        }
        return false;
    }

    PropertyKeyID findConstantPropertyKey(ValueID key)
    {
        return internKey(m_context.types, constantKeyOf(m_program, typeOf(key)));
    }

    bool convertToNamedSuperAccess(ValueID id, const Value& value)
    {
        ValueID computedKey = value.computedKeyOperand();
        PropertyKeyID key = computedKey ? findConstantPropertyKey(computedKey) : PropertyKeyID();
        if (!key)
            return false;
        Value& access = m_function.edit(id, ValueEdit::Definition);
        access.setComputedKeyOperand({ });
        access.setKey(key);
        return true;
    }

    bool convertToNamedAccess(ValueID id, const Value& value)
    {
        PropertyKeyID key = findConstantPropertyKey(value.propertyOperand());
        if (!key)
            return false;

        TypeID expected = value.expectedOrType();
        Value& access = m_function.edit(id);
        becomeNamedAccess(access, key);
        retypeKeepingExpectedType(access, expected);
        return true;
    }

    void retypeKeepingExpectedType(Value& value, TypeID expected)
    {
        TypeArena& types = m_context.types;
        value.type = resultTypeOf(m_program, m_function, value, types);
        TypeID narrowed = types.intersectionOf(value.type, expected);
        // An intersection may keep a part of one side that the subset test does not find in the other.
        bool saysMoreThanType = narrowed != TypeTable::never && narrowed != value.type && types.at(narrowed).isSubsetOf(types.at(value.type), m_program);
        value.setExpected(saysMoreThanType ? narrowed : TypeID());
    }

    bool foldCopyDataProperties(ValueID id, const Value& value)
    {
        static constexpr uint16_t primitivesWithoutProperties = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;
        if (!typeOf(value.sourceOperand()).hasOnlyPrimitives(primitivesWithoutProperties))
            return false;
        becomeUndefined(m_function, id);
        return true;
    }

    bool becomeBoolean(ValueID id, KnownBoolean result)
    {
        switch (result) {
        case KnownBoolean::Unknown:
            return false;
        case KnownBoolean::False:
            return becomeConstant(id, KnownValue::boolean(false));
        case KnownBoolean::True:
            return becomeConstant(id, KnownValue::boolean(true));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool becomeConstant(ValueID id, KnownValue constant)
    {
        if (evaluateTypeTest(m_program, constant, typeOf(id)) != KnownBoolean::True)
            return false;

        Value& value = m_function.edit(id);
        switch (constant.kind()) {
        case KnownValue::Kind::Undefined:
            value.become(Opcode::Undefined);
            break;
        case KnownValue::Kind::Null:
            value.become(Opcode::Null);
            break;
        case KnownValue::Kind::Boolean:
            value.become(Opcode::Boolean);
            value.setBoolean(constant.asBoolean());
            break;
        case KnownValue::Kind::Integer:
            value.become(Opcode::Integer);
            value.setInteger(constant.asInteger());
            break;
        case KnownValue::Kind::Double:
            value.become(Opcode::Double);
            value.setNumber(constant.asNumber());
            break;
        case KnownValue::Kind::String:
            value.become(Opcode::String);
            value.setString(constant.asString());
            break;
        case KnownValue::Kind::Unknown:
        case KnownValue::Kind::Object:
        case KnownValue::Kind::Function:
            RELEASE_ASSERT_NOT_REACHED();
        }
        value.type = resultTypeOf(m_program, m_function, value, m_context.types);
        value.setExpected({ });
        return true;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    // Few bodies ask who uses a value: the users of a text that is too long to fold, which come later in the walk than the text, and the users of a RegExp literal.
    const UseLists& useLists() const
    {
        if (!m_useLists)
            m_useLists = &m_context.analyses.useLists();
        return *m_useLists;
    }

    std::optional<BitVector> m_conditionsOfBranchesWithProofs;
    mutable const UseLists* m_useLists { nullptr };
    mutable bool m_askedForUses { false };
};

} // anonymous namespace

PassOutcome foldConstants(FunctionPassContext& context)
{
    return ConstantFolder(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
