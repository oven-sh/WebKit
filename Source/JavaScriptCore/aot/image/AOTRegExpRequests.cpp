/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRequests.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTLargeVectors.h"
#include "AOTPhaseTimes.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpSummary.h"
#include "AOTScratchRealm.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StackCheck.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpRequests);

static_assert(Typed::Value::opcodesWithPattern == Typed::Value::opcodesWithRegExpFlags);
static_assert(Typed::Value::opcodesWithPattern == Typed::Value::opcodesWithRegExpRecord);

namespace {

struct FoundRegExp {
    RegExpRequestOrigin origin;
    StringID pattern;
    OptionSet<Yarr::Flags> flags;
    RegExpSite site;
};

using FlagSets = Vector<OptionSet<Yarr::Flags>, 8>;

struct CallWithUnknownText {
    RegExpSite site;
    Typed::BlockID block;
    ValueID pattern;
    ValueID flags;
    FlagSets flagSets;
    bool flagsAreComplete;
};

struct CollectionInputs {
    const Typed::Program& program;
    const Typed::FlowResults& flow;
    BuildContext& context;
    Lock lock { };
    Vector<CallWithUnknownText> callsWithUnknownText { };
};

class TextsFromConstants {
public:
    struct Constant {
        Typed::KnownValue primitive;
        std::optional<Typed::RegExpLiteralInType> regExp { };
        FunctionID function { };
        ValueID literal { };
        const Typed::DataNode* data { nullptr };
        FunctionID caller { };
        const Typed::Value* call { nullptr };
        FunctionID declaration { };
        // A value that the walk cannot tell, of one operation under one call. What is stored under it in one place is read under it in another.
        ValueID unnamed { };
        // A list of texts that a built-in has answered, by its place in m_listsOfTexts, counted from 1. Two of them with the same texts are one.
        uint32_t listOfTexts { 0 };

        bool isObjectOrArray() const { return literal || data || declaration; }
        bool hasElements() const { return isObjectOrArray() || listOfTexts; }
        friend bool operator==(const Constant&, const Constant&) = default;
    };
    using Constants = Vector<Constant, 1>;

    struct Pattern {
        StringID text;
        OptionSet<Yarr::Flags> flags;
        friend bool operator==(const Pattern&, const Pattern&) = default;
    };

    TextsFromConstants(const Typed::Program& program, Typed::StringArena& strings, BuildContext& context)
        : m_program(program)
        , m_strings(strings)
        , m_context(context)
    {
        uint32_t numberOfFunctions = program.functions.size();
        m_usesOfClosures.grow(numberOfFunctions);
        Vector<CollectedOperation> operations = collectInParallel<CollectedOperation>(context.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<CollectedOperation>& found) {
            forEachValueListedOrNot(program, FunctionID(index), { Typed::Opcode::PutModuleVariable, Typed::Opcode::CallDirect, Typed::Opcode::NewClosure }, [&](ValueID id, const Typed::Value& value) {
                if (value.opcode == Typed::Opcode::PutModuleVariable)
                    found.append(CollectedOperation { value.opcode, value.variable().index(), FunctionID(index), value.valueOperand() });
                else if (value.function().index() < numberOfFunctions)
                    found.append(CollectedOperation { value.opcode, value.function().index(), FunctionID(index), id });
            });
        });
        const ParallelLoops& loops = context.parallelLoops();
        runTasksInParallel(loops, {
            [&] { m_directCalls = groupByCallee(loops, operations.span(), Typed::Opcode::CallDirect); },
            [&] { m_closures = groupByCallee(loops, operations.span(), Typed::Opcode::NewClosure); },
            [&] {
                m_stores = collectInParallel<Store>(loops, operations.size(), [&](unsigned index, Vector<Store>& stores) {
                    const CollectedOperation& operation = operations[index];
                    if (operation.opcode == Typed::Opcode::PutModuleVariable)
                        stores.append(Store { Typed::VariableID(operation.targetIndex), operation.function, operation.value });
                });
                parallelSort(loops, m_stores.mutableSpan(), [](const Store& a, const Store& b) {
                    return std::tuple { a.variable, a.function, a.stored } < std::tuple { b.variable, b.function, b.stored };
                });
            },
        });
        for (const BuiltinOverload& overload : program.builtins().overloadsOf(Builtins::ArrayPrototypeMap)) {
            const Typed::RuntimeFunctionEntry* entry = overload.code.implementation.function ? findRuntimeFunction(program, overload.code.implementation.function) : nullptr;
            if (entry)
                m_functionsOfMap.appendIfNotContains(entry->function);
        }
    }

    struct Patterns {
        Vector<Pattern, 1> patterns;
        bool sawEveryText { true };
        bool sawAllFlags { true };
    };

    Patterns patternsOf(const CallWithUnknownText& call)
    {
        m_subject = Subject { .function = call.site.function, .block = call.block, .value = call.site.value };
        m_valuesFollowed = 0;
        m_worlds = 0;
        m_active.shrink(0);
        m_bindings.shrink(0);
        m_loopsFollowed.shrink(0);
        m_valuesInTurn.shrink(0);
        Patterns result;
        explore(call, result);
        return result;
    }

private:
    AOT_CLASS_THRESHOLD(unsigned, maxValuesFollowed, 1 << 14);
    AOT_CLASS_THRESHOLD(unsigned, maxValuesFollowedForTest, 64);
    unsigned m_budget { maxValuesFollowed };
    AOT_CLASS_THRESHOLD(unsigned, maxTestsBeingAnswered, 1);
    unsigned m_testsBeingAnswered { 0 };

    struct Store {
        Typed::VariableID variable;
        FunctionID function;
        ValueID stored;
    };

    struct DirectCall {
        FunctionID callee;
        FunctionID caller;
        ValueID call;
    };

    struct CollectedOperation {
        Typed::Opcode opcode;
        uint32_t targetIndex;
        FunctionID function;
        ValueID value;
    };

    // The operations of one callee keep the input order, which is sorted by (function, value).
    static Vector<DirectCall> groupByCallee(const ParallelLoops& loops, std::span<const CollectedOperation> operations, Typed::Opcode opcode)
    {
        RELEASE_ASSERT(operations.size() <= UINT32_MAX);
        Vector<uint64_t> order = collectInParallel<uint64_t>(loops, operations.size(), [&](unsigned index, Vector<uint64_t>& found) {
            if (operations[index].opcode == opcode)
                found.append(static_cast<uint64_t>(operations[index].targetIndex) << 32 | index);
        });
        parallelSort(loops, order.mutableSpan(), std::less<uint64_t> { });
        Vector<DirectCall> result = createVectorInParallel<DirectCall>(loops, order.size());
        constexpr size_t callsInShare = 4096;
        forEachIndexInParallel(loops, static_cast<unsigned>((order.size() + callsInShare - 1) / callsInShare), [&](unsigned share) {
            for (size_t position = share * callsInShare; position < std::min(order.size(), (share + 1) * callsInShare); ++position) {
                const CollectedOperation& operation = operations[static_cast<uint32_t>(order[position])];
                result[position] = DirectCall { FunctionID(operation.targetIndex), operation.function, operation.value };
            }
        });
        return result;
    }

    struct Activation {
        FunctionID function;
        const Activation* caller { nullptr };
        const Typed::Value* call { nullptr };
        mutable std::optional<uint32_t> cachedCallChain { };
    };

    using ValueKey = std::pair<FunctionID, ValueID>;
    using MemoKey = std::tuple<uint32_t, const Typed::Value*, uint32_t, uint32_t>;

    struct Split {
        ValueKey value;
        Constants alternatives;
    };

    // Every way round the loop passes the header, which takes what one turn hands to the next as parameters and ends by testing one of them.
    struct Loop {
        Typed::BlockID header;
        uint32_t targetThatStays { 0 };
        // The header, and what runs between two passes of it.
        BitVector turn;
        // What runs before the header is passed for the first time. A block may be on both ways.
        BitVector wayIn;
        bool isLeftAtHeaderAlone { true };
    };

    enum class PartOfLoop : uint8_t { WayIn, Turn };

    struct LoopFollowed {
        const Typed::Value* call;
        FunctionID function;
        const Loop* loop;
        PartOfLoop part;
    };

    // What a parameter of the header holds in the turn that is being followed. Nothing where the walk cannot tell.
    struct ValueInTurn {
        const Typed::Value* call;
        ValueKey value;
        Constants values;
        bool sawEverything;
    };

    void explore(const CallWithUnknownText& call, Patterns& result)
    {
        m_split.reset();
        m_sawEverything = true;
        Vector<StringID, 1> texts;
        for (const Constant& constant : evaluate(Activation { call.site.function }, call.pattern)) {
            if (constant.regExp)
                texts.appendIfNotContains(constant.regExp->pattern);
            else if (constant.primitive.kind() == Typed::KnownValue::Kind::String)
                texts.appendIfNotContains(constant.primitive.asString());
            else if (constant.unnamed)
                m_sawEverything = false;
        }
        bool sawEveryText = m_sawEverything && !texts.isEmpty();

        FlagSets flagSets = call.flagSets;
        bool sawAllFlags = call.flagsAreComplete;
        if (!sawAllFlags && (!texts.isEmpty() || m_split)) {
            m_sawEverything = true;
            FlagSets evaluated = flagSetsOf(call);
            sawAllFlags = m_sawEverything && !evaluated.isEmpty();
            // What flow names beside them is what it says where it knows nothing.
            if (sawAllFlags)
                flagSets = WTF::move(evaluated);
            else {
                for (OptionSet<Yarr::Flags> flagSet : evaluated)
                    flagSets.appendIfNotContains(flagSet);
            }
        }

        if (m_split) {
            Split split = WTF::move(*m_split);
            for (const Constant& alternative : split.alternatives) {
                if (++m_worlds > maxAlternatives()) {
                    result.sawEveryText = false;
                    return;
                }
                m_bindings.append({ split.value, alternative });
                explore(call, result);
                m_bindings.removeLast();
            }
            return;
        }

        result.sawEveryText &= sawEveryText;
        result.sawAllFlags &= sawAllFlags;
        for (StringID text : texts) {
            for (OptionSet<Yarr::Flags> flagSet : flagSets)
                result.patterns.appendIfNotContains(Pattern { text, flagSet });
        }
    }

    FlagSets flagSetsOf(const CallWithUnknownText& call)
    {
        FlagSets flagSets;
        Vector<char16_t> buffer;
        for (const Constant& constant : evaluate(Activation { call.site.function }, call.flags)) {
            if (constant.primitive.kind() == Typed::KnownValue::Kind::Undefined) {
                flagSets.appendIfNotContains(OptionSet<Yarr::Flags> { });
                continue;
            }
            if (constant.primitive.kind() != Typed::KnownValue::Kind::String)
                continue;
            if (std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(m_program.strings.viewOf(constant.primitive.asString(), buffer)))
                flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
        }
        return flagSets;
    }

    uint64_t maxAlternatives() const { return m_context.limit(LimitID::RegExpCandidatesPerSite); }

    bool add(Constants& constants, const Constant& constant) const
    {
        if (constants.contains(constant))
            return true;
        if (constants.size() >= maxAlternatives())
            return false;
        constants.append(constant);
        return true;
    }

    bool addAll(Constants& constants, const Constants& others)
    {
        m_sawEverything &= !others.isEmpty();
        for (const Constant& other : others) {
            if (!add(constants, other)) {
                constants.clear();
                return false;
            }
        }
        return true;
    }

    static Constant literalIn(const Activation& activation, ValueID id)
    {
        Constant literal { .function = activation.function, .literal = id };
        if (activation.call) {
            literal.caller = activation.caller->function;
            literal.call = activation.call;
        }
        return literal;
    }

    struct WhereMade {
        WTF_MAKE_NONCOPYABLE(WhereMade);
        WTF_MAKE_NONMOVABLE(WhereMade);
        explicit WhereMade(const Constant& literal)
            : caller { literal.caller }
            , activation { literal.function, literal.call ? &caller : nullptr, literal.call }
        {
        }

        Activation caller;
        Activation activation;
    };

    // Never 0, so that no key of m_remembered is the empty one.
    uint32_t callsLeadingTo(const Activation& activation)
    {
        if (!activation.call)
            return 1;
        if (!activation.cachedCallChain)
            activation.cachedCallChain = m_chainsOfCalls.try_emplace({ callsLeadingTo(*activation.caller), activation.call }, static_cast<uint32_t>(m_chainsOfCalls.size() + 2)).first->second;
        return *activation.cachedCallChain;
    }

    const Constants* recall(const MemoKey& key)
    {
        auto remembered = holdsForEveryChoice() ? m_remembered.find(key) : m_remembered.end();
        if (remembered == m_remembered.end())
            return nullptr;
        m_sawEverything &= remembered->value->sawEverything;
        if (remembered->value->split)
            m_split = remembered->value->split;
        return &remembered->value->values;
    }

    // What is read under the choice of one object, or in one turn of a loop, holds there alone.
    bool holdsForEveryChoice() const { return m_bindings.isEmpty() && m_valuesInTurn.isEmpty(); }

    bool mayFollowAnotherValue()
    {
        if (m_stackCheck.isSafeToRecurse() && ++m_valuesFollowed <= m_budget)
            return true;
        ++m_walksCutShort;
        return false;
    }

    bool isActive(const Activation& activation, FunctionID function) const
    {
        for (const Activation* active = &activation; active; active = active->caller) {
            if (active->function == function)
                return true;
        }
        return false;
    }

    Constants evaluate(const Activation& activation, ValueID id)
    {
        if (!id)
            return { Constant { Typed::KnownValue::undefined() } };
        if (!mayFollowAnotherValue())
            return { };
        for (auto& [value, constant] : m_bindings) {
            if (value == ValueKey { activation.function, id })
                return { constant };
        }
        for (const ValueInTurn& bound : m_valuesInTurn.span() | std::views::reverse) {
            if (bound.call == activation.call && bound.value == ValueKey { activation.function, id }) {
                m_sawEverything &= bound.sawEverything;
                return bound.values;
            }
        }
        const Typed::Function& function = m_program.at(activation.function);
        if (Typed::KnownValue known = knownValueOf(m_program, function, id); known.isKnown() && !known.isObject())
            return { Constant { known } };

        // A value that is made of itself goes round a loop. The same value of a function that is called twice, one call inside the other's argument, does not.
        std::tuple key { activation.call, activation.function, id };
        if (m_active.contains(key)) {
            ++m_walksCutShort;
            return { };
        }

        bool mayRemember = holdsForEveryChoice() && !isDefinedInLoopFollowed(activation, function, id);
        MemoKey memoKey { callsLeadingTo(activation), nullptr, activation.function.index(), id.index() };
        if (const Constants* remembered = mayRemember ? recall(memoKey) : nullptr)
            return *remembered;

        unsigned walksCutShortBefore = m_walksCutShort;
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        std::optional<Split> splitBefore = std::exchange(m_split, std::nullopt);
        m_active.append(key);
        Constants result = evaluateOperation(activation, function, id);
        m_active.removeLast();
        bool isOneOfSeveralObjects = result.size() > 1 && std::ranges::all_of(result, &Constant::isObjectOrArray);
        if (isOneOfSeveralObjects)
            m_split = Split { { activation.function, id }, result };
        if (mayRemember && m_walksCutShort == walksCutShortBefore && m_valuesFollowed <= maxValuesFollowed)
            m_remembered.add(memoKey, makeUniqueWithoutFastMallocCheck<Remembered>(Remembered { result, m_sawEverything, m_split }));
        m_sawEverything &= sawEverythingBefore;
        if (!m_split)
            m_split = WTF::move(splitBefore);
        return result;
    }

    Constants evaluateOperation(const Activation& activation, const Typed::Function& function, ValueID id)
    {
        const Typed::Value& value = function.at(id);
        switch (value.opcode) {
        case Typed::Opcode::NewRegExp:
        case Typed::Opcode::SharedRegExpLiteral:
            return { Constant { .regExp = Typed::RegExpLiteralInType { value.pattern(), value.regExpFlags().toRaw() } } };
        case Typed::Opcode::NewObject:
        case Typed::Opcode::NewDictionary:
        case Typed::Opcode::NewArray:
        case Typed::Opcode::NewArrayLike:
        case Typed::Opcode::NewContiguousArray:
        case Typed::Opcode::NewClosure:
            return { literalIn(activation, id) };
        case Typed::Opcode::Data:
            return { constantOf(m_program.at(value.data()).root) };
        case Typed::Opcode::GetSlot:
        case Typed::Opcode::GetSlotOrUndefined:
        case Typed::Opcode::PropGet:
            return fieldsOf(evaluate(activation, value.objectOperand()), value.key());
        case Typed::Opcode::GetArrayElement: {
            Constants arrays = evaluate(activation, value.arrayOperand());
            return elementsAt(activation, arrays, value.positionOperand());
        }
        case Typed::Opcode::ElemGet: {
            Constants arrays = evaluate(activation, value.objectOperand());
            return elementsAt(activation, arrays, value.propertyOperand());
        }
        case Typed::Opcode::GetStringLength:
            return lengthsOf(evaluate(activation, value.stringOperand()));
        case Typed::Opcode::GetCapture:
            return capturesOf(activation, value);
        case Typed::Opcode::GetEnvironmentField:
            return fieldSeenThrough(activation, value.environmentOperand(), value.layout(), value.field());
        case Typed::Opcode::StringBuilderFinish:
        case Typed::Opcode::StringBuilderToString:
            return textsBuiltUpTo(activation, function, value.builderOperand(), id);
        case Typed::Opcode::GetModuleVariable:
            return valuesOf(value.variable());
        case Typed::Opcode::Upcast:
        case Typed::Opcode::Expect:
        case Typed::Opcode::AssumeType:
        case Typed::Opcode::CheckInitialized:
        case Typed::Opcode::FlattenString:
        case Typed::Opcode::TruncateNewArray:
        case Typed::Opcode::FinishObject:
            return evaluate(activation, value.operand(0));
        case Typed::Opcode::RequireObjectCoercible:
        case Typed::Opcode::RequireObject:
        case Typed::Opcode::ToObject: {
            Constants values = evaluate(activation, value.operand(0));
            values.removeAllMatching([](const Constant& constant) {
                return constant.primitive.isNullish();
            });
            return values;
        }
        case Typed::Opcode::Parameter:
            return valuesArrivingAt(activation, function, id);
        case Typed::Opcode::CallDirect:
            if (m_functionsOfMap.contains(sourceOf(value.function())) && value.argumentsOperands().size() >= 2)
                return { literalIn(activation, id) };
            return resultsOf(activation, value);
        case Typed::Opcode::CallBuiltin:
        case Typed::Opcode::CallInline:
            return answersOfBuiltin(activation, value, builtinCallOf(m_program, value));
        case Typed::Opcode::Call:
        case Typed::Opcode::CallMethod: {
            Constants answers;
            bool isBuiltin = false;
            forEachBuiltinCall(value, [&](const Typed::BuiltinCall& call) {
                if (!std::exchange(isBuiltin, true))
                    answers = answersOfBuiltin(activation, value, call);
            });
            return isBuiltin ? answers : answersOfMethodOfText(activation, function, value);
        }
        case Typed::Opcode::Substring:
            return substringsOf(activation, function, value);
        case Typed::Opcode::Increment:
        case Typed::Opcode::Decrement: {
            Constants numbers;
            for (const Constant& constant : evaluate(activation, value.valueOperand())) {
                Typed::KnownValue number = constant.primitive.isNumber() ? Typed::KnownValue::computedNumber(constant.primitive.asNumber() + (value.opcode == Typed::Opcode::Increment ? 1 : -1)) : Typed::KnownValue();
                if (!number.isNumber())
                    m_sawEverything = false;
                else if (!add(numbers, Constant { number }))
                    return { };
            }
            return numbers;
        }
        case Typed::Opcode::Sub:
            return sumsOrDifferencesOf(activation, function, value, { evaluate(activation, value.leftOperand()), evaluate(activation, value.rightOperand()) });
        case Typed::Opcode::ResultAt:
            return resultsAt(activation, function, value.resultsOperand(), value.index());
        default:
            if (Typed::opcodesCreatingKnownStrings.contains(value.opcode))
                return concatenate(activation, value);
            return { };
        }
    }

    struct ValuesOfVariable {
        Constants values;
        bool sawEverything { false };
    };

    Constants valuesOf(Typed::VariableID variable)
    {
        if (auto known = m_variables.find(variable.index()); known != m_variables.end()) {
            m_sawEverything &= known->value.sawEverything;
            return known->value.values;
        }
        // While it is walked, a variable that is made of itself has no value.
        m_variables.add(variable.index(), ValuesOfVariable { });
        unsigned walksCutShortBefore = m_walksCutShort;
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        Constants values;
        if (m_program.at(variable).initialization == Typed::VariableInitialization::Function)
            values.append(Constant { .declaration = m_program.at(variable).function });
        for (const Store& store : std::ranges::equal_range(m_stores, variable, { }, &Store::variable)) {
            if (!addAll(values, evaluate(Activation { store.function }, store.stored)))
                break;
        }
        m_sawEverything &= !values.isEmpty();
        // What was read under the choice of one object holds for that choice alone.
        if (holdsForEveryChoice() && !m_split && m_walksCutShort == walksCutShortBefore)
            m_variables.set(variable.index(), ValuesOfVariable { values, m_sawEverything });
        else
            m_variables.remove(variable.index());
        m_sawEverything &= sawEverythingBefore;
        return values;
    }

    Constant constantOf(const Typed::DataNode& node) const
    {
        switch (node.kind()) {
        case Typed::DataNode::Kind::Undefined:
        case Typed::DataNode::Kind::Hole:
            return { Typed::KnownValue::undefined() };
        case Typed::DataNode::Kind::Null:
            return { Typed::KnownValue::null() };
        case Typed::DataNode::Kind::Boolean:
            return { Typed::KnownValue::boolean(node.as<Typed::DataNode::Boolean>().value) };
        case Typed::DataNode::Kind::Number:
            return { Typed::KnownValue::number(node.as<Typed::DataNode::Number>().value) };
        case Typed::DataNode::Kind::String:
            return { Typed::KnownValue::string(node.as<Typed::DataNode::String>().value) };
        case Typed::DataNode::Kind::BigInt:
            return { };
        case Typed::DataNode::Kind::Array:
        case Typed::DataNode::Kind::Object:
            return { .data = &node };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Constants fieldsOf(const Constants& objects, Typed::PropertyKeyID key)
    {
        Constants values;
        for (const Constant& object : objects) {
            if (object.regExp) {
                StringID source = key == m_program.keyOf(m_program.builtins().at(Builtins::RegExpPrototypeSource).name) ? sourceOf(*object.regExp) : StringID();
                if (!source)
                    m_sawEverything = false;
                else if (!add(values, Constant { Typed::KnownValue::string(source) }))
                    return { };
                continue;
            }
            if (const Typed::DataNode::Object* data = object.data ? object.data->tryAs<Typed::DataNode::Object>() : nullptr) {
                size_t index = data->members.findIf([&](const Typed::DataMember& member) {
                    return member.key == key;
                });
                if (!add(values, index == notFound ? Constant { Typed::KnownValue::undefined() } : constantOf(data->members[index].value)))
                    return { };
                continue;
            }
            const Typed::Value* literal = object.literal ? &m_program.at(object.function).at(object.literal) : nullptr;
            if (literal && literal->opcode == Typed::Opcode::NewDictionary) {
                if (!addAll(values, valuesWrittenUnder(object, key)))
                    return { };
                continue;
            }
            if (!literal || literal->opcode != Typed::Opcode::NewObject) {
                m_sawEverything = false;
                continue;
            }
            const Typed::Layout& layout = m_program.at(literal->layout());
            std::span<const uint32_t> fields = literal->fields();
            size_t index = std::ranges::find_if(fields, [&](uint32_t field) {
                return layout.fields[field].key == key;
            }) - fields.begin();
            if (index == fields.size()) {
                if (Constants written = valuesWrittenUnder(object, key); !written.isEmpty()) {
                    if (!addAll(values, written))
                        return { };
                    continue;
                }
                const Typed::Function& maker = m_program.at(object.function);
                ValueID defined;
                bool isFilledFromOthers = false;
                for (const Typed::Value& value : maker.values) {
                    if (value.opcode == Typed::Opcode::CopyDataProperties)
                        isFilledFromOthers |= throughCasts(maker, value.targetOperand()) == object.literal;
                    else if (value.opcode == Typed::Opcode::PropDefine && value.key() == key && throughCasts(maker, value.objectOperand()) == object.literal)
                        defined = value.valueOperand();
                }
                if (defined) {
                    if (!addAll(values, evaluate(WhereMade(object).activation, defined)))
                        return { };
                } else if (isFilledFromOthers)
                    m_sawEverything = false;
                else if (!add(values, Constant { Typed::KnownValue::undefined() }))
                    return { };
                continue;
            }
            if (!addAll(values, evaluate(WhereMade(object).activation, literal->valuesOperands()[index])))
                return { };
        }
        return values;
    }

    Constants elementsAt(const Activation& activation, const Constants& arrays, ValueID id)
    {
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        Constants positions = evaluate(activation, id);
        bool sawEveryPosition = std::exchange(m_sawEverything, sawEverythingBefore) && !positions.isEmpty();
        bool areKeysOfTable = !positions.isEmpty() && std::ranges::all_of(positions, [](const Constant& position) {
            return position.unnamed || position.primitive.kind() == Typed::KnownValue::Kind::String;
        });
        if (areKeysOfTable) {
            m_sawEverything &= sawEveryPosition;
            Constants values;
            for (const Constant& table : arrays) {
                for (const Constant& position : positions) {
                    Typed::PropertyKeyID key = position.unnamed ? Typed::PropertyKeyID() : keyNamed(table, position.primitive.asString());
                    if (!addAll(values, key ? fieldsOf({ table }, key) : table.literal ? valuesWrittenAt(table, position) : Constants()))
                        return { };
                }
            }
            return values;
        }
        if (!sawEveryPosition || positions.size() != 1 || !positions[0].primitive.isNumber())
            return elementsOf(arrays, std::nullopt);
        double position = positions[0].primitive.asNumber();
        if (position < 0 || position != static_cast<uint32_t>(position))
            return elementsOf(arrays, std::nullopt);
        return elementsOf(arrays, static_cast<uint32_t>(position));
    }

    Constants elementsOf(const Constants& arrays, std::optional<uint32_t> position)
    {
        Constants values;
        for (const Constant& array : arrays) {
            size_t size = sizeOf(array);
            if (size == notFound) {
                m_sawEverything = false;
                continue;
            }
            for (size_t index = position.value_or(0); index < (position ? std::min<size_t>(size, *position + 1) : size); ++index) {
                if (!addAll(values, elementOf(array, index)))
                    return { };
            }
        }
        return values;
    }

    static bool isCall(Typed::Opcode opcode)
    {
        return Typed::builtinCallOpcodes.contains(opcode) || opcode == Typed::Opcode::CallDirect || opcode == Typed::Opcode::Call || opcode == Typed::Opcode::CallMethod;
    }

    static ValueID throughCasts(const Typed::Function& function, ValueID id)
    {
        while (Typed::isCastThatProvesNothing(function.at(id).opcode) || function.at(id).opcode == Typed::Opcode::AssumeType)
            id = function.at(id).valueOperand();
        return id;
    }

    static ValueID throughChecks(const Typed::Function& function, ValueID id)
    {
        for (;;) {
            switch (function.at(id).opcode) {
            case Typed::Opcode::Upcast:
            case Typed::Opcode::Expect:
            case Typed::Opcode::AssumeType:
            case Typed::Opcode::CheckInitialized:
            case Typed::Opcode::FinishObject:
            case Typed::Opcode::RequireObjectCoercible:
            case Typed::Opcode::RequireObject:
                id = function.at(id).operand(0);
                continue;
            default:
                return id;
            }
        }
    }

    // The operation that a value comes of: casts, tests on the way and conversions that leave a number as it is are looked through.
    ValueID originOf(FunctionID id, const Typed::Function& function, ValueID value)
    {
        for (;;) {
            value = throughChecks(function, value);
            const Typed::Value& definition = function.at(value);
            if (definition.opcode == Typed::Opcode::ToNumeric || definition.opcode == Typed::Opcode::ToNumber) {
                value = definition.valueOperand();
                continue;
            }
            std::optional<BlockParameter> merge = findMerge(id, function, value);
            std::span<const Typed::Edge> edges = merge ? cfgOf(id).incomingEdges(merge->block) : std::span<const Typed::Edge>();
            if (edges.size() != 1)
                return value;
            Typed::IncomingValue incoming = incomingValueOf(function, edges[0], merge->index);
            ValueID arriving = incoming.kind == Typed::IncomingValue::Kind::Argument ? incoming.argument : operandNarrowedOnEdge(function, incoming.result);
            if (!arriving)
                return value;
            value = arriving;
        }
    }

    Constants evaluateOrName(const Activation& activation, ValueID id)
    {
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        Constants values = evaluate(activation, id);
        bool isKnown = std::exchange(m_sawEverything, sawEverythingBefore) && !values.isEmpty();
        if (isKnown)
            return values;
        Constant unnamed = literalIn(activation, ValueID());
        unnamed.unnamed = originOf(activation.function, m_program.at(activation.function), id);
        if (!add(values, unnamed))
            return { };
        return values;
    }

    // An object or an array is written through what holds it.
    enum class HolderKind : uint8_t {
        Literal,
        Variable,
        Capture,
        EnvironmentField,
        Member,
        Parameter,
    };

    struct Holder {
        HolderKind kind;
        uint32_t first;
        uint32_t second { 0 };
        friend auto operator<=>(const Holder&, const Holder&) = default;
    };

    struct WritingOperation {
        Holder holder;
        FunctionID function;
        ValueID operation;
    };

    struct WrittenPlace {
        ValueID container;
        Typed::PropertyKeyID key { };
        ValueID position { };
        ValueID stored;
        bool appends { false };
    };

    static constexpr Typed::OpcodeSet writingOpcodes { Typed::Opcode::PutSlot, Typed::Opcode::PropSet, Typed::Opcode::ElemSet, Typed::Opcode::PutArrayElement, Typed::Opcode::AppendElement, Typed::Opcode::CallBuiltin, Typed::Opcode::CallInline };
    static constexpr Typed::OpcodeSet madeOrReturnedBy { Typed::Opcode::NewObject, Typed::Opcode::NewDictionary, Typed::Opcode::NewArray, Typed::Opcode::NewContiguousArray, Typed::Opcode::CallDirect };
    static constexpr Typed::OpcodeSet holdingOpcodes { Typed::Opcode::PutModuleVariable, Typed::Opcode::NewClosure, Typed::Opcode::NewObject, Typed::Opcode::PutEnvironmentField, Typed::Opcode::PutSlot, Typed::Opcode::PropSet, Typed::Opcode::PropDefine, Typed::Opcode::CallDirect };

    std::optional<WrittenPlace> placeWrittenBy(const Typed::Value& value) const
    {
        switch (value.opcode) {
        case Typed::Opcode::PutSlot:
        case Typed::Opcode::PropSet:
            return WrittenPlace { .container = value.objectOperand(), .key = value.key(), .stored = value.valueOperand() };
        case Typed::Opcode::ElemSet:
            return WrittenPlace { .container = value.objectOperand(), .position = value.propertyOperand(), .stored = value.valueOperand() };
        case Typed::Opcode::PutArrayElement:
            return WrittenPlace { .container = value.arrayOperand(), .position = value.positionOperand(), .stored = value.valueOperand() };
        case Typed::Opcode::AppendElement:
            return WrittenPlace { .container = value.arrayOperand(), .stored = value.valueOperand(), .appends = true };
        case Typed::Opcode::CallBuiltin:
        case Typed::Opcode::CallInline: {
            const BuiltinTable& table = m_program.builtins();
            const BuiltinOverload& overload = table.at(value.overload());
            if (overload.mode != BuiltinCallMode::Call || table.canonicalID(overload.builtin) != Builtins::ArrayPrototypePush)
                return std::nullopt;
            Typed::BuiltinCall call = builtinCallOf(m_program, value);
            if (call.form != Typed::BuiltinCallForm::Whole || !call.receiver || call.arguments.size() != 1)
                return std::nullopt;
            return WrittenPlace { .container = call.receiver, .stored = call.arguments[0], .appends = true };
        }
        default:
            return std::nullopt;
        }
    }

    std::optional<Holder> holderOf(FunctionID id, const Typed::Function& function, ValueID container) const
    {
        container = throughChecks(function, container);
        const Typed::Value& value = function.at(container);
        switch (value.opcode) {
        case Typed::Opcode::NewObject:
        case Typed::Opcode::NewDictionary:
        case Typed::Opcode::NewArray:
        case Typed::Opcode::NewContiguousArray:
            return Holder { HolderKind::Literal, id.index(), container.index() };
        case Typed::Opcode::GetModuleVariable:
            return Holder { HolderKind::Variable, value.variable().index() };
        case Typed::Opcode::GetCapture:
            return Holder { HolderKind::Capture, sourceOf(value.function()).index(), value.index() };
        case Typed::Opcode::GetEnvironmentField:
            return Holder { HolderKind::EnvironmentField, value.layout().index(), value.field() };
        case Typed::Opcode::GetSlot:
        case Typed::Opcode::GetSlotOrUndefined:
        case Typed::Opcode::PropGet:
            return Holder { HolderKind::Member, value.key().index() };
        case Typed::Opcode::Parameter: {
            size_t index = function.blocks.first().parameters.find(container);
            if (index == notFound)
                return std::nullopt;
            return Holder { HolderKind::Parameter, id.index(), static_cast<uint32_t>(index) };
        }
        default:
            return std::nullopt;
        }
    }

    // An operation that hands an object or an array to a holder. `held` is the operation that makes it, or a call that returns it.
    struct Holding {
        FunctionID function;
        ValueID held;
        Holder holder;
        ValueID operation;

        friend bool operator==(const Holding&, const Holding&) = default;
    };

    void forEachHoldingBy(FunctionID id, const Typed::Function& function, ValueID operation, const Typed::Value& use, NOESCAPE const Invocable<void(const Holding&)> auto& functor) const
    {
        auto hold = [&](ValueID operand, Holder holder) {
            if (!operand)
                return;
            ValueID held = throughChecks(function, operand);
            if (madeOrReturnedBy.contains(function.at(held).opcode))
                functor(Holding { id, held, holder, operation });
        };
        switch (use.opcode) {
        case Typed::Opcode::PutModuleVariable:
            hold(use.valueOperand(), Holder { HolderKind::Variable, use.variable().index() });
            break;
        case Typed::Opcode::NewClosure: {
            std::span<const ValueID> captures = use.capturesOperands();
            for (uint32_t index = 0; index < captures.size() && use.function().index() < m_program.functions.size(); ++index)
                hold(captures[index], Holder { HolderKind::Capture, sourceOf(use.function()).index(), index });
            break;
        }
        case Typed::Opcode::NewObject: {
            const Typed::Layout& layout = m_program.at(use.layout());
            std::span<const uint32_t> fields = use.fields();
            std::span<const ValueID> values = use.valuesOperands();
            for (size_t index = 0; index < std::min(fields.size(), values.size()); ++index) {
                hold(values[index], Holder { HolderKind::EnvironmentField, use.layout().index(), fields[index] });
                hold(values[index], Holder { HolderKind::Member, layout.fields[fields[index]].key.index() });
            }
            break;
        }
        case Typed::Opcode::PutEnvironmentField:
            hold(use.valueOperand(), Holder { HolderKind::EnvironmentField, use.layout().index(), use.field() });
            break;
        case Typed::Opcode::PutSlot:
        case Typed::Opcode::PropSet:
        case Typed::Opcode::PropDefine:
            hold(use.valueOperand(), Holder { HolderKind::Member, use.key().index() });
            break;
        case Typed::Opcode::CallDirect: {
            std::span<const ValueID> arguments = use.argumentsOperands();
            for (uint32_t index = 0; index < arguments.size(); ++index)
                hold(arguments[index], Holder { HolderKind::Parameter, use.function().index(), index });
            break;
        }
        default:
            break;
        }
    }

    void findWritesAndHoldings()
    {
        if (m_hasFoundWritesAndHoldings)
            return;
        m_hasFoundWritesAndHoldings = true;
        const ParallelLoops& loops = m_context.parallelLoops();
        unsigned numberOfFunctions = m_program.functions.size();
        m_writingOperations = collectInParallel<WritingOperation>(loops, numberOfFunctions, [&](unsigned index, Vector<WritingOperation>& found) {
            FunctionID id(index);
            const Typed::Function& function = m_program.at(id);
            forEachValueListedOrNot(m_program, id, writingOpcodes, [&](ValueID operation, const Typed::Value& value) {
                std::optional<WrittenPlace> place = placeWrittenBy(value);
                std::optional<Holder> holder = place ? holderOf(id, function, place->container) : std::nullopt;
                if (holder)
                    found.append(WritingOperation { *holder, id, operation });
            });
        });
        m_holdings = collectInParallel<Holding>(loops, numberOfFunctions, [&](unsigned index, Vector<Holding>& found) {
            FunctionID id(index);
            const Typed::Function& function = m_program.at(id);
            forEachValueListedOrNot(m_program, id, holdingOpcodes, [&](ValueID operation, const Typed::Value& use) {
                forEachHoldingBy(id, function, operation, use, [&](const Holding& holding) {
                    found.append(holding);
                });
            });
        });
        parallelSort(loops, m_writingOperations.mutableSpan(), [](const WritingOperation& a, const WritingOperation& b) {
            return std::tuple { a.holder, a.function, a.operation } < std::tuple { b.holder, b.function, b.operation };
        });
        parallelSort(loops, m_holdings.mutableSpan(), [](const Holding& a, const Holding& b) {
            return std::tuple { a.function, a.held, a.holder, a.operation } < std::tuple { b.function, b.held, b.holder, b.operation };
        });
    }

    Vector<Holding, 4> holdingsOf(const Constant& container)
    {
        findWritesAndHoldings();
        Vector<Holding, 4> holdings { Holding { container.function, container.literal, Holder { HolderKind::Literal, container.function.index(), container.literal.index() }, container.literal } };
        auto addHoldingsOf = [&](FunctionID function, ValueID held) {
            auto placeOf = [](const Holding& holding) {
                return std::pair { holding.function, holding.held };
            };
            for (const Holding& holding : std::ranges::equal_range(m_holdings, std::pair { function, held }, { }, placeOf)) {
                bool isKnown = holding.holder.kind != HolderKind::Parameter && holdings.containsIf([&](const Holding& other) {
                    return other.holder == holding.holder;
                });
                if (!isKnown)
                    holdings.append(holding);
            }
        };
        addHoldingsOf(container.function, container.literal);
        const Typed::Function& maker = m_program.at(container.function);
        bool isReturned = container.call && std::ranges::any_of(maker.blocks, [&](const Typed::Block& block) {
            return block.terminator.kind == Typed::TerminatorKind::Return && block.terminator.valueOperand() && throughChecks(maker, block.terminator.valueOperand()) == container.literal;
        });
        if (isReturned)
            addHoldingsOf(container.caller, idOf(container.caller, *container.call));
        return holdings;
    }

    // `where` is read as a literal is: the function, the call under which it runs, and for `literal` the operation that writes.
    struct Write {
        Constant where;
        Typed::PropertyKeyID key { };
        Constant position { };
        ValueID stored;
    };

    struct Contents {
        Vector<Write> writes;
        Vector<Write> appends;
        bool knowsOrderOfAppends { true };
    };

    static constexpr uint64_t notListed = std::numeric_limits<uint64_t>::max();

    // By value: the place of its block in reverse post order, and below it the place of the value in the block.
    const Vector<uint64_t>& placesIn(FunctionID id)
    {
        return m_places.ensure(id.index(), [&] {
            const Typed::Function& function = m_program.at(id);
            const Typed::CFG& cfg = cfgOf(id);
            Vector<uint64_t> places;
            places.fill(notListed, function.values.size());
            for (uint32_t block = 0; block < function.blocks.size(); ++block) {
                if (!cfg.isReachable(Typed::BlockID(block)))
                    continue;
                uint64_t place = static_cast<uint64_t>(cfg.reversePostOrderIndex(Typed::BlockID(block))) << 32;
                for (ValueID value : function.blocks[block].values)
                    places[value.index()] = place++;
            }
            return places;
        }).iterator->value;
    }

    Typed::BlockID blockAt(FunctionID id, uint64_t place) { return cfgOf(id).reversePostOrder()[place >> 32]; }

    ValueID idOf(FunctionID function, const Typed::Value& value) const { return ValueID(static_cast<uint32_t>(&value - m_program.at(function).values.span().data())); }

    // One store of the source may stand on two arms, of which one runs.
    bool isOtherArmOf(const Write& a, const Write& b)
    {
        if (a.where.function != b.where.function || a.where.call != b.where.call)
            return false;
        const Typed::Function& function = m_program.at(a.where.function);
        if (function.at(a.where.literal).position != function.at(b.where.literal).position || function.at(a.where.literal).inlineFrame != function.at(b.where.literal).inlineFrame)
            return false;
        const Vector<uint64_t>& places = placesIn(a.where.function);
        Typed::BlockID blockOfA = blockAt(a.where.function, places[a.where.literal.index()]);
        Typed::BlockID blockOfB = blockAt(a.where.function, places[b.where.literal.index()]);
        const Typed::CFG& cfg = cfgOf(a.where.function);
        return blockOfA != blockOfB && !cfg.blocksReachableFrom(blockOfA, { }).get(blockOfB.index()) && !cfg.blocksReachableFrom(blockOfB, { }).get(blockOfA.index());
    }

    void addWrites(Contents& contents, const Constant& container, const Activation& activation, ValueID operation)
    {
        if (placesIn(activation.function)[operation.index()] == notListed)
            return;
        WrittenPlace place = *placeWrittenBy(m_program.at(activation.function).at(operation));
        bool writesContainer = evaluate(activation, place.container).containsIf([&](const Constant& written) {
            return written.function == container.function && written.literal == container.literal && (!written.call || !container.call || written.call == container.call);
        });
        if (!writesContainer)
            return;
        Write write { .where = literalIn(activation, operation), .key = place.key, .stored = place.stored };
        if (place.appends) {
            bool isCounted = contents.appends.containsIf([&](const Write& other) {
                return isOtherArmOf(write, other);
            });
            if (!isCounted)
                contents.appends.append(write);
            return;
        }
        if (!place.position) {
            contents.writes.append(write);
            return;
        }
        for (const Constant& position : evaluateOrName(activation, place.position)) {
            write.position = position;
            contents.writes.append(write);
        }
    }

    void putAppendsInOrder(Contents& contents)
    {
        struct Place {
            FunctionID function;
            uint64_t outer;
            uint64_t inner;
        };
        auto placeOf = [&](const Write& write) {
            uint64_t place = placesIn(write.where.function)[write.where.literal.index()];
            if (!write.where.call)
                return Place { write.where.function, place, 0 };
            return Place { write.where.caller, placesIn(write.where.caller)[idOf(write.where.caller, *write.where.call).index()], place };
        };
        for (const Write& write : contents.appends)
            contents.knowsOrderOfAppends &= placeOf(write).function == placeOf(contents.appends[0]).function && placeOf(write).outer != notListed;
        AOT::stableSort(contents.appends, [&](const Write& a, const Write& b) {
            return std::tuple { placeOf(a).outer, placeOf(a).inner } < std::tuple { placeOf(b).outer, placeOf(b).inner };
        });
    }

    // What is written into an object or an array after it was made, wherever that is done. It is found once for all walks: with a budget of its own, under no choice of an object and inside no other value.
    const Contents& contentsOf(const Constant& container)
    {
        auto [entry, isNew] = m_contents.try_emplace(std::tuple { container.function.index(), container.literal.index(), container.call });
        if (!isNew) {
            if (entry->second)
                return *entry->second;
            ++m_walksCutShort;
            return m_contentsBeingFound;
        }
        SetForScope budget(m_budget, maxValuesFollowed);
        SetForScope testsBeingAnswered(m_testsBeingAnswered, 0u);
        unsigned valuesFollowedBefore = std::exchange(m_valuesFollowed, 0);
        unsigned walksCutShortBefore = m_walksCutShort;
        bool sawEverythingBefore = m_sawEverything;
        auto bindingsBefore = std::exchange(m_bindings, { });
        auto loopsFollowedBefore = std::exchange(m_loopsFollowed, { });
        auto valuesInTurnBefore = std::exchange(m_valuesInTurn, { });
        auto activeBefore = std::exchange(m_active, { });
        std::optional<Split> splitBefore = std::exchange(m_split, std::nullopt);

        auto contents = makeUniqueWithoutFastMallocCheck<Contents>();
        for (const Holding& holding : holdingsOf(container)) {
            for (const WritingOperation& operation : std::ranges::equal_range(m_writingOperations, holding.holder, { }, &WritingOperation::holder)) {
                if (holding.holder.kind == HolderKind::Parameter) {
                    Activation caller { holding.function };
                    addWrites(*contents, container, Activation { operation.function, &caller, &m_program.at(holding.function).at(holding.operation) }, operation.operation);
                    continue;
                }
                if (operation.function == container.function) {
                    addWrites(*contents, container, WhereMade(container).activation, operation.operation);
                    continue;
                }
                bool isCalledDirectly = false;
                for (const DirectCall& call : std::ranges::equal_range(m_directCalls, operation.function, { }, &DirectCall::callee)) {
                    isCalledDirectly = true;
                    Activation caller { call.caller };
                    addWrites(*contents, container, Activation { operation.function, &caller, &m_program.at(call.caller).at(call.call) }, operation.operation);
                }
                if (!isCalledDirectly)
                    addWrites(*contents, container, Activation { operation.function }, operation.operation);
            }
        }
        putAppendsInOrder(*contents);

        m_valuesFollowed = valuesFollowedBefore;
        m_walksCutShort = walksCutShortBefore;
        m_sawEverything = sawEverythingBefore;
        m_bindings = WTF::move(bindingsBefore);
        m_loopsFollowed = WTF::move(loopsFollowedBefore);
        m_valuesInTurn = WTF::move(valuesInTurnBefore);
        m_active = WTF::move(activeBefore);
        m_split = WTF::move(splitBefore);
        entry->second = WTF::move(contents);
        return *entry->second;
    }

    Constants valuesWritten(const Constant& container, NOESCAPE const Invocable<bool(const Write&)> auto& isRead)
    {
        Constants values;
        for (const Write& write : contentsOf(container).writes) {
            if (isRead(write) && !addAll(values, evaluateOrName(WhereMade(write.where).activation, write.stored)))
                return { };
        }
        return values;
    }

    Constants valuesWrittenUnder(const Constant& object, Typed::PropertyKeyID key)
    {
        const Typed::PropertyKey& name = m_program.at(key);
        Constant text;
        if (name.kind == Typed::PropertyKeyKind::Text && name.text)
            text.primitive = Typed::KnownValue::string(name.text);
        return valuesWritten(object, [&](const Write& write) {
            if (write.position.unnamed)
                m_sawEverything = false;
            return write.key ? write.key == key : text.primitive.isKnown() && write.position == text;
        });
    }

    // The key under which the object itself holds or is given a member of that name.
    Typed::PropertyKeyID keyNamed(const Constant& object, StringID text)
    {
        auto isNamed = [&](Typed::PropertyKeyID key) {
            const Typed::PropertyKey& name = m_program.at(key);
            return name.kind == Typed::PropertyKeyKind::Text && name.text == text;
        };
        if (const Typed::DataNode::Object* data = object.data ? object.data->tryAs<Typed::DataNode::Object>() : nullptr) {
            for (const Typed::DataMember& member : data->members) {
                if (isNamed(member.key))
                    return member.key;
            }
        }
        if (!object.literal)
            return { };
        const Typed::Value& literal = m_program.at(object.function).at(object.literal);
        if (literal.opcode == Typed::Opcode::NewObject) {
            const Typed::Layout& layout = m_program.at(literal.layout());
            for (uint32_t field : literal.fields()) {
                if (isNamed(layout.fields[field].key))
                    return layout.fields[field].key;
            }
        }
        if (literal.opcode != Typed::Opcode::NewObject && literal.opcode != Typed::Opcode::NewDictionary)
            return { };
        for (const Write& write : contentsOf(object).writes) {
            if (write.key && isNamed(write.key))
                return write.key;
        }
        return { };
    }

    Constants valuesWrittenAt(const Constant& array, const Constant& position)
    {
        return valuesWritten(array, [&](const Write& write) {
            return !write.key && write.position == position;
        });
    }

    struct MappedArray {
        Constant source;
        ValueID stored;
        FunctionID callback;
        Vector<ValueKey, 2> readsOfSource;
    };

    std::optional<MappedArray> mappedArrayOf(const Constant& array)
    {
        const Typed::Function& function = m_program.at(array.function);
        const Typed::Value& literal = function.at(array.literal);
        ValueID sourceOperand = literal.opcode == Typed::Opcode::NewArrayLike ? literal.sourceOperand() : ValueID();
        ValueID callback;
        if (literal.opcode == Typed::Opcode::CallDirect) {
            sourceOperand = literal.argumentsOperands()[0];
            callback = literal.argumentsOperands()[1];
        } else {
            forEachBuiltinCall(literal, [&](const Typed::BuiltinCall& builtinCall) {
                sourceOperand = builtinCall.receiver;
                callback = builtinCall.arguments[0];
            });
        }
        bool isMappedByCallback = isCall(literal.opcode);
        if (!sourceOperand && !isMappedByCallback)
            return std::nullopt;
        WhereMade whereMade(array);
        Constants sources = evaluate(whereMade.activation, sourceOperand);
        Constants callbacks = isMappedByCallback ? evaluate(whereMade.activation, callback) : Constants();
        if (sources.size() != 1 || !sources[0].hasElements())
            return std::nullopt;
        MappedArray mapped { sources[0], { }, { }, { } };

        if (isMappedByCallback) {
            if (callbacks.size() != 1)
                return std::nullopt;
            mapped.callback = callbacks[0].declaration;
            if (callbacks[0].literal) {
                const Typed::Value& closure = m_program.at(callbacks[0].function).at(callbacks[0].literal);
                if (closure.opcode == Typed::Opcode::NewClosure)
                    mapped.callback = closure.function();
            }
            if (!mapped.callback || !m_program.at(mapped.callback).hasBody())
                return std::nullopt;
            const Typed::Function& body = m_program.at(mapped.callback);
            if (body.firstParameterIndex() < body.blocks.first().parameters.size())
                mapped.readsOfSource.append(ValueKey { mapped.callback, body.blocks.first().parameters[body.firstParameterIndex()] });
            return mapped;
        }

        ValueID source = throughCasts(function, sourceOperand);
        ValueID position;
        for (const Typed::Value& value : function.values) {
            if (value.opcode == Typed::Opcode::PutArrayElement && throughCasts(function, value.arrayOperand()) == array.literal) {
                if (mapped.stored)
                    return std::nullopt;
                mapped.stored = value.valueOperand();
                position = throughCasts(function, value.positionOperand());
            }
        }
        if (!mapped.stored)
            return std::nullopt;
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            const Typed::Value& value = function.values[index];
            if (value.opcode == Typed::Opcode::GetArrayElement && throughCasts(function, value.arrayOperand()) == source && throughCasts(function, value.positionOperand()) == position)
                mapped.readsOfSource.append(ValueKey { array.function, ValueID(index) });
        }
        return mapped;
    }

    const Vector<ValueID>& elementsOfLongLiteral(const Constant& array)
    {
        return m_longLiterals.ensure(static_cast<uint64_t>(array.function.index()) << 32 | array.literal.index(), [&] {
            const Typed::Function& function = m_program.at(array.function);
            Vector<ValueID> elements;
            for (const Typed::Value& value : function.values) {
                if (value.opcode != Typed::Opcode::PutContiguousIndex || throughCasts(function, value.arrayOperand()) != array.literal)
                    continue;
                Typed::KnownValue position = knownValueOf(m_program, function, value.positionOperand());
                if (!position.isNumber() || position.asNumber() != elements.size())
                    return Vector<ValueID>();
                elements.append(value.valueOperand());
            }
            return elements;
        }).iterator->value;
    }

    size_t sizeOf(const Constant& array)
    {
        if (array.listOfTexts)
            return m_listsOfTexts[array.listOfTexts - 1].size();
        if (const Typed::DataNode::Array* data = array.data ? array.data->tryAs<Typed::DataNode::Array>() : nullptr)
            return data->elements.size();
        const Typed::Value* literal = array.literal ? &m_program.at(array.function).at(array.literal) : nullptr;
        if (literal && literal->opcode == Typed::Opcode::NewArray && literal->holes().empty()) {
            const Contents& contents = contentsOf(array);
            return contents.knowsOrderOfAppends ? literal->elementsOperands().size() + contents.appends.size() : notFound;
        }
        if (literal && literal->opcode == Typed::Opcode::NewContiguousArray)
            return elementsOfLongLiteral(array).size();
        if (literal && (literal->opcode == Typed::Opcode::NewArrayLike || isCall(literal->opcode))) {
            if (std::optional<MappedArray> mapped = mappedArrayOf(array))
                return sizeOf(mapped->source);
        }
        return notFound;
    }

    Constants elementOf(const Constant& array, size_t index)
    {
        if (array.listOfTexts)
            return { Constant { Typed::KnownValue::string(m_listsOfTexts[array.listOfTexts - 1][index]) } };
        if (array.data)
            return { constantOf(array.data->as<Typed::DataNode::Array>().elements[index]) };
        const Typed::Value& literal = m_program.at(array.function).at(array.literal);
        if (literal.opcode == Typed::Opcode::NewArray) {
            size_t sizeWhenMade = literal.elementsOperands().size();
            if (index < sizeWhenMade)
                return evaluate(WhereMade(array).activation, literal.elementsOperands()[index]);
            const Write& write = contentsOf(array).appends[index - sizeWhenMade];
            return evaluate(WhereMade(write.where).activation, write.stored);
        }
        if (literal.opcode == Typed::Opcode::NewContiguousArray)
            return evaluate(WhereMade(array).activation, elementsOfLongLiteral(array)[index]);

        std::optional<MappedArray> mapped = mappedArrayOf(array);
        if (!mapped)
            return { };
        Constants elements = elementOf(mapped->source, index);
        if (elements.size() != 1 || (mapped->callback && mapped->readsOfSource.isEmpty()))
            return { };
        size_t numberOfBindings = m_bindings.size();
        for (ValueKey read : mapped->readsOfSource)
            m_bindings.append({ read, elements[0] });
        Constants values = mapped->callback ? returnsOf(Activation { mapped->callback }) : evaluate(WhereMade(array).activation, mapped->stored);
        m_bindings.shrink(numberOfBindings);
        return values;
    }

    Constants lengthsOf(const Constants& strings)
    {
        Constants lengths;
        Vector<char16_t> buffer;
        for (const Constant& string : strings) {
            if (string.primitive.kind() != Typed::KnownValue::Kind::String)
                continue;
            if (!add(lengths, Constant { Typed::KnownValue::number(m_program.strings.viewOf(string.primitive.asString(), buffer).length()) }))
                return { };
        }
        return lengths;
    }

    using TextsWhereBlocksEnd = std::map<std::pair<ValueID, Typed::BlockID>, Constants>;

    Constants textsBuiltUpTo(const Activation& activation, const Typed::Function& function, ValueID builder, ValueID end)
    {
        uint64_t place = placesIn(activation.function)[end.index()];
        if (place == notListed)
            return { };
        TextsWhereBlocksEnd remembered;
        return textsBuiltIn(activation, function, builder, blockAt(activation.function, place), end, remembered);
    }

    // What the builder holds in front of `end`, and without an end where the block ends. The builder may have been begun blocks ago.
    Constants textsBuiltIn(const Activation& activation, const Typed::Function& function, ValueID builder, Typed::BlockID block, ValueID end, TextsWhereBlocksEnd& remembered)
    {
        if (!end) {
            if (auto known = remembered.find({ builder, block }); known != remembered.end())
                return known->second;
        }
        if (!mayFollowAnotherValue())
            return { };
        std::optional<BlockParameter> merge = findMerge(activation.function, function, builder);
        bool arrivesAsParameter = merge && merge->block == block;
        bool isBegunHere = !merge && function.at(block).values.contains(builder);
        if (isBegunHere && function.at(builder).opcode != Typed::Opcode::StringBuilderBegin)
            return { };
        Vector<Constants, 8> parts;
        if (!isBegunHere) {
            // A block that is asked for again while it is walked lies on a way round that nobody follows.
            if (!end)
                remembered.emplace(std::pair { builder, block }, Constants());
            const Typed::CFG& cfg = cfgOf(activation.function);
            Constants before;
            for (Typed::Edge edge : cfg.incomingEdges(block)) {
                if (!cfg.isReachable(edge.from) || isOffTheWayFollowed(activation, block, edge))
                    continue;
                if (cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(block) && !liesInFrontOfHeaderFollowed(activation, block))
                    continue;
                if (isRuledOut(activation, function, cfg, edge))
                    continue;
                ValueID arriving = builder;
                if (arrivesAsParameter) {
                    Typed::IncomingValue incoming = incomingValueOf(function, edge, merge->index);
                    if (incoming.kind != Typed::IncomingValue::Kind::Argument)
                        return { };
                    arriving = incoming.argument;
                }
                if (!addAll(before, textsWhereEdgeBegins(activation, function, arriving, edge, block, remembered)))
                    return { };
            }
            parts.append(WTF::move(before));
        }
        Constants texts = textsAppendedIn(activation, function, builder, block, end, WTF::move(parts));
        if (!end)
            remembered.insert_or_assign(std::pair { builder, block }, texts);
        return texts;
    }

    // A block that both the way into a loop and the way round it pass. The way round comes to it from below.
    bool liesInFrontOfHeaderFollowed(const Activation& activation, Typed::BlockID block) const
    {
        return m_loopsFollowed.containsIf([&](const LoopFollowed& followed) {
            return followed.part == PartOfLoop::Turn && followed.call == activation.call && followed.function == activation.function && followed.loop->turn.get(block.index()) && followed.loop->wayIn.get(block.index());
        });
    }

    // What the builder holds where the edge leaves its block. `to` is the block that the edge leads to.
    Constants textsWhereEdgeBegins(const Activation& activation, const Typed::Function& function, ValueID builder, Typed::Edge edge, Typed::BlockID to, TextsWhereBlocksEnd& remembered)
    {
        if (const Loop* loop = loopTestedAt(activation.function, function, edge.from); loop && !loop->turn.get(to.index())) {
            if (std::optional<Constants> texts = textsBehindLoop(activation, function, builder, *loop))
                return WTF::move(*texts);
        }
        return textsBuiltIn(activation, function, builder, edge.from, ValueID(), remembered);
    }

    // `parts` is what the builder holds where the block begins. The answer is what it holds in front of `end`, and without an end where the block ends.
    Constants textsAppendedIn(const Activation& activation, const Typed::Function& function, ValueID builder, Typed::BlockID block, ValueID end, Vector<Constants, 8>&& parts)
    {
        for (ValueID id : function.at(block).values) {
            const Typed::Value& value = function.at(id);
            if (id == builder || !Typed::OperandPosition::builder(value.opcode) || value.builderOperand() != builder)
                continue;
            if (id == end)
                return concatenated(parts.span());
            switch (value.opcode) {
            case Typed::Opcode::StringBuilderAppendConstant:
                parts.append(Constants { Constant { Typed::KnownValue::string(value.string()) } });
                break;
            case Typed::Opcode::StringBuilderAppendString:
            case Typed::Opcode::StringBuilderAppendValue:
                parts.append(evaluate(activation, value.valueOperand()));
                break;
            default:
                return { };
            }
        }
        if (end)
            return { };
        return concatenated(parts.span());
    }

    Constants concatenated(std::span<const Constants> parts)
    {
        Constants texts;
        addAnswerForEachChoice(texts, parts, [&](std::span<const Constant> choice) -> std::optional<Constant> {
            StringBuilder builder;
            Vector<char16_t> buffer;
            for (const Constant& part : choice) {
                if (part.primitive.isNumber())
                    builder.append(String::number(part.primitive.asNumber()));
                else if (part.primitive.kind() == Typed::KnownValue::Kind::String)
                    builder.append(m_program.strings.viewOf(part.primitive.asString(), buffer));
                else
                    return std::nullopt;
            }
            return textWithinLimit(builder);
        });
        return texts;
    }

    Constants rawTextsOf(const Activation& activation, std::span<const ValueID> arguments)
    {
        const Typed::Function& function = m_program.at(activation.function);
        const Typed::Value& templateObject = function.at(sameObjectThroughCasts(function, arguments[0]));
        if (templateObject.opcode != Typed::Opcode::NewTemplateObject)
            return { };
        std::span<const ValueID> substitutions = arguments.subspan(1);
        auto templateStrings = templateObject.templateStrings();
        Vector<Constants, 8> parts;
        for (size_t index = 0; index < templateStrings.size(); ++index) {
            parts.append(Constants { Constant { Typed::KnownValue::string(templateStrings[index].raw) } });
            if (index + 1 < templateStrings.size() && index < substitutions.size())
                parts.append(evaluate(activation, substitutions[index]));
        }
        return concatenated(parts.span());
    }

    Constants joined(const Constants& arrays, const Constants& separators)
    {
        Constants texts;
        for (const Constant& array : arrays) {
            size_t size = sizeOf(array);
            if (size == notFound || size > maxValuesFollowed)
                continue;
            Vector<Constants, 8> parts;
            parts.append(separators);
            for (size_t index = 0; index < size; ++index)
                parts.append(elementOf(array, index));
            bool isWithinLimits = addAnswerForEachChoice(texts, parts.span(), [&](std::span<const Constant> choice) -> std::optional<Constant> {
                StringBuilder builder;
                Vector<char16_t> buffer;
                for (size_t index = 1; index < choice.size(); ++index) {
                    if (index > 1) {
                        if (choice[0].primitive.kind() == Typed::KnownValue::Kind::Undefined)
                            builder.append(',');
                        else if (choice[0].primitive.kind() == Typed::KnownValue::Kind::String)
                            builder.append(m_program.strings.viewOf(choice[0].primitive.asString(), buffer));
                        else
                            return std::nullopt;
                    }
                    if (choice[index].primitive.isNullish())
                        continue;
                    if (choice[index].primitive.kind() != Typed::KnownValue::Kind::String)
                        return std::nullopt;
                    builder.append(m_program.strings.viewOf(choice[index].primitive.asString(), buffer));
                }
                return textWithinLimit(builder);
            });
            if (!isWithinLimits)
                break;
        }
        return texts;
    }

    struct BlockParameter {
        ValueID parameter;
        Typed::BlockID block;
        uint32_t index;
    };

    const Typed::CFG& cfgOf(FunctionID function)
    {
        return *m_cfgs.ensure(function.index(), [&] {
            return makeUnique<Typed::CFG>(m_program.at(function));
        }).iterator->value;
    }

    std::optional<BlockParameter> findMerge(FunctionID id, const Typed::Function& function, ValueID parameter)
    {
        std::optional<BlockParameter> found = function.at(parameter).opcode == Typed::Opcode::Parameter ? findBlockParameter(id, function, parameter) : std::nullopt;
        return found && found->block != Typed::entryBlock ? found : std::nullopt;
    }

    std::optional<BlockParameter> findBlockParameter(FunctionID id, const Typed::Function& function, ValueID parameter)
    {
        const Vector<BlockParameter>& sortedParameters = m_blockParameters.ensure(id.index(), [&] {
            Vector<BlockParameter> result;
            for (uint32_t block = 0; block < function.blocks.size(); ++block) {
                const auto& parameters = function.blocks[block].parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index)
                    result.append(BlockParameter { parameters[index], Typed::BlockID(block), index });
            }
            AOT::stableSortInLine(result, { }, &BlockParameter::parameter);
            return result;
        }).iterator->value;
        auto found = std::ranges::lower_bound(sortedParameters, parameter, { }, &BlockParameter::parameter);
        if (found == sortedParameters.end() || found->parameter != parameter)
            return std::nullopt;
        return *found;
    }

    static constexpr Typed::OpcodeSet testsOfCounters { Typed::Opcode::Less, Typed::Opcode::LessEq, Typed::Opcode::Greater, Typed::Opcode::GreaterEq };

    const Loop* loopTestedAt(FunctionID id, const Typed::Function& function, Typed::BlockID header)
    {
        const Typed::CFG& cfg = cfgOf(id);
        const Typed::Block& block = function.at(header);
        const Typed::Terminator& terminator = block.terminator;
        if (!cfg.hasCycle() || (terminator.kind != Typed::TerminatorKind::TestAndBranch && terminator.kind != Typed::TerminatorKind::Branch) || terminator.targets.size() != 2)
            return nullptr;
        const Typed::Value& test = function.at(terminator.conditionOperand());
        if (!testsOfCounters.contains(test.opcode))
            return nullptr;
        if (!block.parameters.contains(throughChecks(function, test.leftOperand())) && !block.parameters.contains(throughChecks(function, test.rightOperand())))
            return nullptr;
        return m_loops.ensure(static_cast<uint64_t>(id.index()) << 32 | header.index(), [&]() -> std::unique_ptr<Loop> {
            BitVector atHeader(function.blocks.size());
            atHeader.set(header.index());
            BitVector comesBack(function.blocks.size());
            Vector<Typed::BlockID, 16> worklist { header };
            while (!worklist.isEmpty()) {
                for (Typed::BlockID before : cfg.predecessors(worklist.takeLast())) {
                    if (before != header && cfg.isReachable(before) && !comesBack.set(before.index()))
                        worklist.append(before);
                }
            }
            auto loop = makeUniqueWithoutFastMallocCheck<Loop>();
            loop->header = header;
            unsigned targetsThatStay = 0;
            for (uint32_t index = 0; index < 2; ++index) {
                Typed::BlockID target = terminator.targets[index].block;
                if (target != header && !comesBack.get(target.index()))
                    continue;
                ++targetsThatStay;
                loop->targetThatStays = index;
                if (target != header)
                    loop->turn = cfg.blocksReachableFrom(target, atHeader);
            }
            if (targetsThatStay != 1)
                return nullptr;
            loop->turn.ensureSize(function.blocks.size());
            loop->turn.filter(comesBack);
            for (size_t index : loop->turn) {
                for (Typed::BlockID next : cfg.successors(Typed::BlockID(index)))
                    loop->isLeftAtHeaderAlone &= next == header || loop->turn.get(next.index()) || Typed::wayOutOf(function.at(next).terminator.kind) == Typed::WayOut::Abnormal;
            }
            loop->turn.set(header.index());
            loop->wayIn = cfg.blocksReachableFrom(Typed::entryBlock, atHeader);
            return loop;
        }).iterator->value.get();
    }

    // evaluate() has put the parameter itself on top of the values that are being walked. Below it lies whoever reads it.
    bool isReadInTurn(const Activation& activation, const Typed::Function& function, const Loop& loop)
    {
        if (m_active.size() < 2)
            return !activation.call && activation.function == m_subject.function && loop.turn.get(m_subject.block.index());
        auto [call, reader, value] = m_active[m_active.size() - 2];
        if (call != activation.call || reader != activation.function)
            return false;
        if (std::optional<BlockParameter> merge = findMerge(reader, function, value))
            return loop.turn.get(merge->block.index());
        uint64_t place = placesIn(reader)[value.index()];
        return place != notListed && loop.turn.get(blockAt(reader, place).index());
    }

    // It is read along the part that is followed, and holds for that part alone.
    bool isDefinedInLoopFollowed(const Activation& activation, const Typed::Function& function, ValueID id)
    {
        Typed::BlockID block;
        for (const LoopFollowed& followed : m_loopsFollowed) {
            if (followed.call != activation.call || followed.function != activation.function)
                continue;
            if (!block) {
                std::optional<BlockParameter> merge = findMerge(activation.function, function, id);
                uint64_t place = placesIn(activation.function)[id.index()];
                if (!merge && place == notListed)
                    return false;
                block = merge ? merge->block : blockAt(activation.function, place);
            }
            if (followed.loop->turn.get(block.index()))
                return true;
        }
        return false;
    }

    // While a loop is followed, what arrives at a block of it comes along the part that is followed.
    bool isOffTheWayFollowed(const Activation& activation, Typed::BlockID block, Typed::Edge edge) const
    {
        return m_loopsFollowed.containsIf([&](const LoopFollowed& followed) {
            if (followed.call != activation.call || followed.function != activation.function || !followed.loop->turn.get(block.index()))
                return false;
            return !(followed.part == PartOfLoop::WayIn ? followed.loop->wayIn : followed.loop->turn).get(edge.from.index());
        });
    }

    struct ValuesAtHeader {
        Constants values;
        bool sawEverything { false };
    };

    Vector<ValuesAtHeader, 4> valuesArrivingAtHeader(const Activation& activation, const Typed::Function& function, const Loop& loop, PartOfLoop part)
    {
        m_loopsFollowed.append(LoopFollowed { activation.call, activation.function, &loop, part });
        Vector<ValuesAtHeader, 4> arriving(function.at(loop.header).parameters.size());
        for (uint32_t index = 0; index < arriving.size(); ++index) {
            bool sawEverythingBefore = std::exchange(m_sawEverything, true);
            for (Typed::Edge edge : cfgOf(activation.function).incomingEdges(loop.header)) {
                if (isOffTheWayFollowed(activation, loop.header, edge))
                    continue;
                Typed::IncomingValue incoming = incomingValueOf(function, edge, index);
                if (!addAll(arriving[index].values, incoming.kind == Typed::IncomingValue::Kind::Argument ? evaluate(activation, incoming.argument) : Constants()))
                    break;
            }
            arriving[index].sawEverything = std::exchange(m_sawEverything, sawEverythingBefore) && !arriving[index].values.isEmpty();
        }
        m_loopsFollowed.removeLast();
        return arriving;
    }

    // How far the two sides of the test are apart, where both are one number.
    std::optional<double> distanceTested(const Activation& activation, const Typed::Value& test)
    {
        bool sawEverythingBefore = m_sawEverything;
        Constants left = evaluate(activation, test.leftOperand());
        Constants right = evaluate(activation, test.rightOperand());
        m_sawEverything = sawEverythingBefore;
        if (left.size() != 1 || right.size() != 1 || !left[0].primitive.isNumber() || !right[0].primitive.isNumber())
            return std::nullopt;
        return std::abs(left[0].primitive.asNumber() - right[0].primitive.asNumber());
    }

    using NameOfCarried = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

    NameOfCarried nameOf(const Activation& activation, const Loop& loop, uint32_t carried) { return { callsLeadingTo(activation), activation.function.index(), loop.header.index(), carried }; }

    // A loop over a list that the walk knows hands a value from row to row: the value of one turn comes of the value of the turn before and of that turn's row. Each turn is walked as any
    // other code is, with the parameters of the header known, and the test of the header says which pass of it is the last. `atHeader` is told at each pass whether the loop goes on, and
    // answers whether it can still tell what it is after. `carried` names that among all that the loop hands on.
    std::optional<Vector<ValuesAtHeader, 4>> followLoop(const Activation& activation, const Typed::Function& function, const Loop& loop, uint32_t carried, NOESCAPE const Invocable<bool(bool goesOn)> auto& atHeader)
    {
        bool isBeingFollowed = m_loopsFollowed.containsIf([&](const LoopFollowed& followed) {
            return followed.loop == &loop && followed.call == activation.call;
        });
        NameOfCarried name = nameOf(activation, loop, carried);
        if (isBeingFollowed || m_loopsLeftAlone.contains(name))
            return std::nullopt;

        const Typed::Block& header = function.at(loop.header);
        const Typed::Value& test = function.at(header.terminator.conditionOperand());
        bool holdsForEveryChoiceBefore = holdsForEveryChoice();
        size_t numberOfValuesInTurnBefore = m_valuesInTurn.size();
        bool isLeft = false;
        bool isShortOfBudget = false;
        std::optional<double> distanceBefore;
        Vector<ValuesAtHeader, 4> arriving = valuesArrivingAtHeader(activation, function, loop, PartOfLoop::WayIn);
        std::optional<Split> splitBefore = m_split;
        while (mayFollowAnotherValue()) {
            m_valuesInTurn.shrink(numberOfValuesInTurnBefore);
            for (uint32_t index = 0; index < arriving.size(); ++index)
                m_valuesInTurn.append(ValueInTurn { activation.call, { activation.function, header.parameters[index] }, arriving[index].values, arriving[index].sawEverything });

            Typed::KnownBoolean answer = answerOf(activation, function, header.terminator.conditionOperand());
            if (answer == Typed::KnownBoolean::Unknown)
                break;
            if ((answer == Typed::KnownBoolean::True) == !!loop.targetThatStays) {
                isLeft = atHeader(false);
                break;
            }

            // Two turns say how far a turn gets and what it costs. A loop that the walk cannot go round to its end is left alone.
            std::optional<double> distance = distanceTested(activation, test);
            if (!distance || (distanceBefore && *distance >= *distanceBefore))
                break;
            unsigned valuesFollowedBefore = m_valuesFollowed;
            if (!atHeader(true))
                break;
            arriving = valuesArrivingAtHeader(activation, function, loop, PartOfLoop::Turn);
            isShortOfBudget = distanceBefore && *distance / (*distanceBefore - *distance) * (m_valuesFollowed - valuesFollowedBefore) > m_budget - std::min(m_budget, m_valuesFollowed);
            if (isShortOfBudget)
                break;
            distanceBefore = distance;
        }
        m_valuesInTurn.shrink(numberOfValuesInTurnBefore);
        // The choice of an object in one turn is no choice for the others.
        m_split = WTF::move(splitBefore);
        if (isLeft) {
            m_sawEverything &= loop.isLeftAtHeaderAlone;
            return arriving;
        }
        // Another walk may come here with more left to spend.
        if (holdsForEveryChoiceBefore && !isShortOfBudget && m_valuesFollowed <= m_budget)
            m_loopsLeftAlone.insert(name);
        return std::nullopt;
    }

    // What a parameter of the header holds where the loop has been left.
    std::optional<Constants> valuesBehindLoop(const Activation& activation, const Typed::Function& function, const Loop& loop, uint32_t asked)
    {
        std::optional<Vector<ValuesAtHeader, 4>> arriving = followLoop(activation, function, loop, asked, [&](bool) {
            return true;
        });
        if (!arriving)
            return std::nullopt;
        if (arriving->at(asked).values.isEmpty()) {
            if (holdsForEveryChoice())
                m_loopsLeftAlone.insert(nameOf(activation, loop, asked));
            return std::nullopt;
        }
        m_sawEverything &= arriving->at(asked).sawEverything;
        return WTF::move(arriving->at(asked).values);
    }

    // What a builder that was begun in front of the loop holds where the loop has been left: each turn appends to what the turn before has left.
    std::optional<Constants> textsBehindLoop(const Activation& activation, const Typed::Function& function, ValueID builder, const Loop& loop)
    {
        std::optional<BlockParameter> merge = findMerge(activation.function, function, builder);
        if (merge && loop.turn.get(merge->block.index()))
            return std::nullopt;
        bool isUsedInLoop = false;
        for (size_t block : loop.turn) {
            isUsedInLoop |= std::ranges::any_of(function.at(Typed::BlockID(block)).values, [&](ValueID id) {
                return Typed::OperandPosition::builder(function.at(id).opcode) && function.at(id).builderOperand() == builder;
            });
        }
        if (!isUsedInLoop)
            return std::nullopt;
        const Typed::CFG& cfg = cfgOf(activation.function);
        auto textsArrivingAlong = [&](PartOfLoop part, TextsWhereBlocksEnd& remembered) {
            m_loopsFollowed.append(LoopFollowed { activation.call, activation.function, &loop, part });
            Constants texts;
            for (Typed::Edge edge : cfg.incomingEdges(loop.header)) {
                if (!cfg.isReachable(edge.from) || isOffTheWayFollowed(activation, loop.header, edge))
                    continue;
                if (!addAll(texts, textsWhereEdgeBegins(activation, function, builder, edge, loop.header, remembered)))
                    break;
            }
            m_loopsFollowed.removeLast();
            return texts;
        };
        TextsWhereBlocksEnd rememberedOnWayIn;
        Constants texts = textsArrivingAlong(PartOfLoop::WayIn, rememberedOnWayIn);
        constexpr uint32_t firstNameOfBuilder = 1u << 31;
        std::optional<Vector<ValuesAtHeader, 4>> arriving = followLoop(activation, function, loop, firstNameOfBuilder | builder.index(), [&](bool goesOn) {
            texts = textsAppendedIn(activation, function, builder, loop.header, ValueID(), { WTF::move(texts) });
            if (goesOn && !texts.isEmpty()) {
                TextsWhereBlocksEnd rememberedInTurn;
                rememberedInTurn.emplace(std::pair { builder, loop.header }, WTF::move(texts));
                texts = textsArrivingAlong(PartOfLoop::Turn, rememberedInTurn);
            }
            return !texts.isEmpty();
        });
        if (!arriving)
            return std::nullopt;
        return texts;
    }

    Constants valuesArrivingAt(const Activation& activation, const Typed::Function& function, ValueID parameter)
    {
        std::optional<BlockParameter> location = findBlockParameter(activation.function, function, parameter);
        if (!location)
            return { };
        if (location->block == Typed::entryBlock)
            return argumentsFor(activation, function, location->index);
        if (const Loop* loop = loopTestedAt(activation.function, function, location->block); loop && !isReadInTurn(activation, function, *loop)) {
            if (std::optional<Constants> values = valuesBehindLoop(activation, function, *loop, location->index))
                return WTF::move(*values);
        }
        const Typed::CFG& cfg = cfgOf(activation.function);
        Constants values;
        for (Typed::Edge edge : cfg.incomingEdges(location->block)) {
            if (isOffTheWayFollowed(activation, location->block, edge) || isRuledOut(activation, function, cfg, edge))
                continue;
            Typed::IncomingValue incoming = incomingValueOf(function, edge, location->index);
            if (incoming.kind == Typed::IncomingValue::Kind::Argument) {
                if (!addAll(values, evaluate(activation, incoming.argument)))
                    break;
                continue;
            }
            ValueID narrowed = operandNarrowedOnEdge(function, incoming.result);
            Constants arriving = narrowed ? evaluate(activation, narrowed) : Constants();
            if (arriving.isEmpty()) {
                m_sawEverything = false;
                continue;
            }
            const Typed::Type& type = m_program.typeOf(function, parameter);
            arriving.removeAllMatching([&](const Constant& constant) {
                return hasType(constant, type) == Typed::KnownBoolean::False;
            });
            if (!arriving.isEmpty() && !addAll(values, arriving))
                break;
        }
        return values;
    }

    Typed::KnownBoolean hasType(const Constant& constant, const Typed::Type& type) const
    {
        if (constant.regExp || constant.isObjectOrArray())
            return type.withOnlyObjects().isNever() ? Typed::KnownBoolean::False : Typed::KnownBoolean::Unknown;
        if (constant.unnamed)
            return evaluateTypeTest(m_program, m_program.typeOf(m_program.at(constant.function), constant.unnamed), type);
        return constant.primitive.isKnown() && !constant.primitive.isObject() ? evaluateTypeTest(m_program, constant.primitive, type) : Typed::KnownBoolean::Unknown;
    }

    Typed::KnownBoolean answerOf(const Activation& activation, const Typed::Function& function, ValueID test)
    {
        const Typed::Value& value = function.at(test);
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        auto restore = makeScopeExit([&] {
            m_sawEverything = sawEverythingBefore;
        });
        switch (value.opcode) {
        case Typed::Opcode::TypeTest: {
            Constants constants = evaluate(activation, value.valueOperand());
            Typed::KnownBoolean common = Typed::KnownBoolean::Unknown;
            if (!m_sawEverything)
                return common;
            for (const Constant& constant : constants) {
                Typed::KnownBoolean one = hasType(constant, m_program.at(value.testedType()));
                if (one == Typed::KnownBoolean::Unknown || (common != Typed::KnownBoolean::Unknown && one != common))
                    return Typed::KnownBoolean::Unknown;
                common = one;
            }
            return common;
        }
        case Typed::Opcode::Less:
        case Typed::Opcode::LessEq:
        case Typed::Opcode::Greater:
        case Typed::Opcode::GreaterEq: {
            Constants left = evaluate(activation, value.leftOperand());
            Constants right = evaluate(activation, value.rightOperand());
            if (!m_sawEverything || left.size() != 1 || right.size() != 1)
                return Typed::KnownBoolean::Unknown;
            Typed::KnownValue answer = Typed::evaluateOperation(m_program, function, value, { left[0].primitive, right[0].primitive });
            return answer.isKnown() ? Typed::knownBoolean(answer.asBoolean()) : Typed::KnownBoolean::Unknown;
        }
        default:
            return Typed::KnownBoolean::Unknown;
        }
    }

    bool isRuledOut(const Activation& activation, const Typed::Function& function, const Typed::CFG& cfg, Typed::Edge edge)
    {
        if (function.at(edge.from).terminator.kind == Typed::TerminatorKind::Jump) {
            std::span<const Typed::Edge> ways = cfg.incomingEdges(edge.from);
            if (ways.size() != 1)
                return false;
            edge = ways[0];
        }
        const Typed::Terminator& terminator = function.at(edge.from).terminator;
        if ((terminator.kind != Typed::TerminatorKind::TestAndBranch && terminator.kind != Typed::TerminatorKind::Branch) || edge.isHandlerEdge())
            return false;
        // The operands of a test are walked with a small part of the walk's budget, and few tests deep: a test in a loop asks for the loop's own values.
        if (m_testsBeingAnswered == maxTestsBeingAnswered)
            return false;
        SetForScope testsBeingAnswered(m_testsBeingAnswered, m_testsBeingAnswered + 1);
        unsigned valuesFollowedBefore = m_valuesFollowed;
        unsigned walksCutShortBefore = m_walksCutShort;
        SetForScope budget(m_budget, std::min(m_budget, m_valuesFollowed + maxValuesFollowedForTest));
        Typed::KnownBoolean answer = answerOf(activation, function, terminator.conditionOperand());
        if (m_valuesFollowed > m_budget) {
            m_valuesFollowed = valuesFollowedBefore + maxValuesFollowedForTest;
            m_walksCutShort = walksCutShortBefore;
            return false;
        }
        return answer != Typed::KnownBoolean::Unknown && (answer == Typed::KnownBoolean::True) != !edge.targetIndex;
    }

    Constants argumentsFor(const Activation& activation, const Typed::Function& function, uint32_t index)
    {
        size_t numberOfParameters = function.blocks.first().parameters.size();
        if (activation.call) {
            std::span<const ValueID> arguments = activation.call->argumentsOperands();
            if (numberOfParameters != arguments.size())
                return { };
            return evaluate(*activation.caller, arguments[index]);
        }
        // Other operations may enter the function too.
        m_sawEverything = false;
        Constants values;
        for (const DirectCall& call : std::ranges::equal_range(m_directCalls, activation.function, { }, &DirectCall::callee)) {
            // Nothing can be followed any more, and a function may have thousands of callers.
            if (m_valuesFollowed >= m_budget) {
                ++m_valuesFollowed;
                ++m_walksCutShort;
                break;
            }
            std::span<const ValueID> arguments = m_program.at(call.caller).at(call.call).argumentsOperands();
            if (numberOfParameters == arguments.size() && !addAll(values, evaluate(Activation { call.caller }, arguments[index])))
                return { };
        }
        if (function.entryParameterKind(index) == Typed::EntryParameter::Parameter && !addArgumentsGivenToClosures(values, activation.function, index - function.firstParameterIndex()))
            return { };
        return values;
    }

    static std::optional<ValueID> initialValueOf(const Typed::Value& newObject, Typed::LayoutID layout, uint32_t field)
    {
        if (newObject.layout() != layout)
            return std::nullopt;
        std::span<const uint32_t> fields = newObject.fields();
        size_t index = std::ranges::find(fields, field) - fields.begin();
        if (index == fields.size())
            return std::nullopt;
        return newObject.valuesOperands()[index];
    }

    Constants fieldSeenThrough(const Activation& activation, ValueID environment, Typed::LayoutID layout, uint32_t field)
    {
        if (!mayFollowAnotherValue())
            return { };
        const Typed::Function& function = m_program.at(activation.function);
        const Typed::Value& value = function.at(environment);
        switch (value.opcode) {
        case Typed::Opcode::Upcast:
        case Typed::Opcode::Expect:
        case Typed::Opcode::AssumeType:
        case Typed::Opcode::CheckInitialized:
        case Typed::Opcode::RequireObjectCoercible:
        case Typed::Opcode::RequireObject:
        case Typed::Opcode::FinishObject:
            return fieldSeenThrough(activation, value.operand(0), layout, field);
        case Typed::Opcode::GetCapture:
            return fieldSeenThrough(activation, value.closureOperand(), layout, field);
        case Typed::Opcode::GetSlot:
        case Typed::Opcode::GetSlotOrUndefined:
        case Typed::Opcode::GetDeferredClosureSlot:
        case Typed::Opcode::GetDeferredClosureSlotOrUndefined:
        case Typed::Opcode::PropGet:
            return fieldSeenThrough(activation, value.objectOperand(), layout, field);
        case Typed::Opcode::Parameter: {
            const auto& parameters = function.blocks.first().parameters;
            size_t index = parameters.find(environment);
            if (index == notFound)
                return fieldSeenThroughMerge(activation, function, environment, layout, field);
            if (!activation.call || activation.call->argumentsOperands().size() != parameters.size())
                return { };
            return fieldSeenThrough(*activation.caller, activation.call->argumentsOperands()[index], layout, field);
        }
        case Typed::Opcode::NewObject:
            if (std::optional<ValueID> initial = initialValueOf(value, layout, field))
                return evaluate(activation, *initial);
            return { };
        case Typed::Opcode::CallDirect: {
            FunctionID callee = value.function();
            const Typed::Function& body = m_program.at(callee);
            if (!body.hasBody())
                return { };
            if (isActive(activation, callee)) {
                ++m_walksCutShort;
                return { };
            }
            bool mayRemember = m_bindings.isEmpty();
            MemoKey memoKey { callsLeadingTo(activation), &value, layout.index(), field };
            if (const Constants* remembered = recall(memoKey))
                return *remembered;
            unsigned walksCutShortBefore = m_walksCutShort;
            bool sawEverythingBefore = std::exchange(m_sawEverything, true);
            auto remember = makeScopeExit([&] {
                m_sawEverything &= sawEverythingBefore;
            });
            Activation inCallee { callee, &activation, &value };
            Constants values;
            bool leavesFieldAlone = true;
            for (const Typed::Value& store : body.values) {
                std::optional<ValueID> stored;
                if (store.opcode == Typed::Opcode::PutEnvironmentField && store.layout() == layout && store.field() == field)
                    stored = store.valueOperand();
                else if (store.opcode == Typed::Opcode::NewObject)
                    stored = initialValueOf(store, layout, field);
                leavesFieldAlone &= !stored;
                if (stored && !addAll(values, evaluate(inCallee, *stored)))
                    return { };
            }
            if (leavesFieldAlone) {
                for (uint32_t inner = 0; inner < body.values.size(); ++inner) {
                    if (body.values[inner].opcode != Typed::Opcode::CallDirect)
                        continue;
                    Constants leftByCall = fieldSeenThrough(inCallee, ValueID(inner), layout, field);
                    if (!leftByCall.isEmpty() && !addAll(values, leftByCall))
                        return { };
                }
            }
            if (leavesFieldAlone && values.isEmpty()) {
                for (ValueID operand : value.argumentsOperands()) {
                    Typed::Opcode opcode = function.at(throughCasts(function, operand)).opcode;
                    if (opcode != Typed::Opcode::GetCapture && opcode != Typed::Opcode::NewObject && opcode != Typed::Opcode::Parameter)
                        continue;
                    Constants handedOn = fieldSeenThrough(activation, operand, layout, field);
                    if (!handedOn.isEmpty() && !addAll(values, handedOn))
                        return { };
                }
            }
            if (mayRemember && m_walksCutShort == walksCutShortBefore && !m_split)
                m_remembered.add(memoKey, makeUniqueWithoutFastMallocCheck<Remembered>(Remembered { values, m_sawEverything, std::nullopt }));
            return values;
        }
        default:
            return { };
        }
    }

    Constants fieldSeenThroughMerge(const Activation& activation, const Typed::Function& function, ValueID parameter, Typed::LayoutID layout, uint32_t field)
    {
        std::tuple key { activation.call, activation.function, parameter };
        if (m_active.contains(key)) {
            ++m_walksCutShort;
            return { };
        }
        m_active.append(key);
        auto leave = makeScopeExit([&] {
            m_active.removeLast();
        });
        Constants values;
        std::optional<BlockParameter> merge = findMerge(activation.function, function, parameter);
        for (Typed::Edge edge : merge ? cfgOf(activation.function).incomingEdges(merge->block) : std::span<const Typed::Edge>()) {
            Typed::IncomingValue incoming = incomingValueOf(function, edge, merge->index);
            ValueID arriving = incoming.kind == Typed::IncomingValue::Kind::Argument ? incoming.argument : operandNarrowedOnEdge(function, incoming.result);
            if (arriving && !addAll(values, fieldSeenThrough(activation, arriving, layout, field)))
                break;
        }
        return values;
    }

    Constants capturesOf(const Activation& activation, const Typed::Value& read)
    {
        bool sawEverythingBefore = m_sawEverything;
        Constants closures = evaluate(activation, read.closureOperand());
        m_sawEverything = sawEverythingBefore;
        Constants values;
        bool knowsEveryClosure = !closures.isEmpty();
        for (const Constant& closure : closures) {
            const Typed::Value* literal = closure.literal ? &m_program.at(closure.function).at(closure.literal) : nullptr;
            if (!literal || literal->opcode != Typed::Opcode::NewClosure || read.index() >= literal->capturesOperands().size()) {
                knowsEveryClosure = false;
                break;
            }
            if (!addAll(values, evaluate(WhereMade(closure).activation, literal->capturesOperands()[read.index()])))
                return { };
        }
        if (knowsEveryClosure)
            return values;
        return capturesOfAnyClosure(read.function(), read.index());
    }

    Constants capturesOfAnyClosure(FunctionID function, uint32_t index)
    {
        Constants values;
        for (const DirectCall& closure : std::ranges::equal_range(m_closures, sourceOf(function), { }, &DirectCall::callee)) {
            std::span<const ValueID> captures = m_program.at(closure.caller).at(closure.call).capturesOperands();
            if (index >= captures.size()) {
                m_sawEverything = false;
                continue;
            }
            if (!addAll(values, evaluate(Activation { closure.caller }, captures[index])))
                return { };
        }
        return values;
    }

    FunctionID sourceOf(FunctionID function) const
    {
        while (FunctionID source = m_program.at(function).derivation.source)
            function = source;
        return function;
    }

    static bool callsValue(const Typed::Function& function, const Typed::Value& value, ValueID callee)
    {
        return (value.opcode == Typed::Opcode::Call || value.opcode == Typed::Opcode::CallResolved) && throughCasts(function, value.calleeOperand()) == callee;
    }

    Constants answersOfMethodOfText(const Activation& activation, const Typed::Function& function, const Typed::Value& call)
    {
        ValueID receiver;
        Typed::PropertyKeyID key;
        if (call.opcode == Typed::Opcode::CallMethod) {
            receiver = call.receiverOperand();
            key = call.key();
        } else if (const Typed::Value& read = function.at(throughCasts(function, call.calleeOperand())); read.opcode == Typed::Opcode::PropGet && call.thisValueOperand()) {
            if (throughCasts(function, read.objectOperand()) == throughCasts(function, call.thisValueOperand())) {
                receiver = call.thisValueOperand();
                key = read.key();
            }
        }
        if (!receiver || !key)
            return { };
        Constants receivers = evaluate(activation, receiver);
        bool areTexts = !receivers.isEmpty() && std::ranges::all_of(receivers, [](const Constant& receiver) {
            return receiver.primitive.kind() == Typed::KnownValue::Kind::String;
        });
        if (!areTexts)
            return { };
        const BuiltinTable& table = m_program.builtins();
        for (BuiltinID member : table.membersOf(Builtins::StringPrototype)) {
            if (m_program.keyOf(table.at(member).name) != key)
                continue;
            for (BuiltinOverloadID overload : table.overloadIDsOf(table.canonicalID(member))) {
                if (table.at(overload).mode == BuiltinCallMode::Call)
                    return answersOfBuiltin(activation, call, Typed::BuiltinCall { overload, Typed::BuiltinCallForm::Whole, 0, receiver, call.argumentsOperands() });
            }
        }
        return { };
    }

    void forEachBuiltinCall(const Typed::Value& value, NOESCAPE const Invocable<void(const Typed::BuiltinCall&)> auto& functor) const
    {
        if (Typed::builtinCallOpcodes.contains(value.opcode)) {
            functor(builtinCallOf(m_program, value));
            return;
        }
        if ((value.opcode != Typed::Opcode::Call && value.opcode != Typed::Opcode::CallMethod) || !value.callees())
            return;
        std::optional<Typed::ProvenValue<Typed::Callees>> callees = m_program.at(value.callees()).all();
        if (!callees || !callees->value().functions.empty() || callees->value().builtins.size() != 1)
            return;
        const BuiltinTable& table = m_program.builtins();
        ValueID receiver = value.opcode == Typed::Opcode::Call ? value.thisValueOperand() : value.receiverOperand();
        for (BuiltinOverloadID overload : table.overloadIDsOf(table.canonicalID(callees->value().builtins[0]))) {
            if (table.at(overload).mode == BuiltinCallMode::Call)
                functor(Typed::BuiltinCall { overload, Typed::BuiltinCallForm::Whole, 0, receiver, value.argumentsOperands() });
        }
    }

    bool addArgumentsGivenByBuiltin(Constants& values, const Activation& activation, const Typed::Function& function, const Typed::Value& use, ValueID closure, uint32_t parameter)
    {
        bool isWithinLimits = true;
        forEachBuiltinCall(use, [&](const Typed::BuiltinCall& call) {
            if (call.form != Typed::BuiltinCallForm::Whole)
                return;
            for (const BuiltinCallback& callback : m_program.builtins().at(call.overload).flow.callbacks) {
                ValueID callee = call.operandAt(callback.callee);
                if (!isWithinLimits || !callee || throughCasts(function, callee) != closure)
                    continue;
                if (parameter >= callback.arguments.size()) {
                    isWithinLimits = add(values, Constant { Typed::KnownValue::undefined() });
                    continue;
                }
                const BuiltinPlace& place = callback.arguments[parameter];
                ValueID operand = place.operand ? call.operandAt(*place.operand) : ValueID();
                switch (place.kind) {
                case BuiltinPlaceKind::Operand:
                    isWithinLimits = addAll(values, evaluate(activation, operand));
                    break;
                case BuiltinPlaceKind::Slot:
                case BuiltinPlaceKind::OwnValues:
                case BuiltinPlaceKind::IteratedValues:
                    isWithinLimits = addAll(values, elementsOf(evaluate(activation, operand), std::nullopt));
                    break;
                case BuiltinPlaceKind::None:
                case BuiltinPlaceKind::Result:
                case BuiltinPlaceKind::ResultSlot:
                case BuiltinPlaceKind::ResultField:
                case BuiltinPlaceKind::NewValue:
                case BuiltinPlaceKind::NativeMemory:
                case BuiltinPlaceKind::Prototype:
                case BuiltinPlaceKind::Member:
                case BuiltinPlaceKind::NativeCell:
                    m_sawEverything = false;
                    break;
                }
            }
        });
        return isWithinLimits;
    }

    struct UseOfClosure {
        ValueID closure;
        ValueID use;
        friend bool operator==(const UseOfClosure&, const UseOfClosure&) = default;
        friend auto operator<=>(const UseOfClosure&, const UseOfClosure&) = default;
    };

    // The body of a module makes thousands of closures in hundreds of thousands of values, and the walk asks for the uses of its closures again and again.
    const Vector<UseOfClosure>& usesOfClosuresIn(FunctionID maker)
    {
        std::unique_ptr<Vector<UseOfClosure>>& uses = m_usesOfClosures[maker.index()];
        if (uses)
            return *uses;
        uses = makeUniqueWithoutFastMallocCheck<Vector<UseOfClosure>>();
        const Typed::Function& function = m_program.at(maker);
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            ValueID id(index);
            const Typed::Value& use = function.at(id);
            auto note = [&](ValueID operand) {
                ValueID closure = throughCasts(function, operand);
                if (function.at(closure).opcode == Typed::Opcode::NewClosure)
                    uses->append({ closure, id });
            };
            if (use.opcode == Typed::Opcode::Call || use.opcode == Typed::Opcode::CallResolved)
                note(use.calleeOperand());
            else if (use.opcode == Typed::Opcode::CallDirect) {
                for (ValueID operand : use.argumentsOperands())
                    note(operand);
            }
            forEachBuiltinCall(use, [&](const Typed::BuiltinCall& call) {
                if (call.form != Typed::BuiltinCallForm::Whole)
                    return;
                for (const BuiltinCallback& callback : m_program.builtins().at(call.overload).flow.callbacks) {
                    if (ValueID callee = call.operandAt(callback.callee))
                        note(callee);
                }
            });
        }
        AOT::sort(*uses);
        uses->shrink(std::ranges::unique(*uses).begin() - uses->begin());
        return *uses;
    }

    bool addArgumentsGivenToClosures(Constants& values, FunctionID callee, uint32_t parameter)
    {
        for (const DirectCall& closure : std::ranges::equal_range(m_closures, sourceOf(callee), { }, &DirectCall::callee)) {
            const Typed::Function& maker = m_program.at(closure.caller);
            Activation inMaker { closure.caller };
            for (const UseOfClosure& useOfClosure : std::ranges::equal_range(usesOfClosuresIn(closure.caller), closure.call, { }, &UseOfClosure::closure)) {
                const Typed::Value& use = maker.at(useOfClosure.use);
                if (callsValue(maker, use, closure.call)) {
                    std::span<const ValueID> arguments = use.argumentsOperands();
                    if (!addAll(values, evaluate(inMaker, parameter < arguments.size() ? arguments[parameter] : ValueID())))
                        return false;
                    continue;
                }
                if (!addArgumentsGivenByBuiltin(values, inMaker, maker, use, closure.call, parameter))
                    return false;
                if (use.opcode != Typed::Opcode::CallDirect)
                    continue;
                const Typed::Function& receiver = m_program.at(use.function());
                std::span<const ValueID> operands = use.argumentsOperands();
                if (!receiver.hasBody() || receiver.blocks.first().parameters.size() != operands.size() || isActive(inMaker, use.function()))
                    continue;
                bool handsClosureOver = std::ranges::any_of(operands, [&](ValueID operand) {
                    return throughCasts(maker, operand) == closure.call;
                });
                if (!handsClosureOver)
                    continue;
                Activation inReceiver { use.function(), &inMaker, &use };
                for (const Typed::Value& call : receiver.values) {
                    if (call.opcode != Typed::Opcode::Call && call.opcode != Typed::Opcode::CallResolved)
                        continue;
                    bool sawEverythingBefore = m_sawEverything;
                    bool callsClosure = evaluate(inReceiver, call.calleeOperand()).containsIf([&](const Constant& callee) {
                        return callee.function == closure.caller && callee.literal == closure.call;
                    });
                    m_sawEverything = sawEverythingBefore;
                    if (!callsClosure)
                        continue;
                    std::span<const ValueID> arguments = call.argumentsOperands();
                    if (!addAll(values, evaluate(inReceiver, parameter < arguments.size() ? arguments[parameter] : ValueID())))
                        return false;
                }
            }
        }
        return true;
    }

    Constants resultsOf(const Activation& activation, const Typed::Value& call)
    {
        FunctionID callee = call.function();
        const Typed::Function& function = m_program.at(callee);
        if (!function.hasBody() || function.hasAny({ Typed::FunctionFlags::IsAsync, Typed::FunctionFlags::IsGenerator }) || isActive(activation, callee))
            return { };
        return returnsOf(Activation { callee, &activation, &call });
    }

    Constants returnsOf(const Activation& inCallee)
    {
        Constants values;
        for (const Typed::Block& block : m_program.at(inCallee.function).blocks) {
            ValueID result;
            switch (block.terminator.kind) {
            case Typed::TerminatorKind::Return:
                result = block.terminator.valueOperand();
                break;
            case Typed::TerminatorKind::ReturnCall:
                result = block.terminator.callOperand();
                break;
            case Typed::TerminatorKind::Jump:
            case Typed::TerminatorKind::Branch:
            case Typed::TerminatorKind::Switch:
            case Typed::TerminatorKind::SwitchString:
            case Typed::TerminatorKind::Throw:
            case Typed::TerminatorKind::Rethrow:
            case Typed::TerminatorKind::Unreachable:
            case Typed::TerminatorKind::SwitchOnShape:
            case Typed::TerminatorKind::TryCallInline:
            case Typed::TerminatorKind::TestAndBranch:
                continue;
            }
            if (result && !addAll(values, evaluate(inCallee, result)))
                break;
        }
        return values;
    }

    bool addAnswerForEachChoice(Constants& answers, std::span<const Constants> operands, NOESCAPE const Invocable<std::optional<Constant>(std::span<const Constant>)> auto& functor) const
    {
        uint64_t choices = 1;
        unsigned operandsWithAlternatives = 0;
        for (const Constants& alternatives : operands) {
            choices *= alternatives.size();
            operandsWithAlternatives += alternatives.size() > 1;
            if (!choices || choices > maxAlternatives())
                break;
        }
        // Alternatives of one operand are an enumeration: callers, stores, rows. Alternatives of two, multiplied, are mostly texts that nobody makes.
        bool isWithinLimits = choices && choices <= maxAlternatives() && operandsWithAlternatives <= 1;
        Vector<Constant, 8> choice(isWithinLimits ? operands.size() : 0);
        for (uint64_t number = 0; isWithinLimits && number < choices; ++number) {
            uint64_t rest = number;
            for (size_t index = 0; index < operands.size(); ++index) {
                choice[index] = operands[index][rest % operands[index].size()];
                rest /= operands[index].size();
            }
            std::optional<Constant> answer = functor(choice.span());
            isWithinLimits = !answer || add(answers, *answer);
        }
        if (!isWithinLimits)
            answers.clear();
        return isWithinLimits;
    }

    std::optional<Constant> textWithinLimit(StringBuilder& builder)
    {
        if (builder.hasOverflowed() || m_context.exceeds(LimitID::RegExpCandidateTextLength, builder.length() * sizeof(char16_t), m_subject))
            return std::nullopt;
        return Constant { Typed::KnownValue::string(m_strings.intern(builder.toString())) };
    }

    uint64_t bytesOf(const Constant& constant) const
    {
        if (constant.regExp)
            return m_program.string(constant.regExp->pattern).size();
        return constant.primitive.kind() == Typed::KnownValue::Kind::String ? m_program.string(constant.primitive.asString()).size() : 0;
    }

    StringID sourceOf(Typed::RegExpLiteralInType literal)
    {
        Vector<char16_t> buffer;
        const RegExpSummary& summary = m_context.regExpSummaries().summaryOf(literal.pattern, toYarrFlags(literal.flags()), m_program.strings.viewOf(literal.pattern, buffer));
        if (!summary.isValid())
            return { };
        return summary.sourceIsPattern ? literal.pattern : m_strings.intern(StringView(summary.source));
    }

    Constants answersOfBuiltin(const Activation& activation, const Typed::Value& value, const Typed::BuiltinCall& call)
    {
        const BuiltinTable& table = m_program.builtins();
        const BuiltinOverload& overload = table.at(call.overload);
        BuiltinID callee = table.canonicalID(overload.builtin);
        if (call.form != Typed::BuiltinCallForm::Whole && call.form != Typed::BuiltinCallForm::AnyReceiver)
            return { };
        if (overload.mode == BuiltinCallMode::Get && callee == Builtins::ArrayPrototypeLength) {
            Constants lengths;
            for (const Constant& receiver : evaluate(activation, call.receiver)) {
                size_t size = receiver.hasElements() ? sizeOf(receiver) : notFound;
                if (size == notFound)
                    m_sawEverything = false;
                else if (!add(lengths, Constant { Typed::KnownValue::number(static_cast<double>(size)) }))
                    return { };
            }
            return lengths;
        }
        if (overload.mode == BuiltinCallMode::Get && callee == Builtins::RegExpPrototypeSource) {
            Constants sources;
            for (const Constant& receiver : evaluate(activation, call.receiver)) {
                StringID source = receiver.regExp ? sourceOf(*receiver.regExp) : StringID();
                if (source && !add(sources, Constant { Typed::KnownValue::string(source) }))
                    return { };
            }
            return sources;
        }
        if (callee == Builtins::RegExp)
            return createdBy(activation, call);
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::ArrayPrototypeMap && !call.arguments.empty())
            return { literalIn(activation, ValueID(static_cast<uint32_t>(&value - m_program.at(activation.function).values.span().data()))) };
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::ObjectFreeze && call.arguments.size() == 1)
            return evaluate(activation, call.arguments[0]);
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::ArrayPrototypeJoin && call.arguments.size() <= 1) {
            Constants arrays = evaluate(activation, call.receiver);
            return joined(arrays, evaluate(activation, call.arguments.empty() ? ValueID() : call.arguments[0]));
        }
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::StringRaw && !call.arguments.empty())
            return rawTextsOf(activation, call.arguments);
        // split() makes a new list each time. The texts in it depend on the operands alone.
        bool dependsOnOperandsAlone = table.at(callee).flags.contains(BuiltinFlag::IsDeterministic) || callee == Builtins::NumberPrototypeToString || callee == Builtins::StringPrototypeSplit;
        if (overload.mode != BuiltinCallMode::Call || !dependsOnOperandsAlone || !call.receiver)
            return { };

        Vector<Constants, 4> operands;
        operands.append(evaluate(activation, call.receiver));
        for (ValueID argument : call.arguments)
            operands.append(evaluate(activation, argument));
        return answersOf(callee, WTF::move(operands));
    }

    Constants resultsAt(const Activation& activation, const Typed::Function& function, ValueID results, uint32_t index)
    {
        const Typed::Value& value = function.at(results);
        switch (value.opcode) {
        case Typed::Opcode::Results:
            return index < value.numberOfOperands() ? evaluate(activation, value.operand(index)) : Constants();
        case Typed::Opcode::StringRange:
            return boundsOf(activation, value, index);
        case Typed::Opcode::CallDirect: {
            FunctionID callee = value.function();
            const Typed::Function& body = m_program.at(callee);
            if (!body.hasBody() || isActive(activation, callee))
                return { };
            Activation inCallee { callee, &activation, &value };
            Constants values;
            for (const Typed::Block& block : body.blocks) {
                if (block.terminator.kind != Typed::TerminatorKind::Return || !block.terminator.valueOperand())
                    continue;
                Constants given = resultsAt(inCallee, body, block.terminator.valueOperand(), index);
                if (!given.isEmpty() && !addAll(values, given))
                    return { };
            }
            return values;
        }
        default:
            return { };
        }
    }

    Constants boundsOf(const Activation& activation, const Typed::Value& range, uint32_t index)
    {
        Vector<Constants, 3> operands { evaluate(activation, range.lengthOperand()), evaluate(activation, range.firstOperand()), evaluate(activation, range.secondOperand()) };
        Constants bounds;
        addAnswerForEachChoice(bounds, operands.span(), [&](std::span<const Constant> choice) -> std::optional<Constant> {
            bool hasSecond = choice[2].primitive.kind() != Typed::KnownValue::Kind::Undefined;
            if (!choice[0].primitive.isNumber() || !choice[1].primitive.isNumber() || (hasSecond && !choice[2].primitive.isNumber()))
                return std::nullopt;
            double length = choice[0].primitive.asNumber();
            auto integral = [](double number) {
                return std::isnan(number) ? 0 : std::trunc(number);
            };
            auto fromEitherEnd = [&](double number) {
                return number < 0 ? std::max(length + number, 0.0) : std::min(number, length);
            };
            double first = integral(choice[1].primitive.asNumber());
            double second = hasSecond ? integral(choice[2].primitive.asNumber()) : length;
            double begin = 0;
            double end = 0;
            switch (range.method()) {
            case Typed::StringRangeMethod::Slice:
                begin = fromEitherEnd(first);
                end = std::max(begin, hasSecond ? fromEitherEnd(second) : length);
                break;
            case Typed::StringRangeMethod::Substring:
                begin = std::clamp(first, 0.0, length);
                end = std::clamp(second, 0.0, length);
                if (begin > end)
                    std::swap(begin, end);
                break;
            case Typed::StringRangeMethod::Substr:
                begin = fromEitherEnd(first);
                end = hasSecond ? std::min(begin + std::max(second, 0.0), length) : length;
                break;
            case Typed::StringRangeMethod::SingleIndex:
            case Typed::StringRangeMethod::SingleRelativeIndex:
                return std::nullopt;
            }
            return Constant { Typed::KnownValue::number(index ? end : begin) };
        });
        return bounds;
    }

    Constants substringsOf(const Activation& activation, const Typed::Function& function, const Typed::Value& value)
    {
        const Typed::Value& start = function.at(value.startOperand());
        const Typed::Value& end = function.at(value.endOperand());
        bool cutsRange = start.opcode == Typed::Opcode::ResultAt && end.opcode == Typed::Opcode::ResultAt && start.resultsOperand() == end.resultsOperand() && function.at(start.resultsOperand()).opcode == Typed::Opcode::StringRange;
        if (!cutsRange)
            return answersOf(Builtins::StringPrototypeSubstring, { evaluate(activation, value.stringOperand()), evaluate(activation, value.startOperand()), evaluate(activation, value.endOperand()) });
        const Typed::Value& range = function.at(start.resultsOperand());
        BuiltinID method;
        switch (range.method()) {
        case Typed::StringRangeMethod::Slice:
            method = Builtins::StringPrototypeSlice;
            break;
        case Typed::StringRangeMethod::Substring:
            method = Builtins::StringPrototypeSubstring;
            break;
        case Typed::StringRangeMethod::Substr:
            method = Builtins::StringPrototypeSubstr;
            break;
        case Typed::StringRangeMethod::SingleIndex:
        case Typed::StringRangeMethod::SingleRelativeIndex:
            return { };
        }
        return answersOf(method, { evaluate(activation, value.stringOperand()), evaluate(activation, range.firstOperand()), evaluate(activation, range.secondOperand()) });
    }

    Constants createdBy(const Activation& activation, const Typed::BuiltinCall& call)
    {
        if (call.arguments.empty() || call.arguments.size() > 2)
            return { };
        Vector<Constants, 2> operands { evaluate(activation, call.arguments[0]), evaluate(activation, call.arguments.size() > 1 ? call.arguments[1] : ValueID()) };
        Constants created;
        Vector<char16_t> buffer;
        addAnswerForEachChoice(created, operands.span(), [&](std::span<const Constant> choice) -> std::optional<Constant> {
            StringID text = choice[0].regExp ? choice[0].regExp->pattern : choice[0].primitive.kind() == Typed::KnownValue::Kind::String ? choice[0].primitive.asString() : StringID();
            if (!text)
                return std::nullopt;
            OptionSet<Typed::RegExpFlags> flags = choice[0].regExp ? choice[0].regExp->flags() : OptionSet<Typed::RegExpFlags> { };
            if (choice[1].primitive.kind() == Typed::KnownValue::Kind::String) {
                std::optional<OptionSet<Yarr::Flags>> parsed = Yarr::parseFlags(m_program.strings.viewOf(choice[1].primitive.asString(), buffer));
                if (!parsed)
                    return std::nullopt;
                flags = { };
                for (Typed::RegExpFlags flag : allMembersOf<Typed::RegExpFlags>) {
                    if (parsed->contains(toYarrFlag(flag)))
                        flags.add(flag);
                }
            } else if (choice[1].primitive.kind() != Typed::KnownValue::Kind::Undefined)
                return std::nullopt;
            return Constant { .regExp = Typed::RegExpLiteralInType { text, flags.toRaw() } };
        });
        return created;
    }

    // The texts together are no longer than the operands they were cut from, which are within the limit.
    Constant listOf(std::span<const String> texts)
    {
        Vector<StringID> list(texts.size(), [&](size_t index) {
            return m_strings.intern(StringView(texts[index]));
        });
        auto [known, isNew] = m_placesOfListsOfTexts.try_emplace(std::vector<StringID>(list.begin(), list.end()), static_cast<uint32_t>(m_listsOfTexts.size() + 1));
        if (isNew)
            m_listsOfTexts.append(WTF::move(list));
        return Constant { .listOfTexts = known->second };
    }

    Constants answersOf(BuiltinID callee, Vector<Constants, 4>&& operands)
    {
        Constants answers;
        addAnswerForEachChoice(answers, operands.span(), [&](std::span<const Constant> choice) -> std::optional<Constant> {
            if (choice[0].primitive.kind() != Typed::KnownValue::Kind::String && !choice[0].primitive.isNumber())
                return std::nullopt;
            uint64_t bytes = 0;
            double largestNumber = 1;
            Vector<Typed::ScratchRealm::Operand, 4> arguments;
            for (const Constant& operand : choice) {
                if (operand.isObjectOrArray() || (!operand.regExp && !operand.primitive.isKnown()))
                    return std::nullopt;
                arguments.append(Typed::ScratchRealm::Operand { operand.primitive, operand.regExp });
                bytes += bytesOf(operand);
                if (operand.primitive.isNumber())
                    largestNumber = std::max(largestNumber, operand.primitive.asNumber());
            }
            // A number among the operands may set the length of the answer: a count, a length. The limit is asked before the answer is made.
            double bytesOfAnswer = static_cast<double>(bytes) * largestNumber;
            if (bytesOfAnswer >= static_cast<double>(std::numeric_limits<uint32_t>::max()) || m_context.exceeds(LimitID::RegExpCandidateTextLength, static_cast<uint64_t>(bytesOfAnswer), m_subject))
                return std::nullopt;
            if (!m_scratchRealm)
                m_scratchRealm.emplace();
            Vector<String> listOfTexts;
            std::optional<Typed::ScratchRealm::Answer> answer = m_scratchRealm->call(m_program, callee, choice[0].primitive, arguments.span().subspan(1), &listOfTexts);
            if (!answer)
                return std::nullopt;
            if (answer->text.isNull() && !answer->primitive.isKnown())
                return listOf(listOfTexts.span());
            if (answer->text.isNull())
                return Constant { answer->primitive };
            if (m_context.exceeds(LimitID::RegExpCandidateTextLength, answer->text.sizeInBytes(), m_subject))
                return std::nullopt;
            return Constant { Typed::KnownValue::string(m_strings.intern(StringView(answer->text))) };
        });
        return answers;
    }

    Constants sumsOrDifferencesOf(const Activation&, const Typed::Function& function, const Typed::Value& value, std::array<Constants, 2>&& operands)
    {
        Constants numbers;
        addAnswerForEachChoice(numbers, operands, [&](std::span<const Constant> choice) -> std::optional<Constant> {
            if (!choice[0].primitive.isNumber() || !choice[1].primitive.isNumber())
                return std::nullopt;
            Typed::KnownValue number = Typed::evaluateOperation(m_program, function, value, { choice[0].primitive, choice[1].primitive });
            return number.isNumber() ? std::optional(Constant { number }) : std::nullopt;
        });
        return numbers;
    }

    Constants concatenate(const Activation& activation, const Typed::Value& value)
    {
        Vector<Constants, 8> operands;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index)
            operands.append(evaluate(activation, value.operand(index)));
        auto holdsNumbers = [](const Constants& constants) {
            return !constants.isEmpty() && std::ranges::all_of(constants, [](const Constant& constant) { return constant.primitive.isNumber(); });
        };
        if (value.opcode == Typed::Opcode::Add && operands.size() == 2 && holdsNumbers(operands[0]) && holdsNumbers(operands[1]))
            return sumsOrDifferencesOf(activation, m_program.at(activation.function), value, { WTF::move(operands[0]), WTF::move(operands[1]) });

        Constants texts;
        addAnswerForEachChoice(texts, operands.span(), [&](std::span<const Constant> choice) -> std::optional<Constant> {
            uint64_t bytes = 0;
            for (const Constant& operand : choice) {
                if (operand.primitive.kind() != Typed::KnownValue::Kind::String && !operand.primitive.isNumber())
                    return std::nullopt;
                bytes += bytesOf(operand);
            }
            if (m_context.exceeds(LimitID::RegExpCandidateTextLength, bytes, m_subject))
                return std::nullopt;
            uint32_t next = 0;
            Typed::KnownValue text = evaluateStringOperation(m_program, m_strings, m_program.at(activation.function), value, [&](ValueID) {
                return choice[next++].primitive;
            });
            return text.kind() == Typed::KnownValue::Kind::String ? std::optional(Constant { text }) : std::nullopt;
        });
        return texts;
    }

    const Typed::Program& m_program;
    Typed::StringArena& m_strings;
    BuildContext& m_context;
    // The walk summarizes a RegExp that it has put together itself, at whatever depth it stands.
    WTF::StackCheck m_stackCheck { Thread::currentSingleton().stack(), regExpCompilerStackSize + 16 * KB };
    Subject m_subject;
    std::map<std::pair<uint32_t, const Typed::Value*>, uint32_t> m_chainsOfCalls;
    struct Remembered {
        Constants values;
        bool sawEverything;
        std::optional<Split> split;
    };

    unsigned m_valuesFollowed { 0 };
    unsigned m_walksCutShort { 0 };
    unsigned m_worlds { 0 };
    HashMap<MemoKey, std::unique_ptr<Remembered>> m_remembered;
    bool m_sawEverything { true };
    std::optional<Split> m_split;
    Vector<std::pair<ValueKey, Constant>, 4> m_bindings;
    Vector<LoopFollowed, 2> m_loopsFollowed;
    Vector<ValueInTurn, 8> m_valuesInTurn;
    HashMap<uint64_t, std::unique_ptr<Loop>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_loops;
    std::set<NameOfCarried> m_loopsLeftAlone;
    Vector<Vector<StringID>> m_listsOfTexts;
    std::map<std::vector<StringID>, uint32_t> m_placesOfListsOfTexts;
    Vector<Store> m_stores;
    Vector<DirectCall> m_directCalls;
    Vector<DirectCall> m_closures;
    Vector<std::unique_ptr<Vector<UseOfClosure>>> m_usesOfClosures;
    Vector<FunctionID, 2> m_functionsOfMap;
    HashMap<uint64_t, Vector<ValueID>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_longLiterals;
    std::optional<Typed::ScratchRealm> m_scratchRealm;
    Vector<std::tuple<const Typed::Value*, FunctionID, ValueID>, 32> m_active;
    UncheckedKeyHashMap<uint32_t, ValuesOfVariable, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
    HashMap<uint32_t, std::unique_ptr<Typed::CFG>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cfgs;
    HashMap<uint32_t, Vector<BlockParameter>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blockParameters;
    HashMap<uint32_t, Vector<uint64_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_places;
    bool m_hasFoundWritesAndHoldings { false };
    Vector<WritingOperation> m_writingOperations;
    Vector<Holding> m_holdings;
    std::map<std::tuple<uint32_t, uint32_t, const Typed::Value*>, std::unique_ptr<Contents>> m_contents;
    Contents m_contentsBeingFound;
};

} // anonymous namespace

static auto keyOf(const RegExpRequest& request)
{
    return std::pair { request.pattern.index(), request.flags.toRaw() };
}

static bool isAscendingList(std::span<const RegExpRequest> requests, bool holdsLiterals)
{
    bool hasOrigin = std::ranges::all_of(requests, [&](const RegExpRequest& request) {
        return (request.origin == RegExpRequestOrigin::Literal) == holdsLiterals;
    });
    return hasOrigin && std::ranges::adjacent_find(requests, std::ranges::greater_equal { }, keyOf) == requests.end();
}

static std::optional<size_t> findRequest(std::span<const RegExpRequest> ascendingRequests, StringID pattern, OptionSet<Yarr::Flags> significantFlags)
{
    std::pair key { pattern.index(), significantFlags.toRaw() };
    auto found = std::ranges::lower_bound(ascendingRequests, key, { }, keyOf);
    if (found == ascendingRequests.end() || keyOf(*found) != key)
        return std::nullopt;
    return static_cast<size_t>(found - ascendingRequests.begin());
}

RegExpRequests::RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& candidates)
    : m_requests(WTF::move(literals))
    , m_numberOfLiterals(static_cast<uint32_t>(m_requests.size()))
{
    RELEASE_ASSERT(isAscendingList(m_requests.span(), true));
    RELEASE_ASSERT(isAscendingList(candidates.span(), false));
    m_requests.appendVector(WTF::move(candidates));
}

RegExpRecordID RegExpRequests::recordOf(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    std::optional<size_t> index = findRequest(literals(), pattern, ImageRegExp::significantFlags(flags));
    RELEASE_ASSERT(index);
    return RegExpRecordID(*index);
}

static bool mayPassArgument(const Typed::BuiltinCall& call, uint32_t argumentIndex)
{
    switch (call.form) {
    case Typed::BuiltinCallForm::List:
        return true;
    case Typed::BuiltinCallForm::Whole:
    case Typed::BuiltinCallForm::Fields:
    case Typed::BuiltinCallForm::Prefix:
    case Typed::BuiltinCallForm::AnyReceiver:
        return argumentIndex < call.arguments.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlagSets parseFlagSets(const Typed::Program& program, const Typed::SinkCandidates& candidates)
{
    FlagSets flagSets;
    // Unknown flags may be "", "g" or "d", which all find the record without significant flags.
    if (candidates.mayBeUndefined || !candidates.isComplete)
        flagSets.append(OptionSet<Yarr::Flags> { });
    Vector<char16_t> buffer;
    for (StringID text : candidates.strings) {
        if (std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(program.strings.viewOf(text, buffer)))
            flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
    }
    return flagSets;
}

static void reportTextKnownAtRunTime(BuildContext& context, RegExpSite site, Typed::BlockID block)
{
    context.diagnostics().add(UserDiagnostic {
        .kind = UserDiagnosticKind::RegExpWithoutCode,
        .function = site.function,
        .block = block,
        .value = site.value,
        .count = static_cast<uint32_t>(RegExpDeclineReason::TextKnownAtRunTime),
        .location = site.location,
    });
}

static void addCandidates(const Typed::Program& program, BuildContext& context, RegExpRequestOrigin origin, StringID pattern, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(pattern, buffer);
    for (OptionSet<Yarr::Flags> flagSet : flagSets) {
        const RegExpSummary& summary = context.regExpSummaries().summaryOf(pattern, flagSet, text);
        if (summary.isValid())
            found.append(FoundRegExp { origin, pattern, flagSet, site });
    }
}

static void collectCandidates(CollectionInputs& inputs, const Typed::BuiltinCall& call, Typed::AccessSiteID accessSite, RegExpSite site, Typed::BlockID block, Vector<FoundRegExp>& found)
{
    const Typed::SinkCandidates notPassed { .strings = { }, .mayBeUndefined = true, .isComplete = true };
    auto candidatesOf = [&](uint32_t argumentIndex) -> const Typed::SinkCandidates& {
        return mayPassArgument(call, argumentIndex) ? inputs.flow.sinkCandidates(accessSite, argumentIndex) : notPassed;
    };
    const Typed::SinkCandidates& patterns = candidatesOf(0);
    const Typed::SinkCandidates& flags = candidatesOf(1);
    FlagSets flagSets = parseFlagSets(inputs.program, flags);

    bool takesTextsOfFlow = !inputs.context.exceeds(LimitID::RegExpCandidatesPerSite, patterns.strings.size(), Subject { .function = site.function, .block = block, .value = site.value, .location = site.location });
    if (takesTextsOfFlow) {
        for (StringID pattern : patterns.strings)
            addCandidates(inputs.program, inputs.context, RegExpRequestOrigin::Candidate, pattern, flagSets, site, found);
        // The record of an operand that flow knows nothing about says mayBeUndefined too.
        StringID emptyString = inputs.program.strings.emptyString();
        if (patterns.mayBeUndefined && patterns != Typed::SinkCandidates { } && !patterns.strings.contains(emptyString))
            addCandidates(inputs.program, inputs.context, RegExpRequestOrigin::Candidate, emptyString, flagSets, site, found);
    }
    bool flowKnowsEveryText = takesTextsOfFlow && patterns.isComplete;
    if (flowKnowsEveryText && flags.isComplete)
        return;

    bool passesPatternAsOperand = call.form != Typed::BuiltinCallForm::List && !call.arguments.empty();
    if (flowKnowsEveryText || !passesPatternAsOperand) {
        reportTextKnownAtRunTime(inputs.context, site, block);
        return;
    }
    Locker locker { inputs.lock };
    inputs.callsWithUnknownText.append(CallWithUnknownText { site, block, call.arguments[0], call.arguments.size() > 1 ? call.arguments[1] : ValueID(), WTF::move(flagSets), flags.isComplete });
}

static void addCandidatesFromConstants(const Typed::Program& program, Typed::StringArena& strings, BuildContext& context, Vector<CallWithUnknownText>&& calls, Vector<FoundRegExp>& found)
{
    if (calls.isEmpty())
        return;
    AOT::sort(calls, { }, &CallWithUnknownText::site);
    TextsFromConstants constants(program, strings, context);
    for (const CallWithUnknownText& call : calls) {
        TextsFromConstants::Patterns known = constants.patternsOf(call);
        RegExpRequestOrigin origin = known.sawEveryText || known.patterns.size() == 1 ? RegExpRequestOrigin::Evaluated : RegExpRequestOrigin::Candidate;
        for (const TextsFromConstants::Pattern& pattern : known.patterns)
            addCandidates(program, context, origin, pattern.text, FlagSets { pattern.flags }, call.site, found);
        if (!known.sawEveryText || known.patterns.isEmpty() || !known.sawAllFlags)
            reportTextKnownAtRunTime(context, call.site, call.block);
    }
}

static void collectFromFunction(CollectionInputs& inputs, FunctionID id, Vector<FoundRegExp>& found)
{
    const Typed::Program& program = inputs.program;
    const Typed::Function& function = program.at(id);
    const BuiltinTable& builtins = program.builtins();
    std::optional<Typed::CFG> cfg;
    // Placement asks for the record of a literal in every block, reachable or not.
    forEachListedValue(program, id, Typed::Value::opcodesWithPattern | Typed::builtinCallOpcodes, [&](Typed::BlockID block, uint32_t, ValueID valueID, const Typed::Value& value) {
        bool isLiteral = Typed::Value::opcodesWithPattern.contains(value.opcode);
        if (!isLiteral && builtins.canonicalID(builtins.at(value.overload()).builtin) != Builtins::RegExp)
            return;
        RegExpSite site { id, valueID, function.locationOf(valueID) };
        if (isLiteral) {
            found.append(FoundRegExp { RegExpRequestOrigin::Literal, value.pattern(), ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags())), site });
            return;
        }
        if (!cfg)
            cfg.emplace(function);
        if (cfg->isReachable(block))
            collectCandidates(inputs, builtinCallOf(program, value), value.accessSite(), site, block, found);
    });
}

static void addToRequests(Vector<RegExpRequest>& ascendingRequests, const FoundRegExp& found)
{
    if (ascendingRequests.isEmpty() || ascendingRequests.last().pattern != found.pattern || ascendingRequests.last().flags != found.flags)
        ascendingRequests.append(RegExpRequest { .pattern = found.pattern, .flags = found.flags, .origin = found.origin });
    Vector<RegExpSite, 1>& sites = ascendingRequests.last().sites;
    if (sites.isEmpty() || sites.last() != found.site)
        sites.append(found.site);
}

static void moveSitesOfRequestsAlsoIn(Vector<RegExpRequest>& strongerRequests, Vector<RegExpRequest>& requests)
{
    requests.removeAllMatching([&](const RegExpRequest& request) {
        std::optional<size_t> stronger = findRequest(strongerRequests.span(), request.pattern, request.flags);
        if (!stronger)
            return false;
        for (const RegExpSite& site : request.sites)
            strongerRequests[*stronger].sites.appendIfNotContains(site);
        return true;
    });
}

static void internMetadataStrings(RegExpRequest& request, const Typed::Program& program, Typed::StringArena& strings, BuildContext& context)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(request.pattern, buffer);
    const RegExpSummary& summary = context.regExpSummaries().summaryOf(request.pattern, request.flags, text);
    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        return;
    case RegExpMetadata::Known:
        break;
    }

    if (!summary.atom.isNull())
        request.atom = StringView(summary.atom) == text ? request.pattern : strings.intern(StringView(summary.atom));
    request.captureGroupNames = summary.captureGroupNames.map([&](const String& name) {
        return name.isNull() ? StringID() : strings.intern(StringView(name));
    });
    request.namedGroups = summary.namedGroups.map([&](const RegExpNamedGroupSummary& group) {
        return RegExpNamedGroupStrings { strings.intern(StringView(group.name)), group.parenIndices };
    });
}

RegExpRequests collectRegExpRequests(const Typed::Program& program, const Typed::FlowResults& flow, Typed::StringArena& strings, BuildContext& context)
{
    PhaseTimes times(context, PassID::CollectRegExpLiterals);
    CollectionInputs inputs { program, flow, context };
    Vector<FoundRegExp> found = collectInParallel<FoundRegExp>(context.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundRegExp>& foundInFunction) {
        collectFromFunction(inputs, FunctionID(index), foundInFunction);
    });
    times.didFinish("collect from functions"_s);
    addCandidatesFromConstants(program, strings, context, WTF::move(inputs.callsWithUnknownText), found);
    times.didFinish("addCandidatesFromConstants"_s);
    parallelSort(context.parallelLoops(), found.mutableSpan(), [](const FoundRegExp& a, const FoundRegExp& b) {
        return std::tuple { a.origin, a.pattern, a.flags.toRaw(), a.site } < std::tuple { b.origin, b.pattern, b.flags.toRaw(), b.site };
    });

    Vector<RegExpRequest> literals;
    Vector<RegExpRequest> evaluated;
    Vector<RegExpRequest> candidates;
    for (const FoundRegExp& entry : found) {
        switch (entry.origin) {
        case RegExpRequestOrigin::Literal:
            addToRequests(literals, entry);
            break;
        case RegExpRequestOrigin::Evaluated:
            addToRequests(evaluated, entry);
            break;
        case RegExpRequestOrigin::Candidate:
            addToRequests(candidates, entry);
            break;
        }
    }
    moveSitesOfRequestsAlsoIn(literals, evaluated);
    moveSitesOfRequestsAlsoIn(literals, candidates);
    moveSitesOfRequestsAlsoIn(evaluated, candidates);
    candidates.appendVector(WTF::move(evaluated));
    AOT::sort(candidates, { }, keyOf);

    for (RegExpRequest& request : literals)
        internMetadataStrings(request, program, strings, context);
    for (RegExpRequest& request : candidates)
        internMetadataStrings(request, program, strings, context);

    context.statistics().countChanges(PassID::CollectRegExpLiterals, literals.size() + candidates.size());
    times.didFinish("requests and their strings"_s);
    return RegExpRequests(WTF::move(literals), WTF::move(candidates));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
