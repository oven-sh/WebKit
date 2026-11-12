/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThinSummary.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTIterationHelpers.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTLinkStrings.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTScanKnownEffects.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSchemaResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static bool holdsOnlyWhatItsBodyLaysInScan(const Program&, const BodyValue&);

namespace {

static constexpr size_t mostSourcesOfHeritageLaidOneByOneInScan = 8;

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
    static constexpr uint32_t noParameter = UINT32_MAX;

    uint32_t block { 0 };
    uint32_t position { 0 };
    ListInScan list;
    TypeID passes;
    uint32_t parameter { noParameter };
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
    TargetOfCopyOfMembers,
    SourceOfCopyOfMembers,
    PlaceOfWrittenList,
    OtherOperandOfEquality,
    ConvertedByEquality,
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
    bool storesSurelyIndex { false };
    bool storesZero { false };
};

struct AccessorLaidInScan {
    uint32_t root { 0 };
    PropertyKeyID key;
    FunctionID function;
    bool isGetter { false };
    uint32_t sinkOfSubject { 0 };
};

struct EqualityInScan {
    ValueID site;
    ListInScan converted;
    ListInScan other;
};

struct OperatorInScan {
    static constexpr uint32_t noRecord = UINT32_MAX;

    ValueID site;
    Opcode opcode { Opcode::Add };
    bool isNeeded { false };
    SummaryOperator::WrittenDivisor writtenDivisor { SummaryOperator::WrittenDivisor::None };
    bool shiftsRightByConstant { false };
    int8_t step { 0 };
    uint8_t steppedOperand { 0 };
    PropertyKeyID steppedField;
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

class SetsOfFillsInScan {
public:
    using Set = uint32_t;
    static constexpr Set empty = 0;

    void clear() { m_nodes.shrink(0); }

    Set with(Set set, const FillInScan& fill) { return withKey(set, keyOf(fill)); }
    bool contains(Set set, const FillInScan& fill) const { return containsKey(set, keyOf(fill)); }

    Set meet(Set first, Set second)
    {
        if (first == second)
            return first;
        if (!first || !second)
            return empty;
        Node nodeOfFirst = m_nodes[first];
        Node nodeOfSecond = m_nodes[second];
        if (nodeOfFirst.isLeaf())
            return containsKey(second, nodeOfFirst.bits) ? first : empty;
        if (nodeOfSecond.isLeaf())
            return containsKey(first, nodeOfSecond.bits) ? second : empty;
        if (nodeOfFirst.bits == nodeOfSecond.bits) {
            Set zero = meet(nodeOfFirst.zero, nodeOfSecond.zero);
            Set one = meet(nodeOfFirst.one, nodeOfSecond.one);
            if (zero == nodeOfFirst.zero && one == nodeOfFirst.one)
                return first;
            if (zero == nodeOfSecond.zero && one == nodeOfSecond.one)
                return second;
            if (!zero || !one)
                return zero | one;
            return makeNode(nodeOfFirst.bits, zero, one);
        }
        if (nodeOfFirst.bit() > nodeOfSecond.bit())
            return nodeOfFirst.isAbove(nodeOfSecond.prefix()) ? meet(nodeOfFirst.sideOf(nodeOfSecond.prefix()), second) : empty;
        if (nodeOfSecond.bit() > nodeOfFirst.bit())
            return nodeOfSecond.isAbove(nodeOfFirst.prefix()) ? meet(first, nodeOfSecond.sideOf(nodeOfFirst.prefix())) : empty;
        return empty;
    }

    template<typename Functor>
    void forEachFillOf(Set set, uint32_t root, const Functor& functor) const
    {
        forEachKeyBetween(set, keyOf({ root, false, 0 }), keyOf({ root, true, UINT32_MAX }), functor);
    }

    template<typename Functor>
    void forEachFillMissingIn(Set set, Set other, const Functor& functor) const
    {
        if (set == other || !set)
            return;
        Node node = m_nodes[set];
        if (node.isLeaf()) {
            if (!containsKey(other, node.bits))
                functor(fillOf(node.bits));
            return;
        }
        Set besideZero = empty;
        Set besideOne = empty;
        if (other) {
            Node nodeOfOther = m_nodes[other];
            uint64_t bitOfOther = nodeOfOther.isLeaf() ? 0 : nodeOfOther.bit();
            uint64_t prefixOfOther = nodeOfOther.isLeaf() ? nodeOfOther.bits : nodeOfOther.prefix();
            if (node.bits == nodeOfOther.bits && bitOfOther) {
                besideZero = nodeOfOther.zero;
                besideOne = nodeOfOther.one;
            } else if (node.bit() > bitOfOther) {
                if (node.isAbove(prefixOfOther))
                    (prefixOfOther & node.bit() ? besideOne : besideZero) = other;
            } else if (bitOfOther > node.bit() && nodeOfOther.isAbove(node.prefix()))
                return forEachFillMissingIn(set, nodeOfOther.sideOf(node.prefix()), functor);
        }
        forEachFillMissingIn(node.zero, besideZero, functor);
        forEachFillMissingIn(node.one, besideOne, functor);
    }

private:
    struct Node {
        bool isLeaf() const { return !zero; }
        uint64_t bit() const { return bits & (~bits + 1); }
        uint64_t prefix() const { return bits ^ bit(); }
        bool isAbove(uint64_t key) const { return (key & ~(bit() | (bit() - 1))) == prefix(); }
        Set sideOf(uint64_t key) const { return key & bit() ? one : zero; }

        uint64_t bits { 0 };
        Set zero { empty };
        Set one { empty };
    };

    static uint64_t keyOf(const FillInScan& fill)
    {
        ASSERT(fill.root <= static_cast<uint32_t>(INT32_MAX));
        return static_cast<uint64_t>(fill.root) << 33 | static_cast<uint64_t>(fill.isPart) << 32 | fill.number;
    }
    static FillInScan fillOf(uint64_t key) { return { static_cast<uint32_t>(key >> 33), !!(key >> 32 & 1), static_cast<uint32_t>(key) }; }

    Set makeNode(uint64_t bits, Set zero, Set one)
    {
        if (m_nodes.isEmpty())
            m_nodes.append(Node { });
        m_nodes.append(Node { bits, zero, one });
        return static_cast<Set>(m_nodes.size() - 1);
    }

    Set makeNodeAbove(uint64_t key, Set setOfKey, uint64_t other, Set setOfOther)
    {
        uint64_t bit = 1ull << (63 - std::countl_zero(key ^ other));
        uint64_t bits = (key & ~(bit | (bit - 1))) | bit;
        return key & bit ? makeNode(bits, setOfOther, setOfKey) : makeNode(bits, setOfKey, setOfOther);
    }

    Set withKey(Set set, uint64_t key)
    {
        if (!set)
            return makeNode(key, empty, empty);
        Node node = m_nodes[set];
        if (node.isLeaf())
            return node.bits == key ? set : makeNodeAbove(key, makeNode(key, empty, empty), node.bits, set);
        if (!node.isAbove(key))
            return makeNodeAbove(key, makeNode(key, empty, empty), node.prefix(), set);
        Set side = node.sideOf(key);
        Set sideWithKey = withKey(side, key);
        if (sideWithKey == side)
            return set;
        return key & node.bit() ? makeNode(node.bits, node.zero, sideWithKey) : makeNode(node.bits, sideWithKey, node.one);
    }

    bool containsKey(Set set, uint64_t key) const
    {
        while (set) {
            const Node& node = m_nodes[set];
            if (node.isLeaf())
                return node.bits == key;
            set = node.sideOf(key);
        }
        return false;
    }

    template<typename Functor>
    void forEachKeyBetween(Set set, uint64_t first, uint64_t last, const Functor& functor) const
    {
        if (!set)
            return;
        Node node = m_nodes[set];
        if (node.isLeaf()) {
            if (node.bits >= first && node.bits <= last)
                functor(fillOf(node.bits));
            return;
        }
        if ((node.bits | (node.bit() - 1)) < first || node.prefix() > last)
            return;
        forEachKeyBetween(node.zero, first, last, functor);
        forEachKeyBetween(node.one, first, last, functor);
    }

    Vector<Node> m_nodes;
};

enum class ObjectBeingMadeKindInScan : uint8_t { OfOneLayout, Allocated, AnsweredBySuperCall, This };

struct ObjectBeingMadeInScan {
    uint32_t root { 0 };
    ObjectBeingMadeKindInScan kind { ObjectBeingMadeKindInScan::OfOneLayout };
    bool hasLeft { false };
    bool isGivenUp { false };
    Vector<FillInScan, 8> filled;
    bool hasEnded { false };
    bool isFinished { false };
    bool endsWhereItLeaves { false };
    bool isConstant { false };
    uint32_t blockOfFinish { 0 };
    uint32_t positionOfFinish { 0 };
    uint32_t positionOfLastFill { 0 };
    uint32_t blockWhereFillsWereMetLast { UINT32_MAX };
    Vector<FillInScan, 8> filledAtEnd;
    Vector<PropertyKeyID, 8> keysLookedUp;
    Vector<FunctionID, 1> parts;
    Vector<uint32_t, 1> parametersHandedTo;
    Vector<PropertyKeyID, 4> definedInTurn;
    Vector<PropertyKeyID, 1> keysThatHoldItself;
    uint32_t copyAtAllocation { ThinSummary::noCopy };
};

struct ObjectHandedToBlockInScan {
    uint32_t block { 0 };
    uint32_t position { 0 };
    uint32_t root { 0 };
    uint32_t placeInStream { 0 };
    SetsOfFillsInScan::Set fills { SetsOfFillsInScan::empty };
};

struct BlockThatTookObjectInScan {
    uint32_t block { 0 };
    uint32_t root { 0 };
};

enum class WayOfLeavingInScan : uint8_t { IsSeenFromOutside, IsReturned, IsHandedToParent };
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
    bool laysNewValue { false };

    friend bool operator==(const ChangeOfBuiltinInScan&, const ChangeOfBuiltinInScan&) = default;
};

struct DefinitionByMapInScan {
    uint32_t change { 0 };
    uint32_t access { 0 };
    uint32_t rootOfMap { 0 };
    uint32_t position { 0 };
    ListInScan map;
};

struct DefinitionUnderComputedKeyInScan {
    uint32_t root { 0 };
    ValueID site;
    ValueID key;
    ValueID stored;
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

struct LiteralInParentInScan {
    ValueID site;
    uint32_t root { 0 };
    uint32_t position { 0 };
    uint32_t firstSinkOfParent { 0 };
    uint32_t endOfSinksOfParent { 0 };
    uint32_t firstLeftLater { UINT32_MAX };
    uint32_t lastLeftLater { 0 };
};

struct HeirMadeByBuiltinInScan {
    uint32_t call { 0 };
    uint32_t change { 0 };
    uint32_t sinkOfSubject { 0 };
    ListInScan above;
    bool isMadeEmpty { false };
    ValueID map;
    bool isFollowed { false };
};

} // anonymous namespace

static constexpr OpcodeSet opcodesOfArgumentsObjectsInScan { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument };
static constexpr OpcodeSet opcodesOfSuperCallsInScan { Opcode::SuperConstruct, Opcode::SuperConstructList };

static ALWAYS_INLINE TypeID typeOfPrimitivesInScan(uint16_t primitives, Range integers)
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

static bool stepsSynchronousIteratorInScan(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::IteratorResume)
        return value.as<Opcode::IteratorResume>().iteratorKind() == IteratorKind::Sync;
    if (opcode == Opcode::IteratorClose)
        return value.as<Opcode::IteratorClose>().iteratorKind() == IteratorKind::Sync;
    return false;
}

static constexpr std::optional<SummaryAccess::Kind> accessMadeByInScan(Opcode opcode)
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
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct RowOfOpcodeInScan {
    enum class Result : uint8_t {
        ByRule,
        Nothing,
        Constant,
        Unknown,
        Declared,
        FirstOperand,
    };

    static constexpr uint32_t isPassThrough = 1u << 0;
    static constexpr uint32_t onlyHandsOn = 1u << 1;
    static constexpr uint32_t weighsNothingInCopy = 1u << 2;
    static constexpr uint32_t namesRecords = 1u << 3;
    static constexpr uint32_t namesModules = 1u << 4;
    static constexpr uint32_t givesResult = 1u << 5;
    static constexpr uint32_t isCall = 1u << 6;
    static constexpr uint32_t isOperator = 1u << 7;
    static constexpr uint32_t hasNoEffects = 1u << 8;
    static constexpr uint32_t mayCallUnknownCode = 1u << 9;
    static constexpr uint32_t addsTrait = 1u << 10;
    static constexpr uint32_t hasKey = 1u << 11;
    static constexpr uint32_t isReadInWayOfItsOwn = 1u << 12;
    static constexpr uint32_t hasOperands = 1u << 13;
    static constexpr uint32_t holdsOperands = 1u << 14;
    static constexpr uint32_t isLookedAtBehindOperands = 1u << 15;
    static constexpr uint32_t readsHolder = 1u << 16;
    static constexpr uint32_t convertsToPropertyKey = 1u << 17;
    static constexpr uint32_t hasFormAndFewOperands = 1u << 18;

    static constexpr uint8_t noOperand = UINT8_MAX;

    uint32_t traits { 0 };
    std::optional<SummaryAccess::Kind> access;
    Result result { Result::ByRule };
    uint8_t receiver { noOperand };
    uint8_t stored { noOperand };
    uint8_t computedKey { noOperand };
};

static constexpr OpcodeSet operationsThatWeighNothingInCopyInScan { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::ToBoolean, Opcode::LogicalNot, Opcode::GetModuleVariable };
static constexpr OpcodeSet opcodesWithPureBoundInScan = opcodesWhere([](Opcode opcode) { return declaredBoundOf(opcode).isPure(); });
static constexpr OpcodeSet opcodesThatMayCallUnknownCodeInScan = opcodesWhere([](Opcode opcode) { return declaredBoundOf(opcode).mayCallUnknownCode; });
static constexpr OpcodeSet opcodesOnWhichScanLaysEffects { Opcode::Builtin, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::SuperGet, Opcode::SuperSet };
static constexpr OpcodeSet opcodesReadInWaysOfTheirOwnInScan = catchOpcodes | OpcodeSet { Opcode::NewClass, Opcode::ResolveHeritage, Opcode::NewClassToken, Opcode::DefineMethod, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::ToThis, Opcode::ToObject, Opcode::NarrowByField, Opcode::CheckInitialized, Opcode::NarrowByTest };
struct CaptureInScan {
    auto operator<=>(const CaptureInScan& other) const { return std::pair { function.index(), place } <=> std::pair { other.function.index(), other.place }; }
    bool operator==(const CaptureInScan&) const = default;

    FunctionID function;
    uint32_t place { 0 };
};

static constexpr OpcodeSet opcodesLookedAtBehindOperandsInScan { Opcode::NewObject, Opcode::Data, Opcode::GetSuperBase, Opcode::ScopeHasBinding, Opcode::NewTemplateObject, Opcode::StrictEq };
static constexpr OpcodeSet readsOfDeclaredPlacesInScan { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };

static constexpr RowOfOpcodeInScan rowOfOpcodeInScan(Opcode opcode)
{
    using Row = RowOfOpcodeInScan;
    Row row;
    auto note = [&](uint32_t trait, bool holds) {
        if (holds)
            row.traits |= trait;
    };
    bool isPassThrough = inputPassThroughOpcodes.contains(opcode);
    EffectRule effectRule = effectRuleOf(opcode);
    ResultRule resultRule = resultRuleOf(opcode);
    note(Row::isPassThrough, isPassThrough);
    note(Row::onlyHandsOn, inputIdentityOpcodes.contains(opcode));
    note(Row::weighsNothingInCopy, operationsThatWeighNothingInCopyInScan.contains(opcode));
    note(Row::namesRecords, caseOfForEachReferenceOfBodyValue(opcode) != caseOfForEachReferenceOfBodyValue(Opcode::Undefined));
    note(Row::namesModules, PayloadWord::module(opcode) || PayloadWord::referrer(opcode));
    note(Row::givesResult, resultRule != ResultRule::None);
    note(Row::isCall, effectRule == EffectRule::Call);
    note(Row::isOperator, effectRule == EffectRule::Operator);
    note(Row::hasNoEffects, effectRule == EffectRule::Declared && opcodesWithPureBoundInScan.contains(opcode) && !opcodesOnWhichScanLaysEffects.contains(opcode));
    note(Row::mayCallUnknownCode, opcodesThatMayCallUnknownCodeInScan.contains(opcode));
    note(Row::addsTrait, raisesOf(opcode) == ProgramEvent::EvaluatesText || opcodesOfArgumentsObjectsInScan.contains(opcode));
    note(Row::hasKey, PayloadWord::key(opcode).has_value());
    note(Row::isReadInWayOfItsOwn, opcodesReadInWaysOfTheirOwnInScan.contains(opcode));
    note(Row::hasOperands, caseOfForEachOperandOfBodyValue(opcode) != caseOfForEachOperandOfBodyValue(Opcode::Undefined));
    note(Row::holdsOperands, VariadicOperandPosition::held(opcode).has_value());
    note(Row::isLookedAtBehindOperands, opcodesLookedAtBehindOperandsInScan.contains(opcode) || flowRuleOf(opcode) == FlowRule::ReadOfPrototypeProperty);
    note(Row::readsHolder, interpretedOf(opcode) == Interpretation::Read && holderOf(opcode).has_value());
    note(Row::convertsToPropertyKey, flowRuleOf(opcode) == FlowRule::ToPropertyKey);
    note(Row::hasFormAndFewOperands, PayloadWord::form(opcode).has_value() && numberOfNamedOperandsOf(opcode) <= SummaryOperator::mostOperands);

    if (resultRule == ResultRule::None || resultRule == ResultRule::Results)
        row.result = Row::Result::Nothing;
    else if (resultRule == ResultRule::Constant)
        row.result = Row::Result::Constant;
    else if (resultRule == ResultRule::Unknown && !provenResultRuleOf(opcode))
        row.result = Row::Result::Unknown;
    else if (resultRule == ResultRule::Declared && readsOfDeclaredPlacesInScan.contains(opcode))
        row.result = Row::Result::Declared;
    else if (resultRule == ResultRule::Cast && isPassThrough)
        row.result = Row::Result::FirstOperand;

    row.access = accessMadeByInScan(opcode);
    if (row.access) {
        row.receiver = *receiverOf(opcode);
        std::optional<unsigned> stored = storedOf(opcode) ? std::optional<unsigned>(*storedOf(opcode)) : OperandPosition::value(opcode);
        if (stored)
            row.stored = static_cast<uint8_t>(*stored);
        if (std::optional<unsigned> property = OperandPosition::property(opcode))
            row.computedKey = static_cast<uint8_t>(*property);
    }
    return row;
}

static constexpr std::array<RowOfOpcodeInScan, numberOfEnumerators<Opcode>> rowsOfOpcodesInScan = [] {
    std::array<RowOfOpcodeInScan, numberOfEnumerators<Opcode>> rows { };
    for (Opcode opcode : allMembersOf<Opcode>)
        rows[static_cast<unsigned>(opcode)] = rowOfOpcodeInScan(opcode);
    return rows;
}();

static constexpr uint32_t traitsOfOpcodeInScan(Opcode opcode) { return rowsOfOpcodesInScan[static_cast<unsigned>(opcode)].traits; }

static_assert(!!(traitsOfOpcodeInScan(Opcode::NewClosure) & RowOfOpcodeInScan::namesRecords));
static_assert(opcodesWhere([](Opcode opcode) { return (traitsOfOpcodeInScan(opcode) & RowOfOpcodeInScan::namesModules) && !(traitsOfOpcodeInScan(opcode) & RowOfOpcodeInScan::namesRecords); }).isEmpty());

static bool resultFollowsOperandsInScan(uint32_t traits, TypeID widest)
{
    if (traits & RowOfOpcodeInScan::convertsToPropertyKey)
        return true;
    return (primitivesInID(widest) & Primitive::Number) && (traits & RowOfOpcodeInScan::hasFormAndFewOperands);
}

static constexpr Effects noEffectsInScan = Effects::none();

static constexpr Effects computeEffectsOfAccessItselfInScan(bool writesProperties)
{
    Effects effects = Effects::ofReadByNameThatRunsNoCode();
    effects.reads.remove(compiledCodeHeaps);
    effects.mayThrow = true;
    if (writesProperties)
        effects.writes = effects.reads;
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

static bool loaderEntersBodyOnceInScan(const Program& program, FunctionID id, NOESCAPE const Invocable<bool(FunctionID)> auto& isBodyOfOneModule)
{
    const Function& function = program.at(id);
    if (!function.isModuleBody() || !function.module)
        return false;
    const Module& module = program.at(function.module);
    if (module.body != id || !loaderEntersBodyAtMostOnce(module))
        return false;
    return isBodyOfOneModule(id);
}

static bool isOperandOfNothingButListsOfCallsInScan(const Body& body, ValueID list, uint64_t& numberOfValuesScanned)
{
    const UnitIDRemap* unit = &body.unit();
    auto takes = [&](BodyTarget target) {
        return std::ranges::find(target.arguments(), list) != target.arguments().end();
    };
    bool isSo = true;
    BodyBlock block = body.entry();
    for (uint32_t remaining = body.numberOfBlocks(); remaining-- && isSo;) {
        isSo = block.subject() != list && !(block.hasHandler() && takes(block.handler()));
        BodyValues::Iterator cursor = block.values().begin();
        numberOfValuesScanned += block.numberOfValues();
        for (uint32_t values = block.numberOfValues(); values--; ++cursor) {
            BodyValue value = *cursor;
            std::optional<unsigned> listPosition = OperandPosition::list(value.opcode());
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                isSo &= operand != list || (listPosition && *listPosition == operandIndex);
            });
        }
        BodyTerminator terminator(cursor.words(), unit, block.id());
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            isSo &= operand != list;
        });
        for (BodyTarget target : terminator.targets())
            isSo &= !takes(target);
        block = { terminator.end(), nullptr, unit, BlockID(block.id().index() + 1), block.endOfValues() };
    }
    return isSo;
}

static bool isBodyOfOneModuleInScan(const Program& program, FunctionID id)
{
    return std::ranges::count(program.modules, id, &Module::body) == 1;
}

static BitVector bodiesThatSeveralModulesNameInScan(const Program& program)
{
    BitVector named(program.functions.size());
    BitVector namedAgain;
    for (const Module& module : program.modules) {
        if (module.body && named.set(module.body.index()))
            namedAgain.set(module.body.index());
    }
    return namedAgain;
}

static PropertyKeyID keyOfSymbolThatCallMakesInScan(const Program& program, FunctionID function, const BodyValue& value, const ScopedLambda<BodyValue(ValueID)>& definitionOf, NOESCAPE const Invocable<bool(FunctionID)> auto& isBodyOfOneModule)
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
    if (!key || !program.types.hasStableNumber(*key) || !loaderEntersBodyOnceInScan(program, function, isBodyOfOneModule))
        return { };
    return *key;
}

PropertyKeyID keyOfSymbolThatCallMakes(const Program& program, FunctionID function, const BodyValue& value, const ScopedLambda<BodyValue(ValueID)>& definitionOf)
{
    return keyOfSymbolThatCallMakesInScan(program, function, value, definitionOf, [&](FunctionID id) {
        return isBodyOfOneModuleInScan(program, id);
    });
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
    if (module.flags.containsAny({ ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::LoaderReturnsNamespace }))
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
static NEVER_INLINE void sortSeveralWithoutRepeatsInScan(Vector<T>& list)
{
    std::sort(list.begin(), list.end());
    list.shrink(std::unique(list.begin(), list.end()) - list.begin());
}

template<typename T>
static ALWAYS_INLINE void sortWithoutRepeatsInScan(Vector<T>& list)
{
    if (list.size() > 1)
        sortSeveralWithoutRepeatsInScan(list);
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

static BitVector namesUnderWhichTextIsEvaluatedInScan(const BuiltinTable& builtins)
{
    BitVector names;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (entry.holder && entry.name && !builtins.at(entry.holder).flags.contains(BuiltinFlag::IsGlobalObject) && builtins.at(builtins.canonicalID(BuiltinID(index))).flags.contains(BuiltinFlag::EvaluatesText))
            names.set(entry.name.index());
    }
    return names;
}

class ScanOfBody {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ScanOfBody);
    WTF_MAKE_NONCOPYABLE(ScanOfBody);
public:
    ScanOfBody(const Program& program, Mechanisms mechanisms, const BitVector& textsThatNameFieldsNeverWritten, const BitVector* namesUnderWhichEventsHang = nullptr, const BitVector* bodiesThatSeveralModulesName = nullptr, const BitVector* namesUnderWhichTextIsEvaluated = nullptr)
        : m_program(program)
        , m_namesUnderWhichEventsHang(namesUnderWhichEventsHang)
        , m_namesUnderWhichTextIsEvaluated(namesUnderWhichTextIsEvaluated)
        , m_bodiesThatSeveralModulesName(bodiesThatSeveralModulesName)
        , m_textsThatNameFieldsNeverWritten(textsThatNameFieldsNeverWritten)
        , m_walkFollowsConstants(mechanisms.contains(Mechanism::Folding))
        , m_walkFollowsTests(mechanisms.contains(Mechanism::TypedOperations))
    {
        for (Opcode opcode : allMembersOf<Opcode>) {
            m_declaredEffectsOfEveryValue[static_cast<unsigned>(opcode)] = findDeclaredEffectsOfEveryValue(opcode);
            m_widestPrimitiveResults[static_cast<unsigned>(opcode)] = widestPrimitiveResultOf(opcode);
            if (inputPassThroughOpcodes.contains(opcode) && numberOfNamedOperandsOf(opcode) == 1 && !hasVariadicOperand(opcode)) {
                const OperandInfo& info = operandInfoOf(opcode, 0);
                if (info.uses == OperandUses { OperandUse::Returns } && info.protocols.isEmpty())
                    m_opcodesThatOnlyReturnTheirOperand.add(opcode);
            }
        }
    }

    void run(FunctionID, ThinSummary&);
    std::optional<InternalError> takeError() { return std::exchange(m_error, std::nullopt); }
    void noteTextSpelled(StringID text)
    {
        if (m_textsThatNameFieldsNeverWritten.get(text.index())) [[unlikely]]
            m_textsSpelled.set(text.index());
    }
    void noteTextsSpelledIn(const DataNode&);
    void noteTextThatMayNameEvent(StringID);
    void noteNameThatMayBecomeText(BuiltinName);
    void noteTextsThatMayNameEventsIn(const DataNode&);
    const BitVector& textsSpelled() const { return m_textsSpelled; }
    const SetsOfLayoutsWritten& setsOfLayoutsWritten() const { return m_setsOfLayoutsWritten; }
    uint64_t numberOfValuesScanned() const { return m_numberOfValuesScanned; }

    struct KeyOfVariableInScan {
        VariableID variable;
        PropertyKeyID key;
    };
    struct MemberUnderVariableInScan {
        FunctionID function;
        uint32_t member { 0 };
        VariableID variable;
    };
    const Vector<KeyOfVariableInScan>& keysOfVariables() const { return m_keysOfVariables; }
    const Vector<VariableID>& variablesStoredByElement() const { return m_variablesStoredByElement; }
    const Vector<std::pair<VariableID, DataID>>& literalsOfVariables() const { return m_literalsOfVariables; }
    const Vector<PropertyKeyID>& keysStoredByElement() const { return m_keysStoredByElement; }
    const Vector<std::pair<PropertyKeyID, DataID>>& literalsOfKeys() const { return m_literalsOfKeys; }
    const Vector<FieldReference>& fieldsStoredByElement() const { return m_fieldsStoredByElement; }
    const Vector<std::pair<FieldReference, DataID>>& literalsOfFields() const { return m_literalsOfFields; }
    const Vector<CaptureInScan>& capturesStoredByElement() const { return m_capturesStoredByElement; }
    const Vector<DataID>& literalsStoredByElement() const { return m_literalsStoredByElement; }
    const ThinSummaries::ElementStores& elementStores() const { return m_elementStores; }
    const Vector<std::pair<CaptureInScan, DataID>>& literalsOfCaptures() const { return m_literalsOfCaptures; }
    const Vector<MemberUnderVariableInScan>& membersUnderVariables() const { return m_membersUnderVariables; }
    const Vector<FunctionID>& functionsThatCopyMembers() const { return m_functionsThatCopyMembers; }
    const Vector<FunctionID>& copyingFunctions() const { return m_copyingFunctions; }
    const Vector<ThinSummaries::MakerOfClosures>& makersOfClosures() const { return m_makersOfClosures; }

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
    static constexpr uint32_t isJoinNotYetNeeded = UINT32_MAX - 2;
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
    static constexpr uint16_t isLaidAccessorsUnderNames = 1 << 7;
    static constexpr uint16_t isAccessedBesideNamedStoresAndAccessors = 1 << 8;
    static constexpr uint16_t isDefinedUnderComputedKeys = 1 << 10;
    static constexpr uint16_t isAccessedBesideNamedStores = isLaidAccessorsUnderNames | isAccessedBesideNamedStoresAndAccessors | isDefinedUnderComputedKeys;
    static constexpr uint16_t hasNullAboveByItsOwnText = 1 << 9;
    static constexpr uint16_t isLiteralInParent = 1 << 11;
    static constexpr uint8_t mayComeInFrontOfDefinition = 1 << 5;
    static constexpr uint8_t readsAnswerOfSuperCall = 1 << 6;

    void summarize(FunctionID);
    void reset();
    void summarizeWithoutBody();

    uint32_t nodeFor(SummaryNode::Kind, uint32_t first = 0, uint32_t second = 0);
    uint32_t addNodeAt(uint32_t place, SummaryNode);
    uint32_t addNodeAskedForOnce(SummaryNode node)
    {
        uint32_t index = static_cast<uint32_t>(m_summary.nodes.size());
        m_summary.nodes.append(node);
        m_listsOfNodes.append(ListInScan { });
        return index;
    }
    void growNodeSlots();
    uint32_t constant(TypeID type) { return nodeFor(SummaryNode::Kind::Constant, type.bits()); }
    uint32_t newJoin() { return addNodeAskedForOnce({ SummaryNode::Kind::Join, m_numberOfJoins++, 0 }); }
    uint32_t classInstanceMadeBy(const BodyValue& value, BuiltinClassID builtinClass) { return nodeFor(SummaryNode::Kind::ClassInstance, builtinClass.index(), value.id().index() + 1); }
    void addInstanceMadeBy(const BodyValue&, BuiltinClassID);
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
    void addEdge(uint32_t from, uint32_t to, TypeID passes = { }, bool runsCodeOfValue = false, bool isConversionByEquality = false);

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
    void noteSetOfLayoutsWritten(TypeID);
    template<typename OtherID> void noteFunctionsBehind(OtherID) { }

    ALWAYS_INLINE ListInScan listOfNode(uint32_t node)
    {
        ListInScan& list = m_listsOfNodes[node];
        if (!list.count) {
            list = { static_cast<uint32_t>(m_atoms.size()), 1 };
            m_atoms.append(node);
        }
        return list;
    }
    ALWAYS_INLINE ListInScan listOf(ValueID value)
    {
        static_assert(isJoinNotYetNeeded + 1 == isReadNotYetNeeded && isReadNotYetNeeded + 1 == isResultOfCall && isResultOfCall == UINT32_MAX);
        ListInScan list = m_lists[value.index()];
        if (list.count < isJoinNotYetNeeded) [[likely]]
            return list;
        return listOfValueAskedForFirst(value, list);
    }
    ListInScan listOfValueAskedForFirst(ValueID, ListInScan);
    static constexpr uint32_t noExclusion = UINT32_MAX;
    struct ExclusionInScan {
        ValueID subject;
        uint16_t excluded { 0 };
        Range integersLeft { Range::full() };
        uint32_t above { noExclusion };
        uint32_t join { 0 };
        ListInScan listUnderneath;
        bool letsEverythingThrough { false };
        bool isOutermostOverSubject { false };
        bool isOfObjectTest { false };
        uint16_t excludedInChain { 0 };
        uint32_t depth { 1 };
        uint32_t jump { noExclusion };
    };
    struct ExclusionCarriedInScan {
        uint32_t block { 0 };
        uint32_t exclusion { noExclusion };
    };
    static constexpr bool liesBehind(uint32_t exclusion, uint32_t other) { return exclusion != noExclusion && (other == noExclusion || exclusion > other); }
    void carryExclusionsOver(uint32_t to, uint32_t targetIndex, const BodyValue& test);
    uint32_t addExclusion(ValueID subject, uint16_t excluded, uint32_t above, bool isOfObjectTest = false, Range integersLeft = Range::full());
    uint32_t addExclusionOfObjectTest(const BodyValue& test, uint32_t above);
    uint32_t addExclusionsOfRelation(const BodyValue& relation, bool holds, uint32_t above);
    uint32_t meetOfExclusions(uint32_t, uint32_t);
    void giveUpExclusion(uint32_t);
    void noteViewsAfterObjectTest(BodyTarget, uint32_t position, ValueID subject);
    void takeViewsHandedTo(BodyBlock);
    ValueID valueViewedAs(ValueID);
    uint32_t depthOfExclusion(uint32_t exclusion) const { return exclusion == noExclusion ? 0 : m_exclusions[exclusion].depth; }
    uint32_t jumpOfExclusion(uint32_t exclusion) const { return exclusion == noExclusion ? noExclusion : m_exclusions[exclusion].jump; }
    uint32_t sharedEndOfExclusions(uint32_t, uint32_t) const;
    ListInScan listUnderExclusionsOf(ValueID);
    void takeExclusionsAtBlock(uint32_t block);
    void putInForce(uint32_t exclusion);
    uint32_t joinOfExclusion(uint32_t exclusion);
    void giveUpWhatEdgeLeadingBackDoesNotCarry(uint32_t to);
    void keepObjectOfReadPutOff(uint32_t read, ValueID object);
    ListInScan objectOfReadPutOff(uint32_t read, ValueID object);
    HashMap<uint32_t, ListInScan, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_listsUnderExclusions;
    HashMap<uint32_t, ListInScan, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_objectsOfReadsPutOff;
    Vector<ExclusionInScan, 4> m_exclusions;
    struct ViewHandedInScan {
        uint32_t block { 0 };
        uint32_t position { 0 };
        ValueID viewed;
    };
    struct BlockOfViewsInScan {
        uint32_t block { 0 };
        uint32_t firstExclusion { 0 };
    };
    Vector<ViewHandedInScan, 4> m_viewsHanded;
    HashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesViewed;
    Vector<BlockOfViewsInScan, 4> m_blocksWhoseViewsAreTested;
    Vector<uint32_t> m_numberOfEntriesThatCarry;
    Vector<uint32_t> m_whatEntriesShare;
    bool m_someEntryCarries { false };
    Vector<uint32_t, 4> m_exclusionsToLay;
    Vector<ExclusionCarriedInScan, 4> m_exclusionsTakenAtBlocks;
    uint32_t m_exclusionInForce { noExclusion };
    ListInScan readByName(uint32_t read);
    void noteInspection(ValueID site, ThinSummary::Inspection::Kind, ListInScan);
    uint32_t takerOfMemberBesideFields(uint32_t object, PropertyKeyID);
    ListInScan internAtoms(Vector<uint32_t>&);
    bool holdsOnlyConstants(ListInScan) const;
    bool isAlwaysPrimitive(ListInScan) const;
    bool isAlwaysPrimitive(const OriginInScan&) const;
    bool subjectOfExclusionIsPrimitive(uint32_t join, uint32_t component) const;
    void findExclusionsOfPrimitives();
    uint32_t nodeOfAtom(uint32_t atom) const;

    void beginResult();
    ALWAYS_INLINE void addList(ListInScan list)
    {
        if (!list.count || (m_numberOfSourcesOfResult && list == m_onlySourceOfResult))
            return;
        if (m_numberOfSourcesOfResult++)
            addSourceBesideOthers(list);
        m_onlySourceOfResult = list;
    }
    void addSourceBesideOthers(ListInScan);
    void addNode(uint32_t node) { addList(listOfNode(node)); }
    void addOperand(const BodyValue&, uint32_t operandIndex);
    void addVariadicOperand(const BodyValue&, uint32_t place);
    void addConstant(const BodyValue&);
    uint32_t nodeOfNativeBinding(BuiltinID);
    void addNatural(const BodyValue&, const NaturalType&);
    bool prototypeMaySelectOtherObject(const BodyValue&, LayoutID) const;
    void addAllocation(const BodyValue&, LayoutID);
    void addDeclared(const BodyValue&);
    LayoutID layoutOfNamespaceGivenBy(const BodyValue&) const;
    void addContents(const BodyValue&, uint32_t operandIndex, std::optional<ContentsSlot>);
    void addResultAt(const BodyValue&);
    void addOperator(const BodyValue&);
    PropertyKeyID fieldReadBy(BodyValue) const;
    void noteFieldsReadAsIntegers(const BodyValue&);
    ListInScan takeResult();

    void addSink(SinkKindInScan, ListInScan, uint32_t target, TypeID passes = { });
    void flowInto(ListInScan, uint32_t node);
    void escape(ListInScan list) { flowInto(list, 0); }
    void handToCodeThatRuns(ListInScan);
    ListInScan objectsAmong(ListInScan);
    void addArgument(ValueID, bool leaves = true);
    uint32_t lengthOfListWrittenRightInFrontOf(const BodyValue& call, ValueID operand) const;
    uint32_t callMember(ValueID site, ListInScan receiver, BuiltinName, std::initializer_list<ListInScan> arguments, uint32_t block);
    uint32_t addAccess(SummaryAccess::Kind, ValueID site, ListInScan subject, PropertyKeyID);
    uint32_t addAccessUnder(SummaryAccess::Kind, ValueID site, ListInScan subject, PropertyKeyID, ValueID computedKey);
    bool isSurelyIndex(ValueID) const;
    uint32_t resultOfRead(uint32_t access);
    uint32_t addRead(ValueID site, ListInScan subject, PropertyKeyID);
    uint32_t readContents(ValueID site, ListInScan holder, SlotOfContents);
    void storeIntoContents(ValueID site, ListInScan holder, SlotOfContents, ListInScan stored);
    ListInScan settle(ValueID site, ListInScan, uint32_t block);
    bool standsRightBehindCallThatGives(const BodyValue&, ValueID operand) const;
    bool isNameThatLoopOverGave(ValueID key, ValueID object) const;
    std::optional<SlotOfContents> slotNamedByRowOf(const BodyValue&) const;
    bool someResultIsReadOutOf(const BodyValue&, uint32_t operandIndex) const;
    void addChange(SummaryChange::Kind, ValueID site, ListInScan subject, PropertyKeyID, std::optional<ListInScan> other);
    void defineByDescriptor(ValueID site, ListInScan subject, PropertyKeyID, std::optional<ListInScan> other, bool isUnderNameThatNobodyElseNames = false, bool laysNewValueOfNativeCode = false);
    bool storeWhatLiteralSpells(ValueID site, ListInScan subject, ValueID source);
    VariableID constantReadAs(ValueID) const;
    uint32_t valuesLaidIntoDescriptorHeldBy(VariableID);
    struct KeyWrittenInScan {
        bool isWritten { false };
        PropertyKeyID key;
    };
    KeyWrittenInScan keyWrittenAs(ValueID) const;
    void noteCopyOfMembers(ListInScan target, ListInScan source, bool copiesAccessorsAsTheyAre);
    bool holdSameKey(ValueID, ValueID) const;
    ValueID objectReadUnder(ValueID key, ValueID read) const;
    ValueID objectDescribedBy(ValueID description, ValueID key) const;

    BodyValue definitionOf(uint32_t value) const;
    ALWAYS_INLINE bool onlyTurnsTruthRound(const BodyValue& value) const
    {
        constexpr OpcodeSet questionsOfTruth { Opcode::ToBoolean, Opcode::LogicalNot };
        static_assert((operationsThatWeighNothingInCopyInScan | questionsOfTruth) == operationsThatWeighNothingInCopyInScan);
        if (!questionsOfTruth.contains(value.opcode()))
            return false;
        BodyValue asked = definitionOf(value.operand(0).index());
        return asked && questionsOfTruth.contains(asked.opcode());
    }
    NEVER_INLINE void weighEqualityAsCaseOfSwitch(const BodyValue&);
    bool isMadeInBody(ValueID) const;
    PrimitiveKnownToFolding primitiveHeldBy(ValueID);
    BodyValue definitionBehindCasts(ValueID value) const { return definitionOf(m_roots[value.index()]); }
    DataID findArrayLiteral(ValueID) const;
    void resolveLiteralArguments();
    void noteConstantFieldTaken(ValueID);
    SummaryCall* findCallThatMade(ValueID);
    void noteResultReadAsKey(SummaryCall&);
    void noteIfParameterIsReadAsKey(ValueID key, uint16_t& parameters);
    ALWAYS_INLINE void noteLeaving(ValueID value, WayOfLeavingInScan way = WayOfLeavingInScan::IsSeenFromOutside)
    {
        uint32_t root = m_roots[value.index()];
        if (m_followsSomeEnvironment)
            noteLeavingOverEnvironments(root);
        uint32_t position = m_position;
        uint32_t& first = m_firstLeft[root];
        first = std::min(first, position);
        uint32_t& last = m_lastLeft.begin()[root];
        last = std::max(last, position);
        uint16_t& uses = m_uses.begin()[root];
        if (way == WayOfLeavingInScan::IsSeenFromOutside)
            uses |= isSeenFromOutside;
        if (uses & (isFollowedWhileMade | isLiteralInParent)) [[unlikely]]
            noteLeavingOfObjectBeingMade(root, way);
    }
    void noteLeavingOfObjectBeingMade(uint32_t root, WayOfLeavingInScan);
    std::optional<uint32_t> findCall(ValueID site) const;
    void readBody();
    BodyBlock readBlock(const BodyBlock&);
    void stopAtUndefinedRecord(uint32_t block, ValueID, Wire::SourceLocation, ASCIILiteral operation);
    void readEntryParameter(uint32_t parameter);
    void readArgumentsOf(uint32_t from, BodyTarget, KindOfEdgeInScan = KindOfEdgeInScan::Plain);
    void readOperation(const BodyValue&, uint32_t block);
    bool noteEffectsOf(const BodyValue&, Opcode, uint32_t block, std::optional<SummaryAccess::Kind>);
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
    void convertOperandOfEquality(const BodyValue&, uint32_t operandIndex, ListInScan);
    void constructBySpecies(ValueID site, ListInScan, uint32_t block);
    ListInScan openIterator(ValueID site, ListInScan iterable, IteratorKind, uint32_t block);
    uint32_t stepAsyncIterator(ValueID site, ListInScan iterator, std::optional<ListInScan> sent, uint32_t block);
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
    BuiltinID builtinExpectedOf(ValueID) const;
    BuiltinID builtinExpectedToAnswer(ValueID) const;
    bool isGatheredListThatOnlyCallsTake(ValueID);
    bool appliesGatheredArgumentsOnce(const BodyValue& call, PropertyKeyID key);
    void readChangesMadeByBuiltin(const BodyValue&, const SummaryCall&, uint32_t callIndex, ValueID receiver);
    void readDecoratorsOfMethod(const BodyValue&);
    Vector<std::pair<PropertyKeyID, ValueID>> m_gettersDefined;
    void noteEvaluationOf(ModuleID, ValueID site, uint32_t block);
    bool answersWithResultOfBody(const BodyValue&) const;
    void noteRecordOf(ModuleID);
    void noteRecordOf(VariableID);
    bool listsFirstStoresOfBody() const;
    void lookForFirstStoresAndUses(const BodyValue&);
    void noteUseByBodyOfModule(ValueID operand)
    {
        if (m_listsFirstStoresOfBody)
            noteUseOfValueByBodyOfModule(operand);
    }
    void noteUseOfValueByBodyOfModule(ValueID);
    void noteUseByBodyOfModule(FunctionID);
    FunctionID functionMadeThroughCasts(ValueID) const;
    FunctionID classConstructorThroughCasts(ValueID) const;
    VariableID variableReadThroughCasts(ValueID) const;
    WrittenUnit unitComputedInBodyOfModule(const BodyValue& definition) const;
    std::optional<int64_t> integerReadInBodyOfModule(ValueID operand) const;
    ValueID prototypeReadFunction(const BodyValue&) const;
    ValueID prototypeReadFunctionThroughCasts(ValueID) const;
    void noteStoreOnPrototype(const BodyValue&, ValueID function);
    uint32_t nearestBlockAboveBoth(uint32_t, uint32_t) const;
    void noteEdgeOfBodyOfModule(uint32_t from, uint32_t to, KindOfEdgeInScan);
    void listFirstStoresAndUses();
    bool listsDefinitions() const;
    void lookForDefinitions(const BodyValue&);
    void noteTakerOfValueMadeForVariable(ValueID taken);
    void listDefinitions();
    void noteModuleHandedOut(ModuleID module) { m_summary.modulesHandedOut.append(module); }
    const uint32_t* readTerminator(const BodyTerminator&, uint32_t block);
    void returnValue(ValueID, uint32_t block);
    void readNamesThatLoaderTakesOff(ValueID returned);
    void readWhatHostLooksFor(ValueID site);
    void layCallThatHostMakes(VariableID, const HostRead&);
    void readExportsForFacade(const BodyValue& evaluation);
    void returnFromCoroutine(ListInScan, uint32_t block);

    ObjectBeingMadeInScan& objectBeingMade(uint32_t root);
    void follow(uint32_t root, ObjectBeingMadeKindInScan);
    void followThis(const BodyBlock& entry);
    bool callsParentConstructor(const BodyValue&) const;
    void noteParentCall(const BodyValue&, SummaryCall&);
    std::optional<uint32_t> findTargetWithoutParent(const BodyValue& test) const;
    void takeResultsOfParent(const BodyBlock&);
    void takeThisAfterParent(const BodyBlock&);
    struct ResultHandedToBlock {
        uint32_t block { 0 };
        uint32_t position { 0 };
    };
    FunctionID m_parentConstructor;
    uint32_t m_receiverOfParentCalls { noDefinition };
    uint32_t m_parentTestedForNull { noDefinition };
    Vector<uint32_t, 4> m_resultsOfParent;
    Vector<ResultHandedToBlock, 4> m_resultsOfParentHanded;
    LayoutID layoutOfAllocation(const BodyValue&) const;
    void followAllocation(const BodyValue&);
    void leaveEveryFieldEmpty(LayoutID);
    void leaveFieldsEmptyButFor(LayoutID, std::span<const uint32_t> fieldsHeld);
    void noteCopyAtAllocation(const BodyValue& copy, ValueID target);
    bool deferFieldsToCopy(const ObjectBeingMadeInScan&, LayoutID);
    void listFieldsHeldBy(const ObjectBeingMadeInScan&, const Layout&, Vector<uint32_t>& fieldsHeld);
    bool isListedByAllocation(uint32_t root, PropertyKeyID) const;
    std::span<const FillInScan> fillsOf(uint32_t root, SetsOfFillsInScan::Set) LIFETIME_BOUND;
    void noteFill(uint32_t root, bool isPart, uint32_t number);
    void noteLookUp(uint32_t root, PropertyKeyID);
    void noteStoreIntoObject(uint32_t root, PropertyKeyID, SummaryAccess::Kind);
    bool readMayComeInFrontOfDefinition(uint32_t root, PropertyKeyID);
    void meetFillsWhereObjectLeaves(ObjectBeingMadeInScan&, std::span<const FillInScan> fillsOfObject);
    void meetFillsWhereConstructionEnds(uint32_t root);
    void noteWhereFinishedLiteralLeavesFirst(ObjectBeingMadeInScan&);
    bool m_blockBeingReadHasHandler { false };
    void listBirths();
    void listHalvesOfAccessorsNotLaid();
    bool m_leavesFieldsEmptyForGood { false };
    bool hasFilledInFrontOfEveryLeaving(uint32_t root, PropertyKeyID);
    void giveUp(uint32_t root);
    void handToBlock(ValueID, uint32_t from, uint32_t block, uint32_t position, KindOfEdgeInScan);
    void takeObjectsHandedTo(const BodyBlock&);
    void carryFillsOver(uint32_t from, uint32_t to);
    void listConstructions();
    void listFieldsLeftEmptyByObjectsOfOneLayout();
    void listDefinitionsBehindAllocation(const ObjectBeingMadeInScan&);

    void readWhatNobodyAskedFor();
    void findLoops();
    PropertyKeyID keySpelledBy(const BodyValue&) const;
    void nameKeysOfMembers();
    bool isStillAlone(uint32_t root, uint32_t position, uint32_t block) const;
    std::optional<uint32_t> parameterTakingIncoming(const IncomingInScan&) const;
    void sortIncomingByParameter();
    void markParametersThatSinksNeed();
    void resolveParameters();
    void resolveComponent(std::span<const uint32_t> members, uint32_t component);
    ALWAYS_INLINE OriginInScan behindTest(OriginInScan origin, TypeID passes)
    {
        if (!passes)
            return origin;
        return behindTestThatIsWritten(origin, passes);
    }
    OriginInScan behindTestThatIsWritten(OriginInScan, TypeID passes);
    void normalize(Vector<OriginInScan>&);
    void expand(ListInScan);
    ALWAYS_INLINE uint32_t oneNodeFor(ListInScan list)
    {
        if (list.count == 1) {
            uint32_t atom = m_atoms[list.first];
            if (!(atom & (parameterBit | operatorBit)))
                return atom;
            if (atom & parameterBit) {
                ListInScan resolved = m_resolved[atom & ~parameterBit];
                if (resolved.count == 1 && !m_origins[resolved.first].passes)
                    return m_origins[resolved.first].node;
            }
        }
        return oneNodeForOrigins(list);
    }
    uint32_t subjectNodeFor(ListInScan list)
    {
        uint32_t atom = list.count == 1 ? m_atoms[list.first] : 0;
        if (atom & parameterBit) {
            ListInScan resolved = m_resolved[atom & ~parameterBit];
            if (resolved.count == 1 && TypeTable::isNonNullishTop(m_origins[resolved.first].passes))
                return m_origins[resolved.first].node;
        }
        return oneNodeFor(list);
    }
    uint32_t oneNodeForOrigins(ListInScan);
    bool mayHaveFewSources(ListInScan) const;
    bool gatherSourcesOf(ListInScan, Vector<OriginInScan, 4>& sources, Vector<uint32_t, 4>& parametersGoneThrough);
    void layHeritagesBySource();
    void applySinks();
    void applyConversionToPrimitive(const SinkInScan&);
    void applyConversionByEquality(const EqualityInScan&);
    uint32_t joinObjectsConverted(ListInScan, bool isConversionByEquality);
    void callConversionsOf(uint32_t objects, ValueID site);
    void applyOperator(const OperatorInScan&);
    void applyStore(const StoreInScan&);
    void nameKeysOfDefinitionsByMaps();
    void nameGettersOfDescriptors();
    void layDescriptorsInSight();
    static constexpr uint32_t changeOfNoMap = UINT32_MAX;
    void removeWrittenAccessor(uint32_t access);
    void nameAccessorWrittenInDescriptor(uint32_t access, uint32_t rootOfDescriptor, uint32_t positionOfDefinition, uint32_t changeOfEntryInMap = changeOfNoMap, uint32_t placeWhereMapTakesEntry = 0);
    FunctionID functionWrittenAt(const SummaryNode&) const;
    std::span<const uint32_t> storesInto(uint32_t root);
    bool everyAccessorHalfIsWritten(uint32_t root) const;
    std::optional<uint32_t> findNestedLiteralSink(ValueID site) const;
    void defineAccessorsOfPrototype(uint32_t root, ListInScan prototype);
    void followObjectsMadeToBePrototypes();
    void finish();
    void noteHandlerOf(const BodyBlock&);
    uint32_t nodeCaughtBy(BlockID handler);
    void settleWhereRecordsThrow();
    void removeRepeatedEdges();
    void listNamesSpelled();
    void listWhatWalkAsksOfNodesAndCalls();
    void listSymbolsMadeOnce();
    void listStoresOnResult();

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
    const BitVector* m_namesUnderWhichTextIsEvaluated;
    const BitVector* m_bodiesThatSeveralModulesName;
    BitVector m_namesListedByThisScan;
    BitVector m_namesOfEvaluatorsListedByThisScan;
    const BitVector& m_textsThatNameFieldsNeverWritten;
    BitVector m_textsSpelled;
    const bool m_walkFollowsConstants;
    const bool m_walkFollowsTests;
    FunctionID m_id;
    const Function* m_function { nullptr };
    SummaryBeingMade m_summary;
    bool m_summaryHoldsNothing { true };
    ValueID m_writtenListBeingListed;
    uint32_t m_firstPlaceOfWrittenListBeingListed { 0 };
    std::array<const Effects*, numberOfEnumerators<Opcode>> m_declaredEffectsOfEveryValue { };
    std::array<TypeID, numberOfEnumerators<Opcode>> m_widestPrimitiveResults { };
    OpcodeSet m_opcodesThatOnlyReturnTheirOperand;
    Vector<const Effects*, 8> m_effectsMerged;
    std::array<uint64_t, 256> m_typesThatNameNoFunction { };
    std::array<uint64_t, 64> m_typesWhoseFunctionsBodyHasNamed { };
    std::array<uint64_t, 256> m_typesWhoseSetsAreNoted { };
    SetsOfLayoutsWritten m_setsOfLayoutsWritten;
    uint64_t m_numberOfValuesScanned { 0 };
    bool m_hasStopped { false };
    std::optional<bool> m_onlyCallsTakeGatheredList;
    std::optional<bool> m_argumentsObjectIsNamedOnce;
    std::optional<InternalError> m_error;

    Vector<uint32_t> m_nodeSlots;
    Vector<ListInScan> m_listsOfNodes;
    uint32_t m_numberOfJoins { 0 };
    uint32_t m_numberOfInstancesInData { 0 };

    Vector<uint32_t> m_atoms;
    Vector<ListInScan> m_lists;
    Vector<uint32_t> m_wordOffsets;
    uint32_t m_subjectOfEqualitiesWithConstants { noDefinition };
    uint32_t m_numberOfEqualitiesWithConstants { 0 };
    PropertyKeyID m_keyOfSubjectOfEqualitiesWithConstants;
    BodyValue m_lastConstantOfEqualities;
    Vector<uint32_t> m_blocks;
    Vector<uint32_t> m_roots;
    Vector<uint32_t> m_beginningsOfTextsFound;
    Vector<uint16_t> m_uses;
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
    Vector<EqualityInScan> m_equalities;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_objectsConvertedByEqualities;
    Vector<uint32_t> m_firstCallOfConversion;
    Vector<OperatorInScan> m_operators;
    Vector<PropertyKeyID, 8> m_fieldsReadAsIntegers;
    Vector<uint32_t> m_joinsOfPrimitives;
    struct JoinOfExclusion {
        uint32_t join { 0 };
        ListInScan subject;
    };
    Vector<JoinOfExclusion> m_joinsOfExclusions;
    Vector<uint32_t> m_joinsOfExclusionsOfPrimitives;

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
    struct AnswerTestedForObjectInScan {
        uint32_t blockWhereItIsObject { noDefinition };
        uint32_t position { 0 };
        ValueID call;
        ValueID nameWhereItIsObject;
    };
    AnswerTestedForObjectInScan m_answerTestedForObject;
    uint32_t m_numberOfParameters { 0 };
    Vector<IncomingInScan> m_incoming;
    Vector<SinkInScan> m_sinks;
    Vector<StoreInScan> m_stores;
    Vector<AccessorLaidInScan> m_accessorsLaid;
    struct KeyWrittenForMemberInScan {
        uint32_t member { 0 };
        ValueID key;
    };
    Vector<KeyWrittenForMemberInScan> m_keysWrittenForMembers;
    struct ValueOfVariableInScan {
        VariableID variable;
        ValueID value;
    };
    Vector<ValueOfVariableInScan> m_valuesOfImmutableVariables;
    Vector<std::pair<FieldReference, ValueID>> m_immutableFieldValues;
    Vector<std::pair<VariableID, ValueID>> m_storesIntoImmutableVariables;
    Vector<VariableID> m_moduleVariablesRead;
    std::optional<std::pair<SummaryCall, VariableID>> m_callThatHostMakes;
    Vector<std::pair<VariableID, uint32_t>, 2> m_valuesLaidIntoDescriptors;
    Vector<KeyOfVariableInScan> m_keysOfVariables;
    Vector<VariableID> m_variablesStoredByElement;
    Vector<std::pair<VariableID, DataID>> m_literalsOfVariables;
    Vector<PropertyKeyID> m_keysStoredByElement;
    Vector<std::pair<PropertyKeyID, DataID>> m_literalsOfKeys;
    Vector<FieldReference> m_fieldsStoredByElement;
    Vector<std::pair<FieldReference, DataID>> m_literalsOfFields;
    Vector<CaptureInScan> m_capturesStoredByElement;
    Vector<DataID> m_literalsStoredByElement;
    Vector<std::pair<uint32_t, DataID>> m_literalArguments;
    ThinSummaries::ElementStores m_elementStores;
    Vector<std::pair<CaptureInScan, DataID>> m_literalsOfCaptures;
    Vector<MemberUnderVariableInScan> m_membersUnderVariables;
    Vector<FunctionID> m_functionsThatCopyMembers;
    Vector<FunctionID> m_copyingFunctions;
    Vector<ThinSummaries::MakerOfClosures> m_makersOfClosures;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAccessorsLaid;
    bool hasAccessorLaidUnder(uint32_t root, PropertyKeyID key) const { return !m_accessorsLaid.isEmpty() && m_placesOfAccessorsLaid.contains(static_cast<uint64_t>(root) << 32 | key.bits()); }
    Vector<uint32_t> m_storesByRoot;
    bool m_storesAreSortedByRoot { false };
    Vector<uint32_t> m_sitesOfCopiesThatSpellStores;
    Vector<uint32_t> m_readsNotYetNeeded;
    struct HeritageInScan {
        ValueID site;
        ValueID readOffHeritage;
        ListInScan above;
        ListInScan classObject;
        uint32_t prototypeObject { 0 };
    };
    Vector<HeritageInScan, 1> m_heritages;
    Vector<std::pair<ListInScan, uint32_t>> m_returnsOfCoroutine;
    Vector<EdgeLeadingBackInScan> m_edgesLeadingBack;
    bool m_listsDefinitions { false };
    struct ValueMadeForVariableInScan {
        uint32_t made;
        uint32_t store { noDefinition };
        VariableID variable;
        bool isTakenByOthers { false };
        bool makesClosure { false };
    };
    Vector<ValueMadeForVariableInScan> m_valuesMadeForVariables;
    bool m_listsFirstStoresOfBody { false };
    uint32_t m_blockBeingRead { 0 };
    Vector<uint32_t> m_immediateDominators;
    Vector<uint32_t> m_targetsWhoseDominatorStandsAbove;
    Vector<uint32_t> m_endsOfBlocks;
    uint32_t m_blockAboveReturns { noDefinition };
    bool m_someBlockReturns { false };
    struct PlaceInBlockInScan {
        uint32_t number;
        uint32_t position;
        uint32_t block;
    };
    Vector<PlaceInBlockInScan> m_storesOfBodyOfModule;
    Vector<PlaceInBlockInScan> m_readsOfBodyOfModule;
    Vector<PlaceInBlockInScan> m_usesByBodyOfModule;
    Vector<PlaceInBlockInScan> m_usesOfVariablesByBodyOfModule;
    Vector<uint32_t> m_blocksOfStoresOnPrototypes;
    struct IntegerInVariable {
        int64_t integer { 0 };
        ValueID store;
        uint32_t blockOfStore { 0 };
    };
    HashMap<uint32_t, IntegerInVariable, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integersInVariables;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_readsNotedOfBodyOfModule;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_usesNotedOfBodyOfModule;
    Vector<uint32_t> m_blocksOfCalls;
    Vector<std::pair<ValueID, uint32_t>, 4> m_storesIntoConstantLists;
    Vector<BlockID> m_handlersOfBlocks;
    Vector<uint32_t> m_nodesCaughtByBlocks;
    Vector<bool> m_fieldIsListed;
    Vector<ChangeOfBuiltinInScan, 4> m_changesOfBuiltin;
    Vector<DefinitionByMapInScan, 4> m_definitionsByMaps;
    Vector<DefinitionUnderComputedKeyInScan, 4> m_definitionsUnderComputedKeys;
    Vector<DefinitionByDescriptorInScan, 4> m_definitionsByDescriptors;
    Vector<StoreOfPrototypeInScan, 4> m_storesOfPrototypes;
    Vector<HeirMadeByBuiltinInScan, 4> m_heirsMadeByBuiltins;
    Vector<LiteralInParentInScan, 4> m_literalsBelowParents;
    LiteralInParentInScan* findLiteralInParent(uint32_t root);
    bool isMapUsedOnce(const HeirMadeByBuiltinInScan&) const;
    void noteLiteralInParent(const BodyValue&);

    Vector<ObjectBeingMadeInScan> m_objectsBeingMade;
    uint32_t m_thisBeingMade { noDefinition };
    SetsOfFillsInScan m_setsOfFills;
    SetsOfFillsInScan::Set m_fills { SetsOfFillsInScan::empty };
    Vector<SetsOfFillsInScan::Set> m_fillsAtBlocks;
    Vector<FillInScan, 16> m_fillsOfObjectAsked;
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
    Vector<std::pair<LayoutID, uint32_t>> m_placesOfObjectsOfOneLayout;
    Vector<uint32_t> m_fieldsThatEveryObjectHolds;
    Vector<uint32_t> m_fieldsThatObjectHolds;

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
    void allocation(const BodyValue& value, BuiltinClassID builtinClass, auto&&...) { scan.addInstanceMadeBy(value, builtinClass); }
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
        if constexpr (std::is_same_v<IDType, LayoutID>) {
            if (saysWhosePrivateNameItIs)
                return;
        }
        scan.noteFunctionsBehind(id);
        if constexpr (std::is_same_v<IDType, TypeID>) {
            if (!statesExpectation)
                scan.noteSetOfLayoutsWritten(id);
        }
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            scan.noteRecordOf(scan.m_program.at(id).module);
            if (!isNewClosure)
                scan.noteUseByBodyOfModule(id);
            if (((role == FunctionReferenceKind::Closure) || ...) && !isNewClosure) {
                scan.addEdge(scan.nodeFor(SummaryNode::Kind::Closure, id.index()), 0, { }, true);
                if (!scan.m_program.at(id).captures.isEmpty())
                    scan.m_summary.sitesThatHandOverCaptures.append(site);
            }
        } else if constexpr (std::is_same_v<IDType, VariableID>)
            scan.noteRecordOf(id);
        else if constexpr (std::is_same_v<IDType, ModuleID>) {
            if (((role == ModuleReferenceKind::Target) || ...)) {
                scan.noteFunctionNamed(scan.m_program.at(id).body);
                scan.noteEvaluationOf(id, site, block);
                if (FunctionID body = scan.m_program.at(id).body; body && scan.m_program.at(body).module == scan.m_function->module)
                    scan.noteUseByBodyOfModule(body);
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
    bool statesExpectation;
    bool saysWhosePrivateNameItIs { false };
    bool namesUndefinedRecord { false };
};

static ModuleID fileWhoseModuleObjectLeavesInScan(const Program& program, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::ModuleBuiltin:
        return value.as<Opcode::ModuleBuiltin>().module();
    case Opcode::RequireDynamic:
        return value.as<Opcode::RequireDynamic>().referrer();
    case Opcode::ResolveDynamic:
        return value.as<Opcode::ResolveDynamic>().referrer();
    case Opcode::RequireModule: {
        auto require = value.as<Opcode::RequireModule>();
        return require.module() && requireMayAnswerWithMock(program, program.at(require.module())) ? require.referrer() : ModuleID();
    }
    case Opcode::EvaluateModule: {
        auto evaluation = value.as<Opcode::EvaluateModule>();
        return evaluation.module() && requireMayAnswerWithMock(program, program.at(evaluation.module())) ? evaluation.referrer() : ModuleID();
    }
    default:
        return { };
    }
}

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

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::noteFunctionsBehind(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return;
    uint64_t asKnown = static_cast<uint64_t>(type.bits()) + 1;
    uint64_t& known = m_typesThatNameNoFunction[type.bits() % m_typesThatNameNoFunction.size()];
    uint64_t asNamed = (static_cast<uint64_t>(m_id.index()) + 1) << 32 | type.bits();
    uint64_t& named = m_typesWhoseFunctionsBodyHasNamed[type.bits() % m_typesWhoseFunctionsBodyHasNamed.size()];
    if (known == asKnown || named == asNamed)
        return;
    bool namesFunction = false;
    TypeReader types(m_program);
    forEachFunctionOf(types, types.at(type), [&](FunctionID function) {
        namesFunction = true;
        noteFunctionNamed(function);
    });
    (namesFunction ? named : known) = namesFunction ? asNamed : asKnown;
}

void ScanOfBody::noteSetOfLayoutsWritten(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return;
    uint64_t asNoted = static_cast<uint64_t>(type.bits()) + 1;
    if (std::exchange(m_typesWhoseSetsAreNoted[type.bits() % m_typesWhoseSetsAreNoted.size()], asNoted) == asNoted)
        return;
    if (LayoutSetID set = m_program.at(type).unnamedLayouts; set && !holdsOneMember(set))
        m_setsOfLayoutsWritten.add(set.bits());
}

enum class FlagOfEffectsInScan : unsigned {
    FirstMaskRead,
    FirstMaskWritten = FirstMaskRead + numberOfIndexedHeaps,
    MayThrow = FirstMaskWritten + numberOfIndexedHeaps,
    MayThrowOtherThanStackOverflow,
    MayThrowByRule,
    MayCollect,
    MayCallUnknownCode,
    MaySuspend,
    MayBeTerminated,
};

static_assert(numberOfHeaps <= 32 && std::to_underlying(FlagOfEffectsInScan::MayBeTerminated) < 64);

static constexpr size_t numberOfWordsInFrontOfEffectsInScan = 1;
static constexpr size_t numberOfWordsInFrontOfMasksInScan = 2;
static constexpr size_t mostWordsOfEffectsInScan = numberOfWordsInFrontOfMasksInScan + 2 * numberOfIndexedHeaps;
using WordsOfEffectsInScan = Vector<uint64_t, numberOfWordsInFrontOfEffectsInScan + 2 * mostWordsOfEffectsInScan>;

static void appendMasksInScan(const LocationMasks& masks, FlagOfEffectsInScan firstFlag, uint64_t& flags, WordsOfEffectsInScan& words)
{
    for (AbstractHeap heap : masks.heaps() & indexedHeaps) {
        uint64_t mask = masks.maskOf(heap);
        if (mask == LocationMasks::entireHeap)
            continue;
        flags |= uint64_t { 1 } << (std::to_underlying(firstFlag) + maskIndexOf(heap));
        words.append(mask);
    }
}

static void appendEffectsInScan(const Effects& effects, WordsOfEffectsInScan& words)
{
    uint64_t flags = 0;
    auto note = [&](FlagOfEffectsInScan flag, bool holds) {
        flags |= static_cast<uint64_t>(holds) << std::to_underlying(flag);
    };
    note(FlagOfEffectsInScan::MayThrow, effects.mayThrow);
    note(FlagOfEffectsInScan::MayThrowOtherThanStackOverflow, effects.mayThrowOtherThanStackOverflow());
    note(FlagOfEffectsInScan::MayThrowByRule, effects.mayThrowByRule());
    note(FlagOfEffectsInScan::MayCollect, effects.mayCollect);
    note(FlagOfEffectsInScan::MayCallUnknownCode, effects.mayCallUnknownCode);
    note(FlagOfEffectsInScan::MaySuspend, effects.maySuspend);
    note(FlagOfEffectsInScan::MayBeTerminated, effects.mayBeTerminated);

    size_t placeOfFlags = words.size() + 1;
    words.append(static_cast<uint64_t>(effects.reads.heaps().toRaw()) | static_cast<uint64_t>(effects.writes.heaps().toRaw()) << 32);
    words.append(0);
    appendMasksInScan(effects.reads, FlagOfEffectsInScan::FirstMaskRead, flags, words);
    appendMasksInScan(effects.writes, FlagOfEffectsInScan::FirstMaskWritten, flags, words);
    words[placeOfFlags] = flags;
}

static LocationMasks takeMasksInScan(uint32_t heaps, FlagOfEffectsInScan firstFlag, uint64_t flags, std::span<const uint64_t>& words)
{
    uint64_t heapsWithMask = flags >> std::to_underlying(firstFlag);
    AbstractHeapSet all = AbstractHeapSet::fromRaw(heaps);
    LocationMasks masks(all - indexedHeaps);
    for (AbstractHeap heap : all & indexedHeaps) {
        if (!(heapsWithMask >> maskIndexOf(heap) & 1)) {
            masks.add(heap, LocationMasks::entireHeap);
            continue;
        }
        masks.add(heap, words[0]);
        words = words.subspan(1);
    }
    return masks;
}

static Effects takeEffectsInScan(std::span<const uint64_t>& words)
{
    uint64_t heaps = words[0];
    uint64_t flags = words[1];
    words = words.subspan(numberOfWordsInFrontOfMasksInScan);
    auto holds = [&](FlagOfEffectsInScan flag) {
        return !!(flags >> std::to_underlying(flag) & 1);
    };
    Effects effects = Effects::none();
    effects.reads = takeMasksInScan(static_cast<uint32_t>(heaps), FlagOfEffectsInScan::FirstMaskRead, flags, words);
    effects.writes = takeMasksInScan(static_cast<uint32_t>(heaps >> 32), FlagOfEffectsInScan::FirstMaskWritten, flags, words);
    if (holds(FlagOfEffectsInScan::MayThrowByRule))
        effects.mayThrow = ThrowEffect(true);
    else if (holds(FlagOfEffectsInScan::MayThrowOtherThanStackOverflow))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    else if (holds(FlagOfEffectsInScan::MayThrow))
        effects.mayThrow = ThrowEffect::onlyStackOverflow();
    effects.mayCollect = holds(FlagOfEffectsInScan::MayCollect);
    effects.mayCallUnknownCode = holds(FlagOfEffectsInScan::MayCallUnknownCode);
    effects.maySuspend = holds(FlagOfEffectsInScan::MaySuspend);
    effects.mayBeTerminated = holds(FlagOfEffectsInScan::MayBeTerminated);
    return effects;
}

bool ThinSummary::ownEffectsMayCallUnknownCode() const
{
    std::span<const uint64_t> words = wordsOfEffects.span();
    return words.empty() || (words[numberOfWordsInFrontOfEffectsInScan + 1] >> std::to_underlying(FlagOfEffectsInScan::MayCallUnknownCode) & 1);
}

Effects ThinSummary::ownEffects() const
{
    std::span<const uint64_t> words = wordsOfEffects.span();
    if (words.empty())
        return { };
    words = words.subspan(numberOfWordsInFrontOfEffectsInScan);
    return takeEffectsInScan(words);
}

std::optional<Effects> ThinSummary::ownEffectsOfOperators() const
{
    std::span<const uint64_t> words = wordsOfEffects.span();
    if (words.empty())
        return std::nullopt;
    words = words.subspan(numberOfWordsInFrontOfEffectsInScan);
    constexpr uint64_t flagsOfMasks = (uint64_t { 1 } << std::to_underlying(FlagOfEffectsInScan::MayThrow)) - 1;
    words = words.subspan(numberOfWordsInFrontOfMasksInScan + std::popcount(words[1] & flagsOfMasks));
    if (words.empty())
        return std::nullopt;
    return takeEffectsInScan(words);
}

ThinSummary::ThinSummary(SummaryBeingMade& made)
    : SummaryWithoutLists(made)
{
    using Entry = SummaryStorage::Entry;

    WordsOfEffectsInScan effects;
    effects.append(made.layoutsOfOwnWrites);
    appendEffectsInScan(made.ownEffects, effects);
    if (made.ownEffectsOfOperators)
        appendEffectsInScan(*made.ownEffectsOfOperators, effects);

    auto forEachList = [&](const auto& visit) {
        visit(nodes, made.nodes);
        visit(edges, made.edges);
        visit(calls, made.calls);
        visit(arguments, made.arguments);
        visit(accesses, made.accesses);
        visit(blocksNeverEntered, made.blocksNeverEntered);
        visit(scopesStoredFirst, made.scopesStoredFirst);
        visit(makersThatEndWhereTheyLeave, made.makersThatEndWhereTheyLeave);
        visit(allocationsOfMakersThatEndWhereTheyLeave, made.allocationsOfMakersThatEndWhereTheyLeave);
        visit(constructions, made.constructions);
        visit(keysOfConstructions, made.keysOfConstructions);
        visit(keysFilledAtEnds, made.keysFilledAtEnds);
        visit(otherNamesOfObjectsBeingMade, made.otherNamesOfObjectsBeingMade);
        visit(tailCallThatIsOnlyCall, made.tailCallThatIsOnlyCall);
        visit(symbolsMadeOnce, made.symbolsMadeOnce);
        visit(callThatGivesResult, made.callThatGivesResult);
        visit(storesOnResult, made.storesOnResult);
        visit(constantListsStoredIntoInLoops, made.constantListsStoredIntoInLoops);
        visit(writtenListsOfVariables, made.writtenListsOfVariables);
        visit(keysOfTestedFields, made.keysOfTestedFields);
        visit(operators, made.operators);
        visit(changes, made.changes);
        visit(members, made.members);
        visit(functionVariablesStoredInto, made.functionVariablesStoredInto);
        visit(partsOfConstructions, made.partsOfConstructions);
        visit(fieldsHeldOfLayoutsLeftEmpty, made.fieldsHeldOfLayoutsLeftEmpty);
        visit(fieldsFilledAtEndAlone, made.fieldsFilledAtEndAlone);
        visit(fieldsEmptyAtEndAlone, made.fieldsEmptyAtEndAlone);
        visit(halvesOfAccessorsNotLaid, made.halvesOfAccessorsNotLaid);
        visit(fieldsListedByAllocations, made.fieldsListedByAllocations);
        visit(classesEvaluated, made.classesEvaluated);
        visit(environmentsMade, made.environmentsMade);
        visit(sitesThatHandOverCaptures, made.sitesThatHandOverCaptures);
        visit(definitions, made.definitions);
        visit(functionsNamed, made.functionsNamed);
        visit(handlers, made.handlers);
        visit(modulesNamedInBlocksNeverEntered, made.modulesNamedInBlocksNeverEntered);
        visit(modulesHandedOut, made.modulesHandedOut);
        visit(firstStoresOfBody, made.firstStoresOfBody);
        visit(firstUsesOfFunctions, made.firstUsesOfFunctions);
        visit(otherModulesNamed, made.otherModulesNamed);
        visit(answersOfRequire, made.answersOfRequire);
        visit(objectsOfCompiledModules, made.objectsOfCompiledModules);
        visit(namesThatLoaderTakes, made.namesThatLoaderTakes);
        visit(inspections, made.inspections);
        visit(namesRead, made.namesRead);
        visit(namesCalled, made.namesCalled);
        visit(namesStored, made.namesStored);
        visit(textsThatNameEvents, made.textsThatNameEvents);
        visit(accessesUnderConstantKeys, made.accessesUnderConstantKeys);
        visit(modulesWhosePathIsRead, made.modulesWhosePathIsRead);
        visit(ownWritesOnWaysToReturn, made.ownWritesOnWaysToReturn);
        visit(operandsOfOperatorsLeftApart, made.operandsOfOperatorsLeftApart);
        visit(wordsOfEffects, effects);
        visit(namesThatHostLooksFor, made.namesThatHostLooksFor);
        visit(copiesOfMembers, made.copiesOfMembers);
        visit(placesOfWrittenLists, made.placesOfWrittenLists);
        visit(definitionsByDescriptorInOrderOfSites, made.definitionsByDescriptorInOrderOfSites);
        visit(callsAtSitesOfCopiesThatSpellStores, made.callsAtSitesOfCopiesThatSpellStores);
        visit(writtenAccessors, made.writtenAccessors);
        visit(methodsOfClasses, made.methodsOfClasses);
        visit(classesInVariables, made.classesInVariables);
        visit(unitsInVariables, made.unitsInVariables);
        visit(storesOnPrototypes, made.storesOnPrototypes);
        visit(firstUsesOfVariables, made.firstUsesOfVariables);
        visit(nodesHandedToEvals, made.nodesHandedToEvals);
    };

    SummaryStorage& storage = nodes.m_storage;
    std::array<size_t, SummaryStorage::numberOfListsInRecord> offsetsOfListsInRecord;
    std::array<Entry, 64> entries;
    size_t size = 0;
    size_t sizeKeptForWalk = 0;
    uint64_t numbersInUse = 0;
    forEachList([&]<typename T, unsigned number>(const SummaryList<T, number>&, const auto& list) {
        static_assert(alignof(T) <= alignof(uint64_t));
        if constexpr (number >= SummaryStorage::numberOfListsInRecord) {
            RELEASE_ASSERT(!(numbersInUse & SummaryStorage::bitOf<number>()));
            numbersInUse |= SummaryStorage::bitOf<number>();
        }
        if constexpr (number < SummaryStorage::numberOfListsInRecord) {
            static_assert(std::is_trivially_destructible_v<T>);
            size = WTF::roundUpToMultipleOf<alignof(T)>(size);
            offsetsOfListsInRecord[number] = size;
            storage.sizesOfListsInRecord[number] = static_cast<uint32_t>(list.size());
            size += list.size() * sizeof(T);
        } else if (!list.isEmpty()) {
            constexpr uint64_t bit = SummaryStorage::bitOf<number>();
            constexpr bool isKeptForWalk = !!(bit & SummaryStorage::listsKeptForWalk);
            static_assert(std::is_trivially_destructible_v<T> || (isKeptForWalk && std::is_same_v<T, SummaryScopeStoredFirst>));
            size_t& end = isKeptForWalk ? sizeKeptForWalk : size;
            size_t offset = WTF::roundUpToMultipleOf<alignof(T)>(end);
            end = offset + list.size() * sizeof(T);
            ASSERT(!(storage.listsInDirectories & bit));
            storage.listsInDirectories |= bit;
            entries[number - SummaryStorage::numberOfListsInRecord] = { static_cast<uint32_t>(offset), static_cast<uint32_t>(list.size()) };
        }
    });

    uint64_t keptForWalk = storage.listsInDirectories & SummaryStorage::listsKeptForWalk;
    uint64_t notKeptForWalk = storage.listsInDirectories & ~SummaryStorage::listsKeptForWalk;
    size_t sizeOfDirectoryKeptForWalk = std::popcount(keptForWalk) * sizeof(Entry);
    size_t offsetOfDirectory = WTF::roundUpToMultipleOf<alignof(Entry)>(size);
    size = offsetOfDirectory + std::popcount(notKeptForWalk) * sizeof(Entry);
    RELEASE_ASSERT(size <= UINT32_MAX && sizeOfDirectoryKeptForWalk + sizeKeptForWalk <= UINT32_MAX);

    std::byte* block = static_cast<std::byte*>(fastMalloc(size));
    ASSERT(!offsetsOfListsInRecord[0]);
    for (unsigned number = 0; number < SummaryStorage::numberOfListsInRecord; ++number)
        storage.listsInRecord[number] = block + offsetsOfListsInRecord[number];
    storage.offsetOfDirectory = static_cast<uint32_t>(offsetOfDirectory);
    Entry* entry = SummaryStorage::pointerInto<Entry>(block, offsetOfDirectory);
    for (uint64_t rest = notKeptForWalk; rest; rest &= rest - 1)
        *entry++ = entries[std::countr_zero(rest)];

    if (keptForWalk) {
        storage.blockKeptForWalk = fastMalloc(sizeOfDirectoryKeptForWalk + sizeKeptForWalk);
        entry = static_cast<Entry*>(storage.blockKeptForWalk);
        for (uint64_t rest = keptForWalk; rest; rest &= rest - 1) {
            Entry behindDirectory = entries[std::countr_zero(rest)];
            *entry++ = { static_cast<uint32_t>(sizeOfDirectoryKeptForWalk + behindDirectory.offset), behindDirectory.size };
        }
    }

    forEachList([&]<typename T, unsigned number>(const SummaryList<T, number>&, auto& list) {
        if (list.isEmpty())
            return;
        std::span<T> room = storage.list<T, number>();
        if constexpr (std::is_trivially_copyable_v<T>)
            memcpySpan(room, list.span());
        else
            std::ranges::uninitialized_move(list.mutableSpan(), room);
        list.shrink(0);
    });
    ASSERT(layoutsOfOwnWrites() == made.layoutsOfOwnWrites && ownEffects() == made.ownEffects && ownEffectsOfOperators() == made.ownEffectsOfOperators);

    static_cast<SummaryWithoutLists&>(made) = { };
    made.layoutsOfOwnWrites = 0;
    made.ownEffects = { };
    made.ownEffectsOfOperators = std::nullopt;
    made.makesCopies = false;
}

void ScanOfBody::run(FunctionID id, ThinSummary& result)
{
    summarize(id);
    if (m_summary.makesCopies)
        m_copyingFunctions.append(id);
    if (!m_summary.copiesOfMembers.isEmpty())
        m_functionsThatCopyMembers.append(id);
    result = ThinSummary(m_summary);
    m_summaryHoldsNothing = true;
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
            if (!m_readsNotYetNeeded.isEmpty() || !m_returnsOfCoroutine.isEmpty())
                readWhatNobodyAskedFor();
            if (!m_edgesLeadingBack.isEmpty())
                findLoops();
            if (!m_literalArguments.isEmpty())
                resolveLiteralArguments();
            nameKeysOfMembers();
            if (!m_storesOfPrototypes.isEmpty() || !m_heirsMadeByBuiltins.isEmpty())
                followObjectsMadeToBePrototypes();
            resolveParameters();
            findExclusionsOfPrimitives();
            if (!m_definitionsByDescriptors.isEmpty())
                layDescriptorsInSight();
            applySinks();
            if (!m_definitionsByDescriptors.isEmpty())
                nameGettersOfDescriptors();
            if (!m_definitionsByMaps.isEmpty())
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
    if (!std::exchange(m_summaryHoldsNothing, false)) {
        std::destroy_at(&m_summary);
        std::construct_at(&m_summary);
    }
    m_summary.nodes.append(SummaryNode { });
    m_writtenListBeingListed = { };
    m_hasStopped = false;
    m_onlyCallsTakeGatheredList = std::nullopt;
    m_argumentsObjectIsNamedOnce = std::nullopt;
    m_nodeSlots.fill(0, std::max(initialNumberOfNodeSlots, std::bit_ceil(2 * (m_function->body.numberOfValues() + 1))));
    m_listsOfNodes.shrink(0);
    m_listsOfNodes.append(ListInScan { });
    m_effectsMerged.shrink(0);
    m_numberOfJoins = 0;
    m_numberOfInstancesInData = 0;
    m_atoms.shrink(0);
    m_origins.shrink(0);
    m_incoming.shrink(0);
    m_sinks.shrink(0);
    m_stores.shrink(0);
    m_keysWrittenForMembers.shrink(0);
    m_valuesOfImmutableVariables.shrink(0);
    m_immutableFieldValues.shrink(0);
    m_storesIntoImmutableVariables.shrink(0);
    m_moduleVariablesRead.shrink(0);
    m_callThatHostMakes = std::nullopt;
    m_valuesLaidIntoDescriptors.shrink(0);
    if (!m_accessorsLaid.isEmpty()) {
        m_accessorsLaid.shrink(0);
        m_placesOfAccessorsLaid.clear();
    }
    m_storesAreSortedByRoot = false;
    m_sitesOfCopiesThatSpellStores.shrink(0);
    m_definitionsByMaps.shrink(0);
    m_definitionsUnderComputedKeys.shrink(0);
    m_definitionsByDescriptors.shrink(0);
    m_storesOfPrototypes.shrink(0);
    m_heirsMadeByBuiltins.shrink(0);
    m_parentConstructor = { };
    m_receiverOfParentCalls = noDefinition;
    m_parentTestedForNull = noDefinition;
    m_resultsOfParent.shrink(0);
    m_resultsOfParentHanded.shrink(0);
    m_literalsBelowParents.shrink(0);
    m_readsNotYetNeeded.shrink(0);
    m_heritages.shrink(0);
    m_returnsOfCoroutine.shrink(0);
    m_edgesLeadingBack.shrink(0);
    m_listsDefinitions = listsDefinitions();
    m_valuesMadeForVariables.shrink(0);
    m_listsFirstStoresOfBody = listsFirstStoresOfBody();
    m_blockAboveReturns = noDefinition;
    m_someBlockReturns = false;
    m_storesOfBodyOfModule.shrink(0);
    m_readsOfBodyOfModule.shrink(0);
    m_usesByBodyOfModule.shrink(0);
    m_usesOfVariablesByBodyOfModule.shrink(0);
    m_blocksOfStoresOnPrototypes.shrink(0);
    m_integersInVariables.clear();
    m_readsNotedOfBodyOfModule.clear();
    m_usesNotedOfBodyOfModule.clear();
    m_blocksOfCalls.shrink(0);
    m_storesIntoConstantLists.shrink(0);
    m_handlersOfBlocks.shrink(0);
    m_nodesCaughtByBlocks.shrink(0);
    m_innermostLoopHeads.shrink(0);
    m_isConvertedToPrimitive.shrink(0);
    m_equalities.shrink(0);
    m_objectsConvertedByEqualities.clear();
    m_operators.shrink(0);
    m_fieldsReadAsIntegers.shrink(0);
    m_joinsOfPrimitives.shrink(0);
    m_joinsOfExclusions.shrink(0);
    m_joinsOfExclusionsOfPrimitives.shrink(0);
    m_objectsBeingMade.shrink(0);
    m_thisBeingMade = noDefinition;
    m_leavesFieldsEmptyForGood = false;
    m_setsOfFills.clear();
    m_fills = SetsOfFillsInScan::empty;
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
    while ((m_summary.nodes.size() + 1) * 2 > size)
        size *= 2;
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
    SummaryNode node { kind, first, second };
    const uint32_t* slots = m_nodeSlots.begin();
    const SummaryNode* nodes = m_summary.nodes.begin();
    uint32_t mask = static_cast<uint32_t>(m_nodeSlots.size()) - 1;
    for (uint32_t place = hashOfNodeInScan(node) & mask;; place = (place + 1) & mask) {
        uint32_t slot = slots[place];
        if (!slot)
            return addNodeAt(place, node);
        if (nodes[slot - 1] == node)
            return slot - 1;
    }
}

NEVER_INLINE uint32_t ScanOfBody::addNodeAt(uint32_t place, SummaryNode node)
{
    if ((m_summary.nodes.size() + 1) * 2 > m_nodeSlots.size()) {
        growNodeSlots();
        uint32_t mask = static_cast<uint32_t>(m_nodeSlots.size()) - 1;
        place = hashOfNodeInScan(node) & mask;
        while (m_nodeSlots[place])
            place = (place + 1) & mask;
    }
    uint32_t index = addNodeAskedForOnce(node);
    m_nodeSlots[place] = index + 1;
    return index;
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

void ScanOfBody::addEdge(uint32_t from, uint32_t to, TypeID passes, bool runsCodeOfValue, bool isConversionByEquality)
{
    if (from == to || (!to && (m_summary.nodes[from].kind == SummaryNode::Kind::Constant || m_summary.nodes[from].kind == SummaryNode::Kind::ResultOfOperator)))
        return;
    m_summary.edges.append(SummaryEdge { from, to, passes, runsCodeOfValue, isConversionByEquality });
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
            if (std::ranges::any_of(node->as<DataNode::Array>().elements, [](const DataNode& element) { return element.is<DataNode::Hole>(); }))
                addEdge(constant(TypeTable::undefined), contentsNode(made, SlotOfContents::mayHaveHoles()));
            for (const DataNode& element : node->as<DataNode::Array>().elements)
                nodesToVisit.append(DataAndTaker { &element, contentsNode(made, ContentsSlot::Element) });
            break;
        case DataNode::Kind::Object: {
            const auto& object = node->as<DataNode::Object>();
            made = object.layout ? nodeFor(SummaryNode::Kind::Allocation, object.layout.index()) : constant(TypeTable::object);
            m_fieldsThatObjectHolds.shrink(0);
            for (const DataMember& member : object.members) {
                std::optional<uint32_t> field = object.layout ? m_program.at(object.layout).findField(member.key) : std::nullopt;
                if (field)
                    m_fieldsThatObjectHolds.append(*field);
                uint32_t takerOfMember = field ? fieldNode(object.layout, *field) : object.layout ? takerOfMemberBesideFields(made, member.key) : 0u;
                nodesToVisit.append(DataAndTaker { &member.value, takerOfMember });
                if (!field && !object.layout)
                    addEdge(made, 0);
            }
            if (object.layout && taker != noSummaryNode) {
                sortWithoutRepeatsInScan(m_fieldsThatObjectHolds);
                m_leavesFieldsEmptyForGood |= m_fieldsThatObjectHolds.size() < m_program.at(object.layout).fields.size();
                leaveFieldsEmptyButFor(object.layout, m_fieldsThatObjectHolds.span());
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

ListInScan ScanOfBody::listOfValueAskedForFirst(ValueID value, ListInScan list)
{
    if (list.count == isResultOfCall)
        list = listOfNode(nodeFor(SummaryNode::Kind::ResultOfCall, list.first, 0));
    else if (list.count == isJoinNotYetNeeded)
        list = listOfNode(joinOfExclusion(list.first));
    else
        list = readByName(list.first);
    m_lists[value.index()] = list;
    return list;
}

ListInScan ScanOfBody::readByName(uint32_t read)
{
    if (m_lists[read].count != isReadNotYetNeeded) {
        if (!m_listsUnderExclusions.isEmpty()) [[unlikely]] {
            if (auto underneath = m_listsUnderExclusions.find(read); underneath != m_listsUnderExclusions.end())
                return underneath->value;
        }
        return m_lists[read];
    }
    if (definitionOf(read).opcode() == Opcode::ResolveHeritage) {
        ValueID heritage = definitionOf(read).operand(0);
        if (heritage)
            noteLeaving(heritage);
        Vector<uint32_t> atoms { addRead(ValueID(read), heritage ? objectOfReadPutOff(read, heritage) : ListInScan(), m_program.keyOf(BuiltinNames::prototype)), constant(TypeTable::null) };
        m_lists[read] = internAtoms(atoms);
        return m_lists[read];
    }
    auto definition = definitionOf(read).as<Opcode::PropGet>();
    ValueID object = definition.objectOperand();
    uint32_t access = addAccessUnder(SummaryAccess::Kind::Read, ValueID(read), object ? objectOfReadPutOff(read, object) : ListInScan(), definition.key(), ValueID());
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
    return m_summary.nodes[origin.node].kind == SummaryNode::Kind::ResultOfOperator || std::ranges::binary_search(m_joinsOfPrimitives, origin.node) || std::ranges::binary_search(m_joinsOfExclusionsOfPrimitives, origin.node);
}

bool ScanOfBody::subjectOfExclusionIsPrimitive(uint32_t join, uint32_t component) const
{
    auto found = std::ranges::lower_bound(m_joinsOfExclusions, join, { }, &JoinOfExclusion::join);
    if (found == m_joinsOfExclusions.end() || found->join != join)
        return false;
    return std::ranges::all_of(m_atoms.span().subspan(found->subject.first, found->subject.count), [&](uint32_t atom) {
        if (atom & operatorBit)
            return true;
        if (atom & parameterBit)
            return component != noComponent && m_componentOf[atom & ~parameterBit] == component;
        return isAlwaysPrimitive(OriginInScan { atom, { } }) || subjectOfExclusionIsPrimitive(atom, component);
    });
}

void ScanOfBody::findExclusionsOfPrimitives()
{
    for (const JoinOfExclusion& exclusion : m_joinsOfExclusions) {
        expand(exclusion.subject);
        bool holdsPrimitivesAlone = std::ranges::all_of(m_expanded, [&](const OriginInScan& origin) {
            return isAlwaysPrimitive(origin);
        });
        if (holdsPrimitivesAlone)
            m_joinsOfExclusionsOfPrimitives.append(exclusion.join);
    }
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

void ScanOfBody::addSourceBesideOthers(ListInScan list)
{
    if (m_numberOfSourcesOfResult == 2)
        m_result.append(m_atoms.span().subspan(m_onlySourceOfResult.first, m_onlySourceOfResult.count));
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
        if (data)
            noteTextsThatMayNameEventsIn(m_program.at(data).root);
        addNode(data ? nodeOfData(m_program.at(data).root) : 0);
        return;
    }
    if (opcode == Opcode::Builtin) {
        auto named = value.as<Opcode::Builtin>();
        const BuiltinTable& builtins = m_program.builtins();
        bool isObjectItself = named.builtin() && (named.isNotReadFromHolder() || namesWhatNobodyReplacesInScan(builtins, named.builtin()));
        ModuleID compiled = isObjectItself ? compiledModuleWithObject(m_program, builtins.canonicalID(named.builtin())) : ModuleID();
        if (!compiled || !objectOfLibraryFileIsResultOfBody(m_program, compiled))
            addNode(isObjectItself ? nodeFor(SummaryNode::Kind::Builtin, builtins.canonicalID(named.builtin()).index()) : 0);
        if (compiled) {
            uint32_t answer = newJoin();
            m_summary.objectsOfCompiledModules.append({ compiled, answer });
            addNode(answer);
        }
        return;
    }
    if (opcode == Opcode::NativeBinding) {
        addNode(nodeOfNativeBinding(value.as<Opcode::NativeBinding>().builtin()));
        return;
    }
    if (opcode == Opcode::String)
        noteTextThatMayNameEvent(value.as<Opcode::String>().string());
    TypeID type = typeOfPrimitiveConstantInScan(value);
    addNode(type ? constant(type) : 0);
}

uint32_t ScanOfBody::nodeOfNativeBinding(BuiltinID binding)
{
    BuiltinID function = findNativeFunction(m_program.builtins(), binding);
    return function ? nodeFor(SummaryNode::Kind::Builtin, function.index()) : 0;
}

void ScanOfBody::noteTextThatMayNameEvent(StringID text)
{
    BuiltinName name = m_program.strings.builtinName(text);
    if (!name || isLibraryCode(m_program, *m_function))
        return;
    if (!m_namesUnderWhichEventsHang) {
        m_namesListedByThisScan = namesUnderWhichEventsHangInScan(m_program.builtins());
        m_namesUnderWhichEventsHang = &m_namesListedByThisScan;
    }
    if (m_namesUnderWhichEventsHang->get(name.index()))
        m_summary.textsThatNameEvents.append(m_program.keyOf(name));
}

void ScanOfBody::noteNameThatMayBecomeText(BuiltinName name)
{
    if (!name || isLibraryCode(m_program, *m_function))
        return;
    if (!m_namesUnderWhichTextIsEvaluated) {
        m_namesOfEvaluatorsListedByThisScan = namesUnderWhichTextIsEvaluatedInScan(m_program.builtins());
        m_namesUnderWhichTextIsEvaluated = &m_namesOfEvaluatorsListedByThisScan;
    }
    if (m_namesUnderWhichTextIsEvaluated->get(name.index()))
        m_summary.textsThatNameEvents.append(m_program.keyOf(name));
}

void ScanOfBody::noteTextsThatMayNameEventsIn(const DataNode& root)
{
    Vector<const DataNode*, 32> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const DataNode& node = *nodesToVisit.takeLast();
        if (const auto* string = node.tryAs<DataNode::String>())
            noteTextThatMayNameEvent(string->value);
        else if (const auto* array = node.tryAs<DataNode::Array>()) {
            for (const DataNode& element : array->elements)
                nodesToVisit.append(&element);
        } else if (const auto* object = node.tryAs<DataNode::Object>()) {
            for (const DataMember& member : object->members) {
                noteNameThatMayBecomeText(m_program.builtinNameOf(member.key));
                nodesToVisit.append(&member.value);
            }
        }
    }
}

static bool fillsEveryElement(const BodyValue& list)
{
    return list.opcode() == Opcode::NewArray ? list.as<Opcode::NewArray>().holes().empty() : list.opcode() == Opcode::CreateRest;
}

void ScanOfBody::addInstanceMadeBy(const BodyValue& value, BuiltinClassID builtinClass)
{
    uint32_t made = classInstanceMadeBy(value, builtinClass);
    addNode(made);
    if (builtinClass != BuiltinClasses::Array)
        return;
    if (!fillsEveryElement(value))
        addEdge(constant(TypeTable::undefined), contentsNode(made, SlotOfContents::mayHaveHoles()));
    if (value.opcode() != Opcode::NewArray && value.opcode() != Opcode::CreateRest)
        addEdge(constant(TypeTable::undefined), contentsNode(made, SlotOfContents::mayHaveNamesOfEngine()));
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
        addInstanceMadeBy(value, written.builtinClass);
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

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::addDeclared(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::GetModuleVariable) {
        addNode(variableNode(value.as<Opcode::GetModuleVariable>().variable()));
        if (m_function->isModuleBody())
            m_moduleVariablesRead.append(value.as<Opcode::GetModuleVariable>().variable());
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
    if (LayoutID whole = m_program.layoutOfWholeNamespace(named.module()))
        return whole;
    return m_program.layoutOfNamespaceThatListsNoName(named.module());
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
    ListInScan stepValue = listUnderExclusionsOf(producer);
    if (flowRuleOf(opcode) == FlowRule::Iteration && stepValue.count && ((unknownResultsOf(opcode) >> std::min(read.index(), 31u)) & 1)) {
        addList(stepValue);
        return;
    }
    ResultRulesOfScan rules { *this };
    applyResultRule(rules, definition, read.index(), ResultSide::Proven);
}

PropertyKeyID ScanOfBody::fieldReadBy(BodyValue definition) const
{
    if (definition && definition.opcode() == Opcode::ToNumeric)
        definition = definitionBehindCasts(definition.operand(0));
    if (!definition)
        return { };
    if (definition.opcode() == Opcode::PrivateGet)
        return fieldAt(m_program, FieldReference { definition.layout(), definition.field() }).key;
    return definition.opcode() == Opcode::PropGet && !definition.isFreeName() ? definition.key() : PropertyKeyID();
}

void ScanOfBody::noteFieldsReadAsIntegers(const BodyValue& value)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    auto note = [&](ValueID operand) {
        BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
        if (definition && sums.contains(definition.opcode()))
            definition = definitionBehindCasts(definition.operand(0));
        if (PropertyKeyID key = fieldReadBy(definition); key && !m_fieldsReadAsIntegers.contains(key))
            m_fieldsReadAsIntegers.append(key);
    };
    if (value.opcode() == Opcode::ElemGet || value.opcode() == Opcode::ElemSet) {
        note(value.propertyOperand());
        return;
    }
    note(value.operand(0));
    note(value.operand(1));
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
    if (found.opcode == Opcode::Mod) {
        BodyValue divisor = definitionBehindCasts(value.operand(1));
        if (int64_t written = divisor && divisor.opcode() == Opcode::Integer ? divisor.as<Opcode::Integer>().integer() : 0; written >= 1 && written <= int64_t { 1 } << 32)
            found.writtenDivisor = written <= INT32_MAX ? SummaryOperator::WrittenDivisor::FromOneBelowTwoTo31 : SummaryOperator::WrittenDivisor::FromTwoTo31ToTwoTo32;
    }
    if (found.opcode == Opcode::URShift) {
        BodyValue shift = definitionBehindCasts(value.operand(1));
        found.shiftsRightByConstant = shift && shift.opcode() == Opcode::Integer && shift.as<Opcode::Integer>().integer() & 31;
    }
    auto isLocal = [&](uint32_t index) {
        BodyValue definition = definitionBehindCasts(value.operand(index));
        if (!definition)
            return !m_function->isCoroutine();
        found.steppedField = fieldReadBy(definition);
        return !!found.steppedField;
    };
    if ((found.opcode == Opcode::Increment || found.opcode == Opcode::Decrement) && isLocal(0))
        found.step = found.opcode == Opcode::Increment ? 1 : -1;
    if (found.opcode == Opcode::Add || found.opcode == Opcode::Sub) {
        auto written = [&](uint32_t index) -> int8_t {
            BodyValue operand = definitionBehindCasts(value.operand(index));
            int64_t number = operand && operand.opcode() == Opcode::Integer ? operand.as<Opcode::Integer>().integer() : 0;
            return number == 1 || number == -1 ? static_cast<int8_t>(number) : 0;
        };
        if (isLocal(0))
            found.step = found.opcode == Opcode::Add ? written(1) : static_cast<int8_t>(-written(1));
        else if (found.opcode == Opcode::Add && isLocal(1)) {
            found.step = written(0);
            found.steppedOperand = 1;
        }
    }

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

uint32_t ScanOfBody::lengthOfListWrittenRightInFrontOf(const BodyValue& call, ValueID operand) const
{
    constexpr size_t mostElements = (1 << 13) - 1;
    BodyValue list = definitionBehindCasts(operand);
    if (!list || m_blocks[list.id().index()] != m_blocks[call.id().index()])
        return 0;
    size_t length = 0;
    if (list.opcode() == Opcode::NewArray)
        length = list.as<Opcode::NewArray>().elementsOperands().size();
    else if (DataID data = list.opcode() == Opcode::Data ? list.as<Opcode::Data>().data() : DataID(); data && m_program.at(data).root.kind() == DataNode::Kind::Array)
        length = m_program.at(data).root.as<DataNode::Array>().elements.size();
    if (!length || length > mostElements)
        return 0;
    for (uint32_t between = list.id().index() + 1; between < call.id().index(); ++between) {
        BodyValue operation = definitionOf(between);
        if (operation && !opcodesThatStateTypes.contains(operation.opcode()))
            return 0;
    }
    return static_cast<uint32_t>(length);
}

void ScanOfBody::addArgument(ValueID operand, bool leaves)
{
    uint32_t place = static_cast<uint32_t>(m_summary.arguments.size());
    m_summary.arguments.append(0);
    if (!operand)
        return;
    if (DataID literal = findArrayLiteral(operand))
        m_literalArguments.append({ place, literal });
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
    if (computedKey) {
        m_summary.accesses[access].keyIsSurelyIndex = isSurelyIndex(computedKey);
        BodyValue written = kind == SummaryAccess::Kind::Read ? definitionBehindCasts(computedKey) : BodyValue();
        if (written && written.opcode() == Opcode::Integer && written.as<Opcode::Integer>().integer() >= 0 && written.as<Opcode::Integer>().integer() < noSummaryNode)
            m_summary.accesses[access].writtenIndexOfRead = static_cast<uint32_t>(written.as<Opcode::Integer>().integer());
        addSink(SinkKindInScan::KeyOfAccess, listOf(computedKey), access);
    }
    return access;
}

bool ScanOfBody::isSurelyIndex(ValueID value) const
{
    constexpr int64_t numberOfIndexes = 1ll << 31;
    auto integerWrittenAt = [&](ValueID operand) -> std::optional<int64_t> {
        BodyValue written = operand ? definitionBehindCasts(operand) : BodyValue();
        return written && written.opcode() == Opcode::Integer ? std::optional(written.as<Opcode::Integer>().integer()) : std::nullopt;
    };
    while (value) {
        BodyValue definition = definitionBehindCasts(value);
        if (!definition)
            return false;
        switch (definition.opcode()) {
        case Opcode::Integer:
            return definition.as<Opcode::Integer>().integer() >= 0 && definition.as<Opcode::Integer>().integer() < numberOfIndexes;
        case Opcode::BitAnd:
            return std::ranges::any_of(std::array { definition.operand(0), definition.operand(1) }, [&](ValueID operand) {
                std::optional<int64_t> mask = integerWrittenAt(operand);
                return mask && *mask >= 0 && *mask <= INT32_MAX;
            });
        case Opcode::URShift: {
            std::optional<int64_t> distance = integerWrittenAt(definition.operand(1));
            return distance && (*distance & 31);
        }
        case Opcode::Mod: {
            std::optional<int64_t> divisor = integerWrittenAt(definition.operand(1));
            if (!divisor || *divisor <= 0 || *divisor > numberOfIndexes)
                return false;
            value = definition.operand(0);
            continue;
        }
        default:
            return false;
        }
    }
    return false;
}

uint32_t ScanOfBody::resultOfRead(uint32_t access)
{
    uint32_t node = addNodeAskedForOnce({ SummaryNode::Kind::ResultOfAccess, access, 0 });
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
    m_summary.accesses[access].emptyHolderOrKeyPrefix = whatIsNoPromise;
    uint32_t valueOfPromise = resultOfRead(access);

    ListInScan objects = objectsAmong(listOfNode(whatIsNoPromise));
    uint32_t found = newJoin();
    for (BuiltinName name : m_program.builtins().keysReadBy(Protocol::Thenable)) {
        uint32_t read = addAccessUnder(SummaryAccess::Kind::Read, site, objects, m_program.keyOf(name), ValueID());
        m_summary.accesses[read].answerIsOnlyCalled = true;
        addEdge(resultOfRead(read), found);
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

VariableID ScanOfBody::constantReadAs(ValueID value) const
{
    BodyValue definition = definitionBehindCasts(value);
    if (!definition || definition.opcode() != Opcode::GetModuleVariable)
        return { };
    VariableID variable = definition.as<Opcode::GetModuleVariable>().variable();
    return variable && m_program.at(variable).flags.contains(VariableFlags::IsImmutable) ? variable : VariableID();
}

uint32_t ScanOfBody::valuesLaidIntoDescriptorHeldBy(VariableID constant)
{
    for (auto [variable, node] : m_valuesLaidIntoDescriptors) {
        if (variable == constant)
            return node;
    }
    m_valuesLaidIntoDescriptors.append({ constant, newJoin() });
    return m_valuesLaidIntoDescriptors.last().second;
}

void ScanOfBody::defineByDescriptor(ValueID site, ListInScan subject, PropertyKeyID key, std::optional<ListInScan> other, bool isUnderNameThatNobodyElseNames, bool laysNewValueOfNativeCode)
{
    m_summary.definitionsByDescriptorInOrderOfSites.append(static_cast<uint32_t>(m_summary.changes.size()));
    addChange(SummaryChange::Kind::DefinesByDescriptor, site, subject, key, other);
    m_summary.changes.last().laysNewValueOfNativeCode = laysNewValueOfNativeCode;
    if (isUnderNameThatNobodyElseNames) {
        m_summary.changes.last().isUnderNameThatNobodyElseNames = true;
        return;
    }
    uint32_t access = addAccess(SummaryAccess::Kind::Define, site, subject, key);
    m_summary.accesses[access].isByDescriptor = true;
    m_summary.accesses[access].holdsNewValueOfNativeCode = laysNewValueOfNativeCode;
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
    if (definition && definition.opcode() == Opcode::Builtin) {
        PropertyKeyID symbol = keyOfWellKnownSymbol(m_program, m_program.builtins().canonicalID(definition.as<Opcode::Builtin>().builtin()));
        return { !!symbol, symbol };
    }
    if (definition && definition.opcode() == Opcode::Call) {
        PropertyKeyID symbol = keySpelledBy(definition);
        return { !!symbol, symbol };
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

bool ScanOfBody::isMadeInBody(ValueID value) const
{
    BodyValue made = value ? definitionOf(m_roots[value.index()]) : BodyValue();
    return made && flowOf(made.opcode()) == ValueFlowEdgeClass::Allocates;
}

bool ScanOfBody::storeWhatLiteralSpells(ValueID site, ListInScan subject, ValueID source)
{
    uint32_t root = m_roots[source.index()];
    BodyValue made = definitionOf(root);
    if (!made || made.opcode() != Opcode::NewObject || made.as<Opcode::NewObject>().prototypeOperand() || m_blocks[root] != m_blocks[site.index()])
        return false;
    const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
    if (layout.kind != LayoutKind::Record || m_firstLeft[root] < site.index() || (m_uses[root] & isAccessedBesideNamedStores))
        return false;

    Vector<std::pair<PropertyKeyID, ListInScan>, 16> spelled;
    std::span<const uint32_t> fields = made.as<Opcode::NewObject>().fields();
    std::span<const ValueID> values = made.variadicOperands();
    for (size_t place = 0; place < fields.size(); ++place) {
        if (fields[place] >= layout.fields.size() || place >= values.size() || !values[place] || !layout.fields[fields[place]].key || !layout.fields[fields[place]].form.isEmpty())
            return false;
        spelled.append({ layout.fields[fields[place]].key, listOf(values[place]) });
    }
    size_t firstStoreBehindRoot = m_stores.size();
    while (firstStoreBehindRoot && m_stores[firstStoreBehindRoot - 1].site.index() > root)
        --firstStoreBehindRoot;
    for (size_t index = firstStoreBehindRoot; index < m_stores.size(); ++index) {
        const StoreInScan& store = m_stores[index];
        if (store.root != root)
            continue;
        if (!store.isDefinition || store.block != m_blocks[root])
            return false;
        spelled.append({ store.key, store.value });
    }
    for (const auto& [key, value] : spelled) {
        if (m_program.nameClassOf(key) == NameClass::Proto)
            return false;
    }
    if (!spelled.isEmpty())
        m_sitesOfCopiesThatSpellStores.append(site.index());
    for (const auto& [key, value] : spelled) {
        uint32_t access = addAccess(SummaryAccess::Kind::Store, site, subject, key);
        m_summary.accesses[access].isSpelledByCopy = true;
        addSink(SinkKindInScan::ValueOfAccess, value, access);
    }
    return true;
}

void ScanOfBody::noteCopyOfMembers(ListInScan target, ListInScan source, bool copiesAccessorsAsTheyAre)
{
    if (!target.count || !source.count || isAlwaysPrimitive(target) || isAlwaysPrimitive(source))
        return;
    uint32_t index = static_cast<uint32_t>(m_summary.copiesOfMembers.size());
    m_summary.copiesOfMembers.append(SummaryCopyOfMembers { 0, 0, copiesAccessorsAsTheyAre });
    addSink(SinkKindInScan::TargetOfCopyOfMembers, target, index);
    addSink(SinkKindInScan::SourceOfCopyOfMembers, source, index);
}

bool ScanOfBody::holdSameKey(ValueID first, ValueID second) const
{
    auto behindConversions = [&](ValueID key) {
        uint32_t root = m_roots[key.index()];
        for (BodyValue definition = definitionOf(root); definition && (traitsOfOpcodeInScan(definition.opcode()) & RowOfOpcodeInScan::convertsToPropertyKey) && definition.operand(0); definition = definitionOf(root))
            root = m_roots[definition.operand(0).index()];
        return root;
    };
    uint32_t rootOfFirst = behindConversions(first);
    uint32_t rootOfSecond = behindConversions(second);
    if (rootOfFirst == rootOfSecond)
        return true;
    BodyValue readOfFirst = definitionOf(rootOfFirst);
    BodyValue readOfSecond = definitionOf(rootOfSecond);
    if (!readOfFirst || !readOfSecond || readOfFirst.opcode() != readOfSecond.opcode())
        return false;
    if (readOfFirst.opcode() == Opcode::GetModuleVariable)
        return readOfFirst.as<Opcode::GetModuleVariable>().variable() == readOfSecond.as<Opcode::GetModuleVariable>().variable();
    const RowOfOpcodeInScan& row = rowsOfOpcodesInScan[static_cast<unsigned>(readOfFirst.opcode())];
    if (row.access != SummaryAccess::Kind::Read || row.computedKey == RowOfOpcodeInScan::noOperand)
        return false;
    ValueID holderOfFirst = readOfFirst.operand(row.receiver);
    ValueID holderOfSecond = readOfSecond.operand(row.receiver);
    ValueID placeOfFirst = readOfFirst.operand(row.computedKey);
    ValueID placeOfSecond = readOfSecond.operand(row.computedKey);
    return holderOfFirst && holderOfSecond && placeOfFirst && placeOfSecond && m_roots[holderOfFirst.index()] == m_roots[holderOfSecond.index()] && holdSameKey(placeOfFirst, placeOfSecond);
}

ValueID ScanOfBody::objectReadUnder(ValueID key, ValueID read) const
{
    BodyValue definition = definitionBehindCasts(read);
    if (!definition)
        return { };
    const RowOfOpcodeInScan& row = rowsOfOpcodesInScan[static_cast<unsigned>(definition.opcode())];
    if (row.access != SummaryAccess::Kind::Read || row.computedKey == RowOfOpcodeInScan::noOperand)
        return { };
    ValueID keyOfRead = definition.operand(row.computedKey);
    return keyOfRead && holdSameKey(keyOfRead, key) ? definition.operand(row.receiver) : ValueID();
}

ValueID ScanOfBody::objectDescribedBy(ValueID description, ValueID key) const
{
    BodyValue made = definitionBehindCasts(description);
    if (!made || made.opcode() != Opcode::Call)
        return { };
    const BuiltinTable& builtins = m_program.builtins();
    std::span<const ValueID> arguments = made.variadicOperands();
    auto argumentAt = [&](std::optional<BuiltinOperand> operand) {
        return operand && operand->kind() == BuiltinOperand::Kind::Argument && operand->argumentIndex() < arguments.size() ? arguments[operand->argumentIndex()] : ValueID();
    };
    for (auto record = std::ranges::lower_bound(m_summary.calls, made.id(), { }, &SummaryCall::site); record != m_summary.calls.end() && record->site == made.id(); ++record) {
        if (record->callee != SummaryCall::Callee::Builtin || record->isConstruction || record->passesList)
            continue;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(record->builtin))) {
            if (overload.mode != BuiltinCallMode::Call)
                continue;
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                ValueID described = argumentAt(action.object);
                if (!described)
                    continue;
                if (!key) {
                    if (action.kind == BuiltinPropertyActionKind::DescribesOwnProperties)
                        return described;
                    continue;
                }
                if (action.kind != BuiltinPropertyActionKind::ReadsGetter || action.value.kind != BuiltinPlaceKind::ResultField || action.key.kind != BuiltinKeyKind::Operand)
                    continue;
                if (ValueID keyDescribed = argumentAt(action.key.operand); keyDescribed && holdSameKey(keyDescribed, key))
                    return described;
            }
        }
    }
    return { };
}

void ScanOfBody::weighEqualityAsCaseOfSwitch(const BodyValue& equality)
{
    static constexpr OpcodeSet constantsOfCases { Opcode::Integer, Opcode::String };
    uint32_t roots[] { m_roots[equality.operand(0).index()], m_roots[equality.operand(1).index()] };
    BodyValue definitions[] { definitionOf(roots[0]), definitionOf(roots[1]) };
    auto isConstantOfCase = [&](const BodyValue& definition) {
        return definition && constantsOfCases.contains(definition.opcode());
    };
    bool leftIsConstant = isConstantOfCase(definitions[0]);
    if (leftIsConstant == isConstantOfCase(definitions[1])) {
        m_subjectOfEqualitiesWithConstants = noDefinition;
        return;
    }
    uint32_t subject = roots[leftIsConstant];
    PropertyKeyID key;
    if (BodyValue read = definitions[leftIsConstant]; read && read.opcode() == Opcode::PropGet && !read.as<Opcode::PropGet>().isFreeName()) {
        key = read.as<Opcode::PropGet>().key();
        subject = m_roots[read.as<Opcode::PropGet>().objectOperand().index()];
    }
    BodyValue constant = definitions[!leftIsConstant];
    BodyValue before = std::exchange(m_lastConstantOfEqualities, constant);
    bool goesOn = subject == m_subjectOfEqualitiesWithConstants && key == m_keyOfSubjectOfEqualitiesWithConstants;
    if (goesOn) {
        goesOn = before.opcode() == constant.opcode();
        if (goesOn && (constant.opcode() == Opcode::Integer ? before.as<Opcode::Integer>().integer() == constant.as<Opcode::Integer>().integer() : before.as<Opcode::String>().string() == constant.as<Opcode::String>().string()))
            return;
    }
    if (!goesOn) {
        m_subjectOfEqualitiesWithConstants = subject;
        m_keyOfSubjectOfEqualitiesWithConstants = key;
        m_numberOfEqualitiesWithConstants = 1;
        return;
    }
    uint32_t numberBefore = m_numberOfEqualitiesWithConstants++;
    auto weightOf = [](uint32_t number) { return number < 3 ? number : number / 2; };
    m_summary.weightThatSwitchesSave += 1 + weightOf(numberBefore) - weightOf(numberBefore + 1) + !!key;
}

BodyValue ScanOfBody::definitionOf(uint32_t value) const
{
    uint32_t offset = m_wordOffsets[value];
    return offset == noDefinition ? BodyValue() : m_function->body.operationAt(offset, ValueID(value));
}

PrimitiveKnownToFolding ScanOfBody::primitiveHeldBy(ValueID value)
{
    ValueID unvisited;
    auto findPrimitive = [&](ValueID operand) -> PrimitiveKnownToFolding {
        BodyValue definition = definitionOf(operand.index());
        if (!definition || !operationsThatConstantsMayDecide.contains(definition.opcode()))
            return { };
        if (m_primitivesAsked[operand.index()].stamp == m_stampOfBody)
            return m_primitivesAsked[operand.index()].known;
        unvisited = operand;
        return { };
    };
    PrimitiveKnownToFolding known = findPrimitive(value);
    if (!unvisited)
        return known;

    Vector<ValueID, 8> pending { unvisited };
    while (!pending.isEmpty()) {
        ValueID current = pending.last();
        unvisited = { };
        known = primitiveMadeOfConstants(m_program, definitionOf(current.index()), scopedLambda<PrimitiveKnownToFolding(ValueID)>([&](ValueID operand) {
            return findPrimitive(operand);
        }));
        if (unvisited) {
            pending.append(unvisited);
            continue;
        }
        m_primitivesAsked[current.index()] = { m_stampOfBody, known };
        pending.removeLast();
    }
    return known;
}

LiteralInParentInScan* ScanOfBody::findLiteralInParent(uint32_t root)
{
    for (LiteralInParentInScan& literal : m_literalsBelowParents) {
        if (literal.root == root)
            return &literal;
    }
    return nullptr;
}

void ScanOfBody::noteLiteralInParent(const BodyValue& operation)
{
    uint32_t root = m_roots[operation.as<Opcode::SetLiteralPrototype>().objectOperand().index()];
    BodyValue made = definitionOf(root);
    if (!made || made.opcode() != Opcode::NewDictionary || m_firstLeft[root] != m_position || m_lastLeft[root] != m_position || (m_uses[root] & isLiteralInParent))
        return;
    m_uses[root] |= isLiteralInParent;
    LiteralInParentInScan literal;
    literal.site = operation.id();
    literal.root = root;
    literal.position = m_position;
    m_literalsBelowParents.append(literal);
}

void ScanOfBody::noteLeavingOfObjectBeingMade(uint32_t root, WayOfLeavingInScan way)
{
    if (m_uses[root] & isLiteralInParent) {
        LiteralInParentInScan& literal = *findLiteralInParent(root);
        literal.firstLeftLater = std::min(literal.firstLeftLater, m_position);
        literal.lastLeftLater = std::max(literal.lastLeftLater, m_position);
        if (!(m_uses[root] & isFollowedWhileMade))
            return;
    }
    if (way == WayOfLeavingInScan::IsHandedToParent)
        return;
    noteWhereFinishedLiteralLeavesFirst(objectBeingMade(root));
    if (std::exchange(objectBeingMade(root).blockWhereFillsWereMetLast, m_blockBeingRead) != m_blockBeingRead)
        meetFillsWhereObjectLeaves(objectBeingMade(root), fillsOf(root, m_fills));
    if (way == WayOfLeavingInScan::IsReturned)
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
    m_subjectOfEqualitiesWithConstants = noDefinition;
    m_numberOfEqualitiesWithConstants = 0;
    m_keyOfSubjectOfEqualitiesWithConstants = { };
    m_lastConstantOfEqualities = { };
    m_blocks.fill(0, body.numberOfValues());
    m_roots.fill(0, body.numberOfValues());
    m_beginningsOfTextsFound.shrink(0);
    m_uses.fill(0, body.numberOfValues());
    m_firstLeft.fill(hasNotLeft, body.numberOfValues());
    m_lastLeft.fill(0, body.numberOfValues());
    m_firstParameterOfBlock.fill(0, body.numberOfBlocks() + 1);
    m_answerTestedForObject = { };
    m_fillsAtBlocks.fill(noFillsYet, body.numberOfBlocks());
    m_numberOfEdgesInto.fill(0, body.numberOfBlocks());
    m_exclusions.shrink(0);
    m_someEntryCarries = false;
    m_exclusionsTakenAtBlocks.shrink(0);
    m_exclusionInForce = noExclusion;
    m_listsUnderExclusions.clear();
    m_viewsHanded.shrink(0);
    m_valuesViewed.clear();
    m_blocksWhoseViewsAreTested.shrink(0);
    m_objectsOfReadsPutOff.clear();
    if (m_listsFirstStoresOfBody) {
        m_immediateDominators.fill(noDefinition, body.numberOfBlocks());
        m_targetsWhoseDominatorStandsAbove.fill(noDefinition, body.numberOfBlocks());
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
    putInForce(noExclusion);
    m_firstParameterOfBlock[body.numberOfBlocks()] = m_numberOfParameters;
}

ALWAYS_INLINE BodyBlock ScanOfBody::readBlock(const BodyBlock& block)
{
    uint32_t number = block.id().index();
    m_numberOfValuesScanned += block.numberOfValues();
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
        if (TypeID type = block.parameterType(position))
            noteSetOfLayoutsWritten(type);
        if (block.id() == entryBlock)
            readEntryParameter(position);
    }

    if (m_answerTestedForObject.blockWhereItIsObject == number && m_answerTestedForObject.position < block.numberOfParameters())
        m_answerTestedForObject.nameWhereItIsObject = block.parameter(m_answerTestedForObject.position);

    m_position = block.firstValue().index();
    m_blockBeingRead = number;
    if (!m_viewsHanded.isEmpty()) [[unlikely]]
        takeViewsHandedTo(block);
    takeExclusionsAtBlock(number);
    m_blockBeingReadHasHandler = block.hasHandler();
    if (m_followsSomeEnvironment)
        enterBlockForEnvironments(number);
    if (m_listsFirstStoresOfBody)
        m_endsOfBlocks[number] = block.endOfValues().index();
    if (block.id() == entryBlock)
        followThis(block);
    else if (!m_objectsBeingMade.isEmpty()) {
        m_fills = m_fillsAtBlocks[number] != noFillsYet ? m_fillsAtBlocks[number] : SetsOfFillsInScan::empty;
        if (!m_resultsOfParentHanded.isEmpty()) {
            takeResultsOfParent(block);
            takeThisAfterParent(block);
        }
        takeObjectsHandedTo(block);
    }
    if (block.hasHandler()) {
        readArgumentsOf(number, block.handler(), KindOfEdgeInScan::ToHandler);
        carryExclusionsOver(block.handler().block().index(), 0, BodyValue());
        noteHandlerOf(block);
    }

    BodyValues::Iterator cursor = block.values().begin();
    for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
        readOperation(*cursor, number);
        if (m_hasStopped)
            return { };
    }
    m_position = block.endOfValues().index();
    endOperationsOverEnvironments();
    BodyTerminator terminator(cursor.words(), unit, block.id());
    return { readTerminator(terminator, number), nullptr, unit, BlockID(number + 1), block.endOfValues() };
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

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::readArgumentsOf(uint32_t from, BodyTarget target, KindOfEdgeInScan kind)
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
    ++m_numberOfEdgesInto[to];
    if (!m_objectsBeingMade.isEmpty())
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

static FieldReference instanceFieldRead(const Program& program, const BodyValue& value)
{
    if (value.opcode() != Opcode::GetEnvironmentField || program.moduleInstances.isEmpty())
        return { };
    auto read = value.as<Opcode::GetEnvironmentField>();
    return { read.layout(), read.field() };
}

ALWAYS_INLINE bool ScanOfBody::noteEffectsOf(const BodyValue& value, Opcode opcode, uint32_t block, std::optional<SummaryAccess::Kind> access)
{
    Effects effectsStorage;
    const Effects* declaredEffects = opcode != Opcode::IsNotFrozen ? m_declaredEffectsOfEveryValue[static_cast<unsigned>(opcode)] : nullptr;
    if (!declaredEffects)
        declaredEffects = &declaredEffectsOfOperation(m_program, opcode, value, effectsStorage);
    static constexpr Effects effectsOfNamespaceRead = Effects::ofReadByNameThatRunsNoCode();
    if (opcode == Opcode::ModuleNamespace && namespaceReadRunsNoCode(m_program, value))
        declaredEffects = &effectsOfNamespaceRead;
    if (opcode == Opcode::Builtin && standsForFirstReadOffHolder(m_program.builtins(), value.as<Opcode::Builtin>().builtin(), value.as<Opcode::Builtin>().isNotReadFromHolder())) {
        if (declaredEffects != &effectsStorage)
            effectsStorage = *declaredEffects;
        effectsStorage.merge(declaredBoundInTable(opcodeOfFirstReadOffHolder(m_program.builtins(), value.as<Opcode::Builtin>().builtin())));
        declaredEffects = &effectsStorage;
    }
    if (access && declaredEffects->mayCallUnknownCode)
        declaredEffects = &effectsOfAccessItselfInScan(*access);
    else if (stepsSynchronousIteratorInScan(value) || opcode == Opcode::SuperGet)
        declaredEffects = &effectsOfAccessItselfInScan(SummaryAccess::Kind::Read);
    else if (opcode == Opcode::SuperSet)
        declaredEffects = &effectsOfAccessItselfInScan(SummaryAccess::Kind::Store);
    const Effects& effects = noteOwnEffects(value, block, access, *declaredEffects, effectsStorage);
    if (!effects.isPure()) {
        bool isRowOfTable = &effects != &effectsStorage;
        if (!isRowOfTable || !m_effectsMerged.contains(&effects)) {
            m_summary.ownEffects.merge(effects);
            if (isRowOfTable)
                m_effectsMerged.append(&effects);
        }
    }
    if (effects.maySuspend)
        m_summary.traits.add(SummaryTrait::Suspends);
    return effects.mayCallUnknownCode;
}

void ScanOfBody::noteConstantFieldTaken(ValueID computedKey)
{
    BodyValue read = definitionBehindCasts(computedKey);
    if (read && read.opcode() == Opcode::ToPropertyKey)
        read = definitionBehindCasts(read.operand(0));
    if (!read || read.opcode() != Opcode::PropGet || read.isFreeName())
        return;
    BodyValue holder = definitionBehindCasts(read.objectOperand());
    if (holder && holder.opcode() == Opcode::CheckInitialized)
        holder = definitionBehindCasts(holder.operand(0));
    if (!holder || holder.opcode() != Opcode::GetModuleVariable || !m_program.at(holder.variable()).flags.contains(VariableFlags::IsImmutable))
        return;
    if (!m_summary.keysOfTestedFields.contains(read.key()))
        m_summary.keysOfTestedFields.append(read.key());
}

ALWAYS_INLINE void ScanOfBody::readOperation(const BodyValue& value, uint32_t block)
{
    using Row = RowOfOpcodeInScan;
    Opcode opcode = value.opcode();
    const Row& row = rowsOfOpcodesInScan[static_cast<unsigned>(opcode)];
    uint32_t traits = row.traits;
    uint32_t index = value.id().index();
    m_position = index;
    m_blocks[index] = block;
    m_wordOffsets.begin()[index] = static_cast<uint32_t>(value.words() - m_function->body.words().data());
    ValueID passedThroughOperand = traits & Row::isPassThrough ? value.operand(0) : ValueID();
    uint32_t root = passedThroughOperand ? m_roots[passedThroughOperand.index()] : index;
    m_roots.begin()[index] = root;
    if (passedThroughOperand && opcode == Opcode::FinishObject && (m_uses[root] & isFollowedWhileMade)) {
        meetFillsWhereConstructionEnds(root);
        ObjectBeingMadeInScan& finished = objectBeingMade(root);
        finished.isFinished = !finished.hasLeft;
        finished.blockOfFinish = block;
        finished.positionOfFinish = index;
    }
    if (m_followsSomeEnvironment)
        beginOperationOverEnvironments(value);
    if (!(traits & Row::onlyHandsOn)) {
        FieldReference fieldOfInstance = instanceFieldRead(m_program, value);
        bool readsLink = fieldOfInstance.layout && m_program.isInstanceLink(fieldOfInstance);
        m_summary.numberOfOperations += !readsLink && !readsTokenOfClassInScan(m_program, value);
        if (traits & Row::weighsNothingInCopy)
            m_summary.numberOfOperationsThatOnlyTurnTruthRound += onlyTurnsTruthRound(value);
        else
            m_summary.weightOfOperations += !readsLink && !(fieldOfInstance.layout && m_program.isInstanceVariable(fieldOfInstance));
    }
    if (opcode == Opcode::String)
        noteTextSpelled(value.as<Opcode::String>().string());
    static constexpr OpcodeSet usesOfIntegers = allRelations | OpcodeSet { Opcode::ElemGet, Opcode::ElemSet };
    if (usesOfIntegers.contains(opcode))
        noteFieldsReadAsIntegers(value);
    if (opcode == Opcode::Convert && value.as<Opcode::Convert>().declared().has(PositionFlags::IsInt32Promise))
        m_summary.traits.add(SummaryTrait::HoldsInt32Promise);
    if (m_listsFirstStoresOfBody)
        lookForFirstStoresAndUses(value);
    if (m_listsDefinitions)
        lookForDefinitions(value);
    if (opcode == Opcode::PutModuleVariable && value.as<Opcode::PutModuleVariable>().variable() == m_program.at(m_function->module).exportsVariable)
        readWhatHostLooksFor(value.id());

    if (traits & Row::namesRecords) {
        bool isFollowedFromHere = (opcode == Opcode::ModuleNamespace && layoutOfNamespaceGivenBy(value)) || (opcode == Opcode::RequireModule && answersWithResultOfBody(value));
        bool handsModuleOut = (traits & Row::givesResult) && !isFollowedFromHere;
        bool statesExpectation = opcode == Opcode::Check || (opcode == Opcode::Convert && value.as<Opcode::Convert>().declared().isExpected());
        ReferencesOfScan references { *this, value.id(), block, opcode == Opcode::NewClosure, handsModuleOut, statesExpectation, familyOf(opcode) == OperationFamily::PrivateAccess };
        value.forEachReference(references);
        if (traits & Row::namesModules) {
            notePathsOfModulesReadInScan(value, m_summary);
            if (opcode == Opcode::EvaluateModule && !references.namesUndefinedRecord)
                readExportsForFacade(value);
            if (ModuleID file = fileWhoseModuleObjectLeavesInScan(m_program, value); file && m_program.at(file).kind == ModuleKind::CommonJs)
                noteModuleHandedOut(file);
        }
        if (opcode == Opcode::NewClosure && !references.namesUndefinedRecord) {
            FunctionID made = value.as<Opcode::NewClosure>().function();
            if (StringID name = m_program.at(made).name)
                noteNameThatMayBecomeText(m_program.strings.builtinName(name));
            if (LayoutID constructed = classOfConstructorFunction(m_program, made))
                m_summary.classesEvaluated.append(constructed);
            if (!m_program.at(made).captures.isEmpty())
                m_summary.sitesThatHandOverCaptures.append(value.id());
        }
        if (references.namesUndefinedRecord) {
            stopAtUndefinedRecord(block, value.id(), m_function->locationOf(value), nameOf(opcode));
            return;
        }
    }

    if (traits & Row::isCall) {
        readCall(value, block);
        if (opcodesOfSuperCallsInScan.contains(opcode) && m_function->kind == FunctionKind::DerivedConstructor) {
            m_uses[index] |= isUnderConstruction;
            follow(index, ObjectBeingMadeKindInScan::AnsweredBySuperCall);
        }
        return;
    }

    std::optional<SummaryAccess::Kind> access = row.access;
    m_keyOfOperation = traits & Row::hasKey ? value.key() : PropertyKeyID();
    bool mayCallUnknownCode = !(traits & Row::hasNoEffects) && noteEffectsOf(value, opcode, block, access);
    if (traits & Row::addsTrait) {
        if (raisesOf(opcode) == ProgramEvent::EvaluatesText)
            m_summary.traits.add(SummaryTrait::HasDirectEval);
        if (opcodesOfArgumentsObjectsInScan.contains(opcode))
            m_summary.traits.add(SummaryTrait::UsesArguments);
    }

    beginResult();
    if (access) {
        readAccess(value, *access, block);
        return;
    }
    if (traits & Row::isReadInWayOfItsOwn) {
        if (opcode == Opcode::NewClass) {
            readNewClass(value);
            return;
        }
        if (opcode == Opcode::ResolveHeritage) {
            m_lists[index] = { index, isReadNotYetNeeded };
            m_readsNotYetNeeded.append(index);
            keepObjectOfReadPutOff(index, value.operand(0));
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
            bool isThisBeingMade = opcode == Opcode::ToThis && m_thisBeingMade != noDefinition && m_function->kind != FunctionKind::Initializer && block == entryBlock.index() && m_roots[given.index()] == m_thisBeingMade;
            if (isThisBeingMade)
                m_roots.begin()[index] = m_thisBeingMade;
            else
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
        if (opcode == Opcode::CheckInitialized && passedThroughOperand) {
            uint32_t passed = newJoin();
            addSink(SinkKindInScan::Edge, listOf(passedThroughOperand), passed, TypeTable::anything);
            m_lists[index] = listOfNode(passed);
            return;
        }

        if (opcode == Opcode::NarrowByTest && value.operand(OperandIndex::NarrowByTest::left)) {
            m_lists[index] = listOf(value.operand(OperandIndex::NarrowByTest::left));
            return;
        }
    }

    bool isSuperAccess = opcode == Opcode::SuperGet || opcode == Opcode::SuperSet;
    bool passesThroughLazyRead = passedThroughOperand && m_lists[passedThroughOperand.index()].count == isReadNotYetNeeded;
    if ((traits & Row::isPassThrough) && m_opcodesThatOnlyReturnTheirOperand.contains(opcode)) {
        if (passedThroughOperand && !passesThroughLazyRead)
            addList(listOf(passedThroughOperand));
    } else if (traits & Row::hasOperands) {
        uint32_t endOfOperandsRead = traits & Row::holdsOperands ? numberOfNamedOperandsOf(opcode) : UINT32_MAX;
        value.forEachOperand([this, &value, opcode, block, endOfOperandsRead, passesThroughLazyRead, isSuperAccess](ValueID operand, uint32_t operandIndex) {
            if (operandIndex >= endOfOperandsRead || (passesThroughLazyRead && !operandIndex))
                return;
            bool isLeftToSuperAccess = isSuperAccess && ((opcode == Opcode::SuperGet && operandIndex == OperandIndex::SuperGet::receiver)
                || (opcode == Opcode::SuperSet && (operandIndex == OperandIndex::SuperSet::receiver || operandIndex == OperandIndex::SuperSet::value)));
            if (isLeftToSuperAccess) {
                noteLeaving(operand);
                return;
            }
            readOperand(value, operand, operandIndex, operandInfoOf(opcode, operandIndex), block);
        });
    }
    if (passesThroughLazyRead) {
        m_lists[index] = m_lists[passedThroughOperand.index()];
        return;
    }

    if (m_followsSomeEnvironment)
        noteStoreIntoEnvironment();
    if (traits & Row::isLookedAtBehindOperands) {
        if (opcode == Opcode::StrictEq)
            weighEqualityAsCaseOfSwitch(value);
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
        if (opcode == Opcode::NewTemplateObject) {
            for (TemplateString part : value.as<Opcode::NewTemplateObject>().templateStrings()) {
                if (part.cooked)
                    noteTextThatMayNameEvent(part.cooked);
                noteTextThatMayNameEvent(part.raw);
            }
            uint32_t made = classInstanceMadeBy(value, whatIsMadeBy(value).builtinClass);
            addEdge(constant(typeOfElementsOfTemplateObject(value)), contentsNode(made, ContentsSlot::Element));
            addEdge(constant(TypeTable::undefined), contentsNode(made, SlotOfContents::noLongerAsMade()));
        }
    }
    if (mayCallUnknownCode && whatIsMadeBy(value).builtinClass)
        m_resultLeaves = true;

    if (isSuperAccess)
        readSuperAccess(value);
    else if (opcode == Opcode::RequireModule && answersWithResultOfBody(value))
        addNode(resultOfCall(*findCall(value.id())));
    else if (TypeID primitives = m_widestPrimitiveResults[static_cast<unsigned>(opcode)]) {
        if (ValueID converted = traits & Row::convertsToPropertyKey ? value.operand(0) : ValueID()) {
            if (SummaryCall* madeKey = findCallThatMade(converted == m_answerTestedForObject.nameWhereItIsObject ? m_answerTestedForObject.call : converted))
                noteResultReadAsKey(*madeKey);
        }
        if (resultFollowsOperandsInScan(traits, primitives))
            addOperator(value);
        else
            addNode(constant(primitives));
    } else if ((traits & Row::readsHolder) && value.operand(*holderOf(opcode)) && slotNamedByRowOf(value))
        addNode(readContents(value.id(), listOf(value.operand(*holderOf(opcode))), *slotNamedByRowOf(value)));
    else {
        switch (row.result) {
        case Row::Result::Nothing:
            break;
        case Row::Result::Constant:
            addConstant(value);
            break;
        case Row::Result::Unknown:
            addNode(0);
            break;
        case Row::Result::Declared:
            addDeclared(value);
            break;
        case Row::Result::FirstOperand:
            if (passedThroughOperand)
                addList(listOf(passedThroughOperand));
            break;
        case Row::Result::ByRule: {
            bool saysWhereResultIsReadFrom = resultRuleOf(opcode) == ResultRule::Unknown && provenResultRuleOf(opcode);
            ResultRulesOfScan rules { *this };
            applyResultRule(rules, value, 0, saysWhereResultIsReadFrom ? ResultSide::Proven : ResultSide::Unproven);
            break;
        }
        }
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

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::readAccess(const BodyValue& value, SummaryAccess::Kind kind, uint32_t block)
{
    using Row = RowOfOpcodeInScan;
    Opcode opcode = value.opcode();
    const Row& row = rowsOfOpcodesInScan[static_cast<unsigned>(opcode)];
    uint32_t index = value.id().index();
    uint32_t receiverIndex = row.receiver;
    bool stores = kind == SummaryAccess::Kind::Store || kind == SummaryAccess::Kind::Define;
    uint32_t storedIndex = stores && row.stored != Row::noOperand ? row.stored : UINT32_MAX;
    uint32_t keyIndex = row.computedKey != Row::noOperand ? row.computedKey : UINT32_MAX;

    PropertyKeyID key = m_keyOfOperation;
    if (PayloadWord::field(opcode) && value.layout() && value.field() < m_program.at(value.layout()).fields.size())
        key = m_program.at(value.layout()).fields[value.field()].key;

    ValueID object;
    ValueID stored;
    ValueID computedKey;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) ALWAYS_INLINE_LAMBDA {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (operandIndex == receiverIndex) {
            object = operand;
            if (opcode == Opcode::ElemDefine && operand && info.protocols.isEmpty() && !(m_uses[m_roots[operand.index()]] & isUnderConstruction) && holdsOnlyWhatItsBodyLaysInScan(m_program, definitionBehindCasts(operand))) {
                uint32_t root = m_roots[operand.index()];
                uint32_t firstLeft = m_firstLeft[root];
                uint32_t lastLeft = m_lastLeft[root];
                noteLeaving(operand);
                m_firstLeft[root] = firstLeft;
                m_lastLeft[root] = lastLeft;
            } else if (info.uses != OperandUses { OperandUse::Inspects } || !info.protocols.isEmpty())
                noteLeaving(operand);
            else if (m_followsSomeEnvironment && operand && !environmentBeingMade(m_roots[operand.index()]))
                noteLeavingOverEnvironments(m_roots[operand.index()]);
            ListInScan list = listOf(operand);
            if (!info.protocols.isEmpty())
                readProtocols(value, operandIndex, info, list, block);
            return;
        }
        if (storedIndex == operandIndex) {
            stored = operand;
            if (uint32_t root = m_roots[operand.index()]; key && object && m_roots[object.index()] == root && (m_uses[root] & isFollowedWhileMade) && block == entryBlock.index()
                && m_program.nameClassOf(key) != NameClass::Proto && objectBeingMade(root).kind != ObjectBeingMadeKindInScan::OfOneLayout) {
                Vector<PropertyKeyID, 1>& keys = objectBeingMade(root).keysThatHoldItself;
                if (auto place = std::ranges::lower_bound(keys, key); place == keys.end() || *place != key)
                    keys.insert(place - keys.begin(), key);
                if (m_followsSomeEnvironment)
                    noteLeavingOverEnvironments(root);
                m_firstLeft[root] = std::min(m_firstLeft[root], m_position);
                m_lastLeft[root] = std::max(m_lastLeft[root], m_position);
                return;
            }
            noteLeaving(operand);
            return;
        }
        if (keyIndex == operandIndex)
            computedKey = operand;
        readOperand(value, operand, operandIndex, info, block);
    });
    beginResult();

    StringID beginningOfKey;
    if (!key && computedKey && m_walkFollowsConstants) {
        if (m_beginningsOfTextsFound.isEmpty())
            m_beginningsOfTextsFound.fill(0, m_roots.size());
        key = nameKeyOfAccessInScan(m_program, primitiveHeldBy(computedKey), definitionBehindCasts(computedKey), scopedLambda<BodyValue(ValueID)>([&](ValueID operand) {
            return definitionBehindCasts(operand);
        }), m_beginningsOfTextsFound, beginningOfKey);
        if (key)
            m_summary.accessesUnderConstantKeys.append({ value.id(), key });
    }
    if (SummaryCall* madeKey = !key && computedKey ? findCallThatMade(computedKey) : nullptr)
        noteResultReadAsKey(*madeKey);
    if (!key && computedKey) {
        noteConstantFieldTaken(computedKey);
        noteIfParameterIsReadAsKey(computedKey, m_summary.entryParametersReadAsNames);
    }
    ListInScan subject = object ? listOf(object) : listOfNode(0);
    auto addAccessUnderKey = [&] {
        uint32_t added = addAccessUnder(kind, value.id(), subject, key, computedKey);
        m_summary.accesses[added].keyIsNameThatLoopOverSubjectGave = kind == SummaryAccess::Kind::Read && !key && computedKey && object && isNameThatLoopOverGave(computedKey, object);
        m_summary.accesses[added].emptyHolderOrKeyPrefix = beginningOfKey.bits();
        return added;
    };
    if (object) {
        uint16_t& uses = m_uses[m_roots[object.index()]];
        uses |= isAccessedByName;
        bool isStoredInto = key ? kind == SummaryAccess::Kind::Read && key == m_program.keyOf(BuiltinNames::push) : kind == SummaryAccess::Kind::Store;
        if (BodyValue made = isStoredInto ? definitionBehindCasts(object) : BodyValue(); made && made.opcode() == Opcode::Data && made.as<Opcode::Data>().data()) {
            const auto* list = m_program.at(made.as<Opcode::Data>().data()).root.tryAs<DataNode::Array>();
            if (list && !list->elements.isEmpty()) {
                m_storesIntoConstantLists.append({ made.id(), m_blocks[value.id().index()] });
                m_literalsStoredByElement.append(made.as<Opcode::Data>().data());
            }
        }
        if (!key && kind == SummaryAccess::Kind::Store) {
            BodyValue held = definitionBehindCasts(object);
            if (held && held.opcode() == Opcode::CheckInitialized)
                held = definitionBehindCasts(held.operand(0));
            if (held && held.opcode() == Opcode::GetModuleVariable)
                m_variablesStoredByElement.append(held.as<Opcode::GetModuleVariable>().variable());
            else if (held && held.opcode() == Opcode::PropGet)
                m_keysStoredByElement.append(held.as<Opcode::PropGet>().key());
            else if (held && held.opcode() == Opcode::GetEnvironmentField)
                m_fieldsStoredByElement.append(FieldReference { held.as<Opcode::GetEnvironmentField>().layout(), held.as<Opcode::GetEnvironmentField>().field() });
            else if (held && held.opcode() == Opcode::PrivateGet)
                m_keysStoredByElement.append(m_program.at(held.as<Opcode::PrivateGet>().layout()).fields[held.as<Opcode::PrivateGet>().field()].key);
            else if (uint32_t root = m_roots[object.index()]; root < m_function->numberOfEntryParameters() && m_function->entryParameterKind(root) == EntryParameter::Capture)
                m_capturesStoredByElement.append({ m_id, root - m_function->numberOfEntryParametersBefore(EntryParameter::Capture) });
            else if (root < m_function->numberOfEntryParameters() && m_function->entryParameterKind(root) == EntryParameter::Parameter)
                m_elementStores.parameters.append({ m_id, root - m_function->firstParameterIndex() });
            else if (held && held.opcode() == Opcode::Call)
                m_elementStores.results.append({ m_id, held.id() });
        }
        if (DataID literal = key && stored && (kind == SummaryAccess::Kind::Store || kind == SummaryAccess::Kind::Define) ? findArrayLiteral(stored) : DataID())
            m_literalsOfKeys.append({ key, literal });
        if (!key && computedKey && stored && kind == SummaryAccess::Kind::Define)
            uses |= isDefinedUnderComputedKeys;
        else if (!key || (kind != SummaryAccess::Kind::Store && kind != SummaryAccess::Kind::Define) || (kind == SummaryAccess::Kind::Store && m_program.nameClassOf(key) == NameClass::Proto))
            uses |= isAccessedBesideNamedStores;
        if (!key && (uses & isFollowedWhileMade) && kind != SummaryAccess::Kind::Define && kind != SummaryAccess::Kind::Has)
            noteLeaving(object);
    }

    switch (kind) {
    case SummaryAccess::Kind::Read: {
        if (object && key && (m_uses[m_roots[object.index()]] & isFollowedWhileMade) && std::ranges::binary_search(objectBeingMade(m_roots[object.index()]).keysThatHoldItself, key))
            noteLeaving(object);
        bool comesInFrontOfDefinition = object && key && readMayComeInFrontOfDefinition(m_roots[object.index()], key);
        bool isAnsweredBySuperCall = comesInFrontOfDefinition && objectBeingMade(m_roots[object.index()]).kind == ObjectBeingMadeKindInScan::AnsweredBySuperCall;
        if (opcode == Opcode::PropGet && key) {
            m_lists[index] = { index, isReadNotYetNeeded };
            m_readsNotYetNeeded.append(index);
            keepObjectOfReadPutOff(index, object);
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
        if (TypeID primitives = m_widestPrimitiveResults[static_cast<unsigned>(opcode)])
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
            if (VariableID constant = stored && key == m_program.keyOf(BuiltinNames::value) ? constantReadAs(object) : VariableID())
                flowInto(held, valuesLaidIntoDescriptorHeldBy(constant));
            m_stores.append(StoreInScan { value.id(), kind == SummaryAccess::Kind::Define, subject, held, key, m_roots[object.index()], block });
            m_stores.last().storesSurelyIndex = stored && key == m_program.keyOf(BuiltinNames::length) && isSurelyIndex(stored);
            if (BodyValue written = m_stores.last().storesSurelyIndex ? definitionBehindCasts(stored) : BodyValue())
                m_stores.last().storesZero = written.opcode() == Opcode::Integer && !written.as<Opcode::Integer>().integer();
            noteStoreIntoObject(m_roots[object.index()], key, kind);
            return;
        }
        uint32_t access = addAccessUnderKey();
        m_summary.accesses[access].subjectIsMadeInBody = isMadeInBody(object);
        if (!key && computedKey && object && stored && kind == SummaryAccess::Kind::Define)
            m_definitionsUnderComputedKeys.append(DefinitionUnderComputedKeyInScan { m_roots[object.index()], value.id(), computedKey, stored });
        addSink(SinkKindInScan::ValueOfAccess, held, access);
        if (ValueID source = !key && kind == SummaryAccess::Kind::Store && object && stored && computedKey ? objectReadUnder(computedKey, stored) : ValueID()) {
            noteCopyOfMembers(subject, listOf(source), false);
            if (isNameThatLoopOverGave(computedKey, source)) {
                m_summary.accesses[access].isCopySpelledByLoop = true;
                addChange(SummaryChange::Kind::CopiesInto, value.id(), subject, { }, listOf(source));
                m_summary.changes.last().isSpelledByLoop = true;
            }
        }
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

    ValueID readOffHeritage = evaluation.prototypeParentOperand();
    if (ValueID heritage = evaluation.heritageOperand(); heritage && readOffHeritage && m_lists[readOffHeritage.index()] == ListInScan { readOffHeritage.index(), isReadNotYetNeeded }
        && definitionOf(readOffHeritage.index()).opcode() == Opcode::ResolveHeritage && definitionOf(readOffHeritage.index()).operand(0) == heritage) {
        noteLeaving(heritage);
        uint32_t prototypeObject = addRead(value.id(), classObject, m_program.keyOf(BuiltinNames::prototype));
        m_heritages.append(HeritageInScan { value.id(), readOffHeritage, listOf(heritage), classObject, prototypeObject });
        m_lists[value.id().index()] = classObject;
        return;
    }
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

bool ScanOfBody::mayHaveFewSources(ListInScan list) const
{
    Vector<uint32_t, 16> parametersToLookAt;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> parametersMet;
    size_t numberOfSources = 0;
    auto lookAt = [&](ListInScan asked) {
        for (uint32_t position = 0; position < asked.count; ++position) {
            uint32_t atom = m_atoms[asked.first + position];
            if (atom & operatorBit)
                return false;
            if (!(atom & parameterBit)) {
                if (++numberOfSources > mostSourcesOfHeritageLaidOneByOneInScan)
                    return false;
                continue;
            }
            uint32_t parameter = atom & ~parameterBit;
            if (!parametersMet.add(parameter).isNewEntry)
                return false;
            parametersToLookAt.append(parameter);
        }
        return true;
    };
    if (!lookAt(list))
        return false;
    while (!parametersToLookAt.isEmpty()) {
        uint32_t parameter = parametersToLookAt.takeLast();
        for (uint32_t place = m_firstIncoming[parameter]; place < m_firstIncoming[parameter + 1]; ++place) {
            if (!lookAt(m_incoming[m_incomingByParameter[place]].list))
                return false;
        }
    }
    return true;
}

bool ScanOfBody::gatherSourcesOf(ListInScan list, Vector<OriginInScan, 4>& sources, Vector<uint32_t, 4>& parametersGoneThrough)
{
    for (uint32_t position = 0; position < list.count; ++position) {
        uint32_t atom = m_atoms[list.first + position];
        if (atom & operatorBit)
            return false;
        if (!(atom & parameterBit)) {
            sources.append(OriginInScan { atom, { } });
            continue;
        }
        uint32_t parameter = atom & ~parameterBit;
        if (parametersGoneThrough.contains(parameter))
            return false;
        parametersGoneThrough.append(parameter);
        for (uint32_t place = m_firstIncoming[parameter]; place < m_firstIncoming[parameter + 1]; ++place) {
            const IncomingInScan& incoming = m_incoming[m_incomingByParameter[place]];
            size_t first = sources.size();
            if (!gatherSourcesOf(incoming.list, sources, parametersGoneThrough))
                return false;
            for (size_t index = first; index < sources.size(); ++index)
                sources[index] = behindTest(sources[index], incoming.passes);
        }
    }
    return sources.size() <= mostSourcesOfHeritageLaidOneByOneInScan;
}

void ScanOfBody::layHeritagesBySource()
{
    for (const HeritageInScan& heritage : m_heritages) {
        Vector<OriginInScan, 4> origins;
        Vector<uint32_t, 4> parametersGoneThrough;
        if (m_lists[heritage.readOffHeritage.index()].count != isReadNotYetNeeded || !mayHaveFewSources(heritage.above) || !gatherSourcesOf(heritage.above, origins, parametersGoneThrough)) {
            addChange(SummaryChange::Kind::BecomesPrototype, heritage.site, heritage.above, { }, heritage.classObject);
            addChange(SummaryChange::Kind::BecomesPrototype, heritage.site, readByName(heritage.readOffHeritage.index()), { }, listOfNode(heritage.prototypeObject));
            continue;
        }
        if (origins.isEmpty())
            origins.append(OriginInScan { });
        for (const OriginInScan& origin : origins) {
            uint32_t source = origin.node;
            if (origin.passes) {
                source = newJoin();
                addEdge(origin.node, source, origin.passes);
            }
            addChange(SummaryChange::Kind::BecomesPrototype, heritage.site, listOfNode(source), { }, heritage.classObject);
            uint32_t access = addAccessUnder(SummaryAccess::Kind::Read, heritage.readOffHeritage, listOfNode(source), m_program.keyOf(BuiltinNames::prototype), ValueID());
            m_summary.accesses[access].readsPrototypeOfHeritage = true;
            addChange(SummaryChange::Kind::BecomesPrototype, heritage.site, listOfNode(resultOfRead(access)), { }, listOfNode(heritage.prototypeObject));
        }
    }
}

void ScanOfBody::readDefineAccessor(const BodyValue& value, ValueID object, ValueID function, PropertyKeyID key, SummaryChange::Lays lays)
{
    if (function)
        noteLeaving(function);
    if (object)
        m_uses[m_roots[object.index()]] |= key ? isLaidAccessorsUnderNames : isAccessedBesideNamedStores;
    ListInScan subject = object ? listOf(object) : listOfNode(0);
    ListInScan laid = function ? listOf(function) : listOfNode(0);
    if (object && key) {
        uint32_t root = m_roots[object.index()];
        FunctionID half;
        if (function) {
            if (BodyValue closure = definitionOf(m_roots[function.index()]); closure && closure.opcode() == Opcode::NewClosure)
                half = closure.as<Opcode::NewClosure>().function();
        }
        m_accessorsLaid.append(AccessorLaidInScan { root, key, half, lays == SummaryChange::Lays::Getter, static_cast<uint32_t>(m_sinks.size()) });
        bool isFirstUnderName = m_placesOfAccessorsLaid.add(static_cast<uint64_t>(root) << 32 | key.bits()).isNewEntry;
        if ((m_uses[root] & isFollowedWhileMade) && objectBeingMade(root).kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            if (isFirstUnderName)
                objectBeingMade(root).definedInTurn.append(key);
            noteFill(root, false, key.bits());
            if (half)
                noteFill(root, true, half.index());
        }
    }
    uint32_t change = static_cast<uint32_t>(m_summary.changes.size());
    m_summary.definitionsByDescriptorInOrderOfSites.append(static_cast<uint32_t>(m_summary.changes.size()));
    addChange(SummaryChange::Kind::DefinesByDescriptor, value.id(), subject, key, laid);
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
    if (!member.key && definition.computedKeyOperand())
        m_keysWrittenForMembers.append({ place, definition.computedKeyOperand() });

    if (ValueID classObject = definition.classObjectOperand())
        noteLeaving(classObject);
    ValueID function = definition.functionOperand();
    if (function)
        noteLeaving(function);
    if (function && member.key && member.kind == MethodKind::Getter && member.side == ClassSide::Prototype)
        m_gettersDefined.append({ member.key, function });
    addSink(SinkKindInScan::ValueOfMember, function ? listOf(function) : ListInScan(), place);
}

bool ScanOfBody::isNameThatLoopOverGave(ValueID key, ValueID object) const
{
    BodyValue given = definitionBehindCasts(key);
    if (!given || given.opcode() != Opcode::ResultAt || given.as<Opcode::ResultAt>().index() != static_cast<uint32_t>(ForInStepResult::Key))
        return false;
    ValueID producer = given.as<Opcode::ResultAt>().resultsOperand();
    BodyValue step = producer ? definitionOf(producer.index()) : BodyValue();
    return step && step.opcode() == Opcode::ForInStep && m_roots[step.as<Opcode::ForInStep>().objectOperand().index()] == m_roots[object.index()];
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
    bool runsProtocols = !info.protocols.isEmpty();
    if (!runsProtocols && info.uses == OperandUses { OperandUse::Inspects })
        return;
    bool isPassedThrough = !operandIndex && inputPassThroughOpcodes.contains(value.opcode());
    bool isGivenNullAbove = value.opcode() == Opcode::SetLiteralPrototype && operandIndex == OperandIndex::SetLiteralPrototype::object && definitionBehindCasts(value.as<Opcode::SetLiteralPrototype>().prototypeOperand()).opcode() == Opcode::Null;
    if (isGivenNullAbove)
        m_uses[m_roots[operand.index()]] |= hasNullAboveByItsOwnText;
    else if (!isPassedThrough)
        noteLeaving(operand);
    bool isSetOnLiteral = value.opcode() == Opcode::SetLiteralPrototype && !isGivenNullAbove;
    if (isSetOnLiteral && operandIndex == OperandIndex::SetLiteralPrototype::object)
        noteLiteralInParent(value);
    bool isParentOfLiteral = isSetOnLiteral && operandIndex != OperandIndex::SetLiteralPrototype::object && !m_literalsBelowParents.isEmpty() && m_literalsBelowParents.last().site == value.id();
    if (isParentOfLiteral)
        m_literalsBelowParents.last().firstSinkOfParent = static_cast<uint32_t>(m_sinks.size());

    if (runsProtocols && info.protocols.contains(Protocol::Thenable)) {
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
            noteCopyAtAllocation(value, value.operand(operandIndex));
            m_summary.changes.last().subjectIsMadeInBody = isMadeInBody(value.operand(operandIndex));
            break;
        }
        case OperandUse::DefinesName:
        case OperandUse::TakesDescriptor:
            defineByDescriptor(value.id(), list, value.opcode() == Opcode::SetFunctionName ? m_program.keyOf(BuiltinNames::name) : m_keyOfOperation, std::nullopt);
            m_summary.changes.last().descriptorLaysNoAccessor = value.opcode() == Opcode::SetFunctionName;
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
    if (isParentOfLiteral)
        m_literalsBelowParents.last().endOfSinksOfParent = static_cast<uint32_t>(m_sinks.size());
    if (runsProtocols)
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
            if (value.opcode() == Opcode::Eq)
                convertOperandOfEquality(value, operandIndex, list);
            else
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

void ScanOfBody::convertOperandOfEquality(const BodyValue& value, uint32_t operandIndex, ListInScan list)
{
    ListInScan other = listOf(value.operand(1 - operandIndex));
    if (isAlwaysPrimitive(list) || list == other)
        return;
    uint32_t index = static_cast<uint32_t>(m_equalities.size());
    m_equalities.append(EqualityInScan { value.id(), list, other });
    addSink(SinkKindInScan::OtherOperandOfEquality, other, index);
    addSink(SinkKindInScan::ConvertedByEquality, list, index);
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
    case IteratorKind::Async: {
        handToCodeThatRuns(iterator);
        ListInScan asyncIterator = listOfNode(resultOfCall(callMember(site, iterable, builtins.nameOf(WellKnownSymbol::AsyncIterator), { }, block)));
        addRead(site, asyncIterator, m_program.keyOf(BuiltinNames::next));
        return asyncIterator;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t ScanOfBody::stepAsyncIterator(ValueID site, ListInScan iterator, std::optional<ListInScan> sent, uint32_t block)
{
    uint32_t call = sent ? callMember(site, iterator, BuiltinNames::next, { *sent }, block) : callMember(site, iterator, BuiltinNames::next, { }, block);
    size_t numberOfAccesses = m_summary.accesses.size();
    ListInScan result = settle(site, listOfNode(resultOfCall(call)), block);
    if (m_summary.accesses.size() > numberOfAccesses)
        m_summary.accesses[numberOfAccesses].standsRightBehindCall = true;
    addRead(site, result, m_program.keyOf(BuiltinNames::done));
    return addRead(site, result, m_program.keyOf(BuiltinNames::value));
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
            for (IterationHelper helper : { IterationHelper::OpenIterator, IterationHelper::StepIterator, IterationHelper::WayToStepIterator, IterationHelper::StepIteratorInWay, IterationHelper::CloseIterator, IterationHelper::CloseIteratorAfterThrow, IterationHelper::NextOfStepsOverText }) {
                if (!functionOfIterationHelper(m_program, helper))
                    continue;
                if ((helper == IterationHelper::WayToStepIterator || helper == IterationHelper::StepIteratorInWay) && value.as<Opcode::IteratorOpen>().isDestructuring())
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
        auto open = value.as<Opcode::IteratorOpen>();
        ListInScan iterator = openIterator(site, list, open.iteratorKind(), block);
        if (open.iteratorKind() == IteratorKind::Async && open.isDelegating()) {
            handToCodeThatRuns(iterator);
            iterator = listOfNode(0);
        }
        addList(iterator);
        return;
    }
    if (info.kind == OperandKind::IteratorState && !stepsSynchronousIteratorInScan(value)) {
        if (opcode == Opcode::IteratorResume && value.as<Opcode::IteratorResume>().resumeMode() == ResumeMode::Next) {
            ValueID operand = value.as<Opcode::IteratorResume>().sentOperand();
            addNode(stepAsyncIterator(site, list, operand ? std::optional(listOf(operand)) : std::nullopt, block));
            addNode(0);
            return;
        }
        if (opcode == Opcode::IteratorClose) {
            handToCodeThatRuns(listOfNode(resultOfCall(callMember(site, list, BuiltinNames::returnKeyword, { }, block))));
            return;
        }
        if (opcode == Opcode::IteratorResume && value.as<Opcode::IteratorResume>().resumeMode() == ResumeMode::Throw)
            callMember(site, listOfNode(0), BuiltinNames::throwKeyword, { listOfNode(0) }, block);
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
    uint32_t stepValue = stepIterator(site, iterator, BuiltinNames::next, std::nullopt, block);
    callMember(site, iterator, BuiltinNames::returnKeyword, { }, block);
    if (info.uses.contains(OperandUse::RetainsContents))
        retainContents(value, listOfNode(stepValue));
    else
        escape(listOfNode(stepValue));
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
            if (m_function->isModuleBody() && m_program.at(variable).module == m_function->module && m_program.at(variable).flags.contains(VariableFlags::IsImmutable)) {
                m_valuesOfImmutableVariables.append({ variable, value.as<Opcode::PutModuleVariable>().valueOperand() });
                m_storesIntoImmutableVariables.append({ variable, value.id() });
            }
            if (m_program.at(variable).initialization == VariableInitialization::Function)
                m_summary.functionVariablesStoredInto.append(variable);
            flowInto(list, variableNode(variable));
        }
        else if (value.opcode() == Opcode::DirectEval) {
            uint32_t handed = newJoin();
            flowInto(list, handed);
            m_summary.nodesHandedToEvals.append(handed);
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
        FunctionID closure = value.as<Opcode::NewClosure>().function();
        if (closure && (m_makersOfClosures.isEmpty() || m_makersOfClosures.last().closure != closure || m_makersOfClosures.last().maker != m_id))
            m_makersOfClosures.append({ closure, m_id });
        if (DataID literal = findArrayLiteral(value.operand(operandIndex)))
            m_literalsOfCaptures.append({ CaptureInScan { closure, place }, literal });
        flowInto(list, captureNode(closure, place));
        return;
    }

    if (opcode != Opcode::NewObject || !isListed) {
        MadeByOperationInScan made = whatIsMadeBy(value);
        if (made.isOpaque)
            return;
        if (isListed && made.builtinClass && m_program.builtins().findSlot(made.builtinClass, ContentsSlot::Element)) {
            flowInto(list, contentsNode(classInstanceMadeBy(value, made.builtinClass), ContentsSlot::Element));
            if (opcode == Opcode::NewArray && value.numberOfVariadicOperands() > 1 && value.as<Opcode::NewArray>().holes().empty()) {
                Vector<uint32_t>& places = m_summary.placesOfWrittenLists;
                if (m_writtenListBeingListed != value.id()) {
                    m_writtenListBeingListed = value.id();
                    if (places.isEmpty())
                        places.append(0);
                    places.append(classInstanceMadeBy(value, made.builtinClass));
                    places.append(value.numberOfVariadicOperands());
                    m_firstPlaceOfWrittenListBeingListed = static_cast<uint32_t>(places.size());
                    places.insertFill(places.size(), 0, value.numberOfVariadicOperands());
                    places[0] = static_cast<uint32_t>(places.size());
                }
                addSink(SinkKindInScan::PlaceOfWrittenList, list, m_firstPlaceOfWrittenListBeingListed + place);
            }
            return;
        }
        if (opcode == Opcode::CreateMappedArguments && made.builtinClass) {
            flowInto(list, contentsNode(classInstanceMadeBy(value, made.builtinClass), SlotOfContents::under(m_program.keyOf(BuiltinNames::callee))));
            return;
        }
        escape(list);
        m_resultLeaves = true;
        return;
    }

    auto allocation = value.as<Opcode::NewObject>();
    LayoutID layout = allocation.layout();
    std::span<const uint32_t> fields = allocation.fields();
    if (DataID literal = place < fields.size() ? findArrayLiteral(value.operand(operandIndex)) : DataID()) {
        const Layout& holder = m_program.at(layout);
        if (holder.isEnvironment())
            m_literalsOfFields.append({ FieldReference { layout, fields[place] }, literal });
        else if (PropertyKeyID key = holder.fields[fields[place]].key)
            m_literalsOfKeys.append({ key, literal });
    }
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
    if (opcode == Opcode::SetLiteralPrototype && definitionBehindCasts(value.as<Opcode::SetLiteralPrototype>().prototypeOperand()).opcode() == Opcode::Null)
        return;
    if (opcode == Opcode::PutEnvironmentField) {
        auto store = value.as<Opcode::PutEnvironmentField>();
        if (FieldReference field { store.layout(), store.field() }; !m_program.moduleInstances.isEmpty() && m_function->isModuleBody() && m_program.isInstanceVariable(field) && fieldAt(m_program, field).flags.contains(FieldFlags::IsImmutable))
            m_immutableFieldValues.append({ field, store.valueOperand() });
        if (DataID literal = findArrayLiteral(store.valueOperand()))
            m_literalsOfFields.append({ FieldReference { store.layout(), store.field() }, literal });
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
    if (opcode == Opcode::AppendHole) {
        storeIntoContents(value.id(), list, SlotOfContents::mayHaveHoles(), listOfNode(constant(TypeTable::undefined)));
        return;
    }
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
    if (isInitializer) {
        m_uses[m_thisBeingMade] |= isUnderConstruction;
        return;
    }
    m_parentConstructor = parentConstructorOf(m_program, m_id);
}

bool ScanOfBody::callsParentConstructor(const BodyValue& value) const
{
    if (!m_parentConstructor || value.opcode() != Opcode::Call || m_blockBeingReadHasHandler)
        return false;
    auto call = value.as<Opcode::Call>();
    ValueID receiver = call.thisValueOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    BodyValue callee = call.calleeOperand() ? definitionBehindCasts(call.calleeOperand()) : BodyValue();
    if (!receiver || arguments.empty() || !arguments[0] || m_roots[arguments[0].index()] != m_thisBeingMade || !callee || callee.opcode() != Opcode::PropGet)
        return false;
    auto read = callee.as<Opcode::PropGet>();
    if (!read.key() || !read.objectOperand() || m_roots[read.objectOperand().index()] != m_roots[receiver.index()])
        return false;
    uint32_t parent = m_roots[receiver.index()];
    if ((m_receiverOfParentCalls != noDefinition && m_receiverOfParentCalls != parent) || (m_parentTestedForNull != noDefinition && m_parentTestedForNull != parent))
        return false;
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinID member = findOwnMember(m_program, builtins.canonicalID(Builtins::FunctionPrototype), read.key());
    member = member ? builtins.canonicalID(member) : BuiltinID();
    return member && (member == builtins.canonicalID(Builtins::FunctionPrototypeCall) || member == builtins.canonicalID(Builtins::FunctionPrototypeApply));
}

void ScanOfBody::noteParentCall(const BodyValue& value, SummaryCall& call)
{
    call.continuesConstruction = true;
    m_receiverOfParentCalls = m_roots[value.as<Opcode::Call>().thisValueOperand().index()];
    noteLeaving(ValueID(m_thisBeingMade), WayOfLeavingInScan::IsHandedToParent);
    m_uses[m_thisBeingMade] |= isAccessedBesideNamedStores;
    ObjectBeingMadeInScan& object = objectBeingMade(m_thisBeingMade);
    if (!object.parts.contains(m_parentConstructor))
        object.parts.append(m_parentConstructor);
    noteFill(m_thisBeingMade, true, m_parentConstructor.index());
    m_resultsOfParent.append(value.id().index());
}

std::optional<uint32_t> ScanOfBody::findTargetWithoutParent(const BodyValue& test) const
{
    if (!m_parentConstructor || !test)
        return std::nullopt;
    BodyValue asked = test;
    uint32_t target = 0;
    while (asked && (asked.opcode() == Opcode::ToBoolean || asked.opcode() == Opcode::LogicalNot)) {
        target ^= asked.opcode() == Opcode::LogicalNot;
        asked = asked.operand(0) ? definitionOf(asked.operand(0).index()) : BodyValue();
    }
    if (!asked || asked.opcode() != Opcode::TypeTest)
        return std::nullopt;
    TypeID tested = asked.as<Opcode::TypeTest>().testedType();
    if (tested != TypeID(TypeTable::null) && tested != TypeID(TypeTable::undefined))
        return std::nullopt;
    uint32_t parent = m_roots[asked.operand(0).index()];
    if (parent >= m_function->captures.size() || (m_receiverOfParentCalls != noDefinition && m_receiverOfParentCalls != parent) || (m_parentTestedForNull != noDefinition && m_parentTestedForNull != parent))
        return std::nullopt;
    return target;
}

void ScanOfBody::takeResultsOfParent(const BodyBlock& block)
{
    uint32_t number = block.id().index();
    if (m_numberOfEdgesInto[number] != 1)
        return;
    for (const ResultHandedToBlock& handed : m_resultsOfParentHanded) {
        if (handed.block == number && handed.position < block.numberOfParameters())
            m_resultsOfParent.append(block.parameter(handed.position).index());
    }
}

void ScanOfBody::takeThisAfterParent(const BodyBlock& block)
{
    uint32_t number = block.id().index();
    for (uint32_t position = 0; position < block.numberOfParameters(); ++position) {
        auto isResultHandedHere = [&](const ResultHandedToBlock& handed) {
            return handed.block == number && handed.position == position;
        };
        auto isThisHandedHere = [&](const ObjectHandedToBlockInScan& handed) {
            return handed.block == number && handed.position == position && handed.root == m_thisBeingMade;
        };
        size_t numberOfResults = std::ranges::count_if(m_resultsOfParentHanded, isResultHandedHere);
        size_t numberOfObjects = std::ranges::count_if(m_objectsHandedToBlocks, isThisHandedHere);
        if (!numberOfResults || !numberOfObjects || numberOfResults + numberOfObjects != m_numberOfEdgesInto[number])
            continue;
        m_objectsHandedToBlocks.removeAllMatching(isThisHandedHere);
        m_roots[block.parameter(position).index()] = m_thisBeingMade;
        m_blocksThatTookObjects.append(BlockThatTookObjectInScan { number, m_thisBeingMade });
        m_summary.otherNamesOfObjectsBeingMade.append(OtherNameOfObjectBeingMade { block.parameter(position), ValueID(m_thisBeingMade) });
    }
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
        if (numberListed < layout.fields.size()) {
            follow(value.id().index(), ObjectBeingMadeKindInScan::OfOneLayout);
            if (value.opcode() == Opcode::Data) {
                ObjectBeingMadeInScan& constant = m_objectsBeingMade.last();
                constant.isFinished = true;
                constant.isConstant = true;
                constant.blockOfFinish = m_blockBeingRead;
                constant.positionOfFinish = value.id().index();
            }
        }
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
    m_leavesFieldsEmptyForGood |= !m_program.at(id).fields.isEmpty();
    leaveFieldsEmptyButFor(id, { });
}

void ScanOfBody::leaveFieldsEmptyButFor(LayoutID id, std::span<const uint32_t> fieldsHeld)
{
    if (fieldsHeld.size() == m_program.at(id).fields.size())
        return;
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(id.index());
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(static_cast<uint32_t>(fieldsHeld.size()));
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(fieldsHeld);
}

void ScanOfBody::noteCopyAtAllocation(const BodyValue& copy, ValueID target)
{
    if (copy.opcode() != Opcode::CopyDataProperties)
        return;
    auto operation = copy.as<Opcode::CopyDataProperties>();
    uint32_t root = m_roots[target.index()];
    if (!operation.excludedKeys().isEmpty() || !operation.excludedOperands().empty() || !(m_uses[root] & isFollowedWhileMade) || m_blocks[root] != m_blockBeingRead || m_firstLeft[root] < copy.id().index())
        return;
    BodyValue made = definitionOf(root);
    if (!made || made.opcode() != Opcode::NewObject || m_program.at(made.as<Opcode::NewObject>().layout()).kind != LayoutKind::Record)
        return;
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout && !object.positionOfFinish && object.copyAtAllocation == ThinSummary::noCopy)
        object.copyAtAllocation = static_cast<uint32_t>(m_summary.changes.size() - 1);
}

bool ScanOfBody::deferFieldsToCopy(const ObjectBeingMadeInScan& object, LayoutID id)
{
    if (object.copyAtAllocation == ThinSummary::noCopy || !object.positionOfFinish || object.isGivenUp)
        return false;
    listFieldsHeldBy(object, m_program.at(id), m_fieldsThatObjectHolds);
    if (m_fieldsThatObjectHolds.size() == m_program.at(id).fields.size())
        return true;
    m_summary.makesCopies = true;
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(id.index());
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(static_cast<uint32_t>(m_fieldsThatObjectHolds.size()) | ThinSummary::isFilledByCopy);
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(object.copyAtAllocation);
    m_summary.fieldsHeldOfLayoutsLeftEmpty.append(m_fieldsThatObjectHolds.span());
    return true;
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

std::span<const FillInScan> ScanOfBody::fillsOf(uint32_t root, SetsOfFillsInScan::Set fills)
{
    m_fillsOfObjectAsked.shrink(0);
    m_setsOfFills.forEachFillOf(fills, root, [&](const FillInScan& fill) {
        m_fillsOfObjectAsked.append(fill);
    });
    return m_fillsOfObjectAsked.span();
}

void ScanOfBody::noteFill(uint32_t root, bool isPart, uint32_t number)
{
    FillInScan fill { root, isPart, number };
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.isGivenUp || (object.hasLeft && object.hasEnded && !std::ranges::binary_search(object.filled, fill) && !std::ranges::binary_search(object.filledAtEnd, fill)))
        return;
    object.positionOfLastFill = m_position;
    m_fills = m_setsOfFills.with(m_fills, fill);
}

void ScanOfBody::noteWhereFinishedLiteralLeavesFirst(ObjectBeingMadeInScan& object)
{
    if (object.hasLeft || !object.isFinished || object.blockOfFinish != m_blockBeingRead || m_blockBeingReadHasHandler)
        return;
    if (object.positionOfLastFill <= object.positionOfFinish || object.positionOfLastFill >= m_position)
        return;
    object.endsWhereItLeaves = true;
    m_summary.makersThatEndWhereTheyLeave.append({ ValueID(object.root), m_blockBeingRead, m_position });
}

void ScanOfBody::noteLookUp(uint32_t root, PropertyKeyID key)
{
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.kind != ObjectBeingMadeKindInScan::OfOneLayout) {
        if (object.keysLookedUp.isEmpty() || object.keysLookedUp.last() != key)
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
    if (!m_resultsOfParent.isEmpty() && block > from && kind == KindOfEdgeInScan::Plain && m_resultsOfParent.contains(root)) [[unlikely]]
        m_resultsOfParentHanded.append({ block, position });
    if (!(m_uses[root] & isFollowedWhileMade) || block <= from || kind == KindOfEdgeInScan::ToHandler) {
        m_uses[root] |= isAccessedBesideNamedStores;
        noteLeaving(value);
        return;
    }
    m_objectsHandedToBlocks.append(ObjectHandedToBlockInScan { block, position, root, m_position, m_fills });
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
        m_lastLeft[handed.root] = std::max(m_lastLeft[handed.root], handed.placeInStream);
        m_uses[handed.root] |= isAccessedBesideNamedStores;
        if (hasParameter)
            object.parametersHandedTo.append(block.parameter(handed.position).index());
        meetFillsWhereObjectLeaves(object, fillsOf(handed.root, handed.fills));
    }
}

void ScanOfBody::carryFillsOver(uint32_t from, uint32_t to)
{
    SetsOfFillsInScan::Set& kept = m_fillsAtBlocks[to];
    if (to <= from) {
        if (kept != noFillsYet) {
            m_setsOfFills.forEachFillMissingIn(kept, m_fills, [&](const FillInScan& fill) {
                giveUp(fill.root);
            });
        }
        for (const BlockThatTookObjectInScan& taken : m_blocksThatTookObjects) {
            if (taken.block == to)
                giveUp(taken.root);
        }
        return;
    }
    kept = kept == noFillsYet ? m_fills : m_setsOfFills.meet(kept, m_fills);
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

void ScanOfBody::listFieldsHeldBy(const ObjectBeingMadeInScan& object, const Layout& layout, Vector<uint32_t>& fieldsHeld)
{
    fieldsHeld.shrink(0);
    auto hold = [&](PropertyKeyID key) {
        if (std::optional<uint32_t> field = layout.findField(key))
            fieldsHeld.append(*field);
    };
    bool goesByLiteralAlone = layout.kind == LayoutKind::Record && !object.endsWhereItLeaves;
    if (!goesByLiteralAlone || (object.isFinished && !object.isConstant && !object.isGivenUp)) {
        for (const FillInScan& fill : goesByLiteralAlone ? object.filledAtEnd : object.filled) {
            if (fill.root == object.root && !fill.isPart)
                hold(PropertyKeyID::fromBits(fill.number));
        }
    }
    BodyValue made = definitionOf(object.root);
    if (made && made.opcode() == Opcode::Data) {
        if (const DataNode::Object* data = objectOfDataInScan(m_program, made)) {
            for (const DataMember& member : data->members)
                hold(member.key);
        }
    } else if (made && made.opcode() == Opcode::NewObject) {
        for (uint32_t field : made.as<Opcode::NewObject>().fields()) {
            if (field < layout.fields.size() && layout.fields[field].key)
                fieldsHeld.append(field);
        }
    }
    sortWithoutRepeatsInScan(fieldsHeld);
}

void ScanOfBody::listFieldsLeftEmptyByObjectsOfOneLayout()
{
    std::ranges::sort(m_placesOfObjectsOfOneLayout);
    std::span<const std::pair<LayoutID, uint32_t>> rest = m_placesOfObjectsOfOneLayout.span();
    while (!rest.empty()) {
        LayoutID id = rest.front().first;
        const Layout& layout = m_program.at(id);
        listFieldsHeldBy(m_objectsBeingMade[rest.front().second], layout, m_fieldsThatEveryObjectHolds);
        for (rest = rest.subspan(1); !rest.empty() && rest.front().first == id; rest = rest.subspan(1)) {
            if (m_fieldsThatEveryObjectHolds.isEmpty())
                continue;
            listFieldsHeldBy(m_objectsBeingMade[rest.front().second], layout, m_fieldsThatObjectHolds);
            m_fieldsThatEveryObjectHolds.removeAllMatching([&](uint32_t field) {
                return !std::ranges::binary_search(m_fieldsThatObjectHolds, field);
            });
        }
        leaveFieldsEmptyButFor(id, m_fieldsThatEveryObjectHolds.span());
    }
}

void ScanOfBody::listConstructions()
{
    m_placesOfObjectsOfOneLayout.shrink(0);
    for (const ObjectBeingMadeInScan& object : m_objectsBeingMade) {
        if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            listDefinitionsBehindAllocation(object);
            LayoutID id = layoutOfAllocation(definitionOf(object.root));
            const Layout& layout = m_program.at(id);
            bool goesByLiteralAlone = layout.kind == LayoutKind::Record && !object.endsWhereItLeaves;
            if ((object.hasLeft || goesByLiteralAlone) && !deferFieldsToCopy(object, id))
                m_placesOfObjectsOfOneLayout.append({ id, static_cast<uint32_t>(&object - m_objectsBeingMade.begin()) });
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
        for (PropertyKeyID key : object.keysThatHoldItself)
            m_keysOfConstruction.append(SummaryConstruction::Key { key, false, false, true });
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
            m_summary.keysOfConstructions.last().holdsObject |= key.holdsObject;
        }
        construction.numberOfKeys = static_cast<uint32_t>(m_summary.keysOfConstructions.size()) - construction.firstKey;

        construction.firstPart = static_cast<uint32_t>(m_summary.partsOfConstructions.size());
        for (FunctionID part : object.parts)
            m_summary.partsOfConstructions.append(SummaryConstruction::Part { part, std::ranges::binary_search(object.filled, FillInScan { object.root, true, part.index() }) });
        construction.numberOfParts = static_cast<uint32_t>(object.parts.size());
        m_summary.constructions.append(construction);
    }
    listFieldsLeftEmptyByObjectsOfOneLayout();
    if (!m_accessorsLaid.isEmpty())
        listHalvesOfAccessorsNotLaid();
    if (!m_objectsBeingMade.isEmpty())
        listBirths();
    AOT::stableSort(m_summary.makersThatEndWhereTheyLeave, { }, &SummaryMakerThatEndsWhereItLeaves::positionOfLeaving);
    for (const SummaryMakerThatEndsWhereItLeaves& maker : m_summary.makersThatEndWhereTheyLeave)
        m_summary.allocationsOfMakersThatEndWhereTheyLeave.append(maker.allocation);
    std::ranges::sort(m_summary.allocationsOfMakersThatEndWhereTheyLeave, { }, &ValueID::index);
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
    if (numberOfCaptures)
        m_summary.sitesThatHandOverCaptures.append(value.id());
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

        if (index < numberOfCaptures && arguments[index])
            flowInto(listOf(arguments[index]), captureNode(call.function, index));
    }
}

bool ScanOfBody::isGatheredListThatOnlyCallsTake(ValueID list)
{
    if (!m_function->has(FunctionFlags::HasRestParameter) || m_function->kind == FunctionKind::Getter || m_function->kind == FunctionKind::Setter)
        return false;
    if (list != m_function->body.entry().parameter(m_function->firstParameterIndex() + m_function->numberOfPositionalParameters()))
        return false;
    if (!m_onlyCallsTakeGatheredList)
        m_onlyCallsTakeGatheredList = isOperandOfNothingButListsOfCallsInScan(m_function->body, list, m_numberOfValuesScanned);
    return *m_onlyCallsTakeGatheredList;
}

static uint32_t countMentionsInScan(const Body& body, ValueID mentioned, uint64_t& numberOfValuesScanned)
{
    const UnitIDRemap* unit = &body.unit();
    uint32_t count = 0;
    auto countArguments = [&](BodyTarget target) {
        count += std::ranges::count(target.arguments(), mentioned);
    };
    BodyBlock block = body.entry();
    for (uint32_t remaining = body.numberOfBlocks(); remaining--;) {
        count += block.subject() == mentioned;
        if (block.hasHandler())
            countArguments(block.handler());
        BodyValues::Iterator cursor = block.values().begin();
        numberOfValuesScanned += block.numberOfValues();
        for (uint32_t values = block.numberOfValues(); values--; ++cursor) {
            (*cursor).forEachOperand([&](ValueID operand, uint32_t) {
                count += operand == mentioned;
            });
        }
        BodyTerminator terminator(cursor.words(), unit, block.id());
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            count += operand == mentioned;
        });
        for (BodyTarget target : terminator.targets())
            countArguments(target);
        block = { terminator.end(), nullptr, unit, BlockID(block.id().index() + 1), block.endOfValues() };
    }
    return count;
}

bool ScanOfBody::appliesGatheredArgumentsOnce(const BodyValue& call, PropertyKeyID key)
{
    if (!m_function->has(FunctionFlags::HasHiddenRest) || m_function->numberOfPositionalParameters() || m_function->kind == FunctionKind::Getter || m_function->kind == FunctionKind::Setter)
        return false;
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    BuiltinID found = findOwnMember(m_program, m_program.builtins().canonicalID(Builtins::FunctionPrototype), key);
    if (arguments.size() != 2 || !found || m_program.builtins().canonicalID(found) != Builtins::FunctionPrototypeApply)
        return false;
    if (call.id().index() >= m_function->body.entry().endOfValues().index())
        return false;
    BodyValue made = definitionOf(arguments[1].index());
    ValueID gathered;
    if (made && made.opcode() == Opcode::CreateMappedArguments)
        gathered = made.as<Opcode::CreateMappedArguments>().restOperand();
    else if (made && made.opcode() == Opcode::CreateUnmappedArguments)
        gathered = made.as<Opcode::CreateUnmappedArguments>().restOperand();
    if (!gathered || made.numberOfVariadicOperands())
        return false;
    if (!m_argumentsObjectIsNamedOnce)
        m_argumentsObjectIsNamedOnce = countMentionsInScan(m_function->body, made.id(), m_numberOfValuesScanned) == 1 && countMentionsInScan(m_function->body, gathered, m_numberOfValuesScanned) == 1;
    return *m_argumentsObjectIsNamedOnce;
}

ALWAYS_INLINE_INTO_OTHER_FILES BuiltinID ScanOfBody::builtinExpectedOf(ValueID value) const
{
    TypeReader types(m_program);
    for (BodyValue cast = definitionOf(value.index()); cast && (rowsOfOpcodesInScan[static_cast<unsigned>(cast.opcode())].traits & RowOfOpcodeInScan::isPassThrough); cast = definitionOf(cast.operand(0).index())) {
        TypeID expected = cast.opcode() == Opcode::Convert ? cast.as<Opcode::Convert>().declared().written() : TypeID();
        if (BuiltinID builtin = expected && !holdsOnlyPrimitives(expected) ? singleBuiltinOf(types, types.at(expected)) : BuiltinID())
            return builtin;
    }
    return { };
}

BuiltinID ScanOfBody::builtinExpectedToAnswer(ValueID value) const
{
    BodyValue answer = definitionBehindCasts(value);
    while (answer && answer.opcode() == Opcode::PropGet && answer.as<Opcode::PropGet>().objectOperand())
        answer = definitionBehindCasts(answer.as<Opcode::PropGet>().objectOperand());
    if (!answer || answer.opcode() != Opcode::Call)
        return { };
    for (auto record = std::ranges::lower_bound(m_summary.calls, answer.id(), { }, &SummaryCall::site); record != m_summary.calls.end() && record->site == answer.id(); ++record) {
        if (record->expectedBuiltin && record->expectsCalleeItself)
            return record->expectedBuiltin;
    }
    return { };
}

ALWAYS_INLINE void ScanOfBody::readCallByValue(const BodyValue& value, SummaryCall& call, uint32_t callIndex)
{
    Opcode opcode = value.opcode();
    std::optional<unsigned> thisPosition = OperandPosition::thisValue(opcode);
    std::optional<unsigned> newTargetPosition = OperandPosition::newTarget(opcode);
    std::optional<unsigned> listPosition = OperandPosition::list(opcode);
    call.isConstruction = newTargetPosition.has_value();
    call.passesList = listPosition.has_value();
    bool callsParent = callsParentConstructor(value);

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
            if (isGatheredListThatOnlyCallsTake(operand)) {
                call.passesOnGatheredArguments = true;
                m_summary.traits.add(SummaryTrait::OnlyPassesOnGatheredArguments);
            }
            m_summary.arguments.append(readContents(value.id(), listOf(operand), ContentsSlot::Element));
        } else if (BodyValue written = call.isConstruction && callee && definitionBehindCasts(callee) && definitionBehindCasts(callee).opcode() == Opcode::Builtin ? definitionBehindCasts(operand) : BodyValue(); written && written.opcode() == Opcode::Integer && written.as<Opcode::Integer>().integer() > 0 && WrittenUnit::integer(written.as<Opcode::Integer>().integer())) {
            uint32_t place = static_cast<uint32_t>(m_summary.arguments.size());
            m_summary.arguments.append(0);
            addSink(SinkKindInScan::Argument, listOfNode(nodeFor(SummaryNode::Kind::Constant, typeOfIntegerInScan(written.as<Opcode::Integer>().integer()).bits(), WrittenUnit::integer(written.as<Opcode::Integer>().integer()).bits())), place);
        } else {
            if (call.isConstruction && callee && definitionBehindCasts(callee) && definitionBehindCasts(callee).opcode() == Opcode::Builtin)
                call.lengthOfWrittenList = lengthOfListWrittenRightInFrontOf(value, operand);
            addArgument(operand, !callsParent || static_cast<uint32_t>(m_summary.arguments.size()) != call.firstArgument);
        }
    });
    if (callsParent)
        noteParentCall(value, call);

    m_summary.ownEffects.mayThrow |= ThrowEffect(true);

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
    call.expectedBuiltin = builtinExpectedOf(callee);
    call.expectsCalleeItself = !!call.expectedBuiltin;
    if (definition && definition.opcode() == Opcode::NativeBinding)
        call.builtin = definition.as<Opcode::NativeBinding>().builtin();
    auto isAsBuilt = [&](auto named) {
        return named.builtin() && (named.isNotReadFromHolder() || namesWhatNobodyReplacesInScan(m_program.builtins(), named.builtin()));
    };
    auto callBuiltinThatNobodyReplaces = [&](BuiltinID builtin) {
        call.callee = SummaryCall::Callee::Builtin;
        call.builtin = builtin;
        call.subject = noSummaryNode;
        call.key = keyOfSymbolThatCallMakesInScan(m_program, m_id, value, scopedLambda<BodyValue(ValueID)>([&](ValueID operand) {
            return definitionOf(operand.index());
        }), [&](FunctionID body) {
            return m_bodiesThatSeveralModulesName ? !m_bodiesThatSeveralModulesName->get(body.index()) : isBodyOfOneModuleInScan(m_program, body);
        });
        readChangesMadeByBuiltin(value, call, callIndex, receiver);
        if (opcode == Opcode::Call && m_program.builtins().has(BuiltinMark::AppliesMethodDecorators, m_program.builtins().canonicalID(call.builtin)))
            readDecoratorsOfMethod(value);
    };
    if (definition && definition.opcode() == Opcode::Builtin) {
        auto named = definition.as<Opcode::Builtin>();
        if (isAsBuilt(named) && !compiledModuleWithObject(m_program, m_program.builtins().canonicalID(named.builtin()))) {
            callBuiltinThatNobodyReplaces(named.builtin());
            return;
        }
        call.builtin = named.builtin();
    }
    if (definition && definition.opcode() == Opcode::PropGet && receiver) {
        auto read = definition.as<Opcode::PropGet>();
        if (read.key() && read.objectOperand() && m_roots[read.objectOperand().index()] == m_roots[receiver.index()]) {
            BodyValue holder = definitionBehindCasts(receiver);
            if (holder && holder.opcode() == Opcode::Builtin && m_program.at(read.key()).kind == PropertyKeyKind::EnginePrivateName && isAsBuilt(holder.as<Opcode::Builtin>())) {
                BuiltinID member = findOwnMember(m_program, m_program.builtins().canonicalID(holder.as<Opcode::Builtin>().builtin()), read.key());
                if (member && !compiledModuleWithObject(m_program, m_program.builtins().canonicalID(member))) {
                    callBuiltinThatNobodyReplaces(member);
                    return;
                }
            }
            call.callee = SummaryCall::Callee::Name;
            call.key = read.key();
            call.subject = noSummaryNode;
            if (opcode == Opcode::Call && appliesGatheredArgumentsOnce(value, read.key())) {
                call.appliesGatheredArguments = true;
                m_summary.traits.add(SummaryTrait::OnlyPassesOnGatheredArguments);
            }
            if (!call.expectedBuiltin)
                call.expectedBuiltin = builtinExpectedOf(receiver);
            if (!call.expectedBuiltin)
                call.expectedBuiltin = builtinExpectedToAnswer(receiver);
            m_uses[definition.id().index()] |= isCalledByName;
            ValueID firstArgument;
            if (opcode == Opcode::Call) {
                auto arguments = value.as<Opcode::Call>().argumentsOperands();
                if (arguments.size())
                    firstArgument = *arguments.begin();
            }
            SummaryCall* madeKey = firstArgument ? findCallThatMade(firstArgument) : nullptr;
            if (madeKey || (firstArgument && m_roots[firstArgument.index()] < m_function->numberOfEntryParameters())) {
                const BuiltinTable& table = m_program.builtins();
                bool isKeyOfCollection = findOwnMember(m_program, table.at(BuiltinClasses::Map).prototype, read.key()) || findOwnMember(m_program, table.at(BuiltinClasses::Set).prototype, read.key());
                if (isKeyOfCollection && madeKey)
                    noteResultReadAsKey(*madeKey);
                else if (isKeyOfCollection)
                    noteIfParameterIsReadAsKey(firstArgument, m_summary.entryParametersReadAsKeys);
            }
            if (m_uses[definition.id().index()] & mayComeInFrontOfDefinition)
                readByName(definition.id().index());
            return;
        }
    }
    noteLeaving(callee);
    addSink(SinkKindInScan::SubjectOfCall, listUnderExclusionsOf(callee), callIndex);
}

void ScanOfBody::readDecoratorsOfMethod(const BodyValue& value)
{
    std::span<const ValueID> arguments = value.variadicOperands();
    if (arguments.size() < 3)
        return;
    BodyValue decorators = definitionBehindCasts(arguments[0]);
    BodyValue element = definitionBehindCasts(arguments[2]);
    if (!decorators || decorators.opcode() != Opcode::NewArray || !element || element.opcode() != Opcode::Call)
        return;
    std::span<const ValueID> described = element.variadicOperands();
    BodyValue kind = described.size() < 2 ? BodyValue() : definitionBehindCasts(described[0]);
    PropertyKeyID key = described.size() < 2 ? PropertyKeyID() : keyWrittenAs(described[1]).key;
    if (!kind || kind.opcode() != Opcode::String || !key)
        return;
    ValueID site = value.id();
    ListInScan holder = listOf(arguments[1]);
    String kindOfElement = m_program.strings.toWTFString(kind.as<Opcode::String>().string());
    bool isMethod = kindOfElement == "method"_s;
    auto declared = std::ranges::find_if(m_gettersDefined.rbegin(), m_gettersDefined.rend(), [&](const auto& getter) { return getter.first == key; });
    if (!isMethod && (kindOfElement != "getter"_s || declared == m_gettersDefined.rend())) {
        addChange(SummaryChange::Kind::DefinesByDescriptor, site, holder, key, { });
        m_summary.changes.last().laysAccessorAsWritten = true;
        return;
    }
    uint32_t found = newJoin();
    addEdge(isMethod ? addRead(site, holder, key) : oneNodeFor(listOf(declared->second)), found);
    uint32_t outermost = found;
    std::span<const ValueID> listed = decorators.variadicOperands();
    for (size_t place = listed.size(); place--;) {
        if (!listed[place])
            return;
        uint32_t callIndex = static_cast<uint32_t>(m_summary.calls.size());
        SummaryCall call;
        call.site = site;
        call.callee = SummaryCall::Callee::Value;
        call.isMadeByProtocol = true;
        call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
        call.numberOfArguments = 2;
        addSink(SinkKindInScan::Argument, listOfNode(found), call.firstArgument);
        m_summary.arguments.append(0);
        m_summary.arguments.append(0);
        m_summary.calls.append(call);
        m_blocksOfCalls.append(m_blocks[site.index()]);
        addSink(SinkKindInScan::SubjectOfCall, listOf(listed[place]), callIndex);
        uint32_t answered = newJoin();
        addEdge(resultOfCall(callIndex), answered);
        if (isMethod) {
            uint32_t store = addAccess(SummaryAccess::Kind::Store, site, holder, key);
            m_summary.accesses[store].isByMethodDecorator = true;
            addSink(SinkKindInScan::ValueOfAccess, listOfNode(answered), store);
        }
        addEdge(answered, found);
        outermost = answered;
    }
    if (isMethod)
        return;
    addChange(SummaryChange::Kind::DefinesByDescriptor, site, holder, key, listOfNode(outermost));
    m_summary.changes.last().laysAccessorAsWritten = true;
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
            ChangeOfBuiltinInScan change { action.kind, action.object, storesWhatOperandHolds ? std::nullopt : action.value.operand, action.key.kind == BuiltinKeyKind::Operand ? action.key.operand : std::nullopt, action.value.kind == BuiltinPlaceKind::NewValue };
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
                if (stored && keyWritten.key)
                    addSink(SinkKindInScan::ValueOfAccess, *stored, addAccess(SummaryAccess::Kind::Store, site, subject, keyWritten.key));
                if (stored)
                    escape(*stored);
            });
            break;
        case BuiltinPropertyActionKind::DeletesByKey:
            note(SummaryChange::Kind::Deletes);
            break;
        case BuiltinPropertyActionKind::AssignsFrom: {
            auto copy = [&](ListInScan subject, std::optional<ListInScan> source) {
                addChange(SummaryChange::Kind::CopiesInto, site, subject, { }, source ? *source : listOfNode(0));
                if (source)
                    noteCopyOfMembers(subject, *source, false);
            };
            if (!change.other || change.other->isReceiver() || call.passesList) {
                forEachPair(copy);
                break;
            }
            forEachListAt(change.object, [&](ListInScan subject) {
                bool findsSource = false;
                for (uint32_t index = change.other->argumentIndex(); index < arguments.size() && change.other->coversArgument(index); ++index) {
                    if (!arguments[index])
                        continue;
                    findsSource = true;
                    if (!storeWhatLiteralSpells(site, subject, arguments[index]))
                        copy(subject, listOf(arguments[index]));
                }
                if (!findsSource)
                    copy(subject, std::nullopt);
            });
            break;
        }
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::TakesDescriptor:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::DefinesFrom:
            forEachPair([&](ListInScan subject, std::optional<ListInScan> descriptor) {
                bool isOneArgument = change.kind == BuiltinPropertyActionKind::TakesDescriptorMap && descriptor && change.other->kind() == BuiltinOperand::Kind::Argument && !call.passesList;
                ValueID map = isOneArgument ? arguments[change.other->argumentIndex()] : ValueID();
                bool mapWaitsForItsJudgment = map && m_firstLeft[m_roots[map.index()]] >= site.index();
                if (mapWaitsForItsJudgment)
                    m_definitionsByMaps.append(DefinitionByMapInScan { static_cast<uint32_t>(m_summary.changes.size()), static_cast<uint32_t>(m_summary.accesses.size()), m_roots[map.index()], site.index(), *descriptor });
                bool isOneDescriptor = change.kind == BuiltinPropertyActionKind::TakesDescriptor && descriptor && change.other->kind() == BuiltinOperand::Kind::Argument && !call.passesList;
                ValueID written = isOneDescriptor ? arguments[change.other->argumentIndex()] : ValueID();
                bool hasNotLeft = written && m_firstLeft[m_roots[written.index()]] >= site.index();
                bool waitsForItsJudgment = hasNotLeft && keyWritten.isWritten;
                if (hasNotLeft)
                    m_definitionsByDescriptors.append(DefinitionByDescriptorInScan { static_cast<uint32_t>(m_summary.changes.size()), m_roots[written.index()], site.index(), waitsForItsJudgment, *descriptor });
                defineByDescriptor(site, subject, keyWritten.key, descriptor, keyWritten.isWritten && !keyWritten.key, change.laysNewValue);
                if (VariableID constant = written && keyWritten.key ? constantReadAs(written) : VariableID())
                    m_summary.changes.last().valueLaidThroughConstant = valuesLaidIntoDescriptorHeldBy(constant);
                if (hasNotLeft && keyWritten.key)
                    m_definitionsByDescriptors.last().access = static_cast<uint32_t>(m_summary.accesses.size()) - 1;
                if (descriptor && !waitsForItsJudgment && !mapWaitsForItsJudgment)
                    escape(*descriptor);
                bool keyIsOneArgument = written && !keyWritten.isWritten && change.key && change.key->kind() == BuiltinOperand::Kind::Argument && change.key->argumentIndex() < arguments.size();
                ValueID keyOfDefinition = keyIsOneArgument ? arguments[change.key->argumentIndex()] : ValueID();
                if (ValueID described = keyOfDefinition ? objectDescribedBy(written, keyOfDefinition) : map ? objectDescribedBy(map, { }) : ValueID())
                    noteCopyOfMembers(subject, listOf(described), true);
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
                    m_heirsMadeByBuiltins.append(HeirMadeByBuiltinInScan { site.index(), static_cast<uint32_t>(m_summary.changes.size()), static_cast<uint32_t>(m_sinks.size()), prototype, mode == BuiltinCallMode::Call && !call.passesList && arguments.size() == 1, mode == BuiltinCallMode::Call && !call.passesList && arguments.size() == 2 ? arguments[1] : ValueID() });
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

static bool isFilledAfterBodyReturns(const Program& program, VariableID id)
{
    const Variable& variable = program.at(id);
    if (!variable.module || variable.initialization != VariableInitialization::Uninitialized)
        return false;
    const Module& module = program.at(variable.module);
    return loaderFillsVariablesOffValueOf(module) && std::ranges::contains(module.syntheticExports, id, &SyntheticExport::variable);
}

void ScanOfBody::noteRecordOf(VariableID variable)
{
    ModuleID module = m_program.at(variable).module;
    if (module != m_function->module && !isFilledAfterBodyReturns(m_program, variable))
        noteRecordOf(module);
}

bool ScanOfBody::listsDefinitions() const
{
    if (!m_function->isModuleBody() || !m_function->module || m_function->isCoroutine())
        return false;
    const Module& module = m_program.at(m_function->module);
    return module.body == m_id && loaderEntersBodyAtMostOnce(module);
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
    if (inputPassThroughOpcodes.contains(opcode))
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
        m_valuesMadeForVariables.append(ValueMadeForVariableInScan { value.id().index(), noDefinition, { }, false, opcode == Opcode::NewClosure });
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
        m_summary.definitions.append(SummaryDefinition { made.variable, ValueID(made.made), ValueID(made.store), made.makesClosure });
    }
}

static bool classStaysInVariable(const Program& program, VariableID variable)
{
    const Module& module = program.at(program.at(variable).module);
    return variable != module.exportsVariable && std::ranges::none_of(module.syntheticExports, [&](const SyntheticExport& synthetic) {
        return synthetic.variable == variable;
    });
}

bool isBodyOfWrappedModule(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.syntax.contains(FunctionSyntax::IsWrappedModuleBody) || function.flags.containsAny({ FunctionFlags::TakesCallee, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return false;
    FunctionID caller = program.at(function.module).body;
    return caller && !program.at(caller).syntax.contains(FunctionSyntax::MayRunAsOriginal);
}

bool ScanOfBody::listsFirstStoresOfBody() const
{
    if (!m_function->module)
        return false;
    const Module& module = m_program.at(m_function->module);
    return (module.body == m_id || isBodyOfWrappedModule(m_program, m_id)) && isEvaluatedAtMostOnceOf(module.kind) && !hasExportTableOf(module.kind)
        && !module.flags.containsAny({ ModuleFlags::MayBeReevaluated, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked });
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
    if (above == noDefinition) {
        above = source;
        return;
    }
    uint32_t lower = source;
    while (lower > above && m_targetsWhoseDominatorStandsAbove[lower] != to) {
        m_targetsWhoseDominatorStandsAbove[lower] = to;
        lower = m_immediateDominators[lower];
    }
    if (lower <= above)
        above = nearestBlockAboveBoth(above, lower);
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
    sortWithoutRepeatsInScan(m_summary.methodsOfClasses);
    sortWithoutRepeatsInScan(m_summary.classesInVariables);
    sortWithoutRepeatsInScan(m_summary.unitsInVariables);
    for (size_t i = 0; i < m_blocksOfStoresOnPrototypes.size(); ++i) {
        SummaryStoreOnPrototype& store = m_summary.storesOnPrototypes[i];
        uint32_t block = m_blocksOfStoresOnPrototypes[i];
        store.isOnEveryPath = everyWayComesThrough.quickGet(block) && (m_innermostLoopHeads.isEmpty() || m_innermostLoopHeads[block] == noLoop);
        store.position = placeAmongThoseBlocks(PlaceInBlockInScan { 0, store.position, m_blocksOfStoresOnPrototypes[i] });
    }
    std::ranges::sort(m_summary.storesOnPrototypes, { }, &SummaryStoreOnPrototype::site);
    for (const PlaceInBlockInScan& use : m_usesOfVariablesByBodyOfModule)
        m_summary.firstUsesOfVariables.append(SummaryPlaceInBody { use.number, placeAmongThoseBlocks(use) });
    for (const PlaceInBlockInScan& use : m_usesByBodyOfModule)
        m_summary.firstUsesOfFunctions.append(SummaryPlaceInBody { use.number, placeAmongThoseBlocks(use) });
    for (Vector<SummaryPlaceInBody>* places : { &m_summary.firstStoresOfBody, &m_summary.firstUsesOfFunctions, &m_summary.firstUsesOfVariables }) {
        std::ranges::sort(*places);
        auto repeated = std::ranges::unique(*places, { }, &SummaryPlaceInBody::number);
        places->shrink(places->size() - repeated.size());
    }
}

FunctionID ScanOfBody::functionMadeThroughCasts(ValueID operand) const
{
    BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
    return definition && definition.opcode() == Opcode::NewClosure ? definition.as<Opcode::NewClosure>().function() : FunctionID();
}

FunctionID ScanOfBody::classConstructorThroughCasts(ValueID operand) const
{
    BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
    return definition && definition.opcode() == Opcode::NewClass ? functionMadeThroughCasts(definition.operand(0)) : FunctionID();
}

VariableID ScanOfBody::variableReadThroughCasts(ValueID operand) const
{
    BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
    return definition && definition.opcode() == Opcode::GetModuleVariable ? definition.as<Opcode::GetModuleVariable>().variable() : VariableID();
}

std::optional<int64_t> ScanOfBody::integerReadInBodyOfModule(ValueID operand) const
{
    BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
    if (!definition)
        return std::nullopt;
    if (definition.opcode() == Opcode::Integer)
        return definition.as<Opcode::Integer>().integer();
    VariableID variable = variableReadThroughCasts(operand);
    auto found = variable ? m_integersInVariables.find(variable.index()) : m_integersInVariables.end();
    if (found == m_integersInVariables.end() || definition.id() < found->value.store)
        return std::nullopt;
    uint32_t block = m_blockBeingRead;
    while (block != noDefinition && block > found->value.blockOfStore)
        block = m_immediateDominators[block];
    return block == found->value.blockOfStore ? std::optional(found->value.integer) : std::nullopt;
}

WrittenUnit ScanOfBody::unitComputedInBodyOfModule(const BodyValue& definition) const
{
    static constexpr OpcodeSet integerOperators { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::Add, Opcode::Sub };
    if (!m_listsFirstStoresOfBody || !definition || !integerOperators.contains(definition.opcode()))
        return { };
    std::optional<int64_t> left = integerReadInBodyOfModule(definition.operand(0));
    std::optional<int64_t> right = definition.opcode() == Opcode::BitNot ? left : integerReadInBodyOfModule(definition.operand(1));
    if (!left || !right)
        return { };
    auto a = static_cast<int32_t>(*left);
    auto b = static_cast<int32_t>(*right);
    switch (definition.opcode()) {
    case Opcode::BitAnd:
        return WrittenUnit::integer(a & b);
    case Opcode::BitOr:
        return WrittenUnit::integer(a | b);
    case Opcode::BitXor:
        return WrittenUnit::integer(a ^ b);
    case Opcode::BitNot:
        return WrittenUnit::integer(~a);
    case Opcode::LShift:
        return WrittenUnit::integer(static_cast<int32_t>(static_cast<uint32_t>(a) << (b & 31)));
    case Opcode::RShift:
        return WrittenUnit::integer(a >> (b & 31));
    case Opcode::Add:
        return WrittenUnit::integer(*left + *right);
    case Opcode::Sub:
        return WrittenUnit::integer(*left - *right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ValueID ScanOfBody::prototypeReadFunction(const BodyValue& read) const
{
    if (read.opcode() != Opcode::PropGet || read.as<Opcode::PropGet>().key() != m_program.keyOf(BuiltinNames::prototype))
        return { };
    ValueID function = read.as<Opcode::PropGet>().objectOperand();
    if (functionMadeThroughCasts(function))
        return function;
    VariableID variable = variableReadThroughCasts(function);
    return variable && m_program.at(variable).module == m_function->module && m_program.at(variable).flags.contains(VariableFlags::IsImmutable) ? function : ValueID();
}

ValueID ScanOfBody::prototypeReadFunctionThroughCasts(ValueID operand) const
{
    BodyValue definition = operand ? definitionBehindCasts(operand) : BodyValue();
    return definition ? prototypeReadFunction(definition) : ValueID();
}

void ScanOfBody::noteStoreOnPrototype(const BodyValue& store, ValueID function)
{
    auto written = store.as<Opcode::PropSet>();
    m_summary.storesOnPrototypes.append(SummaryStoreOnPrototype { store.id(), m_position, variableReadThroughCasts(function), functionMadeThroughCasts(function), functionMadeThroughCasts(written.valueOperand()), written.key() });
    m_blocksOfStoresOnPrototypes.append(m_blockBeingRead);
}

void ScanOfBody::noteUseOfValueByBodyOfModule(ValueID operand)
{
    if (ValueID owner = prototypeReadFunctionThroughCasts(operand))
        operand = owner;
    FunctionID function = functionMadeThroughCasts(operand);
    if (!function)
        function = classConstructorThroughCasts(operand);
    if (function)
        noteUseByBodyOfModule(function);
    if (VariableID variable = variableReadThroughCasts(operand))
        m_usesOfVariablesByBodyOfModule.append(PlaceInBlockInScan { variable.index(), m_position, m_blockBeingRead });
}

void ScanOfBody::lookForFirstStoresAndUses(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    ValueID classDefined;
    if (opcode == Opcode::PutModuleVariable) {
        VariableID variable = value.as<Opcode::PutModuleVariable>().variable();
        if (variable && m_program.at(variable).module == m_function->module) {
            m_storesOfBodyOfModule.append(PlaceInBlockInScan { variable.index(), m_position, m_blockBeingRead });
            BodyValue stored = m_program.at(variable).flags.contains(VariableFlags::IsImmutable) ? definitionBehindCasts(value.operand(0)) : BodyValue();
            WrittenUnit unit = unitWrittenInScan(stored);
            if (!unit)
                unit = unitComputedInBodyOfModule(stored);
            if (unit)
                m_summary.unitsInVariables.append(SummaryUnitInVariable { variable, unit.bits() });
            if (unit.kind() == WrittenUnit::Kind::Integer)
                m_integersInVariables.add(variable.index(), IntegerInVariable { unit.integer(), value.id(), m_blockBeingRead });
            FunctionID held = functionMadeThroughCasts(value.operand(0));
            if (!held)
                held = classConstructorThroughCasts(value.operand(0));
            if (held && classStaysInVariable(m_program, variable)) {
                m_summary.classesInVariables.append(SummaryClassInVariable { variable, held });
                return;
            }
        }
    } else if (opcode == Opcode::DefineMethod) {
        FunctionID constructor = classConstructorThroughCasts(value.operand(0));
        if (FunctionID method = constructor ? functionMadeThroughCasts(value.operand(1)) : FunctionID()) {
            m_summary.methodsOfClasses.append(SummaryMethodOfClass { constructor, method });
            classDefined = value.operand(0);
        }
    } else if (opcode == Opcode::GetModuleVariable) {
        if (VariableID variable = value.as<Opcode::GetModuleVariable>().variable()) {
            if (m_readsNotedOfBodyOfModule.add(static_cast<uint64_t>(variable.index()) << 32 | m_blockBeingRead).isNewEntry)
                m_readsOfBodyOfModule.append(PlaceInBlockInScan { variable.index(), m_position, m_blockBeingRead });
        }
    } else if (opcode == Opcode::PropGet) {
        if (prototypeReadFunction(value))
            return;
    } else if (opcode == Opcode::PropSet) {
        auto store = value.as<Opcode::PropSet>();
        ValueID function = prototypeReadFunctionThroughCasts(store.objectOperand());
        BodyValue stored = function && !m_program.isIndexLike(store.key()) ? definitionBehindCasts(store.valueOperand()) : BodyValue();
        if (stored && (stored.opcode() == Opcode::NewClosure || primitiveConstants.contains(stored.opcode()))) {
            noteStoreOnPrototype(value, function);
            return;
        }
    }
    if (raisesOf(opcode) == ProgramEvent::EvaluatesText) {
        m_listsFirstStoresOfBody = false;
        return;
    }
    if (inputPassThroughOpcodes.contains(opcode))
        return;
    uint32_t firstOperandUsed = classDefined ? 2 : opcode == Opcode::NewClass ? 1 : 0;
    value.forEachOperand([&](ValueID operand, uint32_t place) {
        if (operand && place >= firstOperandUsed)
            noteUseByBodyOfModule(operand);
    });
}

bool ScanOfBody::answersWithResultOfBody(const BodyValue& value) const
{
    if (value.opcode() != Opcode::RequireModule)
        return false;
    auto require = value.as<Opcode::RequireModule>();
    return !require.onlyIfImported() && require.module() && requireAnswersWithResultOfBody(m_program, require.referrer(), require.module());
}

template<typename Holds>
static constexpr TerminatorKindSet terminatorKindsWhereInScan(const Holds& holds)
{
    TerminatorKindSet kinds;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (holds(kind))
            kinds.add(kind);
    }
    return kinds;
}

static constexpr TerminatorKindSet terminatorKindsThatNameRecordsInScan = terminatorKindsWhereInScan([](TerminatorKind kind) {
    return caseOfForEachReferenceOfBodyTerminator(kind) != caseOfForEachReferenceOfBodyTerminator(TerminatorKind::Jump);
});
static constexpr TerminatorKindSet terminatorKindsWithoutEffectsInScan = terminatorKindsWhereInScan([](TerminatorKind kind) {
    return declaredBoundOf(kind).isPure();
});

ALWAYS_INLINE const uint32_t* ScanOfBody::readTerminator(const BodyTerminator& terminator, uint32_t block)
{
    TerminatorKind kind = terminator.kind();
    if (terminatorKindsThatNameRecordsInScan.contains(kind)) {
        ReferencesOfScan references { *this, ValueID(), block, false, false, kind == TerminatorKind::ReturnCall };
        terminator.forEachReference(references);
        if (kind == TerminatorKind::ReturnCall) {
            for (TailCast cast : terminator.as<TerminatorKind::ReturnCall>().casts()) {
                if (cast.declared.has(PositionFlags::IsInt32Promise))
                    m_summary.traits.add(SummaryTrait::HoldsInt32Promise);
                if (cast.declared.written() && !cast.declared.isExpected())
                    noteSetOfLayoutsWritten(cast.declared.written());
            }
        }
        if (references.namesUndefinedRecord) {
            stopAtUndefinedRecord(block, ValueID(), m_function->locationOf(terminator), nameOf(kind));
            return terminator.end();
        }
    }

    if (kind == TerminatorKind::SwitchString) {
        m_summary.weightOfOperations += terminator.numberOfTargets() * 2;
        for (StringID written : terminator.as<TerminatorKind::SwitchString>().strings())
            noteTextThatMayNameEvent(written);
    } else if (kind == TerminatorKind::Switch)
        m_summary.weightOfOperations += terminator.numberOfTargets() / 2;
    bool mayThrow = false;
    if (terminatorKindsWithoutEffectsInScan.contains(kind))
        noteEndOfBlock(terminator, block, noEffectsInScan);
    else {
        Effects effects = declaredEffectsOfTerminator(m_program, terminator);
        noteEndOfBlock(terminator, block, effects);
        m_summary.ownEffects.merge(effects);
        mayThrow = effects.mayThrow;
    }

    if (DataID literal = kind == TerminatorKind::Return ? findArrayLiteral(terminator.operand(0)) : DataID())
        m_elementStores.literalResults.append({ m_id, literal });
    m_someBlockReturns |= kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall;
    if (m_listsFirstStoresOfBody && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall) && m_immediateDominators[block] != noDefinition)
        m_blockAboveReturns = m_blockAboveReturns == noDefinition ? block : nearestBlockAboveBoth(m_blockAboveReturns, block);
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        noteUseByBodyOfModule(operand);
        noteTakerOfValueMadeForVariable(operand);
        constexpr OperandUses usesThatFlowNowhereFromTerminator {
            OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents,
            OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
            OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::PassesToUserCode, OperandUse::AppliesTransitively,
            OperandUse::AppliesToContents,
        };
        static_assert(everyOperandUseIsInExactlyOneOf({ OperandUse::Inspects, OperandUse::Returns, OperandUse::Retains, usesThatFlowNowhereFromTerminator }));
        OperandUses uses = operandUsesOf(kind, operandIndex);
        if (uses != OperandUses { OperandUse::Inspects })
            noteLeaving(operand, kind == TerminatorKind::Return && !m_function->isCoroutine() ? WayOfLeavingInScan::IsReturned : WayOfLeavingInScan::IsSeenFromOutside);
        if (uses.contains(OperandUse::Returns))
            returnValue(operand, block);
        if (uses.contains(OperandUse::Retains)) {
            if (mayThrow)
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
        meetFillsWhereObjectLeaves(objectBeingMade(m_thisBeingMade), fillsOf(m_thisBeingMade, m_fills));
        meetFillsWhereConstructionEnds(m_thisBeingMade);
    }

    BodyValue test;
    Proof proof = provesOf(kind);
    if (terminator.isConditionalBranch()) {
        ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
        test = condition ? definitionOf(condition.index()) : BodyValue();
        proof = test ? provesOf(test.opcode()) : Proof::None;
    }
    std::optional<uint32_t> targetWithoutParent = findTargetWithoutParent(test);
    std::optional<uint32_t> onlyTarget;
    if (m_followsConstants && goesByOneOperand(kind) && terminator.operand(0))
        onlyTarget = onlyTargetWith(m_program, terminator, primitiveHeldBy(terminator.operand(0)));
    uint32_t targetIndex = 0;
    const uint32_t* endOfTargets = nullptr;
    for (BodyTarget target : terminator.targets()) {
        endOfTargets = target.end();
        if (onlyTarget && *onlyTarget != targetIndex) {
            ++targetIndex;
            continue;
        }
        SetForScope fillsOnThisWay(m_fills, m_fills);
        if (targetWithoutParent == targetIndex) {
            BodyValue asked = test;
            while (asked.opcode() != Opcode::TypeTest)
                asked = definitionOf(asked.operand(0).index());
            m_parentTestedForNull = m_roots[asked.operand(0).index()];
            m_fills = m_setsOfFills.with(m_fills, FillInScan { m_thisBeingMade, true, m_parentConstructor.index() });
        }
        readArgumentsOf(block, target);
        carryExclusionsOver(target.block().index(), targetIndex, test);
        uint32_t position = static_cast<uint32_t>(target.arguments().size());
        for (uint32_t operandIndex : target.proofs()) {
            ValueID subject;
            if (proof != Proof::None)
                subject = test ? test.operand(operandIndex) : terminator.operand(operandIndex);
            TypeID passes;
            if (subject && test && test.opcode() == Opcode::TypeTest) {
                TypeID tested = test.as<Opcode::TypeTest>().testedType();
                passes = targetIndex ? TypeTable::anythingBut(tested) : tested;
            }
            if (subject && test && test.opcode() == Opcode::TypeTest && !holdsOnlyPrimitives(test.as<Opcode::TypeTest>().testedType())) [[unlikely]]
                noteViewsAfterObjectTest(target, position, subject);
            if (SummaryCall* madeAnswer = passes == TypeTable::object ? findCallThatMade(subject) : nullptr)
                m_answerTestedForObject = { target.block().index(), position, madeAnswer->site, { } };
            if (subject)
                handToBlock(subject, block, target.block().index(), position, KindOfEdgeInScan::Plain);
            m_incoming.append(IncomingInScan { target.block().index(), position, subject ? listOf(subject) : listOfNode(0), passes });
            ++position;
        }
        ++targetIndex;
    }
    return endOfTargets ? endOfTargets : terminator.end();
}

void ScanOfBody::readExportsForFacade(const BodyValue& evaluation)
{
    auto evaluated = evaluation.as<Opcode::EvaluateModule>();
    const Module& facade = m_program.at(evaluated.referrer());
    if (facade.body != m_id || commonJsModuleCapturedBy(m_program, facade) != evaluated.module())
        return;
    if (!refusedCodeOf(m_program).codeOutsideImage)
        return;
    uint32_t exports = variableNode(m_program.at(evaluated.module()).exportsVariable);
    uint32_t undefined = constant(TypeTable::undefined);
    for (const SyntheticExport& entry : facade.syntheticExports) {
        if (!entry.variable)
            continue;
        m_summary.namesThatLoaderTakes.append({ undefined, entry.variable });
        if (entry.name == m_program.stringOf(BuiltinNames::defaultKeyword))
            m_summary.namesThatLoaderTakes.append({ exports, entry.variable });
        std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = entry.name });
        m_summary.namesThatLoaderTakes.append({ key ? addRead(evaluation.id(), listOfNode(exports), *key) : 0, entry.variable });
    }
}

void ScanOfBody::readNamesThatLoaderTakesOff(ValueID returned)
{
    const Module& module = m_program.at(m_function->module);
    if (module.body != m_id || module.exportRule != SyntheticExportRule::RuntimeModule || module.requireResult != RequireResult::DefaultExport)
        return;
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (!entry.variable)
            continue;
        std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = entry.name });
        m_summary.namesThatLoaderTakes.append({ key ? addRead(returned, listOf(returned), *key) : 0, entry.variable });
        if (entry.name == m_program.stringOf(BuiltinNames::defaultKeyword))
            m_summary.namesThatLoaderTakes.append({ resultNode(0), entry.variable });
    }
}

void ScanOfBody::readWhatHostLooksFor(ValueID site)
{
    const Module& module = m_program.at(m_function->module);
    if (module.body != m_id || !m_summary.namesThatHostLooksFor.isEmpty())
        return;
    if (m_program.admitsReplacedBuiltins || m_program.admitsInspector || !refusedCodeOf(m_program).codeOutsideImage)
        return;
    auto entry = std::ranges::find(m_program.entries, m_function->module, &Entry::module);
    if (entry == m_program.entries.end() || entry->hostReads.isEmpty())
        return;
    StringID nameOfDefault = m_program.stringOf(BuiltinNames::defaultKeyword);
    auto keyOf = [&](StringID name) {
        return m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name });
    };
    auto hasKey = [&](StringID name) {
        return keyOf(name).has_value();
    };
    bool looksForNamesAlone = std::ranges::all_of(entry->hostReads, [&](const HostRead& read) {
        return hasKey(read.exportName) && (read.exportName != nameOfDefault || !read.namesLookedFor.isEmpty()) && std::ranges::all_of(read.namesLookedFor, hasKey);
    }) && std::ranges::all_of(module.requests, [&](const ModuleRequest& request) {
        return !request.isDynamicStarExport || commonJsModuleCapturedBy(m_program, m_program.at(request.target));
    });
    bool isCommonJs = module.kind == ModuleKind::CommonJs && module.exportsVariable;
    LayoutID namespaceOfEntry = m_program.layoutOfWholeNamespace(m_function->module);
    if (!looksForNamesAlone || (!isCommonJs && !namespaceOfEntry))
        return;

    auto lookAt = [&](uint32_t found, const HostRead& read) {
        if (read.namesLookedFor.isEmpty())
            m_summary.namesThatHostLooksFor.append(found);
        for (StringID name : read.namesLookedFor)
            m_summary.namesThatHostLooksFor.append(addRead(site, listOfNode(found), *keyOf(name)));
    };
    for (const HostRead& read : entry->hostReads) {
        PropertyKeyID key = *keyOf(read.exportName);
        if (isCommonJs) {
            uint32_t exports = variableNode(module.exportsVariable);
            if (read.exportName == nameOfDefault)
                lookAt(exports, read);
            lookAt(addRead(site, listOfNode(exports), key), read);
            continue;
        }
        if (auto listed = std::ranges::find(module.exports, read.exportName, &ModuleExport::name); listed != module.exports.end()) {
            if (listed->kind == ModuleExportKind::Variable && listed->variable) {
                lookAt(variableNode(listed->variable), read);
                layCallThatHostMakes(listed->variable, read);
            } else
                lookAt(addRead(site, listOfNode(nodeFor(SummaryNode::Kind::Allocation, namespaceOfEntry.index())), key), read);
            continue;
        }
        if (read.exportName == nameOfDefault)
            continue;
        for (const ModuleRequest& request : module.requests) {
            if (request.isDynamicStarExport)
                lookAt(addRead(site, listOfNode(variableNode(m_program.at(commonJsModuleCapturedBy(m_program, m_program.at(request.target))).exportsVariable)), key), read);
        }
    }
    if (m_summary.namesThatHostLooksFor.isEmpty())
        m_summary.namesThatHostLooksFor.append(constant(TypeTable::undefined));
}

void ScanOfBody::layCallThatHostMakes(VariableID variable, const HostRead& read)
{
    if (!read.builtin || read.parameter)
        return;
    ValueID store;
    for (auto [stored, site] : m_storesIntoImmutableVariables) {
        if (stored != variable)
            continue;
        if (store)
            return;
        store = site;
    }
    if (!store)
        return;
    BodyValue finished = definitionOf(definitionOf(store.index()).as<Opcode::PutModuleVariable>().valueOperand().index());
    bool isLiteral = finished && finished.opcode() == Opcode::FinishObject;
    ValueID literal = isLiteral ? finished.operand(0) : ValueID();
    BodyValue made = literal ? definitionOf(literal.index()) : BodyValue();
    if (isLiteral && (!made || made.opcode() != Opcode::NewObject || !made.as<Opcode::NewObject>().layout()))
        return;
    auto isLookedFor = [&](PropertyKeyID written) {
        return std::ranges::any_of(read.namesLookedFor, [&](StringID name) {
            return m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name }) == written;
        });
    };
    bool writesNameLookedFor = !isLiteral;
    if (isLiteral) {
        for (uint32_t field : made.as<Opcode::NewObject>().fields())
            writesNameLookedFor |= isLookedFor(m_program.at(made.as<Opcode::NewObject>().layout()).fields[field].key);
    }
    for (uint32_t number = literal.index() + 1; isLiteral && number < finished.id().index(); ++number) {
        BodyValue part = definitionOf(number);
        if (part && part.opcode() == Opcode::PropDefine && part.operand(0) == literal)
            writesNameLookedFor |= isLookedFor(part.as<Opcode::PropDefine>().key());
    }
    static constexpr OptionSet<ModuleFlags> flagsOfModulesThatOthersFind {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::IsImportedAsNamespace, ModuleFlags::LoaderReturnsNamespace,
        ModuleFlags::IsListedInCache, ModuleFlags::IsExposedByRequireCache, ModuleFlags::IsExposedByTestRunner, ModuleFlags::IsChangedOnDiskByRun,
        ModuleFlags::IsImportableDuringEvaluation, ModuleFlags::MayBeReevaluated, ModuleFlags::IsLoadedByLoader, ModuleFlags::IsRequiredDuringLoad,
    };
    const Module& module = m_program.at(m_function->module);
    if (!writesNameLookedFor || module.flags.containsAny(flagsOfModulesThatOthersFind) || std::ranges::any_of(module.requests, &ModuleRequest::isDynamicStarExport))
        return;
    for (const Entry& entry : m_program.entries) {
        if (entry.module != m_function->module)
            continue;
        for (const HostRead& other : entry.hostReads) {
            if (other.exportName != read.exportName && std::ranges::contains(module.exports, other.exportName, &ModuleExport::name))
                return;
        }
    }
    for (const Module& other : m_program.modules) {
        if (std::ranges::contains(other.requests, m_function->module, &ModuleRequest::target))
            return;
    }

    SummaryCall call;
    call.site = store;
    call.callee = SummaryCall::Callee::Builtin;
    call.builtin = read.builtin;
    call.isMadeByHost = true;
    call.passesLiteral = isLiteral;
    call.numberOfArguments = 1;
    m_callThatHostMakes = std::pair { call, variable };
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
            if (value) {
                ListInScan returned = listOf(value);
                flowInto(returned, resultNode(result));
            }
            ++result;
        }
        return;
    }
    ListInScan returned = listOf(operand);
    flowInto(returned, resultNode(0));
    readNamesThatLoaderTakesOff(operand);
    readWhatHostLooksFor(operand);
    if (!definition || effectRuleOf(definition.opcode()) != EffectRule::Call)
        return;
    std::optional<uint32_t> call = findCall(operand);
    for (uint32_t result = 1; call && result < m_function->results.size(); ++result)
        addEdge(nodeFor(SummaryNode::Kind::ResultOfCall, *call, result), resultNode(result));
}

void ScanOfBody::readWhatNobodyAskedFor()
{
    size_t nextHeritage = 0;
    for (uint32_t read : m_readsNotYetNeeded) {
        bool isReadBySource = false;
        for (; nextHeritage < m_heritages.size() && m_heritages[nextHeritage].readOffHeritage.index() == read; ++nextHeritage)
            isReadBySource = true;
        if (!(m_uses[read] & isCalledByName) && !isReadBySource)
            readByName(read);
    }
    RELEASE_ASSERT(nextHeritage == m_heritages.size());
    for (auto [list, block] : m_returnsOfCoroutine)
        returnFromCoroutine(list, block);
}

void ScanOfBody::resolveLiteralArguments()
{
    size_t next = 0;
    for (auto [place, literal] : m_literalArguments) {
        while (next < m_summary.calls.size() && m_summary.calls[next].firstArgument + m_summary.calls[next].numberOfArguments <= place)
            ++next;
        if (next < m_summary.calls.size() && m_summary.calls[next].firstArgument <= place && !m_summary.calls[next].passesList)
            m_elementStores.literalArguments.append({ m_id, m_summary.calls[next].site, place - m_summary.calls[next].firstArgument, literal });
    }
    m_literalArguments.shrink(0);
}

DataID ScanOfBody::findArrayLiteral(ValueID value) const
{
    BodyValue literal = value ? definitionBehindCasts(value) : BodyValue();
    DataID data = literal && literal.opcode() == Opcode::Data ? literal.as<Opcode::Data>().data() : DataID();
    const auto* array = data ? m_program.at(data).root.tryAs<DataNode::Array>() : nullptr;
    return array && !array->elements.isEmpty() ? data : DataID();
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
    for (auto [list, blockOfStore] : m_storesIntoConstantLists) {
        if (m_innermostLoopHeads[blockOfStore] != noLoop && m_innermostLoopHeads[blockOfStore] != m_innermostLoopHeads[m_blocks[list.index()]])
            m_summary.constantListsStoredIntoInLoops.append(list);
    }
    std::ranges::sort(m_summary.constantListsStoredIntoInLoops, { }, &ValueID::index);
    m_summary.constantListsStoredIntoInLoops.shrink(std::ranges::unique(m_summary.constantListsStoredIntoInLoops).begin() - m_summary.constantListsStoredIntoInLoops.begin());
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

SummaryCall* ScanOfBody::findCallThatMade(ValueID value)
{
    BodyValue made = definitionBehindCasts(value);
    if (!made || (made.opcode() != Opcode::Call && made.opcode() != Opcode::CallDirect))
        return nullptr;
    auto record = std::ranges::lower_bound(m_summary.calls, made.id(), { }, &SummaryCall::site);
    return record != m_summary.calls.end() && record->site == made.id() ? &*record : nullptr;
}

void ScanOfBody::noteIfParameterIsReadAsKey(ValueID key, uint16_t& parameters)
{
    uint32_t entryParameter = m_roots[key.index()];
    if (entryParameter < std::min<uint32_t>(m_function->numberOfEntryParameters(), 16))
        parameters |= 1u << entryParameter;
}

void ScanOfBody::noteResultReadAsKey(SummaryCall& call)
{
    call.resultIsReadAsKey = true;
    m_summary.someResultIsReadAsKey = true;
}

PropertyKeyID ScanOfBody::keySpelledBy(const BodyValue& definition) const
{
    if (!definition)
        return { };
    if (definition.opcode() == Opcode::String)
        return keyWrittenAs(definition.id()).key;
    if (definition.opcode() == Opcode::Builtin)
        return keyOfWellKnownSymbol(m_program, m_program.builtins().canonicalID(definition.as<Opcode::Builtin>().builtin()));
    if (definition.opcode() != Opcode::Call)
        return { };
    auto record = std::ranges::lower_bound(m_summary.calls, definition.id(), { }, &SummaryCall::site);
    return record != m_summary.calls.end() && record->site == definition.id() ? keyOfSymbolMadeOnceBy(m_program, *record) : PropertyKeyID();
}

void ScanOfBody::nameKeysOfMembers()
{
    auto addLiteralArray = [&](LiteralArrayLocation place, ValueID value) {
        BodyValue made = value ? definitionBehindCasts(value) : BodyValue();
        const auto* written = made && made.opcode() == Opcode::Data && made.as<Opcode::Data>().data() ? m_program.at(made.as<Opcode::Data>().data()).root.tryAs<DataNode::Array>() : nullptr;
        bool holdsStrings = written && isShortStringList(*written);
        bool isWhole = written && (holdsStrings || isStorageOfArray(*written)) && std::ranges::none_of(written->elements, [](const DataNode& element) {
            return element.is<DataNode::Hole>();
        });
        if (ArrayStorage storage = isWhole ? arrayStorageOfData(m_program, *written) : ArrayStorage::Other; isNumericStorage(storage) || (holdsStrings && storage == ArrayStorage::Contiguous))
            m_summary.writtenListsOfVariables.append({ place, made.as<Opcode::Data>().data(), made.id(), static_cast<uint32_t>(written->elements.size()), static_cast<uint8_t>(storage) });
    };
    for (auto [variable, value] : m_valuesOfImmutableVariables) {
        if (PropertyKeyID key = value ? keySpelledBy(definitionBehindCasts(value)) : PropertyKeyID())
            m_keysOfVariables.append({ variable, key });
        addLiteralArray(LiteralArrayLocation::of(variable), value);
        if (BodyValue literal = value ? definitionBehindCasts(value) : BodyValue(); literal && literal.opcode() == Opcode::Data && literal.as<Opcode::Data>().data())
            m_literalsOfVariables.append({ variable, literal.as<Opcode::Data>().data() });
    }
    for (auto [field, value] : m_immutableFieldValues)
        addLiteralArray(LiteralArrayLocation::of(field), value);
    std::ranges::sort(m_summary.writtenListsOfVariables, { }, &SummaryWrittenListOfVariable::place);
    for (auto [member, written] : m_keysWrittenForMembers) {
        BodyValue definition = definitionBehindCasts(written);
        if (definition && definition.opcode() == Opcode::ToPropertyKey && definition.operand(0))
            definition = definitionBehindCasts(definition.operand(0));
        if (definition && definition.opcode() == Opcode::GetModuleVariable)
            m_membersUnderVariables.append({ m_id, member, definition.as<Opcode::GetModuleVariable>().variable() });
        else
            m_summary.members[member].key = keySpelledBy(definition);
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
    for (IncomingInScan& incoming : m_incoming) {
        incoming.parameter = parameterTakingIncoming(incoming).value_or(IncomingInScan::noParameter);
        if (incoming.parameter != IncomingInScan::noParameter)
            ++m_firstIncoming[incoming.parameter + 2];
    }
    for (uint32_t parameter = 0; parameter < count; ++parameter)
        m_firstIncoming[parameter + 2] += m_firstIncoming[parameter + 1];
    m_incomingByParameter.resize(m_firstIncoming[count + 1]);
    for (uint32_t index = 0; index < m_incoming.size(); ++index) {
        uint32_t parameter = m_incoming[index].parameter;
        if (parameter != IncomingInScan::noParameter)
            m_incomingByParameter[m_firstIncoming[parameter + 1]++] = index;
    }
}

void ScanOfBody::markParametersThatSinksNeed()
{
    m_isNeeded.fill(false, m_numberOfParameters);
    m_worklist.shrink(0);
    const uint32_t* atoms = m_atoms.begin();
    auto need = [&](ListInScan list) {
        for (uint32_t place = list.first, end = list.first + list.count; place < end; ++place) {
            uint32_t atom = atoms[place];
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
        record.writtenDivisor = found.writtenDivisor;
        record.shiftsRightByConstant = found.shiftsRightByConstant;
        record.step = found.steppedField && !m_fieldsReadAsIntegers.contains(found.steppedField) ? 0 : found.step;
        record.steppedOperand = found.steppedOperand;
        record.result = found.joinOfResult ? found.joinOfResult : addNodeAskedForOnce({ SummaryNode::Kind::ResultOfOperator, found.record, 0 });
        m_summary.operators.append(record);
    }
}

void ScanOfBody::resolveParameters()
{
    uint32_t count = m_numberOfParameters;
    sortIncomingByParameter();
    if (!m_heritages.isEmpty())
        layHeritagesBySource();
    markParametersThatSinksNeed();

    m_resolved.fill(ListInScan { }, count);
    if (m_edgesLeadingBack.isEmpty()) {
        for (uint32_t parameter = 0; parameter < count; ++parameter) {
            if (m_isNeeded[parameter])
                resolveComponent(std::span<const uint32_t>(&parameter, 1), noComponent);
        }
        return;
    }

    m_order.fill(0, count);
    m_lowest.fill(0, count);
    m_componentOf.fill(noComponent, count);
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
                if (component != noComponent && m_componentOf[other] == component)
                    continue;
                ListInScan resolved = m_resolved[other];
                for (uint32_t index = 0; index < resolved.count; ++index)
                    m_gathered.append(behindTest(m_origins[resolved.first + index], incoming.passes));
            }
        }
    }
    if (m_gathered.size() > 1)
        normalize(m_gathered);

    if (m_gathered.size() > 1) {
        bool takesOnlyPrimitives = std::ranges::all_of(m_gathered, [&](const OriginInScan& origin) {
            return isAlwaysPrimitive(origin) || subjectOfExclusionIsPrimitive(origin.node, component);
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

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::carryExclusionsOver(uint32_t to, uint32_t targetIndex, const BodyValue& test)
{
    if (to <= m_blockBeingRead) {
        giveUpWhatEdgeLeadingBackDoesNotCarry(to);
        return;
    }
    uint32_t carried = m_exclusionInForce;
    if (test && test.opcode() == Opcode::StrictEq && targetIndex == 1) {
        for (uint32_t side = 0; side < 2; ++side) {
            ValueID written = test.operand(side), subject = test.operand(1 - side);
            if (!written || !subject)
                continue;
            BodyValue definition = definitionOf(written.index());
            uint16_t excluded = 0;
            if (definition && definition.opcode() == Opcode::Boolean)
                excluded = definition.as<Opcode::Boolean>().boolean() ? Primitive::True : Primitive::False;
            else if (definition && definition.opcode() == Opcode::Undefined)
                excluded = Primitive::Undefined;
            else if (definition && definition.opcode() == Opcode::Null)
                excluded = Primitive::Null;
            if (!excluded)
                continue;
            carried = addExclusion(subject, excluded, carried);
            break;
        }
    } else if (test && test.opcode() == Opcode::TypeTest && !targetIndex)
        carried = addExclusionOfObjectTest(test, carried);
    else if (test && allRelations.contains(test.opcode()))
        carried = addExclusionsOfRelation(test, !targetIndex, carried);
    if (carried == noExclusion)
        return;
    if (!std::exchange(m_someEntryCarries, true)) {
        m_numberOfEntriesThatCarry.fill(0, m_numberOfEdgesInto.size());
        m_whatEntriesShare.fill(noExclusion, m_numberOfEdgesInto.size());
    }
    m_whatEntriesShare[to] = m_numberOfEntriesThatCarry[to]++ ? meetOfExclusions(m_whatEntriesShare[to], carried) : carried;
}

void ScanOfBody::noteViewsAfterObjectTest(BodyTarget target, uint32_t position, ValueID subject)
{
    if (target.block().index() > m_blockBeingRead)
        m_viewsHanded.append({ target.block().index(), position, ValueID(m_roots[subject.index()]) });
}

void ScanOfBody::takeViewsHandedTo(BodyBlock block)
{
    uint32_t number = block.id().index();
    for (const ViewHandedInScan& view : m_viewsHanded) {
        if (view.block == number && m_numberOfEdgesInto[number] == 1 && view.position < block.numberOfParameters())
            m_valuesViewed.add(block.parameter(view.position).index(), view.viewed);
    }
    m_viewsHanded.removeAllMatching([&](const ViewHandedInScan& view) {
        return view.block <= number;
    });
}

ValueID ScanOfBody::valueViewedAs(ValueID name)
{
    name = ValueID(m_roots[name.index()]);
    while (!m_valuesViewed.isEmpty()) {
        auto found = m_valuesViewed.find(name.index());
        if (found == m_valuesViewed.end())
            break;
        uint32_t block = m_blocks[name.index()];
        if (!std::ranges::contains(m_blocksWhoseViewsAreTested, block, &BlockOfViewsInScan::block))
            m_blocksWhoseViewsAreTested.append({ block, static_cast<uint32_t>(m_exclusions.size()) });
        name = found->value;
    }
    return name;
}

uint32_t ScanOfBody::addExclusionOfObjectTest(const BodyValue& test, uint32_t above)
{
    TypeID tested = test.as<Opcode::TypeTest>().testedType();
    ValueID subject = test.operand(0);
    if (!subject || !tested || holdsOnlyPrimitives(tested))
        return above;
    uint16_t excluded = Primitive::All & ~m_program.at(tested).primitives;
    return excluded ? addExclusion(valueViewedAs(subject), excluded, above, true) : above;
}

static Range integersThatRelationLeaves(Opcode relation, bool holds, int64_t constant)
{
    switch (relation) {
    case Opcode::Less:
        return holds ? Range { -maxTrackedInteger, constant - 1 } : Range { constant, maxTrackedInteger };
    case Opcode::LessEq:
        return holds ? Range { -maxTrackedInteger, constant } : Range { constant + 1, maxTrackedInteger };
    case Opcode::Greater:
        return holds ? Range { constant + 1, maxTrackedInteger } : Range { -maxTrackedInteger, constant };
    case Opcode::GreaterEq:
        return holds ? Range { constant, maxTrackedInteger } : Range { -maxTrackedInteger, constant - 1 };
    default:
        return Range::full();
    }
}

static Opcode relationWithOperandsSwapped(Opcode relation)
{
    switch (relation) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        return relation;
    }
}

uint32_t ScanOfBody::addExclusionsOfRelation(const BodyValue& relation, bool holds, uint32_t above)
{
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID written = relation.operand(side), subject = relation.operand(1 - side);
        BodyValue definition = written ? definitionOf(written.index()) : BodyValue();
        if (!subject || !definition || definition.opcode() != Opcode::Integer)
            continue;
        int64_t constant = definition.as<Opcode::Integer>().integer();
        if (std::abs(constant) >= maxExactInteger)
            continue;
        Range left = integersThatRelationLeaves(side ? relation.opcode() : relationWithOperandsSwapped(relation.opcode()), holds, constant);
        Range kept { left.minimum >= 0 ? 0 : -maxTrackedInteger, left.maximum <= INT32_MAX ? INT32_MAX : maxTrackedInteger };
        return kept == Range::full() ? above : addExclusion(subject, 0, above, false, kept);
    }
    return above;
}

uint32_t ScanOfBody::meetOfExclusions(uint32_t first, uint32_t second)
{
    uint32_t shared = sharedEndOfExclusions(first, second);
    if (first == shared || second == shared)
        return shared;
    const ExclusionInScan& lastOnFirst = m_exclusions[first];
    const ExclusionInScan& lastOnSecond = m_exclusions[second];
    if (!lastOnFirst.isOfObjectTest || !lastOnSecond.isOfObjectTest || lastOnFirst.subject != lastOnSecond.subject || lastOnFirst.letsEverythingThrough || lastOnSecond.letsEverythingThrough)
        return shared;
    uint16_t excluded = lastOnFirst.excludedInChain & lastOnSecond.excludedInChain;
    return excluded ? addExclusion(lastOnFirst.subject, excluded, shared, true) : shared;
}

uint32_t ScanOfBody::addExclusion(ValueID subject, uint16_t excluded, uint32_t above, bool isOfObjectTest, Range integersLeft)
{
    ExclusionInScan added { subject, excluded, integersLeft, above, 0, { }, false, false, isOfObjectTest, excluded, depthOfExclusion(above) + 1, noExclusion };
    if (isOfObjectTest && above != noExclusion && m_exclusions[above].isOfObjectTest && m_exclusions[above].subject == subject && !m_exclusions[above].letsEverythingThrough)
        added.excludedInChain |= m_exclusions[above].excludedInChain;
    uint32_t jump = jumpOfExclusion(above);
    if (above != noExclusion)
        added.jump = depthOfExclusion(above) - depthOfExclusion(jump) == depthOfExclusion(jump) - depthOfExclusion(jumpOfExclusion(jump)) ? jumpOfExclusion(jump) : above;
    m_exclusions.append(added);
    return m_exclusions.size() - 1;
}

uint32_t ScanOfBody::sharedEndOfExclusions(uint32_t first, uint32_t second) const
{
    auto climbTo = [&](uint32_t exclusion, uint32_t depth) {
        while (depthOfExclusion(exclusion) > depth)
            exclusion = depthOfExclusion(jumpOfExclusion(exclusion)) >= depth ? jumpOfExclusion(exclusion) : m_exclusions[exclusion].above;
        return exclusion;
    };
    first = climbTo(first, depthOfExclusion(second));
    second = climbTo(second, depthOfExclusion(first));
    while (first != second) {
        bool jumpsPart = jumpOfExclusion(first) != jumpOfExclusion(second);
        first = jumpsPart ? jumpOfExclusion(first) : m_exclusions[first].above;
        second = jumpsPart ? jumpOfExclusion(second) : m_exclusions[second].above;
    }
    return first;
}

ListInScan ScanOfBody::listUnderExclusionsOf(ValueID value)
{
    if (!m_listsUnderExclusions.isEmpty()) [[unlikely]] {
        if (auto underneath = m_listsUnderExclusions.find(value.index()); underneath != m_listsUnderExclusions.end())
            return underneath->value;
    }
    return listOf(value);
}

ALWAYS_INLINE void ScanOfBody::takeExclusionsAtBlock(uint32_t block)
{
    if (!m_someEntryCarries) [[likely]]
        return;
    uint32_t entries = m_numberOfEntriesThatCarry[block];
    uint32_t shared = m_whatEntriesShare[block];
    putInForce(entries == m_numberOfEdgesInto[block] ? shared : noExclusion);
    if (m_exclusionInForce != noExclusion)
        m_exclusionsTakenAtBlocks.append(ExclusionCarriedInScan { block, m_exclusionInForce });
}

void ScanOfBody::giveUpWhatEdgeLeadingBackDoesNotCarry(uint32_t to)
{
    for (const BlockOfViewsInScan& tested : m_blocksWhoseViewsAreTested) {
        for (uint32_t number = tested.block == to ? tested.firstExclusion : static_cast<uint32_t>(m_exclusions.size()); number < m_exclusions.size(); ++number) {
            if (m_exclusions[number].isOfObjectTest)
                giveUpExclusion(number);
        }
    }
    if (m_exclusionsTakenAtBlocks.isEmpty()) [[likely]]
        return;
    auto taken = std::ranges::lower_bound(m_exclusionsTakenAtBlocks, to, { }, &ExclusionCarriedInScan::block);
    if (taken == m_exclusionsTakenAtBlocks.end() || taken->block != to)
        return;
    uint32_t held = taken->exclusion, carried = sharedEndOfExclusions(taken->exclusion, m_exclusionInForce);
    taken->exclusion = carried;
    while (held != carried) {
        giveUpExclusion(held);
        held = m_exclusions[held].above;
    }
}

void ScanOfBody::giveUpExclusion(uint32_t number)
{
    ExclusionInScan& givenUp = m_exclusions[number];
    if (std::exchange(givenUp.letsEverythingThrough, true) || !givenUp.join)
        return;
    ListInScan underneath = givenUp.listUnderneath;
    uint32_t join = givenUp.join;
    if (underneath.count == isJoinNotYetNeeded)
        underneath = listOfNode(joinOfExclusion(underneath.first));
    addSink(SinkKindInScan::Edge, underneath, join);
}

void ScanOfBody::keepObjectOfReadPutOff(uint32_t read, ValueID object)
{
    if (m_listsUnderExclusions.isEmpty() || !object) [[likely]]
        return;
    if (m_listsUnderExclusions.contains(object.index()))
        m_objectsOfReadsPutOff.add(read, m_lists[object.index()]);
}

ALWAYS_INLINE_INTO_OTHER_FILES ListInScan ScanOfBody::objectOfReadPutOff(uint32_t read, ValueID object)
{
    if (!m_objectsOfReadsPutOff.isEmpty()) [[unlikely]] {
        if (auto kept = m_objectsOfReadsPutOff.find(read); kept != m_objectsOfReadsPutOff.end())
            return kept->value.count == isJoinNotYetNeeded ? listOfNode(joinOfExclusion(kept->value.first)) : kept->value;
    }
    if (!m_listsUnderExclusions.isEmpty()) [[unlikely]] {
        if (auto underneath = m_listsUnderExclusions.find(object.index()); underneath != m_listsUnderExclusions.end())
            return underneath->value;
    }
    return listOf(object);
}

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::putInForce(uint32_t exclusion)
{
    m_exclusionsToLay.shrink(0);
    while (m_exclusionInForce != exclusion) {
        if (liesBehind(m_exclusionInForce, exclusion)) {
            const ExclusionInScan& taken = m_exclusions[m_exclusionInForce];
            m_lists[taken.subject.index()] = taken.listUnderneath;
            if (taken.isOutermostOverSubject)
                m_listsUnderExclusions.remove(taken.subject.index());
            m_exclusionInForce = taken.above;
            continue;
        }
        m_exclusionsToLay.append(exclusion);
        exclusion = m_exclusions[exclusion].above;
    }
    while (!m_exclusionsToLay.isEmpty()) {
        uint32_t number = m_exclusionsToLay.takeLast();
        ValueID subject = m_exclusions[number].subject;
        ListInScan underneath = m_lists[subject.index()];
        if (underneath.count > isJoinNotYetNeeded)
            underneath = listOf(subject);
        m_exclusions[number].listUnderneath = underneath;
        m_exclusions[number].isOutermostOverSubject = m_listsUnderExclusions.add(subject.index(), underneath).isNewEntry;
        m_lists[subject.index()] = m_exclusions[number].join ? listOfNode(m_exclusions[number].join) : ListInScan { number, isJoinNotYetNeeded };
        m_exclusionInForce = number;
    }
}

uint32_t ScanOfBody::joinOfExclusion(uint32_t asked)
{
    Vector<uint32_t, 4> marks;
    for (uint32_t number = asked; !m_exclusions[number].join; number = m_exclusions[number].listUnderneath.first) {
        marks.append(number);
        if (m_exclusions[number].listUnderneath.count != isJoinNotYetNeeded)
            break;
    }
    while (!marks.isEmpty()) {
        uint32_t number = marks.takeLast();
        ListInScan underneath = m_exclusions[number].listUnderneath;
        if (underneath.count == isJoinNotYetNeeded)
            underneath = listOfNode(m_exclusions[underneath.first].join);
        uint32_t join = newJoin();
        if (m_exclusions[number].letsEverythingThrough)
            addSink(SinkKindInScan::Edge, underneath, join);
        else {
            addSink(SinkKindInScan::Edge, underneath, join, TypeTable::object);
            if (uint16_t left = Primitive::All & ~m_exclusions[number].excluded) {
                Range integers = left & Primitive::Integer ? m_exclusions[number].integersLeft : Range::empty();
                addSink(SinkKindInScan::Edge, underneath, join, *tryMakeTypeOfPrimitives(left, integers));
            }
        }
        m_exclusions[number].join = join;
        m_joinsOfExclusions.append({ join, underneath });
    }
    return m_exclusions[asked].join;
}

OriginInScan ScanOfBody::behindTestThatIsWritten(OriginInScan origin, TypeID passes)
{
    if (!origin.node)
        return { constant(passes), { } };
    if (TypeTable::passesEveryObject(passes)) {
        SummaryNode::Kind kind = m_summary.nodes[origin.node].kind;
        if (kind == SummaryNode::Kind::Allocation || kind == SummaryNode::Kind::ClassInstance || kind == SummaryNode::Kind::Closure || kind == SummaryNode::Kind::Builtin)
            return origin;
    }
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
        for (uint32_t index = 0; index < resolved.count; ++index)
            m_expanded.append(m_origins[resolved.first + index]);
    }
    if (list.count > 1)
        normalize(m_expanded);
}

uint32_t ScanOfBody::oneNodeForOrigins(ListInScan list)
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
    auto layEdgeFrom = [&](const SinkInScan& sink, OriginInScan origin) {
        if (sink.passes == TypeTable::object && isAlwaysPrimitive(origin))
            return;
        OriginInScan from = behindTest(origin, sink.passes);
        bool leavesNoEdge = !from.node || m_summary.nodes[from.node].kind == SummaryNode::Kind::Constant;
        if (sink.runsCodeOfValue && leavesNoEdge && !primitiveConstantAt(from.node))
            m_summary.ownEffects.merge(Effects::unknownCode());
        addEdge(from.node, sink.target, from.passes, sink.runsCodeOfValue);
    };
    for (const SinkInScan& sink : m_sinks) {
        switch (sink.kind) {
        case SinkKindInScan::PlaceOfWrittenList: {
            Vector<uint32_t>& places = m_summary.placesOfWrittenLists;
            expand(sink.value);
            if (m_expanded.isEmpty())
                break;
            places[sink.target] = static_cast<uint32_t>(places.size());
            places.append(static_cast<uint32_t>(m_expanded.size()));
            for (const OriginInScan& origin : m_expanded) {
                places.append(origin.node);
                places.append(origin.passes.bits());
            }
            break;
        }
        case SinkKindInScan::Edge: {
            uint32_t onlyAtom = sink.value.count == 1 ? m_atoms[sink.value.first] : parameterBit;
            if (!(onlyAtom & (parameterBit | operatorBit))) {
                layEdgeFrom(sink, OriginInScan { onlyAtom, { } });
                break;
            }
            expand(sink.value);
            for (const OriginInScan& origin : m_expanded)
                layEdgeFrom(sink, origin);
            break;
        }
        case SinkKindInScan::Argument:
            m_summary.arguments[sink.target] = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfCall:
            m_summary.calls[sink.target].subject = subjectNodeFor(sink.value);
            break;
        case SinkKindInScan::ReceiverOfCall:
            m_summary.calls[sink.target].receiver = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::NewTargetOfCall:
            m_summary.calls[sink.target].newTarget = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfAccess:
            m_summary.accesses[sink.target].subject = subjectNodeFor(sink.value);
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
            m_summary.changes[sink.target].subject = subjectNodeFor(sink.value);
            break;
        case SinkKindInScan::OtherOfChange:
            m_summary.changes[sink.target].other = m_summary.changes[sink.target].kind == SummaryChange::Kind::CopiesInto ? subjectNodeFor(sink.value) : oneNodeFor(sink.value);
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
        case SinkKindInScan::TargetOfCopyOfMembers:
            m_summary.copiesOfMembers[sink.target].target = subjectNodeFor(sink.value);
            break;
        case SinkKindInScan::SourceOfCopyOfMembers:
            m_summary.copiesOfMembers[sink.target].source = subjectNodeFor(sink.value);
            break;
        case SinkKindInScan::SubjectOfInspection:
            m_summary.inspections[sink.target].subject = oneNodeFor(sink.value);
            break;
        case SinkKindInScan::ConvertedToPrimitive:
            applyConversionToPrimitive(sink);
            break;
        case SinkKindInScan::OtherOperandOfEquality:
            break;
        case SinkKindInScan::ConvertedByEquality:
            applyConversionByEquality(m_equalities[sink.target]);
            break;
        }
    }
    for (const OperatorInScan& found : m_operators)
        applyOperator(found);
    for (const StoreInScan& store : m_stores)
        applyStore(store);
}

bool ScanOfBody::everyAccessorHalfIsWritten(uint32_t root) const
{
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> halves;
    for (const AccessorLaidInScan& laid : m_accessorsLaid) {
        if (laid.root == root && (!laid.function || !halves.add(static_cast<uint64_t>(laid.key.bits()) << 1 | laid.isGetter).isNewEntry))
            return false;
    }
    return true;
}

void ScanOfBody::defineAccessorsOfPrototype(uint32_t root, ListInScan prototype)
{
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> accessorsUnderKeys;
    for (const AccessorLaidInScan& laid : m_accessorsLaid) {
        if (laid.root != root)
            continue;
        SummaryChange& change = m_summary.changes[m_sinks[laid.sinkOfSubject].target];
        change.laysAccessorAsWritten = true;
        auto added = accessorsUnderKeys.add(laid.key.bits(), static_cast<uint32_t>(m_summary.writtenAccessors.size()));
        if (added.isNewEntry) {
            uint32_t access = addAccess(SummaryAccess::Kind::Define, change.site, prototype, laid.key);
            m_summary.accesses[access].isByDescriptor = true;
            m_summary.accesses[access].laysAccessorAsWritten = true;
            m_summary.accesses[access].value = 0;
            m_summary.writtenAccessors.append({ access, { }, { }, { } });
        }
        SummaryWrittenAccessor& written = m_summary.writtenAccessors[added.iterator->value];
        (laid.isGetter ? written.getter : written.setter) = laid.function;
    }
}

bool ScanOfBody::isMapUsedOnce(const HeirMadeByBuiltinInScan& heir) const
{
    if (!heir.map || !findCall(ValueID(heir.call)))
        return false;
    uint32_t root = m_roots[heir.map.index()];
    return holdsOnlyWhatItsBodyLaysInScan(m_program, definitionOf(root)) && m_firstLeft[root] == heir.call && m_lastLeft[root] == heir.call;
}

std::optional<uint32_t> ScanOfBody::findNestedLiteralSink(ValueID site) const
{
    for (uint32_t sink = 0; sink < m_sinks.size(); ++sink) {
        if (m_sinks[sink].kind != SinkKindInScan::SubjectOfChange)
            continue;
        if (const SummaryChange& change = m_summary.changes[m_sinks[sink].target]; change.site == site && change.kind == SummaryChange::Kind::ChangesPrototype)
            return sink;
    }
    return std::nullopt;
}

void ScanOfBody::followObjectsMadeToBePrototypes()
{
    for (const StoreOfPrototypeInScan& candidate : m_storesOfPrototypes) {
        uint32_t root = candidate.rootOfStored;
        uint32_t position = m_stores[candidate.store].site.index();
        BodyValue made = definitionOf(root);
        LiteralInParentInScan* literal = m_uses[root] & isLiteralInParent ? findLiteralInParent(root) : nullptr;
        std::optional<uint32_t> sinkOfLiteral = literal ? findNestedLiteralSink(literal->site) : std::nullopt;
        if (literal && !sinkOfLiteral)
            continue;
        bool isStoredOnlyHere = literal ? m_firstLeft[root] == literal->position && literal->firstLeftLater == position && literal->lastLeftLater == position : m_firstLeft[root] == position;
        if (!made || !isStoredOnlyHere || m_lastLeft[root] != position || m_uses[root] & (isAccessedBesideNamedStoresAndAccessors | isDefinedUnderComputedKeys | hasNullAboveByItsOwnText))
            continue;
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_stores[candidate.store].block];
        if (head != noLoop && head > m_blocks[root])
            continue;
        HeirMadeByBuiltinInScan* heir = nullptr;
        if (made.opcode() != Opcode::NewDictionary) {
            for (HeirMadeByBuiltinInScan& listed : m_heirsMadeByBuiltins) {
                if (listed.call == root && !listed.isFollowed && (listed.isMadeEmpty || isMapUsedOnce(listed)))
                    heir = &listed;
            }
            if (!heir)
                continue;
        }

        bool laysAccessors = m_uses[root] & isLaidAccessorsUnderNames;
        m_stores[candidate.store].storesObjectMadeForIt = !laysAccessors || everyAccessorHalfIsWritten(root);
        uint32_t standsForObject = addAccessUnder(SummaryAccess::Kind::Read, ValueID(root), m_stores[candidate.store].base, m_stores[candidate.store].key, ValueID());
        m_summary.accesses[standsForObject].standsForObjectMadeAtSite = true;
        ListInScan known = listOfNode(resultOfRead(standsForObject));
        for (StoreInScan& store : m_stores) {
            if (store.root != root)
                continue;
            store.base = known;
            store.isIntoObjectKnownByRead = true;
        }
        for (const AccessorLaidInScan& laid : laysAccessors ? m_accessorsLaid.span() : std::span<AccessorLaidInScan> { }) {
            if (laid.root == root)
                m_sinks[laid.sinkOfSubject].value = known;
        }
        if (laysAccessors && m_stores[candidate.store].storesObjectMadeForIt)
            defineAccessorsOfPrototype(root, known);
        if (literal) {
            m_sinks[*sinkOfLiteral].value = known;
            for (uint32_t sink = literal->firstSinkOfParent; sink < literal->endOfSinksOfParent; ++sink) {
                if (m_sinks[sink].kind == SinkKindInScan::OtherOfChange)
                    m_sinks[sink].value = known;
                else if (m_sinks[sink].kind == SinkKindInScan::Edge)
                    m_sinks[sink].value = { };
            }
        }
        if (heir) {
            heir->isFollowed = true;
            m_summary.changes[heir->change].kind = SummaryChange::Kind::ChangesPrototype;
            m_sinks[heir->sinkOfSubject].value = known;
            addSink(SinkKindInScan::OtherOfChange, heir->above, heir->change);
        }
        if (heir && heir->map) {
            ValueID site(heir->call);
            m_definitionsByMaps.append(DefinitionByMapInScan { static_cast<uint32_t>(m_summary.changes.size()), static_cast<uint32_t>(m_summary.accesses.size()), m_roots[heir->map.index()], heir->call, listOf(heir->map) });
            defineByDescriptor(site, known, { }, listOf(heir->map));
            std::ranges::stable_sort(m_summary.definitionsByDescriptorInOrderOfSites, { }, [&](uint32_t change) { return m_summary.changes[change].site.index(); });
            m_summary.calls[*findCall(site)].describesPrototypeInSight = true;
        }
    }
    for (const HeirMadeByBuiltinInScan& heir : m_heirsMadeByBuiltins) {
        if (heir.isFollowed)
            continue;
        escape(heir.above);
        m_summary.changes[heir.change].heirIsMadeByCall = true;
        addSink(SinkKindInScan::OtherOfChange, listOf(ValueID(heir.call)), heir.change);
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
    auto isOpened = [&](BuiltinName name) {
        StringID text = m_program.stringOf(name);
        return text && m_program.isOpenedSomewhere(text);
    };
    bool objectsInheritNoAccessor = !isOpened(BuiltinNames::get) && !isOpened(BuiltinNames::set);
    for (const DefinitionByDescriptorInScan& definition : m_definitionsByDescriptors) {
        uint32_t root = definition.rootOfDescriptor;
        BodyValue made = definitionOf(root);
        uint32_t headOfDefinition = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[definition.position]];
        bool holdsWhatIsWritten = storesInto(root).empty() && !(m_uses[root] & isAccessedBesideNamedStores) && (headOfDefinition == noLoop || headOfDefinition <= m_blocks[root]);
        if (made && made.opcode() == Opcode::Data && objectsInheritNoAccessor && holdsWhatIsWritten) {
            DataID data = made.as<Opcode::Data>().data();
            const auto* written = data ? m_program.at(data).root.tryAs<DataNode::Object>() : nullptr;
            m_summary.changes[definition.change].descriptorLaysNoAccessor = written && !std::ranges::contains(written->members, get, &DataMember::key) && !std::ranges::contains(written->members, set, &DataMember::key);
        }
        if (!definition.waitsForItsJudgment)
            continue;
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
            if (definition.access == DefinitionByDescriptorInScan::noDefinitionUnderName)
                escape(definition.descriptor);
            continue;
        }

        m_summary.changes[definition.change].descriptorIsInSight = true;
        m_summary.changes[definition.change].descriptorLaysNoAccessor = !std::ranges::contains(held, get, &Held::key) && !std::ranges::contains(held, set, &Held::key);
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
        if (!isAccessor && std::ranges::count(held, valueKey, &Held::key) == 1) {
            TypeID written = heldUnderValue->list.count == 1 ? primitiveConstantAt(m_atoms[heldUnderValue->list.first]) : TypeID();
            bool isTextNumberOrBoolean = written && !(primitivesInID(written) & ~(Primitive::AnyString | Primitive::Number | Primitive::Boolean));
            m_summary.changes[definition.change].laysTextNumberOrBooleanThatIsWritten = isTextNumberOrBoolean;
            if (definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName)
                m_summary.accesses[definition.access].laysTextNumberOrBooleanThatIsWritten = isTextNumberOrBoolean;
        }
        bool valueIsInSight = definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName && !isAccessor && std::ranges::count(held, valueKey, &Held::key) == 1 && isEnumerable && isConfigurable && isWritable
            && m_firstLeft[root] == definition.position && m_lastLeft[root] == definition.position;
        bool definitionBringsValue = valueIsInSight;
        m_summary.changes[definition.change].laysValueInSight = definitionBringsValue;
        for (const Held& member : held) {
            if (member.key != get && member.key != set && member.key != valueKey)
                continue;
            if (valueIsInSight) {
                for (SinkInScan& sink : m_sinks) {
                    if (sink.kind != SinkKindInScan::Edge || sink.target != member.field || sink.value.first != member.list.first || sink.value.count != member.list.count)
                        continue;
                    sink.value.count = 0;
                    break;
                }
                if (definitionBringsValue)
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
        if (definition.access != DefinitionByDescriptorInScan::noDefinitionUnderName) {
            nameAccessorWrittenInDescriptor(definition.access, root, definition.position);
            m_summary.changes[definition.change].laysAccessorAsWritten = m_summary.accesses[definition.access].laysAccessorAsWritten;
        }
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
        for (uint32_t index : storesInto(root)) {
            const StoreInScan& store = m_stores[index];
            if (store.key != get || store.site.index() >= definition.position)
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
        AOT::stableSort(m_storesByRoot, { }, [&](uint32_t index) {
            return m_stores[index].root;
        });
    }
    auto found = std::ranges::equal_range(m_storesByRoot, root, { }, [&](uint32_t index) {
        return m_stores[index].root;
    });
    return m_storesByRoot.span().subspan(found.begin() - m_storesByRoot.begin(), found.size());
}

FunctionID ScanOfBody::functionWrittenAt(const SummaryNode& held) const
{
    if (held.kind == SummaryNode::Kind::Closure)
        return FunctionID(held.first);
    if (held.kind != SummaryNode::Kind::Variable)
        return { };
    const Variable& variable = m_program.at(VariableID(held.first));
    return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) ? variable.function : FunctionID();
}

void ScanOfBody::removeWrittenAccessor(uint32_t access)
{
    m_summary.writtenAccessors.removeAllMatching([&](const SummaryWrittenAccessor& written) {
        return written.access == access;
    });
}

void ScanOfBody::nameAccessorWrittenInDescriptor(uint32_t access, uint32_t root, uint32_t position, uint32_t changeOfEntryInMap, uint32_t placeWhereMapTakesEntry)
{
    bool hasLeftIntoMapAlone = changeOfEntryInMap != changeOfNoMap && m_firstLeft[root] == placeWhereMapTakesEntry && m_lastLeft[root] == placeWhereMapTakesEntry;
    if (changeOfEntryInMap != changeOfNoMap && !hasLeftIntoMapAlone)
        return;
    BodyValue made = definitionOf(root);
    if (!holdsOnlyWhatItsBodyLaysInScan(m_program, made) || m_uses[root] & isAccessedBesideNamedStores)
        return;
    uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[position]];
    if (head != noLoop && head > m_blocks[root])
        return;
    PropertyKeyID get = m_program.keyOf(BuiltinNames::get);
    PropertyKeyID set = m_program.keyOf(BuiltinNames::set);
    std::optional<ListInScan> halves[2];
    std::optional<ListInScan> value;
    std::optional<ListInScan> writable;
    std::optional<ListInScan> otherFlags[2];
    bool laysValue = false;
    auto take = [&](PropertyKeyID key, ListInScan held) {
        if (!key)
            return false;
        laysValue |= key == m_program.keyOf(BuiltinNames::value);
        if (key == m_program.keyOf(BuiltinNames::enumerable) || key == m_program.keyOf(BuiltinNames::configurable)) {
            std::optional<ListInScan>& flag = otherFlags[key == m_program.keyOf(BuiltinNames::configurable)];
            if (flag)
                return false;
            flag = held;
            return true;
        }
        if (key == m_program.keyOf(BuiltinNames::value) || key == m_program.keyOf(BuiltinNames::writable)) {
            std::optional<ListInScan>& part = key == m_program.keyOf(BuiltinNames::value) ? value : writable;
            if (part)
                return false;
            part = held;
            return true;
        }
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
    bool everyStoreStandsOnEveryWay = true;
    for (uint32_t index : storesInto(root)) {
        const StoreInScan& store = m_stores[index];
        if (store.site.index() < position && !take(store.key, store.value))
            return;
        everyStoreStandsOnEveryWay &= store.site.index() >= position || store.block == m_blocks[root] || store.block == m_blocks[position];
    }
    if (laysValue && (halves[0] || halves[1]))
        return;
    auto flagWritten = [&](const std::optional<ListInScan>& written) -> std::optional<bool> {
        if (!written)
            return false;
        TypeID constant = written->count == 1 ? primitiveConstantAt(m_atoms[written->first]) : TypeID();
        uint16_t primitives = constant ? primitivesInID(constant) : 0;
        if (primitives == Primitive::True)
            return true;
        if (!primitives || primitives & ~(Primitive::False | Primitive::UndefinedOrNull))
            return std::nullopt;
        return false;
    };
    if (!halves[0] && !halves[1]) {
        if (!hasLeftIntoMapAlone)
            return;
        m_summary.changes[changeOfEntryInMap].isEntryWrittenInMap = true;
        std::optional<bool> isWritable = flagWritten(writable);
        std::optional<bool> isEnumerable = flagWritten(otherFlags[0]);
        std::optional<bool> isConfigurable = flagWritten(otherFlags[1]);
        if (value && isWritable && isEnumerable && isConfigurable && everyStoreStandsOnEveryWay && m_summary.accesses[access].key) {
            OptionSet<FieldForm> form;
            if (!*isWritable)
                form.add(FieldForm::IsReadOnly);
            if (!*isEnumerable)
                form.add(FieldForm::IsNotEnumerable);
            if (!*isConfigurable)
                form.add(FieldForm::IsNotConfigurable);
            m_summary.accesses[access].formCreated = form;
            m_summary.changes[changeOfEntryInMap].descriptorIsInSight = true;
            m_summary.changes[changeOfEntryInMap].descriptorLaysNoAccessor = true;
            m_summary.changes[changeOfEntryInMap].valueInSight = oneNodeFor(*value);
        }
        TypeID flag = writable && writable->count == 1 ? primitiveConstantAt(m_atoms[writable->first]) : TypeID();
        if (!value || !flag || primitivesInID(flag) != Primitive::True)
            return;
        uint32_t laid = oneNodeFor(*value);
        m_summary.changes[changeOfEntryInMap].laysValueInSight = true;
        m_summary.changes[changeOfEntryInMap].valueInSight = laid;
        m_summary.accesses[access].value = laid;
        return;
    }
    FunctionID functions[2];
    for (unsigned half = 0; half < 2; ++half) {
        if (!halves[half])
            continue;
        functions[half] = functionWrittenAt(m_summary.nodes[oneNodeFor(*halves[half])]);
        if (!functions[half])
            return;
    }
    m_summary.accesses[access].laysAccessorAsWritten = true;
    removeWrittenAccessor(access);
    m_summary.writtenAccessors.append({ access, functions[0], functions[1], { } });
    if (!hasLeftIntoMapAlone)
        return;
    uint32_t nodes[2] { halves[0] ? oneNodeFor(*halves[0]) : noSummaryNode, halves[1] ? oneNodeFor(*halves[1]) : noSummaryNode };
    SummaryChange& change = m_summary.changes[changeOfEntryInMap];
    change.isEntryWrittenInMap = true;
    change.getterInSight = nodes[0];
    change.setterInSight = nodes[1];
    std::optional<bool> isEnumerable = flagWritten(otherFlags[0]);
    std::optional<bool> isConfigurable = flagWritten(otherFlags[1]);
    if (writable || !isEnumerable || !isConfigurable || !everyStoreStandsOnEveryWay || !m_summary.accesses[access].key)
        return;
    OptionSet<FieldForm> form { FieldForm::IsAccessor };
    if (!*isEnumerable)
        form.add(FieldForm::IsNotEnumerable);
    if (!*isConfigurable)
        form.add(FieldForm::IsNotConfigurable);
    m_summary.accesses[access].formCreated = form;
    change.descriptorIsInSight = true;
}

void ScanOfBody::nameKeysOfDefinitionsByMaps()
{
    auto escapeBehindSinks = [&](ListInScan list) {
        if (!list.count || holdsOnlyConstants(list))
            return;
        expand(list);
        for (const OriginInScan& origin : m_expanded) {
            OriginInScan from = behindTest(origin, { });
            addEdge(from.node, 0, from.passes);
        }
    };
    Vector<uint32_t, 8> sitesOfEntriesOfMapsThatStay;
    Vector<uint32_t, 16> keysNamedInMap;
    for (const DefinitionByMapInScan& definition : m_definitionsByMaps) {
        uint32_t root = definition.rootOfMap;
        BodyValue made = definitionOf(root);
        if (!holdsOnlyWhatItsBodyLaysInScan(m_program, made) || m_uses[root] & (isLaidAccessorsUnderNames | isAccessedBesideNamedStoresAndAccessors)) {
            escapeBehindSinks(definition.map);
            continue;
        }
        uint32_t head = m_innermostLoopHeads.isEmpty() ? noLoop : m_innermostLoopHeads[m_blocks[definition.position]];
        if (head != noLoop && head > m_blocks[root]) {
            escapeBehindSinks(definition.map);
            continue;
        }
        std::span<const uint32_t> listed;
        if (made.opcode() == Opcode::NewObject) {
            listed = made.as<Opcode::NewObject>().fields();
            const Layout& layout = m_program.at(made.as<Opcode::NewObject>().layout());
            bool eachIsNamed = listed.size() <= made.variadicOperands().size() && std::ranges::all_of(listed, [&](uint32_t field) {
                return field < layout.fields.size() && layout.fields[field].key;
            });
            if (!eachIsNamed) {
                escapeBehindSinks(definition.map);
                continue;
            }
        }

        bool eachKeyIsNamedOnceOnEveryWay = true;
        keysNamedInMap.shrink(0);
        for (uint32_t field : listed)
            keysNamedInMap.append(m_program.at(made.as<Opcode::NewObject>().layout()).fields[field].key.bits());
        for (uint32_t index : storesInto(root)) {
            const StoreInScan& store = m_stores[index];
            if (store.site.index() >= definition.position)
                continue;
            eachKeyIsNamedOnceOnEveryWay &= !!store.key && (store.block == m_blocks[root] || store.block == m_blocks[definition.position]);
            keysNamedInMap.append(store.key.bits());
        }
        for (const DefinitionUnderComputedKeyInScan& entry : m_definitionsUnderComputedKeys)
            eachKeyIsNamedOnceOnEveryWay &= entry.root != root || entry.site.index() >= definition.position;
        std::ranges::sort(keysNamedInMap);
        eachKeyIsNamedOnceOnEveryWay &= std::ranges::adjacent_find(keysNamedInMap) == keysNamedInMap.end();
        bool isFirst = true;
        bool eachEntryLaysAccessorAsWritten = true;
        uint32_t valueWhereNoneIsInSight = m_summary.accesses[definition.access].value;
        auto nameKey = [&](PropertyKeyID key, ValueID descriptor, uint32_t placeWhereMapTakesEntry, ValueID computedKey = { }) {
            uint32_t keyNode = computedKey ? oneNodeFor(listOf(computedKey)) : noSummaryNode;
            if (!isFirst) {
                if (m_summary.changes[definition.change].kind == SummaryChange::Kind::DefinesByDescriptor)
                    m_summary.definitionsByDescriptorInOrderOfSites.append(static_cast<uint32_t>(m_summary.changes.size()));
                m_summary.changes.append(SummaryChange(m_summary.changes[definition.change]));
                m_summary.accesses.append(SummaryAccess(m_summary.accesses[definition.access]));
            }
            uint32_t placeOfAccess = isFirst ? definition.access : static_cast<uint32_t>(m_summary.accesses.size() - 1);
            uint32_t placeOfChange = isFirst ? definition.change : static_cast<uint32_t>(m_summary.changes.size() - 1);
            SummaryChange& change = isFirst ? m_summary.changes[definition.change] : m_summary.changes.last();
            SummaryAccess& access = m_summary.accesses[placeOfAccess];
            change.key = key;
            change.keyNode = keyNode;
            change.isEntryWrittenInMap = false;
            change.laysAccessorAsWritten = false;
            change.getterInSight = noSummaryNode;
            change.setterInSight = noSummaryNode;
            change.laysValueInSight = false;
            change.valueInSight = noSummaryNode;
            change.descriptorIsInSight = false;
            change.descriptorLaysNoAccessor = false;
            access.formCreated = std::nullopt;
            access.key = key;
            access.keyNode = keyNode;
            access.value = valueWhereNoneIsInSight;
            access.laysAccessorAsWritten = false;
            removeWrittenAccessor(placeOfAccess);
            access.descriptorFromElsewhere = noSummaryNode;
            isFirst = false;
            eachEntryLaysAccessorAsWritten &= key && descriptor;
            if (!descriptor)
                return;
            nameAccessorWrittenInDescriptor(placeOfAccess, m_roots[descriptor.index()], definition.position, placeOfChange, placeWhereMapTakesEntry);
            m_summary.changes[placeOfChange].laysAccessorAsWritten = m_summary.accesses[placeOfAccess].laysAccessorAsWritten;
            if (!eachKeyIsNamedOnceOnEveryWay && m_summary.changes[placeOfChange].descriptorIsInSight) {
                SummaryChange& read = m_summary.changes[placeOfChange];
                read.descriptorIsInSight = false;
                read.descriptorLaysNoAccessor = false;
                if (!read.laysValueInSight)
                    read.valueInSight = noSummaryNode;
                m_summary.accesses[placeOfAccess].formCreated = std::nullopt;
            }
            eachEntryLaysAccessorAsWritten &= m_summary.accesses[placeOfAccess].laysAccessorAsWritten || m_summary.changes[placeOfChange].descriptorIsInSight;
            if (m_summary.changes[placeOfChange].descriptorIsInSight && !m_summary.accesses[placeOfAccess].laysAccessorAsWritten)
                return;
            if (m_summary.accesses[placeOfAccess].laysAccessorAsWritten) {
                if (made.opcode() == Opcode::NewObject)
                    m_summary.writtenAccessors.last().map = made.as<Opcode::NewObject>().layout();
                return;
            }
            uint32_t from = oneNodeFor(listOf(descriptor));
            m_summary.accesses[placeOfAccess].descriptorFromElsewhere = from;
        };
        for (size_t place = 0; place < listed.size(); ++place)
            nameKey(m_program.at(made.as<Opcode::NewObject>().layout()).fields[listed[place]].key, made.variadicOperands()[place], root);
        for (uint32_t index : storesInto(root)) {
            StoreInScan store = m_stores[index];
            if (store.site.index() >= definition.position)
                continue;
            BodyValue operation = definitionOf(store.site.index());
            bool storesOperand = operation && (operation.opcode() == Opcode::PropDefine || operation.opcode() == Opcode::PropSet);
            nameKey(store.key, storesOperand ? operation.valueOperand() : ValueID(), store.site.index());
        }
        for (const DefinitionUnderComputedKeyInScan& entry : m_definitionsUnderComputedKeys) {
            if (entry.root != root || entry.site.index() >= definition.position)
                continue;
            ValueID written = entry.key;
            if (BodyValue conversion = definitionBehindCasts(written); conversion && conversion.opcode() == Opcode::ToPropertyKey && conversion.operand(0))
                written = conversion.operand(0);
            PropertyKeyID spelled = keySpelledBy(definitionBehindCasts(written));
            nameKey(spelled, entry.stored, entry.site.index(), spelled ? ValueID() : written);
        }
        if (!eachEntryLaysAccessorAsWritten) {
            escapeBehindSinks(definition.map);
            continue;
        }
        if (made.opcode() == Opcode::NewObject || m_firstLeft[root] != definition.position || m_lastLeft[root] != definition.position)
            continue;
        for (uint32_t index : storesInto(root)) {
            if (m_stores[index].site.index() < definition.position)
                sitesOfEntriesOfMapsThatStay.append(m_stores[index].site.index());
        }
    }
    if (sitesOfEntriesOfMapsThatStay.isEmpty())
        return;
    std::ranges::sort(sitesOfEntriesOfMapsThatStay);
    for (SummaryAccess& access : m_summary.accesses) {
        if (access.kind == SummaryAccess::Kind::Define && !access.isByDescriptor && std::ranges::binary_search(sitesOfEntriesOfMapsThatStay, access.site.index()))
            access.value = 0;
    }
}

uint32_t ScanOfBody::joinObjectsConverted(ListInScan list, bool isConversionByEquality)
{
    expand(list);
    uint32_t objects = 0;
    for (const OriginInScan& origin : m_expanded) {
        if (isAlwaysPrimitive(origin))
            continue;
        if (!objects)
            objects = newJoin();
        OriginInScan from = behindTest(origin, TypeTable::object);
        addEdge(from.node, objects, from.passes, false, isConversionByEquality);
    }
    return objects;
}

void ScanOfBody::applyConversionToPrimitive(const SinkInScan& sink)
{
    uint32_t objects = joinObjectsConverted(sink.value, false);
    if (!objects)
        return;
    m_firstCallOfConversion[sink.value.first] = static_cast<uint32_t>(m_summary.calls.size());
    callConversionsOf(objects, ValueID(sink.target));
}

void ScanOfBody::applyConversionByEquality(const EqualityInScan& equality)
{
    uint32_t firstAtom = equality.converted.first;
    if (firstAtom < m_isConvertedToPrimitive.size() && m_isConvertedToPrimitive[firstAtom])
        return;
    expand(equality.other);
    bool mayBeUnseen = m_expanded.isEmpty();
    bool mayConvert = mayBeUnseen;
    for (const OriginInScan& origin : m_expanded) {
        TypeID constant = primitiveConstantAt(origin.node);
        mayConvert |= !constant || (m_program.types.at(constant).primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent));
        mayBeUnseen |= !origin.node;
    }
    if (!mayConvert)
        return;
    if (mayBeUnseen) {
        while (m_isConvertedToPrimitive.size() <= firstAtom)
            m_isConvertedToPrimitive.append(false);
        m_isConvertedToPrimitive[firstAtom] = true;
        applyConversionToPrimitive(SinkInScan { SinkKindInScan::ConvertedToPrimitive, equality.converted, equality.site.index(), { }, false });
        return;
    }
    auto converted = m_objectsConvertedByEqualities.add(firstAtom, 0);
    if (converted.isNewEntry) {
        converted.iterator->value = joinObjectsConverted(equality.converted, true);
        if (converted.iterator->value)
            callConversionsOf(converted.iterator->value, equality.site);
    }
    if (!converted.iterator->value)
        return;
    SummaryOperator record;
    record.site = equality.site;
    record.opcode = Opcode::Eq;
    record.operands[0] = oneNodeFor(equality.other);
    record.result = converted.iterator->value;
    m_summary.operators.append(record);
}

void ScanOfBody::callConversionsOf(uint32_t objects, ValueID site)
{
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinName memberTakingHint = builtins.nameOf(WellKnownSymbol::ToPrimitive);
    bool isInLoop = !m_innermostLoopHeads.isEmpty() && m_innermostLoopHeads[m_blocks[site.index()]] != noLoop;
    for (BuiltinName name : builtins.keysReadBy(Protocol::ToPrimitive)) {
        SummaryCall call;
        call.site = site;
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
    access.storesSurelyIndex = store.storesSurelyIndex;
    access.storesZero = store.storesZero;
    access.subjectIsMadeInBody = isMadeInBody(ValueID(store.root));
    access.subject = oneNodeFor(store.base);
    access.key = store.key;
    access.value = oneNodeFor(store.value);
    m_summary.accesses.append(access);
}

static void markRedefinedMembersInScan(auto& members)
{
    if (members.size() < 2)
        return;
    auto placeOf = [&](uint32_t index) {
        const SummaryMember& member = members[index];
        return std::tuple { member.classLayout, member.side, member.key };
    };
    Vector<uint32_t, 16> order;
    for (uint32_t index = 0; index < members.size(); ++index) {
        if (members[index].key)
            order.append(index);
    }
    std::ranges::sort(order, { }, [&](uint32_t index) {
        return std::pair { placeOf(index), members[index].index };
    });
    static constexpr auto bitOf = [](MethodKind kind) -> uint8_t {
        return 1 << static_cast<unsigned>(kind);
    };
    uint8_t laterKinds = 0;
    for (size_t end = order.size(); end; ) {
        size_t begin = end - 1;
        const SummaryMember& last = members[order[begin]];
        while (begin && placeOf(order[begin - 1]) == placeOf(order[end - 1]) && members[order[begin - 1]].index == last.index)
            --begin;
        if (end == order.size() || placeOf(order[end]) != placeOf(order[begin]))
            laterKinds = 0;
        uint8_t replacingKinds = last.kind == MethodKind::Method ? 0xff : bitOf(MethodKind::Method) | bitOf(last.kind);
        for (size_t copy = begin; copy < end; ++copy)
            members[order[copy]].isRedefined = laterKinds & replacingKinds;
        laterKinds |= bitOf(last.kind);
        end = begin;
    }
}

void ScanOfBody::finish()
{
    markRedefinedMembersInScan(m_summary.members);
    if (m_function->isCoroutine()) {
        if (uint32_t result = resultNode(0))
            addEdge(objectOfCoroutine(), result);
    }
    if (!m_someBlockReturns && !m_function->isCoroutine())
        m_summary.traits.add(SummaryTrait::NeverReturns);
    if (!m_environmentsBeingMade.isEmpty())
        letFirstValuesOfSlotsArrive();
    if (!m_summary.handlers.isEmpty())
        settleWhereRecordsThrow();
    finishKnownEffects();
    if (m_summary.ownEffects.mayThrow) {
        m_summary.traits.add(SummaryTrait::MayThrow);
        addEdge(0, nodeFor(SummaryNode::Kind::Thrown));
    }

    if (m_callThatHostMakes && !(m_callThatHostMakes->first.passesLiteral && m_moduleVariablesRead.contains(m_callThatHostMakes->second))) {
        auto [call, argumentVariable] = *m_callThatHostMakes;
        call.firstArgument = static_cast<uint32_t>(m_summary.arguments.size());
        m_summary.arguments.append(variableNode(argumentVariable));
        m_summary.calls.append(call);
    }
    removeRepeatedEdges();
    listNamesSpelled();
    if (!m_summary.modulesWhosePathIsRead.isEmpty())
        finishListsOfStringsInScan(m_summary);
    listConstructions();
    sortWithoutRepeatsInScan(m_summary.classesEvaluated);
    sortWithoutRepeatsInScan(m_summary.modulesHandedOut);
    sortWithoutRepeatsInScan(m_summary.otherModulesNamed);
    if (m_listsFirstStoresOfBody)
        listFirstStoresAndUses();
    if (m_listsDefinitions)
        listDefinitions();
    sortWithoutRepeatsInScan(m_summary.modulesNamedInBlocksNeverEntered);
    sortWithoutRepeatsInScan(m_summary.functionsNamed);
    listWhatWalkAsksOfNodesAndCalls();
    listStoresOnResult();
    AOT::stableSort(m_summary.writtenAccessors, { }, &SummaryWrittenAccessor::access);
    AOT::stableSort(m_summary.definitionsByDescriptorInOrderOfSites, { }, [&](uint32_t change) {
        return m_summary.changes[change].site.index();
    });
    if (!m_sitesOfCopiesThatSpellStores.isEmpty()) {
        std::ranges::sort(m_sitesOfCopiesThatSpellStores);
        for (uint32_t call = 0; call < m_summary.calls.size(); ++call) {
            if (std::ranges::binary_search(m_sitesOfCopiesThatSpellStores, m_summary.calls[call].site.index()))
                m_summary.callsAtSitesOfCopiesThatSpellStores.append(call);
        }
        AOT::stableSort(m_summary.callsAtSitesOfCopiesThatSpellStores, { }, [&](uint32_t call) {
            return m_summary.calls[call].site.index();
        });
    }
}

void ScanOfBody::listWhatWalkAsksOfNodesAndCalls()
{
    m_summary.makesObjects = std::ranges::any_of(m_summary.nodes, [](const SummaryNode& node) {
        return node.kind == SummaryNode::Kind::Allocation || node.kind == SummaryNode::Kind::ClassInstance || node.kind == SummaryNode::Kind::Closure;
    });

    std::span<const SummaryCall> calls = m_summary.calls.span();
    bool callsBuiltinWithKey = false;
    for (const SummaryCall& call : calls) {
        if (call.callee == SummaryCall::Callee::Builtin)
            callsBuiltinWithKey |= !!call.key;
        else if (!call.isMadeByProtocol && !call.isMadeByHost) {
            ++m_summary.numberOfOwnCalls;
            m_summary.numberOfArgumentsOfOwnCalls += call.numberOfArguments;
        }
    }
    if (callsBuiltinWithKey)
        listSymbolsMadeOnce();
    if (calls.size() == 1 && calls[0].isInTailPosition && !calls[0].isConstruction && !calls[0].passesList && !calls[0].isMadeByProtocol)
        m_summary.tailCallThatIsOnlyCall.append({ calls[0].site, calls[0].numberOfArguments });
}

NEVER_INLINE void ScanOfBody::listSymbolsMadeOnce()
{
    BitVector sitesOfCallsOfBuiltins;
    for (const SummaryCall& call : m_summary.calls) {
        if (call.callee != SummaryCall::Callee::Builtin)
            continue;
        bool isFirstRecordOfSite = !sitesOfCallsOfBuiltins.set(call.site.index());
        PropertyKeyID key = keyOfSymbolMadeOnceBy(m_program, call);
        if (key && isFirstRecordOfSite)
            m_summary.symbolsMadeOnce.append({ call.site, key });
    }
    std::ranges::sort(m_summary.symbolsMadeOnce, { }, &SummaryKeyAtSite::site);
}

void ScanOfBody::listStoresOnResult()
{
    if (m_summary.calls.isEmpty() && m_summary.accesses.isEmpty())
        return;
    uint32_t returned = noSummaryNode;
    for (const SummaryEdge& edge : m_summary.edges) {
        if (m_summary.nodes[edge.to].kind != SummaryNode::Kind::Result)
            continue;
        if (returned != noSummaryNode && returned != edge.from)
            return;
        returned = edge.from;
    }
    if (returned == noSummaryNode)
        return;
    const SummaryNode& node = m_summary.nodes[returned];
    if (node.kind != SummaryNode::Kind::ResultOfCall && node.kind != SummaryNode::Kind::Allocation)
        return;
    if (node.kind == SummaryNode::Kind::ResultOfCall && !m_summary.calls[node.first].isConstruction)
        m_summary.callThatGivesResult.append(m_summary.calls[node.first].site);
    for (const SummaryAccess& access : m_summary.accesses) {
        if ((access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define) && !access.isToContents() && access.subject == returned)
            m_summary.storesOnResult.append({ access.site, access.key });
    }
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
    if (m_sitesOfRows.size() > 1)
        std::sort(m_sitesOfRows.begin(), m_sitesOfRows.end());
    auto note = [&](ValueID site, PropertyKeyID key, Vector<PropertyKeyID>& names, SummaryTrait underComputedKey) {
        if (!m_sitesOfRows.isEmpty() && std::binary_search(m_sitesOfRows.begin(), m_sitesOfRows.end(), site))
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
    constexpr size_t mostEdgesSortedInPlace = 32;
    auto placeOf = [](const SummaryEdge& edge) {
        return static_cast<uint64_t>(edge.to) << 32 | edge.from;
    };
    size_t size = m_summary.edges.size();
    SummaryEdge* edges = m_summary.edges.begin();
    if (size <= mostEdgesSortedInPlace) {
        for (size_t index = 1; index < size; ++index) {
            SummaryEdge edge = edges[index];
            uint64_t place = placeOf(edge);
            size_t gap = index;
            for (; gap && placeOf(edges[gap - 1]) > place; --gap)
                edges[gap] = edges[gap - 1];
            edges[gap] = edge;
        }
    } else {
        AOT::stableSort(std::span(edges, size), [&](const SummaryEdge& first, const SummaryEdge& second) {
            return placeOf(first) < placeOf(second);
        });
    }

    size_t kept = 0;
    for (size_t first = 0; first < size;) {
        uint64_t place = placeOf(edges[first]);
        size_t end = first + 1;
        if (end == size || placeOf(edges[end]) != place) {
            edges[kept++] = edges[first];
            first = end;
            continue;
        }
        bool passesAll = false;
        bool runsCodeOfValue = false;
        bool isConversionByEquality = true;
        for (end = first; end < size && placeOf(edges[end]) == place; ++end) {
            passesAll |= !edges[end].passes;
            runsCodeOfValue |= edges[end].runsCodeOfValue;
            isConversionByEquality &= edges[end].isConversionByEquality;
        }

        size_t firstKept = kept;
        for (size_t index = first; index < end; ++index) {
            SummaryEdge edge = edges[index];
            edge.runsCodeOfValue = runsCodeOfValue;
            edge.isConversionByEquality = isConversionByEquality;
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
    m_summary.edges.shrink(kept);
}

LayoutID classOfConstructorFunction(const Program& program, FunctionID function)
{
    LayoutID constructed = function ? program.at(function).constructedClass : LayoutID();
    if (!constructed || !program.at(constructed).has(LayoutFlags::IsConstructorFunction) || program.at(constructed).constructor != function)
        return { };
    return constructed;
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

static bool isWriteOnceModuleVariable(const Program& program, VariableID id)
{
    const Variable& variable = program.at(id);
    if (variable.owner || !variable.module || variable.initialization != VariableInitialization::Uninitialized || !variable.flags.contains(VariableFlags::IsImmutable) || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
        return false;
    return loaderEntersBodyAtMostOnce(program.at(variable.module));
}

static void nameKeysOfMembersByVariablesInScan(const Program& program, const Vector<std::unique_ptr<ScanOfBody>>& scans, ThinSummaries& summaries)
{
    Vector<ScanOfBody::KeyOfVariableInScan> keys;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (scan && !scan->membersUnderVariables().isEmpty() && keys.isEmpty()) {
            for (const std::unique_ptr<ScanOfBody>& other : scans) {
                if (other)
                    keys.appendVector(other->keysOfVariables());
            }
            std::ranges::sort(keys, { }, &ScanOfBody::KeyOfVariableInScan::variable);
        }
    }
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        for (const ScanOfBody::MemberUnderVariableInScan& member : scan->membersUnderVariables()) {
            auto stored = std::ranges::equal_range(keys, member.variable, { }, &ScanOfBody::KeyOfVariableInScan::variable);
            if (stored.size() != 1 || !isWriteOnceModuleVariable(program, member.variable))
                continue;
            auto& members = summaries.at(member.function).members;
            members[member.member].key = stored.front().key;
            markRedefinedMembersInScan(members);
        }
    }
}

static void collectVariablesStoredByElement(const Program& program, const Vector<std::unique_ptr<ScanOfBody>>& scans, ThinSummaries& summaries)
{
    BitVector stored(program.variables.size());
    BitVector keys;
    Vector<FieldReference> fields;
    Vector<CaptureInScan> captures;
    ThinSummaries::ElementStores elementStores;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        for (VariableID variable : scan->variablesStoredByElement())
            stored.set(variable.index());
        for (PropertyKeyID key : scan->keysStoredByElement())
            keys.set(key.index());
        fields.appendVector(scan->fieldsStoredByElement());
        captures.appendVector(scan->capturesStoredByElement());
        elementStores.merge(scan->elementStores());
    }
    auto order = [](FieldReference field) {
        return std::pair { field.layout.index(), field.field };
    };
    std::ranges::sort(fields, { }, order);
    std::ranges::sort(captures);
    BitVector literals;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        for (auto [variable, literal] : scan->literalsOfVariables()) {
            if (stored.get(variable.index()))
                literals.set(literal.index());
        }
        for (auto [key, literal] : scan->literalsOfKeys()) {
            if (keys.get(key.index()))
                literals.set(literal.index());
        }
        for (auto [field, literal] : scan->literalsOfFields()) {
            if (std::ranges::binary_search(fields, order(field), { }, order))
                literals.set(literal.index());
        }
        for (DataID literal : scan->literalsStoredByElement())
            literals.set(literal.index());
        for (auto [capture, literal] : scan->literalsOfCaptures()) {
            if (std::ranges::binary_search(captures, capture))
                literals.set(literal.index());
        }
    }
    summaries.setElementStores(WTF::move(elementStores));
    summaries.setStoresByElement(WTF::move(stored), WTF::move(literals));
}

static void collectSymbolValuedVariables(const Program& program, const Vector<std::unique_ptr<ScanOfBody>>& scans, ThinSummaries& summaries)
{
    Vector<PropertyKeyID> symbols;
    BitVector isStored;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        for (auto [variable, key] : scan->keysOfVariables()) {
            PropertyKeyKind kind = program.at(key).kind;
            bool isSymbol = kind == PropertyKeyKind::WellKnownSymbol || kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::RegisteredSymbol;
            if (!isSymbol || !isWriteOnceModuleVariable(program, variable))
                continue;
            if (symbols.isEmpty())
                symbols.fill(PropertyKeyID(), program.variables.size());
            symbols[variable.index()] = isStored.set(variable.index()) ? PropertyKeyID() : key;
        }
    }
    summaries.setSymbolsOfVariables(WTF::move(symbols));
}
static void listFunctionsThatCopyMembersInScan(size_t numberOfFunctions, const Vector<std::unique_ptr<ScanOfBody>>& scans, ThinSummaries& summaries)
{
    Vector<FunctionID> functions;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (scan)
            functions.appendVector(scan->functionsThatCopyMembers());
    }
    if (functions.isEmpty())
        return;
    std::ranges::sort(functions);

    Vector<FunctionID> makers(numberOfFunctions);
    BitVector haveSeveralMakers(numberOfFunctions);
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        for (auto [closure, maker] : scan->makersOfClosures()) {
            if (closure.index() >= numberOfFunctions)
                continue;
            FunctionID known = std::exchange(makers[closure.index()], maker);
            if (known && known != maker)
                haveSeveralMakers.quickSet(closure.index());
        }
    }
    Vector<ThinSummaries::MakerOfClosures> makersAbove;
    BitVector isListed(numberOfFunctions);
    for (FunctionID function : functions) {
        for (FunctionID closure = function; makers[closure.index()] && !haveSeveralMakers.quickGet(closure.index()) && !isListed.quickSet(closure.index()); closure = makers[closure.index()])
            makersAbove.append({ closure, makers[closure.index()] });
    }
    std::ranges::sort(makersAbove, { }, &ThinSummaries::MakerOfClosures::closure);
    summaries.setMakersAboveCopiesOfMembers(WTF::move(makersAbove));
    summaries.setFunctionsThatCopyMembers(WTF::move(functions));
}

ThinSummaries scanBodies(const Program& program, Diagnostics& diagnostics, const ParallelLoops& loops, Mechanisms mechanisms)
{
    constexpr size_t functionsPerTask = 32;
    size_t numberOfFunctions = program.functions.size();
    ThinSummaries summaries(numberOfFunctions, loops);
    BitVector textsThatNameFieldsNeverWritten = findTextsThatNameFieldsNeverWrittenForScan(program);
    BitVector namesUnderWhichEventsHang = namesUnderWhichEventsHangInScan(program.builtins());
    BitVector namesUnderWhichTextIsEvaluated = namesUnderWhichTextIsEvaluatedInScan(program.builtins());
    summaries.setNamesUnderWhichEventsHang(namesUnderWhichEventsHang);
    BitVector bodiesThatSeveralModulesName = bodiesThatSeveralModulesNameInScan(program);
    Vector<std::unique_ptr<ScanOfBody>> scans(loops.width());
    auto scanOf = [&](unsigned worker) -> ScanOfBody& {
        std::unique_ptr<ScanOfBody>& scan = scans[worker];
        if (!scan)
            scan = makeUnique<ScanOfBody>(program, mechanisms, textsThatNameFieldsNeverWritten, &namesUnderWhichEventsHang, &bodiesThatSeveralModulesName, &namesUnderWhichTextIsEvaluated);
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

    SetsOfLayoutsWritten setsOfLayoutsWritten;
    uint64_t numberOfValuesScanned = 0;
    for (std::unique_ptr<ScanOfBody>& scan : scans) {
        if (!scan)
            continue;
        numberOfValuesScanned += scan->numberOfValuesScanned();
        for (uint32_t set : scan->setsOfLayoutsWritten())
            setsOfLayoutsWritten.add(set);
    }
    summaries.setSetsOfLayoutsWrittenInBodies(WTF::move(setsOfLayoutsWritten));
    summaries.setNumberOfValuesScanned(numberOfValuesScanned);

    Vector<FunctionID> copyingFunctions;
    for (const std::unique_ptr<ScanOfBody>& scan : scans) {
        if (scan)
            copyingFunctions.appendVector(scan->copyingFunctions());
    }
    std::ranges::sort(copyingFunctions);
    summaries.setCopyingFunctions(WTF::move(copyingFunctions));
    nameKeysOfMembersByVariablesInScan(program, scans, summaries);
    collectSymbolValuedVariables(program, scans, summaries);
    collectVariablesStoredByElement(program, scans, summaries);
    listFunctionsThatCopyMembersInScan(numberOfFunctions, scans, summaries);

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
