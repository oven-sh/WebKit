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
#include "AOTIRPrinter.h"
#include "AOTKnownValue.h"
#include "AOTPhaseTimes.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpSummary.h"
#include "AOTScratchRealm.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Lock.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StackCheck.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using namespace Typed;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpRequests);

static_assert(Value::opcodesWithPattern == Value::opcodesWithRegExpFlags);
static_assert(Value::opcodesWithPattern == Value::opcodesWithRegExpRecord);

namespace {

struct CallsWithUnknownText;

struct CollectionInputs {
    const Program& program;
    const FlowResults& flow;
    BuildContext& context;
    CallsWithUnknownText& callsWithUnknownText;
};

struct FoundRegExp {
    RegExpRequestOrigin origin;
    StringID pattern;
    OptionSet<Yarr::Flags> flags;
    RegExpSite site;
};

struct RegExpConstructorCall {
    BuiltinCall builtinCall;
    AccessSiteID accessSite;
    RegExpSite site;
    BlockID block;
    SourceLocation location;
};

using FlagSets = Vector<OptionSet<Yarr::Flags>, 8>;

struct CallWithUnknownText {
    RegExpSite site;
    BlockID block;
    SourceLocation location;
    ValueID pattern;
    ValueID flags;
    FlagSets flagSets;
    bool flagsAreComplete;
};

struct CallsWithUnknownText {
    Lock lock;
    Vector<CallWithUnknownText> calls;
};

// The calls that lead to the function, as one number; no call and a function and a value of it, or a call and a layout and a field that is seen through it.
struct MemoKey {
    uint32_t callChain { UINT32_MAX };
    const Value* call { nullptr };
    uint32_t functionOrLayout { 0 };
    uint32_t valueOrField { 0 };

    friend bool operator==(const MemoKey&, const MemoKey&) = default;
};

struct MemoKeyHash {
    static unsigned hash(const MemoKey& key) { return computeHash(key.callChain, key.call, key.functionOrLayout, key.valueOrField); }
    static bool equal(const MemoKey& a, const MemoKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};

struct MemoKeyHashTraits : WTF::GenericHashTraits<MemoKey> {
    static void constructDeletedValue(MemoKey& slot) { slot = MemoKey { UINT32_MAX - 1, nullptr, 0, 0 }; }
    static bool isDeletedValue(const MemoKey& key) { return key.callChain == UINT32_MAX - 1; }
};

// The texts that an operand may have, read off the operations that make it: constants, variables with what the stores of the program give them, parameters with the arguments of the direct calls,
// results of direct calls, concatenations, functions of the engine that depend on their operands alone, and the fields and elements that object and array literals are made with.
// No branch is decided, no loop is followed and no code of the program runs. Nothing is proven: the run time finds a record by the text it is given.
class TextsFromConstants {
public:
    // A primitive, a RegExp literal, or an object or array literal: the operation that makes it, or a node of constant data.
    struct Constant {
        KnownValue primitive;
        std::optional<RegExpLiteralInType> regExp { };
        FunctionID function { };
        ValueID literal { };
        const DataNode* data { nullptr };
        // The call under which the function made the literal, where the walk came through one: what the literal is made of is read with that call's operands.
        FunctionID caller { };
        const Value* call { nullptr };
        // A function declaration, which no operation makes.
        FunctionID declaration { };

        bool isObjectOrArray() const { return literal || data || declaration; }
        friend bool operator==(const Constant&, const Constant&) = default;
    };
    using Constants = Vector<Constant, 1>;

    struct Pattern {
        StringID text;
        OptionSet<Yarr::Flags> flags;
        friend bool operator==(const Pattern&, const Pattern&) = default;
    };

    TextsFromConstants(const Program& program, StringArena& strings, BuildContext& context)
        : m_program(program)
        , m_strings(strings)
        , m_context(context)
    {
        uint32_t numberOfFunctions = program.functions.size();
        Vector<CollectedOperation> operations = collectInParallel<CollectedOperation>(context.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<CollectedOperation>& found) {
            forEachValueListedOrNot(program, FunctionID(index), { Opcode::PutModuleVariable, Opcode::CallDirect, Opcode::NewClosure }, [&](ValueID id, const Value& value) {
                if (value.opcode == Opcode::PutModuleVariable)
                    found.append(CollectedOperation { value.opcode, value.variable().index(), FunctionID(index), value.valueOperand() });
                else if (value.function().index() < numberOfFunctions)
                    found.append(CollectedOperation { value.opcode, value.function().index(), FunctionID(index), id });
            });
        });
        runTasksInParallel(context.parallelLoops(), {
            [&] { m_directCalls = groupByCallee(operations.span(), Opcode::CallDirect, numberOfFunctions); },
            [&] { m_closures = groupByCallee(operations.span(), Opcode::NewClosure, numberOfFunctions); },
            [&] {
                for (const CollectedOperation& operation : operations) {
                    if (operation.opcode == Opcode::PutModuleVariable)
                        m_stores.append(Store { VariableID(operation.targetIndex), operation.function, operation.value });
                }
                std::ranges::sort(m_stores, { }, [](const Store& store) {
                    return std::tuple { store.variable, store.function, store.stored };
                });
            },
        });
        for (const BuiltinOverload& overload : program.builtins().overloadsOf(Builtins::ArrayPrototypeMap)) {
            const RuntimeFunctionEntry* entry = overload.code.implementation.function ? findRuntimeFunction(program, overload.code.implementation.function) : nullptr;
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
        Patterns result;
        explore(call, result);
        return result;
    }

private:
    static constexpr unsigned maxValuesFollowed = 1 << 14;
    static constexpr unsigned maxValuesFollowedForTest = 64;
    unsigned m_budget { maxValuesFollowed };
    static constexpr unsigned maxTestsBeingAnswered = 1;
    unsigned m_testsBeingAnswered { 0 };

    struct Store {
        VariableID variable;
        FunctionID function;
        ValueID stored;
    };

    struct DirectCall {
        FunctionID callee;
        FunctionID caller;
        ValueID call;
    };

    struct CollectedOperation {
        Opcode opcode;
        uint32_t targetIndex;
        FunctionID function;
        ValueID value;
    };

    // A stable counting sort: the input is ordered by (function, value).
    static Vector<DirectCall> groupByCallee(std::span<const CollectedOperation> operations, Opcode opcode, uint32_t numberOfFunctions)
    {
        Vector<uint32_t> positions(FillWith { }, numberOfFunctions + 1, 0);
        for (const CollectedOperation& operation : operations) {
            if (operation.opcode == opcode)
                ++positions[operation.targetIndex + 1];
        }
        for (uint32_t index = 0; index < numberOfFunctions; ++index)
            positions[index + 1] += positions[index];
        Vector<DirectCall> result(positions.last());
        for (const CollectedOperation& operation : operations) {
            if (operation.opcode == opcode)
                result[positions[operation.targetIndex]++] = DirectCall { FunctionID(operation.targetIndex), operation.function, operation.value };
        }
        return result;
    }

    struct Activation {
        FunctionID function;
        const Activation* caller { nullptr };
        const Value* call { nullptr };
        mutable std::optional<uint32_t> cachedCallChain { };
    };

    using ValueKey = std::pair<FunctionID, ValueID>;

    struct Split {
        ValueKey value;
        Constants alternatives;
    };

    // A value that is one of several objects, as a row of a table is, is given each of them in turn, so that what is read of one object stays together: its text with its flags.
    void explore(const CallWithUnknownText& call, Patterns& result)
    {
        m_split.reset();
        m_sawEverything = true;
        Vector<StringID, 1> texts;
        for (const Constant& constant : evaluate(Activation { call.site.function }, call.pattern)) {
            if (constant.regExp)
                texts.appendIfNotContains(constant.regExp->pattern);
            else if (constant.primitive.kind() == KnownValue::Kind::String)
                texts.appendIfNotContains(constant.primitive.asString());
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
            if (constant.primitive.kind() == KnownValue::Kind::Undefined) {
                flagSets.appendIfNotContains(OptionSet<Yarr::Flags> { });
                continue;
            }
            if (constant.primitive.kind() != KnownValue::Kind::String)
                continue;
            if (std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(m_program.strings.viewOf(constant.primitive.asString(), buffer)))
                flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
        }
        return flagSets;
    }

    uint64_t maxAlternatives() const { return m_context.limit(LimitID::RegExpCandidatesPerSite); }

    // False once there are more alternatives than a call may have: a crowd says little about the text that the call gets.
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

    uint32_t callsLeadingTo(const Activation& activation)
    {
        if (!activation.call)
            return 0;
        if (!activation.cachedCallChain)
            activation.cachedCallChain = m_chainsOfCalls.try_emplace({ callsLeadingTo(*activation.caller), activation.call }, static_cast<uint32_t>(m_chainsOfCalls.size() + 1)).first->second;
        return *activation.cachedCallChain;
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
            return { Constant { KnownValue::undefined() } };
        if (!m_stackCheck.isSafeToRecurse() || ++m_valuesFollowed > m_budget) {
            ++m_walksCutShort;
            return { };
        }
        for (auto& [value, constant] : m_bindings) {
            if (value == ValueKey { activation.function, id })
                return { constant };
        }
        const Function& function = m_program.at(activation.function);
        if (KnownValue known = knownValueOf(m_program, function, id); known.isKnown() && !known.isObject())
            return { Constant { known } };

        // A value that is made of itself goes round a loop. The same value of a function that is called twice, one call inside the other's argument, does not.
        std::tuple key { activation.call, activation.function, id };
        if (m_active.contains(key)) {
            ++m_walksCutShort;
            return { };
        }

        // What a value is does not depend on who asks, as long as no object has been chosen for another value and the calls that lead here are the same.
        bool mayRemember = m_bindings.isEmpty();
        MemoKey memoKey { callsLeadingTo(activation), nullptr, activation.function.index(), id.index() };
        if (mayRemember) {
            if (auto remembered = m_remembered.find(memoKey); remembered != m_remembered.end()) {
                m_sawEverything &= remembered->value->sawEverything;
                if (remembered->value->split)
                    m_split = remembered->value->split;
                return remembered->value->values;
            }
        }

        unsigned walksCutShortBefore = m_walksCutShort;
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        std::optional<Split> splitBefore = std::exchange(m_split, std::nullopt);
        m_active.append(key);
        Constants result = evaluateOperation(activation, function, id);
        m_active.removeLast();
        bool isOneOfSeveralObjects = result.size() > 1 && std::ranges::all_of(result, &Constant::isObjectOrArray);
        // The last such value is the nearest to the pattern: what it is made of, several ways perhaps, needs no choice of its own then.
        if (isOneOfSeveralObjects)
            m_split = Split { { activation.function, id }, result };
        if (mayRemember && m_walksCutShort == walksCutShortBefore && m_valuesFollowed <= maxValuesFollowed)
            m_remembered.add(memoKey, makeUniqueWithoutFastMallocCheck<Remembered>(Remembered { result, m_sawEverything, m_split }));
        m_sawEverything &= sawEverythingBefore;
        if (!m_split)
            m_split = WTF::move(splitBefore);
        return result;
    }

    Constants evaluateOperation(const Activation& activation, const Function& function, ValueID id)
    {
        const Value& value = function.at(id);
        switch (value.opcode) {
        case Opcode::NewRegExp:
        case Opcode::SharedRegExpLiteral:
            return { Constant { .regExp = RegExpLiteralInType { value.pattern(), value.regExpFlags().toRaw() } } };
        case Opcode::NewObject:
        case Opcode::NewArray:
        case Opcode::NewArrayLike:
        case Opcode::NewContiguousArray:
        case Opcode::NewClosure:
            return { literalIn(activation, id) };
        case Opcode::TruncateNewArray:
            return evaluate(activation, value.arrayOperand());
        case Opcode::Data:
            return { constantOf(m_program.at(value.data()).root) };
        case Opcode::FinishObject:
            return evaluate(activation, value.objectOperand());
        case Opcode::GetSlot:
        case Opcode::GetSlotOrUndefined:
        case Opcode::PropGet:
            return fieldsOf(evaluate(activation, value.objectOperand()), value.key());
        case Opcode::GetArrayElement:
            return elementsOf(evaluate(activation, value.arrayOperand()), positionOf(activation, value.positionOperand()));
        case Opcode::ElemGet:
            return elementsOf(evaluate(activation, value.objectOperand()), positionOf(activation, value.propertyOperand()));
        case Opcode::GetStringLength:
            return lengthsOf(evaluate(activation, value.stringOperand()));
        case Opcode::GetCapture:
            return capturesOf(activation, value);
        case Opcode::GetEnvironmentField:
            return fieldSeenThrough(activation, value.environmentOperand(), value.layout(), value.field());
        case Opcode::StringBuilderFinish:
        case Opcode::StringBuilderToString:
            return textsBuiltBy(activation, function, id);
        case Opcode::GetModuleVariable:
            return valuesOf(value.variable());
        case Opcode::Upcast:
        case Opcode::AssumeType:
        case Opcode::CheckInitialized:
        case Opcode::FlattenString:
            return evaluate(activation, value.operand(0));
        case Opcode::RequireObjectCoercible:
        case Opcode::RequireObject:
        case Opcode::ToObject: {
            // Each throws where it is given undefined or null.
            Constants values = evaluate(activation, value.operand(0));
            values.removeAllMatching([](const Constant& constant) {
                return constant.primitive.isNullish();
            });
            return values;
        }
        case Opcode::Parameter:
            return valuesArrivingAt(activation, function, id);
        case Opcode::CallDirect:
            // The function of the library that `map` is.
            if (m_functionsOfMap.contains(sourceOf(value.function())) && value.argumentsOperands().size() >= 2)
                return { literalIn(activation, id) };
            return resultsOf(activation, value);
        case Opcode::CallBuiltin:
        case Opcode::CallInline:
            return answersOfBuiltin(activation, value, builtinCallOf(m_program, value));
        case Opcode::Call:
        case Opcode::CallMethod: {
            Constants answers;
            bool isBuiltin = false;
            forEachBuiltinCall(value, [&](const BuiltinCall& call) {
                if (!std::exchange(isBuiltin, true))
                    answers = answersOfBuiltin(activation, value, call);
            });
            return isBuiltin ? answers : answersOfMethodOfText(activation, function, value);
        }
        case Opcode::Substring:
            return substringsOf(activation, function, value);
        case Opcode::ResultAt:
            return resultsAt(activation, function, value.resultsOperand(), value.index());
        default:
            if (opcodesCreatingKnownStrings.contains(value.opcode))
                return concatenate(activation, value);
            return { };
        }
    }

    struct ValuesOfVariable {
        Constants values;
        bool sawEverything { false };
    };

    Constants valuesOf(VariableID variable)
    {
        if (auto known = m_variables.find(variable.index()); known != m_variables.end()) {
            m_sawEverything &= known->value.sawEverything;
            return known->value.values;
        }
        // While it is walked, a variable that is made of itself has no value.
        m_variables.add(variable.index(), ValuesOfVariable { });
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        Constants values;
        if (m_program.at(variable).initialization == VariableInitialization::Function)
            values.append(Constant { .declaration = m_program.at(variable).function });
        for (const Store& store : std::ranges::equal_range(m_stores, variable, { }, &Store::variable)) {
            if (!addAll(values, evaluate(Activation { store.function }, store.stored)))
                break;
        }
        m_sawEverything &= !values.isEmpty();
        // What was read under the choice of one object holds for that choice alone.
        if (m_bindings.isEmpty() && !m_split)
            m_variables.set(variable.index(), ValuesOfVariable { values, m_sawEverything });
        else
            m_variables.remove(variable.index());
        m_sawEverything &= sawEverythingBefore;
        return values;
    }

    Constant constantOf(const DataNode& node) const
    {
        switch (node.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Hole:
            return { KnownValue::undefined() };
        case DataNode::Kind::Null:
            return { KnownValue::null() };
        case DataNode::Kind::Boolean:
            return { KnownValue::boolean(node.as<DataNode::Boolean>().value) };
        case DataNode::Kind::Number:
            return { KnownValue::number(node.as<DataNode::Number>().value) };
        case DataNode::Kind::String:
            return { KnownValue::string(node.as<DataNode::String>().value) };
        case DataNode::Kind::BigInt:
            return { };
        case DataNode::Kind::Array:
        case DataNode::Kind::Object:
            return { .data = &node };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What the literal is made with. A store that comes later is not looked for.
    Constants fieldsOf(const Constants& objects, PropertyKeyID key)
    {
        Constants values;
        for (const Constant& object : objects) {
            if (object.regExp) {
                StringID source = key == m_program.keyOf(m_program.builtins().at(Builtins::RegExpPrototypeSource).name) ? sourceOf(*object.regExp) : StringID();
                if (!source)
                    m_sawEverything = false;
                else if (!add(values, Constant { KnownValue::string(source) }))
                    return { };
                continue;
            }
            if (const DataNode::Object* data = object.data ? object.data->tryAs<DataNode::Object>() : nullptr) {
                size_t index = data->members.findIf([&](const DataMember& member) {
                    return member.key == key;
                });
                if (!add(values, index == notFound ? Constant { KnownValue::undefined() } : constantOf(data->members[index].value)))
                    return { };
                continue;
            }
            const Value* literal = object.literal ? &m_program.at(object.function).at(object.literal) : nullptr;
            if (!literal || literal->opcode != Opcode::NewObject) {
                m_sawEverything = false;
                continue;
            }
            const Layout& layout = m_program.at(literal->layout());
            std::span<const uint32_t> fields = literal->fields();
            size_t index = std::ranges::find_if(fields, [&](uint32_t field) {
                return layout.fields[field].key == key;
            }) - fields.begin();
            if (index == fields.size()) {
                // `{ ...others, key: value }` is made empty and filled behind that.
                const Function& maker = m_program.at(object.function);
                ValueID defined;
                bool isFilledFromOthers = false;
                for (const Value& value : maker.values) {
                    if (value.opcode == Opcode::CopyDataProperties)
                        isFilledFromOthers |= throughCasts(maker, value.targetOperand()) == object.literal;
                    else if (value.opcode == Opcode::PropDefine && value.key() == key && throughCasts(maker, value.objectOperand()) == object.literal)
                        defined = value.valueOperand();
                }
                if (defined) {
                    if (!addAll(values, evaluate(WhereMade(object).activation, defined)))
                        return { };
                } else if (isFilledFromOthers)
                    m_sawEverything = false;
                else if (!add(values, Constant { KnownValue::undefined() }))
                    return { };
                continue;
            }
            if (!addAll(values, evaluate(WhereMade(object).activation, literal->valuesOperands()[index])))
                return { };
        }
        return values;
    }

    // None for the counter of a loop, of which the walk sees where it begins and no more. That leaves nothing out: every element is taken then.
    std::optional<uint32_t> positionOf(const Activation& activation, ValueID id)
    {
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        Constants positions = evaluate(activation, id);
        bool isKnown = std::exchange(m_sawEverything, sawEverythingBefore) && positions.size() == 1 && positions[0].primitive.isNumber();
        if (!isKnown)
            return std::nullopt;
        double position = positions[0].primitive.asNumber();
        if (position < 0 || position != static_cast<uint32_t>(position))
            return std::nullopt;
        return static_cast<uint32_t>(position);
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

    static bool isCall(Opcode opcode)
    {
        return builtinCallOpcodes.contains(opcode) || opcode == Opcode::CallDirect || opcode == Opcode::Call || opcode == Opcode::CallMethod;
    }

    static ValueID throughCasts(const Function& function, ValueID id)
    {
        while (function.at(id).opcode == Opcode::Upcast || function.at(id).opcode == Opcode::AssumeType)
            id = function.at(id).valueOperand();
        return id;
    }

    // An array as long as another whose element at a position is an expression over the other's element at that position: what `map` makes once it is a loop in the function.
    struct MappedArray {
        Constant source;
        ValueID stored;
        Vector<ValueID, 2> readsOfSource;
    };

    // The same where `map` is a call: the expression is what the callback returns, over its first parameter.
    struct ArrayMappedByCallback {
        Constant source;
        FunctionID callback;
    };

    std::optional<ArrayMappedByCallback> arrayMappedByCallbackOf(const Constant& array)
    {
        const Value& call = m_program.at(array.function).at(array.literal);
        ValueID receiver;
        ValueID callback;
        if (call.opcode == Opcode::CallDirect) {
            receiver = call.argumentsOperands()[0];
            callback = call.argumentsOperands()[1];
        } else {
            forEachBuiltinCall(call, [&](const BuiltinCall& builtinCall) {
                receiver = builtinCall.receiver;
                callback = builtinCall.arguments[0];
            });
        }
        WhereMade whereMade(array);
        Constants sources = evaluate(whereMade.activation, receiver);
        Constants callbacks = evaluate(whereMade.activation, callback);
        if (sources.size() != 1 || !sources[0].isObjectOrArray() || callbacks.size() != 1)
            return std::nullopt;
        FunctionID callee = callbacks[0].declaration;
        if (callbacks[0].literal) {
            const Value& closure = m_program.at(callbacks[0].function).at(callbacks[0].literal);
            if (closure.opcode == Opcode::NewClosure)
                callee = closure.function();
        }
        if (!callee || !m_program.at(callee).hasBody())
            return std::nullopt;
        return ArrayMappedByCallback { sources[0], callee };
    }

    std::optional<MappedArray> mappedArrayOf(const Constant& array)
    {
        const Function& function = m_program.at(array.function);
        ValueID sourceOperand = function.at(array.literal).sourceOperand();
        if (!sourceOperand)
            return std::nullopt;
        Constants sources = evaluate(WhereMade(array).activation, sourceOperand);
        if (sources.size() != 1 || !sources[0].isObjectOrArray())
            return std::nullopt;

        MappedArray mapped { sources[0], { }, { } };
        ValueID source = throughCasts(function, sourceOperand);
        ValueID position;
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            const Value& value = function.values[index];
            if (value.opcode == Opcode::PutArrayElement && throughCasts(function, value.arrayOperand()) == array.literal) {
                if (mapped.stored)
                    return std::nullopt;
                mapped.stored = value.valueOperand();
                position = throughCasts(function, value.positionOperand());
            }
        }
        if (!mapped.stored)
            return std::nullopt;
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            const Value& value = function.values[index];
            if (value.opcode == Opcode::GetArrayElement && throughCasts(function, value.arrayOperand()) == source && throughCasts(function, value.positionOperand()) == position)
                mapped.readsOfSource.append(ValueID(index));
        }
        return mapped;
    }

    // A long literal is an array of its length and a store for each element.
    const Vector<ValueID>& elementsOfLongLiteral(const Constant& array)
    {
        return m_longLiterals.ensure(static_cast<uint64_t>(array.function.index()) << 32 | array.literal.index(), [&] {
            const Function& function = m_program.at(array.function);
            Vector<ValueID> elements;
            for (const Value& value : function.values) {
                if (value.opcode != Opcode::PutContiguousIndex || throughCasts(function, value.arrayOperand()) != array.literal)
                    continue;
                KnownValue position = knownValueOf(m_program, function, value.positionOperand());
                if (!position.isNumber() || position.asNumber() != elements.size())
                    return Vector<ValueID>();
                elements.append(value.valueOperand());
            }
            return elements;
        }).iterator->value;
    }

    size_t sizeOf(const Constant& array)
    {
        if (const DataNode::Array* data = array.data ? array.data->tryAs<DataNode::Array>() : nullptr)
            return data->elements.size();
        const Value* literal = array.literal ? &m_program.at(array.function).at(array.literal) : nullptr;
        if (literal && literal->opcode == Opcode::NewArray && literal->holes().empty())
            return literal->elementsOperands().size();
        if (literal && literal->opcode == Opcode::NewContiguousArray)
            return elementsOfLongLiteral(array).size();
        if (literal && literal->opcode == Opcode::NewArrayLike) {
            if (std::optional<MappedArray> mapped = mappedArrayOf(array))
                return sizeOf(mapped->source);
        }
        if (literal && isCall(literal->opcode)) {
            if (std::optional<ArrayMappedByCallback> mapped = arrayMappedByCallbackOf(array))
                return sizeOf(mapped->source);
        }
        return notFound;
    }

    Constants elementOf(const Constant& array, size_t index)
    {
        if (array.data)
            return { constantOf(array.data->as<DataNode::Array>().elements[index]) };
        const Value& literal = m_program.at(array.function).at(array.literal);
        if (literal.opcode == Opcode::NewArray)
            return evaluate(WhereMade(array).activation, literal.elementsOperands()[index]);
        if (literal.opcode == Opcode::NewContiguousArray)
            return evaluate(WhereMade(array).activation, elementsOfLongLiteral(array)[index]);

        if (isCall(literal.opcode)) {
            std::optional<ArrayMappedByCallback> mapped = arrayMappedByCallbackOf(array);
            if (!mapped)
                return { };
            Constants elements = elementOf(mapped->source, index);
            const Function& callback = m_program.at(mapped->callback);
            unsigned first = callback.firstParameterIndex();
            if (elements.size() != 1 || first >= callback.blocks.first().parameters.size())
                return { };
            m_bindings.append({ ValueKey { mapped->callback, callback.blocks.first().parameters[first] }, elements[0] });
            Constants values = returnsOf(Activation { mapped->callback });
            m_bindings.removeLast();
            return values;
        }

        std::optional<MappedArray> mapped = mappedArrayOf(array);
        if (!mapped)
            return { };
        Constants elements = elementOf(mapped->source, index);
        if (elements.size() != 1)
            return { };
        size_t numberOfBindings = m_bindings.size();
        for (ValueID read : mapped->readsOfSource)
            m_bindings.append({ ValueKey { array.function, read }, elements[0] });
        Constants values = evaluate(WhereMade(array).activation, mapped->stored);
        m_bindings.shrink(numberOfBindings);
        return values;
    }

    Constants lengthsOf(const Constants& strings)
    {
        Constants lengths;
        Vector<char16_t> buffer;
        for (const Constant& string : strings) {
            if (string.primitive.kind() != KnownValue::Kind::String)
                continue;
            if (!add(lengths, Constant { KnownValue::number(m_program.strings.viewOf(string.primitive.asString(), buffer).length()) }))
                return { };
        }
        return lengths;
    }

    // A builder that is begun, filled and read in one block is a concatenation.
    Constants textsBuiltBy(const Activation& activation, const Function& function, ValueID end)
    {
        return textsBuiltUpTo(activation, function, function.at(end).builderOperand(), end);
    }

    // What a string builder holds at an operation of the block that begins it, or at the end of that block. A builder that arrives as a parameter of a block holds what it held at the end of
    // the blocks it comes from, and goes on in the block of the parameter.
    Constants textsBuiltUpTo(const Activation& activation, const Function& function, ValueID builder, ValueID end)
    {
        if (!m_stackCheck.isSafeToRecurse() || ++m_valuesFollowed > m_budget) {
            ++m_walksCutShort;
            return { };
        }
        Vector<Constants, 8> parts;
        const Block* blockOfBuilder = nullptr;
        if (function.at(builder).opcode == Opcode::Parameter) {
            for (uint32_t block = 1; block < function.blocks.size() && !blockOfBuilder; ++block) {
                size_t index = function.blocks[block].parameters.find(builder);
                if (index == notFound)
                    continue;
                blockOfBuilder = &function.blocks[block];
                const CFG& cfg = *m_cfgs.ensure(activation.function.index(), [&] {
                    return makeUnique<CFG>(function);
                }).iterator->value;
                Constants before;
                for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                    if (edge.from.index() >= block || isRuledOut(activation, function, cfg, edge))
                        continue;
                    IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(index));
                    if (incoming.kind != IncomingValue::Kind::Argument || !addAll(before, textsBuiltUpTo(activation, function, incoming.argument, ValueID())))
                        return { };
                }
                parts.append(WTF::move(before));
            }
        } else if (function.at(builder).opcode == Opcode::StringBuilderBegin) {
            for (const Block& block : function.blocks) {
                if (block.values.contains(builder))
                    blockOfBuilder = &block;
            }
        }
        if (!blockOfBuilder)
            return { };

        for (ValueID id : blockOfBuilder->values) {
            const Value& value = function.at(id);
            if (id == builder || !OperandPosition::builder(value.opcode) || value.builderOperand() != builder)
                continue;
            if (id == end)
                return concatenated(parts.span());
            switch (value.opcode) {
            case Opcode::StringBuilderAppendConstant:
                parts.append(Constants { Constant { KnownValue::string(value.string()) } });
                break;
            case Opcode::StringBuilderAppendString:
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
        bool isWithinLimits = true;
        bool hasChoices = forEachChoice(parts, [&](std::span<const Constant> choice) {
            StringBuilder builder;
            Vector<char16_t> buffer;
            for (const Constant& part : choice) {
                if (part.primitive.isNumber())
                    builder.append(String::number(part.primitive.asNumber()));
                else if (part.primitive.kind() == KnownValue::Kind::String)
                    builder.append(m_program.strings.viewOf(part.primitive.asString(), buffer));
                else
                    return;
            }
            if (!isWithinLimits || builder.hasOverflowed() || m_context.exceeds(LimitID::RegExpCandidateTextLength, builder.length() * sizeof(char16_t), m_subject))
                return;
            isWithinLimits = add(texts, Constant { KnownValue::string(m_strings.intern(builder.toString())) });
        });
        if (!hasChoices || !isWithinLimits)
            return { };
        return texts;
    }

    // A template object and its `raw` array are frozen, so they hold the strings of the site.
    Constants rawTextsOf(const Activation& activation, std::span<const ValueID> arguments)
    {
        const Function& function = m_program.at(activation.function);
        const Value& templateObject = function.at(sameObjectThroughCasts(function, arguments[0]));
        if (templateObject.opcode != Opcode::NewTemplateObject)
            return { };
        std::span<const ValueID> substitutions = arguments.subspan(1);
        auto templateStrings = templateObject.templateStrings();
        Vector<Constants, 8> parts;
        for (size_t index = 0; index < templateStrings.size(); ++index) {
            parts.append(Constants { Constant { KnownValue::string(templateStrings[index].raw) } });
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
            bool isWithinLimits = true;
            bool hasChoices = forEachChoice(parts.span(), [&](std::span<const Constant> choice) {
                StringBuilder builder;
                Vector<char16_t> buffer;
                for (size_t index = 1; index < choice.size(); ++index) {
                    if (index > 1) {
                        if (choice[0].primitive.kind() == KnownValue::Kind::Undefined)
                            builder.append(',');
                        else if (choice[0].primitive.kind() == KnownValue::Kind::String)
                            builder.append(m_program.strings.viewOf(choice[0].primitive.asString(), buffer));
                        else
                            return;
                    }
                    if (choice[index].primitive.kind() == KnownValue::Kind::Undefined || choice[index].primitive.kind() == KnownValue::Kind::Null)
                        continue;
                    if (choice[index].primitive.kind() != KnownValue::Kind::String)
                        return;
                    builder.append(m_program.strings.viewOf(choice[index].primitive.asString(), buffer));
                }
                if (!isWithinLimits || builder.hasOverflowed() || m_context.exceeds(LimitID::RegExpCandidateTextLength, builder.length() * sizeof(char16_t), m_subject))
                    return;
                isWithinLimits = add(texts, Constant { KnownValue::string(m_strings.intern(builder.toString())) });
            });
            if (!hasChoices || !isWithinLimits)
                return { };
        }
        return texts;
    }

    struct BlockParameter {
        ValueID parameter;
        BlockID block;
        uint32_t index;
    };

    std::optional<BlockParameter> findBlockParameter(FunctionID id, const Function& function, ValueID parameter)
    {
        const Vector<BlockParameter>& sortedParameters = m_blockParameters.ensure(id.index(), [&] {
            Vector<BlockParameter> result;
            for (uint32_t block = 0; block < function.blocks.size(); ++block) {
                const auto& parameters = function.blocks[block].parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index)
                    result.append(BlockParameter { parameters[index], BlockID(block), index });
            }
            std::ranges::stable_sort(result, { }, &BlockParameter::parameter);
            return result;
        }).iterator->value;
        auto found = std::ranges::lower_bound(sortedParameters, parameter, { }, &BlockParameter::parameter);
        if (found == sortedParameters.end() || found->parameter != parameter)
            return std::nullopt;
        return *found;
    }

    Constants valuesArrivingAt(const Activation& activation, const Function& function, ValueID parameter)
    {
        std::optional<BlockParameter> location = findBlockParameter(activation.function, function, parameter);
        if (!location)
            return { };
        if (location->block == entryBlock)
            return argumentsFor(activation, function, location->index);
        const CFG& cfg = *m_cfgs.ensure(activation.function.index(), [&] {
            return makeUnique<CFG>(function);
        }).iterator->value;
        Constants values;
        for (Edge edge : cfg.incomingEdges(location->block)) {
            if (isRuledOut(activation, function, cfg, edge))
                continue;
            IncomingValue incoming = incomingValueOf(function, edge, location->index);
            if (incoming.kind == IncomingValue::Kind::Argument) {
                if (!addAll(values, evaluate(activation, incoming.argument)))
                    break;
                continue;
            }
            // The operand of a test, narrowed on this edge: what does not have the type of the parameter does not come this way.
            ValueID narrowed = operandNarrowedOnEdge(function, incoming.result);
            Constants arriving = narrowed ? evaluate(activation, narrowed) : Constants();
            if (arriving.isEmpty()) {
                m_sawEverything = false;
                continue;
            }
            const Type& type = m_program.typeOf(function, parameter);
            arriving.removeAllMatching([&](const Constant& constant) {
                if (constant.regExp || constant.isObjectOrArray())
                    return type.withOnlyObjects().isNever();
                return constant.primitive.isKnown() && !constant.primitive.isObject() && evaluateTypeTest(m_program, constant.primitive, type) == KnownBoolean::False;
            });
            if (!arriving.isEmpty() && !addAll(values, arriving))
                break;
        }
        return values;
    }

    // What a test answers for every value that its operands may have, if that is one answer.
    KnownBoolean answerOf(const Activation& activation, const Function& function, ValueID test)
    {
        const Value& value = function.at(test);
        bool sawEverythingBefore = std::exchange(m_sawEverything, true);
        auto restore = makeScopeExit([&] {
            m_sawEverything = sawEverythingBefore;
        });
        auto sameForAll = [&](const Constants& constants, NOESCAPE const Invocable<KnownBoolean(const Constant&)> auto& answer) {
            KnownBoolean common = KnownBoolean::Unknown;
            if (!m_sawEverything)
                return common;
            for (const Constant& constant : constants) {
                KnownBoolean one = answer(constant);
                if (one == KnownBoolean::Unknown || (common != KnownBoolean::Unknown && one != common))
                    return KnownBoolean::Unknown;
                common = one;
            }
            return common;
        };
        switch (value.opcode) {
        case Opcode::TypeTest: {
            const Type& tested = m_program.at(value.testedType());
            return sameForAll(evaluate(activation, value.valueOperand()), [&](const Constant& constant) {
                if (constant.regExp || constant.isObjectOrArray())
                    return tested.withOnlyObjects().isNever() ? KnownBoolean::False : KnownBoolean::Unknown;
                return constant.primitive.isKnown() && !constant.primitive.isObject() ? evaluateTypeTest(m_program, constant.primitive, tested) : KnownBoolean::Unknown;
            });
        }
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq: {
            Constants left = evaluate(activation, value.leftOperand());
            Constants right = evaluate(activation, value.rightOperand());
            if (!m_sawEverything || left.size() != 1 || right.size() != 1 || !left[0].primitive.isNumber() || !right[0].primitive.isNumber())
                return KnownBoolean::Unknown;
            double a = left[0].primitive.asNumber();
            double b = right[0].primitive.asNumber();
            bool answer = value.opcode == Opcode::Less ? a < b : value.opcode == Opcode::LessEq ? a <= b : value.opcode == Opcode::Greater ? a > b : a >= b;
            return answer ? KnownBoolean::True : KnownBoolean::False;
        }
        default:
            return KnownBoolean::Unknown;
        }
    }

    // An arm of a condition that the known values never take gives no text. The way into the merge comes straight from the branch or through a block that only the branch enters.
    bool isRuledOut(const Activation& activation, const Function& function, const CFG& cfg, Edge edge)
    {
        if (function.at(edge.from).terminator.kind == TerminatorKind::Jump) {
            std::span<const Edge> ways = cfg.incomingEdges(edge.from);
            if (ways.size() != 1)
                return false;
            edge = ways[0];
        }
        const Terminator& terminator = function.at(edge.from).terminator;
        if ((terminator.kind != TerminatorKind::TestAndBranch && terminator.kind != TerminatorKind::Branch) || edge.isHandlerEdge())
            return false;
        Opcode test = function.at(terminator.conditionOperand()).opcode;
        if (test != Opcode::TypeTest && test != Opcode::Less && test != Opcode::LessEq && test != Opcode::Greater && test != Opcode::GreaterEq)
            return false;
        // The operands of a test are walked with a small part of the walk's budget, and few tests deep: a test in a loop asks for the loop's own values.
        if (m_testsBeingAnswered == maxTestsBeingAnswered)
            return false;
        SetForScope testsBeingAnswered(m_testsBeingAnswered, m_testsBeingAnswered + 1);
        unsigned valuesFollowedBefore = m_valuesFollowed;
        unsigned walksCutShortBefore = m_walksCutShort;
        SetForScope budget(m_budget, std::min(m_budget, m_valuesFollowed + maxValuesFollowedForTest));
        KnownBoolean answer = answerOf(activation, function, terminator.conditionOperand());
        if (m_valuesFollowed > m_budget) {
            m_valuesFollowed = valuesFollowedBefore + maxValuesFollowedForTest;
            m_walksCutShort = walksCutShortBefore;
            return false;
        }
        return answer != KnownBoolean::Unknown && (answer == KnownBoolean::True) != !edge.targetIndex;
    }

    Constants argumentsFor(const Activation& activation, const Function& function, uint32_t index)
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
        auto calls = std::ranges::equal_range(m_directCalls, activation.function, { }, &DirectCall::callee);
        for (const DirectCall& call : calls) {
            std::span<const ValueID> arguments = m_program.at(call.caller).at(call.call).argumentsOperands();
            if (numberOfParameters == arguments.size() && !addAll(values, evaluate(Activation { call.caller }, arguments[index])))
                return { };
        }
        if (function.entryParameterKind(index) == EntryParameter::Parameter && !addArgumentsGivenToClosures(values, activation.function, index - function.firstParameterIndex()))
            return { };
        return values;
    }

    // A variable that closures share and rewrite, as the text in a builder whose methods answer the builder itself: `make(a).replace(b, c).replace(d, e).finish()`.
    // The environment of each call comes out of the answer of the call before, so the variable is read as the call left it through which the environment has come:
    // what that call's function stores there, with the operands of that call. There the variable is read the same way, through the call before.
    Constants fieldSeenThrough(const Activation& activation, ValueID environment, LayoutID layout, uint32_t field)
    {
        if (!m_stackCheck.isSafeToRecurse() || ++m_valuesFollowed > m_budget) {
            ++m_walksCutShort;
            return { };
        }
        const Function& function = m_program.at(activation.function);
        const Value& value = function.at(environment);
        switch (value.opcode) {
        case Opcode::Upcast:
        case Opcode::AssumeType:
        case Opcode::CheckInitialized:
        case Opcode::RequireObjectCoercible:
        case Opcode::RequireObject:
        case Opcode::FinishObject:
            return fieldSeenThrough(activation, value.operand(0), layout, field);
        case Opcode::GetCapture:
            return fieldSeenThrough(activation, value.closureOperand(), layout, field);
        case Opcode::GetSlot:
        case Opcode::GetSlotOrUndefined:
        case Opcode::GetDeferredClosureSlot:
        case Opcode::GetDeferredClosureSlotOrUndefined:
        case Opcode::PropGet:
            return fieldSeenThrough(activation, value.objectOperand(), layout, field);
        case Opcode::Parameter: {
            const auto& parameters = function.blocks.first().parameters;
            size_t index = parameters.find(environment);
            if (index == notFound)
                return fieldSeenThroughMerge(activation, function, environment, layout, field);
            if (!activation.call || activation.call->argumentsOperands().size() != parameters.size())
                return { };
            return fieldSeenThrough(*activation.caller, activation.call->argumentsOperands()[index], layout, field);
        }
        case Opcode::NewObject: {
            std::span<const uint32_t> fields = value.fields();
            size_t index = std::ranges::find(fields, field) - fields.begin();
            if (value.layout() != layout || index == fields.size())
                return { };
            return evaluate(activation, value.valuesOperands()[index]);
        }
        case Opcode::CallDirect: {
            FunctionID callee = value.function();
            const Function& body = m_program.at(callee);
            if (!body.hasBody())
                return { };
            if (isActive(activation, callee)) {
                ++m_walksCutShort;
                return { };
            }
            // Each step of a long chain is asked for by every store of the step behind it.
            bool mayRemember = m_bindings.isEmpty();
            MemoKey memoKey { callsLeadingTo(activation), &value, layout.index(), field };
            if (mayRemember) {
                if (auto remembered = m_remembered.find(memoKey); remembered != m_remembered.end()) {
                    m_sawEverything &= remembered->value->sawEverything;
                    return remembered->value->values;
                }
            }
            unsigned walksCutShortBefore = m_walksCutShort;
            bool sawEverythingBefore = std::exchange(m_sawEverything, true);
            auto remember = makeScopeExit([&] {
                m_sawEverything &= sawEverythingBefore;
            });
            Activation inCallee { callee, &activation, &value };
            Constants values;
            bool leavesFieldAlone = true;
            for (const Value& store : body.values) {
                bool storesField = store.opcode == Opcode::PutEnvironmentField && store.layout() == layout && store.field() == field;
                bool makesEnvironment = store.opcode == Opcode::NewObject && store.layout() == layout && std::ranges::contains(store.fields(), field);
                leavesFieldAlone &= !storesField && !makesEnvironment;
                if (storesField && !addAll(values, evaluate(inCallee, store.valueOperand())))
                    return { };
                if (makesEnvironment) {
                    std::span<const uint32_t> fields = store.fields();
                    if (!addAll(values, evaluate(inCallee, store.valuesOperands()[std::ranges::find(fields, field) - fields.begin()])))
                        return { };
                }
            }
            // A function that only stands in front of another, a version of it for some operands, leaves the variable as its own calls leave it.
            if (leavesFieldAlone) {
                for (uint32_t inner = 0; inner < body.values.size(); ++inner) {
                    if (body.values[inner].opcode != Opcode::CallDirect)
                        continue;
                    Constants leftByCall = fieldSeenThrough(inCallee, ValueID(inner), layout, field);
                    if (!leftByCall.isEmpty() && !addAll(values, leftByCall))
                        return { };
                }
            }
            // A step that does not touch the variable hands it on as it got it, with the environment among its operands.
            if (leavesFieldAlone && values.isEmpty()) {
                for (ValueID operand : value.argumentsOperands()) {
                    Opcode opcode = function.at(throughCasts(function, operand)).opcode;
                    if (opcode != Opcode::GetCapture && opcode != Opcode::NewObject && opcode != Opcode::Parameter)
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

    Constants fieldSeenThroughMerge(const Activation& activation, const Function& function, ValueID parameter, LayoutID layout, uint32_t field)
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
        for (uint32_t block = 1; block < function.blocks.size(); ++block) {
            size_t index = function.blocks[block].parameters.find(parameter);
            if (index == notFound)
                continue;
            const CFG& cfg = *m_cfgs.ensure(activation.function.index(), [&] {
                return makeUnique<CFG>(function);
            }).iterator->value;
            Constants values;
            for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(index));
                ValueID arriving = incoming.kind == IncomingValue::Kind::Argument ? incoming.argument : operandNarrowedOnEdge(function, incoming.result);
                if (arriving && !addAll(values, fieldSeenThrough(activation, arriving, layout, field)))
                    return { };
            }
            return values;
        }
        return { };
    }

    // What the closure is made with, where the walk knows the closure. Else what any closure of the function is made with.
    Constants capturesOf(const Activation& activation, const Value& read)
    {
        bool sawEverythingBefore = m_sawEverything;
        Constants closures = evaluate(activation, read.closureOperand());
        m_sawEverything = sawEverythingBefore;
        Constants values;
        bool knowsEveryClosure = !closures.isEmpty();
        for (const Constant& closure : closures) {
            const Value* literal = closure.literal ? &m_program.at(closure.function).at(closure.literal) : nullptr;
            if (!literal || literal->opcode != Opcode::NewClosure || read.index() >= literal->capturesOperands().size()) {
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

    static bool callsValue(const Function& function, const Value& value, ValueID callee)
    {
        return (value.opcode == Opcode::Call || value.opcode == Opcode::CallResolved) && throughCasts(function, value.calleeOperand()) == callee;
    }

    Constants answersOfMethodOfText(const Activation& activation, const Function& function, const Value& call)
    {
        ValueID receiver;
        PropertyKeyID key;
        if (call.opcode == Opcode::CallMethod) {
            receiver = call.receiverOperand();
            key = call.key();
        } else if (const Value& read = function.at(throughCasts(function, call.calleeOperand())); read.opcode == Opcode::PropGet && call.thisValueOperand()) {
            if (throughCasts(function, read.objectOperand()) == throughCasts(function, call.thisValueOperand())) {
                receiver = call.thisValueOperand();
                key = read.key();
            }
        }
        if (!receiver || !key)
            return { };
        Constants receivers = evaluate(activation, receiver);
        bool areTexts = !receivers.isEmpty() && std::ranges::all_of(receivers, [](const Constant& receiver) {
            return receiver.primitive.kind() == KnownValue::Kind::String;
        });
        if (!areTexts)
            return { };
        const BuiltinTable& table = m_program.builtins();
        for (BuiltinID member : table.membersOf(Builtins::StringPrototype)) {
            if (m_program.keyOf(table.at(member).name) != key)
                continue;
            for (BuiltinOverloadID overload : table.overloadIDsOf(table.canonicalID(member))) {
                if (table.at(overload).mode == BuiltinCallMode::Call)
                    return answersOfBuiltin(activation, call, BuiltinCall { overload, BuiltinCallForm::Whole, 0, receiver, call.argumentsOperands() });
            }
        }
        return { };
    }

    // Calls the functor with the call as each overload that it may be describes it.
    void forEachBuiltinCall(const Value& value, NOESCAPE const Invocable<void(const BuiltinCall&)> auto& functor) const
    {
        if (builtinCallOpcodes.contains(value.opcode)) {
            functor(builtinCallOf(m_program, value));
            return;
        }
        if ((value.opcode != Opcode::Call && value.opcode != Opcode::CallMethod) || !value.callees())
            return;
        const CalleeSet& callees = m_program.at(value.callees());
        if (!callees.isComplete || !callees.functions.isEmpty() || callees.builtins.size() != 1)
            return;
        const BuiltinTable& table = m_program.builtins();
        ValueID receiver = value.opcode == Opcode::Call ? value.thisValueOperand() : value.receiverOperand();
        for (BuiltinOverloadID overload : table.overloadIDsOf(table.canonicalID(callees.builtins[0]))) {
            if (table.at(overload).mode == BuiltinCallMode::Call)
                functor(BuiltinCall { overload, BuiltinCallForm::Whole, 0, receiver, value.argumentsOperands() });
        }
    }

    // The table of built-ins says what a built-in calls an operand with: `map` its callback with the elements of its receiver.
    bool addArgumentsGivenByBuiltin(Constants& values, const Activation& activation, const Function& function, const Value& use, ValueID closure, uint32_t parameter)
    {
        bool isWithinLimits = true;
        forEachBuiltinCall(use, [&](const BuiltinCall& call) {
            if (call.form != BuiltinCallForm::Whole)
                return;
            for (const BuiltinCallback& callback : m_program.builtins().at(call.overload).flow.callbacks) {
                ValueID callee = call.operandAt(callback.callee);
                if (!isWithinLimits || !callee || throughCasts(function, callee) != closure)
                    continue;
                if (parameter >= callback.arguments.size()) {
                    isWithinLimits = add(values, Constant { KnownValue::undefined() });
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
                default:
                    m_sawEverything = false;
                    break;
                }
            }
        });
        return isWithinLimits;
    }

    // A function that is made a closure and handed to another function is called there. What it is given there is read with the operands of the very call that hands it over,
    // so the callback of one `map` sees the elements of its own array and of no other.
    bool addArgumentsGivenToClosures(Constants& values, FunctionID callee, uint32_t parameter)
    {
        for (const DirectCall& closure : std::ranges::equal_range(m_closures, sourceOf(callee), { }, &DirectCall::callee)) {
            const Function& maker = m_program.at(closure.caller);
            Activation inMaker { closure.caller };
            for (const Value& use : maker.values) {
                if (callsValue(maker, use, closure.call)) {
                    std::span<const ValueID> arguments = use.argumentsOperands();
                    if (!addAll(values, evaluate(inMaker, parameter < arguments.size() ? arguments[parameter] : ValueID())))
                        return false;
                    continue;
                }
                if (!addArgumentsGivenByBuiltin(values, inMaker, maker, use, closure.call, parameter))
                    return false;
                if (use.opcode != Opcode::CallDirect)
                    continue;
                const Function& receiver = m_program.at(use.function());
                std::span<const ValueID> operands = use.argumentsOperands();
                if (!receiver.hasBody() || receiver.blocks.first().parameters.size() != operands.size() || isActive(inMaker, use.function()))
                    continue;
                bool handsClosureOver = std::ranges::any_of(operands, [&](ValueID operand) {
                    return throughCasts(maker, operand) == closure.call;
                });
                if (!handsClosureOver)
                    continue;
                Activation inReceiver { use.function(), &inMaker, &use };
                Constant handedOver { .function = closure.caller, .literal = closure.call };
                for (const Value& call : receiver.values) {
                    if (call.opcode != Opcode::Call && call.opcode != Opcode::CallResolved)
                        continue;
                    // The callee may have come a long way from the parameter: through tests, merges and loops.
                    bool sawEverythingBefore = m_sawEverything;
                    bool callsClosure = evaluate(inReceiver, call.calleeOperand()).containsIf([&](const Constant& callee) {
                        return callee.function == handedOver.function && callee.literal == handedOver.literal;
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

    Constants resultsOf(const Activation& activation, const Value& call)
    {
        FunctionID callee = call.function();
        const Function& function = m_program.at(callee);
        if (!function.hasBody() || function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || isActive(activation, callee))
            return { };
        return returnsOf(Activation { callee, &activation, &call });
    }

    Constants returnsOf(const Activation& inCallee)
    {
        Constants values;
        for (const Block& block : m_program.at(inCallee.function).blocks) {
            ValueID result;
            switch (block.terminator.kind) {
            case TerminatorKind::Return:
                result = block.terminator.valueOperand();
                break;
            case TerminatorKind::ReturnCall:
                result = block.terminator.callOperand();
                break;
            default:
                continue;
            }
            if (result && !addAll(values, evaluate(inCallee, result)))
                break;
        }
        return values;
    }

    // Calls the functor with every choice of one alternative an operand. False if there are more choices than a call may have alternatives.
    bool forEachChoice(std::span<const Constants> operands, NOESCAPE const Invocable<void(std::span<const Constant>)> auto& functor) const
    {
        uint64_t choices = 1;
        unsigned operandsWithAlternatives = 0;
        for (const Constants& alternatives : operands) {
            choices *= alternatives.size();
            operandsWithAlternatives += alternatives.size() > 1;
            if (!choices || choices > maxAlternatives())
                return false;
        }
        // Alternatives of one operand are an enumeration: callers, stores, rows. Alternatives of two, multiplied, are mostly texts that nobody makes.
        if (operandsWithAlternatives > 1)
            return false;
        Vector<Constant, 8> choice(operands.size());
        for (uint64_t number = 0; number < choices; ++number) {
            uint64_t rest = number;
            for (size_t index = 0; index < operands.size(); ++index) {
                choice[index] = operands[index][rest % operands[index].size()];
                rest /= operands[index].size();
            }
            functor(choice.span());
        }
        return true;
    }

    uint64_t bytesOf(const Constant& constant) const
    {
        if (constant.regExp)
            return m_program.string(constant.regExp->pattern).size();
        return constant.primitive.kind() == KnownValue::Kind::String ? m_program.string(constant.primitive.asString()).size() : 0;
    }

    StringID sourceOf(RegExpLiteralInType literal)
    {
        Vector<char16_t> buffer;
        const RegExpSummary& summary = m_context.regExpSummaries().summaryOf(literal.pattern, toYarrFlags(literal.flags()), m_program.strings.viewOf(literal.pattern, buffer));
        if (!summary.isValid())
            return { };
        return summary.sourceIsPattern ? literal.pattern : m_strings.intern(StringView(summary.source));
    }

    // A function of the engine that depends on its operands and on nothing else, no locale and no clock, answers in a realm of the build's own.
    Constants answersOfBuiltin(const Activation& activation, const Value& value, const BuiltinCall& call)
    {
        const BuiltinTable& table = m_program.builtins();
        const BuiltinOverload& overload = table.at(call.overload);
        BuiltinID callee = table.canonicalID(overload.builtin);
        if (call.form != BuiltinCallForm::Whole && call.form != BuiltinCallForm::AnyReceiver)
            return { };
        if (overload.mode == BuiltinCallMode::Get && callee == Builtins::ArrayPrototypeLength) {
            Constants lengths;
            for (const Constant& receiver : evaluate(activation, call.receiver)) {
                size_t size = receiver.isObjectOrArray() ? sizeOf(receiver) : notFound;
                if (size == notFound)
                    m_sawEverything = false;
                else if (!add(lengths, Constant { KnownValue::number(static_cast<double>(size)) }))
                    return { };
            }
            return lengths;
        }
        if (overload.mode == BuiltinCallMode::Get && callee == Builtins::RegExpPrototypeSource) {
            Constants sources;
            for (const Constant& receiver : evaluate(activation, call.receiver)) {
                StringID source = receiver.regExp ? sourceOf(*receiver.regExp) : StringID();
                if (source && !add(sources, Constant { KnownValue::string(source) }))
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
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::ArrayPrototypeJoin && call.arguments.size() <= 1)
            return joined(evaluate(activation, call.receiver), evaluate(activation, call.arguments.empty() ? ValueID() : call.arguments[0]));
        if (overload.mode == BuiltinCallMode::Call && callee == Builtins::StringRaw && !call.arguments.empty())
            return rawTextsOf(activation, call.arguments);
        bool dependsOnOperandsAlone = table.at(callee).flags.contains(BuiltinFlag::IsDeterministic) || callee == Builtins::NumberPrototypeToString;
        if (overload.mode != BuiltinCallMode::Call || !dependsOnOperandsAlone || !call.receiver)
            return { };

        Vector<Constants, 4> operands;
        operands.append(evaluate(activation, call.receiver));
        for (ValueID argument : call.arguments)
            operands.append(evaluate(activation, argument));
        return answersOf(callee, WTF::move(operands));
    }

    // One of the several results of an operation.
    Constants resultsAt(const Activation& activation, const Function& function, ValueID results, uint32_t index)
    {
        const Value& value = function.at(results);
        switch (value.opcode) {
        case Opcode::Results:
            return index < value.numberOfOperands() ? evaluate(activation, value.operand(index)) : Constants();
        case Opcode::StringRange:
            return boundsOf(activation, value, index);
        case Opcode::CallDirect: {
            FunctionID callee = value.function();
            const Function& body = m_program.at(callee);
            if (!body.hasBody() || isActive(activation, callee))
                return { };
            Activation inCallee { callee, &activation, &value };
            Constants values;
            for (const Block& block : body.blocks) {
                if (block.terminator.kind != TerminatorKind::Return || !block.terminator.valueOperand())
                    continue;
                // A result that a way out of the function does not give is left unspecified there.
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

    Constants boundsOf(const Activation& activation, const Value& range, uint32_t index)
    {
        Vector<Constants, 3> operands { evaluate(activation, range.lengthOperand()), evaluate(activation, range.firstOperand()), evaluate(activation, range.secondOperand()) };
        Constants bounds;
        bool isWithinLimits = true;
        bool hasChoices = forEachChoice(operands.span(), [&](std::span<const Constant> choice) {
            bool hasSecond = choice[2].primitive.kind() != KnownValue::Kind::Undefined;
            if (!isWithinLimits || !choice[0].primitive.isNumber() || !choice[1].primitive.isNumber() || (hasSecond && !choice[2].primitive.isNumber()))
                return;
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
            case StringRangeMethod::Slice:
                begin = fromEitherEnd(first);
                end = std::max(begin, hasSecond ? fromEitherEnd(second) : length);
                break;
            case StringRangeMethod::Substring:
                begin = std::clamp(first, 0.0, length);
                end = std::clamp(second, 0.0, length);
                if (begin > end)
                    std::swap(begin, end);
                break;
            case StringRangeMethod::Substr:
                begin = fromEitherEnd(first);
                end = hasSecond ? std::min(begin + std::max(second, 0.0), length) : length;
                break;
            case StringRangeMethod::SingleIndex:
            case StringRangeMethod::SingleRelativeIndex:
                return;
            }
            isWithinLimits = add(bounds, Constant { KnownValue::number(index ? end : begin) });
        });
        if (!hasChoices || !isWithinLimits)
            return { };
        return bounds;
    }

    // `slice`, `substring` and `substr` are an operation that makes two bounds of their operands and one that cuts. The engine's function is asked with the operands.
    Constants substringsOf(const Activation& activation, const Function& function, const Value& value)
    {
        const Value& start = function.at(value.startOperand());
        const Value& end = function.at(value.endOperand());
        bool cutsRange = start.opcode == Opcode::ResultAt && end.opcode == Opcode::ResultAt && start.resultsOperand() == end.resultsOperand() && function.at(start.resultsOperand()).opcode == Opcode::StringRange;
        if (!cutsRange)
            return answersOf(Builtins::StringPrototypeSubstring, { evaluate(activation, value.stringOperand()), evaluate(activation, value.startOperand()), evaluate(activation, value.endOperand()) });
        const Value& range = function.at(start.resultsOperand());
        BuiltinID method;
        switch (range.method()) {
        case StringRangeMethod::Slice:
            method = Builtins::StringPrototypeSlice;
            break;
        case StringRangeMethod::Substring:
            method = Builtins::StringPrototypeSubstring;
            break;
        case StringRangeMethod::Substr:
            method = Builtins::StringPrototypeSubstr;
            break;
        case StringRangeMethod::SingleIndex:
        case StringRangeMethod::SingleRelativeIndex:
            return { };
        }
        return answersOf(method, { evaluate(activation, value.stringOperand()), evaluate(activation, range.firstOperand()), evaluate(activation, range.secondOperand()) });
    }

    // A RegExp that is made of a known text and known flags is as good as a literal where its source is read or another is made of it.
    Constants createdBy(const Activation& activation, const BuiltinCall& call)
    {
        if (call.arguments.empty() || call.arguments.size() > 2)
            return { };
        Vector<Constants, 2> operands { evaluate(activation, call.arguments[0]), evaluate(activation, call.arguments.size() > 1 ? call.arguments[1] : ValueID()) };
        Constants created;
        bool isWithinLimits = true;
        Vector<char16_t> buffer;
        bool hasChoices = forEachChoice(operands.span(), [&](std::span<const Constant> choice) {
            StringID text = choice[0].regExp ? choice[0].regExp->pattern : choice[0].primitive.kind() == KnownValue::Kind::String ? choice[0].primitive.asString() : StringID();
            if (!text || !isWithinLimits)
                return;
            OptionSet<RegExpFlags> flags = choice[0].regExp ? choice[0].regExp->flags() : OptionSet<RegExpFlags> { };
            if (choice[1].primitive.kind() == KnownValue::Kind::String) {
                std::optional<OptionSet<Yarr::Flags>> parsed = Yarr::parseFlags(m_program.strings.viewOf(choice[1].primitive.asString(), buffer));
                if (!parsed)
                    return;
                flags = { };
                for (RegExpFlags flag : allMembersOf<RegExpFlags>) {
                    if (parsed->contains(toYarrFlag(flag)))
                        flags.add(flag);
                }
            } else if (choice[1].primitive.kind() != KnownValue::Kind::Undefined)
                return;
            isWithinLimits = add(created, Constant { .regExp = RegExpLiteralInType { text, flags.toRaw() } });
        });
        if (!hasChoices || !isWithinLimits)
            return { };
        return created;
    }

    // The first operand is the receiver.
    Constants answersOf(BuiltinID callee, Vector<Constants, 4>&& operands)
    {
        Constants answers;
        bool isWithinLimits = true;
        bool hasChoices = forEachChoice(operands.span(), [&](std::span<const Constant> choice) {
            if (!isWithinLimits || (choice[0].primitive.kind() != KnownValue::Kind::String && !choice[0].primitive.isNumber()))
                return;
            uint64_t bytes = 0;
            double largestNumber = 1;
            Vector<ScratchRealm::Operand, 4> arguments;
            for (const Constant& operand : choice) {
                if (operand.isObjectOrArray() || (!operand.regExp && !operand.primitive.isKnown()))
                    return;
                arguments.append(ScratchRealm::Operand { operand.primitive, operand.regExp });
                bytes += bytesOf(operand);
                if (operand.primitive.isNumber())
                    largestNumber = std::max(largestNumber, operand.primitive.asNumber());
            }
            // A number among the operands may set the length of the answer: a count, a length. The limit is asked before the answer is made.
            double bytesOfAnswer = static_cast<double>(bytes) * largestNumber;
            if (bytesOfAnswer >= static_cast<double>(std::numeric_limits<uint32_t>::max()) || m_context.exceeds(LimitID::RegExpCandidateTextLength, static_cast<uint64_t>(bytesOfAnswer), m_subject))
                return;
            if (!m_scratchRealm)
                m_scratchRealm.emplace();
            std::optional<ScratchRealm::Answer> answer = m_scratchRealm->call(m_program, callee, choice[0].primitive, arguments.span().subspan(1));
            if (!answer)
                return;
            KnownValue primitive = answer->primitive;
            if (!answer->text.isNull()) {
                if (m_context.exceeds(LimitID::RegExpCandidateTextLength, answer->text.sizeInBytes(), m_subject))
                    return;
                primitive = KnownValue::string(m_strings.intern(StringView(answer->text)));
            }
            isWithinLimits = add(answers, Constant { primitive });
        });
        if (!hasChoices || !isWithinLimits)
            return { };
        return answers;
    }

    Constants concatenate(const Activation& activation, const Value& value)
    {
        Vector<Constants, 8> operands;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index)
            operands.append(evaluate(activation, value.operand(index)));

        Constants texts;
        bool isWithinLimits = true;
        bool hasChoices = forEachChoice(operands.span(), [&](std::span<const Constant> choice) {
            uint64_t bytes = 0;
            for (const Constant& operand : choice) {
                if (operand.primitive.kind() != KnownValue::Kind::String && !operand.primitive.isNumber())
                    return;
                bytes += bytesOf(operand);
            }
            if (!isWithinLimits || m_context.exceeds(LimitID::RegExpCandidateTextLength, bytes, m_subject))
                return;
            uint32_t next = 0;
            KnownValue text = evaluateStringOperation(m_program, m_strings, m_program.at(activation.function), value, [&](ValueID) {
                return choice[next++].primitive;
            });
            if (text.kind() == KnownValue::Kind::String)
                isWithinLimits = add(texts, Constant { text });
        });
        if (!hasChoices || !isWithinLimits)
            return { };
        return texts;
    }


    const Program& m_program;
    StringArena& m_strings;
    BuildContext& m_context;
    // The walk summarizes a RegExp that it has put together itself, at whatever depth it stands.
    StackCheck m_stackCheck { Thread::currentSingleton().stack(), regExpCompilerStackSize + 16 * KB };
    Subject m_subject;
    std::map<std::pair<uint32_t, const Value*>, uint32_t> m_chainsOfCalls;
    struct Remembered {
        Constants values;
        bool sawEverything;
        std::optional<Split> split;
    };

    unsigned m_valuesFollowed { 0 };
    unsigned m_walksCutShort { 0 };
    unsigned m_worlds { 0 };
    HashMap<MemoKey, std::unique_ptr<Remembered>, MemoKeyHash, MemoKeyHashTraits> m_remembered;
    bool m_sawEverything { true };
    std::optional<Split> m_split;
    Vector<std::pair<ValueKey, Constant>, 4> m_bindings;
    Vector<Store> m_stores;
    Vector<DirectCall> m_directCalls;
    Vector<DirectCall> m_closures;
    Vector<FunctionID, 2> m_functionsOfMap;
    HashMap<uint64_t, Vector<ValueID>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_longLiterals;
    std::optional<ScratchRealm> m_scratchRealm;
    Vector<std::tuple<const Value*, FunctionID, ValueID>, 32> m_active;
    UncheckedKeyHashMap<uint32_t, ValuesOfVariable, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
    HashMap<uint32_t, std::unique_ptr<CFG>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cfgs;
    HashMap<uint32_t, Vector<BlockParameter>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blockParameters;
};

} // anonymous namespace

static auto keyOf(const RegExpRequest& request)
{
    return std::pair { request.pattern.index(), request.flags.toRaw() };
}

static auto orderKey(const FoundRegExp& found)
{
    return std::tuple { found.origin, found.pattern, found.flags.toRaw(), found.site };
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

static bool callsRegExpConstructor(const Program& program, BuiltinOverloadID overload)
{
    const BuiltinTable& builtins = program.builtins();
    return builtins.canonicalID(builtins.at(overload).builtin) == Builtins::RegExp;
}

static bool mayPassArgument(const BuiltinCall& call, uint32_t argumentIndex)
{
    switch (call.form) {
    case BuiltinCallForm::List:
        return true;
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
        return argumentIndex < call.arguments.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlagSets parseFlagSets(const Program& program, const SinkCandidates& candidates)
{
    FlagSets flagSets;
    // Unknown flags may be "", "g" or "d", which all find the record without significant flags.
    if (candidates.mayBeUndefined || !candidates.isComplete)
        flagSets.append(OptionSet<Yarr::Flags> { });
    Vector<char16_t> buffer;
    for (StringID text : candidates.strings) {
        std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(program.strings.viewOf(text, buffer));
        if (!flags)
            continue;
        flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
    }
    return flagSets;
}

static void reportTextKnownAtRunTime(BuildContext& context, RegExpSite site, BlockID block, SourceLocation location)
{
    context.diagnostics().add(UserDiagnostic {
        .kind = UserDiagnosticKind::RegExpWithoutCode,
        .function = site.function,
        .block = block,
        .value = site.value,
        .count = static_cast<uint32_t>(RegExpDeclineReason::TextKnownAtRunTime),
        .location = location,
    });
}

// Plain characters are searched for without code, and the run time sees that they are plain when it parses them.
static bool mayHaveCode(const RegExpSummary& summary)
{
    return summary.isValid() && summary.atom.isNull();
}

static void addCandidates(const Program& program, BuildContext& context, RegExpRequestOrigin origin, StringID pattern, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(pattern, buffer);
    for (OptionSet<Yarr::Flags> flagSet : flagSets) {
        if (mayHaveCode(context.regExpSummaries().summaryOf(pattern, flagSet, text)))
            found.append(FoundRegExp { origin, pattern, flagSet, site });
    }
}

static void addCandidatesOfFlow(const CollectionInputs& inputs, const SinkCandidates& patterns, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    const StringTable& strings = inputs.program.strings;
    auto addPattern = [&](StringID pattern) {
        addCandidates(inputs.program, inputs.context, RegExpRequestOrigin::Candidate, pattern, flagSets, site, found);
    };
    for (StringID pattern : patterns.strings)
        addPattern(pattern);
    // The record of an operand that flow knows nothing about says mayBeUndefined too.
    bool flowSawUndefined = patterns.mayBeUndefined && patterns != SinkCandidates { };
    if (flowSawUndefined && !patterns.strings.contains(strings.emptyString()))
        addPattern(strings.emptyString());
}

static void collectCandidates(const CollectionInputs& inputs, const RegExpConstructorCall& call, Vector<FoundRegExp>& found)
{
    const SinkCandidates notPassed { .strings = { }, .mayBeUndefined = true, .isComplete = true };
    auto candidatesOf = [&](uint32_t argumentIndex) -> const SinkCandidates& {
        return mayPassArgument(call.builtinCall, argumentIndex) ? inputs.flow.sinkCandidates(call.accessSite, argumentIndex) : notPassed;
    };
    const SinkCandidates& patterns = candidatesOf(0);
    const SinkCandidates& flags = candidatesOf(1);
    FlagSets flagSets = parseFlagSets(inputs.program, flags);

    Subject subject { .function = call.site.function, .block = call.block, .value = call.site.value, .location = call.location };
    // A crowd of texts at one call says little about the text that the call gets.
    bool takesTextsOfFlow = !inputs.context.exceeds(LimitID::RegExpCandidatesPerSite, patterns.strings.size(), subject);
    if (takesTextsOfFlow)
        addCandidatesOfFlow(inputs, patterns, flagSets, call.site, found);
    bool flowKnowsEveryText = takesTextsOfFlow && patterns.isComplete;
    if (flowKnowsEveryText && flags.isComplete)
        return;

    bool passesPatternAsOperand = call.builtinCall.form != BuiltinCallForm::List && !call.builtinCall.arguments.empty();
    if (flowKnowsEveryText || !passesPatternAsOperand) {
        reportTextKnownAtRunTime(inputs.context, call.site, call.block, call.location);
        return;
    }
    Locker locker { inputs.callsWithUnknownText.lock };
    ValueID flagsOperand = call.builtinCall.arguments.size() > 1 ? call.builtinCall.arguments[1] : ValueID();
    inputs.callsWithUnknownText.calls.append(CallWithUnknownText { call.site, call.block, call.location, call.builtinCall.arguments[0], flagsOperand, WTF::move(flagSets), flags.isComplete });
}

static void addCandidatesFromConstants(const Program& program, StringArena& strings, BuildContext& context, Vector<CallWithUnknownText>&& calls, Vector<FoundRegExp>& found)
{
    if (calls.isEmpty())
        return;
    std::ranges::sort(calls, { }, &CallWithUnknownText::site);
    TextsFromConstants constants(program, strings, context);
    for (const CallWithUnknownText& call : calls) {
        TextsFromConstants::Patterns known = constants.patternsOf(call);
        // What the walk has seen all of is as good as literals. Of the others the call makes some, and they share what code one call may have.
        RegExpRequestOrigin origin = known.sawEveryText || known.patterns.size() == 1 ? RegExpRequestOrigin::Evaluated : RegExpRequestOrigin::Candidate;
        for (const TextsFromConstants::Pattern& pattern : known.patterns)
            addCandidates(program, context, origin, pattern.text, FlagSets { pattern.flags }, call.site, found);
        bool knowsTexts = known.sawEveryText && !known.patterns.isEmpty();
        bool knowsFlags = known.sawAllFlags;
        auto& patterns = known.patterns;
        if (!knowsTexts || !knowsFlags)
            reportTextKnownAtRunTime(context, call.site, call.block, call.location);
    }
}

static void collectFromFunction(const CollectionInputs& inputs, FunctionID id, Vector<FoundRegExp>& found)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);

    Vector<RegExpConstructorCall, 4> calls;
    // Placement asks for the record of a literal in every block, reachable or not.
    forEachListedValue(program, id, Value::opcodesWithPattern | builtinCallOpcodes, [&](BlockID blockID, uint32_t, ValueID valueID, const Value& value) {
        bool isLiteral = Value::opcodesWithPattern.contains(value.opcode);
        if (!isLiteral && !callsRegExpConstructor(program, value.overload()))
            return;
        RegExpSite site { id, valueID, function.locationOf(valueID) };
        if (isLiteral)
            found.append(FoundRegExp { RegExpRequestOrigin::Literal, value.pattern(), ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags())), site });
        else
            calls.append(RegExpConstructorCall { builtinCallOf(program, value), value.accessSite(), site, blockID, site.location });
    });
    if (calls.isEmpty())
        return;

    CFG cfg(function);
    for (const RegExpConstructorCall& call : calls) {
        if (cfg.isReachable(call.block))
            collectCandidates(inputs, call, found);
    }
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

static void internMetadataStrings(RegExpRequest& request, const Program& program, StringArena& strings, BuildContext& context)
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

RegExpRequests collectRegExpRequests(const Program& program, const FlowResults& flow, StringArena& strings, BuildContext& context)
{
    PhaseTimes times(context, PassID::CollectRegExpLiterals);
    CallsWithUnknownText callsWithUnknownText;
    CollectionInputs inputs { program, flow, context, callsWithUnknownText };
    Vector<FoundRegExp> found = collectInParallel<FoundRegExp>(context.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundRegExp>& foundInFunction) {
        collectFromFunction(inputs, FunctionID(index), foundInFunction);
    });
    times.didFinish("collect from functions"_s);
    addCandidatesFromConstants(program, strings, context, WTF::move(callsWithUnknownText.calls), found);
    times.didFinish("addCandidatesFromConstants"_s);
    parallelSort(context.parallelLoops(), found.mutableSpan(), [](const FoundRegExp& a, const FoundRegExp& b) {
        return orderKey(a) < orderKey(b);
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
    std::ranges::sort(candidates, { }, keyOf);

    for (RegExpRequest& request : literals)
        internMetadataStrings(request, program, strings, context);
    for (RegExpRequest& request : candidates)
        internMetadataStrings(request, program, strings, context);

    context.statistics().countChanges(PassID::CollectRegExpLiterals, literals.size() + candidates.size());
    times.didFinish("requests and their strings"_s);
    return RegExpRequests(WTF::move(literals), WTF::move(candidates));
}

static ASCIILiteral originName(RegExpRequestOrigin origin)
{
    switch (origin) {
    case RegExpRequestOrigin::Literal:
        return "literal"_s;
    case RegExpRequestOrigin::Evaluated:
        return "evaluated"_s;
    case RegExpRequestOrigin::Candidate:
        return "candidate"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RegExpRequests::dump(PrintStream& out, const Program& program) const
{
    out.print("regexp requests: ", m_requests.size(), "\n");
    for (size_t index = 0; index < m_requests.size(); ++index) {
        const RegExpRequest& request = m_requests[index];
        out.print("  ", index, " ", originName(request.origin), " ");
        printString(out, program, request.pattern);
        Yarr::FlagsString characters = Yarr::flagsString(request.flags);
        out.print(" flags=");
        if (request.flags.isEmpty())
            out.print("-");
        else
            out.print(StringView(characters.span()));
        out.print(" sites=");
        CommaPrinter comma;
        for (const RegExpSite& site : request.sites)
            out.print(comma, "f", site.function.index(), ":v", site.value.index());
        out.print("\n");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
