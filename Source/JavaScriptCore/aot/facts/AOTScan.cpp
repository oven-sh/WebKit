/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThinSummary.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTIterationHelpers.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTLinkStrings.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTScanKnownEffects.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSchemaResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ListInScan {
    uint32_t first { 0 };
    uint32_t count { 0 };

    friend constexpr bool operator==(const ListInScan&, const ListInScan&) = default;
};

struct OriginInScan {
    uint32_t node { 0 };
    TypeID passes;
};

struct IncomingInScan {
    uint32_t block { 0 };
    uint32_t position { 0 };
    ListInScan list;
    TypeID passes;
};

enum class SinkKindInScan : uint8_t {
    Edge,
    Argument,
    SubjectOfCall,
    ReceiverOfCall,
    NewTargetOfCall,
    SubjectOfAccess,
    ReceiverOfAccess,
    KeyOfAccess,
    ValueOfAccess,
    SubjectOfChange,
    OtherOfChange,
    ValueOfMember,
    ConvertedToPrimitive,
    SubjectOfInspection,
    GetterInSightOfChange,
    SetterInSightOfChange,
    ValueInSightOfChange,
};

struct SinkInScan {
    SinkKindInScan kind { SinkKindInScan::Edge };
    ListInScan value;
    uint32_t target { 0 };
    TypeID passes;
    bool runsCodeOfValue { false };
};

struct StoreInScan {
    ValueID site;
    bool isDefinition { false };
    ListInScan base;
    ListInScan value;
    PropertyKeyID key;
    uint32_t root { 0 };
    uint32_t block { 0 };
    bool storesObjectMadeForIt { false };
    bool isIntoObjectKnownByRead { false };
};

struct AccessorLaidInScan {
    uint32_t root { 0 };
    PropertyKeyID key;
    FunctionID function;
    bool isGetter { false };
};

struct OperatorInScan {
    static constexpr uint32_t noRecord = UINT32_MAX;

    ValueID site;
    Opcode opcode { Opcode::Add };
    bool isNeeded { false };
    uint32_t numberOfOperands { 0 };
    ListInScan operands[SummaryOperator::mostOperands];
    uint32_t record { noRecord };
    uint32_t joinOfResult { 0 };
};

struct FillInScan {
    uint32_t root { 0 };
    bool isPart { false };
    uint32_t number { 0 };

    friend constexpr auto operator<=>(const FillInScan&, const FillInScan&) = default;
};

class FillsAtPointInScan {
public:
    void clear()
    {
        m_fills.shrink(0);
        m_firstsOfRuns.shrink(0);
    }

    void set(std::span<const FillInScan> inOrder)
    {
        clear();
        m_fills.append(inOrder);
    }

    bool contains(const FillInScan& fill) const
    {
        size_t end = m_fills.size();
        for (size_t run = m_firstsOfRuns.size(); run--; end = m_firstsOfRuns[run]) {
            if (std::ranges::binary_search(m_fills.span().subspan(m_firstsOfRuns[run], end - m_firstsOfRuns[run]), fill))
                return true;
        }
        return std::ranges::binary_search(m_fills.span().first(end), fill);
    }

    void add(const FillInScan& fill)
    {
        if (contains(fill))
            return;
        size_t firstOfLastRun = m_firstsOfRuns.isEmpty() ? 0 : m_firstsOfRuns.last();
        if (m_fills.size() - firstOfLastRun >= longestRunThatTakesFill) {
            m_firstsOfRuns.append(m_fills.size());
            m_fills.append(fill);
            return;
        }
        std::span<const FillInScan> lastRun = m_fills.span().subspan(firstOfLastRun);
        m_fills.insert(firstOfLastRun + (std::ranges::lower_bound(lastRun, fill) - lastRun.begin()), fill);
        while (!m_firstsOfRuns.isEmpty() && m_fills.size() - m_firstsOfRuns.last() >= m_firstsOfRuns.last() - firstOfRunInFrontOfLast())
            uniteLastTwoRuns();
    }

    std::span<const FillInScan> inOrder() LIFETIME_BOUND
    {
        while (!m_firstsOfRuns.isEmpty())
            uniteLastTwoRuns();
        return m_fills.span();
    }

private:
    static constexpr size_t longestRunThatTakesFill = 64;

    size_t firstOfRunInFrontOfLast() const { return m_firstsOfRuns.size() > 1 ? m_firstsOfRuns[m_firstsOfRuns.size() - 2] : 0; }
    void uniteLastTwoRuns()
    {
        std::inplace_merge(m_fills.begin() + firstOfRunInFrontOfLast(), m_fills.begin() + m_firstsOfRuns.last(), m_fills.end());
        m_firstsOfRuns.removeLast();
    }

    Vector<FillInScan, 16> m_fills;
    Vector<size_t, 4> m_firstsOfRuns;
};

enum class ObjectBeingMadeKindInScan : uint8_t { OfOneLayout, Allocated, AnsweredBySuperCall, This };

struct ObjectBeingMadeInScan {
    uint32_t root { 0 };
    ObjectBeingMadeKindInScan kind { ObjectBeingMadeKindInScan::OfOneLayout };
    bool hasLeft { false };
    bool isGivenUp { false };
    Vector<FillInScan, 8> filled;
    bool hasEnded { false };
    Vector<FillInScan, 8> filledAtEnd;
    Vector<PropertyKeyID, 8> keysLookedUp;
    Vector<FunctionID, 1> parts;
    Vector<uint32_t, 1> parametersHandedTo;
    Vector<PropertyKeyID, 4> definedInTurn;
};

struct ObjectHandedToBlockInScan {
    uint32_t block { 0 };
    uint32_t position { 0 };
    uint32_t root { 0 };
    uint32_t placeInStream { 0 };
    ListInScan fills;
};

struct BlockThatTookObjectInScan {
    uint32_t block { 0 };
    uint32_t root { 0 };
};

enum class WayOfLeavingInScan : uint8_t { IsSeenFromOutside, IsReturned };
enum class KindOfEdgeInScan : uint8_t { Plain, ToHandler };

enum class FirstValueOfSlotInScan : uint8_t { IsEmpty, IsUndefined, IsListed };

struct SlotOfEnvironmentInScan {
    FirstValueOfSlotInScan firstValue { FirstValueOfSlotInScan::IsEmpty };
    bool isSeenAsAtFirst { false };
    bool isRead { false };
    bool holdsClosureOverEnvironment { false };
    uint32_t firstStore { UINT32_MAX };
};

struct EnvironmentBeingMadeInScan {
    uint32_t root { 0 };
    LayoutID layout;
    uint32_t firstSlot { 0 };
    uint32_t numberOfSlots { 0 };
    uint32_t blockOfLeaving { UINT32_MAX };
    uint32_t positionOfLeaving { 0 };
    bool isGivenUp { false };
};

struct ValueOverEnvironmentInScan {
    uint32_t value { 0 };
    uint32_t environment { 0 };
};

struct EdgeLeadingBackInScan {
    uint32_t head { 0 };
    uint32_t last { 0 };
};

struct FrameInScan {
    uint32_t parameter { 0 };
    uint32_t incoming { 0 };
    uint32_t atom { 0 };
};

struct MadeByOperationInScan {
    BuiltinClassID builtinClass;
    bool isOpaque { false };
};

struct RulesOfWhereResultIsReadFromInScan {
    using Result = std::optional<uint32_t>;

    Result none(auto&&...) { return std::nullopt; }
    Result unknown(auto&&...) { return std::nullopt; }
    Result constant(auto&&...) { return std::nullopt; }
    Result natural(auto&&...) { return std::nullopt; }
    Result sameAs(auto&&...) { return std::nullopt; }
    Result cast(auto&&...) { return std::nullopt; }
    Result allocation(auto&&...) { return std::nullopt; }
    Result declared(auto&&...) { return std::nullopt; }
    Result declaredContents(const BodyValue&, uint32_t operandIndex, auto&&...) { return operandIndex; }
    Result declaredResult(auto&&...) { return std::nullopt; }
    Result declaredParameter(auto&&...) { return std::nullopt; }
    Result resultOperand(auto&&...) { return std::nullopt; }
};

struct RulesOfWhatIsMadeInScan {
    using Result = MadeByOperationInScan;

    Result none(auto&&...) { return { }; }
    Result unknown(auto&&...) { return { }; }
    Result constant(auto&&...) { return { }; }
    Result natural(const BodyValue&, const NaturalType& written) { return { { }, written.opaque.has_value() }; }
    Result natural(const BodyValue&, NaturalTable) { return { }; }
    Result sameAs(auto&&...) { return { }; }
    Result cast(auto&&...) { return { }; }
    Result allocation(const BodyValue&, LayoutID) { return { }; }
    Result allocation(const BodyValue&, BuiltinLayoutID) { return { }; }
    Result allocation(const BodyValue&, BuiltinClassID builtinClass, auto&&...) { return { builtinClass, false }; }
    Result declared(auto&&...) { return { }; }
    Result declaredContents(auto&&...) { return { }; }
    Result declaredResult(auto&&...) { return { }; }
    Result declaredParameter(auto&&...) { return { }; }
    Result resultOperand(auto&&...) { return { }; }
};

struct ChangeOfBuiltinInScan {
    BuiltinPropertyActionKind kind;
    BuiltinOperand object;
    std::optional<BuiltinOperand> other;
    std::optional<BuiltinOperand> key;

    friend bool operator==(const ChangeOfBuiltinInScan&, const ChangeOfBuiltinInScan&) = default;
};

struct DefinitionByMapInScan {
    uint32_t change { 0 };
    uint32_t access { 0 };
    uint32_t rootOfMap { 0 };
    uint32_t position { 0 };
};

struct DefinitionByDescriptorInScan {
    uint32_t change { 0 };
    uint32_t rootOfDescriptor { 0 };
    uint32_t position { 0 };
    bool waitsForItsJudgment { false };
    ListInScan descriptor;
    static constexpr uint32_t noDefinitionUnderName = UINT32_MAX;
    uint32_t access { noDefinitionUnderName };
};

struct StoreOfPrototypeInScan {
    uint32_t store { 0 };
    uint32_t rootOfStored { 0 };
};

struct HeirMadeByBuiltinInScan {
    uint32_t call { 0 };
    uint32_t change { 0 };
    uint32_t sinkOfSubject { 0 };
    ListInScan above;
    bool isMadeEmpty { false };
    bool isFollowed { false };
};

} // anonymous namespace

static constexpr OpcodeSet opcodesOfArgumentsObjectsInScan { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument };
static constexpr OpcodeSet opcodesOfSuperCallsInScan { Opcode::SuperConstruct, Opcode::SuperConstructList };

static TypeID typeOfPrimitivesInScan(uint16_t primitives, Range integers)
{
    if (std::optional<TypeID> type = tryMakeTypeOfPrimitives(primitives, integers))
        return *type;
    std::optional<TypeID> widened = tryMakeTypeOfPrimitives(primitives, { widenedMinimum(integers.minimum), widenedMaximum(integers.maximum) });
    RELEASE_ASSERT(widened);
    return *widened;
}

static TypeID unionOfPrimitiveTypesInScan(TypeID first, TypeID second)
{
    return typeOfPrimitivesInScan(static_cast<uint16_t>(primitivesInID(first) | primitivesInID(second)), integersInID(first).unionWith(integersInID(second)));
}

static TypeID typeOfIntegerInScan(int64_t integer)
{
    return typeOfPrimitivesInScan(Primitive::Integer, { widenedMinimum(integer), widenedMaximum(integer) });
}

static TypeID typeOfNumberInScan(double number)
{
    PrimitiveSet primitive = primitiveOfNumber(number);
    return primitive == PrimitiveSet::Integer ? typeOfIntegerInScan(static_cast<int64_t>(number)) : typeOfPrimitives(static_cast<uint16_t>(primitive));
}

static TypeID typeOfPrimitivesWrittenInScan(const NaturalType& written)
{
    uint16_t primitives = fromWire(written.primitives);
    Range integers = primitives & Primitive::Integer ? Range::full() : Range::empty();
    if (written.integers) {
        primitives |= Primitive::Integer;
        integers = integers.unionWith({ written.integers->minimum, written.integers->maximum });
    }
    return primitives ? typeOfPrimitivesInScan(primitives, integers) : TypeID();
}

static TypeID typeOfPrimitiveConstantInScan(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::Undefined)
        return TypeTable::undefined;
    if (opcode == Opcode::Null)
        return TypeTable::null;
    if (opcode == Opcode::Absent)
        return TypeTable::absent;
    if (opcode == Opcode::Boolean)
        return typeOfPrimitives(value.as<Opcode::Boolean>().boolean() ? Primitive::True : Primitive::False);
    if (opcode == Opcode::Integer)
        return typeOfIntegerInScan(value.as<Opcode::Integer>().integer());
    if (opcode == Opcode::Double)
        return typeOfNumberInScan(value.as<Opcode::Double>().number());
    if (opcode == Opcode::String)
        return TypeTable::string;
    if (opcode == Opcode::BigInt)
        return TypeTable::bigInt;
    return { };
}

static bool resultFollowsOperandsInScan(Opcode opcode, TypeID widest)
{
    if (flowRuleOf(opcode) == FlowRule::ToPropertyKey)
        return true;
    return (primitivesInID(widest) & Primitive::Number) && PayloadWord::form(opcode) && numberOfNamedOperandsOf(opcode) <= SummaryOperator::mostOperands;
}

static bool stepsSynchronousIteratorInScan(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::IteratorResume)
        return value.as<Opcode::IteratorResume>().iteratorKind() == IteratorKind::Sync;
    if (opcode == Opcode::IteratorClose)
        return value.as<Opcode::IteratorClose>().iteratorKind() == IteratorKind::Sync;
    return false;
}

static std::optional<SummaryAccess::Kind> accessMadeByInScan(Opcode opcode)
{
    if (accessClassOf(opcode) != AccessClass::ByName || !receiverOf(opcode))
        return std::nullopt;
    switch (accessOf(opcode)) {
    case Access::Read:
        return SummaryAccess::Kind::Read;
    case Access::Store:
        return SummaryAccess::Kind::Store;
    case Access::Define:
        return SummaryAccess::Kind::Define;
    case Access::Has:
        return SummaryAccess::Kind::Has;
    case Access::None:
        break;
    }
    switch (interpretedOf(opcode)) {
    case Interpretation::Read:
        return SummaryAccess::Kind::Read;
    case Interpretation::Test:
        return SummaryAccess::Kind::Has;
    case Interpretation::Store:
        return opcode == Opcode::ElemDefine ? SummaryAccess::Kind::Define : SummaryAccess::Kind::Store;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr Effects noEffectsInScan = Effects::none();

static constexpr Effects computeEffectsOfAccessItselfInScan(bool writesProperties)
{
    LocationMasks outsideCompiledCode = LocationMasks::top();
    outsideCompiledCode.remove(compiledCodeHeaps);

    Effects effects = Effects::none();
    effects.reads = outsideCompiledCode;
    effects.mayThrow = true;
    effects.mayCollect = true;
    if (writesProperties)
        effects.writes = outsideCompiledCode;
    else
        effects.writes.add(heapsWhoseWritesNobodySees);
    return effects;
}

static const Effects& effectsOfAccessItselfInScan(SummaryAccess::Kind kind)
{
    static constexpr Effects effectsOfRead = computeEffectsOfAccessItselfInScan(false);
    static constexpr Effects effectsOfStore = computeEffectsOfAccessItselfInScan(true);
    switch (kind) {
    case SummaryAccess::Kind::Read:
    case SummaryAccess::Kind::Has:
        return effectsOfRead;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        return effectsOfStore;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool namesWhatNobodyReplacesInScan(const BuiltinTable& builtins, BuiltinID named)
{
    const Builtin& entry = builtins.at(named);
    return entry.mutability == BuiltinMutability::AlwaysLocked || builtins.isLocationOfLockedObject(named) || !entry.holder;
}

static BuiltinClassID classOfCoroutineResultInScan(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Promise;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

static bool loaderEntersBodyOnceInScan(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.isModuleBody() || !function.module)
        return false;
    const Module& module = program.at(function.module);
    if (module.body != id || !isEvaluatedAtMostOnceOf(module.kind) || module.flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked }))
        return false;
    return std::ranges::count(program.modules, id, &Module::body) == 1;
}

PropertyKeyID keyOfSymbolThatCallMakes(const Program& program, FunctionID function, const BodyValue& value, const ScopedLambda<BodyValue(ValueID)>& definitionOf)
{
    if (value.opcode() != Opcode::Call)
        return { };
    auto call = value.as<Opcode::Call>();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() > 1)
        return { };

    auto definitionInFrontOfCasts = [&](ValueID id) {
        BodyValue definition = id ? definitionOf(id) : BodyValue();
        while (definition && opcodesThatStateTypes.contains(definition.opcode())) {
            ValueID operand = definition.operand(0);
            definition = operand ? definitionOf(operand) : BodyValue();
        }
        return definition;
    };
    BodyValue callee = definitionInFrontOfCasts(call.calleeOperand());
    if (!callee || callee.opcode() != Opcode::Builtin)
        return { };
    auto named = callee.as<Opcode::Builtin>();
    if (!named.isNotReadFromHolder() && !namesWhatNobodyReplacesInScan(program.builtins(), named.builtin()))
        return { };
    BuiltinID called = program.builtins().canonicalID(named.builtin());
    bool findsRegisteredSymbol = called == Builtins::SymbolFor;
    if (findsRegisteredSymbol ? arguments.empty() : called != Builtins::Symbol || !call.callNumber())
        return { };
    StringID description;
    if (!arguments.empty()) {
        BodyValue text = definitionInFrontOfCasts(arguments[0]);
        if (!text || text.opcode() != Opcode::String)
            return { };
        description = text.as<Opcode::String>().string();
    }

    if (findsRegisteredSymbol) {
        std::optional<PropertyKeyID> registered = program.types.find(PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = description });
        return registered && program.types.hasStableNumber(*registered) ? *registered : PropertyKeyID();
    }

    std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::CreatedSymbol, .text = description, .module = program.at(function).module, .callNumber = call.callNumber() });
    if (!key || !program.types.hasStableNumber(*key) || !loaderEntersBodyOnceInScan(program, function))
        return { };
    return *key;
}

PropertyKeyID keyOfSymbolMadeOnceBy(const Program& program, const SummaryCall& call)
{
    if (call.callee != SummaryCall::Callee::Builtin || !call.key)
        return { };
    return call.isInLoop && program.at(call.key).kind != PropertyKeyKind::RegisteredSymbol ? PropertyKeyID() : call.key;
}

bool requireAnswersWithResultOfBody(const Program& program, ModuleID referrer, ModuleID target)
{
    const Module& module = program.at(target);
    if (module.kind != ModuleKind::RuntimeSource || !requireReadsModuleValue(program, module) || requireAddsToRequireCache(program, referrer, target))
        return false;
    if (module.flags.containsAny({ ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::AnswersLoaderWithNamespace }))
        return false;
    switch (module.requireResult) {
    case RequireResult::ModuleExports:
    case RequireResult::DefaultExport:
        break;
    case RequireResult::Namespace:
    case RequireResult::NamespaceIfImported:
        return false;
    }
    if (!module.body)
        return false;
    const Function& body = program.at(module.body);
    return body.hasBody() && !body.isCoroutine() && body.results.size() == 1;
}

template<typename T>
static void sortWithoutRepeatsInScan(Vector<T>& list)
{
    std::sort(list.begin(), list.end());
    list.shrink(std::unique(list.begin(), list.end()) - list.begin());
    list.shrinkToFit();
}

static BitVector namesUnderWhichEventsHangInScan(const BuiltinTable& builtins)
{
    BitVector names;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (entry.holder && entry.name && callOrReadOfBuiltinIsEvent(builtins, builtins.canonicalID(BuiltinID(index))))
            names.set(entry.name.index());
    }
    return names;
}

class ScanOfBody {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ScanOfBody);
    WTF_MAKE_NONCOPYABLE(ScanOfBody);
public:
    ScanOfBody(const Program& program, Mechanisms mechanisms, const BitVector& textsThatNameFieldsNeverWritten, const BitVector* namesUnderWhichEventsHang = nullptr)
        : m_program(program)
        , m_namesUnderWhichEventsHang(namesUnderWhichEventsHang)
        , m_textsThatNameFieldsNeverWritten(textsThatNameFieldsNeverWritten)
        , m_walkFollowsConstants(mechanisms.contains(Mechanism::Folding))
        , m_walkFollowsTests(mechanisms.contains(Mechanism::TypedOperations))
    {
    }

    void run(FunctionID, ThinSummary&);
    std::optional<InternalError> takeError() { return std::exchange(m_error, std::nullopt); }
    void noteTextSpelled(StringID text)
    {
        if (m_textsThatNameFieldsNeverWritten.get(text.index())) [[unlikely]]
            m_textsSpelled.set(text.index());
    }
    void noteTextsSpelledIn(const DataNode&);
    const BitVector& textsSpelled() const { return m_textsSpelled; }

private:
    friend struct ResultRulesOfScan;
    friend struct ReferencesOfScan;

    static constexpr uint32_t parameterBit = 1u << 31;
    static constexpr uint32_t operatorBit = 1u << 30;
    static constexpr uint32_t noConversion = UINT32_MAX;
    static constexpr uint32_t staticSideBit = 1u << 31;
    static constexpr uint32_t isInData = 1u << 31;
    static constexpr uint32_t isResultOfCall = UINT32_MAX;
    static constexpr uint32_t isReadNotYetNeeded = UINT32_MAX - 1;
    static constexpr uint32_t noDefinition = UINT32_MAX;
    static constexpr uint32_t noComponent = UINT32_MAX;
    static constexpr uint32_t hasNotLeft = UINT32_MAX;
    static constexpr uint32_t noLoop = UINT32_MAX;
    static constexpr uint32_t initialNumberOfNodeSlots = 32;
    static constexpr uint32_t noFillsYet = UINT32_MAX;
    static constexpr uint8_t isCalledByName = 1 << 0;
    static constexpr uint8_t isUnderConstruction = 1 << 1;
    static constexpr uint8_t isFollowedWhileMade = 1 << 2;
    static constexpr uint8_t isSeenFromOutside = 1 << 3;
    static constexpr uint8_t isAccessedByName = 1 << 4;
    static constexpr uint8_t isAccessedBesideNamedStores = 1 << 7;
    static constexpr uint8_t mayComeInFrontOfDefinition = 1 << 5;
    static constexpr uint8_t readsAnswerOfSuperCall = 1 << 6;

    void summarize(FunctionID);
    void reset();
    void summarizeWithoutBody();

    uint32_t nodeFor(SummaryNode::Kind, uint32_t first = 0, uint32_t second = 0);
    void growNodeSlots();
    uint32_t constant(TypeID type) { return nodeFor(SummaryNode::Kind::Constant, type.bits()); }
    uint32_t newJoin() { return nodeFor(SummaryNode::Kind::Join, m_numberOfJoins++); }
    uint32_t classInstanceMadeBy(const BodyValue& value, BuiltinClassID builtinClass) { return nodeFor(SummaryNode::Kind::ClassInstance, builtinClass.index(), value.id().index() + 1); }
    uint32_t classInstanceInData(BuiltinClassID builtinClass) { return nodeFor(SummaryNode::Kind::ClassInstance, builtinClass.index(), isInData | m_numberOfInstancesInData++); }
    uint32_t objectOfCoroutine() { return nodeFor(SummaryNode::Kind::ClassInstance, classOfCoroutineResultInScan(*m_function).index()); }
    uint32_t contentsNode(uint32_t classInstance, SlotOfContents slot) { return nodeFor(SummaryNode::Kind::Contents, classInstance, slot.number()); }
    uint32_t captureNode(FunctionID, uint32_t capture);
    uint32_t fieldNode(LayoutID, uint32_t field);
    uint32_t dataFieldNode(LayoutID, uint32_t field);
    uint32_t variableNode(VariableID variable) { return variable ? nodeFor(SummaryNode::Kind::Variable, variable.index()) : 0; }
    uint32_t resultNode(uint32_t result) { return result < m_function->results.size() ? nodeFor(SummaryNode::Kind::Result, result) : 0; }
    uint32_t nodeOfData(const DataNode&);
    uint32_t resultOfCall(uint32_t call) { return nodeFor(SummaryNode::Kind::ResultOfCall, call, 0); }
    TypeID primitiveConstantAt(uint32_t atom) const;
    void addEdge(uint32_t from, uint32_t to, TypeID passes = { }, bool runsCodeOfValue = false);

    void noteFunctionNamed(FunctionID function)
    {
        if (function)
            m_summary.functionsNamed.append(function);
    }
    void noteFunctionsBehind(FunctionID function) { noteFunctionNamed(function); }
    void noteFunctionsBehind(VariableID variable) { noteFunctionNamed(m_program.at(variable).function); }
    void noteFunctionsBehind(ExpectedResultID expected) { noteFunctionsBehind(m_program.at(expected).type); }
    void noteFunctionsBehind(LayoutID);
    void noteFunctionsBehind(TypeID);
    template<typename OtherID> void noteFunctionsBehind(OtherID) { }

    ListInScan listOfNode(uint32_t node);
    ListInScan listOf(ValueID);
    ListInScan readByName(uint32_t read);
    void noteInspection(ValueID site, ThinSummary::Inspection::Kind, ListInScan);
    uint32_t takerOfMemberBesideFields(uint32_t object, PropertyKeyID);
    ListInScan internAtoms(Vector<uint32_t>&);
    bool holdsOnlyConstants(ListInScan) const;
    bool isAlwaysPrimitive(ListInScan) const;
    bool isAlwaysPrimitive(const OriginInScan&) const;
    uint32_t nodeOfAtom(uint32_t atom) const;

    void beginResult();
    void addList(ListInScan);
    void addNode(uint32_t node) { addList(listOfNode(node)); }
    void addOperand(const BodyValue&, uint32_t operandIndex);
    void addVariadicOperand(const BodyValue&, uint32_t place);
    void addConstant(const BodyValue&);
    void addNatural(const BodyValue&, const NaturalType&);
    bool prototypeMaySelectOtherObject(const BodyValue&, LayoutID) const;
    void addAllocation(const BodyValue&, LayoutID);
    void addDeclared(const BodyValue&);
    LayoutID layoutOfNamespaceGivenBy(const BodyValue&) const;
    void addContents(const BodyValue&, uint32_t operandIndex, std::optional<ContentsSlot>);
    void addResultAt(const BodyValue&);
    void addOperator(const BodyValue&);
    ListInScan takeResult();

    void addSink(SinkKindInScan, ListInScan, uint32_t target, TypeID passes = { });
    void flowInto(ListInScan, uint32_t node);
    void escape(ListInScan list) { flowInto(list, 0); }
    void handToCodeThatRuns(ListInScan);
    ListInScan objectsAmong(ListInScan);
    void addArgument(ValueID, bool leaves = true);
    uint32_t callMember(ValueID site, ListInScan receiver, BuiltinName, std::initializer_list<ListInScan> arguments, uint32_t block);
    uint32_t addAccess(SummaryAccess::Kind, ValueID site, ListInScan subject, PropertyKeyID);
    uint32_t addAccessUnder(SummaryAccess::Kind, ValueID site, ListInScan subject, PropertyKeyID, ValueID computedKey);
    uint32_t resultOfRead(uint32_t access);
    uint32_t addRead(ValueID site, ListInScan subject, PropertyKeyID);
    uint32_t readContents(ValueID site, ListInScan holder, SlotOfContents);
    void storeIntoContents(ValueID site, ListInScan holder, SlotOfContents, ListInScan stored);
    ListInScan settle(ValueID site, ListInScan, uint32_t block);
    bool standsRightBehindCallThatGives(const BodyValue&, ValueID operand) const;
    std::optional<SlotOfContents> slotNamedByRowOf(const BodyValue&) const;
    bool someResultIsReadOutOf(const BodyValue&, uint32_t operandIndex) const;
    void addChange(SummaryChange::Kind, ValueID site, ListInScan subject, PropertyKeyID, std::optional<ListInScan> other);
    void defineByDescriptor(ValueID site, ListInScan subject, PropertyKeyID, std::optional<ListInScan> other, bool isUnderNameThatNobodyElseNames = false);
    struct KeyWrittenInScan {
        bool isWritten { false };
        PropertyKeyID key;
    };
    KeyWrittenInScan keyWrittenAs(ValueID) const;

    BodyValue definitionOf(uint32_t value) const;
    PrimitiveKnownToFolding primitiveHeldBy(ValueID);
    BodyValue definitionBehindCasts(ValueID value) const { return definitionOf(m_roots[value.index()]); }
    void noteLeaving(ValueID, WayOfLeavingInScan = WayOfLeavingInScan::IsSeenFromOutside);
    std::optional<uint32_t> findCall(ValueID site) const;
    void readBody();
    BodyBlock readBlock(const BodyBlock&);
    void stopAtUndefinedRecord(uint32_t block, ValueID, Wire::SourceLocation, ASCIILiteral operation);
    void readEntryParameter(uint32_t parameter);
    void readArgumentsOf(uint32_t from, BodyTarget, KindOfEdgeInScan = KindOfEdgeInScan::Plain);
    void readOperation(const BodyValue&, uint32_t block);
    void readAccess(const BodyValue&, SummaryAccess::Kind, uint32_t block);
    uint32_t readSuperBase(const BodyValue&);
    void readSuperAccess(const BodyValue&);
    void readNewClass(const BodyValue&);
    void readDefineMethod(const BodyValue&);
    void readDefineAccessor(const BodyValue&, ValueID object, ValueID function, PropertyKeyID, SummaryChange::Lays);
    void readOperand(const BodyValue&, ValueID operand, uint32_t operandIndex, const OperandInfo&, uint32_t block);
    void readProtocols(const BodyValue&, uint32_t operandIndex, const OperandInfo&, ListInScan, uint32_t block);
    void readOwnValues(const BodyValue&, uint32_t operandIndex, ListInScan);
    void convertToPrimitive(ValueID site, ListInScan);
    void constructBySpecies(ValueID site, ListInScan, uint32_t block);
    ListInScan openIterator(ValueID site, ListInScan iterable, IteratorKind, uint32_t block);
    uint32_t stepIterator(ValueID site, ListInScan iterator, BuiltinName method, std::optional<ListInScan> sent, uint32_t block);
    void readIteration(const BodyValue&, const OperandInfo&, ListInScan, uint32_t block);
    void callHasInstance(const BodyValue&, uint32_t operandIndex, ListInScan constructor, uint32_t block);
    ValueID operandUsedAs(const BodyValue&, OperandUse) const;
    MadeByOperationInScan whatIsMadeBy(const BodyValue&) const;
    void retain(const BodyValue&, uint32_t operandIndex, const OperandInfo&, ListInScan);
    void retainInResult(const BodyValue&, uint32_t operandIndex, ListInScan);
    void retainInOperand(const BodyValue&, const OperandInfo&, ListInScan);
    void retainContents(const BodyValue&, ListInScan held);
    void storeInspectedValueIntoList(const BodyValue&, ListInScan list);
    void noteFieldsLeftEmpty(const BodyValue&);
    TypeID typeOfEmptySlotOfEnvironment(LayoutID, uint32_t field) const;
    bool followsEnvironmentsOfBody() const;
    bool isFirstValueThatMayGoUnseen(const BodyValue& allocation, ValueID listed) const;
    EnvironmentBeingMadeInScan* environmentBeingMade(uint32_t root);
    void followEnvironment(const BodyValue&);
    uint32_t nearestBlockAboveBothForEnvironments(uint32_t first, uint32_t second) const;
    void noteEdgeForEnvironments(uint32_t from, uint32_t to, KindOfEdgeInScan);
    void enterBlockForEnvironments(uint32_t block);
    void noteEnvironmentLeaves(EnvironmentBeingMadeInScan&);
    void noteValueStandsOverEnvironment(uint32_t value, uint32_t environment);
    void noteLeavingOverEnvironments(uint32_t root);
    void beginOperationOverEnvironments(const BodyValue&);
    void noteStoreIntoEnvironment();
    void endOperationsOverEnvironments();
    void letFirstValuesOfSlotsArrive();
    void readCall(const BodyValue&, uint32_t block);
    void readDirectCall(const BodyValue&, SummaryCall&);
    void readCallByValue(const BodyValue&, SummaryCall&, uint32_t callIndex);
    void readChangesMadeByBuiltin(const BodyValue&, const SummaryCall&, uint32_t callIndex, ValueID receiver);
    void noteEvaluationOf(ModuleID, ValueID site, uint32_t block);
    bool answersWithResultOfBody(const BodyValue&) const;
    void noteRecordOf(ModuleID);
    bool listsFirstStoresOfBody() const;
    void lookForFirstStoresAndUses(const BodyValue&);
    void noteUseByBodyOfModule(ValueID);
    void noteUseByBodyOfModule(FunctionID);
    uint32_t nearestBlockAboveBoth(uint32_t, uint32_t) const;
    void noteEdgeOfBodyOfModule(uint32_t from, uint32_t to, KindOfEdgeInScan);
    void listFirstStoresAndUses();
    bool listsDefinitions() const;
    void lookForDefinitions(const BodyValue&);
    void noteTakerOfValueMadeForVariable(ValueID taken);
    void listDefinitions();
    void noteModuleHandedOut(ModuleID module) { m_summary.modulesHandedOut.append(module); }
    void readTerminator(const BodyTerminator&, uint32_t block);
    void returnValue(ValueID, uint32_t block);
    void readNamesThatLoaderTakesOff(ValueID returned);
    void returnFromCoroutine(ListInScan, uint32_t block);

    ObjectBeingMadeInScan& objectBeingMade(uint32_t root);
    void follow(uint32_t root, ObjectBeingMadeKindInScan);
    void followThis(const BodyBlock& entry);
    LayoutID layoutOfAllocation(const BodyValue&) const;
    void followAllocation(const BodyValue&);
    void leaveEveryFieldEmpty(LayoutID);
    bool isListedByAllocation(uint32_t root, PropertyKeyID) const;
    std::span<const FillInScan> fillsOf(uint32_t root, std::span<const FillInScan>) const;
    void noteFill(uint32_t root, bool isPart, uint32_t number);
    void noteLookUp(uint32_t root, PropertyKeyID);
    void noteStoreIntoObject(uint32_t root, PropertyKeyID, SummaryAccess::Kind);
    bool readMayComeInFrontOfDefinition(uint32_t root, PropertyKeyID);
    void meetFillsWhereObjectLeaves(ObjectBeingMadeInScan&, std::span<const FillInScan> fillsOfObject);
    void meetFillsWhereConstructionEnds(uint32_t root);
    void listBirths();
    void noteFieldLeftEmptyForGood(FieldReference);
    void listHalvesOfAccessorsNotLaid();
    bool m_leavesFieldsEmptyForGood { false };
    bool hasFilledInFrontOfEveryLeaving(uint32_t root, PropertyKeyID);
    void giveUp(uint32_t root);
    void handToBlock(ValueID, uint32_t from, uint32_t block, uint32_t position, KindOfEdgeInScan);
    void takeObjectsHandedTo(const BodyBlock&);
    void carryFillsOver(uint32_t from, uint32_t to);
    void listConstructions();
    void listDefinitionsBehindAllocation(const ObjectBeingMadeInScan&);

    void readWhatNobodyAskedFor();
    void findLoops();
    bool isStillAlone(uint32_t root, uint32_t position, uint32_t block) const;
    std::optional<uint32_t> parameterTakingIncoming(const IncomingInScan&) const;
    void sortIncomingByParameter();
    void markParametersThatSinksNeed();
    void resolveParameters();
    void resolveComponent(std::span<const uint32_t> members, uint32_t component);
    OriginInScan behindTest(OriginInScan, TypeID passes);
    void normalize(Vector<OriginInScan>&);
    void expand(ListInScan);
    uint32_t oneNodeFor(ListInScan);
    void applySinks();
    void applyConversionToPrimitive(const SinkInScan&);
    void applyOperator(const OperatorInScan&);
    void applyStore(const StoreInScan&);
    void nameKeysOfDefinitionsByMaps();
    void nameGettersOfDescriptors();
    void layDescriptorsInSight();
    void nameAccessorWrittenInDescriptor(uint32_t access, uint32_t rootOfDescriptor, uint32_t positionOfDefinition);
    std::span<const uint32_t> storesInto(uint32_t root);
    void followObjectsMadeToBePrototypes();
    void finish();
    void noteHandlerOf(const BodyBlock&);
    uint32_t nodeCaughtBy(BlockID handler);
    void settleWhereRecordsThrow();
    void removeRepeatedEdges();
    void listNamesSpelled();

    void noteWritesIn(uint32_t block, const LocationMasks&);
    void noteObjectsWrittenBy(const BodyValue&, const Effects&);
    const Effects& noteOwnEffectsOfOperator(const BodyValue&, uint32_t block, const Effects&, Effects& storage);
    const Effects& noteOwnEffects(const BodyValue&, uint32_t block, std::optional<SummaryAccess::Kind>, const Effects&, Effects& storage);
    void noteEndOfBlock(const BodyTerminator&, uint32_t block, const Effects&);
    void noteEdgeBetweenBlocks(uint32_t from, uint32_t to, KindOfEdgeInScan);
    void noteStoreThatBecameEdge(const StoreInScan&);
    void listOperandsOfOperatorsLeftApart();
    void tellWaysToReturnApart();
    void finishKnownEffects();
    OriginInScan narrowerOfTwoTests(OriginInScan, TypeID passes);

    const Program& m_program;
    const BitVector* m_namesUnderWhichEventsHang;
    BitVector m_namesListedByThisScan;
    const BitVector& m_textsThatNameFieldsNeverWritten;
    BitVector m_textsSpelled;
    const bool m_walkFollowsConstants;
    const bool m_walkFollowsTests;
    FunctionID m_id;
    const Function* m_function { nullptr };
    ThinSummary m_summary;
    bool m_hasStopped { false };
    std::optional<InternalError> m_error;

    Vector<uint32_t> m_nodeSlots;
    Vector<ListInScan> m_listsOfNodes;
    uint32_t m_numberOfJoins { 0 };
    uint32_t m_numberOfInstancesInData { 0 };

    Vector<uint32_t> m_atoms;
    Vector<ListInScan> m_lists;
    Vector<uint32_t> m_wordOffsets;
    Vector<uint32_t> m_blocks;
    Vector<uint32_t> m_roots;
    Vector<uint8_t> m_uses;
    Vector<uint32_t> m_firstLeft;
    Vector<uint32_t> m_lastLeft;
    uint32_t m_position { 0 };

    Vector<uint32_t> m_result;
    ListInScan m_onlySourceOfResult;
    uint32_t m_numberOfSourcesOfResult { 0 };
    PropertyKeyID m_keyOfOperation;
    uint32_t m_nodeReadOffOperand { 0 };
    std::optional<uint32_t> m_operandSettled;
    ListInScan m_whatOperandSettlesTo;
    bool m_resultLeaves { false };
    Vector<ListInScan, 2> m_sourcesCopiedIntoResult;
    std::optional<uint32_t> m_objectCopiedInto;
    std::optional<ListInScan> m_prototypeOfResult;
    Vector<bool> m_isConvertedToPrimitive;
    Vector<uint32_t> m_firstCallOfConversion;
    Vector<OperatorInScan> m_operators;
    Vector<uint32_t> m_joinsOfPrimitives;

    bool m_followsConstants { false };
    bool m_followsTests { false };
    bool m_hasEnteredBlockPassedOver { false };
    struct PrimitiveAskedInScan {
        uint32_t stamp { 0 };
        PrimitiveKnownToFolding known;
    };
    Vector<PrimitiveAskedInScan> m_primitivesAsked;
    uint32_t m_stampOfBody { 0 };
    Vector<bool> m_isEntered;

    Vector<uint32_t> m_firstParameterOfBlock;
    uint32_t m_numberOfParameters { 0 };
    Vector<IncomingInScan> m_incoming;
    Vector<SinkInScan> m_sinks;
    Vector<StoreInScan> m_stores;
    Vector<AccessorLaidInScan> m_accessorsLaid;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAccessorsLaid;
    bool hasAccessorLaidUnder(uint32_t root, PropertyKeyID key) const { return !m_accessorsLaid.isEmpty() && m_placesOfAccessorsLaid.contains(static_cast<uint64_t>(root) << 32 | key.bits()); }
    Vector<uint32_t> m_storesByRoot;
    bool m_storesAreSortedByRoot { false };
    Vector<uint32_t> m_readsNotYetNeeded;
    Vector<std::pair<ListInScan, uint32_t>> m_returnsOfCoroutine;
    Vector<EdgeLeadingBackInScan> m_edgesLeadingBack;
    bool m_listsDefinitions { false };
    struct ValueMadeForVariableInScan {
        uint32_t made;
        uint32_t store { noDefinition };
        VariableID variable;
        bool isTakenByOthers { false };
    };
    Vector<ValueMadeForVariableInScan> m_valuesMadeForVariables;
    bool m_listsFirstStoresOfBody { false };
    uint32_t m_blockBeingRead { 0 };
    Vector<uint32_t> m_immediateDominators;
    Vector<uint32_t> m_endsOfBlocks;
    uint32_t m_blockAboveReturns { noDefinition };
    struct PlaceInBlockInScan {
        uint32_t number;
        uint32_t position;
        uint32_t block;
    };
    Vector<PlaceInBlockInScan> m_storesOfBodyOfModule;
    Vector<PlaceInBlockInScan> m_readsOfBodyOfModule;
    Vector<PlaceInBlockInScan> m_usesByBodyOfModule;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_readsNotedOfBodyOfModule;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_usesNotedOfBodyOfModule;
    Vector<uint32_t> m_blocksOfCalls;
    Vector<BlockID> m_handlersOfBlocks;
    Vector<uint32_t> m_nodesCaughtByBlocks;
    Vector<bool> m_fieldIsListed;
    Vector<ChangeOfBuiltinInScan, 4> m_changesOfBuiltin;
    Vector<DefinitionByMapInScan, 4> m_definitionsByMaps;
    Vector<DefinitionByDescriptorInScan, 4> m_definitionsByDescriptors;
    Vector<StoreOfPrototypeInScan, 4> m_storesOfPrototypes;
    Vector<HeirMadeByBuiltinInScan, 4> m_heirsMadeByBuiltins;

    Vector<ObjectBeingMadeInScan> m_objectsBeingMade;
    uint32_t m_thisBeingMade { noDefinition };
    FillsAtPointInScan m_fills;
    Vector<FillInScan> m_fillsKept;
    Vector<ListInScan> m_fillsAtBlocks;
    Vector<uint32_t> m_numberOfEdgesInto;
    Vector<ObjectHandedToBlockInScan> m_objectsHandedToBlocks;
    Vector<BlockThatTookObjectInScan> m_blocksThatTookObjects;
    bool m_followsSomeEnvironment { false };
    bool m_hasGivenUpEnvironments { false };
    Vector<EnvironmentBeingMadeInScan, 4> m_environmentsBeingMade;
    Vector<SlotOfEnvironmentInScan> m_slotsOfEnvironments;
    Vector<ValueOverEnvironmentInScan, 8> m_valuesOverEnvironments;
    BitVector m_hasToDoWithEnvironment;
    Vector<uint32_t> m_blocksAbove;
    Vector<uint32_t> m_blocksAboveBlockBeingRead;
    Vector<bool> m_isAboveBlockBeingRead;
    Vector<uint32_t> m_blocksOnWayUp;
    Vector<uint32_t> m_environmentsWhoseLeavingLiesBelow;
    uint32_t m_closureBeingMade { noDefinition };
    uint32_t m_environmentStoredInto { noDefinition };
    uint32_t m_slotStoredInto { 0 };
    Vector<SummaryConstruction::Key> m_keysOfConstruction;

    Vector<EdgeLeadingBackInScan> m_openLoops;
    Vector<uint32_t> m_innermostLoopHeads;
    Vector<uint32_t> m_firstIncoming;
    Vector<uint32_t> m_incomingByParameter;
    Vector<bool> m_isNeeded;
    Vector<uint32_t> m_worklist;
    Vector<uint32_t> m_order;
    Vector<uint32_t> m_lowest;
    Vector<uint32_t> m_componentOf;
    Vector<uint32_t> m_open;
    Vector<FrameInScan> m_frames;
    Vector<OriginInScan> m_origins;
    Vector<ListInScan> m_resolved;
    Vector<OriginInScan> m_gathered;
    Vector<OriginInScan> m_expanded;
    Vector<uint32_t> m_nodesOfLists;
    Vector<uint32_t> m_fieldsStoredInto;
    Vector<ValueID> m_sitesOfRows;
    KnownEffectsOfScan m_knownEffects;
};

struct ResultRulesOfScan {
    using Result = void;

    void none(const BodyValue&) { }
    void unknown(const BodyValue&) { scan.addNode(0); }
    void constant(const BodyValue& value) { scan.addConstant(value); }
    void natural(const BodyValue& value, const NaturalType& written) { scan.addNatural(value, written); }
    void natural(const BodyValue& value, NaturalTable table)
    {
        Vector<OperandClass, 2> classes;
        classes.fill(OperandClass::Any, numberOfOperandsOf(table));
        scan.addNatural(value, naturalTypeOf(table, classes.span()));
    }
    void sameAs(const BodyValue& value, uint32_t operandIndex) { scan.addOperand(value, operandIndex); }
    void cast(const BodyValue& value, uint32_t operandIndex, auto&&...) { scan.addOperand(value, operandIndex); }
    void allocation(const BodyValue& value, LayoutID layout) { scan.addAllocation(value, layout); }
    void allocation(const BodyValue& value, BuiltinLayoutID layout) { scan.addAllocation(value, scan.m_program.layoutOf(layout)); }
    void allocation(const BodyValue& value, BuiltinClassID builtinClass, auto&&...) { scan.addNode(scan.classInstanceMadeBy(value, builtinClass)); }
    void declared(const BodyValue& value, auto&&...) { scan.addDeclared(value); }
    void declaredContents(const BodyValue& value, uint32_t operandIndex, std::optional<ContentsSlot> slot) { scan.addContents(value, operandIndex, slot); }
    void declaredResult(const BodyValue& value, auto&&...) { scan.addResultAt(value); }
    void declaredParameter(const BodyValue&) { }
    void resultOperand(const BodyValue& value, uint32_t resultIndex) { scan.addVariadicOperand(value, resultIndex); }

    ScanOfBody& scan;
};

struct ReferencesOfScan {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if (!id) {
            namesUndefinedRecord = true;
            return;
        }
        scan.noteFunctionsBehind(id);
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            scan.noteRecordOf(scan.m_program.at(id).module);
            if (!isNewClosure)
                scan.noteUseByBodyOfModule(id);
            if (((role == FunctionReferenceKind::Closure) || ...) && !isNewClosure)
                scan.addEdge(scan.nodeFor(SummaryNode::Kind::Closure, id.index()), 0, { }, true);
        } else if constexpr (std::is_same_v<IDType, VariableID>)
            scan.noteRecordOf(scan.m_program.at(id).module);
        else if constexpr (std::is_same_v<IDType, ModuleID>) {
            if (((role == ModuleReferenceKind::Target) || ...)) {
                scan.noteFunctionNamed(scan.m_program.at(id).body);
                scan.noteEvaluationOf(id, site, block);
                if (handsModuleOut)
                    scan.noteModuleHandedOut(id);
            }
        }
    }

    ScanOfBody& scan;
    ValueID site;
    uint32_t block;
    bool isNewClosure;
    bool handsModuleOut;
    bool namesUndefinedRecord { false };
};

struct ModulesNamedWhereNoRunComesInScan {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, ModuleID>) {
            if (id && ((role == ModuleReferenceKind::Target) || ...))
                modules.append(id);
        }
    }

    Vector<ModuleID>& modules;
};

void ScanOfBody::noteFunctionsBehind(LayoutID layout)
{
    noteFunctionNamed(m_program.at(layout).constructor);
    noteFunctionNamed(m_program.at(declaredLayoutOf(m_program, layout)).constructor);
}

void ScanOfBody::noteFunctionsBehind(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return;
    TypeReader types(m_program);
    forEachFunctionOf(types, types.at(type), [&](FunctionID function) {
        noteFunctionNamed(function);
    });
}

void ScanOfBody::run(FunctionID id, ThinSummary& result)
{
    summarize(id);
    result = WTF::move(m_summary);
}

void ScanOfBody::summarize(FunctionID id)
{
    m_id = id;
    m_function = &m_program.at(id);
    reset();
    if (m_function->hasBody()) {
        m_summary.ownEffects = Effects::none();
        m_followsConstants = m_walkFollowsConstants;
        readBody();
        if (m_hasEnteredBlockPassedOver && !m_hasStopped) {
            reset();
            m_summary.ownEffects = Effects::none();
            m_followsConstants = false;
            m_followsTests = false;
            readBody();
        }
        if (!m_hasStopped) {
            readWhatNobodyAskedFor();
            findLoops();
            followObjectsMadeToBePrototypes();
            resolveParameters();
            layDescriptorsInSight();
            applySinks();
            nameGettersOfDescriptors();
            nameKeysOfDefinitionsByMaps();
            finish();
            return;
        }
        reset();
    }
    summarizeWithoutBody();
}

void ScanOfBody::reset()
{
    std::destroy_at(&m_summary);
    std::construct_at(&m_summary);
    m_summary.nodes.append(SummaryNode { });
    m_hasStopped = false;
    m_nodeSlots.fill(0, initialNumberOfNodeSlots);
    m_listsOfNodes.shrink(0);
    m_numberOfJoins = 0;
    m_numberOfInstancesInData = 0;
    m_atoms.shrink(0);
    m_origins.shrink(0);
    m_incoming.shrink(0);
    m_sinks.shrink(0);
    m_stores.shrink(0);
    if (!m_accessorsLaid.isEmpty()) {
        m_accessorsLaid.shrink(0);
        m_placesOfAccessorsLaid.clear();
    }
    m_storesAreSortedByRoot = false;
    m_definitionsByMaps.shrink(0);
    m_definitionsByDescriptors.shrink(0);
    m_storesOfPrototypes.shrink(0);
    m_heirsMadeByBuiltins.shrink(0);
    m_readsNotYetNeeded.shrink(0);
    m_returnsOfCoroutine.shrink(0);
    m_edgesLeadingBack.shrink(0);
    m_listsDefinitions = listsDefinitions();
    m_valuesMadeForVariables.shrink(0);
    m_listsFirstStoresOfBody = listsFirstStoresOfBody();
    m_blockAboveReturns = noDefinition;
    m_storesOfBodyOfModule.shrink(0);
    m_readsOfBodyOfModule.shrink(0);
    m_usesByBodyOfModule.shrink(0);
    m_readsNotedOfBodyOfModule.clear();
    m_usesNotedOfBodyOfModule.clear();
    m_blocksOfCalls.shrink(0);
    m_handlersOfBlocks.shrink(0);
    m_nodesCaughtByBlocks.shrink(0);
    m_innermostLoopHeads.shrink(0);
    m_isConvertedToPrimitive.shrink(0);
    m_operators.shrink(0);
    m_joinsOfPrimitives.shrink(0);
    m_objectsBeingMade.shrink(0);
    m_thisBeingMade = noDefinition;
    m_leavesFieldsEmptyForGood = false;
    m_fills.clear();
    m_fillsKept.shrink(0);
    m_objectsHandedToBlocks.shrink(0);
    m_blocksThatTookObjects.shrink(0);
    m_followsSomeEnvironment = false;
    m_hasGivenUpEnvironments = false;
    m_environmentsBeingMade.shrink(0);
    m_slotsOfEnvironments.shrink(0);
    m_valuesOverEnvironments.shrink(0);
    m_blocksAboveBlockBeingRead.shrink(0);
    m_closureBeingMade = noDefinition;
    m_environmentStoredInto = noDefinition;
    m_numberOfParameters = 0;
    m_position = 0;
    m_hasEnteredBlockPassedOver = false;
    m_knownEffects.reset();
}

void ScanOfBody::summarizeWithoutBody()
{
    m_summary.traits.add(SummaryTrait::MayThrow);
    uint32_t numberOfEntryParameters = m_function->numberOfEntryParameters();
    uint32_t numberOfCaptures = m_function->captures.size();
    for (uint32_t parameter = 0; parameter < numberOfEntryParameters; ++parameter)
        addEdge(parameter < numberOfCaptures ? nodeFor(SummaryNode::Kind::Capture, parameter) : nodeFor(SummaryNode::Kind::Parameter, parameter), 0);
    for (uint32_t result = 0; result < m_function->results.size(); ++result)
        addEdge(0, resultNode(result));
    addEdge(0, nodeFor(SummaryNode::Kind::Thrown));
}

static uint32_t hashOfNodeInScan(const SummaryNode& node)
{
    uint32_t hash = node.first * 0x9e3779b1u ^ node.second * 0x85ebca6bu ^ static_cast<uint32_t>(node.kind) * 0xc2b2ae35u;
    return hash ^ (hash >> 15);
}

void ScanOfBody::growNodeSlots()
{
    uint32_t size = static_cast<uint32_t>(m_nodeSlots.size()) * 2;
    m_nodeSlots.fill(0, size);
    uint32_t mask = size - 1;
    for (uint32_t index = 1; index < m_summary.nodes.size(); ++index) {
        uint32_t place = hashOfNodeInScan(m_summary.nodes[index]) & mask;
        while (m_nodeSlots[place])
            place = (place + 1) & mask;
        m_nodeSlots[place] = index + 1;
    }
}

uint32_t ScanOfBody::nodeFor(SummaryNode::Kind kind, uint32_t first, uint32_t second)
{
    ASSERT(kind != SummaryNode::Kind::Unknown);
    if ((m_summary.nodes.size() + 1) * 2 > m_nodeSlots.size())
        growNodeSlots();

    SummaryNode node { kind, first, second };
    uint32_t mask = static_cast<uint32_t>(m_nodeSlots.size()) - 1;
    for (uint32_t place = hashOfNodeInScan(node) & mask;; place = (place + 1) & mask) {
        uint32_t slot = m_nodeSlots[place];
        if (!slot) {
            uint32_t index = static_cast<uint32_t>(m_summary.nodes.size());
            m_summary.nodes.append(node);
            m_nodeSlots[place] = index + 1;
            return index;
        }
        if (m_summary.nodes[slot - 1] == node)
            return slot - 1;
    }
}

uint32_t ScanOfBody::captureNode(FunctionID function, uint32_t capture)
{
    if (!function || capture >= m_program.at(function).captures.size())
        return 0;
    return nodeFor(SummaryNode::Kind::Capture, capture, function == m_id ? 0 : function.index() + 1);
}

uint32_t ScanOfBody::fieldNode(LayoutID layout, uint32_t field)
{
    if (!layout || field >= m_program.at(layout).fields.size())
        return 0;
    return nodeFor(SummaryNode::Kind::Field, layout.index(), field);
}

uint32_t ScanOfBody::dataFieldNode(LayoutID layout, uint32_t field)
{
    if (!layout || field >= m_program.at(layout).fields.size() || m_program.at(layout).fields[field].form.contains(FieldForm::IsAccessor))
        return 0;
    return nodeFor(SummaryNode::Kind::Field, layout.index(), field);
}

TypeID ScanOfBody::primitiveConstantAt(uint32_t atom) const
{
    if (atom & (parameterBit | operatorBit))
        return { };
    const SummaryNode& node = m_summary.nodes[atom];
    if (node.kind != SummaryNode::Kind::Constant)
        return { };
    TypeID type = TypeID::fromBits(node.first);
    return holdsOnlyPrimitives(type) ? type : TypeID();
}

void ScanOfBody::addEdge(uint32_t from, uint32_t to, TypeID passes, bool runsCodeOfValue)
{
    if (from == to || (!to && (m_summary.nodes[from].kind == SummaryNode::Kind::Constant || m_summary.nodes[from].kind == SummaryNode::Kind::ResultOfOperator)))
        return;
    m_summary.edges.append(SummaryEdge { from, to, passes, runsCodeOfValue });
}

uint32_t ScanOfBody::nodeOfData(const DataNode& root)
{
    using DataAndTaker = std::pair<const DataNode*, uint32_t>;
    Vector<DataAndTaker, 32> nodesToVisit;
    nodesToVisit.append(DataAndTaker { &root, noSummaryNode });
    uint32_t nodeOfRoot = 0;
    while (!nodesToVisit.isEmpty()) {
        auto [node, taker] = nodesToVisit.takeLast();
        uint32_t made = 0;
        bool isListedForField = taker && taker != noSummaryNode && m_summary.nodes[taker].kind == SummaryNode::Kind::Field;
        switch (node->kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Hole:
            made = constant(TypeTable::undefined);
            break;
        case DataNode::Kind::Null:
            made = constant(TypeTable::null);
            break;
        case DataNode::Kind::Boolean:
            made = nodeFor(SummaryNode::Kind::Constant, TypeID(TypeTable::boolean).bits(), isListedForField ? WrittenUnit::boolean(node->as<DataNode::Boolean>().value).bits() : 0);
            break;
        case DataNode::Kind::Number: {
            double number = node->as<DataNode::Number>().value;
            bool isInteger = primitiveOfNumber(number) == PrimitiveSet::Integer;
            made = nodeFor(SummaryNode::Kind::Constant, typeOfNumberInScan(number).bits(), isListedForField && isInteger ? WrittenUnit::integer(static_cast<int64_t>(number)).bits() : 0);
            break;
        }
        case DataNode::Kind::String:
            made = nodeFor(SummaryNode::Kind::Constant, TypeID(TypeTable::string).bits(), isListedForField ? WrittenUnit::text(node->as<DataNode::String>().value).bits() : 0);
            break;
        case DataNode::Kind::BigInt:
            made = constant(TypeTable::bigInt);
            break;
        case DataNode::Kind::Array:
            made = classInstanceInData(BuiltinClasses::Array);
            for (const DataNode& element : node->as<DataNode::Array>().elements)
                nodesToVisit.append(DataAndTaker { &element, contentsNode(made, ContentsSlot::Element) });
            break;
        case DataNode::Kind::Object: {
            const auto& object = node->as<DataNode::Object>();
            made = object.layout ? nodeFor(SummaryNode::Kind::Allocation, object.layout.index()) : constant(TypeTable::object);
            for (const DataMember& member : object.members) {
                std::optional<uint32_t> field = object.layout ? m_program.at(object.layout).findField(member.key) : std::nullopt;
                uint32_t takerOfMember = field ? fieldNode(object.layout, *field) : object.layout ? takerOfMemberBesideFields(made, member.key) : 0u;
                nodesToVisit.append(DataAndTaker { &member.value, takerOfMember });
                if (!field && !object.layout)
                    addEdge(made, 0);
            }
            if (object.layout && taker != noSummaryNode) {
                const Layout& layout = m_program.at(object.layout);
                for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                    if (!std::ranges::contains(object.members, layout.fields[field].key, &DataMember::key))
                        noteFieldLeftEmptyForGood(FieldReference { object.layout, field });
                }
            }
            break;
        }
        }
        if (taker == noSummaryNode)
            nodeOfRoot = made;
        else
            addEdge(made, taker);
    }
    return nodeOfRoot;
}

ListInScan ScanOfBody::listOfNode(uint32_t node)
{
    if (m_listsOfNodes.size() <= node)
        m_listsOfNodes.grow(node + 1);
    ListInScan& list = m_listsOfNodes[node];
    if (!list.count) {
        list = { static_cast<uint32_t>(m_atoms.size()), 1 };
        m_atoms.append(node);
    }
    return list;
}

ListInScan ScanOfBody::listOf(ValueID value)
{
    ListInScan list = m_lists[value.index()];
    if (list.count == isResultOfCall)
        list = listOfNode(nodeFor(SummaryNode::Kind::ResultOfCall, list.first, 0));
    else if (list.count == isReadNotYetNeeded)
        list = readByName(list.first);
    else
        return list;
    m_lists[value.index()] = list;
    return list;
}

ListInScan ScanOfBody::readByName(uint32_t read)
{
    if (m_lists[read].count != isReadNotYetNeeded)
        return m_lists[read];
    auto definition = definitionOf(read).as<Opcode::PropGet>();
    ValueID object = definition.objectOperand();
    uint32_t access = addAccessUnder(SummaryAccess::Kind::Read, ValueID(read), object ? listOf(object) : ListInScan(), definition.key(), ValueID());
    m_summary.accesses[access].isPartOfConstruction = m_uses[read] & mayComeInFrontOfDefinition;
    m_summary.accesses[access].subjectIsAnsweredBySuperCall = m_uses[read] & readsAnswerOfSuperCall;
    ListInScan list = listOfNode(resultOfRead(access));
    m_lists[read] = list;
    return list;
}

ListInScan ScanOfBody::internAtoms(Vector<uint32_t>& atoms)
{
    std::sort(atoms.begin(), atoms.end());
    atoms.shrink(std::unique(atoms.begin(), atoms.end()) - atoms.begin());

    TypeID united;
    unsigned numberOfConstants = 0;
    for (uint32_t atom : atoms) {
        if (TypeID type = primitiveConstantAt(atom))
            united = numberOfConstants++ ? unionOfPrimitiveTypesInScan(united, type) : type;
    }
    if (numberOfConstants > 1) {
        atoms.removeAllMatching([&](uint32_t atom) {
            return !!primitiveConstantAt(atom);
        });
        atoms.append(constant(united));
        std::sort(atoms.begin(), atoms.end());
    }

    if (atoms.size() == 1 && !(atoms[0] & (parameterBit | operatorBit)))
        return listOfNode(atoms[0]);
    ListInScan list { static_cast<uint32_t>(m_atoms.size()), static_cast<uint32_t>(atoms.size()) };
    m_atoms.appendVector(atoms);
    return list;
}

bool ScanOfBody::holdsOnlyConstants(ListInScan list) const
{
    for (uint32_t atom : m_atoms.span().subspan(list.first, list.count)) {
        if (atom & parameterBit)
            return false;
        if (!(atom & operatorBit) && atom && m_summary.nodes[atom].kind != SummaryNode::Kind::Constant)
            return false;
    }
    return true;
}

bool ScanOfBody::isAlwaysPrimitive(ListInScan list) const
{
    for (uint32_t atom : m_atoms.span().subspan(list.first, list.count)) {
        if (atom & parameterBit || (!(atom & operatorBit) && !primitiveConstantAt(atom)))
            return false;
    }
    return true;
}

bool ScanOfBody::isAlwaysPrimitive(const OriginInScan& origin) const
{
    if (primitiveConstantAt(origin.node) || (origin.passes && holdsOnlyPrimitives(origin.passes)))
        return true;
    return m_summary.nodes[origin.node].kind == SummaryNode::Kind::ResultOfOperator || std::ranges::binary_search(m_joinsOfPrimitives, origin.node);
}

uint32_t ScanOfBody::nodeOfAtom(uint32_t atom) const
{
    if (!(atom & operatorBit))
        return atom;
    uint32_t record = m_operators[atom & ~operatorBit].record;
    RELEASE_ASSERT(record != OperatorInScan::noRecord);
    return m_summary.operators[record].result;
}

void ScanOfBody::beginResult()
{
    m_result.shrink(0);
    m_onlySourceOfResult = { };
    m_numberOfSourcesOfResult = 0;
    m_nodeReadOffOperand = 0;
    m_operandSettled = std::nullopt;
    m_resultLeaves = false;
    m_sourcesCopiedIntoResult.shrink(0);
    m_objectCopiedInto = std::nullopt;
    m_prototypeOfResult = std::nullopt;
}

void ScanOfBody::addList(ListInScan list)
{
    if (!list.count || (m_numberOfSourcesOfResult && list == m_onlySourceOfResult))
        return;
    ++m_numberOfSourcesOfResult;
    m_onlySourceOfResult = list;
    m_result.append(m_atoms.span().subspan(list.first, list.count));
}

ListInScan ScanOfBody::takeResult()
{
    return m_numberOfSourcesOfResult > 1 ? internAtoms(m_result) : m_onlySourceOfResult;
}

void ScanOfBody::addOperand(const BodyValue& value, uint32_t operandIndex)
{
    if (ValueID operand = value.operand(operandIndex))
        addList(listOf(operand));
}

void ScanOfBody::addVariadicOperand(const BodyValue& value, uint32_t place)
{
    std::span<const ValueID> operands = value.variadicOperands();
    if (place < operands.size() && operands[place])
        addList(listOf(operands[place]));
    else
        addNode(0);
}

void ScanOfBody::addConstant(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::NewClosure) {
        FunctionID function = value.as<Opcode::NewClosure>().function();
        addNode(function ? nodeFor(SummaryNode::Kind::Closure, function.index()) : 0);
        return;
    }
    if (opcode == Opcode::Data) {
        DataID data = value.as<Opcode::Data>().data();
        addNode(data ? nodeOfData(m_program.at(data).root) : 0);
        return;
    }
    if (opcode == Opcode::Builtin) {
        auto named = value.as<Opcode::Builtin>();
        const BuiltinTable& builtins = m_program.builtins();
        bool isObjectItself = named.builtin() && (named.isNotReadFromHolder() || namesWhatNobodyReplacesInScan(builtins, named.builtin()));
        addNode(isObjectItself ? nodeFor(SummaryNode::Kind::Builtin, builtins.canonicalID(named.builtin()).index()) : 0);
        if (ModuleID compiled = isObjectItself ? compiledModuleWithObject(m_program, builtins.canonicalID(named.builtin())) : ModuleID()) {
            uint32_t answer = newJoin();
            m_summary.objectsOfCompiledModules.append({ compiled, answer });
            addNode(answer);
        }
        return;
    }
    if (opcode == Opcode::String) {
        BuiltinName name = m_program.strings.builtinName(value.as<Opcode::String>().string());
        if (name && !isLibraryCode(m_program, *m_function)) {
            if (!m_namesUnderWhichEventsHang) {
                m_namesListedByThisScan = namesUnderWhichEventsHangInScan(m_program.builtins());
                m_namesUnderWhichEventsHang = &m_namesListedByThisScan;
            }
            if (m_namesUnderWhichEventsHang->get(name.index()))
                m_summary.textsThatNameEvents.append(m_program.keyOf(name));
        }
    }
    TypeID type = typeOfPrimitiveConstantInScan(value);
    addNode(type ? constant(type) : 0);
}

void ScanOfBody::addNatural(const BodyValue& value, const NaturalType& written)
{
    if (written.opaque)
        return;
    if (TypeID primitives = typeOfPrimitivesWrittenInScan(written))
        addNode(constant(primitives));
    if (!written.isObject && !written.isFunction && !written.builtinClass)
        return;
    if (flowOf(value.opcode()) != ValueFlowEdgeClass::Allocates) {
        addNode(m_nodeReadOffOperand);
        return;
    }
    if (written.builtinClass) {
        addNode(classInstanceMadeBy(value, written.builtinClass));
        return;
    }
    uint32_t made = constant(written.isFunction && !written.isObject ? TypeTable::function : TypeTable::object);
    addNode(made);
    if (value.opcode() == Opcode::CopyObject) {
        addOperand(value, 0);
        m_objectCopiedInto = made;
    }
}

bool ScanOfBody::prototypeMaySelectOtherObject(const BodyValue& value, LayoutID layout) const
{
    std::optional<unsigned> prototype = OperandPosition::prototype(value.opcode());
    return prototype && value.operand(*prototype) && m_program.prototypeMaySelectOtherClass(layout);
}

void ScanOfBody::addAllocation(const BodyValue& value, LayoutID layout)
{
    if (!layout) {
        addNode(0);
        return;
    }
    uint32_t allocation = nodeFor(SummaryNode::Kind::Allocation, layout.index());
    if (!prototypeMaySelectOtherObject(value, layout)) {
        addNode(allocation);
        return;
    }
    uint32_t made = newJoin();
    addEdge(allocation, made);
    for (LayoutID below : m_program.layoutsExpectedBelow(layout))
        addEdge(nodeFor(SummaryNode::Kind::Allocation, below.index()), made);
    addNode(made);
}

void ScanOfBody::addDeclared(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::GetModuleVariable) {
        addNode(variableNode(value.as<Opcode::GetModuleVariable>().variable()));
        return;
    }
    if (opcode == Opcode::GetEnvironmentField) {
        auto read = value.as<Opcode::GetEnvironmentField>();
        addNode(fieldNode(read.layout(), read.field()));
        return;
    }
    if (opcode == Opcode::GetCapture) {
        auto read = value.as<Opcode::GetCapture>();
        addNode(captureNode(read.function(), read.index()));
        return;
    }
    if (LayoutID layout = layoutOfNamespaceGivenBy(value)) {
        addNode(nodeFor(SummaryNode::Kind::Allocation, layout.index()));
        return;
    }
    addNode(0);
}

LayoutID ScanOfBody::layoutOfNamespaceGivenBy(const BodyValue& value) const
{
    if (value.opcode() != Opcode::ModuleNamespace)
        return { };
    auto named = value.as<Opcode::ModuleNamespace>();
    if (!named.module() || named.phase() == ImportPhase::Defer)
        return { };
    return m_program.layoutOfWholeNamespace(named.module());
}

void ScanOfBody::addContents(const BodyValue& value, uint32_t operandIndex, std::optional<ContentsSlot> slot)
{
    if (m_operandSettled == operandIndex) {
        addList(m_whatOperandSettlesTo);
        return;
    }
    ValueID holder = value.operand(operandIndex);
    if (!slot && operandKindOf(value.opcode(), operandIndex) == OperandKind::Array)
        slot = ContentsSlot::Element;
    if (holder && slot) {
        addNode(readContents(value.id(), listOf(holder), *slot));
        addNode(constant(TypeTable::undefined));
        return;
    }
    if (holder)
        escape(listOf(holder));
    addNode(0);
}

void ScanOfBody::addResultAt(const BodyValue& value)
{
    auto read = value.as<Opcode::ResultAt>();
    ValueID producer = read.resultsOperand();
    BodyValue definition = producer ? definitionOf(producer.index()) : BodyValue();
    if (!definition) {
        addNode(0);
        return;
    }

    Opcode opcode = definition.opcode();
    if (effectRuleOf(opcode) == EffectRule::Call) {
        std::optional<uint32_t> call = findCall(producer);
        addNode(call ? nodeFor(SummaryNode::Kind::ResultOfCall, *call, read.index()) : 0);
        return;
    }
    std::optional<uint32_t> numberOfResults = numberOfDeclaredResultsOf(opcode);
    if (resultRuleOf(opcode) != ResultRule::Results || (numberOfResults && read.index() >= *numberOfResults)) {
        addNode(0);
        return;
    }
    ListInScan handedOut = m_lists[producer.index()];
    if (flowRuleOf(opcode) == FlowRule::Iteration && handedOut.count && ((unknownResultsOf(opcode) >> std::min(read.index(), 31u)) & 1)) {
        addList(handedOut);
        return;
    }
    ResultRulesOfScan rules { *this };
    applyResultRule(rules, definition, read.index(), ResultSide::Proven);
}

void ScanOfBody::addOperator(const BodyValue& value)
{
    OperatorInScan found;
    found.site = value.id();
    found.opcode = value.opcode();
    found.numberOfOperands = numberOfNamedOperandsOf(found.opcode);
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        found.operands[operandIndex] = listOf(operand);
    });

    ListInScan result { static_cast<uint32_t>(m_atoms.size()), 1 };
    m_atoms.append(operatorBit | static_cast<uint32_t>(m_operators.size()));
    m_operators.append(found);
    addList(result);
}

void ScanOfBody::noteInspection(ValueID site, ThinSummary::Inspection::Kind kind, ListInScan list)
{
    if (list.count && isAlwaysPrimitive(list))
        return;
    addSink(SinkKindInScan::SubjectOfInspection, list, static_cast<uint32_t>(m_summary.inspections.size()));
    m_summary.inspections.append({ site, kind, 0 });
}

uint32_t ScanOfBody::takerOfMemberBesideFields(uint32_t object, PropertyKeyID key)
{
    SummaryAccess access;
    access.site = ValueID(m_position);
    access.kind = SummaryAccess::Kind::Define;
    access.isPartOfConstruction = true;
    access.subject = object;
    access.key = key;
    access.value = newJoin();
    m_summary.accesses.append(access);
    return access.value;
}

void ScanOfBody::addSink(SinkKindInScan kind, ListInScan list, uint32_t target, TypeID passes)
{
    m_sinks.append(SinkInScan { kind, list, target, passes, false });
}

void ScanOfBody::flowInto(ListInScan list, uint32_t node)
{
    if (!list.count || (!node && holdsOnlyConstants(list)))
        return;
    addSink(SinkKindInScan::Edge, list, node);
}

void ScanOfBody::handToCodeThatRuns(ListInScan list)
{
    if (!list.count || isAlwaysPrimitive(list))
        return;
    m_sinks.append(SinkInScan { SinkKindInScan::Edge, list, 0, { }, true });
}

ListInScan ScanOfBody::objectsAmong(ListInScan list)
{
    uint32_t objects = newJoin();
    addSink(SinkKindInScan::Edge, list, objects, TypeTable::object);
    return listOfNode(objects);
}

uint32_t ScanOfBody::callMember(ValueID site, ListInScan receiver, BuiltinName name, std::initializer_list<ListInScan> arguments, uint32_t block)
{
    uint32_t callIndex = static_cast<uint32_t>(m_summary.calls.size());
    SummaryCall call;
    call.site = site;
    call.callee = SummaryCall::Callee::Name;
    call.isMadeByProtocol = true;
    call.key = m_program.keyOf(name);
    call.receiver = 0;
    call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
    call.numberOfArguments = static_cast<uint32_t>(arguments.size());
    for (ListInScan argument : arguments) {
        addSink(SinkKindInScan::Argument, argument, static_cast<uint32_t>(m_summary.arguments.size()));
        m_summary.arguments.append(0);
    }
    m_summary.calls.append(call);
    m_blocksOfCalls.append(block);
    addSink(SinkKindInScan::ReceiverOfCall, receiver, callIndex);
    return callIndex;
}

void ScanOfBody::addArgument(ValueID operand, bool leaves)
{
    uint32_t place = static_cast<uint32_t>(m_summary.arguments.size());
    m_summary.arguments.append(0);
    if (!operand)
        return;
    if (leaves)
        noteLeaving(operand);
    addSink(SinkKindInScan::Argument, listOf(operand), place);
}

uint32_t ScanOfBody::addAccess(SummaryAccess::Kind kind, ValueID site, ListInScan subject, PropertyKeyID key)
{
    uint32_t index = static_cast<uint32_t>(m_summary.accesses.size());
    SummaryAccess access;
    access.site = site;
    access.kind = kind;
    access.key = key;
    m_summary.accesses.append(access);
    addSink(SinkKindInScan::SubjectOfAccess, subject, index);
    return index;
}

uint32_t ScanOfBody::addAccessUnder(SummaryAccess::Kind kind, ValueID site, ListInScan subject, PropertyKeyID key, ValueID computedKey)
{
    uint32_t access = addAccess(kind, site, subject, key);
    if (key)
        return access;
    m_summary.accesses[access].keyNode = 0;
    if (computedKey)
        addSink(SinkKindInScan::KeyOfAccess, listOf(computedKey), access);
    return access;
}

uint32_t ScanOfBody::resultOfRead(uint32_t access)
{
    uint32_t node = nodeFor(SummaryNode::Kind::ResultOfAccess, access);
    m_summary.accesses[access].value = node;
    return node;
}

uint32_t ScanOfBody::addRead(ValueID site, ListInScan subject, PropertyKeyID key)
{
    return resultOfRead(addAccessUnder(SummaryAccess::Kind::Read, site, subject, key, ValueID()));
}

uint32_t ScanOfBody::readContents(ValueID site, ListInScan holder, SlotOfContents slot)
{
    uint32_t access = addAccess(SummaryAccess::Kind::Read, site, holder, { });
    m_summary.accesses[access].slot = slot;
    return resultOfRead(access);
}

void ScanOfBody::storeIntoContents(ValueID site, ListInScan holder, SlotOfContents slot, ListInScan stored)
{
    if (!stored.count)
        return;
    uint32_t access = addAccess(SummaryAccess::Kind::Store, site, holder, { });
    m_summary.accesses[access].slot = slot;
    addSink(SinkKindInScan::ValueOfAccess, stored, access);
}

ListInScan ScanOfBody::settle(ValueID site, ListInScan list, uint32_t block)
{
    if (isAlwaysPrimitive(list))
        return list;

    uint32_t access = addAccess(SummaryAccess::Kind::Read, site, list, { });
    m_summary.accesses[access].slot = ContentsSlot::Value;
    uint32_t whatIsNoPromise = newJoin();
    m_summary.accesses[access].whatHoldsNothing = whatIsNoPromise;
    uint32_t valueOfPromise = resultOfRead(access);

    ListInScan objects = objectsAmong(listOfNode(whatIsNoPromise));
    uint32_t found = newJoin();
    for (BuiltinName name : m_program.builtins().keysReadBy(Protocol::Thenable)) {
        addEdge(addRead(site, objects, m_program.keyOf(name)), found);
        callMember(site, objects, name, { listOfNode(0), listOfNode(0) }, block);
    }
    m_summary.accesses[access].keyNode = found;

    Vector<uint32_t> atoms { valueOfPromise, whatIsNoPromise };
    return internAtoms(atoms);
}

std::optional<SlotOfContents> ScanOfBody::slotNamedByRowOf(const BodyValue& value) const
{
    Opcode opcode = value.opcode();
    switch (slotOfHolderOf(opcode)) {
    case SlotOfHolder::None:
        return std::nullopt;
    case SlotOfHolder::Element:
        return SlotOfContents(ContentsSlot::Element);
    case SlotOfHolder::Key:
        return SlotOfContents(ContentsSlot::Key);
    case SlotOfHolder::Value:
        return SlotOfContents(ContentsSlot::Value);
    case SlotOfHolder::InternalField:
        return SlotOfContents::internalField(PayloadWord::index(opcode) ? value.index() : fieldOfHolderOf(opcode));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ScanOfBody::someResultIsReadOutOf(const BodyValue& value, uint32_t operandIndex) const
{
    Opcode opcode = value.opcode();
    RulesOfWhereResultIsReadFromInScan rules;
    uint32_t numberOfResults = resultRuleOf(opcode) == ResultRule::Results ? numberOfDeclaredResultsOf(opcode).value_or(0) : 1;
    for (uint32_t result = 0; result < numberOfResults; ++result) {
        if (applyResultRule(rules, value, result, ResultSide::Proven) == operandIndex)
            return true;
    }
    return false;
}

void ScanOfBody::addChange(SummaryChange::Kind kind, ValueID site, ListInScan subject, PropertyKeyID key, std::optional<ListInScan> other)
{
    uint32_t index = static_cast<uint32_t>(m_summary.changes.size());
    SummaryChange change;
    change.site = site;
    change.kind = kind;
    change.key = key;
    m_summary.changes.append(change);
    addSink(SinkKindInScan::SubjectOfChange, subject, index);
    if (other)
        addSink(SinkKindInScan::OtherOfChange, *other, index);
}

void ScanOfBody::defineByDescriptor(ValueID site, ListInScan subject, PropertyKeyID key, std::optional<ListInScan> other, bool isUnderNameThatNobodyElseNames)
{
    addChange(SummaryChange::Kind::DefinesByDescriptor, site, subject, key, other);
    if (isUnderNameThatNobodyElseNames) {
        m_summary.changes.last().isUnderNameThatNobodyElseNames = true;
        return;
    }
    uint32_t access = addAccess(SummaryAccess::Kind::Define, site, subject, key);
    m_summary.accesses[access].isByDescriptor = true;
    if (!key)
        m_summary.accesses[access].keyNode = 0;
    m_summary.accesses[access].value = 0;
}

ScanOfBody::KeyWrittenInScan ScanOfBody::keyWrittenAs(ValueID operand) const
{
    BodyValue definition = operand ? definitionOf(operand.index()) : BodyValue();
    while (definition && opcodesThatStateTypes.contains(definition.opcode())) {
        ValueID stated = definition.operand(0);
        definition = stated ? definitionOf(stated.index()) : BodyValue();
    }
    if (!definition || definition.opcode() != Opcode::String)
        return { };
    StringID text = definition.as<Opcode::String>().string();
    NameClass nameClass = m_program.nameClass(text);
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
        return { };
    std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
    return { true, key && m_program.types.hasStableNumber(*key) ? *key : PropertyKeyID() };
}

BodyValue ScanOfBody::definitionOf(uint32_t value) const
{
    uint32_t offset = m_wordOffsets[value];
    return offset == noDefinition ? BodyValue() : m_function->body.operationAt(offset, ValueID(value));
}

PrimitiveKnownToFolding ScanOfBody::primitiveHeldBy(ValueID value)
{
    BodyValue definition = definitionOf(value.index());
    if (!definition || !operationsThatConstantsMayDecide.contains(definition.opcode()))
        return { };
    if (m_primitivesAsked[value.index()].stamp != m_stampOfBody) {
        PrimitiveKnownToFolding known = primitiveMadeOfConstants(m_program, definition, scopedLambda<PrimitiveKnownToFolding(ValueID)>([&](ValueID operand) {
            return primitiveHeldBy(operand);
        }));
        m_primitivesAsked[value.index()] = { m_stampOfBody, known };
    }
    return m_primitivesAsked[value.index()].known;
}

void ScanOfBody::noteLeaving(ValueID value, WayOfLeavingInScan way)
{
    uint32_t root = m_roots[value.index()];
    if (m_followsSomeEnvironment)
        noteLeavingOverEnvironments(root);
    uint32_t& first = m_firstLeft[root];
    first = std::min(first, m_position);
    m_lastLeft[root] = std::max(m_lastLeft[root], m_position);
    if (way == WayOfLeavingInScan::IsSeenFromOutside)
        m_uses[root] |= isSeenFromOutside;
    if (m_uses[root] & isFollowedWhileMade)
        meetFillsWhereObjectLeaves(objectBeingMade(root), fillsOf(root, m_fills.inOrder()));
    if (way == WayOfLeavingInScan::IsReturned && (m_uses[root] & isFollowedWhileMade))
        meetFillsWhereConstructionEnds(root);
}

std::optional<uint32_t> ScanOfBody::findCall(ValueID site) const
{
    auto found = std::ranges::lower_bound(m_summary.calls, site, { }, &SummaryCall::site);
    if (found == m_summary.calls.end() || found->site != site)
        return std::nullopt;
    return static_cast<uint32_t>(found - m_summary.calls.begin());
}

void ScanOfBody::readBody()
{
    const Body& body = m_function->body;
    m_lists.fill(ListInScan { }, body.numberOfValues());
    m_wordOffsets.fill(noDefinition, body.numberOfValues());
    m_blocks.fill(0, body.numberOfValues());
    m_roots.fill(0, body.numberOfValues());
    m_uses.fill(0, body.numberOfValues());
    m_firstLeft.fill(hasNotLeft, body.numberOfValues());
    m_lastLeft.fill(0, body.numberOfValues());
    m_firstParameterOfBlock.fill(0, body.numberOfBlocks() + 1);
    m_fillsAtBlocks.fill(ListInScan { 0, noFillsYet }, body.numberOfBlocks());
    m_numberOfEdgesInto.fill(0, body.numberOfBlocks());
    if (m_listsFirstStoresOfBody) {
        m_immediateDominators.fill(noDefinition, body.numberOfBlocks());
        m_immediateDominators[entryBlock.index()] = entryBlock.index();
        m_endsOfBlocks.fill(0, body.numberOfBlocks());
    }
    m_isEntered.fill(false, body.numberOfBlocks());
    m_isEntered[entryBlock.index()] = true;
    ++m_stampOfBody;
    m_summary.mayReadPathOfAnyModule = false;
    if (m_primitivesAsked.size() < body.numberOfValues())
        m_primitivesAsked.grow(body.numberOfValues());
    BodyBlock block = body.entry();
    for (uint32_t remaining = body.numberOfBlocks(); remaining-- && !m_hasStopped;)
        block = readBlock(block);
    m_firstParameterOfBlock[body.numberOfBlocks()] = m_numberOfParameters;
}

BodyBlock ScanOfBody::readBlock(const BodyBlock& block)
{
    uint32_t number = block.id().index();
    m_firstParameterOfBlock[number] = m_numberOfParameters;
    const UnitIDRemap* unit = &m_function->body.unit();
    if (m_followsConstants && !m_isEntered[number]) {
        m_summary.blocksNeverEntered.append(block.id());
        ModulesNamedWhereNoRunComesInScan named { m_summary.modulesNamedInBlocksNeverEntered };
        BodyValues::Iterator end = block.values().begin();
        for (uint32_t remaining = block.numberOfValues(); remaining--; ++end)
            (*end).forEachReference(named);
        return { BodyTerminator(end.words(), unit, block.id()).end(), nullptr, unit, BlockID(number + 1), block.endOfValues() };
    }
    for (uint32_t position = 0; position < block.numberOfParameters(); ++position) {
        uint32_t value = block.parameter(position).index();
        m_lists[value] = { static_cast<uint32_t>(m_atoms.size()), 1 };
        m_atoms.append(parameterBit | m_numberOfParameters++);
        m_roots[value] = value;
        m_blocks[value] = number;
        if (block.id() == entryBlock)
            readEntryParameter(position);
    }

    m_position = block.firstValue().index();
    m_blockBeingRead = number;
    if (m_followsSomeEnvironment)
        enterBlockForEnvironments(number);
    if (m_listsFirstStoresOfBody)
        m_endsOfBlocks[number] = block.endOfValues().index();
    if (block.id() == entryBlock)
        followThis(block);
    else if (!m_objectsBeingMade.isEmpty()) {
        ListInScan kept = m_fillsAtBlocks[number];
        m_fills.set(kept.count != noFillsYet ? m_fillsKept.span().subspan(kept.first, kept.count) : std::span<const FillInScan> { });
        takeObjectsHandedTo(block);
    }
    if (block.hasHandler())
        readArgumentsOf(number, block.handler(), KindOfEdgeInScan::ToHandler);
    noteHandlerOf(block);

    BodyValues::Iterator cursor = block.values().begin();
    for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
        readOperation(*cursor, number);
        if (m_hasStopped)
            return { };
    }
    m_position = block.endOfValues().index();
    endOperationsOverEnvironments();
    BodyTerminator terminator(cursor.words(), unit, block.id());
    readTerminator(terminator, number);
    return { terminator.end(), nullptr, unit, BlockID(number + 1), block.endOfValues() };
}

void ScanOfBody::stopAtUndefinedRecord(uint32_t block, ValueID value, Wire::SourceLocation location, ASCIILiteral operation)
{
    m_hasStopped = true;
    if (m_error && m_error->function.index() < m_id.index())
        return;
    m_error = InternalError {
        .kind = InternalErrorKind::PayloadOutOfRange,
        .function = m_id,
        .block = BlockID(block),
        .value = value,
        .location = location,
        .detail = "it names a record that no file of the build defines"_s,
        .operation = operation,
    };
}

void ScanOfBody::readEntryParameter(uint32_t parameter)
{
    uint32_t node = 0;
    if (parameter < m_function->captures.size())
        node = nodeFor(SummaryNode::Kind::Capture, parameter);
    else if (parameter < m_function->numberOfEntryParameters())
        node = nodeFor(SummaryNode::Kind::Parameter, parameter);
    m_incoming.append(IncomingInScan { entryBlock.index(), parameter, listOfNode(node), { } });
}

void ScanOfBody::readArgumentsOf(uint32_t from, BodyTarget target, KindOfEdgeInScan kind)
{
    uint32_t to = target.block().index();
    noteEdgeOfBodyOfModule(from, to, kind);
    if (m_followsSomeEnvironment)
        noteEdgeForEnvironments(from, to, kind);
    noteEdgeBetweenBlocks(from, to, kind);
    if (to <= from) {
        m_edgesLeadingBack.append(EdgeLeadingBackInScan { to, from });
        m_hasEnteredBlockPassedOver |= !m_isEntered[to];
    }
    m_isEntered[to] = true;
    carryFillsOver(from, to);
    uint32_t position = 0;
    for (ValueID argument : target.arguments()) {
        if (argument) {
            noteTakerOfValueMadeForVariable(argument);
            handToBlock(argument, from, to, position, kind);
            m_incoming.append(IncomingInScan { to, position, listOf(argument), { } });
        }
        ++position;
    }
}

static bool readsTokenOfClassInScan(const Program& program, const BodyValue& value)
{
    if (value.opcode() != Opcode::GetModuleVariable)
        return false;
    TypeID written = program.at(value.as<Opcode::GetModuleVariable>().variable()).declared.written();
    return written && program.at(written).isOpaque(OpaqueKind::ClassToken);
}

void ScanOfBody::readOperation(const BodyValue& value, uint32_t block)
{
    Opcode opcode = value.opcode();
    uint32_t index = value.id().index();
    m_position = index;
    m_blocks[index] = block;
    m_wordOffsets[index] = static_cast<uint32_t>(value.words() - m_function->body.words().data());
    ValueID handedOn = inputOperationsThatHandOn.contains(opcode) ? value.operand(0) : ValueID();
    m_roots[index] = handedOn ? m_roots[handedOn.index()] : index;
    if (handedOn && opcode == Opcode::FinishObject && (m_uses[m_roots[index]] & isFollowedWhileMade))
        meetFillsWhereConstructionEnds(m_roots[index]);
    if (m_followsSomeEnvironment)
        beginOperationOverEnvironments(value);
    if (!inputOperationsThatOnlyHandOn.contains(opcode))
    {
        static constexpr OpcodeSet operationsThatWeighNothingInCopy { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::ToBoolean, Opcode::LogicalNot, Opcode::GetModuleVariable };
        m_summary.numberOfOperations += !readsTokenOfClassInScan(m_program, value);
        m_summary.weightOfOperations += !operationsThatWeighNothingInCopy.contains(opcode);
    }
    if (opcode == Opcode::String)
        noteTextSpelled(value.as<Opcode::String>().string());
    if (m_listsFirstStoresOfBody)
        lookForFirstStoresAndUses(value);
    if (m_listsDefinitions)
        lookForDefinitions(value);

    bool isFollowedFromHere = (opcode == Opcode::ModuleNamespace && layoutOfNamespaceGivenBy(value)) || answersWithResultOfBody(value);
    bool handsModuleOut = opcode == Opcode::EvaluateModule || (resultRuleOf(opcode) != ResultRule::None && !isFollowedFromHere);
    ReferencesOfScan references { *this, value.id(), block, opcode == Opcode::NewClosure, handsModuleOut };
    value.forEachReference(references);
    notePathsOfModulesReadInScan(value, m_summary);
    if (opcode == Opcode::NewClosure && !references.namesUndefinedRecord) {
        if (LayoutID constructed = classOfConstructorFunction(m_program, value.as<Opcode::NewClosure>().function()))
            m_summary.classesEvaluated.append(constructed);
    }
    if (references.namesUndefinedRecord) {
        stopAtUndefinedRecord(block, value.id(), m_function->locationOf(value), nameOf(opcode));
        return;
    }

    if (effectRuleOf(opcode) == EffectRule::Call) {
        readCall(value, block);
        if (opcodesOfSuperCallsInScan.contains(opcode) && m_function->kind == FunctionKind::DerivedConstructor) {
            m_uses[index] |= isUnderConstruction;
            follow(index, ObjectBeingMadeKindInScan::AnsweredBySuperCall);
        }
        return;
    }

    std::optional<SummaryAccess::Kind> access = accessMadeByInScan(opcode);
    Effects effectsStorage;
    const Effects* declaredEffects = &declaredEffectsOfOperation(m_program, opcode, value, effectsStorage);
    if (opcode == Opcode::Builtin && standsForFirstReadOffHolder(m_program.builtins(), value.as<Opcode::Builtin>().builtin(), value.as<Opcode::Builtin>().isNotReadFromHolder())) {
        if (declaredEffects != &effectsStorage)
            effectsStorage = *declaredEffects;
        effectsStorage.merge(declaredBoundInTable(Opcode::InitializeBuiltin));
        declaredEffects = &effectsStorage;
    }
    if (access && declaredEffects->mayCallUnknownCode)
        declaredEffects = &effectsOfAccessItselfInScan(*access);
    else if (stepsSynchronousIteratorInScan(value) || opcode == Opcode::SuperGet)
        declaredEffects = &effectsOfAccessItselfInScan(SummaryAccess::Kind::Read);
    else if (opcode == Opcode::SuperSet)
        declaredEffects = &effectsOfAccessItselfInScan(SummaryAccess::Kind::Store);
    const Effects& effects = noteOwnEffects(value, block, access, *declaredEffects, effectsStorage);
    if (!effects.isPure())
        m_summary.ownEffects.merge(effects);
    if (effects.maySuspend)
        m_summary.traits.add(SummaryTrait::Suspends);
    if (raisesOf(opcode) == ProgramEvent::EvaluatesText)
        m_summary.traits.add(SummaryTrait::HasDirectEval);
    if (opcodesOfArgumentsObjectsInScan.contains(opcode))
        m_summary.traits.add(SummaryTrait::UsesArguments);

    beginResult();
    m_keyOfOperation = PayloadWord::key(opcode) ? value.key() : PropertyKeyID();
    if (access) {
        readAccess(value, *access, block);
        return;
    }
    if (opcode == Opcode::NewClass) {
        readNewClass(value);
        return;
    }
    if (opcode == Opcode::NewClassToken)
        m_summary.classesEvaluated.append(value.as<Opcode::NewClassToken>().layout());
    if (opcode == Opcode::DefineMethod) {
        readDefineMethod(value);
        return;
    }
    if (opcode == Opcode::DefineGetter && value.as<Opcode::DefineGetter>().key()) {
        auto definition = value.as<Opcode::DefineGetter>();
        readDefineAccessor(value, definition.objectOperand(), definition.functionOperand(), definition.key(), SummaryChange::Lays::Getter);
        return;
    }
    if (opcode == Opcode::DefineSetter && value.as<Opcode::DefineSetter>().key()) {
        auto definition = value.as<Opcode::DefineSetter>();
        readDefineAccessor(value, definition.objectOperand(), definition.functionOperand(), definition.key(), SummaryChange::Lays::Setter);
        return;
    }
    if ((opcode == Opcode::ToThis || opcode == Opcode::ToObject) && value.operand(0)) {
        ValueID given = value.operand(0);
        noteLeaving(given);
        OperatorInScan conversion;
        conversion.site = value.id();
        conversion.opcode = opcode;
        conversion.isNeeded = true;
        conversion.numberOfOperands = 1;
        conversion.operands[0] = listOf(given);
        conversion.joinOfResult = newJoin();
        addSink(SinkKindInScan::Edge, conversion.operands[0], conversion.joinOfResult, TypeTable::object);
        m_lists[index] = listOfNode(conversion.joinOfResult);
        m_operators.append(conversion);
        return;
    }
    if (catchOpcodes.contains(opcode)) {
        m_lists[index] = listOfNode(nodeCaughtBy(BlockID(block)));
        return;
    }
    if (opcode == Opcode::NarrowByField && !m_summary.keysOfTestedFields.contains(value.key()))
        m_summary.keysOfTestedFields.append(value.key());
    if (opcode == Opcode::CheckInitialized && handedOn) {
        uint32_t passed = newJoin();
        addSink(SinkKindInScan::Edge, listOf(handedOn), passed, TypeTable::anything);
        m_lists[index] = listOfNode(passed);
        return;
    }

    if (opcode == Opcode::NarrowByTest && value.operand(OperandIndex::NarrowByTest::left)) {
        m_lists[index] = listOf(value.operand(OperandIndex::NarrowByTest::left));
        return;
    }

    bool handsOnReadNotYetNeeded = handedOn && m_lists[handedOn.index()].count == isReadNotYetNeeded;
    uint32_t endOfOperandsRead = VariadicOperandPosition::held(opcode) ? numberOfNamedOperandsOf(opcode) : UINT32_MAX;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex >= endOfOperandsRead || (handsOnReadNotYetNeeded && !operandIndex))
            return;
        bool isLeftToSuperAccess = (opcode == Opcode::SuperGet && operandIndex == OperandIndex::SuperGet::receiver)
            || (opcode == Opcode::SuperSet && (operandIndex == OperandIndex::SuperSet::receiver || operandIndex == OperandIndex::SuperSet::value));
        if (isLeftToSuperAccess) {
            noteLeaving(operand);
            return;
        }
        readOperand(value, operand, operandIndex, operandInfoOf(opcode, operandIndex), block);
    });
    if (handsOnReadNotYetNeeded) {
        m_lists[index] = m_lists[handedOn.index()];
        return;
    }

    if (opcode == Opcode::ReleaseHint) {
        if (uint32_t root = m_roots[value.operand(0).index()]; root < std::min<uint32_t>(m_function->numberOfEntryParameters(), 64))
            m_summary.entryParametersLetGo |= uint64_t(1) << root;
    }
    if (m_followsSomeEnvironment)
        noteStoreIntoEnvironment();
    if (opcode == Opcode::NewObject) {
        m_uses[index] |= isUnderConstruction;
        noteFieldsLeftEmpty(value);
        followAllocation(value);
    }
    if (opcode == Opcode::Data)
        followAllocation(value);
    if (flowRuleOf(opcode) == FlowRule::ReadOfPrototypeProperty && !m_nodeReadOffOperand) {
        if (ValueID holder = value.operand(0))
            m_nodeReadOffOperand = addRead(value.id(), listOf(holder), m_program.keyOf(BuiltinNames::prototype));
    }
    if (opcode == Opcode::GetSuperBase)
        m_nodeReadOffOperand = readSuperBase(value);
    if (ValueID scope = opcode == Opcode::ScopeHasBinding ? value.operand(0) : ValueID()) {
        noteLeaving(scope);
        uint32_t unscopables = addRead(value.id(), listOf(scope), m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::Unscopables)));
        addRead(value.id(), listOfNode(unscopables), value.key());
    }
    if (opcode == Opcode::NewTemplateObject)
        addEdge(constant(typeOfElementsOfTemplateObject(value)), contentsNode(classInstanceMadeBy(value, whatIsMadeBy(value).builtinClass), ContentsSlot::Element));
    if (effects.mayCallUnknownCode && whatIsMadeBy(value).builtinClass)
        m_resultLeaves = true;

    std::optional<uint8_t> holderRead = interpretedOf(opcode) == Interpretation::Read ? holderOf(opcode) : std::nullopt;
    if (opcode == Opcode::SuperGet || opcode == Opcode::SuperSet)
        readSuperAccess(value);
    else if (answersWithResultOfBody(value))
        addNode(resultOfCall(*findCall(value.id())));
    else if (TypeID primitives = widestPrimitiveResultOf(opcode)) {
        if (resultFollowsOperandsInScan(opcode, primitives))
            addOperator(value);
        else
            addNode(constant(primitives));
    } else if (holderRead && value.operand(*holderRead) && slotNamedByRowOf(value))
        addNode(readContents(value.id(), listOf(value.operand(*holderRead)), *slotNamedByRowOf(value)));
    else if (resultRuleOf(opcode) != ResultRule::Results) {
        bool saysWhereResultIsReadFrom = resultRuleOf(opcode) == ResultRule::Unknown && provenResultRuleOf(opcode);
        ResultRulesOfScan rules { *this };
        applyResultRule(rules, value, 0, saysWhereResultIsReadFrom ? ResultSide::Proven : ResultSide::Unproven);
    }
    if (opcode == Opcode::RequireModule) {
        uint32_t answer = newJoin();
        m_summary.answersOfRequire.append({ value.as<Opcode::RequireModule>().module(), answer });
        addNode(answer);
    }
    ListInScan result = takeResult();
    m_lists[index] = result;
    if (m_resultLeaves)
        escape(result);
    for (ListInScan source : m_sourcesCopiedIntoResult) {
        addChange(SummaryChange::Kind::CopiesInto, value.id(), m_objectCopiedInto ? listOfNode(*m_objectCopiedInto) : result, { }, source);
        m_summary.changes.last().definesWhatItCopies = opcode == Opcode::CopyObject && !value.as<Opcode::CopyObject>().assigns();
    }
    if (opcode == Opcode::CopyObject) {
        for (PropertyKeyID key : value.as<Opcode::CopyObject>().excludedKeys())
            addChange(SummaryChange::Kind::Deletes, value.id(), result, key, std::nullopt);
    }
    if (m_prototypeOfResult)
        addChange(SummaryChange::Kind::BecomesPrototype, value.id(), *m_prototypeOfResult, { }, result);
}

void ScanOfBody::readAccess(const BodyValue& value, SummaryAccess::Kind kind, uint32_t block)
{
    Opcode opcode = value.opcode();
    uint32_t index = value.id().index();
    uint32_t receiverIndex = *receiverOf(opcode);
    std::optional<unsigned> storedIndex;
    if (kind == SummaryAccess::Kind::Store || kind == SummaryAccess::Kind::Define)
        storedIndex = storedOf(opcode) ? std::optional<unsigned>(*storedOf(opcode)) : OperandPosition::value(opcode);
    std::optional<unsigned> keyIndex = OperandPosition::property(opcode);

    PropertyKeyID key = m_keyOfOperation;
    if (PayloadWord::field(opcode) && value.layout() && value.field() < m_program.at(value.layout()).fields.size())
        key = m_program.at(value.layout()).fields[value.field()].key;

    ValueID object;
    ValueID stored;
    ValueID computedKey;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (operandIndex == receiverIndex) {
            object = operand;
            if (info.uses != OperandUses { OperandUse::Inspects } || !info.protocols.isEmpty())
                noteLeaving(operand);
            else if (m_followsSomeEnvironment && operand && !environmentBeingMade(m_roots[operand.index()]))
                noteLeavingOverEnvironments(m_roots[operand.index()]);
            readProtocols(value, operandIndex, info, listOf(operand), block);
            return;
        }
        if (storedIndex && *storedIndex == operandIndex) {
            stored = operand;
            noteLeaving(operand);
            return;
        }
        if (keyIndex && *keyIndex == operandIndex)
            computedKey = operand;
        readOperand(value, operand, operandIndex, info, block);
    });
    beginResult();

    StringID beginningOfKey;
    if (!key && computedKey && m_walkFollowsConstants) {
        key = nameKeyOfAccessInScan(m_program, primitiveHeldBy(computedKey), definitionBehindCasts(computedKey), scopedLambda<BodyValue(ValueID)>([&](ValueID operand) {
            return definitionBehindCasts(operand);
        }), beginningOfKey);
        if (key)
            m_summary.accessesUnderConstantKeys.append({ value.id(), key });
    }
    ListInScan subject = object ? listOf(object) : listOfNode(0);
    auto addAccessUnderKey = [&] {
        uint32_t added = addAccessUnder(kind, value.id(), subject, key, computedKey);
        m_summary.accesses[added].beginningOfKey = beginningOfKey;
        return added;
    };
    if (object) {
        uint8_t& uses = m_uses[m_roots[object.index()]];
        uses |= isAccessedByName;
        if (!key || (kind != SummaryAccess::Kind::Store && kind != SummaryAccess::Kind::Define))
            uses |= isAccessedBesideNamedStores;
        if (!key && (uses & isFollowedWhileMade) && kind != SummaryAccess::Kind::Define && kind != SummaryAccess::Kind::Has)
            noteLeaving(object);
    }

    switch (kind) {
    case SummaryAccess::Kind::Read: {
        bool comesInFrontOfDefinition = object && key && readMayComeInFrontOfDefinition(m_roots[object.index()], key);
        bool isAnsweredBySuperCall = comesInFrontOfDefinition && objectBeingMade(m_roots[object.index()]).kind == ObjectBeingMadeKindInScan::AnsweredBySuperCall;
        if (opcode == Opcode::PropGet && key) {
            m_lists[index] = { index, isReadNotYetNeeded };
            m_readsNotYetNeeded.append(index);
            if (comesInFrontOfDefinition)
                m_uses[index] |= mayComeInFrontOfDefinition;
            if (isAnsweredBySuperCall)
                m_uses[index] |= readsAnswerOfSuperCall;
            return;
        }
        uint32_t read = addAccessUnderKey();
        m_summary.accesses[read].isPartOfConstruction = comesInFrontOfDefinition;
        m_summary.accesses[read].subjectIsAnsweredBySuperCall = isAnsweredBySuperCall;
        m_lists[index] = listOfNode(resultOfRead(read));
        return;
    }
    case SummaryAccess::Kind::Has:
        addAccessUnderKey();
        if (TypeID primitives = widestPrimitiveResultOf(opcode))
            m_lists[index] = listOfNode(constant(primitives));
        else
            m_lists[index] = listOfNode(0);
        return;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define: {
        ListInScan held = stored ? listOf(stored) : listOfNode(0);
        if (key && object) {
            if (stored && kind == SummaryAccess::Kind::Store && m_program.nameClassOf(key) == NameClass::Prototype)
                m_storesOfPrototypes.append(StoreOfPrototypeInScan { static_cast<uint32_t>(m_stores.size()), m_roots[stored.index()] });
            m_stores.append(StoreInScan { value.id(), kind == SummaryAccess::Kind::Define, subject, held, key, m_roots[object.index()], block });
            noteStoreIntoObject(m_roots[object.index()], key, kind);
            return;
        }
        addSink(SinkKindInScan::ValueOfAccess, held, addAccessUnderKey());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t ScanOfBody::readSuperBase(const BodyValue& value)
{
    auto read = value.as<Opcode::GetSuperBase>();
    ValueID home = read.homeOperand();
    ListInScan homeObject = home ? listOf(home) : listOfNode(0);
    switch (read.homeKind()) {
    case HomeObjectKind::Object:
        break;
    case HomeObjectKind::ClassPrototype:
        homeObject = listOfNode(addRead(value.id(), homeObject, m_program.keyOf(BuiltinNames::prototype)));
        break;
    }
    return addRead(value.id(), homeObject, m_program.keyOf(BuiltinNames::__proto__));
}

void ScanOfBody::readSuperAccess(const BodyValue& value)
{
    if (value.opcode() == Opcode::SuperGet) {
        auto read = value.as<Opcode::SuperGet>();
        ValueID base = read.baseOperand();
        uint32_t access = addAccessUnder(SummaryAccess::Kind::Read, value.id(), base ? listOf(base) : listOfNode(0), read.key(), read.computedKeyOperand());
        m_summary.accesses[access].receiver = 0;
        if (ValueID receiver = read.receiverOperand())
            addSink(SinkKindInScan::ReceiverOfAccess, listOf(receiver), access);
        addNode(resultOfRead(access));
        return;
    }
    auto store = value.as<Opcode::SuperSet>();
    ValueID base = store.baseOperand();
    uint32_t access = addAccessUnder(SummaryAccess::Kind::Store, value.id(), base ? listOf(base) : listOfNode(0), store.key(), store.computedKeyOperand());
    m_summary.accesses[access].receiver = 0;
    if (ValueID receiver = store.receiverOperand())
        addSink(SinkKindInScan::ReceiverOfAccess, listOf(receiver), access);
    m_summary.accesses[access].value = 0;
    if (ValueID stored = store.valueOperand())
        addSink(SinkKindInScan::ValueOfAccess, listOf(stored), access);
}

void ScanOfBody::readNewClass(const BodyValue& value)
{
    auto evaluation = value.as<Opcode::NewClass>();
    m_summary.classesEvaluated.append(evaluation.layout());
    ValueID constructor = evaluation.constructorOperand();
    ListInScan classObject = constructor ? listOf(constructor) : listOfNode(0);
    if (constructor)
        noteLeaving(constructor);

    if (ValueID heritage = evaluation.heritageOperand()) {
        noteLeaving(heritage);
        addChange(SummaryChange::Kind::BecomesPrototype, value.id(), listOf(heritage), { }, classObject);
    }
    if (ValueID parent = evaluation.prototypeParentOperand()) {
        noteLeaving(parent);
        uint32_t prototypeObject = addRead(value.id(), classObject, m_program.keyOf(BuiltinNames::prototype));
        addChange(SummaryChange::Kind::BecomesPrototype, value.id(), listOf(parent), { }, listOfNode(prototypeObject));
    }
    m_lists[value.id().index()] = classObject;
}

void ScanOfBody::readDefineAccessor(const BodyValue& value, ValueID object, ValueID function, PropertyKeyID key, SummaryChange::Lays lays)
{
    if (function)
        noteLeaving(function);
    if (object)
        m_uses[m_roots[object.index()]] |= isAccessedBesideNamedStores;
    if (object && key) {
        uint32_t root = m_roots[object.index()];
        FunctionID half;
        if (function) {
            if (BodyValue closure = definitionOf(m_roots[function.index()]); closure && closure.opcode() == Opcode::NewClosure)
                half = closure.as<Opcode::NewClosure>().function();
        }
        m_accessorsLaid.append(AccessorLaidInScan { root, key, half, lays == SummaryChange::Lays::Getter });
        if ((m_uses[root] & isFollowedWhileMade) && objectBeingMade(root).kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            noteFill(root, false, key.bits());
            if (half)
                noteFill(root, true, half.index());
        }
        m_placesOfAccessorsLaid.add(static_cast<uint64_t>(m_roots[object.index()]) << 32 | key.bits());
    }
    uint32_t change = static_cast<uint32_t>(m_summary.changes.size());
    addChange(SummaryChange::Kind::DefinesByDescriptor, value.id(), object ? listOf(object) : listOfNode(0), key, function ? listOf(function) : listOfNode(0));
    m_summary.changes[change].lays = lays;
}

void ScanOfBody::readDefineMethod(const BodyValue& value)
{
    auto definition = value.as<Opcode::DefineMethod>();
    uint32_t place = static_cast<uint32_t>(m_summary.members.size());
    SummaryMember member;
    member.classLayout = definition.layout();
    member.side = definition.side();
    member.kind = definition.methodKind();
    member.index = definition.index();
    member.key = definition.key();
    m_summary.members.append(member);

    if (ValueID classObject = definition.classObjectOperand())
        noteLeaving(classObject);
    ValueID function = definition.functionOperand();
    if (function)
        noteLeaving(function);
    addSink(SinkKindInScan::ValueOfMember, function ? listOf(function) : ListInScan(), place);
}

bool ScanOfBody::standsRightBehindCallThatGives(const BodyValue& value, ValueID operand) const
{
    uint32_t given = m_roots[operand.index()];
    BodyValue definition = definitionOf(given);
    if (!definition || definition.opcode() != Opcode::Call)
        return false;
    for (uint32_t between = given + 1; between < value.id().index(); ++between) {
        if (m_roots[between] != given)
            return false;
    }
    return true;
}

void ScanOfBody::readOperand(const BodyValue& value, ValueID operand, uint32_t operandIndex, const OperandInfo& info, uint32_t block)
{
    ListInScan list = listOf(operand);
    bool isHandedOn = !operandIndex && inputOperationsThatHandOn.contains(value.opcode());
    if (!isHandedOn && (info.uses != OperandUses { OperandUse::Inspects } || !info.protocols.isEmpty()))
        noteLeaving(operand);

    if (info.protocols.contains(Protocol::Thenable)) {
        size_t numberOfAccesses = m_summary.accesses.size();
        list = settle(value.id(), list, block);
        if (m_summary.accesses.size() > numberOfAccesses && standsRightBehindCallThatGives(value, operand))
            m_summary.accesses[numberOfAccesses].standsRightBehindCall = true;
        m_operandSettled = operandIndex;
        m_whatOperandSettlesTo = list;
    }

    for (OperandUse use : info.uses) {
        switch (use) {
        case OperandUse::Inspects:
        case OperandUse::MovesElements:
            break;
        case OperandUse::WritesContents:
            if (info.kind == OperandKind::Array)
                storeInspectedValueIntoList(value, list);
            break;
        case OperandUse::Retains:
            retain(value, operandIndex, info, list);
            break;
        case OperandUse::Returns:
            addList(list);
            break;
        case OperandUse::Calls:
        case OperandUse::CallsPerElement:
        case OperandUse::CallsRepeatedly:
        case OperandUse::PassesToUserCode:
        case OperandUse::AppliesTransitively:
        case OperandUse::AppliesToContents:
        case OperandUse::StoresByKey:
            handToCodeThatRuns(list);
            break;
        case OperandUse::ReadsContents:
            if (info.kind != OperandKind::Array && !someResultIsReadOutOf(value, operandIndex))
                escape(list);
            break;
        case OperandUse::RetainsContents:
            if (!info.protocols.contains(Protocol::Iterate))
                retainContents(value, listOfNode(info.kind == OperandKind::Array ? readContents(value.id(), list, ContentsSlot::Element) : 0));
            break;
        case OperandUse::DeletesByKey:
            addChange(SummaryChange::Kind::Deletes, value.id(), list, m_keyOfOperation, std::nullopt);
            break;
        case OperandUse::AssignsFrom: {
            ValueID source = info.assignedFromOperand ? value.operand(*info.assignedFromOperand) : ValueID();
            addChange(SummaryChange::Kind::CopiesInto, value.id(), list, { }, source ? listOf(source) : listOfNode(0));
            m_summary.changes.last().definesWhatItCopies = !declaredBoundInTable(value.opcode()).mayCallUnknownCode;
            break;
        }
        case OperandUse::DefinesName:
        case OperandUse::TakesDescriptor:
            defineByDescriptor(value.id(), list, m_keyOfOperation, std::nullopt);
            break;
        case OperandUse::ChangesIntegrity:
            addChange(SummaryChange::Kind::Freezes, value.id(), list, { }, std::nullopt);
            break;
        case OperandUse::ChangesPrototype: {
            ValueID prototype = operandUsedAs(value, OperandUse::BecomesPrototype);
            addChange(SummaryChange::Kind::ChangesPrototype, value.id(), list, { }, prototype ? listOf(prototype) : listOfNode(0));
            break;
        }
        case OperandUse::BecomesPrototype: {
            ValueID below = info.retainingOperand ? value.operand(*info.retainingOperand) : ValueID();
            addChange(SummaryChange::Kind::BecomesPrototype, value.id(), list, { }, below ? std::optional(listOf(below)) : std::nullopt);
            break;
        }
        }
    }
    readProtocols(value, operandIndex, info, list, block);
}

void ScanOfBody::readProtocols(const BodyValue& value, uint32_t operandIndex, const OperandInfo& info, ListInScan list, uint32_t block)
{
    ValueID site = value.id();
    const BuiltinTable& builtins = m_program.builtins();
    for (Protocol protocol : info.protocols) {
        switch (protocol) {
        case Protocol::OwnKeys:
            m_summary.traits.add(SummaryTrait::ListsKeys);
            noteInspection(site, ThinSummary::Inspection::Kind::ListsKeys, list);
            break;
        case Protocol::OwnValues:
            readOwnValues(value, operandIndex, list);
            break;
        case Protocol::ToPrimitive:
            convertToPrimitive(site, list);
            break;
        case Protocol::Thenable:
            if (m_operandSettled != operandIndex)
                settle(site, list, block);
            break;
        case Protocol::Species:
            if (!isAlwaysPrimitive(list))
                constructBySpecies(site, objectsAmong(list), block);
            break;
        case Protocol::Iterate:
            readIteration(value, info, list, block);
            break;
        case Protocol::ToJSON:
            for (BuiltinName name : builtins.keysReadBy(protocol))
                handToCodeThatRuns(listOfNode(resultOfCall(callMember(site, list, name, { listOfNode(constant(TypeTable::string)) }, block))));
            break;
        case Protocol::CallsNamed:
            if (info.namedMembers.empty()) {
                m_summary.ownEffects.merge(Effects::unknownCode());
                handToCodeThatRuns(list);
                break;
            }
            for (BuiltinName name : info.namedMembers) {
                uint32_t call = callMember(site, list, name, { listOfNode(0) }, block);
                m_summary.calls[call].passesList = true;
                handToCodeThatRuns(listOfNode(resultOfCall(call)));
            }
            break;
        case Protocol::HasInstance:
            callHasInstance(value, operandIndex, list, block);
            break;
        case Protocol::PrototypeChain:
            noteInspection(site, ThinSummary::Inspection::Kind::WalksChain, list);
            break;
        case Protocol::Named:
            for (BuiltinName name : info.namedMembers) {
                uint32_t read = addRead(value.id(), list, m_program.keyOf(name));
                if (flowRuleOf(value.opcode()) == FlowRule::ReadOfPrototypeProperty)
                    m_nodeReadOffOperand = read;
            }
            break;
        }
    }
}

void ScanOfBody::readOwnValues(const BodyValue& value, uint32_t operandIndex, ListInScan list)
{
    Opcode opcode = value.opcode();
    bool isAssignedFrom = false;
    value.forEachOperand([&](ValueID, uint32_t otherIndex) {
        std::optional<uint8_t> source = operandInfoOf(opcode, otherIndex).assignedFromOperand;
        isAssignedFrom |= source && *source == operandIndex;
    });
    if (isAssignedFrom)
        return;
    if (flowOf(opcode) == ValueFlowEdgeClass::Allocates)
        m_sourcesCopiedIntoResult.append(list);
    else
        handToCodeThatRuns(list);
}

void ScanOfBody::convertToPrimitive(ValueID site, ListInScan list)
{
    if (isAlwaysPrimitive(list))
        return;
    while (m_isConvertedToPrimitive.size() <= list.first)
        m_isConvertedToPrimitive.append(false);
    if (std::exchange(m_isConvertedToPrimitive[list.first], true))
        return;
    addSink(SinkKindInScan::ConvertedToPrimitive, list, site.index());
}

void ScanOfBody::constructBySpecies(ValueID site, ListInScan list, uint32_t block)
{
    uint32_t constructor = addRead(site, list, m_program.keyOf(BuiltinNames::constructor));
    uint32_t species = addRead(site, listOfNode(constructor), m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::Species)));

    uint32_t callIndex = static_cast<uint32_t>(m_summary.calls.size());
    SummaryCall call;
    call.site = site;
    call.isConstruction = true;
    call.subject = species;
    call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
    call.numberOfArguments = 1;
    m_summary.arguments.append(constant(TypeTable::number));
    m_summary.calls.append(call);
    m_blocksOfCalls.append(block);
    addNode(resultOfCall(callIndex));
}

ListInScan ScanOfBody::openIterator(ValueID site, ListInScan iterable, IteratorKind kind, uint32_t block)
{
    const BuiltinTable& builtins = m_program.builtins();
    ListInScan iterator = listOfNode(resultOfCall(callMember(site, iterable, builtins.nameOf(WellKnownSymbol::Iterator), { }, block)));
    switch (kind) {
    case IteratorKind::Sync:
        addRead(site, iterator, m_program.keyOf(BuiltinNames::next));
        return iterator;
    case IteratorKind::Async:
        handToCodeThatRuns(iterator);
        handToCodeThatRuns(listOfNode(resultOfCall(callMember(site, iterable, builtins.nameOf(WellKnownSymbol::AsyncIterator), { }, block))));
        return listOfNode(0);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t ScanOfBody::stepIterator(ValueID site, ListInScan iterator, BuiltinName method, std::optional<ListInScan> sent, uint32_t block)
{
    uint32_t call = sent ? callMember(site, iterator, method, { *sent }, block) : callMember(site, iterator, method, { }, block);
    ListInScan result = listOfNode(resultOfCall(call));
    addRead(site, result, m_program.keyOf(BuiltinNames::done));
    return addRead(site, result, m_program.keyOf(BuiltinNames::value));
}

void ScanOfBody::readIteration(const BodyValue& value, const OperandInfo& info, ListInScan list, uint32_t block)
{
    Opcode opcode = value.opcode();
    ValueID site = value.id();
    if (opcode == Opcode::IteratorOpen) {
        if (value.as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync) {
            for (IterationHelper helper : { IterationHelper::OpenIterator, IterationHelper::StepIterator, IterationHelper::CloseIterator, IterationHelper::CloseIteratorAfterThrow, IterationHelper::NextOfStepsOverText }) {
                if (!functionOfIterationHelper(m_program, helper))
                    continue;
                SummaryCall call;
                call.site = site;
                call.callee = SummaryCall::Callee::Builtin;
                call.isMadeByProtocol = true;
                call.builtin = m_program.builtins().canonicalID(builtinOfIterationHelper(helper));
                call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
                m_summary.calls.append(call);
                m_blocksOfCalls.append(block);
            }
        }
        if (value.as<Opcode::IteratorOpen>().isDestructuring() && functionOfIterationHelper(m_program, IterationHelper::TakeFromIterable)) {
            SummaryCall call;
            call.site = site;
            call.callee = SummaryCall::Callee::Builtin;
            call.isMadeByProtocol = true;
            call.builtin = m_program.builtins().canonicalID(builtinOfIterationHelper(IterationHelper::TakeFromIterable));
            call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
            m_summary.calls.append(call);
            m_blocksOfCalls.append(block);
        }
        addList(openIterator(site, list, value.as<Opcode::IteratorOpen>().iteratorKind(), block));
        return;
    }
    if (info.kind == OperandKind::IteratorState && !stepsSynchronousIteratorInScan(value)) {
        handToCodeThatRuns(list);
        addNode(0);
        return;
    }
    if (opcode == Opcode::IteratorClose) {
        callMember(site, list, BuiltinNames::returnKeyword, { }, block);
        return;
    }
    if (opcode == Opcode::IteratorResume) {
        auto resume = value.as<Opcode::IteratorResume>();
        std::optional<ListInScan> sent;
        if (ValueID operand = resume.sentOperand())
            sent = listOf(operand);
        switch (resume.resumeMode()) {
        case ResumeMode::Next:
            addNode(stepIterator(site, list, BuiltinNames::next, sent, block));
            break;
        case ResumeMode::Throw:
            addNode(stepIterator(site, list, BuiltinNames::throwKeyword, sent, block));
            callMember(site, list, BuiltinNames::returnKeyword, { }, block);
            break;
        case ResumeMode::Return:
            addNode(stepIterator(site, list, BuiltinNames::returnKeyword, sent, block));
            if (sent)
                addList(*sent);
            break;
        }
        addNode(constant(TypeTable::undefined));
        return;
    }

    if (opcode == Opcode::AppendSpread) {
        for (IterationHelper helper : { IterationHelper::AppendGenerator, IterationHelper::AppendIterable }) {
            if (!functionOfIterationHelper(m_program, helper))
                continue;
            SummaryCall call;
            call.site = site;
            call.callee = SummaryCall::Callee::Builtin;
            call.isMadeByProtocol = true;
            call.builtin = m_program.builtins().canonicalID(builtinOfIterationHelper(helper));
            call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
            m_summary.calls.append(call);
            m_blocksOfCalls.append(block);
        }
    }

    ListInScan iterator = openIterator(site, list, IteratorKind::Sync, block);
    uint32_t handedOut = stepIterator(site, iterator, BuiltinNames::next, std::nullopt, block);
    callMember(site, iterator, BuiltinNames::returnKeyword, { }, block);
    if (info.uses.contains(OperandUse::RetainsContents))
        retainContents(value, listOfNode(handedOut));
    else
        escape(listOfNode(handedOut));
}

void ScanOfBody::callHasInstance(const BodyValue& value, uint32_t operandIndex, ListInScan constructor, uint32_t block)
{
    uint32_t callIndex = static_cast<uint32_t>(m_summary.calls.size());
    SummaryCall call;
    call.site = value.id();
    call.callee = SummaryCall::Callee::Name;
    call.isMadeByProtocol = true;
    call.key = m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::HasInstance));
    call.receiver = 0;
    call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
    value.forEachOperand([&](ValueID operand, uint32_t otherIndex) {
        if (otherIndex != operandIndex)
            addArgument(operand);
    });
    call.numberOfArguments = static_cast<uint32_t>(m_summary.arguments.size()) - call.firstArgument;
    m_summary.calls.append(call);
    m_blocksOfCalls.append(block);
    addSink(SinkKindInScan::ReceiverOfCall, constructor, callIndex);
}

ValueID ScanOfBody::operandUsedAs(const BodyValue& value, OperandUse use) const
{
    ValueID found;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!found && operandUsesOf(value.opcode(), operandIndex).contains(use))
            found = operand;
    });
    return found;
}

MadeByOperationInScan ScanOfBody::whatIsMadeBy(const BodyValue& value) const
{
    ResultRule rule = resultRuleOf(value.opcode());
    if (rule != ResultRule::Natural && rule != ResultRule::Allocation)
        return { };
    RulesOfWhatIsMadeInScan rules;
    return applyResultRule(rules, value, 0, ResultSide::Unproven);
}

void ScanOfBody::retain(const BodyValue& value, uint32_t operandIndex, const OperandInfo& info, ListInScan list)
{
    switch (info.retainedIn) {
    case RetainedIn::UnknownMemory:
        if (value.opcode() == Opcode::PutModuleVariable) {
            VariableID variable = value.as<Opcode::PutModuleVariable>().variable();
            if (m_program.at(variable).initialization == VariableInitialization::Function)
                m_summary.functionVariablesStoredInto.append(variable);
            flowInto(list, variableNode(variable));
        }
        else if (m_operandSettled == operandIndex && someResultIsReadOutOf(value, operandIndex))
            return;
        else if (std::optional<SlotOfContents> slot = slotNamedByRowOf(value); slot && !holderOf(value.opcode()) && m_function->isCoroutine() && !m_function->flags.containsAll({ FunctionFlags::IsGenerator, FunctionFlags::IsAsync }))
            flowInto(list, contentsNode(objectOfCoroutine(), *slot));
        else
            escape(list);
        return;
    case RetainedIn::Result:
        retainInResult(value, operandIndex, list);
        return;
    case RetainedIn::Operand:
        retainInOperand(value, info, list);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WrittenUnit unitWrittenInScan(const BodyValue& definition)
{
    if (!definition)
        return { };
    switch (definition.opcode()) {
    case Opcode::String:
        return WrittenUnit::text(definition.as<Opcode::String>().string());
    case Opcode::Integer:
        return WrittenUnit::integer(definition.as<Opcode::Integer>().integer());
    case Opcode::Boolean:
        return WrittenUnit::boolean(definition.as<Opcode::Boolean>().boolean());
    default:
        return { };
    }
}

void ScanOfBody::retainInResult(const BodyValue& value, uint32_t operandIndex, ListInScan list)
{
    Opcode opcode = value.opcode();
    std::optional<unsigned> prototype = OperandPosition::prototype(opcode);
    if (prototype && *prototype == operandIndex) {
        m_prototypeOfResult = list;
        return;
    }

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    bool isListed = operandIndex >= numberOfNamedOperands;
    uint32_t place = operandIndex - numberOfNamedOperands;
    if (opcode == Opcode::NewClosure && isListed) {
        flowInto(list, captureNode(value.as<Opcode::NewClosure>().function(), place));
        return;
    }

    if (opcode != Opcode::NewObject || !isListed) {
        MadeByOperationInScan made = whatIsMadeBy(value);
        if (made.isOpaque)
            return;
        if (isListed && made.builtinClass && m_program.builtins().findSlot(made.builtinClass, ContentsSlot::Element)) {
            flowInto(list, contentsNode(classInstanceMadeBy(value, made.builtinClass), ContentsSlot::Element));
            return;
        }
        escape(list);
        m_resultLeaves = true;
        return;
    }

    auto allocation = value.as<Opcode::NewObject>();
    LayoutID layout = allocation.layout();
    std::span<const uint32_t> fields = allocation.fields();
    if (isFirstValueThatMayGoUnseen(value, value.operand(operandIndex)))
        return;
    uint32_t node = place < fields.size() ? fieldNode(layout, fields[place]) : 0;
    if (WrittenUnit unit = node && list.count == 1 ? unitWrittenInScan(definitionBehindCasts(value.operand(operandIndex))) : WrittenUnit()) {
        uint32_t atom = m_atoms[list.first];
        if (!(atom & (parameterBit | operatorBit)) && m_summary.nodes[atom].kind == SummaryNode::Kind::Constant)
            list = listOfNode(nodeFor(SummaryNode::Kind::Constant, m_summary.nodes[atom].first, unit.bits()));
    }
    flowInto(list, node);
    if (!node) {
        m_resultLeaves = true;
        return;
    }
    if (!prototypeMaySelectOtherObject(value, layout))
        return;
    PropertyKeyID key = m_program.at(layout).fields[fields[place]].key;
    for (LayoutID below : m_program.layoutsExpectedBelow(layout)) {
        if (std::optional<uint32_t> field = m_program.at(below).findField(key))
            flowInto(list, fieldNode(below, *field));
    }
}

void ScanOfBody::retainInOperand(const BodyValue& value, const OperandInfo& info, ListInScan list)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::PutEnvironmentField) {
        auto store = value.as<Opcode::PutEnvironmentField>();
        flowInto(list, fieldNode(store.layout(), store.field()));
        return;
    }
    if (opcode == Opcode::TryReExportFields) {
        auto reExport = value.as<Opcode::TryReExportFields>();
        uint32_t captureOfKey = reExport.keyIsFirstCapture() ? 0 : 1;
        flowInto(list, captureNode(reExport.function(), 1 - captureOfKey));
        addEdge(constant(TypeTable::string), captureNode(reExport.function(), captureOfKey));
    }

    if (ValueID holder = info.retainingOperand ? value.operand(*info.retainingOperand) : ValueID()) {
        const OperandInfo& infoOfHolder = operandInfoOf(opcode, *info.retainingOperand);
        if (infoOfHolder.kind == OperandKind::Array && infoOfHolder.uses.contains(OperandUse::WritesContents)) {
            storeIntoContents(value.id(), listOf(holder), ContentsSlot::Element, list);
            return;
        }
        if (std::optional<SlotOfContents> slot = slotNamedByRowOf(value); slot && holderOf(opcode) == info.retainingOperand) {
            storeIntoContents(value.id(), listOf(holder), *slot, list);
            return;
        }
        if (!infoOfHolder.uses.containsAny({ OperandUse::DefinesName, OperandUse::TakesDescriptor }))
            escape(listOf(holder));
    }
    escape(list);
}

void ScanOfBody::retainContents(const BodyValue& value, ListInScan held)
{
    Opcode opcode = value.opcode();
    MadeByOperationInScan made = whatIsMadeBy(value);
    if (made.builtinClass && m_program.builtins().findSlot(made.builtinClass, ContentsSlot::Element)) {
        flowInto(held, contentsNode(classInstanceMadeBy(value, made.builtinClass), ContentsSlot::Element));
        return;
    }
    bool isKept = false;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        const OperandInfo& other = operandInfoOf(opcode, operandIndex);
        if (other.kind != OperandKind::Array || !other.uses.contains(OperandUse::WritesContents))
            return;
        storeIntoContents(value.id(), listOf(operand), ContentsSlot::Element, held);
        isKept = true;
    });
    if (!isKept)
        escape(held);
}

void ScanOfBody::storeInspectedValueIntoList(const BodyValue& value, ListInScan list)
{
    Opcode opcode = value.opcode();
    std::optional<unsigned> position = OperandPosition::value(opcode);
    if (!position || operandUsesOf(opcode, *position).contains(OperandUse::Retains))
        return;
    if (ValueID stored = value.operand(*position)) {
        noteLeaving(stored);
        storeIntoContents(value.id(), list, ContentsSlot::Element, listOf(stored));
    }
}

TypeID ScanOfBody::typeOfEmptySlotOfEnvironment(LayoutID id, uint32_t field) const
{
    TypeID written = m_program.at(id).fields[field].declared.written();
    if (written && (m_program.at(written).primitives & Primitive::Absent))
        return TypeTable::absent;
    return typeOfPrimitives(Primitive::Undefined | Primitive::Absent);
}

void ScanOfBody::noteFieldsLeftEmpty(const BodyValue& value)
{
    auto allocation = value.as<Opcode::NewObject>();
    LayoutID id = allocation.layout();
    if (!id || !m_program.at(id).isEnvironment())
        return;
    m_summary.environmentsMade.append(value.id());
    if (followsEnvironmentsOfBody()) {
        followEnvironment(value);
        return;
    }
    uint32_t numberOfFields = static_cast<uint32_t>(m_program.at(id).fields.size());
    m_fieldIsListed.fill(false, numberOfFields);
    for (uint32_t field : allocation.fields()) {
        if (field < numberOfFields)
            m_fieldIsListed[field] = true;
    }
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        if (!m_fieldIsListed[field])
            addEdge(constant(typeOfEmptySlotOfEnvironment(id, field)), fieldNode(id, field));
    }
}

ObjectBeingMadeInScan& ScanOfBody::objectBeingMade(uint32_t root)
{
    auto found = std::ranges::lower_bound(m_objectsBeingMade, root, { }, &ObjectBeingMadeInScan::root);
    RELEASE_ASSERT(found != m_objectsBeingMade.end() && found->root == root);
    return *found;
}

void ScanOfBody::follow(uint32_t root, ObjectBeingMadeKindInScan kind)
{
    RELEASE_ASSERT(m_objectsBeingMade.isEmpty() || m_objectsBeingMade.last().root < root);
    m_uses[root] |= isFollowedWhileMade;
    m_objectsBeingMade.append(ObjectBeingMadeInScan { .root = root, .kind = kind });
}

void ScanOfBody::followThis(const BodyBlock& entry)
{
    bool isInitializer = m_function->kind == FunctionKind::Initializer;
    std::optional<unsigned> parameter = m_function->thisParameterIndex();
    if (!parameter || *parameter >= entry.numberOfParameters() || (!isInitializer && (!m_function->takesThisFromConstruction() || !m_function->constructedClass)))
        return;
    m_thisBeingMade = entry.parameter(*parameter).index();
    follow(m_thisBeingMade, ObjectBeingMadeKindInScan::This);
    if (isInitializer)
        m_uses[m_thisBeingMade] |= isUnderConstruction;
}

static const DataNode::Object* objectOfDataInScan(const Program& program, const BodyValue& value)
{
    DataID data = value.as<Opcode::Data>().data();
    return data ? program.at(data).root.tryAs<DataNode::Object>() : nullptr;
}

LayoutID ScanOfBody::layoutOfAllocation(const BodyValue& made) const
{
    if (made.opcode() == Opcode::NewObject)
        return made.as<Opcode::NewObject>().layout();
    const DataNode::Object* object = objectOfDataInScan(m_program, made);
    return object ? object->layout : LayoutID();
}

void ScanOfBody::followAllocation(const BodyValue& value)
{
    LayoutID id = layoutOfAllocation(value);
    if (!id)
        return;
    const Layout& layout = m_program.at(id);
    if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance && layout.kind != LayoutKind::Container)
        return;
    if (value.opcode() == Opcode::NewObject) {
        std::span<const uint32_t> listed = value.as<Opcode::NewObject>().fields();
        bool beginsDeclaredOrder = !writtenBaseOf(layout) && listed.size() <= layout.declaredOrder.size() && std::ranges::equal(listed, layout.declaredOrder.span().first(listed.size()));
        if (!beginsDeclaredOrder) {
            m_summary.fieldsListedByAllocations.append(id.index());
            m_summary.fieldsListedByAllocations.append(static_cast<uint32_t>(listed.size()));
            m_summary.fieldsListedByAllocations.append(listed);
        }
    }
    if (!prototypeMaySelectOtherObject(value, id)) {
        size_t numberListed = value.opcode() == Opcode::NewObject ? value.as<Opcode::NewObject>().fields().size() : static_cast<size_t>(std::ranges::count_if(objectOfDataInScan(m_program, value)->members, [&](const DataMember& member) {
            return layout.findField(member.key).has_value();
        }));
        if (numberListed < layout.fields.size())
            follow(value.id().index(), ObjectBeingMadeKindInScan::OfOneLayout);
        return;
    }
    if (m_function->kind == FunctionKind::BaseConstructor && m_function->constructedClass == id) {
        follow(value.id().index(), ObjectBeingMadeKindInScan::Allocated);
        return;
    }
    leaveEveryFieldEmpty(id);
    for (LayoutID below : m_program.layoutsExpectedBelow(id))
        leaveEveryFieldEmpty(below);
}

void ScanOfBody::leaveEveryFieldEmpty(LayoutID id)
{
    uint32_t numberOfFields = static_cast<uint32_t>(m_program.at(id).fields.size());
    for (uint32_t field = 0; field < numberOfFields; ++field)
        noteFieldLeftEmptyForGood(FieldReference { id, field });
}

bool ScanOfBody::isListedByAllocation(uint32_t root, PropertyKeyID key) const
{
    BodyValue made = definitionOf(root);
    if (made && made.opcode() == Opcode::Data) {
        const DataNode::Object* object = objectOfDataInScan(m_program, made);
        return object && std::ranges::contains(object->members, key, &DataMember::key);
    }
    if (!made || made.opcode() != Opcode::NewObject)
        return false;
    auto allocation = made.as<Opcode::NewObject>();
    std::optional<uint32_t> field = m_program.at(allocation.layout()).findField(key);
    return field && std::ranges::contains(allocation.fields(), *field);
}

std::span<const FillInScan> ScanOfBody::fillsOf(uint32_t root, std::span<const FillInScan> fills) const
{
    auto [first, end] = std::ranges::equal_range(fills, root, { }, &FillInScan::root);
    return fills.subspan(first - fills.begin(), end - first);
}

void ScanOfBody::noteFill(uint32_t root, bool isPart, uint32_t number)
{
    FillInScan fill { root, isPart, number };
    const ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.isGivenUp || (object.hasLeft && object.hasEnded && !std::ranges::binary_search(object.filled, fill) && !std::ranges::binary_search(object.filledAtEnd, fill)))
        return;
    m_fills.add(fill);
}

void ScanOfBody::noteLookUp(uint32_t root, PropertyKeyID key)
{
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.kind != ObjectBeingMadeKindInScan::OfOneLayout) {
        if (!object.keysLookedUp.contains(key))
            object.keysLookedUp.append(key);
        return;
    }
    const Layout& layout = m_program.at(layoutOfAllocation(definitionOf(root)));
    std::optional<uint32_t> field = layout.findField(key);
    const PropertyKey& record = m_program.at(key);
    bool isOpenedAbove = record.kind == PropertyKeyKind::Text && m_program.isOpenedSomewhere(record.text);
    BuiltinID objectPrototype = m_program.builtins().canonicalID(Builtins::ObjectPrototype);
    bool mayBeReplacedAbove = m_program.admitsReplacedBuiltins && findOwnMember(m_program, objectPrototype, key) && !isLockedLocation(m_program, objectPrototype, key);
    if (isOpenedAbove || mayBeReplacedAbove || (field && layout.fields[*field].form.contains(FieldForm::IsAccessor)) || hasAccessorLaidUnder(root, key))
        noteLeaving(ValueID(root));
}

static bool isAbsentFromObjectPrototypeInScan(const Program& program, PropertyKeyID key)
{
    if (isOnObjectPrototype(program.nameClassOf(key)))
        return false;
    const PropertyKey& record = program.at(key);
    if (record.kind == PropertyKeyKind::Text && program.isOpenedSomewhere(record.text))
        return false;
    BuiltinName name = program.builtinNameOf(key);
    if (!name)
        return true;
    const BuiltinTable& builtins = program.builtins();
    BuiltinClassID objectClass = BuiltinClasses::Object;
    return builtins.findMember(builtins.at(objectClass).prototype, name).outcome == BuiltinMemberLookup::Outcome::Absent;
}

void ScanOfBody::noteStoreIntoObject(uint32_t root, PropertyKeyID key, SummaryAccess::Kind kind)
{
    if (!(m_uses[root] & isFollowedWhileMade))
        return;
    if (kind == SummaryAccess::Kind::Store) {
        noteLookUp(root, key);
        if (objectBeingMade(root).kind == ObjectBeingMadeKindInScan::OfOneLayout && !isAbsentFromObjectPrototypeInScan(m_program, key))
            return;
    }
    if (kind == SummaryAccess::Kind::Define) {
        if (ObjectBeingMadeInScan& object = objectBeingMade(root); object.kind == ObjectBeingMadeKindInScan::OfOneLayout)
            object.definedInTurn.append(key);
    }
    noteFill(root, false, key.bits());
}

bool ScanOfBody::readMayComeInFrontOfDefinition(uint32_t root, PropertyKeyID key)
{
    if (!(m_uses[root] & isFollowedWhileMade))
        return false;
    noteLookUp(root, key);
    return !hasFilledInFrontOfEveryLeaving(root, key) && !isListedByAllocation(root, key);
}

void ScanOfBody::meetFillsWhereObjectLeaves(ObjectBeingMadeInScan& object, std::span<const FillInScan> fillsOfObject)
{
    if (object.isGivenUp)
        return;
    if (!std::exchange(object.hasLeft, true)) {
        object.filled.append(fillsOfObject);
        return;
    }
    object.filled.removeAllMatching([&](const FillInScan& fill) {
        return !std::ranges::binary_search(fillsOfObject, fill);
    });
}

void ScanOfBody::giveUp(uint32_t root)
{
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    object.isGivenUp = true;
    object.hasLeft = true;
    object.filled.shrink(0);
    object.hasEnded = true;
    object.filledAtEnd.shrink(0);
    m_firstLeft[root] = 0;
}

void ScanOfBody::handToBlock(ValueID value, uint32_t from, uint32_t block, uint32_t position, KindOfEdgeInScan kind)
{
    noteUseByBodyOfModule(value);
    uint32_t root = m_roots[value.index()];
    if (!(m_uses[root] & isFollowedWhileMade) || block <= from || kind == KindOfEdgeInScan::ToHandler) {
        m_uses[root] |= isAccessedBesideNamedStores;
        noteLeaving(value);
        return;
    }
    std::span<const FillInScan> fills = fillsOf(root, m_fills.inOrder());
    m_objectsHandedToBlocks.append(ObjectHandedToBlockInScan { block, position, root, m_position, { static_cast<uint32_t>(m_fillsKept.size()), static_cast<uint32_t>(fills.size()) } });
    m_fillsKept.append(fills);
}

void ScanOfBody::takeObjectsHandedTo(const BodyBlock& block)
{
    uint32_t number = block.id().index();
    for (size_t index = 0; index < m_objectsHandedToBlocks.size();) {
        ObjectHandedToBlockInScan handed = m_objectsHandedToBlocks[index];
        if (handed.block != number) {
            ++index;
            continue;
        }
        m_objectsHandedToBlocks[index] = m_objectsHandedToBlocks.last();
        m_objectsHandedToBlocks.removeLast();

        bool hasParameter = handed.position < block.numberOfParameters();
        if (hasParameter && m_numberOfEdgesInto[number] == 1) {
            m_roots[block.parameter(handed.position).index()] = handed.root;
            m_blocksThatTookObjects.append(BlockThatTookObjectInScan { number, handed.root });
            m_summary.otherNamesOfObjectsBeingMade.append(OtherNameOfObjectBeingMade { block.parameter(handed.position), ValueID(handed.root) });
            continue;
        }
        ObjectBeingMadeInScan& object = objectBeingMade(handed.root);
        m_firstLeft[handed.root] = std::min(m_firstLeft[handed.root], handed.placeInStream);
        if (hasParameter)
            object.parametersHandedTo.append(block.parameter(handed.position).index());
        meetFillsWhereObjectLeaves(object, m_fillsKept.span().subspan(handed.fills.first, handed.fills.count));
    }
}

void ScanOfBody::carryFillsOver(uint32_t from, uint32_t to)
{
    ++m_numberOfEdgesInto[to];
    if (m_objectsBeingMade.isEmpty())
        return;

    ListInScan& kept = m_fillsAtBlocks[to];
    if (to <= from) {
        if (kept.count != noFillsYet) {
            for (const FillInScan& fill : m_fillsKept.span().subspan(kept.first, kept.count)) {
                if (!m_fills.contains(fill))
                    giveUp(fill.root);
            }
        }
        for (const BlockThatTookObjectInScan& taken : m_blocksThatTookObjects) {
            if (taken.block == to)
                giveUp(taken.root);
        }
        return;
    }

    if (kept.count == noFillsYet) {
        std::span<const FillInScan> fills = m_fills.inOrder();
        kept = { static_cast<uint32_t>(m_fillsKept.size()), static_cast<uint32_t>(fills.size()) };
        m_fillsKept.append(fills);
        return;
    }
    uint32_t numberKept = 0;
    for (uint32_t index = 0; index < kept.count; ++index) {
        FillInScan fill = m_fillsKept[kept.first + index];
        if (m_fills.contains(fill))
            m_fillsKept[kept.first + numberKept++] = fill;
    }
    kept.count = numberKept;
}

void ScanOfBody::listDefinitionsBehindAllocation(const ObjectBeingMadeInScan& object)
{
    BodyValue made = definitionOf(object.root);
    if (object.definedInTurn.isEmpty() || object.isGivenUp || !made || made.opcode() != Opcode::NewObject)
        return;
    LayoutID id = made.as<Opcode::NewObject>().layout();
    const Layout& layout = m_program.at(id);
    Vector<uint32_t>& lists = m_summary.fieldsListedByAllocations;
    size_t start = lists.size();
    lists.append(id.index());
    lists.append(0);
    lists.append(made.as<Opcode::NewObject>().fields());
    for (PropertyKeyID key : object.definedInTurn) {
        std::optional<uint32_t> field = layout.findField(key);
        lists.append(field ? *field : ThinSummary::keyInListOfAllocation | key.index());
    }
    std::span<const uint32_t> listed = lists.span().subspan(start + 2);
    if (!writtenBaseOf(layout) && listed.size() <= layout.declaredOrder.size() && std::ranges::equal(listed, layout.declaredOrder.span().first(listed.size()))) {
        lists.shrink(start);
        return;
    }
    lists[start + 1] = static_cast<uint32_t>(listed.size());
}

void ScanOfBody::listConstructions()
{
    for (const ObjectBeingMadeInScan& object : m_objectsBeingMade) {
        auto isFilled = [&](PropertyKeyID key) {
            return key && (std::ranges::binary_search(object.filled, FillInScan { object.root, false, key.bits() }) || isListedByAllocation(object.root, key));
        };

        if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            listDefinitionsBehindAllocation(object);
            if (!object.hasLeft)
                continue;
            LayoutID id = layoutOfAllocation(definitionOf(object.root));
            const Layout& layout = m_program.at(id);
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                if (!isFilled(layout.fields[field].key))
                    m_summary.fieldsLeftEmpty.append(FieldReference { id, field });
            }
            continue;
        }

        SummaryConstruction construction;
        switch (object.kind) {
        case ObjectBeingMadeKindInScan::OfOneLayout:
            RELEASE_ASSERT_NOT_REACHED();
        case ObjectBeingMadeKindInScan::Allocated:
            construction.object = SummaryConstruction::Object::Allocated;
            break;
        case ObjectBeingMadeKindInScan::AnsweredBySuperCall:
            construction.object = SummaryConstruction::Object::AnsweredBySuperCall;
            break;
        case ObjectBeingMadeKindInScan::This:
            construction.object = SummaryConstruction::Object::This;
            break;
        }
        construction.isSeenFromOutside = object.isGivenUp || (m_uses[object.root] & isSeenFromOutside) || object.parametersHandedTo.containsIf([&](uint32_t parameter) {
            return m_uses[parameter] & (isSeenFromOutside | isAccessedByName);
        });

        m_keysOfConstruction.shrink(0);
        for (const FillInScan& fill : object.filled) {
            if (!fill.isPart)
                m_keysOfConstruction.append(SummaryConstruction::Key { PropertyKeyID::fromBits(fill.number), true, false });
        }
        if (BodyValue made = definitionOf(object.root); made && made.opcode() == Opcode::NewObject && !object.isGivenUp) {
            auto allocation = made.as<Opcode::NewObject>();
            const Layout& layout = m_program.at(allocation.layout());
            for (uint32_t field : allocation.fields()) {
                if (field < layout.fields.size() && layout.fields[field].key)
                    m_keysOfConstruction.append(SummaryConstruction::Key { layout.fields[field].key, true, false });
            }
        }
        for (PropertyKeyID key : object.keysLookedUp)
            m_keysOfConstruction.append(SummaryConstruction::Key { key, false, true });
        std::ranges::sort(m_keysOfConstruction, { }, &SummaryConstruction::Key::key);

        construction.firstKey = static_cast<uint32_t>(m_summary.keysOfConstructions.size());
        for (const SummaryConstruction::Key& key : m_keysOfConstruction) {
            bool isRepeated = m_summary.keysOfConstructions.size() > construction.firstKey && m_summary.keysOfConstructions.last().key == key.key;
            if (!isRepeated) {
                m_summary.keysOfConstructions.append(key);
                continue;
            }
            m_summary.keysOfConstructions.last().isFilled |= key.isFilled;
            m_summary.keysOfConstructions.last().isLookedUp |= key.isLookedUp;
        }
        construction.numberOfKeys = static_cast<uint32_t>(m_summary.keysOfConstructions.size()) - construction.firstKey;

        construction.firstPart = static_cast<uint32_t>(m_summary.partsOfConstructions.size());
        for (FunctionID part : object.parts)
            m_summary.partsOfConstructions.append(SummaryConstruction::Part { part, std::ranges::binary_search(object.filled, FillInScan { object.root, true, part.index() }) });
        construction.numberOfParts = static_cast<uint32_t>(object.parts.size());
        m_summary.constructions.append(construction);
    }
    listHalvesOfAccessorsNotLaid();
    listBirths();
    sortWithoutRepeatsInScan(m_summary.fieldsLeftEmpty);
}

void ScanOfBody::listHalvesOfAccessorsNotLaid()
{
    if (m_accessorsLaid.isEmpty())
        return;
    struct Half {
        uint32_t root;
        uint32_t key;
        bool isGetter;
        bool liesThereAtEveryLeaving;
    };
    Vector<Half, 8> halves;
    for (const AccessorLaidInScan& laid : m_accessorsLaid) {
        if (!(m_uses[laid.root] & isFollowedWhileMade))
            continue;
        const ObjectBeingMadeInScan& object = objectBeingMade(laid.root);
        if (object.kind != ObjectBeingMadeKindInScan::OfOneLayout || !object.hasLeft)
            continue;
        halves.append(Half { laid.root, laid.key.bits(), laid.isGetter, laid.function && std::ranges::binary_search(object.filled, FillInScan { laid.root, true, laid.function.index() }) });
    }
    std::ranges::sort(halves, { }, [](const Half& half) {
        return std::pair { half.root, half.key };
    });
    for (size_t first = 0; first < halves.size();) {
        bool hasGetter = false;
        bool hasSetter = false;
        size_t end = first;
        for (; end < halves.size() && halves[end].root == halves[first].root && halves[end].key == halves[first].key; ++end)
            (halves[end].isGetter ? hasGetter : hasSetter) |= halves[end].liesThereAtEveryLeaving;
        LayoutID id = layoutOfAllocation(definitionOf(halves[first].root));
        if (std::optional<uint32_t> field = m_program.at(id).findField(PropertyKeyID::fromBits(halves[first].key))) {
            if (!hasGetter)
                m_summary.halvesOfAccessorsNotLaid.append(HalfOfAccessorInSummary { { id, *field }, true });
            if (!hasSetter)
                m_summary.halvesOfAccessorsNotLaid.append(HalfOfAccessorInSummary { { id, *field }, false });
        }
        first = end;
    }
    sortWithoutRepeatsInScan(m_summary.halvesOfAccessorsNotLaid);
}

void ScanOfBody::readCall(const BodyValue& value, uint32_t block)
{
    uint32_t callIndex = static_cast<uint32_t>(m_summary.calls.size());
    SummaryCall call;
    call.site = value.id();
    call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
    if (value.opcode() == Opcode::CallDirect && value.as<Opcode::CallDirect>().function())
        readDirectCall(value, call);
    else
        readCallByValue(value, call, callIndex);
    call.numberOfArguments = static_cast<uint32_t>(m_summary.arguments.size()) - call.firstArgument;
    m_summary.calls.append(call);
    m_blocksOfCalls.append(block);
    m_lists[value.id().index()] = { callIndex, isResultOfCall };
}

void ScanOfBody::readDirectCall(const BodyValue& value, SummaryCall& call)
{
    auto direct = value.as<Opcode::CallDirect>();
    call.callee = SummaryCall::Callee::Binding;
    call.function = direct.function();
    std::span<const ValueID> arguments = direct.argumentsOperands();
    const Function& callee = m_program.at(call.function);
    uint32_t numberOfCaptures = static_cast<uint32_t>(callee.captures.size());
    std::optional<unsigned> thisOfInitializer = callee.kind == FunctionKind::Initializer ? callee.thisParameterIndex() : std::nullopt;
    for (uint32_t index = 0; index < arguments.size(); ++index) {
        uint32_t root = arguments[index] ? m_roots[arguments[index].index()] : noDefinition;
        bool goesOnWithConstruction = thisOfInitializer && *thisOfInitializer == index && root != noDefinition && (m_uses[root] & isFollowedWhileMade)
            && objectBeingMade(root).kind != ObjectBeingMadeKindInScan::OfOneLayout;
        addArgument(arguments[index], !goesOnWithConstruction);
        if (goesOnWithConstruction) {
            m_uses[root] |= isAccessedBesideNamedStores;
            ObjectBeingMadeInScan& object = objectBeingMade(root);
            if (!object.parts.contains(call.function))
                object.parts.append(call.function);
            noteFill(root, true, call.function.index());
        }
        if (index < numberOfCaptures && m_summary.environmentsMade.contains(arguments[index]))
            m_summary.environmentsHandedToFunctionsEntered.appendIfNotContains(arguments[index]);
        if (index < numberOfCaptures && arguments[index])
            flowInto(listOf(arguments[index]), captureNode(call.function, index));
    }
}

void ScanOfBody::readCallByValue(const BodyValue& value, SummaryCall& call, uint32_t callIndex)
{
    Opcode opcode = value.opcode();
    std::optional<unsigned> thisPosition = OperandPosition::thisValue(opcode);
    std::optional<unsigned> newTargetPosition = OperandPosition::newTarget(opcode);
    std::optional<unsigned> listPosition = OperandPosition::list(opcode);
    call.isConstruction = newTargetPosition.has_value();
    call.passesList = listPosition.has_value();

    ValueID callee;
    ValueID receiver;
    ValueID newTarget;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandUsesOf(opcode, operandIndex).contains(OperandUse::Calls))
            callee = operand;
        else if (thisPosition && *thisPosition == operandIndex)
            receiver = operand;
        else if (newTargetPosition && *newTargetPosition == operandIndex)
            newTarget = operand;
        else if (listPosition && *listPosition == operandIndex) {
            noteLeaving(operand);
            m_summary.arguments.append(readContents(value.id(), listOf(operand), ContentsSlot::Element));
        } else
            addArgument(operand);
    });

    Effects inFrontOfCallee = Effects::none();
    inFrontOfCallee.mayThrow = true;
    m_summary.ownEffects.merge(inFrontOfCallee);

    if (receiver) {
        noteLeaving(receiver);
        call.receiver = 0;
        addSink(SinkKindInScan::ReceiverOfCall, listOf(receiver), callIndex);
    }
    if (newTarget) {
        noteLeaving(newTarget);
        call.newTarget = 0;
        addSink(SinkKindInScan::NewTargetOfCall, listOf(newTarget), callIndex);
    }
    call.subject = 0;
    if (!callee)
        return;

    BodyValue definition = definitionBehindCasts(callee);
    if (definition && definition.opcode() == Opcode::NativeBinding)
        call.builtin = definition.as<Opcode::NativeBinding>().builtin();
    if (definition && definition.opcode() == Opcode::Builtin) {
        auto named = definition.as<Opcode::Builtin>();
        if (named.builtin() && (named.isNotReadFromHolder() || namesWhatNobodyReplacesInScan(m_program.builtins(), named.builtin())) && !compiledModuleWithObject(m_program, m_program.builtins().canonicalID(named.builtin()))) {
            call.callee = SummaryCall::Callee::Builtin;
            call.builtin = named.builtin();
            call.subject = noSummaryNode;
            call.key = keyOfSymbolThatCallMakes(m_program, m_id, value, scopedLambda<BodyValue(ValueID)>([&](ValueID operand) {
                return definitionOf(operand.index());
            }));
            readChangesMadeByBuiltin(value, call, callIndex, receiver);
            return;
        }
    }
    if (definition && definition.opcode() == Opcode::PropGet && receiver) {
        auto read = definition.as<Opcode::PropGet>();
        if (read.key() && read.objectOperand() && m_roots[read.objectOperand().index()] == m_roots[receiver.index()]) {
            call.callee = SummaryCall::Callee::Name;
            call.key = read.key();
            call.subject = noSummaryNode;
            m_uses[definition.id().index()] |= isCalledByName;
            ValueID firstArgument;
            if (opcode == Opcode::Call) {
                auto arguments = value.as<Opcode::Call>().argumentsOperands();
                if (arguments.size())
                    firstArgument = *arguments.begin();
            }
            BodyValue keyMadeByCall = firstArgument ? definitionBehindCasts(firstArgument) : BodyValue();
            if (keyMadeByCall && (keyMadeByCall.opcode() == Opcode::Call || keyMadeByCall.opcode() == Opcode::CallDirect)) {
                const BuiltinTable& table = m_program.builtins();
                if (findOwnMember(m_program, table.at(BuiltinClasses::Map).prototype, read.key()) || findOwnMember(m_program, table.at(BuiltinClasses::Set).prototype, read.key())) {
                    auto record = std::ranges::lower_bound(m_summary.calls, keyMadeByCall.id(), { }, &SummaryCall::site);
                    if (record != m_summary.calls.end() && record->site == keyMadeByCall.id()) {
                        record->resultIsReadAsKey = true;
                        m_summary.someResultIsReadAsKey = true;
                    }
                }
            }
            if (m_uses[definition.id().index()] & mayComeInFrontOfDefinition)
                readByName(definition.id().index());
            return;
        }
    }
    noteLeaving(callee);
    addSink(SinkKindInScan::SubjectOfCall, listOf(callee), callIndex);
}

void ScanOfBody::readChangesMadeByBuiltin(const BodyValue& value, const SummaryCall& call, uint32_t callIndex, ValueID receiver)
{
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinCallMode mode = call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    std::span<const ValueID> arguments = call.passesList ? std::span<const ValueID>() : value.variadicOperands();
    ValueID site = value.id();

    auto forEachListAt = [&](BuiltinOperand operand, const auto& functor) {
        if (operand.isReceiver()) {
            if (receiver)
                functor(listOf(receiver));
            return;
        }
        if (call.passesList) {
            if (call.firstArgument < m_summary.arguments.size())
                functor(listOfNode(m_summary.arguments[call.firstArgument]));
            return;
        }
        for (uint32_t index = operand.argumentIndex(); index < arguments.size() && operand.coversArgument(index); ++index) {
            if (arguments[index])
                functor(listOf(arguments[index]));
        }
    };

    bool hasDeclaredRow = false;
    m_changesOfBuiltin.shrink(0);
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(call.builtin))) {
        if (overload.mode != mode)
            continue;
        hasDeclaredRow |= !overload.isUndeclared();
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            const BuiltinParameter* actedOn = builtins.parameterFor(overload, action.object);
            if (actedOn && actedOn->accepted && actedOn->conversion == BuiltinConversion::None && !builtins.at(actedOn->accepted).mayBeObject())
                continue;
            bool storesWhatOperandHolds = action.kind == BuiltinPropertyActionKind::StoresByKey && action.value.kind == BuiltinPlaceKind::OwnValues;
            ChangeOfBuiltinInScan change { action.kind, action.object, storesWhatOperandHolds ? std::nullopt : action.value.operand, action.key.kind == BuiltinKeyKind::Operand ? action.key.operand : std::nullopt };
            if (!m_changesOfBuiltin.contains(change))
                m_changesOfBuiltin.append(change);
        }
    }

    if (!hasDeclaredRow) {
        forEachListAt(BuiltinOperand::receiver(), [&](ListInScan list) {
            escape(list);
        });
        forEachListAt(BuiltinOperand::argumentsFrom(0), [&](ListInScan list) {
            escape(list);
        });
        addEdge(0, nodeFor(SummaryNode::Kind::ResultOfCall, callIndex, 0));
        return;
    }

    for (const ChangeOfBuiltinInScan& change : m_changesOfBuiltin) {
        auto forEachPair = [&](const auto& functor) {
            forEachListAt(change.object, [&](ListInScan subject) {
                bool findsOther = false;
                if (change.other) {
                    forEachListAt(*change.other, [&](ListInScan other) {
                        findsOther = true;
                        functor(subject, std::optional(other));
                    });
                }
                if (!findsOther)
                    functor(subject, std::optional<ListInScan>());
            });
        };
        KeyWrittenInScan keyWritten;
        if (change.key && change.key->kind() == BuiltinOperand::Kind::Argument && !call.passesList && change.key->argumentIndex() < arguments.size())
            keyWritten = keyWrittenAs(arguments[change.key->argumentIndex()]);
        auto note = [&](SummaryChange::Kind kind) {
            forEachPair([&](ListInScan subject, std::optional<ListInScan> other) {
                addChange(kind, site, subject, keyWritten.key, other);
            });
        };

        switch (change.kind) {
        case BuiltinPropertyActionKind::ReadsByKey:
        case BuiltinPropertyActionKind::ReadsGetter:
        case BuiltinPropertyActionKind::ReadsSetter:
        case BuiltinPropertyActionKind::DescribesOwnProperties:
        case BuiltinPropertyActionKind::DescribesResult:
            break;
        case BuiltinPropertyActionKind::TestsByKey:
            forEachListAt(change.object, [&](ListInScan subject) {
                addAccessUnder(SummaryAccess::Kind::Has, site, subject, { }, ValueID());
            });
            break;
        case BuiltinPropertyActionKind::StoresByKey:
            forEachPair([&](ListInScan subject, std::optional<ListInScan> stored) {
                uint32_t access = addAccess(SummaryAccess::Kind::Store, site, subject, keyWritten.key);
                if (!keyWritten.key)
                    m_summary.accesses[access].keyNode = 0;
                m_summary.accesses[access].value = 0;
                if (stored)
                    escape(*stored);
            });
            break;
        case BuiltinPropertyActionKind::DeletesByKey:
            note(SummaryChange::Kind::Deletes);
            break;
        case BuiltinPropertyActionKind::AssignsFrom:
            forEachPair([&](ListInScan subject, std::optional<ListInScan> source) {
                addChange(SummaryChange::Kind::CopiesInto, site, subject, { }, source ? *source : listOfNode(0));
            });
            break;
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::TakesDescriptor:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::DefinesFrom:
            forEachPair([&](ListInScan subject, std::optional<ListInScan> descriptor) {
                bool isOneArgument = change.kind == BuiltinPropertyActionKind::TakesDescriptorMap && descriptor && change.other->kind() == BuiltinOperand::Kind::Argument && !call.passesList;
                ValueID map = isOneArgument ? arguments[change.other->argumentIndex()] : ValueID();
                if (map && m_firstLeft[m_roots[map.index()]] >= site.index())
                    m_definitionsByMaps.append(DefinitionByMapInScan { static_cast<uint32_t>(m_summary.changes.size()), static_cast<uint32_t>(m_summary.accesses.size()), m_roots[map.index()], site.index() });
                bool isOneDescriptor = change.kind == BuiltinPropertyActionKind::TakesDescriptor && descriptor && change.other->kind() == BuiltinOperand::Kind::Argument && !call.passesList;
                ValueID written = isOneDescriptor ? arguments[change.other->argumentIndex()] : ValueID();
                bool hasNotLeft = written && m_firstLeft[m_roots[written.index()]] >= site.index();
                bool waitsForItsJudgment = hasNotLeft && keyWritten.isWritten;
                if (hasNotLeft)
                    m_definitionsByDescriptors.append(DefinitionByDescriptorInScan { static_cast<uint32_t>(m_summary.changes.size()), m_roots[written.index()], site.index(), waitsForItsJudgment, *descriptor });
                defineByDescriptor(site, subject, keyWritten.key, descriptor, keyWritten.isWritten && !keyWritten.key);
                if (hasNotLeft && keyWritten.key)
                    m_definitionsByDescriptors.last().access = static_cast<uint32_t>(m_summary.accesses.size()) - 1;
                if (descriptor && !waitsForItsJudgment)
                    escape(*descriptor);
            });
            break;
        case BuiltinPropertyActionKind::ChangesIntegrity:
            note(SummaryChange::Kind::Freezes);
            break;
        case BuiltinPropertyActionKind::ChangesPrototype:
            forEachPair([&](ListInScan subject, std::optional<ListInScan> prototype) {
                addChange(SummaryChange::Kind::ChangesPrototype, site, subject, { }, prototype ? *prototype : listOfNode(0));
            });
            break;
        case BuiltinPropertyActionKind::BecomesPrototype:
            forEachPair([&](ListInScan prototype, std::optional<ListInScan> below) {
                if (!below)
                    m_heirsMadeByBuiltins.append(HeirMadeByBuiltinInScan { site.index(), static_cast<uint32_t>(m_summary.changes.size()), static_cast<uint32_t>(m_sinks.size()), prototype, mode == BuiltinCallMode::Call && !call.passesList && arguments.size() == 1 });
                addChange(SummaryChange::Kind::BecomesPrototype, site, prototype, { }, below);
                if (below)
                    escape(prototype);
            });
            break;
        }
    }
}

void ScanOfBody::noteEvaluationOf(ModuleID module, ValueID site, uint32_t block)
{
    FunctionID body = m_program.at(module).body;
    if (!body)
        return;
    SummaryCall call;
    call.site = site;
    call.callee = SummaryCall::Callee::Binding;
    call.function = body;
    call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
    call.numberOfArguments = m_program.at(body).numberOfEntryParameters();
    for (uint32_t argument = 0; argument < call.numberOfArguments; ++argument)
        m_summary.arguments.append(0);
    m_summary.calls.append(call);
    m_blocksOfCalls.append(block);
}

void ScanOfBody::noteRecordOf(ModuleID module)
{
    Vector<ModuleID>& named = m_summary.otherModulesNamed;
    if (module && module != m_function->module && (named.isEmpty() || named.last() != module))
        named.append(module);
}

bool ScanOfBody::listsDefinitions() const
{
    if (!m_function->isModuleBody() || !m_function->module || m_function->isCoroutine())
        return false;
    const Module& module = m_program.at(m_function->module);
    return module.body == m_id && isEvaluatedAtMostOnceOf(module.kind) && !module.flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked });
}

void ScanOfBody::noteTakerOfValueMadeForVariable(ValueID taken)
{
    if (!m_listsDefinitions || !taken || m_valuesMadeForVariables.isEmpty())
        return;
    auto made = std::ranges::lower_bound(m_valuesMadeForVariables, m_roots[taken.index()], { }, &ValueMadeForVariableInScan::made);
    if (made != m_valuesMadeForVariables.end() && made->made == m_roots[taken.index()])
        made->isTakenByOthers = true;
}

void ScanOfBody::lookForDefinitions(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (raisesOf(opcode) == ProgramEvent::EvaluatesText) {
        m_listsDefinitions = false;
        return;
    }
    if (inputOperationsThatHandOn.contains(opcode))
        return;
    if (opcode == Opcode::PutModuleVariable) {
        auto store = value.as<Opcode::PutModuleVariable>();
        ValueID stored = store.valueOperand();
        auto made = stored ? std::ranges::lower_bound(m_valuesMadeForVariables, m_roots[stored.index()], { }, &ValueMadeForVariableInScan::made) : m_valuesMadeForVariables.end();
        if (made == m_valuesMadeForVariables.end() || made->made != m_roots[stored.index()])
            return;
        if (made->store != noDefinition || !store.variable() || m_program.at(store.variable()).module != m_function->module) {
            made->isTakenByOthers = true;
            return;
        }
        made->store = value.id().index();
        made->variable = store.variable();
        return;
    }
    value.forEachOperand([&](ValueID operand, uint32_t) {
        noteTakerOfValueMadeForVariable(operand);
    });
    bool makesObjectOutOfTextAlone = opcode == Opcode::NewRegExp || opcode == Opcode::Data || (opcode == Opcode::NewClosure && value.as<Opcode::NewClosure>().capturesOperands().empty());
    if (makesObjectOutOfTextAlone)
        m_valuesMadeForVariables.append(ValueMadeForVariableInScan { value.id().index(), noDefinition, { }, false });
}

void ScanOfBody::listDefinitions()
{
    if (!m_listsDefinitions)
        return;
    auto comesAgain = [&](uint32_t value) {
        return !m_innermostLoopHeads.isEmpty() && m_innermostLoopHeads[m_blocks[value]] != noLoop;
    };
    for (const ValueMadeForVariableInScan& made : m_valuesMadeForVariables) {
        if (made.store == noDefinition || made.isTakenByOthers || comesAgain(made.made) || comesAgain(made.store))
            continue;
        m_summary.definitions.append(SummaryDefinition { made.variable, ValueID(made.made), ValueID(made.store) });
    }
}

bool ScanOfBody::listsFirstStoresOfBody() const
{
    if (!m_function->isModuleBody() || !m_function->module)
        return false;
    const Module& module = m_program.at(m_function->module);
    return module.body == m_id && isEvaluatedAtMostOnceOf(module.kind) && !hasExportTableOf(module.kind)
        && !module.flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked });
}

void ScanOfBody::noteUseByBodyOfModule(FunctionID function)
{
    if (m_listsFirstStoresOfBody && m_usesNotedOfBodyOfModule.add(static_cast<uint64_t>(function.index()) << 32 | m_blockBeingRead).isNewEntry)
        m_usesByBodyOfModule.append(PlaceInBlockInScan { function.index(), m_position, m_blockBeingRead });
}

uint32_t ScanOfBody::nearestBlockAboveBoth(uint32_t first, uint32_t second) const
{
    while (first != second) {
        uint32_t& lower = first > second ? first : second;
        lower = m_immediateDominators[lower];
    }
    return first;
}

void ScanOfBody::noteEdgeOfBodyOfModule(uint32_t from, uint32_t to, KindOfEdgeInScan kind)
{
    if (!m_listsFirstStoresOfBody || to <= from || m_immediateDominators[from] == noDefinition)
        return;
    uint32_t source = kind == KindOfEdgeInScan::ToHandler ? m_immediateDominators[from] : from;
    uint32_t& above = m_immediateDominators[to];
    above = above == noDefinition ? source : nearestBlockAboveBoth(above, source);
}

void ScanOfBody::listFirstStoresAndUses()
{
    if (!m_listsFirstStoresOfBody || m_blockAboveReturns == noDefinition)
        return;
    BitVector everyWayComesThrough(m_immediateDominators.size());
    for (uint32_t block = m_blockAboveReturns; !everyWayComesThrough.quickSet(block);)
        block = m_immediateDominators[block];
    auto placeInItsBlock = [&](const PlaceInBlockInScan& place) -> uint32_t {
        return 2 * place.position + (place.position != m_endsOfBlocks[place.block]);
    };
    auto placeAmongThoseBlocks = [&](const PlaceInBlockInScan& place) -> uint32_t {
        uint32_t above = place.block;
        while (above != noDefinition && !everyWayComesThrough.quickGet(above))
            above = m_immediateDominators[above];
        if (above == noDefinition)
            return 0;
        return above == place.block ? placeInItsBlock(place) : 2 * m_endsOfBlocks[above];
    };

    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstReads;
    for (const PlaceInBlockInScan& read : m_readsOfBodyOfModule) {
        uint32_t place = placeAmongThoseBlocks(read);
        auto added = firstReads.add(read.number, place);
        added.iterator->value = std::min(added.iterator->value, place);
    }
    for (const PlaceInBlockInScan& store : m_storesOfBodyOfModule) {
        auto read = firstReads.find(store.number);
        if (everyWayComesThrough.quickGet(store.block) && (read == firstReads.end() || placeInItsBlock(store) < read->value))
            m_summary.firstStoresOfBody.append(SummaryPlaceInBody { store.number, placeInItsBlock(store) });
    }
    for (const PlaceInBlockInScan& use : m_usesByBodyOfModule)
        m_summary.firstUsesOfFunctions.append(SummaryPlaceInBody { use.number, placeAmongThoseBlocks(use) });
    for (Vector<SummaryPlaceInBody>* places : { &m_summary.firstStoresOfBody, &m_summary.firstUsesOfFunctions }) {
        std::ranges::sort(*places);
        auto repeated = std::ranges::unique(*places, { }, &SummaryPlaceInBody::number);
        places->shrink(places->size() - repeated.size());
    }
}

void ScanOfBody::noteUseByBodyOfModule(ValueID operand)
{
    if (!m_listsFirstStoresOfBody)
        return;
    BodyValue definition = definitionBehindCasts(operand);
    if (definition && definition.opcode() == Opcode::NewClosure)
        noteUseByBodyOfModule(definition.as<Opcode::NewClosure>().function());
}

void ScanOfBody::lookForFirstStoresAndUses(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::PutModuleVariable) {
        VariableID variable = value.as<Opcode::PutModuleVariable>().variable();
        if (variable && m_program.at(variable).module == m_function->module)
            m_storesOfBodyOfModule.append(PlaceInBlockInScan { variable.index(), m_position, m_blockBeingRead });
    } else if (opcode == Opcode::GetModuleVariable) {
        if (VariableID variable = value.as<Opcode::GetModuleVariable>().variable()) {
            if (m_readsNotedOfBodyOfModule.add(static_cast<uint64_t>(variable.index()) << 32 | m_blockBeingRead).isNewEntry)
                m_readsOfBodyOfModule.append(PlaceInBlockInScan { variable.index(), m_position, m_blockBeingRead });
            if (FunctionID declared = m_program.at(variable).function)
                noteUseByBodyOfModule(declared);
        }
    }
    if (raisesOf(opcode) == ProgramEvent::EvaluatesText) {
        m_listsFirstStoresOfBody = false;
        return;
    }
    if (inputOperationsThatHandOn.contains(opcode))
        return;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand)
            noteUseByBodyOfModule(operand);
    });
}

bool ScanOfBody::answersWithResultOfBody(const BodyValue& value) const
{
    if (value.opcode() != Opcode::RequireModule)
        return false;
    auto require = value.as<Opcode::RequireModule>();
    return !require.ifHeldForImport() && require.module() && requireAnswersWithResultOfBody(m_program, require.referrer(), require.module());
}

void ScanOfBody::readTerminator(const BodyTerminator& terminator, uint32_t block)
{
    TerminatorKind kind = terminator.kind();
    ReferencesOfScan references { *this, ValueID(), block, false, false };
    terminator.forEachReference(references);
    if (references.namesUndefinedRecord) {
        stopAtUndefinedRecord(block, ValueID(), m_function->locationOf(terminator), nameOf(kind));
        return;
    }

    if (kind == TerminatorKind::Switch || kind == TerminatorKind::SwitchString)
        m_summary.weightOfOperations += terminator.numberOfTargets() / 4;
    Effects effects = declaredEffectsOfTerminator(m_program, terminator);
    noteEndOfBlock(terminator, block, effects);
    m_summary.ownEffects.merge(effects);

    if (m_listsFirstStoresOfBody && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall) && m_immediateDominators[block] != noDefinition)
        m_blockAboveReturns = m_blockAboveReturns == noDefinition ? block : nearestBlockAboveBoth(m_blockAboveReturns, block);
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        noteUseByBodyOfModule(operand);
        noteTakerOfValueMadeForVariable(operand);
        OperandUses uses = operandUsesOf(kind, operandIndex);
        if (uses != OperandUses { OperandUse::Inspects })
            noteLeaving(operand, kind == TerminatorKind::Return && !m_function->isCoroutine() ? WayOfLeavingInScan::IsReturned : WayOfLeavingInScan::IsSeenFromOutside);
        if (uses.contains(OperandUse::Returns))
            returnValue(operand, block);
        if (uses.contains(OperandUse::Retains)) {
            if (effects.mayThrow)
                flowInto(listOf(operand), nodeFor(SummaryNode::Kind::Thrown));
            escape(listOf(operand));
        }
    });
    if (kind == TerminatorKind::Return && !terminator.operand(0) && m_function->isCoroutine())
        m_returnsOfCoroutine.append(std::pair { listOfNode(constant(TypeTable::undefined)), block });
    if (kind == TerminatorKind::Return && !terminator.operand(0) && !m_function->isCoroutine()) {
        if (uint32_t result = resultNode(0))
            addEdge(constant(TypeTable::undefined), result);
    }
    if (ValueID returned = kind == TerminatorKind::ReturnCall ? terminator.operand(0) : ValueID()) {
        ValueID site(m_roots[returned.index()]);
        for (size_t call = m_summary.calls.size(); call-- && m_summary.calls[call].site == site;)
            m_summary.calls[call].isInTailPosition = true;
    }
    if (m_thisBeingMade != noDefinition && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall)) {
        meetFillsWhereObjectLeaves(objectBeingMade(m_thisBeingMade), fillsOf(m_thisBeingMade, m_fills.inOrder()));
        meetFillsWhereConstructionEnds(m_thisBeingMade);
    }

    BodyValue test;
    Proof proof = provesOf(kind);
    if (terminator.isConditionalBranch()) {
        ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
        test = condition ? definitionOf(condition.index()) : BodyValue();
        proof = test ? provesOf(test.opcode()) : Proof::None;
    }
    std::optional<uint32_t> onlyTarget;
    if (m_followsConstants && goesByOneOperand(kind) && terminator.operand(0))
        onlyTarget = onlyTargetWith(m_program, terminator, primitiveHeldBy(terminator.operand(0)));
    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        if (onlyTarget && *onlyTarget != targetIndex) {
            ++targetIndex;
            continue;
        }
        readArgumentsOf(block, target);
        uint32_t position = static_cast<uint32_t>(target.arguments().size());
        for (uint32_t operandIndex : target.proofs()) {
            ValueID subject;
            if (proof != Proof::None)
                subject = test ? test.operand(operandIndex) : terminator.operand(operandIndex);
            TypeID passes;
            if (subject && !targetIndex && test && test.opcode() == Opcode::TypeTest)
                passes = test.as<Opcode::TypeTest>().testedType();
            if (subject)
                handToBlock(subject, block, target.block().index(), position, KindOfEdgeInScan::Plain);
            m_incoming.append(IncomingInScan { target.block().index(), position, subject ? listOf(subject) : listOfNode(0), passes });
            ++position;
        }
        ++targetIndex;
    }
}

void ScanOfBody::readNamesThatLoaderTakesOff(ValueID returned)
{
    const Module& module = m_program.at(m_function->module);
    if (module.body != m_id || module.exportRule != SyntheticExportRule::RuntimeModule || module.requireResult != RequireResult::DefaultExport)
        return;
    for (const SyntheticExport& entry : module.syntheticExports) {
        std::optional<PropertyKeyID> key = entry.variable ? m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = entry.name }) : std::nullopt;
        if (key)
            m_summary.namesThatLoaderTakes.append({ addRead(returned, listOf(returned), *key), entry.variable });
    }
}

void ScanOfBody::returnValue(ValueID operand, uint32_t block)
{
    if (m_function->isCoroutine()) {
        m_returnsOfCoroutine.append(std::pair { listOf(operand), block });
        return;
    }

    BodyValue definition = definitionOf(operand.index());
    if (definition && definition.opcode() == Opcode::Results) {
        uint32_t result = 0;
        for (ValueID value : definition.variadicOperands()) {
            if (value)
                flowInto(listOf(value), resultNode(result));
            ++result;
        }
        return;
    }
    flowInto(listOf(operand), resultNode(0));
    readNamesThatLoaderTakesOff(operand);
    if (!definition || effectRuleOf(definition.opcode()) != EffectRule::Call)
        return;
    std::optional<uint32_t> call = findCall(operand);
    for (uint32_t result = 1; call && result < m_function->results.size(); ++result)
        addEdge(nodeFor(SummaryNode::Kind::ResultOfCall, *call, result), resultNode(result));
}

void ScanOfBody::readWhatNobodyAskedFor()
{
    for (uint32_t read : m_readsNotYetNeeded) {
        if (!(m_uses[read] & isCalledByName))
            readByName(read);
    }
    for (auto [list, block] : m_returnsOfCoroutine)
        returnFromCoroutine(list, block);
}

void ScanOfBody::returnFromCoroutine(ListInScan list, uint32_t block)
{
    bool isGenerator = m_function->has(FunctionFlags::IsGenerator);
    flowInto(list, contentsNode(objectOfCoroutine(), SlotOfContents::returnedAlone()));
    if (m_function->flags.containsAll({ FunctionFlags::IsGenerator, FunctionFlags::IsAsync })) {
        escape(list);
        return;
    }
    if (isGenerator)
        flowInto(list, contentsNode(objectOfCoroutine(), ContentsSlot::Element));
    else
        flowInto(settle(ValueID(), list, block), contentsNode(objectOfCoroutine(), ContentsSlot::Value));
}

void ScanOfBody::findLoops()
{
    if (m_edgesLeadingBack.isEmpty())
        return;
    m_summary.traits.add(SummaryTrait::HasLoops);
    std::sort(m_edgesLeadingBack.begin(), m_edgesLeadingBack.end(), [](const EdgeLeadingBackInScan& first, const EdgeLeadingBackInScan& second) {
        return std::pair { first.head, first.last } < std::pair { second.head, second.last };
    });

    uint32_t numberOfBlocks = m_function->body.numberOfBlocks();
    m_innermostLoopHeads.fill(noLoop, numberOfBlocks);
    m_openLoops.shrink(0);
    size_t next = 0;
    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        while (next < m_edgesLeadingBack.size() && m_edgesLeadingBack[next].head == block)
            m_openLoops.append(m_edgesLeadingBack[next++]);
        while (!m_openLoops.isEmpty() && m_openLoops.last().last < block)
            m_openLoops.removeLast();
        if (!m_openLoops.isEmpty())
            m_innermostLoopHeads[block] = m_openLoops.last().head;
    }
    uint32_t numberOfEntryParameters = std::min<uint32_t>(m_function->numberOfEntryParameters(), 64);
    for (uint32_t call = 0; call < m_summary.calls.size(); ++call) {
        SummaryCall& record = m_summary.calls[call];
        record.isInLoop = m_innermostLoopHeads[m_blocksOfCalls[call]] != noLoop;
        if (!record.isInLoop || record.callee != SummaryCall::Callee::Value || record.isConstruction || record.subject == noSummaryNode)
            continue;
        const SummaryNode& subject = m_summary.nodes[record.subject];
        if (subject.kind == SummaryNode::Kind::Parameter && subject.first < 64)
            m_summary.entryParametersCalledInLoops |= uint64_t(1) << subject.first;
        BodyValue written = definitionOf(record.site.index());
        ValueID callee = written && written.opcode() == Opcode::Call ? written.as<Opcode::Call>().calleeOperand() : ValueID();
        if (callee && m_roots[callee.index()] < numberOfEntryParameters)
            m_summary.entryParametersCalledInLoops |= uint64_t(1) << m_roots[callee.index()];
    }
}

bool ScanOfBody::isStillAlone(uint32_t root, uint32_t position, uint32_t block) const
{
    if (!(m_uses[root] & isUnderConstruction) || position >= m_firstLeft[root])
        return false;
    if (m_firstLeft[root] == hasNotLeft || m_innermostLoopHeads.isEmpty())
        return true;
    uint32_t head = m_innermostLoopHeads[block];
    return head == noLoop || head <= m_blocks[root];
}

std::optional<uint32_t> ScanOfBody::parameterTakingIncoming(const IncomingInScan& incoming) const
{
    uint32_t first = m_firstParameterOfBlock[incoming.block];
    if (incoming.position >= m_firstParameterOfBlock[incoming.block + 1] - first)
        return std::nullopt;
    return first + incoming.position;
}

void ScanOfBody::sortIncomingByParameter()
{
    uint32_t count = m_numberOfParameters;
    m_firstIncoming.fill(0, count + 2);
    for (const IncomingInScan& incoming : m_incoming) {
        if (std::optional<uint32_t> parameter = parameterTakingIncoming(incoming))
            ++m_firstIncoming[*parameter + 2];
    }
    for (uint32_t parameter = 0; parameter < count; ++parameter)
        m_firstIncoming[parameter + 2] += m_firstIncoming[parameter + 1];
    m_incomingByParameter.fill(0, m_firstIncoming[count + 1]);
    for (uint32_t index = 0; index < m_incoming.size(); ++index) {
        if (std::optional<uint32_t> parameter = parameterTakingIncoming(m_incoming[index]))
            m_incomingByParameter[m_firstIncoming[*parameter + 1]++] = index;
    }
}

void ScanOfBody::markParametersThatSinksNeed()
{
    m_isNeeded.fill(false, m_numberOfParameters);
    m_worklist.shrink(0);
    auto need = [&](ListInScan list) {
        for (uint32_t atom : m_atoms.span().subspan(list.first, list.count)) {
            if (!(atom & (parameterBit | operatorBit)))
                continue;
            bool wasNeeded = atom & parameterBit ? std::exchange(m_isNeeded[atom & ~parameterBit], true) : std::exchange(m_operators[atom & ~operatorBit].isNeeded, true);
            if (!wasNeeded)
                m_worklist.append(atom);
        }
    };

    for (const SinkInScan& sink : m_sinks)
        need(sink.value);
    for (const StoreInScan& store : m_stores) {
        need(store.base);
        need(store.value);
    }
    while (!m_worklist.isEmpty()) {
        uint32_t atom = m_worklist.takeLast();
        if (atom & parameterBit) {
            uint32_t parameter = atom & ~parameterBit;
            for (uint32_t place = m_firstIncoming[parameter]; place < m_firstIncoming[parameter + 1]; ++place)
                need(m_incoming[m_incomingByParameter[place]].list);
            continue;
        }
        for (ListInScan operand : m_operators[atom & ~operatorBit].operands)
            need(operand);
    }

    for (OperatorInScan& found : m_operators) {
        if (!found.isNeeded)
            continue;
        found.record = static_cast<uint32_t>(m_summary.operators.size());
        SummaryOperator record;
        record.site = found.site;
        record.opcode = found.opcode;
        record.result = found.joinOfResult ? found.joinOfResult : nodeFor(SummaryNode::Kind::ResultOfOperator, found.record);
        m_summary.operators.append(record);
    }
}

void ScanOfBody::resolveParameters()
{
    uint32_t count = m_numberOfParameters;
    sortIncomingByParameter();
    markParametersThatSinksNeed();

    m_order.fill(0, count);
    m_lowest.fill(0, count);
    m_componentOf.fill(noComponent, count);
    m_resolved.fill(ListInScan { }, count);
    m_open.shrink(0);
    m_frames.shrink(0);
    uint32_t numberEntered = 0;
    uint32_t numberOfComponents = 0;
    auto enter = [&](uint32_t parameter) {
        m_order[parameter] = m_lowest[parameter] = ++numberEntered;
        m_open.append(parameter);
        m_frames.append(FrameInScan { parameter, m_firstIncoming[parameter], 0 });
    };

    for (uint32_t root = 0; root < count; ++root) {
        if (m_order[root])
            continue;
        enter(root);
        while (!m_frames.isEmpty()) {
            FrameInScan& frame = m_frames.last();
            uint32_t parameter = frame.parameter;
            if (frame.incoming < m_firstIncoming[parameter + 1]) {
                ListInScan list = m_incoming[m_incomingByParameter[frame.incoming]].list;
                if (frame.atom == list.count) {
                    ++frame.incoming;
                    frame.atom = 0;
                    continue;
                }
                uint32_t atom = m_atoms[list.first + frame.atom++];
                if (!(atom & parameterBit))
                    continue;
                uint32_t other = atom & ~parameterBit;
                if (!m_order[other])
                    enter(other);
                else if (m_componentOf[other] == noComponent)
                    m_lowest[parameter] = std::min(m_lowest[parameter], m_order[other]);
                continue;
            }

            m_frames.removeLast();
            if (!m_frames.isEmpty()) {
                uint32_t parent = m_frames.last().parameter;
                m_lowest[parent] = std::min(m_lowest[parent], m_lowest[parameter]);
            }
            if (m_lowest[parameter] != m_order[parameter])
                continue;

            size_t first = m_open.size();
            do {
                --first;
                m_componentOf[m_open[first]] = numberOfComponents;
            } while (m_open[first] != parameter);
            resolveComponent(m_open.span().subspan(first), numberOfComponents++);
            m_open.shrink(first);
        }
    }
}

void ScanOfBody::resolveComponent(std::span<const uint32_t> members, uint32_t component)
{
    if (!m_isNeeded[members[0]])
        return;

    m_gathered.shrink(0);
    for (uint32_t member : members) {
        for (uint32_t place = m_firstIncoming[member]; place < m_firstIncoming[member + 1]; ++place) {
            const IncomingInScan& incoming = m_incoming[m_incomingByParameter[place]];
            for (uint32_t position = 0; position < incoming.list.count; ++position) {
                uint32_t atom = m_atoms[incoming.list.first + position];
                if (!(atom & parameterBit)) {
                    m_gathered.append(behindTest({ nodeOfAtom(atom), { } }, incoming.passes));
                    continue;
                }
                uint32_t other = atom & ~parameterBit;
                if (m_componentOf[other] == component)
                    continue;
                ListInScan resolved = m_resolved[other];
                for (uint32_t index = 0; index < resolved.count; ++index)
                    m_gathered.append(behindTest(m_origins[resolved.first + index], incoming.passes));
            }
        }
    }
    normalize(m_gathered);

    if (m_gathered.size() > 1) {
        bool takesOnlyPrimitives = std::ranges::all_of(m_gathered, [&](const OriginInScan& origin) {
            return isAlwaysPrimitive(origin);
        });
        uint32_t join = newJoin();
        if (takesOnlyPrimitives)
            m_joinsOfPrimitives.append(join);
        for (const OriginInScan& origin : m_gathered)
            addEdge(origin.node, join, origin.passes);
        m_gathered.shrink(0);
        m_gathered.append(OriginInScan { join, { } });
    }

    ListInScan list { static_cast<uint32_t>(m_origins.size()), static_cast<uint32_t>(m_gathered.size()) };
    m_origins.appendVector(m_gathered);
    for (uint32_t member : members)
        m_resolved[member] = list;
}

OriginInScan ScanOfBody::behindTest(OriginInScan origin, TypeID passes)
{
    if (!passes)
        return origin;
    if (!origin.node)
        return { constant(passes), { } };
    return narrowerOfTwoTests(origin, passes);
}

void ScanOfBody::normalize(Vector<OriginInScan>& origins)
{
    if (origins.size() < 2)
        return;
    std::sort(origins.begin(), origins.end(), [](const OriginInScan& first, const OriginInScan& second) {
        return first.node < second.node;
    });
    size_t kept = 0;
    for (size_t index = 0; index < origins.size(); ++index) {
        if (kept && origins[kept - 1].node == origins[index].node) {
            if (origins[kept - 1].passes != origins[index].passes)
                origins[kept - 1].passes = TypeID();
            continue;
        }
        origins[kept++] = origins[index];
    }
    origins.shrink(kept);

    auto isPrimitiveConstant = [&](const OriginInScan& origin) {
        return !origin.passes && !!primitiveConstantAt(origin.node);
    };
    TypeID united;
    unsigned numberOfConstants = 0;
    for (const OriginInScan& origin : origins) {
        if (isPrimitiveConstant(origin))
            united = numberOfConstants++ ? unionOfPrimitiveTypesInScan(united, primitiveConstantAt(origin.node)) : primitiveConstantAt(origin.node);
    }
    if (numberOfConstants < 2)
        return;
    origins.removeAllMatching(isPrimitiveConstant);
    origins.append(OriginInScan { constant(united), { } });
    std::sort(origins.begin(), origins.end(), [](const OriginInScan& first, const OriginInScan& second) {
        return first.node < second.node;
    });
}

void ScanOfBody::expand(ListInScan list)
{
    m_expanded.shrink(0);
    for (uint32_t position = 0; position < list.count; ++position) {
        uint32_t atom = m_atoms[list.first + position];
        if (!(atom & parameterBit)) {
            m_expanded.append(OriginInScan { nodeOfAtom(atom), { } });
            continue;
        }
        ListInScan resolved = m_resolved[atom & ~parameterBit];
        m_expanded.append(m_origins.span().subspan(resolved.first, resolved.count));
    }
    if (list.count > 1)
        normalize(m_expanded);
}

uint32_t ScanOfBody::oneNodeFor(ListInScan list)
{
    if (!list.count)
        return 0;
    if (uint32_t known = m_nodesOfLists[list.first])
        return known - 1;

    expand(list);
    uint32_t node = 0;
    if (m_expanded.size() == 1 && !m_expanded[0].passes)
        node = m_expanded[0].node;
    else if (!m_expanded.isEmpty()) {
        node = newJoin();
        for (const OriginInScan& origin : m_expanded)
            addEdge(origin.node, node, origin.passes);
    }
    m_nodesOfLists[list.first] = node + 1;
    return node;
}

void ScanOfBody::applySinks()
{
    m_nodesOfLists.fill(0, m_atoms.size());
    m_firstCallOfConversion.fill(noConversion, m_atoms.size());
    for (const SinkInScan& sink : m_sinks) {
        switch (sink.kind) {
        case SinkKindInScan::Edge:
            expand(sink.value);
            for (const OriginInScan& origin : m_expanded) {
                if (sink.passes == TypeTable::object && isAlwaysPrimitive(origin))
                    continue;
                OriginInScan from = behindTest(origin, sink.passes);
                bool leavesNoEdge = !from.node || m_summary.nodes[from.node].kind == SummaryNode::Kind::Constant;
                if (sink.runsCodeOfValue && leavesNoEdge && !primitiveConstantAt(from.node))
                    m_summary.ownEffects.merge(Effects::unknownCode());
                addEdge(from.node, sink.target, from.passes, sink.runsCodeOfValue);
            }
            break;
        case SinkKindInScan::Argument:
            m_summary.arguments[sink.target] = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfCall:
            m_summary.calls[sink.target].subject = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ReceiverOfCall:
            m_summary.calls[sink.target].receiver = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::NewTargetOfCall:
            m_summary.calls[sink.target].newTarget = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfAccess:
            m_summary.accesses[sink.target].subject = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ReceiverOfAccess:
            m_summary.accesses[sink.target].receiver = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::KeyOfAccess:
            m_summary.accesses[sink.target].keyNode = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ValueOfAccess:
            m_summary.accesses[sink.target].value = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfChange:
            m_summary.changes[sink.target].subject = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::OtherOfChange:
            m_summary.changes[sink.target].other = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ValueOfMember: {
            uint32_t value = oneNodeFor(sink.value);
            SummaryMember& member = m_summary.members[sink.target];
            member.value = value;
            addEdge(value, nodeFor(SummaryNode::Kind::Member, member.classLayout.index(), member.index | (member.side == ClassSide::Static ? staticSideBit : 0)));
            break;
        }
        case SinkKindInScan::GetterInSightOfChange:
            m_summary.changes[sink.target].getterInSight = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SetterInSightOfChange:
            m_summary.changes[sink.target].setterInSight = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ValueInSightOfChange:
            m_summary.changes[sink.target].valueInSight = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfInspection:
            m_summary.inspections[sink.target].subject = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ConvertedToPrimitive:
            applyConversionToPrimitive(sink);
            break;
        }
    }
    for (const OperatorInScan& found : m_operators)
        applyOperator(found);
    for (const StoreInScan& store : m_stores)
        applyStore(store);
}

void ScanOfBody::followObjectsMadeToBePrototypes()
{
    for (const StoreOfPrototypeInScan& candidate : m_storesOfPrototypes) {
        uint32_t root = candidate.rootOfStored;
        uint32_t position = m_stores[candidate.store].site.index();
        BodyValue made = definitionOf(root);
        if (!made || m_firstLeft[root] != position || m_lastLeft[root] != position || m_uses[root] & isAccessedBesideNamedStores)
            continue;
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_stores[candidate.store].block];
        if (head != noLoop && head > m_blocks[root])
            continue;
        HeirMadeByBuiltinInScan* heir = nullptr;
        if (made.opcode() != Opcode::NewDictionary) {
            for (HeirMadeByBuiltinInScan& listed : m_heirsMadeByBuiltins) {
                if (listed.call == root && listed.isMadeEmpty && !listed.isFollowed)
                    heir = &listed;
            }
            if (!heir)
                continue;
        }

        m_stores[candidate.store].storesObjectMadeForIt = true;
        uint32_t standsForObject = addAccessUnder(SummaryAccess::Kind::Read, ValueID(root), m_stores[candidate.store].base, m_stores[candidate.store].key, ValueID());
        m_summary.accesses[standsForObject].standsForObjectMadeAtSite = true;
        ListInScan known = listOfNode(resultOfRead(standsForObject));
        for (StoreInScan& store : m_stores) {
            if (store.root != root)
                continue;
            store.base = known;
            store.isIntoObjectKnownByRead = true;
        }
        if (heir) {
            heir->isFollowed = true;
            m_summary.changes[heir->change].kind = SummaryChange::Kind::ChangesPrototype;
            m_sinks[heir->sinkOfSubject].value = known;
            addSink(SinkKindInScan::OtherOfChange, heir->above, heir->change);
        }
    }
    for (const HeirMadeByBuiltinInScan& heir : m_heirsMadeByBuiltins) {
        if (!heir.isFollowed)
            escape(heir.above);
    }
}

static bool holdsOnlyWhatItsBodyLaysInScan(const Program& program, const BodyValue& made)
{
    if (!made)
        return false;
    for (BuiltinName name : { BuiltinNames::enumerable, BuiltinNames::configurable, BuiltinNames::value, BuiltinNames::writable, BuiltinNames::get, BuiltinNames::set }) {
        if (StringID text = program.stringOf(name); text && program.isOpenedSomewhere(text))
            return false;
    }
    if (made.opcode() == Opcode::NewDictionary)
        return true;
    return made.opcode() == Opcode::NewObject && !made.as<Opcode::NewObject>().prototypeOperand() && program.at(made.as<Opcode::NewObject>().layout()).kind == LayoutKind::Record;
}

void ScanOfBody::layDescriptorsInSight()
{
    PropertyKeyID get = m_program.keyOf(BuiltinNames::get);
    PropertyKeyID set = m_program.keyOf(BuiltinNames::set);
    PropertyKeyID valueKey = m_program.keyOf(BuiltinNames::value);
    struct Held {
        PropertyKeyID key;
        ListInScan list;
        uint32_t field { noSummaryNode };
    };
    Vector<Held, 6> held;
    for (const DefinitionByDescriptorInScan& definition : m_definitionsByDescriptors) {
        if (!definition.waitsForItsJudgment)
            continue;
        uint32_t root = definition.rootOfDescriptor;
        BodyValue made = definitionOf(root);
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[definition.position]];
        bool isInSight = holdsOnlyWhatItsBodyLaysInScan(m_program, made) && !(m_uses[root] & isAccessedBesideNamedStores) && (head == noLoop || head <= m_blocks[root]);
        held.shrink(0);
        if (isInSight && made.opcode() == Opcode::NewObject) {
            const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
            std::span<const uint32_t> fields = made.as<Opcode::NewObject>().fields();
            std::span<const ValueID> values = made.variadicOperands();
            for (size_t place = 0; place < fields.size() && isInSight; ++place) {
                isInSight = fields[place] < layout.fields.size() && place < values.size() && values[place] && layout.fields[fields[place]].key && layout.fields[fields[place]].form.isEmpty();
                if (isInSight)
                    held.append({ layout.fields[fields[place]].key, listOf(values[place]), fieldNode(made.as<Opcode::NewObject>().layout(), fields[place]) });
            }
        }
        if (isInSight) {
            for (uint32_t index : storesInto(root)) {
                const StoreInScan& store = m_stores[index];
                if (store.site.index() >= definition.position)
                    continue;
                isInSight = isInSight && !!store.key && (store.block == m_blocks[root] || store.block == m_blocks[definition.position]);
                held.append({ store.key, store.value });
            }
        }
        auto isNamedOnceAtMost = [&](PropertyKeyID key) {
            return std::ranges::count(held, key, &Held::key) <= 1;
        };
        if (!isInSight || !isNamedOnceAtMost(get) || !isNamedOnceAtMost(set)) {
            escape(definition.descriptor);
            continue;
        }

        m_summary.changes[definition.change].descriptorIsInSight = true;
        auto flagWritten = [&](BuiltinName name) -> std::optional<bool> {
            PropertyKeyID key = m_program.keyOf(name);
            std::optional<bool> flag = false;
            unsigned timesNamed = 0;
            for (const Held& member : held) {
                if (member.key != key)
                    continue;
                TypeID constant = member.list.count == 1 ? primitiveConstantAt(m_atoms[member.list.first]) : TypeID();
                uint16_t primitives = constant ? primitivesInID(constant) : 0;
                if (++timesNamed > 1 || !primitives)
                    flag = std::nullopt;
                else if (primitives == Primitive::True)
                    flag = true;
                else if (primitives & ~(Primitive::False | Primitive::UndefinedOrNull))
                    flag = std::nullopt;
            }
            return flag;
        };
        auto isNamed = [&](PropertyKeyID key) {
            return std::ranges::contains(held, key, &Held::key);
        };
        std::optional<bool> isEnumerable = flagWritten(BuiltinNames::enumerable);
        std::optional<bool> isConfigurable = flagWritten(BuiltinNames::configurable);
        std::optional<bool> isWritable = flagWritten(BuiltinNames::writable);
        bool isAccessor = isNamed(get) || isNamed(set);
        bool isRefused = isAccessor && (isNamed(valueKey) || isNamed(m_program.keyOf(BuiltinNames::writable)));
        if (definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName && isEnumerable && isConfigurable && isWritable && !isRefused) {
            OptionSet<FieldForm> form;
            if (isAccessor)
                form.add(FieldForm::IsAccessor);
            else if (!*isWritable)
                form.add(FieldForm::IsReadOnly);
            if (!*isEnumerable)
                form.add(FieldForm::IsNotEnumerable);
            if (!*isConfigurable)
                form.add(FieldForm::IsNotConfigurable);
            m_summary.accesses[definition.access].formCreated = form;
        }
        auto heldUnderValue = std::ranges::find(held, valueKey, &Held::key);
        bool definitionBringsValue = definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName && !isAccessor && std::ranges::count(held, valueKey, &Held::key) == 1 && isEnumerable && isConfigurable && isWritable && *isWritable
            && heldUnderValue->field != noSummaryNode && m_firstLeft[root] == definition.position && m_lastLeft[root] == definition.position;
        m_summary.changes[definition.change].laysValueInSight = definitionBringsValue;
        for (const Held& member : held) {
            if (member.key != get && member.key != set && member.key != valueKey)
                continue;
            if (definitionBringsValue) {
                for (SinkInScan& sink : m_sinks) {
                    if (sink.kind != SinkKindInScan::Edge || sink.target != member.field || sink.value.first != member.list.first || sink.value.count != member.list.count)
                        continue;
                    sink.value.count = 0;
                    break;
                }
                addSink(SinkKindInScan::ValueOfAccess, member.list, definition.access);
                addSink(SinkKindInScan::ValueInSightOfChange, member.list, definition.change);
                continue;
            }
            escape(member.list);
            if (member.key != valueKey)
                addSink(member.key == get ? SinkKindInScan::GetterInSightOfChange : SinkKindInScan::SetterInSightOfChange, member.list, definition.change);
        }
    }
}

void ScanOfBody::nameGettersOfDescriptors()
{
    PropertyKeyID get = m_program.keyOf(BuiltinNames::get);
    for (const DefinitionByDescriptorInScan& definition : m_definitionsByDescriptors) {
        uint32_t root = definition.rootOfDescriptor;
        if (definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName)
            nameAccessorWrittenInDescriptor(definition.access, root, definition.position);
        BodyValue made = definitionOf(root);
        if (!holdsOnlyWhatItsBodyLaysInScan(m_program, made) || m_uses[root] & isAccessedBesideNamedStores)
            continue;
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[definition.position]];
        if (head != noLoop && head > m_blocks[root])
            continue;

        std::optional<ListInScan> held;
        bool isNamedOnce = true;
        if (made.opcode() == Opcode::NewObject) {
            const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
            std::span<const uint32_t> fields = made.as<Opcode::NewObject>().fields();
            std::span<const ValueID> values = made.variadicOperands();
            for (size_t place = 0; place < fields.size(); ++place) {
                if (fields[place] >= layout.fields.size() || layout.fields[fields[place]].key != get)
                    continue;
                isNamedOnce = !held && place < values.size() && values[place];
                if (isNamedOnce)
                    held = listOf(values[place]);
            }
        }
        for (const StoreInScan& store : m_stores) {
            if (store.root != root || store.key != get || store.site.index() >= definition.position)
                continue;
            isNamedOnce = isNamedOnce && !held;
            held = store.value;
        }
        if (held && isNamedOnce)
            m_summary.changes[definition.change].getter = oneNodeFor(*held);
    }
}

std::span<const uint32_t> ScanOfBody::storesInto(uint32_t root)
{
    if (!m_storesAreSortedByRoot) {
        m_storesAreSortedByRoot = true;
        m_storesByRoot.shrink(0);
        for (uint32_t index = 0; index < m_stores.size(); ++index)
            m_storesByRoot.append(index);
        std::ranges::stable_sort(m_storesByRoot, { }, [&](uint32_t index) {
            return m_stores[index].root;
        });
    }
    auto found = std::ranges::equal_range(m_storesByRoot, root, { }, [&](uint32_t index) {
        return m_stores[index].root;
    });
    return m_storesByRoot.span().subspan(found.begin() - m_storesByRoot.begin(), found.size());
}

void ScanOfBody::nameAccessorWrittenInDescriptor(uint32_t access, uint32_t root, uint32_t position)
{
    BodyValue made = definitionOf(root);
    if (!holdsOnlyWhatItsBodyLaysInScan(m_program, made) || m_uses[root] & isAccessedBesideNamedStores)
        return;
    uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[position]];
    if (head != noLoop && head > m_blocks[root])
        return;
    PropertyKeyID get = m_program.keyOf(BuiltinNames::get);
    PropertyKeyID set = m_program.keyOf(BuiltinNames::set);
    std::optional<ListInScan> halves[2];
    auto take = [&](PropertyKeyID key, ListInScan held) {
        if (!key || key == m_program.keyOf(BuiltinNames::value))
            return false;
        if (key != get && key != set)
            return true;
        std::optional<ListInScan>& half = halves[key == set];
        if (half)
            return false;
        half = held;
        return true;
    };
    if (made.opcode() == Opcode::NewObject) {
        const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
        std::span<const uint32_t> fields = made.as<Opcode::NewObject>().fields();
        std::span<const ValueID> values = made.variadicOperands();
        for (size_t place = 0; place < fields.size(); ++place) {
            if (fields[place] >= layout.fields.size() || place >= values.size() || !values[place] || !take(layout.fields[fields[place]].key, listOf(values[place])))
                return;
        }
    }
    for (uint32_t index : storesInto(root)) {
        const StoreInScan& store = m_stores[index];
        if (store.site.index() < position && !take(store.key, store.value))
            return;
    }
    if (!halves[0] && !halves[1])
        return;
    FunctionID functions[2];
    for (unsigned half = 0; half < 2; ++half) {
        if (!halves[half])
            continue;
        const SummaryNode& held = m_summary.nodes[oneNodeFor(*halves[half])];
        if (held.kind != SummaryNode::Kind::Closure)
            return;
        functions[half] = FunctionID(held.first);
    }
    m_summary.accesses[access].laysAccessorAsWritten = true;
    m_summary.accesses[access].writtenGetter = functions[0];
    m_summary.accesses[access].writtenSetter = functions[1];
}

void ScanOfBody::nameKeysOfDefinitionsByMaps()
{
    for (const DefinitionByMapInScan& definition : m_definitionsByMaps) {
        uint32_t root = definition.rootOfMap;
        BodyValue made = definitionOf(root);
        if (!holdsOnlyWhatItsBodyLaysInScan(m_program, made) || m_uses[root] & isAccessedBesideNamedStores)
            continue;
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[definition.position]];
        if (head != noLoop && head > m_blocks[root])
            continue;
        std::span<const uint32_t> listed;
        if (made.opcode() == Opcode::NewObject) {
            listed = made.as<Opcode::NewObject>().fields();
            const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
            bool eachIsNamed = listed.size() <= made.variadicOperands().size() && std::ranges::all_of(listed, [&](uint32_t field) {
                return field < layout.fields.size() && layout.fields[field].key;
            });
            if (!eachIsNamed)
                continue;
        }

        bool isFirst = true;
        auto nameKey = [&](PropertyKeyID key, ValueID descriptor) {
            if (!isFirst) {
                m_summary.changes.append(SummaryChange(m_summary.changes[definition.change]));
                m_summary.accesses.append(SummaryAccess(m_summary.accesses[definition.access]));
            }
            uint32_t placeOfAccess = isFirst ? definition.access : static_cast<uint32_t>(m_summary.accesses.size() - 1);
            SummaryChange& change = isFirst ? m_summary.changes[definition.change] : m_summary.changes.last();
            SummaryAccess& access = m_summary.accesses[placeOfAccess];
            change.key = key;
            access.key = key;
            access.keyNode = noSummaryNode;
            access.laysAccessorAsWritten = false;
            access.writtenGetter = { };
            access.writtenSetter = { };
            access.mapOfWrittenAccessor = { };
            access.descriptorFromElsewhere = noSummaryNode;
            isFirst = false;
            if (!descriptor)
                return;
            nameAccessorWrittenInDescriptor(placeOfAccess, m_roots[descriptor.index()], definition.position);
            if (m_summary.accesses[placeOfAccess].laysAccessorAsWritten) {
                if (made.opcode() == Opcode::NewObject)
                    m_summary.accesses[placeOfAccess].mapOfWrittenAccessor = made.as<Opcode::NewObject>().layout();
                return;
            }
            uint32_t from = oneNodeFor(listOf(descriptor));
            m_summary.accesses[placeOfAccess].descriptorFromElsewhere = from;
        };
        for (size_t place = 0; place < listed.size(); ++place)
            nameKey(m_program.at(made.as<Opcode::NewObject>().layout()).fields[listed[place]].key, made.variadicOperands()[place]);
        for (uint32_t index : storesInto(root)) {
            StoreInScan store = m_stores[index];
            if (store.site.index() >= definition.position)
                continue;
            BodyValue operation = definitionOf(store.site.index());
            bool storesOperand = operation && (operation.opcode() == Opcode::PropDefine || operation.opcode() == Opcode::PropSet);
            nameKey(store.key, storesOperand ? operation.valueOperand() : ValueID());
        }
    }
}

void ScanOfBody::applyConversionToPrimitive(const SinkInScan& sink)
{
    expand(sink.value);
    uint32_t objects = 0;
    for (const OriginInScan& origin : m_expanded) {
        if (isAlwaysPrimitive(origin))
            continue;
        if (!objects)
            objects = newJoin();
        OriginInScan from = behindTest(origin, TypeTable::object);
        addEdge(from.node, objects, from.passes);
    }
    if (!objects)
        return;

    const BuiltinTable& builtins = m_program.builtins();
    BuiltinName memberTakingHint = builtins.nameOf(WellKnownSymbol::ToPrimitive);
    bool isInLoop = !m_innermostLoopHeads.isEmpty() && m_innermostLoopHeads[m_blocks[sink.target]] != noLoop;
    m_firstCallOfConversion[sink.value.first] = static_cast<uint32_t>(m_summary.calls.size());
    for (BuiltinName name : builtins.keysReadBy(Protocol::ToPrimitive)) {
        SummaryCall call;
        call.site = ValueID(sink.target);
        call.callee = SummaryCall::Callee::Name;
        call.isMadeByProtocol = true;
        call.isInLoop = isInLoop;
        call.key = m_program.keyOf(name);
        call.receiver = objects;
        call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
        if (name == memberTakingHint) {
            m_summary.arguments.append(constant(TypeTable::string));
            call.numberOfArguments = 1;
        }
        m_summary.calls.append(call);
    }
}

void ScanOfBody::applyOperator(const OperatorInScan& found)
{
    if (found.record == OperatorInScan::noRecord)
        return;
    for (uint32_t operand = 0; operand < found.numberOfOperands; ++operand) {
        ListInScan list = found.operands[operand];
        uint32_t node = oneNodeFor(list);
        uint32_t firstCall = list.count ? m_firstCallOfConversion[list.first] : noConversion;
        uint32_t converted = firstCall == noConversion ? noSummaryNode : nodeFor(SummaryNode::Kind::ResultOfConversion, firstCall);
        m_summary.operators[found.record].operands[operand] = node;
        m_summary.operators[found.record].converted[operand] = converted;
    }
}

void ScanOfBody::applyStore(const StoreInScan& store)
{
    bool runsAccessor = !store.isDefinition && hasAccessorLaidUnder(store.root, store.key);
    bool isAlone = !runsAccessor && !store.isIntoObjectKnownByRead && isStillAlone(store.root, store.site.index(), store.block);
    bool everyFieldIsKnown = false;
    m_fieldsStoredInto.shrink(0);
    if (isAlone) {
        expand(store.base);
        everyFieldIsKnown = !m_expanded.isEmpty();
        for (const OriginInScan& origin : m_expanded) {
            SummaryNode node = m_summary.nodes[origin.node];
            uint32_t field = 0;
            if (node.kind == SummaryNode::Kind::Allocation && !origin.passes) {
                LayoutID layout(node.first);
                if (std::optional<uint32_t> found = m_program.at(layout).findField(store.key))
                    field = dataFieldNode(layout, *found);
            }
            if (!field) {
                everyFieldIsKnown = false;
                break;
            }
            m_fieldsStoredInto.append(field);
        }
    }

    if (everyFieldIsKnown) {
        expand(store.value);
        for (const OriginInScan& origin : m_expanded) {
            for (uint32_t field : m_fieldsStoredInto)
                addEdge(origin.node, field, origin.passes);
        }
        noteStoreThatBecameEdge(store);
        if (!store.isDefinition)
            m_summary.namesStored.append(store.key);
        return;
    }

    SummaryAccess access;
    access.site = store.site;
    access.kind = store.isDefinition ? SummaryAccess::Kind::Define : SummaryAccess::Kind::Store;
    access.isPartOfConstruction = isAlone;
    access.storesObjectMadeForIt = store.storesObjectMadeForIt;
    access.subject = oneNodeFor(store.base);
    access.key = store.key;
    access.value = oneNodeFor(store.value);
    m_summary.accesses.append(access);
}

void ScanOfBody::finish()
{
    if (m_function->isCoroutine()) {
        if (uint32_t result = resultNode(0))
            addEdge(objectOfCoroutine(), result);
    }
    letFirstValuesOfSlotsArrive();
    settleWhereRecordsThrow();
    finishKnownEffects();
    if (m_summary.ownEffects.mayThrow) {
        m_summary.traits.add(SummaryTrait::MayThrow);
        addEdge(0, nodeFor(SummaryNode::Kind::Thrown));
    }

    removeRepeatedEdges();
    listNamesSpelled();
    finishListsOfStringsInScan(m_summary);
    listConstructions();
    sortWithoutRepeatsInScan(m_summary.classesEvaluated);
    sortWithoutRepeatsInScan(m_summary.modulesHandedOut);
    sortWithoutRepeatsInScan(m_summary.otherModulesNamed);
    listFirstStoresAndUses();
    listDefinitions();
    sortWithoutRepeatsInScan(m_summary.modulesNamedInBlocksNeverEntered);
    sortWithoutRepeatsInScan(m_summary.functionsNamed);
    m_summary.nodes.shrinkToFit();
    m_summary.calls.shrinkToFit();
    m_summary.arguments.shrinkToFit();
    m_summary.accesses.shrinkToFit();
    m_summary.operators.shrinkToFit();
    m_summary.changes.shrinkToFit();
    m_summary.members.shrinkToFit();
    m_summary.constructions.shrinkToFit();
    m_summary.keysOfConstructions.shrinkToFit();
    m_summary.partsOfConstructions.shrinkToFit();
}

void ScanOfBody::listNamesSpelled()
{
    m_sitesOfRows.shrink(0);
    for (const SummaryCall& call : m_summary.calls) {
        if (call.callee == SummaryCall::Callee::Name && call.key)
            m_summary.namesCalled.append(call.key);
        if (call.callee == SummaryCall::Callee::Builtin && !call.isMadeByProtocol)
            m_sitesOfRows.append(call.site);
    }
    std::sort(m_sitesOfRows.begin(), m_sitesOfRows.end());
    auto note = [&](ValueID site, PropertyKeyID key, Vector<PropertyKeyID>& names, SummaryTrait underComputedKey) {
        if (std::binary_search(m_sitesOfRows.begin(), m_sitesOfRows.end(), site))
            return;
        if (key)
            names.append(key);
        else
            m_summary.traits.add(underComputedKey);
    };

    for (const SummaryAccess& access : m_summary.accesses) {
        if (access.isToContents())
            continue;
        switch (access.kind) {
        case SummaryAccess::Kind::Read:
            note(access.site, access.key, m_summary.namesRead, SummaryTrait::ReadsUnderComputedKey);
            break;
        case SummaryAccess::Kind::Store:
        case SummaryAccess::Kind::Define:
            note(access.site, access.key, m_summary.namesStored, SummaryTrait::StoresUnderComputedKey);
            break;
        case SummaryAccess::Kind::Has:
            break;
        }
    }
    for (const SummaryChange& change : m_summary.changes) {
        if (change.kind == SummaryChange::Kind::Deletes)
            note(change.site, change.key, m_summary.namesStored, SummaryTrait::StoresUnderComputedKey);
    }
    sortWithoutRepeatsInScan(m_summary.namesRead);
    sortWithoutRepeatsInScan(m_summary.namesCalled);
    sortWithoutRepeatsInScan(m_summary.namesStored);
    sortWithoutRepeatsInScan(m_summary.textsThatNameEvents);
}

// The number of a type is never sorted by: among the edges between two nodes the order in which they were found stays.
void ScanOfBody::removeRepeatedEdges()
{
    Vector<SummaryEdge>& edges = m_summary.edges;
    std::stable_sort(edges.begin(), edges.end(), [](const SummaryEdge& first, const SummaryEdge& second) {
        return std::pair { first.to, first.from } < std::pair { second.to, second.from };
    });

    size_t kept = 0;
    for (size_t first = 0; first < edges.size();) {
        size_t end = first;
        bool passesAll = false;
        bool runsCodeOfValue = false;
        for (; end < edges.size() && edges[end].to == edges[first].to && edges[end].from == edges[first].from; ++end) {
            passesAll |= !edges[end].passes;
            runsCodeOfValue |= edges[end].runsCodeOfValue;
        }

        size_t firstKept = kept;
        for (size_t index = first; index < end; ++index) {
            SummaryEdge edge = edges[index];
            edge.runsCodeOfValue = runsCodeOfValue;
            if (passesAll)
                edge.passes = TypeID();
            bool isRepeated = false;
            for (size_t earlier = firstKept; earlier < kept; ++earlier)
                isRepeated |= edges[earlier].passes == edge.passes;
            if (!isRepeated)
                edges[kept++] = edge;
        }
        first = end;
    }
    edges.shrink(kept);
    edges.shrinkToFit();
}

LayoutID classOfConstructorFunction(const Program& program, FunctionID function)
{
    LayoutID constructed = function ? program.at(function).constructedClass : LayoutID();
    if (!constructed || !program.at(constructed).has(LayoutFlags::IsConstructorFunction) || program.at(constructed).constructor != function)
        return { };
    return constructed;
}

ThinSummary scanBody(const Program& program, Diagnostics& diagnostics, FunctionID function, Mechanisms mechanisms)
{
    BitVector noTexts;
    ScanOfBody scan(program, mechanisms, noTexts);
    ThinSummary summary;
    scan.run(function, summary);
    if (std::optional<InternalError> error = scan.takeError())
        diagnostics.add(*error);
    return summary;
}

void scanBodiesAppendedToProgram(const Program& program, Diagnostics& diagnostics, ThinSummaries& summaries, Mechanisms mechanisms)
{
    auto numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    if (summaries.size() >= numberOfFunctions)
        return;
    BitVector noTexts;
    ScanOfBody scan(program, mechanisms, noTexts);
    for (auto index = static_cast<uint32_t>(summaries.size()); index < numberOfFunctions; ++index) {
        ThinSummary summary;
        scan.run(FunctionID(index), summary);
        summaries.append(WTF::move(summary));
    }
    if (std::optional<InternalError> error = scan.takeError())
        diagnostics.add(*error);
}

void ScanOfBody::noteTextsSpelledIn(const DataNode& root)
{
    Vector<const DataNode*, 32> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const DataNode& node = *nodesToVisit.takeLast();
        if (const auto* string = node.tryAs<DataNode::String>())
            noteTextSpelled(string->value);
        else if (const auto* array = node.tryAs<DataNode::Array>()) {
            for (const DataNode& element : array->elements)
                nodesToVisit.append(&element);
        } else if (const auto* object = node.tryAs<DataNode::Object>()) {
            for (const DataMember& member : object->members)
                nodesToVisit.append(&member.value);
        }
    }
}

static BitVector findTextsThatNameFieldsNeverWrittenForScan(const Program& program)
{
    BitVector texts;
    for (const Layout& layout : program.layouts) {
        for (const Field& field : layout.fields) {
            if (!field.key || !field.flags.contains(FieldFlags::IsNeverWritten))
                continue;
            const PropertyKey& key = program.at(field.key);
            if (key.kind == PropertyKeyKind::Text && key.text)
                texts.set(key.text.index());
        }
    }
    return texts;
}

ThinSummaries scanBodies(const Program& program, Diagnostics& diagnostics, const ParallelLoops& loops, Mechanisms mechanisms)
{
    constexpr size_t functionsPerTask = 32;
    size_t numberOfFunctions = program.functions.size();
    ThinSummaries summaries(numberOfFunctions, loops);
    BitVector textsThatNameFieldsNeverWritten = findTextsThatNameFieldsNeverWrittenForScan(program);
    BitVector namesUnderWhichEventsHang = namesUnderWhichEventsHangInScan(program.builtins());
    Vector<std::unique_ptr<ScanOfBody>> scans(loops.width());
    auto scanOf = [&](unsigned worker) -> ScanOfBody& {
        std::unique_ptr<ScanOfBody>& scan = scans[worker];
        if (!scan)
            scan = makeUnique<ScanOfBody>(program, mechanisms, textsThatNameFieldsNeverWritten, &namesUnderWhichEventsHang);
        return *scan;
    };
    forEachIndexInParallel(loops, static_cast<unsigned>((numberOfFunctions + functionsPerTask - 1) / functionsPerTask), [&](unsigned task, unsigned worker) {
        ScanOfBody& scan = scanOf(worker);
        for (size_t index = task * functionsPerTask, end = std::min(numberOfFunctions, index + functionsPerTask); index < end; ++index)
            scan.run(FunctionID(index), summaries.at(FunctionID(index)));
    });

    if (!textsThatNameFieldsNeverWritten.isEmpty()) {
        forEachIndexInParallel(loops, static_cast<unsigned>(program.data.size()), [&](unsigned index, unsigned worker) {
            scanOf(worker).noteTextsSpelledIn(program.data[index].root);
        });
        BitVector textsSpelled;
        for (std::unique_ptr<ScanOfBody>& scan : scans) {
            if (scan)
                textsSpelled.merge(scan->textsSpelled());
        }
        summaries.setTextsSpelledThatNameFieldsNeverWritten(WTF::move(textsSpelled));
    }

    std::optional<InternalError> firstError;
    for (std::unique_ptr<ScanOfBody>& scan : scans) {
        std::optional<InternalError> error = scan ? scan->takeError() : std::nullopt;
        if (error && (!firstError || error->function.index() < firstError->function.index()))
            firstError = WTF::move(error);
    }
    if (firstError)
        diagnostics.add(*firstError);
    forEachIndexInParallel(loops, static_cast<unsigned>(scans.size()), [&](unsigned worker) {
        scans[worker] = nullptr;
    });
    return summaries;
}

} } } // namespace JSC::AOT::Typed

#include "AOTScanBirthsInlines.h"
#include "AOTScanEnvironmentsInlines.h"

#include "AOTScanExceptionsInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#include "AOTScanKnownEffectsInlines.h"

#endif // ENABLE(AOT)
