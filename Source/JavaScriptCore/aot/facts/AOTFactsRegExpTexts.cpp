/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFacts.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeHooks.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include "Collector.h"
#include "ErrorInstance.h"
#include "GetterSetter.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "MarkedVector.h"
#include "ObjectConstructor.h"
#include "ProxyObject.h"
#include "RegExpObject.h"
#include "StringObject.h"
#include "TopExceptionScope.h"
#include "YarrFlags.h"
#include "YarrPattern.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/SetForScope.h>
#include <wtf/text/AdaptiveStringSearcher.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t operationsFromOneStart = 1 << 17;
static constexpr uint64_t operationsInAllForEachOfModule = 4;
static constexpr uint64_t operationsInAllOfSmallestModule = 8 * operationsFromOneStart;
static constexpr uint64_t operationsMoreForBodyInStartOfProgram = 56 * operationsFromOneStart;
static constexpr unsigned deepestCall = 256;
static constexpr unsigned mostOpenRegions = 1024;
static constexpr uint32_t valuesMadeRoomForAtPriceOfOneOperation = 16;
static constexpr uint64_t placesOfEntryInMap = 4;
static constexpr uint64_t placesOfEntryInSet = 3;
static constexpr uint32_t charactersGoneOverAtPriceOfOneOperation = 16;
static constexpr uint64_t operationsForCharacterOfPatternThatIsMade = 2;
static constexpr uint8_t timesThroughBlockWhileGoingBothWays = 4;
static constexpr uint32_t operationsForBuiltinHandedRegExp = 1 << 10;
static constexpr size_t mostBytesMade = 256 * MB;
static constexpr PropertyOffset placeOfMarkOfWhatNobodyKnows = 0;

enum class GrowthOfWork : uint32_t {
    WithWhatIsHanded = 1 << 0,
    WithNumbersHanded = 1 << 1,
    WithPropertiesOfWhatIsHanded = 1 << 2,
    WithAllThatIsReached = 1 << 3,
    CopiesWhatIsHeld = 1 << 4,
    CopiesWhatIsHeldIntoStructuresOfStructures = 1 << 5,
    WithLengthOfWhatIsHanded = 1 << 6,
    GoesOverItsArguments = 1 << 7,
    MakesPatternOfTextHanded = 1 << 8,
    LaysTextBetweenPlaces = 1 << 9,
    LaysTextOverFind = 1 << 10,
    LaysTextOverEveryFind = 1 << 11,
    AsksWhetherWhatIsGivenBackIsTrue = 1 << 12,
    GoesIntoWhatItIsHandedFirst = 1 << 13,
    AsksIdentityOfWhatIsHeld = 1 << 14,
    MakesCellsOfCharactersHanded = 1 << 15,
    TouchesOnePlaceOfWhatIsHanded = 1 << 16,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithWhatIsHanded[] = {
    "ObjectDefineProperty"_s, "ObjectGetOwnPropertyDescriptor"_s, "ObjectGetPrototypeOf"_s, "ObjectSetPrototypeOf"_s, "ObjectPrototypeToString"_s, "FunctionPrototypeToString"_s,
    "String"_s, "Number"_s, "Boolean"_s, "RegExp"_s, "Error"_s, "Proxy"_s, "StringPrototypeCharCodeAt"_s, "StringPrototypeToUpperCase"_s, "StringPrototypeToLowerCase"_s,
    "StringPrototypeSplit"_s, "StringPrototypeSlice"_s, "StringPrototypeIncludes"_s, "StringPrototypeIndexOf"_s, "StringPrototypeCharAt"_s, "StringPrototypeEndsWith"_s,
    "StringPrototypeTrim"_s, "StringPrototypeAt"_s, "StringPrototypeMatch"_s, "StringPrototypeToString"_s, "NumberPrototypeToString"_s, "ArrayIsArray"_s, "ArrayPrototypePush"_s,
    "ArrayPrototypeUnshift"_s, "ArrayPrototypeJoin"_s, "ArrayPrototypeIncludes"_s, "ArrayPrototypeSlice"_s, "ArrayPrototypeConcat"_s, "Map"_s, "MapPrototypeGet"_s, "Set"_s,
    "SetPrototypeAdd"_s, "SetPrototypeHas"_s, "WeakMap"_s, "WeakMapPrototypeGet"_s, "WeakMapPrototypeSet"_s, "RegExpPrototypeTest"_s, "MapPrototypeKeys"_s, "StringFromCharCode"_s,
    "ArrayPrototypeAt"_s, "StringPrototypeTrimEnd"_s, "ArrayPrototypeMap"_s, "ArrayPrototypeFilter"_s, "ArrayPrototypeSort"_s, "ArrayPrototypeForEach"_s, "ArrayPrototypeSome"_s,
    "ArrayPrototypeReduce"_s, "StringPrototypeReplace"_s, "StringPrototypeReplaceAll"_s, "ArrayPrototypeFlatMap"_s, "ArrayFrom"_s, "FunctionPrototypeCall"_s,
    "FunctionPrototypeApply"_s, "ArrayPrototypePop"_s, "ArrayPrototypeShift"_s, "ArrayPrototypeSplice"_s, "ArrayPrototypeReverse"_s, "ArrayPrototypeFill"_s, "MapPrototypeSet"_s,
    "MapPrototypeDelete"_s, "MapPrototypeClear"_s, "MapPrototypeForEach"_s, "SetPrototypeDelete"_s, "SetPrototypeClear"_s, "SetPrototypeForEach"_s, "WeakMapPrototypeDelete"_s,
    "ObjectPrototype__defineGetter__"_s, "ReflectDefineProperty"_s, "ObjectHasOwn"_s, "ObjectPrototypeHasOwnProperty"_s, "ObjectPrototypeValueOf"_s,
    "ObjectPrototypePropertyIsEnumerable"_s, "ObjectIsExtensible"_s, "Object"_s, "ObjectFromEntries"_s, "ObjectPrototype__lookupGetter__"_s, "ObjectPrototype__defineSetter__"_s,
    "ReflectGetPrototypeOf"_s, "ReflectGetOwnPropertyDescriptor"_s, "ArrayPrototypeToString"_s, "ErrorPrototypeToString"_s, "Symbol"_s, "SymbolFor"_s, "TypeError"_s,
    "RangeError"_s, "SyntaxError"_s, "ReferenceError"_s, "ErrorIsError"_s, "StringPrototypeLastIndexOf"_s, "StringPrototypeStartsWith"_s,
    "StringPrototypeTrimLeft"_s, "StringPrototypeSubstring"_s, "StringPrototypeNormalize"_s, "StringFromCodePoint"_s, "NumberIsSafeInteger"_s, "NumberIsInteger"_s,
    "NumberIsFinite"_s, "NumberIsNaN"_s, "NumberPrototypeToFixed"_s, "ParseInt"_s, "IsFinite"_s, "IsNaN"_s, "MathMin"_s, "MathMax"_s, "MathFloor"_s, "MathCeil"_s, "MathAbs"_s,
    "MathPow"_s, "MathImul"_s, "MathRound"_s, "ArrayPrototypeIndexOf"_s, "ArrayPrototypeFind"_s, "ArrayPrototypeEvery"_s, "ArrayPrototypeReduceRight"_s,
    "ArrayPrototypeFindIndex"_s, "ArrayPrototypeFindLastIndex"_s, "ArrayPrototypeToSorted"_s, "ArrayPrototypeToReversed"_s, "MapPrototypeHas"_s, "WeakSet"_s,
    "WeakSetPrototypeAdd"_s, "WeakSetPrototypeHas"_s, "WeakSetPrototypeDelete"_s, "EncodeURIComponent"_s, "EncodeURI"_s, "DecodeURI"_s, "DecodeURIComponent"_s,
    "RegExpPrototypeExec"_s, "JSONParse"_s,
};

static constexpr ASCIILiteral builtinsThatCopyWhatIsHeld[] = {
    "ObjectGetOwnPropertyDescriptor"_s, "ObjectValues"_s, "ReflectGetOwnPropertyDescriptor"_s,
};

static constexpr ASCIILiteral builtinsThatCopyWhatIsHeldIntoStructuresOfStructures[] = {
    "ObjectGetOwnPropertyDescriptors"_s, "ObjectEntries"_s,
};

static constexpr ASCIILiteral builtinsThatGoThroughWhatIsHandedByItsLength[] = {
    "ArrayPrototypePush"_s, "ArrayPrototypeUnshift"_s, "ArrayPrototypeJoin"_s, "ArrayPrototypeIncludes"_s, "ArrayPrototypeSlice"_s, "ArrayPrototypeConcat"_s, "ArrayPrototypeAt"_s,
    "ArrayPrototypeMap"_s, "ArrayPrototypeFilter"_s, "ArrayPrototypeSort"_s, "ArrayPrototypeForEach"_s, "ArrayPrototypeSome"_s, "ArrayPrototypeReduce"_s, "ArrayPrototypeFlatMap"_s,
    "ArrayFrom"_s, "StringRaw"_s, "ArrayPrototypePop"_s, "ArrayPrototypeShift"_s, "ArrayPrototypeSplice"_s, "ArrayPrototypeReverse"_s, "ArrayPrototypeFill"_s,
    "ArrayPrototypeToString"_s, "ArrayPrototypeIndexOf"_s, "ArrayPrototypeFind"_s, "ArrayPrototypeEvery"_s, "ArrayPrototypeReduceRight"_s, "ArrayPrototypeFindIndex"_s,
    "ArrayPrototypeFindLastIndex"_s, "ArrayPrototypeToSorted"_s, "ArrayPrototypeToReversed"_s,
};

static constexpr ASCIILiteral builtinsThatTouchOnePlaceOfWhatTheyAreHanded[] = {
    "ArrayPrototypePush"_s, "ArrayPrototypePop"_s, "ArrayPrototypeAt"_s, "MapPrototypeGet"_s, "MapPrototypeSet"_s, "MapPrototypeHas"_s, "MapPrototypeDelete"_s, "SetPrototypeAdd"_s,
    "SetPrototypeHas"_s, "SetPrototypeDelete"_s,
};

static constexpr ASCIILiteral builtinsThatGoOverTheirArguments[] = {
    "ArrayFrom"_s, "Set"_s, "Map"_s, "WeakMap"_s, "WeakSet"_s, "ObjectFromEntries"_s,
};

static constexpr ASCIILiteral builtinsThatAskWhetherWhatIsGivenBackIsTrue[] = {
    "ArrayPrototypeEvery"_s, "ArrayPrototypeSome"_s, "ArrayPrototypeFind"_s, "ArrayPrototypeFindIndex"_s, "ArrayPrototypeFindLastIndex"_s, "ArrayPrototypeFilter"_s,
};

static constexpr ASCIILiteral builtinsThatGoIntoWhatTheyAreHandedFirst[] = {
    "ObjectAssign"_s, "ObjectDefineProperty"_s, "ObjectDefineProperties"_s, "ObjectFreeze"_s, "ObjectSeal"_s, "ObjectSetPrototypeOf"_s, "ReflectDefineProperty"_s, "ObjectCreate"_s,
    "ObjectKeys"_s, "ObjectValues"_s, "ObjectEntries"_s, "ObjectFromEntries"_s, "ObjectGetOwnPropertyNames"_s, "ObjectGetOwnPropertySymbols"_s, "ObjectGetOwnPropertyDescriptor"_s,
    "ObjectGetOwnPropertyDescriptors"_s, "ObjectGetPrototypeOf"_s, "ReflectGetPrototypeOf"_s, "ReflectGetOwnPropertyDescriptor"_s, "ReflectOwnKeys"_s, "Object"_s, "ArrayFrom"_s, "JSONParse"_s,
};

static constexpr ASCIILiteral builtinsThatAskIdentityOfWhatIsHeld[] = {
    "ArrayPrototypeIncludes"_s, "ArrayPrototypeIndexOf"_s,
};

static constexpr ASCIILiteral builtinsThatMakePatternOfTextHanded[] = {
    "StringPrototypeMatch"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithNumbersHanded[] = {
    "Array"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithPropertiesOfWhatIsHanded[] = {
    "ObjectAssign"_s, "ObjectKeys"_s, "ObjectValues"_s, "ObjectEntries"_s, "ObjectFreeze"_s, "ObjectGetOwnPropertyDescriptors"_s, "ObjectGetOwnPropertyNames"_s,
    "ObjectDefineProperties"_s, "ObjectCreate"_s, "ReflectOwnKeys"_s, "ObjectSeal"_s, "ObjectIsFrozen"_s, "ObjectGetOwnPropertySymbols"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithAllThatIsReached[] = {
    "StringRaw"_s,
};

static bool repeatsInsideRepeat(const Yarr::PatternDisjunction& disjunction, bool isInsideRepeat)
{
    for (const auto& alternative : disjunction.m_alternatives) {
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            bool varies = term.quantityType != Yarr::QuantifierType::FixedCount;
            if (varies && isInsideRepeat)
                return true;
            bool hasParentheses = term.type == Yarr::PatternTerm::Type::ParenthesesSubpattern || term.type == Yarr::PatternTerm::Type::ParentheticalAssertion;
            if (hasParentheses && repeatsInsideRepeat(*term.parentheses.disjunction, isInsideRepeat || term.quantityMaxCount > 1u))
                return true;
        }
    }
    return false;
}

struct WaysThroughChoices {
    uint64_t once { 1 };
    uint64_t forEachCharacter { 1 };
};

static constexpr uint64_t moreWaysThanAnyAllowance = 1ull << 32;

static WaysThroughChoices waysThroughChoicesOf(const Yarr::PatternDisjunction& disjunction)
{
    auto times = [](uint64_t a, uint64_t b) {
        return a && b > moreWaysThanAnyAllowance / a ? moreWaysThanAnyAllowance : a * b;
    };
    WaysThroughChoices ways { 0, 1 };
    for (const auto& alternative : disjunction.m_alternatives) {
        uint64_t throughThisOne = 1;
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            if (term.type != Yarr::PatternTerm::Type::ParenthesesSubpattern && term.type != Yarr::PatternTerm::Type::ParentheticalAssertion)
                continue;
            WaysThroughChoices inside = waysThroughChoicesOf(*term.parentheses.disjunction);
            ways.forEachCharacter = times(ways.forEachCharacter, inside.forEachCharacter);
            if (term.quantityMaxCount > 1u)
                ways.forEachCharacter = times(ways.forEachCharacter, inside.once);
            else
                throughThisOne = times(throughThisOne, inside.once);
        }
        ways.once = std::min(ways.once + throughThisOne, moreWaysThanAnyAllowance);
    }
    return ways;
}

static unsigned mostTermsThatVaryInOneAlternative(const Yarr::PatternDisjunction& disjunction)
{
    unsigned most = 0;
    for (const auto& alternative : disjunction.m_alternatives) {
        unsigned inThisOne = 0;
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            if (term.quantityType != Yarr::QuantifierType::FixedCount || term.type == Yarr::PatternTerm::Type::NumberedBackReference || term.type == Yarr::PatternTerm::Type::NamedBackReference)
                ++inThisOne;
            if (term.type == Yarr::PatternTerm::Type::ParenthesesSubpattern || term.type == Yarr::PatternTerm::Type::ParentheticalAssertion)
                inThisOne += mostTermsThatVaryInOneAlternative(*term.parentheses.disjunction);
        }
        most = std::max(most, inThisOne);
    }
    return most;
}

static uint64_t numberOfNodesIn(const DataNode& node)
{
    uint64_t nodes = 1;
    if (const auto* array = node.tryAs<DataNode::Array>()) {
        for (const DataNode& element : array->elements)
            nodes += numberOfNodesIn(element);
    } else if (const auto* object = node.tryAs<DataNode::Object>()) {
        for (const DataMember& member : object->members)
            nodes += numberOfNodesIn(member.value);
    }
    return nodes;
}

class RehearsalOfRegExpTexts {
    WTF_MAKE_NONCOPYABLE(RehearsalOfRegExpTexts);
public:
    RehearsalOfRegExpTexts(VM&, const Program&, const BodyIndexTable&, const ThinSummaries&);

    void run(std::span<const FunctionID> functionsThatCallRegExp);
    void storeTexts(ProgramFactTables&);

private:
    enum class StateOfModule : uint8_t { IsNotEntered, IsRunning, HasRun };

    using Values = MarkedVector<JSValue, 8, RecordOverflow>;

    struct OpenRegion {
        BlockID join;
        size_t firstArrival { 0 };
        uint32_t numberOfArrivals { 0 };
        bool hasBroughtOtherValues { false };
    };

    struct Activation {
        const BodyIndex& index;
        FunctionID function;
        Values values;
        Vector<uint8_t> timesEnteredWhileGoingBothWays;
        Vector<BlockID, 8> blocksCounted;
        Vector<OpenRegion, 2> regions;
        Values arrivals;
        const Vector<BlockID>* joins { nullptr };
        unsigned numberOfWaysGoneSeparately { 0 };
        bool waysDifferInWhatTheyReturn { false };

        bool goesBothWays() const { return !regions.isEmpty(); }
    };

    struct ClosureOfProgram {
        FunctionID function;
        JSArray* captures { nullptr };
    };

    bool isKnown(JSValue value) const { return !value || !value.isObject() || !asObject(value)->hasInlineStorage() || asObject(value)->getDirect(placeOfMarkOfWhatNobodyKnows) != JSValue(m_whatMarksWhatNobodyKnows.get()); }
    bool isNumberThatNobodyKnows(JSValue value) const { return !isKnown(value) && value != JSValue(m_unknown.get()); }
    bool isNumberKnownOrNot(JSValue value) const { return value.isNumber() || isNumberThatNobodyKnows(value); }
    JSValue makeNumberThatNobodyKnows();
    void layUnderNumberThatNobodyKnows(JSObject*, JSValue number, JSValue laid);
    JSValue findUnderNumberThatNobodyKnows(JSObject*, JSValue number);
    bool asksTheWorld(JSValue value) const
    {
        if (JSObject* original = originalOf(value))
            value = original;
        return value && value.isCell() && m_functionsThatAskTheWorld.contains(value.asCell());
    }
    bool isGlobalObject(JSValue value) const { return value == JSValue(m_realm.get()) || value == JSValue(m_realm->globalThis()); }
    JSValue unknown() const { return m_unknown.get(); }
    JSValue at(const Activation& activation, ValueID id) const
    {
        if (!id)
            return jsUndefined();
        JSValue value = activation.values[id.index()];
        return value ? value : unknown();
    }

    String charactersOf(StringID) const;
    JSValue textOf(StringID);
    Identifier nameOf(PropertyKeyID);
    JSValue builtinInRealm(BuiltinID);
    JSValue valueOfData(const DataNode&);
    JSValue listOf(std::span<const JSValue>);
    JSValue closureOf(FunctionID, std::span<const JSValue> captures);
    JSValue variable(VariableID);
    JSValue valueOfCommonJsModule(ModuleID);
    JSValue moduleObjectOf(ModuleID);
    JSValue namespaceOf(ModuleID);
    JSValue prototypeFor(JSValue newTarget);
    Identifier privateNameOf(LayoutID, uint32_t field);
    ClosureOfProgram closureIn(JSValue) const;
    void givePrototypeTo(JSValue closure);
    bool isPatternMadeHere(JSValue value) const { return value.inherits<RegExpObject>() && asObject(value)->getDirect(*m_vm, m_markOfPatternMadeHere); }
    JSValue notePatternMadeHere(JSValue);
    void noteHandedToCodeThatNobodyKnows(std::span<const JSValue> arguments);
    std::optional<bool> isOfType(JSValue, TypeID) const;

    bool noteTexts(std::span<const JSValue> arguments);
    JSValue call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments);
    JSValue construct(JSValue callee, JSValue newTarget, std::span<const JSValue> arguments);
    bool appendElementsOf(JSValue list, Values&);
    struct PriceOfBuiltin {
        uint64_t handed { 0 };
        uint64_t characters { 0 };
        uint64_t stepsOfSearch { 0 };
        bool holdsWhatNobodyKnows { false };
        uint64_t laidOut { 0 };
        uint64_t walked { 0 };
        Values* ropesHanded { nullptr };
        unsigned termsThatVary { 0 };
        WaysThroughChoices waysThroughChoices;
        bool mayMatch { false };

        uint64_t operations() const { return handed + std::max(characters, stepsOfSearch) / charactersGoneOverAtPriceOfOneOperation; }
        void takeText(JSString* text, bool byPiece)
        {
            (byPiece ? handed : characters) += text->length();
            if (!text->isRope())
                return;
            if (ropesHanded)
                ropesHanded->append(text);
            else
                laidOut += text->length();
        }
    };
    OptionSet<GrowthOfWork> workOf(JSValue builtin) const
    {
        if (!builtin || !builtin.isCell())
            return { };
        auto listed = m_workOfBuiltins.find(builtin.asCell());
        return listed == m_workOfBuiltins.end() ? OptionSet<GrowthOfWork>() : listed->value;
    }
    bool passesWhatIsLeft(const PriceOfBuiltin& price) const { return std::max(price.operations(), price.walked) >= std::min<uint64_t>(m_operationsLeft, m_operationsLeftInAll); }
    bool take(JSValue, OptionSet<GrowthOfWork>, PriceOfBuiltin&, unsigned depth);
    JSObject* originalOf(JSValue wrapper) const { return wrapper && wrapper.isCell() ? m_originalsOfWrappers.get(wrapper.asCell()) : nullptr; }
    JSValue inHandsOfProgram(JSValue);
    void exchangeBuiltinsHeldBy(JSValue made, unsigned levels);
    bool pay(const PriceOfBuiltin&);
    bool payForWalkThatEndedInRefusal(const PriceOfBuiltin&);
    bool payForBuiltin(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments, uint64_t& charactersHanded, Values& ropesHanded);
    bool wouldBeConverted(OptionSet<GrowthOfWork>, JSValue thisValue, std::span<const JSValue> arguments) const;
    bool wouldMakeMoreThanAnybodyKeeps(OptionSet<GrowthOfWork>, JSValue thisValue, std::span<const JSValue> arguments, const PriceOfBuiltin&);
    bool payToLayOut(JSValue);
    void noteTextMade(JSValue);
    void noteLaidOutByBuiltin(Values& ropesHanded);
    JSValue readProperty(JSValue object, PropertyName);
    bool mayBeMadeKey(JSValue value) { return mayBeConverted(value) && payToLayOut(value); }
    void payForPlacesMadeOutOfCharacters(uint64_t places, uint64_t characters);
    void payForWhatIsGoneOver(uint64_t width) { payForPlacesMadeOutOfCharacters(width / charactersGoneOverAtPriceOfOneOperation, width); }
    bool payForConversion(JSValue);
    bool payFor(JSValue);
    bool payForNamesListedOf(JSObject*);
    unsigned termsThatVaryIn(RegExp*);
    WaysThroughChoices waysThroughChoicesIn(RegExp*);
    bool mayBeConverted(JSValue value) { return isKnown(value) && payForConversion(value); }
    std::optional<uint64_t> elementsOfObjectHandedToBuiltin(JSObject*, bool lengthIsRead, uint64_t* holdersPassed = nullptr) const;
    bool takeRawOf(JSValue, OptionSet<GrowthOfWork>, PriceOfBuiltin&);
    bool payForHoldersPassed(JSValue, JSObject* last = nullptr);
    JSValue noteMadeByBuiltin(JSValue, uint64_t charactersHanded);
    bool mayMatchWithoutEnd(RegExp*);
    bool isAsRealmMadeIt(RegExpObject* pattern) { return pattern->structure() == m_realm->regExpStructure() && pattern->getLastIndex().isNumber() && m_realm.get()->regExpPrimordialPropertiesWatchpointSet().isStillValid(); }
    void noteThrown(Exception*);
    bool hasThrown(TopExceptionScope&);
    void letGoOfWhatWasThrown(TopExceptionScope&);
    void payForFramesThatEngineWalks();
    JSArray* newList();
    void noteObjectMade() { m_bytesMade += JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity); }
    void notePlaceFilled();
    static uint64_t elementsOf(JSObject* holder) { return hasAnyArrayStorage(holder->indexingType()) ? holder->butterfly()->arrayStorage()->m_numValuesInVector : holder->getArrayLength(); }
    static uint64_t placesWithRoomIn(JSObject* holder)
    {
        uint64_t elements = elementsOf(holder);
        return (elements ? roundUpToPowerOfTwo(elements) : 0) + holder->structure()->outOfLineCapacity();
    }
    static uint64_t placesInEntriesOf(JSObject* holder)
    {
        if (auto* map = dynamicDowncast<JSMap>(holder))
            return static_cast<uint64_t>(map->size()) * placesOfEntryInMap;
        if (auto* set = dynamicDowncast<JSSet>(holder))
            return static_cast<uint64_t>(set->size()) * placesOfEntryInSet;
        SparseArrayValueMap* sparse = hasAnyArrayStorage(holder->indexingType()) ? holder->butterfly()->arrayStorage()->m_sparseMap.get() : nullptr;
        return sparse ? static_cast<uint64_t>(sparse->size()) * placesOfEntryInSet : 0;
    }
    void noteRoomMadeIn(JSObject* holder, uint64_t placesBefore, uint64_t placesInEntriesBefore);
    void noteStructureMade(JSValue);
    void noteStructuresMadeIn(JSValue);

    class StoreThatMayMakeRoom {
        WTF_MAKE_NONCOPYABLE(StoreThatMayMakeRoom);
    public:
        StoreThatMayMakeRoom(RehearsalOfRegExpTexts& rehearsal, JSObject* holder)
            : m_rehearsal(rehearsal)
            , m_holder(holder)
            , m_placesBefore(placesWithRoomIn(holder))
            , m_placesInEntriesBefore(placesInEntriesOf(holder))
        {
        }

        ~StoreThatMayMakeRoom() { m_rehearsal.noteRoomMadeIn(m_holder, m_placesBefore, m_placesInEntriesBefore); }

    private:
        RehearsalOfRegExpTexts& m_rehearsal;
        JSObject* m_holder;
        uint64_t m_placesBefore;
        uint64_t m_placesInEntriesBefore;
    };
    bool append(JSArray*, JSValue);
    bool storeAt(JSObject*, unsigned index, JSValue);
    bool storeUnder(JSObject*, const Identifier&, JSValue);
    bool setLengthOf(JSArray*, unsigned);
    JSValue elementAt(JSArray*, unsigned index);
    bool areStrictlyEqual(JSValue, JSValue);
    void end()
    {
        m_operationsLeftInAll = 0;
        m_isOver = true;
    }

    class StartThatDrawsOnModule {
        WTF_MAKE_NONCOPYABLE(StartThatDrawsOnModule);
    public:
        StartThatDrawsOnModule(RehearsalOfRegExpTexts& rehearsal, ModuleID module, uint32_t mostOperations)
            : m_rehearsal(rehearsal)
            , m_module(module)
            , m_operationsLeft(rehearsal.m_operationsLeft)
            , m_operationsLeftInAll(rehearsal.m_operationsLeftInAll)
        {
            uint64_t left = rehearsal.m_isOver || !module ? 0 : rehearsal.m_operationsLeftInAllOfModules[module.index()];
            rehearsal.m_operationsLeftInAll = left;
            rehearsal.m_operationsLeft = static_cast<uint32_t>(std::min<uint64_t>(left, mostOperations));
        }

        ~StartThatDrawsOnModule()
        {
            if (m_module)
                m_rehearsal.m_operationsLeftInAllOfModules[m_module.index()] = m_rehearsal.m_operationsLeftInAll;
            m_rehearsal.m_operationsLeft = m_operationsLeft;
            m_rehearsal.m_operationsLeftInAll = m_rehearsal.m_isOver ? 0 : m_operationsLeftInAll;
        }

    private:
        RehearsalOfRegExpTexts& m_rehearsal;
        ModuleID m_module;
        uint32_t m_operationsLeft;
        uint64_t m_operationsLeftInAll;
    };

    JSValue execute(Activation&, const BodyValue&);
    bool takeEdge(Activation&, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    BlockID goEachWayFrom(Activation&, BodyBlock, JSValue& result);
    BlockID joinBehind(Activation&, BlockID);
    void runFrom(Activation&, BlockID, JSValue& result);
    void runOneOfWays(Activation&, BlockID, JSValue& result, uint32_t numberOfWaysLeft);
    JSValue runBlocks(Activation&);
    JSValue enter(FunctionID, std::span<const JSValue> captures, JSValue thisValue, JSValue callee, JSValue newTarget, std::span<const JSValue> arguments);
    JSValue enterWithNothingKnown(FunctionID);
    JSValue enterWithEntryParameters(FunctionID, std::span<const JSValue>);
    void runModule(ModuleID);
    bool rehearse(FunctionID);

    const Program& m_program;
    const BodyIndexTable& m_bodies;
    const ThinSummaries& m_summaries;
    static constexpr uint32_t m_longestText = longestRegExpPatternWithCode;

    VM* const m_vm;
    Strong<JSGlobalObject> m_realm;
    Strong<JSObject> m_unknown;
    Strong<JSObject> m_absent;
    Strong<JSObject> m_whatMarksWhatNobodyKnows;
    JSValue m_regExpConstructor;
    JSValue m_objectAssign;
    UncheckedKeyHashSet<JSCell*> m_functionsThatAskTheWorld;
    UncheckedKeyHashMap<JSCell*, OptionSet<GrowthOfWork>> m_workOfBuiltins;
    UncheckedKeyHashMap<JSCell*, JSObject*> m_originalsOfWrappers;
    UncheckedKeyHashMap<JSCell*, JSObject*> m_wrappersOfOriginals;
    Values m_wrappersMade;
    UncheckedKeyHashSet<JSCell*> m_handsOnItsReceiver;
    JSValue m_functionPrototypeApply;
    JSValue m_objectSetPrototypeOf;
    UncheckedKeyHashMap<uint32_t, uint64_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfData;
    bool m_callGoesStraightToProgram { false };
    bool m_receiverOfBuiltinIsText { false };
    bool m_builtinAsksWhetherWhatIsGivenBackIsTrue { false };
    Values m_builtins;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_texts;
    Values m_textsMade;
    UncheckedKeyHashMap<uint32_t, Identifier, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_names;
    Values m_variables;
    Values m_resultsOfModules;
    Values m_moduleObjects;
    Values m_namespaces;
    Values m_handlers;
    UncheckedKeyHashSet<JSCell*> m_handlersListed;
    bool m_goesThroughStartOfProgram { false };
    Identifier m_numberOfFunction;
    Identifier m_capturesOfFunction;
    Identifier m_markOfPatternMadeHere;
    Identifier m_markOfWhatNobodyKnows;
    Identifier m_laidUnderNumbersThatNobodyKnows;
    UncheckedKeyHashMap<uint64_t, Identifier> m_privateNames;
    UncheckedKeyHashMap<uint32_t, Vector<BlockID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_joins;
    Vector<StateOfModule> m_statesOfModules;
    Vector<ModuleID> m_modulesOfRuntimeModules;
    BitVector m_rehearsed;

    bool hasRoomLeft() const { return m_bytesMade <= mostBytesMade; }

    uint32_t m_operationsLeft { 0 };
    uint64_t m_operationsLeftInAll { operationsFromOneStart };
    Vector<uint64_t> m_operationsInAllOfModules;
    Vector<uint64_t> m_operationsLeftInAllOfModules;
    bool m_isOver { false };
    size_t m_bytesMade { 0 };
    uint32_t m_placesFilledSinceOneWasPaidFor { 0 };
    uint64_t m_bytesThatParsesMayHaveMade { 0 };
    unsigned m_depth { 0 };
    unsigned m_openRegions { 0 };
    Vector<unsigned> m_regionsOpenAtEntryPlusOne;
    bool m_sawTextThatNobodyKnows { false };
    uint64_t m_operation { 0 };
    UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_callsOfRegExp;

    UncheckedKeyHashMap<std::pair<String, uint32_t>, bool> m_patternsThatMayMatchWithoutEnd;
    UncheckedKeyHashMap<std::pair<String, uint32_t>, unsigned> m_termsThatVaryInPatterns;
    UncheckedKeyHashMap<std::pair<String, uint32_t>, WaysThroughChoices> m_waysThroughChoicesInPatterns;
    UncheckedKeyHashSet<std::pair<String, uint32_t>> m_seen;
    Vector<RegExpTextWorkedOut> m_found;
};

RehearsalOfRegExpTexts::RehearsalOfRegExpTexts(VM& vm, const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries)
    : m_program(program)
    , m_bodies(bodies)
    , m_summaries(summaries)
    , m_vm(&vm)
{
    m_realm.set(vm, JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull())));
    JSGlobalObject* realm = m_realm.get();
    realm->setEvalEnabled(false, "the rehearsal evaluates no text"_s);
    realm->setWebAssemblyEnabled(false, "the rehearsal compiles no module"_s);
    realm->weakRandom().setSeed(0);
    asObject(realm->errorPrototype()->getDirect(vm, vm.propertyNames->constructor))->putDirect(vm, vm.propertyNames->stackTraceLimit, jsNumber(0));

    m_whatMarksWhatNobodyKnows.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
    m_markOfWhatNobodyKnows = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "unknown"_s));
    m_unknown.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
    m_unknown->putDirect(vm, m_markOfWhatNobodyKnows, m_whatMarksWhatNobodyKnows.get());
    m_unknown->putDirect(vm, vm.propertyNames->toPrimitiveSymbol, JSNativeStdFunction::create(vm, realm, 0, String(), [](JSGlobalObject* realm, CallFrame*) {
        VM& vm = realm->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        return throwVMTypeError(realm, scope, "nobody knows the value"_s);
    }));

    m_absent.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
    m_numberOfFunction = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "function"_s));
    m_capturesOfFunction = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "captures"_s));
    m_markOfPatternMadeHere = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "made"_s));
    m_laidUnderNumbersThatNobodyKnows = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "numbers"_s));

    const BuiltinTable& table = program.builtins();
    auto leaveEmpty = [](JSValue*) { };
    m_builtins.fill(vm, table.builtinCount(), leaveEmpty);
    m_variables.fill(vm, program.variables.size(), leaveEmpty);
    m_resultsOfModules.fill(vm, program.modules.size(), leaveEmpty);
    m_moduleObjects.fill(vm, program.modules.size(), leaveEmpty);
    m_namespaces.fill(vm, program.modules.size(), leaveEmpty);
    m_statesOfModules.fill(StateOfModule::IsNotEntered, program.modules.size());
    for (uint32_t index = program.modules.size(); index--;) {
        const Module& module = program.modules[index];
        if (!module.runtimeModule || !module.body)
            continue;
        if (m_modulesOfRuntimeModules.size() <= module.runtimeModule.index())
            m_modulesOfRuntimeModules.grow(module.runtimeModule.index() + 1);
        m_modulesOfRuntimeModules[module.runtimeModule.index()] = ModuleID(index);
    }
    m_rehearsed.ensureSize(program.functions.size());
    m_regionsOpenAtEntryPlusOne.fill(0, program.functions.size());
    m_operationsInAllOfModules.fill(operationsInAllOfSmallestModule, program.modules.size());
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(static_cast<uint32_t>(index));
        if (ModuleID module = program.at(function).module)
            m_operationsInAllOfModules[module.index()] += operationsInAllForEachOfModule * summaries.at(function).numberOfOperations;
    }
    m_operationsLeftInAllOfModules = m_operationsInAllOfModules;
    for (uint64_t& left : m_operationsLeftInAllOfModules)
        left += operationsMoreForBodyInStartOfProgram;

    m_regExpConstructor = builtinInRealm(Builtins::RegExp);
    m_objectAssign = builtinInRealm(Builtins::ObjectAssign);
    auto inRealmByToken = [&](ASCIILiteral token) {
        BuiltinID id = table.builtinFromToken(token);
        RELEASE_ASSERT(id);
        return builtinInRealm(id);
    };
    auto list = [&](std::span<const ASCIILiteral> tokens, OptionSet<GrowthOfWork> work) {
        for (ASCIILiteral token : tokens) {
            if (JSValue function = inRealmByToken(token); isKnown(function) && function.isCell())
                m_workOfBuiltins.add(function.asCell(), work);
        }
    };
    list(builtinsWhoseWorkGrowsWithWhatIsHanded, { GrowthOfWork::WithWhatIsHanded });
    list(builtinsWhoseWorkGrowsWithNumbersHanded, { GrowthOfWork::WithWhatIsHanded, GrowthOfWork::WithNumbersHanded });
    list(builtinsWhoseWorkGrowsWithPropertiesOfWhatIsHanded, { GrowthOfWork::WithWhatIsHanded, GrowthOfWork::WithPropertiesOfWhatIsHanded });
    list(builtinsWhoseWorkGrowsWithAllThatIsReached, { GrowthOfWork::WithWhatIsHanded, GrowthOfWork::WithPropertiesOfWhatIsHanded, GrowthOfWork::WithAllThatIsReached });
    auto mark = [&](std::span<const ASCIILiteral> tokens, GrowthOfWork growth) {
        for (ASCIILiteral token : tokens) {
            JSValue function = inRealmByToken(token);
            if (auto listed = isKnown(function) && function.isCell() ? m_workOfBuiltins.find(function.asCell()) : m_workOfBuiltins.end(); listed != m_workOfBuiltins.end())
                listed->value.add(growth);
        }
    };
    mark(builtinsThatCopyWhatIsHeld, GrowthOfWork::CopiesWhatIsHeld);
    mark(builtinsThatCopyWhatIsHeldIntoStructuresOfStructures, GrowthOfWork::CopiesWhatIsHeldIntoStructuresOfStructures);
    mark(builtinsThatGoThroughWhatIsHandedByItsLength, GrowthOfWork::WithLengthOfWhatIsHanded);
    mark(builtinsThatTouchOnePlaceOfWhatTheyAreHanded, GrowthOfWork::TouchesOnePlaceOfWhatIsHanded);
    mark(builtinsThatGoOverTheirArguments, GrowthOfWork::GoesOverItsArguments);
    mark(builtinsThatMakePatternOfTextHanded, GrowthOfWork::MakesPatternOfTextHanded);
    mark(builtinsThatAskWhetherWhatIsGivenBackIsTrue, GrowthOfWork::AsksWhetherWhatIsGivenBackIsTrue);
    mark(builtinsThatGoIntoWhatTheyAreHandedFirst, GrowthOfWork::GoesIntoWhatItIsHandedFirst);
    mark(builtinsThatAskIdentityOfWhatIsHeld, GrowthOfWork::AsksIdentityOfWhatIsHeld);
    m_objectSetPrototypeOf = inRealmByToken("ObjectSetPrototypeOf"_s);
    for (auto [token, growth] : { std::pair { "ArrayPrototypeJoin"_s, GrowthOfWork::LaysTextBetweenPlaces }, std::pair { "StringPrototypeReplace"_s, GrowthOfWork::LaysTextOverFind }, std::pair { "StringPrototypeReplaceAll"_s, GrowthOfWork::LaysTextOverEveryFind }, std::pair { "JSONParse"_s, GrowthOfWork::MakesCellsOfCharactersHanded } })
        mark(std::span(&token, 1), growth);
    for (ASCIILiteral token : { "FunctionPrototypeCall"_s, "FunctionPrototypeApply"_s }) {
        if (JSValue function = inRealmByToken(token); isKnown(function) && function.isCell())
            m_handsOnItsReceiver.add(function.asCell());
        m_functionPrototypeApply = inRealmByToken(token);
    }
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        BuiltinID id(index);
        const Builtin& builtin = table.at(id);
        if (builtin.origin != BuiltinOrigin::Engine)
            continue;
        bool asksTheWorld = builtin.flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode, BuiltinFlag::StartsInstance });
        for (const BuiltinOverload& overload : table.overloadsOf(id))
            asksTheWorld |= overload.effects.reads.contains(AbstractHeap::ExternalState) || overload.effects.writes.contains(AbstractHeap::ExternalState);
        if (!asksTheWorld)
            continue;
        if (JSValue function = builtinInRealm(id); isKnown(function) && function.isCell())
            m_functionsThatAskTheWorld.add(function.asCell());
    }
}

String RehearsalOfRegExpTexts::charactersOf(StringID id) const
{
    Vector<char16_t> buffer;
    String characters = m_program.strings.viewOf(id, buffer).toString();
    return characters.is8Bit() || !characters.containsOnlyLatin1() ? characters : String::make8Bit(characters.span16());
}

JSValue RehearsalOfRegExpTexts::textOf(StringID id)
{
    return m_textsMade[m_texts.ensure(id.index(), [&] {
        m_textsMade.append(jsString(*m_vm, charactersOf(id)));
        return static_cast<uint32_t>(m_textsMade.size() - 1);
    }).iterator->value];
}

Identifier RehearsalOfRegExpTexts::nameOf(PropertyKeyID id)
{
    const PropertyKey& key = m_program.at(id);
    if (key.kind != PropertyKeyKind::Text || !key.text)
        return { };
    return m_names.ensure(id.index(), [&] {
        return Identifier::fromString(*m_vm, charactersOf(key.text));
    }).iterator->value;
}

JSValue RehearsalOfRegExpTexts::builtinInRealm(BuiltinID id)
{
    JSValue& found = m_builtins[id.index()];
    if (!found) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
        auto cell = findInRealm(m_realm.get(), BuiltinCellKey::object(id));
        letGoOfWhatWasThrown(scope);
        found = cell && *cell ? JSValue(*cell) : unknown();
    }
    return found;
}

void RehearsalOfRegExpTexts::payForFramesThatEngineWalks()
{
    m_operationsLeft -= std::min<uint32_t>(m_depth, m_operationsLeft);
    m_operationsLeftInAll -= std::min<uint64_t>(m_depth, m_operationsLeftInAll);
}

void RehearsalOfRegExpTexts::letGoOfWhatWasThrown(TopExceptionScope& scope)
{
    if (scope.exception())
        payForFramesThatEngineWalks();
    scope.clearException();
}

bool RehearsalOfRegExpTexts::hasThrown(TopExceptionScope& scope)
{
    Exception* exception = scope.exception();
    if (!exception)
        return false;
    noteThrown(exception);
    letGoOfWhatWasThrown(scope);
    return true;
}

void RehearsalOfRegExpTexts::notePlaceFilled()
{
    m_bytesMade += sizeof(JSValue);
    if (++m_placesFilledSinceOneWasPaidFor < valuesMadeRoomForAtPriceOfOneOperation)
        return;
    m_placesFilledSinceOneWasPaidFor = 0;
    payForPlacesMadeOutOfCharacters(1, 1);
}

ALWAYS_INLINE void RehearsalOfRegExpTexts::noteRoomMadeIn(JSObject* holder, uint64_t placesBefore, uint64_t placesInEntriesBefore)
{
    uint64_t places = placesWithRoomIn(holder);
    uint64_t made = places > placesBefore ? nextLength(elementsOf(holder)) + holder->structure()->outOfLineCapacity() : 0;
    if (uint64_t placesInEntries = placesInEntriesOf(holder); placesInEntries > placesInEntriesBefore)
        made += placesInEntries - placesInEntriesBefore;
    m_bytesMade += made * sizeof(JSValue);
    payForPlacesMadeOutOfCharacters(made / valuesMadeRoomForAtPriceOfOneOperation, made);
}

void RehearsalOfRegExpTexts::noteStructureMade(JSValue made)
{
    if (!made || !made.isObject())
        return;
    noteObjectMade();
    m_bytesMade += placesWithRoomIn(asObject(made)) * sizeof(JSValue);
}

void RehearsalOfRegExpTexts::noteStructuresMadeIn(JSValue made)
{
    if (!made.isObject())
        return;
    JSObject* holder = asObject(made);
    if (auto* list = dynamicDowncast<JSArray>(holder)) {
        for (unsigned index = 0; index < list->length(); ++index)
            noteStructureMade(list->tryGetIndexQuickly(index));
        return;
    }
    noteStructureMade(holder);
    holder->structure()->forEachProperty(*m_vm, [&](const PropertyTableEntry& entry) {
        noteStructureMade(holder->getDirect(entry.offset()));
        return true;
    });
}

JSArray* RehearsalOfRegExpTexts::newList()
{
    m_bytesMade += sizeof(JSArray) + sizeof(IndexingHeader);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSArray* list = constructEmptyArray(m_realm.get(), nullptr);
    return hasThrown(scope) ? nullptr : list;
}

bool RehearsalOfRegExpTexts::append(JSArray* list, JSValue element)
{
    notePlaceFilled();
    StoreThatMayMakeRoom store(*this, list);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    list->push(m_realm.get(), element);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::storeAt(JSObject* object, unsigned index, JSValue stored)
{
    notePlaceFilled();
    StoreThatMayMakeRoom store(*this, object);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    object->putDirectIndex(m_realm.get(), index, stored);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::storeUnder(JSObject* object, const Identifier& name, JSValue stored)
{
    notePlaceFilled();
    StoreThatMayMakeRoom store(*this, object);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    object->putDirectMayBeIndex(m_realm.get(), name, stored);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::setLengthOf(JSArray* list, unsigned length)
{
    StoreThatMayMakeRoom store(*this, list);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    list->setLength(m_realm.get(), length);
    return !hasThrown(scope);
}

JSValue RehearsalOfRegExpTexts::elementAt(JSArray* list, unsigned index)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue element = list->getIndex(m_realm.get(), index);
    return hasThrown(scope) ? unknown() : element;
}

bool RehearsalOfRegExpTexts::areStrictlyEqual(JSValue first, JSValue second)
{
    if (!payToLayOut(first) || !payToLayOut(second))
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    bool areEqual = JSValue::strictEqual(m_realm.get(), first, second);
    return !hasThrown(scope) && areEqual;
}

JSValue RehearsalOfRegExpTexts::listOf(std::span<const JSValue> elements)
{
    JSArray* list = newList();
    for (JSValue element : elements) {
        if (!list || !append(list, element))
            return unknown();
    }
    return list ? JSValue(list) : unknown();
}

JSValue RehearsalOfRegExpTexts::valueOfData(const DataNode& node)
{
    if (node.is<DataNode::Undefined>())
        return jsUndefined();
    if (node.is<DataNode::Null>())
        return jsNull();
    if (const auto* boolean = node.tryAs<DataNode::Boolean>())
        return jsBoolean(boolean->value);
    if (const auto* number = node.tryAs<DataNode::Number>())
        return jsNumber(number->value);
    if (const auto* string = node.tryAs<DataNode::String>())
        return textOf(string->value);
    if (const auto* array = node.tryAs<DataNode::Array>()) {
        JSArray* list = newList();
        unsigned index = 0;
        for (const DataNode& element : array->elements) {
            if (!list || (!element.is<DataNode::Hole>() && !storeAt(list, index, valueOfData(element))))
                return unknown();
            ++index;
        }
        return list && setLengthOf(list, index) ? JSValue(list) : unknown();
    }
    if (const auto* object = node.tryAs<DataNode::Object>()) {
        noteObjectMade();
        JSObject* made = constructEmptyObject(m_realm.get());
        for (const DataMember& member : object->members) {
            if (Identifier name = nameOf(member.key); !name.isNull() && !storeUnder(made, name, valueOfData(member.value)))
                return unknown();
        }
        return made;
    }
    return unknown();
}

JSValue RehearsalOfRegExpTexts::closureOf(FunctionID function, std::span<const JSValue> captures)
{
    const Function& record = m_program.at(function);
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    JSValue list = listOf(captures);
    if (!isKnown(list))
        return unknown();
    m_bytesMade += sizeof(JSNativeStdFunction);
    noteObjectMade();
    JSArray* held = asArray(list);
    bool isGoneThrough = !record.has(FunctionFlags::IsGenerator) && record.hasBody();
    JSObject* made = JSNativeStdFunction::create(vm, realm, record.length, String(), [this, function, held, isGoneThrough](JSGlobalObject* caller, CallFrame* frame) {
        bool givesBackToBuiltin = !std::exchange(m_callGoesStraightToProgram, false);
        bool receiverOfBuiltinIsText = m_receiverOfBuiltinIsText;
        bool isAskedWhetherTrue = givesBackToBuiltin && m_builtinAsksWhetherWhatIsGivenBackIsTrue;
        if (!m_operationsLeft || !m_operationsLeftInAll || (isAskedWhetherTrue && !isGoneThrough)) {
            auto scope = DECLARE_THROW_SCOPE(*m_vm);
            return throwVMTypeError(caller, scope, "nobody knows the value"_s);
        }
        --m_operationsLeft;
        --m_operationsLeftInAll;
        if (!isGoneThrough)
            return JSValue::encode(unknown());
        Values captures;
        appendElementsOf(held, captures);
        Values arguments;
        for (unsigned index = 0; index < frame->argumentCount(); ++index)
            arguments.append(frame->uncheckedArgument(index));
        JSValue given = enter(function, captures.span(), frame->thisValue(), frame->jsCallee(), jsUndefined(), arguments.span());
        if (givesBackToBuiltin && isKnown(given)) {
            PriceOfBuiltin price;
            bool isTaken = !(receiverOfBuiltinIsText && given.isObject()) && take(given, { GrowthOfWork::WithWhatIsHanded }, price, 0);
            price.mayMatch = false;
            if (!isTaken || !pay(price))
                given = unknown();
        }
        if (isAskedWhetherTrue && !isKnown(given)) {
            auto scope = DECLARE_THROW_SCOPE(*m_vm);
            return throwVMTypeError(caller, scope, "nobody knows the value"_s);
        }
        return JSValue::encode(given);
    }, held);
    made->putDirect(vm, m_numberOfFunction, jsNumber(function.index()));
    made->putDirect(vm, m_capturesOfFunction, held);
    return made;
}

RehearsalOfRegExpTexts::ClosureOfProgram RehearsalOfRegExpTexts::closureIn(JSValue value) const
{
    if (!value.inherits<JSNativeStdFunction>())
        return { };
    JSValue number = asObject(value)->getDirect(*m_vm, m_numberOfFunction);
    if (!number)
        return { };
    return { FunctionID(number.asUInt32()), asArray(asObject(value)->getDirect(*m_vm, m_capturesOfFunction)) };
}

void RehearsalOfRegExpTexts::givePrototypeTo(JSValue closure)
{
    VM& vm = *m_vm;
    FunctionID function = closureIn(closure).function;
    if (!function || asObject(closure)->getDirect(vm, vm.propertyNames->prototype))
        return;
    const Function& record = m_program.at(function);
    if (record.kind != FunctionKind::Plain || record.isCoroutine() || !record.hasBody())
        return;
    noteObjectMade();
    JSObject* prototype = constructEmptyObject(m_realm.get());
    prototype->putDirect(vm, vm.propertyNames->constructor, closure, static_cast<unsigned>(PropertyAttribute::DontEnum));
    asObject(closure)->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

JSValue RehearsalOfRegExpTexts::variable(VariableID id)
{
    JSValue& held = m_variables[id.index()];
    if (held)
        return held;
    const Variable& record = m_program.at(id);
    if (record.initialization == VariableInitialization::Function && record.function)
        return held = closureOf(record.function, { });
    runModule(record.module);
    if (held)
        return held;
    const Module& module = m_program.at(record.module);
    for (const SyntheticExport& synthetic : module.syntheticExports) {
        if (synthetic.variable != id || module.requests.size() != 1)
            continue;
        JSValue exported = valueOfCommonJsModule(module.requests[0].target);
        if (!isKnown(exported))
            return unknown();
        String name = charactersOf(synthetic.name);
        if (name == "default"_s)
            return held = exported;
        if (!exported.isObject())
            return unknown();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
        JSValue member = exported.get(m_realm.get(), Identifier::fromString(*m_vm, name));
        if (scope.exception()) {
            letGoOfWhatWasThrown(scope);
            return unknown();
        }
        return held = member;
    }
    return record.initialization == VariableInitialization::Undefined && m_statesOfModules[record.module.index()] == StateOfModule::HasRun ? jsUndefined() : unknown();
}

JSValue RehearsalOfRegExpTexts::moduleObjectOf(ModuleID id)
{
    JSValue& held = m_moduleObjects[id.index()];
    if (held)
        return held;
    VM& vm = *m_vm;
    noteObjectMade();
    JSObject* made = constructEmptyObject(m_realm.get());
    const Module& module = m_program.at(id);
    JSValue exported = module.exportsVariable ? m_variables[module.exportsVariable.index()] : JSValue();
    made->putDirect(vm, Identifier::fromString(vm, "exports"_s), exported ? exported : JSValue(constructEmptyObject(m_realm.get())));
    return held = made;
}

JSValue RehearsalOfRegExpTexts::valueOfCommonJsModule(ModuleID id)
{
    runModule(id);
    const Module& module = m_program.at(id);
    if (JSValue object = m_moduleObjects[id.index()])
        return asObject(object)->getDirect(*m_vm, Identifier::fromString(*m_vm, "exports"_s));
    if (!module.exportsVariable)
        return unknown();
    JSValue exported = m_variables[module.exportsVariable.index()];
    return exported ? exported : unknown();
}

JSValue RehearsalOfRegExpTexts::namespaceOf(ModuleID id)
{
    VM& vm = *m_vm;
    if (JSValue held = m_namespaces[id.index()])
        return held;
    runModule(id);
    const Module& module = m_program.at(id);
    noteObjectMade();
    JSObject* made = constructEmptyObject(m_realm.get());
    uint64_t numberOfExports = module.exports.size() + module.syntheticExports.size();
    payForPlacesMadeOutOfCharacters(numberOfExports, numberOfExports);
    for (const ModuleExport& exported : module.exports) {
        if (exported.kind == ModuleExportKind::Variable && exported.variable)
            made->putDirect(vm, Identifier::fromString(vm, charactersOf(exported.name)), variable(exported.variable));
    }
    for (const SyntheticExport& synthetic : module.syntheticExports) {
        if (synthetic.variable)
            made->putDirect(vm, Identifier::fromString(vm, charactersOf(synthetic.name)), variable(synthetic.variable));
    }
    if (m_statesOfModules[id.index()] == StateOfModule::HasRun)
        m_namespaces[id.index()] = made;
    return made;
}

JSValue RehearsalOfRegExpTexts::prototypeFor(JSValue newTarget)
{
    JSGlobalObject* realm = m_realm.get();
    if (!isKnown(newTarget) || !newTarget.isObject())
        return realm->objectPrototype();
    givePrototypeTo(newTarget);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue prototype = newTarget.get(realm, m_vm->propertyNames->prototype);
    if (scope.exception())
        letGoOfWhatWasThrown(scope);
    else if (isKnown(prototype) && prototype.isObject())
        return prototype;
    return realm->objectPrototype();
}

Identifier RehearsalOfRegExpTexts::privateNameOf(LayoutID layout, uint32_t field)
{
    return m_privateNames.ensure((static_cast<uint64_t>(layout.index()) + 1) << 32 | field, [&] {
        return Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "field"_s));
    }).iterator->value;
}

JSValue RehearsalOfRegExpTexts::notePatternMadeHere(JSValue made)
{
    if (made.inherits<RegExpObject>())
        asObject(made)->putDirect(*m_vm, m_markOfPatternMadeHere, jsBoolean(true));
    return made;
}

std::optional<bool> RehearsalOfRegExpTexts::isOfType(JSValue value, TypeID id) const
{
    Type type = m_program.at(id);
    if (!type.isJavaScriptValue())
        return std::nullopt;
    auto has = [&](auto primitives) {
        return !!(type.primitives & primitives);
    };
    if (value == JSValue(m_absent.get()))
        return has(Primitive::Absent);
    if (value.isUndefined())
        return has(Primitive::Undefined);
    if (value.isNull())
        return has(Primitive::Null);
    if (value.isBoolean())
        return has(value.asBoolean() ? Primitive::True : Primitive::False);
    if (value.isNumber()) {
        if (!has(Primitive::Number))
            return false;
        if ((type.primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full() && type.boundsOfFractions.isEmpty())
            return true;
        return std::nullopt;
    }
    if (value.isString()) {
        if (has(Primitive::String))
            return true;
        if (!has(Primitive::LiteralString))
            return false;
        return std::nullopt;
    }
    if (value.isSymbol())
        return has(Primitive::Symbol) ? std::optional(true) : has(Primitive::LiteralSymbol) ? std::nullopt : std::optional(false);
    if (value.isBigInt())
        return has(Primitive::BigInt);
    if (!value.isObject())
        return std::nullopt;
    if (!type.hasObjects())
        return false;
    bool isCallable = value.isCallable();
    if (isCallable ? type.anyFunction : type.anyObject)
        return true;
    if (!isCallable && !type.anyObject && !type.objects && !type.unnamedLayouts && !type.unnamedClasses)
        return false;
    return std::nullopt;
}

bool RehearsalOfRegExpTexts::noteTexts(std::span<const JSValue> arguments)
{
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue pattern = arguments.size() > 0 ? arguments[0] : jsUndefined();
    JSValue flags = arguments.size() > 1 ? arguments[1] : jsUndefined();
    uint64_t site = m_operation;
    m_callsOfRegExp.add(site, false);
    if (!isKnown(pattern) || (!pattern.inherits<RegExpObject>() && !payFor(pattern)) || (isKnown(flags) && !payFor(flags))) {
        m_sawTextThatNobodyKnows = true;
        return true;
    }

    String text = emptyString();
    std::optional<OptionSet<Yarr::Flags>> flagsOfText = OptionSet<Yarr::Flags>();
    if (auto* copied = dynamicDowncast<RegExpObject>(pattern)) {
        text = copied->regExp()->pattern();
        flagsOfText = copied->regExp()->flags();
    } else if (!pattern.isUndefined())
        text = pattern.toWTFString(realm);
    if (scope.exception()) {
        letGoOfWhatWasThrown(scope);
        m_sawTextThatNobodyKnows = true;
        return true;
    }

    bool knowsFlags = isKnown(flags);
    if (!knowsFlags) {
        m_sawTextThatNobodyKnows = true;
        flagsOfText = OptionSet<Yarr::Flags>();
    } else if (!flags.isUndefined()) {
        String written = flags.toWTFString(realm);
        if (scope.exception()) {
            letGoOfWhatWasThrown(scope);
            return true;
        }
        flagsOfText = Yarr::parseFlags(written);
    }
    m_callsOfRegExp.set(site, true);
    if (!flagsOfText || text.isNull() || text.length() > m_longestText)
        return true;

    uint16_t significant = ImageRegExp::significantFlags(*flagsOfText).toRaw();
    if (!m_seen.contains({ text, significant })) {
        uint64_t priceOfMaking = text.length() * operationsForCharacterOfPatternThatIsMade;
        if (priceOfMaking > m_operationsLeftInAll) {
            m_callsOfRegExp.set(site, false);
            m_sawTextThatNobodyKnows = true;
            return false;
        }
        m_operationsLeftInAll -= priceOfMaking;
        m_seen.add({ text, significant });
        m_found.append(RegExpTextWorkedOut { text.isolatedCopy(), significant, knowsFlags });
    }
    m_operationsLeft = std::max(m_operationsLeft, operationsFromOneStart);
    return true;
}

JSValue RehearsalOfRegExpTexts::makeNumberThatNobodyKnows()
{
    VM& vm = *m_vm;
    noteObjectMade();
    JSObject* number = constructEmptyObject(vm, m_realm->nullPrototypeObjectStructure());
    number->putDirect(vm, m_markOfWhatNobodyKnows, m_whatMarksWhatNobodyKnows.get());
    number->putDirect(vm, vm.propertyNames->toPrimitiveSymbol, m_unknown->getDirect(vm, vm.propertyNames->toPrimitiveSymbol));
    return number;
}

void RehearsalOfRegExpTexts::layUnderNumberThatNobodyKnows(JSObject* object, JSValue number, JSValue laid)
{
    VM& vm = *m_vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue found = object->getDirect(vm, m_laidUnderNumbersThatNobodyKnows);
    JSMap* all = found ? uncheckedDowncast<JSMap>(found) : JSMap::create(vm, m_realm->mapStructure());
    if (!found) {
        noteObjectMade();
        object->putDirect(vm, m_laidUnderNumbersThatNobodyKnows, all);
    }
    StoreThatMayMakeRoom store(*this, all);
    all->set(m_realm.get(), number, laid);
    hasThrown(scope);
}

JSValue RehearsalOfRegExpTexts::findUnderNumberThatNobodyKnows(JSObject* object, JSValue number)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue found = object->getDirect(*m_vm, m_laidUnderNumbersThatNobodyKnows);
    if (!found)
        return unknown();
    bool isLaid = uncheckedDowncast<JSMap>(found)->has(m_realm.get(), number);
    if (hasThrown(scope) || !isLaid)
        return unknown();
    JSValue laid = uncheckedDowncast<JSMap>(found)->get(m_realm.get(), number);
    return hasThrown(scope) ? unknown() : laid;
}

void RehearsalOfRegExpTexts::noteHandedToCodeThatNobodyKnows(std::span<const JSValue> arguments)
{
    if (!m_goesThroughStartOfProgram)
        return;
    VM& vm = *m_vm;
    auto list = [&](JSValue value) {
        if (closureIn(value).function && m_handlersListed.add(value.asCell()).isNewEntry)
            m_handlers.append(value);
    };
    for (JSValue argument : arguments) {
        list(argument);
        if (!isKnown(argument) || !argument.inherits<JSFinalObject>())
            continue;
        JSObject* options = asObject(argument);
        payForWhatIsGoneOver(options->structure()->maxOffset() + 1);
        options->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            list(options->getDirect(entry.offset()));
            return true;
        });
    }
}

ALWAYS_INLINE JSValue RehearsalOfRegExpTexts::inHandsOfProgram(JSValue value)
{
    if (!value || !value.isCallable() || value.inherits<JSNativeStdFunction>() || value.inherits<ProxyObject>())
        return value;
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    JSObject* original = asObject(value);
    if (JSObject* made = m_wrappersOfOriginals.get(original))
        return made;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue name = original->get(realm, vm.propertyNames->name);
    letGoOfWhatWasThrown(scope);
    JSValue length = original->get(realm, vm.propertyNames->length);
    letGoOfWhatWasThrown(scope);
    String nameOfWrapper = name && name.isString() ? name.toWTFString(realm) : String();
    letGoOfWhatWasThrown(scope);
    JSObject* wrapper = JSNativeStdFunction::create(vm, realm, length && length.isUInt32() ? length.asUInt32() : 0, nameOfWrapper, [this, original](JSGlobalObject* caller, CallFrame* frame) {
        if (!m_operationsLeft || !m_operationsLeftInAll || !hasRoomLeft()) {
            auto scope = DECLARE_THROW_SCOPE(*m_vm);
            return throwVMTypeError(caller, scope, "nobody knows the value"_s);
        }
        --m_operationsLeft;
        --m_operationsLeftInAll;
        m_bytesMade += sizeof(JSValue);
        Values arguments;
        for (unsigned index = 0; index < frame->argumentCount(); ++index)
            arguments.append(frame->uncheckedArgument(index));
        JSValue result = arguments.hasOverflowed() ? unknown() : call(original, frame->thisValue(), arguments.span());
        if (!isKnown(result)) {
            auto scope = DECLARE_THROW_SCOPE(*m_vm);
            return throwVMTypeError(caller, scope, "nobody knows the value"_s);
        }
        return JSValue::encode(result);
    }, original);
    wrapper->setPrototypeDirect(vm, original->getPrototypeDirect());
    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    original->methodTable()->getOwnPropertyNames(original, realm, names, DontEnumPropertiesMode::Include);
    letGoOfWhatWasThrown(scope);
    for (const Identifier& name : names) {
        if (name == vm.propertyNames->name || name == vm.propertyNames->length)
            continue;
        PropertySlot slot(original, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = original->methodTable()->getOwnPropertySlot(original, realm, name, slot);
        letGoOfWhatWasThrown(scope);
        if (!isFound)
            continue;
        if (slot.isAccessor())
            wrapper->putDirectAccessor(realm, name, slot.getterSetter(), slot.attributes());
        else if (slot.isValue())
            wrapper->putDirect(vm, name, slot.getValue(realm, name), slot.attributes());
        letGoOfWhatWasThrown(scope);
    }
    m_wrappersMade.append(wrapper);
    m_wrappersOfOriginals.add(original, wrapper);
    m_originalsOfWrappers.add(wrapper, original);
    return wrapper;
}

void RehearsalOfRegExpTexts::exchangeBuiltinsHeldBy(JSValue made, unsigned levels)
{
    if (!levels || !made.isObject())
        return;
    VM& vm = *m_vm;
    JSObject* object = asObject(made);
    if (object->classInfo() == JSArray::info()) {
        auto* list = uncheckedDowncast<JSArray>(object);
        for (unsigned index = 0; index < list->length(); ++index) {
            JSValue held = list->tryGetIndexQuickly(index);
            if (held && held.isCallable()) {
                JSValue wrapper = inHandsOfProgram(held);
                if (index < list->length())
                    list->setIndexQuickly(vm, index, wrapper);
            } else if (held)
                exchangeBuiltinsHeldBy(held, levels - 1);
        }
        return;
    }
    if (object->classInfo() != JSFinalObject::info())
        return;
    Vector<PropertyOffset, 8> offsets;
    object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        offsets.append(entry.offset());
        return true;
    });
    for (PropertyOffset offset : offsets) {
        JSValue held = object->getDirect(offset);
        if (held.isCallable())
            object->putDirectOffset(vm, offset, inHandsOfProgram(held));
        else
            exchangeBuiltinsHeldBy(held, levels - 1);
    }
}

JSValue RehearsalOfRegExpTexts::call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    if (JSObject* original = originalOf(callee))
        callee = original;
    if (JSObject* original = originalOf(thisValue); original && original->isConstructor() && !callee.inherits<JSNativeStdFunction>() && !m_handsOnItsReceiver.contains(callee.asCell()))
        thisValue = original;
    if (!isKnown(callee) || !callee.isCell() || m_functionsThatAskTheWorld.contains(callee.asCell())) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    if (callee == m_regExpConstructor && !noteTexts(arguments))
        return unknown();
    bool isOfProgram = callee.inherits<JSNativeStdFunction>();
    if (!isOfProgram && workOf(callee).isEmpty()) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    if (!isOfProgram && (!isKnown(thisValue) || (workOf(callee).contains(GrowthOfWork::GoesIntoWhatItIsHandedFirst) && !arguments.empty() && !isKnown(arguments[0]))))
        return unknown();
    if (callee == m_objectSetPrototypeOf && arguments.size() > 1 && !isKnown(arguments[1]))
        return unknown();
    if (isOfProgram && !arguments.empty() && (!isKnown(thisValue) || thisValue.isUndefined()) && std::ranges::none_of(arguments, [&](JSValue argument) { return isKnown(argument); }))
        return unknown();
    bool onlyHandsOn = m_handsOnItsReceiver.contains(callee.asCell());
    if (!isOfProgram && !onlyHandsOn && callee != m_regExpConstructor && (isPatternMadeHere(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return isPatternMadeHere(argument); })))
        return unknown();
    if (!isOfProgram && (asksTheWorld(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return asksTheWorld(argument); }))) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    CallData callData = getCallData(callee);
    if (callData.type == CallData::Type::None)
        return unknown();

    uint64_t charactersHanded = 0;
    Values ropesHanded;
    if (onlyHandsOn) {
        PriceOfBuiltin price;
        price.handed = arguments.size();
        for (JSValue argument : arguments) {
            if (auto* handedOn = dynamicDowncast<JSArray>(argument))
                price.handed += handedOn->length();
        }
        if (callee == m_functionPrototypeApply && arguments.size() > 1 && isKnown(arguments[1]) && arguments[1].isObject() && !arguments[1].inherits<JSArray>()) {
            std::optional<uint64_t> elements = elementsOfObjectHandedToBuiltin(asObject(arguments[1]), true);
            if (!elements)
                return unknown();
            price.handed += *elements;
        }
        if (!pay(price))
            return unknown();
    } else if (!isOfProgram && !payForBuiltin(callee, thisValue, arguments, charactersHanded, ropesHanded))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    NakedPtr<Exception> exception;
    SetForScope goesStraightToProgram(m_callGoesStraightToProgram, isOfProgram || onlyHandsOn);
    SetForScope receiverIsText(m_receiverOfBuiltinIsText, thisValue.isString());
    SetForScope asksWhetherTrue(m_builtinAsksWhetherWhatIsGivenBackIsTrue, !isOfProgram && workOf(callee).contains(GrowthOfWork::AsksWhetherWhatIsGivenBackIsTrue));
    std::optional<StoreThatMayMakeRoom> intoReceiver;
    std::optional<StoreThatMayMakeRoom> intoFirstArgument;
    if (!isOfProgram && isKnown(thisValue) && thisValue.isObject())
        intoReceiver.emplace(*this, asObject(thisValue));
    if (!isOfProgram && !arguments.empty() && isKnown(arguments[0]) && arguments[0].isObject())
        intoFirstArgument.emplace(*this, asObject(arguments[0]));
    JSValue result = JSC::call(realm, callee, callData, thisValue, list, exception);
    letGoOfWhatWasThrown(scope);
    noteLaidOutByBuiltin(ropesHanded);
    if (exception) {
        payForFramesThatEngineWalks();
        noteThrown(exception.get());
    }
    if (exception || !result)
        return unknown();
    if (callee == m_regExpConstructor)
        return notePatternMadeHere(result);
    if (isOfProgram)
        return result;
    if (!onlyHandsOn && !result.isObject() && std::ranges::any_of(arguments, [&](JSValue argument) { return !isKnown(argument); }))
        return unknown();
    if (workOf(callee).contains(GrowthOfWork::CopiesWhatIsHeld)) {
        exchangeBuiltinsHeldBy(result, 1);
        if (!isJSArray(result))
            noteStructureMade(result);
    } else if (workOf(callee).contains(GrowthOfWork::CopiesWhatIsHeldIntoStructuresOfStructures)) {
        exchangeBuiltinsHeldBy(result, 2);
        noteStructuresMadeIn(result);
    }
    if (result == thisValue || std::ranges::find(arguments, result) != arguments.end())
        return result;
    noteTextMade(result);
    return noteMadeByBuiltin(result, charactersHanded);
}

bool RehearsalOfRegExpTexts::mayMatchWithoutEnd(RegExp* regExp)
{
    return m_patternsThatMayMatchWithoutEnd.ensure({ regExp->pattern(), regExp->flags().toRaw() }, [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern pattern(regExp->pattern(), regExp->flags(), error);
        return Yarr::hasError(error) || repeatsInsideRepeat(*pattern.m_body, false);
    }).iterator->value;
}

WaysThroughChoices RehearsalOfRegExpTexts::waysThroughChoicesIn(RegExp* regExp)
{
    return m_waysThroughChoicesInPatterns.ensure({ regExp->pattern(), regExp->flags().toRaw() }, [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern pattern(regExp->pattern(), regExp->flags(), error);
        return Yarr::hasError(error) ? WaysThroughChoices { } : waysThroughChoicesOf(*pattern.m_body);
    }).iterator->value;
}

unsigned RehearsalOfRegExpTexts::termsThatVaryIn(RegExp* regExp)
{
    return m_termsThatVaryInPatterns.ensure({ regExp->pattern(), regExp->flags().toRaw() }, [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern pattern(regExp->pattern(), regExp->flags(), error);
        return Yarr::hasError(error) ? 0u : mostTermsThatVaryInOneAlternative(*pattern.m_body);
    }).iterator->value;
}

std::optional<uint64_t> RehearsalOfRegExpTexts::elementsOfObjectHandedToBuiltin(JSObject* object, bool lengthIsRead, uint64_t* holdersPassed) const
{
    auto above = [](JSObject* holder) {
        JSValue prototype = holder->getPrototypeDirect();
        return prototype.isObject() ? asObject(prototype) : nullptr;
    };
    for (JSObject* holder = object; holder; holder = above(holder)) {
        if (holdersPassed)
            ++*holdersPassed;
        if (holder->inherits<ProxyObject>() || holder->inherits<StringObject>())
            return std::nullopt;
        lengthIsRead |= holder == m_realm->arrayPrototype();
    }
    for (JSObject* holder = lengthIsRead ? object : nullptr; holder; holder = above(holder)) {
        auto* list = dynamicDowncast<JSArray>(holder);
        if (JSValue length = list ? jsNumber(list->length()) : holder->getDirect(*m_vm, m_vm->propertyNames->length)) {
            if (!length.isNumber() || !(length.asNumber() <= m_longestText))
                return std::nullopt;
            return length.asNumber() > 0 ? static_cast<uint64_t>(length.asNumber()) : 0;
        }
    }
    return 0;
}

bool RehearsalOfRegExpTexts::take(JSValue value, OptionSet<GrowthOfWork> work, PriceOfBuiltin& price, unsigned depth)
{
    if (value.isString()) {
        price.takeText(asString(value), false);
        return true;
    }
    if (value.isNumber())
        return !work.contains(GrowthOfWork::WithNumbersHanded) || value.asNumber() <= m_longestText;
    if (value.isHeapBigInt()) {
        uint64_t words = value.asHeapBigInt()->length();
        price.handed += words * words;
        return true;
    }
    if (!value.isObject())
        return true;
    if (!isKnown(value)) {
        price.holdsWhatNobodyKnows |= !!depth;
        return true;
    }
    if (depth >= deepestCall)
        return false;
    JSObject* object = asObject(value);
    if (auto* pattern = dynamicDowncast<RegExpObject>(object)) {
        if ((depth && isPatternMadeHere(value)) || !isAsRealmMadeIt(pattern) || mayMatchWithoutEnd(pattern->regExp()))
            return false;
        price.mayMatch = true;
        price.termsThatVary = std::max(price.termsThatVary, termsThatVaryIn(pattern->regExp()));
        WaysThroughChoices ways = waysThroughChoicesIn(pattern->regExp());
        price.waysThroughChoices.once = std::max(price.waysThroughChoices.once, ways.once);
        price.waysThroughChoices.forEachCharacter = std::max(price.waysThroughChoices.forEachCharacter, ways.forEachCharacter);
        return true;
    }
    bool touchesOnePlace = work.contains(GrowthOfWork::TouchesOnePlaceOfWhatIsHanded);
    if (auto* map = dynamicDowncast<JSMap>(object))
        price.handed += touchesOnePlace ? 1 : map->size();
    else if (auto* set = dynamicDowncast<JSSet>(object))
        price.handed += touchesOnePlace ? 1 : set->size();
    if (object->isCallable()) {
        bool isGoneThroughByLength = work.contains(GrowthOfWork::WithLengthOfWhatIsHanded);
        if (auto* proxy = dynamicDowncast<ProxyObject>(object))
            return !isGoneThroughByLength && !proxy->isRevoked() && take(proxy->target(), work, price, depth + 1);
        uint64_t holdersPassed = 0;
        std::optional<uint64_t> elements = elementsOfObjectHandedToBuiltin(object, isGoneThroughByLength, &holdersPassed);
        if (!elements)
            return false;
        if (*elements >= charactersGoneOverAtPriceOfOneOperation)
            price.handed += *elements;
        price.handed += holdersPassed / charactersGoneOverAtPriceOfOneOperation * (1 + *elements);
        if (work.contains(GrowthOfWork::WithPropertiesOfWhatIsHanded))
            price.handed += object->structure()->maxOffset() + 1;
        return !passesWhatIsLeft(price);
    }
    if (auto* iterator = dynamicDowncast<JSMapIterator>(object))
        return take(iterator->iteratedObject(), { GrowthOfWork::WithWhatIsHanded }, price, depth + 1);
    switch (object->type()) {
    case ObjectType:
    case FinalObjectType:
    case ArrayType:
    case DerivedArrayType:
    case JSMapType:
    case JSSetType:
    case JSWeakMapType:
    case JSWeakSetType:
    case JSPromiseType:
    case ErrorInstanceType:
        break;
    default:
        return false;
    }
    bool walksAll = work.contains(GrowthOfWork::WithAllThatIsReached);
    auto takePlaces = [&](uint64_t places) {
        price.handed += places;
        if (passesWhatIsLeft(price))
            return false;
        if (!hasContiguous(object->indexingType()) && !hasAnyArrayStorage(object->indexingType())) {
            price.walked += places;
            return true;
        }
        for (unsigned index = 0; index < places; ++index) {
            ++price.walked;
            JSValue element = object->tryGetIndexQuickly(index);
            if (!element || !element.isCell())
                continue;
            if (!take(element, work, price, depth + 1) || passesWhatIsLeft(price))
                return false;
        }
        return true;
    };
    uint64_t holdersPassed = 0;
    std::optional<uint64_t> elements = elementsOfObjectHandedToBuiltin(object, work.contains(GrowthOfWork::WithLengthOfWhatIsHanded), &holdersPassed);
    auto* list = dynamicDowncast<JSArray>(object);
    uint64_t placesThatAreRead = touchesOnePlace ? 1 : list ? list->length() : elements.value_or(0);
    price.handed += holdersPassed / charactersGoneOverAtPriceOfOneOperation * (1 + placesThatAreRead);
    if (touchesOnePlace) {
        ++price.handed;
        return (list || elements) && !passesWhatIsLeft(price);
    }
    if (list)
        return takePlaces(placesThatAreRead);
    if (!elements || !takePlaces(*elements))
        return false;
    if (!work.contains(GrowthOfWork::WithPropertiesOfWhatIsHanded) || (depth && !walksAll))
        return true;
    bool isTaken = true;
    object->structure()->forEachProperty(*m_vm, [&](const PropertyTableEntry& entry) {
        ++price.handed;
        ++price.walked;
        if (walksAll)
            isTaken = take(object->getDirect(entry.offset()), work, price, depth + 1);
        isTaken = isTaken && !passesWhatIsLeft(price);
        return isTaken;
    });
    return isTaken;
}

bool RehearsalOfRegExpTexts::takeRawOf(JSValue value, OptionSet<GrowthOfWork> work, PriceOfBuiltin& price)
{
    if (!isKnown(value) || !value.isObject())
        return true;
    JSObject* object = asObject(value);
    for (JSObject* holder = object; holder; holder = holder->getPrototypeDirect().getObject()) {
        if (holder->inherits<ProxyObject>())
            return false;
        JSValue raw = holder->getDirect(*m_vm, m_vm->propertyNames->raw);
        if (!raw)
            continue;
        if (raw.isGetterSetter() || raw.isCustomGetterSetter())
            return false;
        return (holder == object && !object->isCallable()) || take(raw, work, price, 1);
    }
    return true;
}

bool RehearsalOfRegExpTexts::payForHoldersPassed(JSValue value, JSObject* last)
{
    if (!isKnown(value))
        return true;
    PriceOfBuiltin price;
    for (JSObject* holder = value.getObject(); holder && holder != last; holder = holder->getPrototypeDirect().getObject())
        ++price.walked;
    if (price.walked < charactersGoneOverAtPriceOfOneOperation)
        return true;
    price.handed = price.walked / charactersGoneOverAtPriceOfOneOperation;
    return pay(price);
}

bool RehearsalOfRegExpTexts::payForWalkThatEndedInRefusal(const PriceOfBuiltin& asked)
{
    m_operationsLeft -= static_cast<uint32_t>(std::min<uint64_t>(asked.walked, m_operationsLeft));
    m_operationsLeftInAll -= std::min(asked.walked, m_operationsLeftInAll);
    return false;
}

bool RehearsalOfRegExpTexts::pay(const PriceOfBuiltin& asked)
{
    uint64_t matched = asked.handed + asked.characters;
    uint64_t worstCase = asked.mayMatch ? matched : asked.operations();
    uint64_t most = std::max<uint64_t>(m_operationsLeft, 1);
    for (unsigned term = asked.mayMatch ? asked.termsThatVary : 0; term-- && worstCase < most;)
        worstCase *= std::max<uint64_t>(matched, 1);
    if (asked.mayMatch && worstCase < most)
        worstCase *= asked.waysThroughChoices.once;
    for (uint64_t character = asked.mayMatch && asked.waysThroughChoices.forEachCharacter > 1 ? matched : 0; character-- && worstCase < most;)
        worstCase *= asked.waysThroughChoices.forEachCharacter;
    if (asked.mayMatch)
        worstCase += operationsForBuiltinHandedRegExp;
    uint64_t price = asked.operations() + (asked.mayMatch ? operationsForBuiltinHandedRegExp : 0);
    worstCase = std::max(worstCase, price);
    if (worstCase >= m_operationsLeft || worstCase >= m_operationsLeftInAll)
        return payForWalkThatEndedInRefusal(asked);
    price = std::max(price, worstCase / charactersGoneOverAtPriceOfOneOperation);
    m_operationsLeft -= static_cast<uint32_t>(price);
    m_operationsLeftInAll -= price;
    m_bytesMade += asked.laidOut;
    return true;
}

bool RehearsalOfRegExpTexts::payToLayOut(JSValue value)
{
    if (!value.isString() || !asString(value)->isRope())
        return true;
    PriceOfBuiltin price;
    price.takeText(asString(value), false);
    return pay(price);
}

void RehearsalOfRegExpTexts::noteTextMade(JSValue made)
{
    if (made && made.isString())
        m_bytesMade += asString(made)->isRope() ? sizeof(JSRopeString) : asString(made)->length();
}

void RehearsalOfRegExpTexts::noteLaidOutByBuiltin(Values& ropesHanded)
{
    for (JSValue handed : ropesHanded) {
        if (!asString(handed)->isRope())
            m_bytesMade += asString(handed)->length();
    }
}

JSValue RehearsalOfRegExpTexts::readProperty(JSValue object, PropertyName name)
{
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool isFound = object.getPropertySlot(realm, name, slot);
    if (scope.exception())
        return { };
    if (object.isObject() && !payForHoldersPassed(object, isFound ? slot.slotBase() : nullptr))
        return { };
    if (!isFound)
        return jsUndefined();
    JSValue found = slot.getValue(realm, name);
    if (scope.exception())
        return { };
    if (found.isString() && (slot.isCustom() || (slot.isAccessor() && !slot.getterSetter()->isGetterNull() && !slot.getterSetter()->getter()->inherits<JSNativeStdFunction>()))) {
        noteTextMade(found);
        payForPlacesMadeOutOfCharacters(asString(found)->length() / charactersGoneOverAtPriceOfOneOperation, asString(found)->length());
    }
    return found;
}

bool RehearsalOfRegExpTexts::wouldBeConverted(OptionSet<GrowthOfWork> work, JSValue thisValue, std::span<const JSValue> arguments) const
{
    auto isBoxed = [&](JSValue value) {
        return isKnown(value) && value.isObject() && (asObject(value)->type() == StringObjectType || asObject(value)->type() == DerivedStringObjectType || asObject(value)->type() == NumberObjectType || asObject(value)->type() == BooleanObjectType);
    };
    if (isBoxed(thisValue) || std::ranges::any_of(arguments, isBoxed))
        return true;
    if (work.contains(GrowthOfWork::MakesCellsOfCharactersHanded) && !arguments.empty() && isKnown(arguments[0]) && arguments[0].isObject())
        return true;
    bool takesFunctionThatGivesWhatIsLaid = work.containsAny({ GrowthOfWork::LaysTextOverFind, GrowthOfWork::LaysTextOverEveryFind });
    for (size_t index = 0; thisValue.isString() && index < arguments.size(); ++index) {
        JSValue argument = arguments[index];
        if (isKnown(argument) && argument.isObject() && !argument.inherits<RegExpObject>() && !(index == 1 && takesFunctionThatGivesWhatIsLaid && argument.isCallable()))
            return true;
    }
    if (work.contains(GrowthOfWork::LaysTextBetweenPlaces) && !arguments.empty() && isKnown(arguments[0]) && arguments[0].isObject())
        return true;
    return work.contains(GrowthOfWork::MakesPatternOfTextHanded) && !arguments.empty() && arguments[0].isString();
}

bool RehearsalOfRegExpTexts::wouldMakeMoreThanAnybodyKeeps(OptionSet<GrowthOfWork> work, JSValue thisValue, std::span<const JSValue> arguments, const PriceOfBuiltin& price)
{
    if (work.contains(GrowthOfWork::LaysTextBetweenPlaces) && thisValue.isObject()) {
        auto* list = dynamicDowncast<JSArray>(thisValue);
        uint64_t places = list ? list->length() : elementsOfObjectHandedToBuiltin(asObject(thisValue), true).value_or(m_longestText + 1);
        return price.characters + places * (!arguments.empty() && arguments[0].isString() ? asString(arguments[0])->length() : 1) > m_longestText;
    }
    if (!work.containsAny({ GrowthOfWork::LaysTextOverFind, GrowthOfWork::LaysTextOverEveryFind }) || !thisValue.isString() || arguments.size() < 2 || !arguments[1].isString())
        return false;
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    auto* pattern = dynamicDowncast<RegExpObject>(arguments[0]);
    uint64_t length = asString(thisValue)->length();
    uint64_t wholes = 0;
    uint64_t parts = 0;
    String laid = asString(arguments[1])->value(realm);
    String subject = asString(thisValue)->value(realm);
    String sought = arguments[0].isString() ? String(asString(arguments[0])->value(realm)) : String();
    if (hasThrown(scope))
        return true;
    for (unsigned index = 0; index + 1 < laid.length(); ++index) {
        if (laid[index] != '$')
            continue;
        ++(laid[index + 1] == '`' || laid[index + 1] == '\'' ? wholes : parts);
        ++index;
    }
    auto made = [&](uint64_t finds) {
        return length + finds * (laid.length() + wholes * length) + parts * length;
    };
    bool laysOverEveryFind = pattern ? pattern->regExp()->global() : work.contains(GrowthOfWork::LaysTextOverEveryFind);
    if (!laysOverEveryFind)
        return made(1) > m_longestText;
    if (!pattern && sought.isNull())
        return made(length + 1) > m_longestText;
    if (made(length + 1) <= m_longestText)
        return false;
    if (!pay(price))
        return true;
    uint64_t finds = 0;
    for (unsigned start = 0; start <= length;) {
        size_t begin = notFound;
        size_t end = 0;
        if (pattern) {
            MatchResult found = pattern->regExp()->match(realm, subject, start);
            if (hasThrown(scope))
                return true;
            if (found) {
                begin = found.start;
                end = found.end;
            }
        } else if ((begin = subject.find(sought, start)) != notFound)
            end = begin + sought.length();
        if (begin == notFound)
            break;
        ++finds;
        start = end > begin ? end : end + 1;
    }
    return made(finds) > m_longestText;
}

bool RehearsalOfRegExpTexts::payForBuiltin(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments, uint64_t& charactersHanded, Values& ropesHanded)
{
    OptionSet<GrowthOfWork> work = workOf(callee);
    if (wouldBeConverted(work, thisValue, arguments))
        return false;
    PriceOfBuiltin price;
    price.ropesHanded = &ropesHanded;
    auto takeByRole = [&](JSValue value, bool isGoneThroughByPiece) {
        if (!isGoneThroughByPiece || !value.isString())
            return take(value, work, price, 0);
        price.takeText(asString(value), true);
        return true;
    };
    if (!takeByRole(thisValue, work.contains(GrowthOfWork::WithLengthOfWhatIsHanded)))
        return payForWalkThatEndedInRefusal(price);
    price.handed += arguments.size();
    bool goesOverArguments = work.containsAny({ GrowthOfWork::GoesOverItsArguments, GrowthOfWork::WithPropertiesOfWhatIsHanded });
    for (JSValue argument : arguments) {
        if (!takeByRole(argument, goesOverArguments) || passesWhatIsLeft(price))
            return payForWalkThatEndedInRefusal(price);
    }
    uint64_t mayBeMadeOfCharacters = work.contains(GrowthOfWork::MakesCellsOfCharactersHanded) ? price.characters / 2 * (JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity) + sizeof(JSValue)) : 0;
    if (m_bytesThatParsesMayHaveMade + mayBeMadeOfCharacters > mostBytesMade)
        return payForWalkThatEndedInRefusal(price);
    if (work.contains(GrowthOfWork::WithAllThatIsReached) && !arguments.empty() && !takeRawOf(arguments[0], work, price))
        return payForWalkThatEndedInRefusal(price);
    if (thisValue.isString() && !arguments.empty() && arguments[0].isString() && asString(arguments[0])->length() > static_cast<unsigned>(WTF::AdaptiveStringSearcherBase::bmMaxShift))
        price.stepsOfSearch = static_cast<uint64_t>(asString(thisValue)->length()) * asString(arguments[0])->length();
    charactersHanded = price.characters;
    if (!pay(price) || (price.holdsWhatNobodyKnows && work.contains(GrowthOfWork::AsksIdentityOfWhatIsHeld)))
        return false;
    m_bytesThatParsesMayHaveMade += mayBeMadeOfCharacters;
    return !wouldMakeMoreThanAnybodyKeeps(work, thisValue, arguments, price);
}

void RehearsalOfRegExpTexts::payForPlacesMadeOutOfCharacters(uint64_t places, uint64_t characters)
{
    uint64_t price = std::min(places, characters);
    m_operationsLeft -= static_cast<uint32_t>(std::min<uint64_t>(price, m_operationsLeft));
    m_operationsLeftInAll -= std::min(price, m_operationsLeftInAll);
}

bool RehearsalOfRegExpTexts::payForNamesListedOf(JSObject* object)
{
    PriceOfBuiltin price;
    for (JSObject* holder = object; holder;) {
        if (holder->inherits<ProxyObject>())
            return false;
        if (auto* list = dynamicDowncast<JSArray>(holder))
            price.handed += list->length();
        else if (auto* text = dynamicDowncast<StringObject>(holder))
            price.handed += text->internalValue()->length();
        price.handed += holder->structure()->maxOffset() + 1;
        ++price.characters;
        if (passesWhatIsLeft(price))
            return false;
        JSValue above = holder->getPrototypeDirect();
        holder = above.isObject() ? asObject(above) : nullptr;
    }
    return pay(price);
}

bool RehearsalOfRegExpTexts::payFor(JSValue value)
{
    if (value.isNumber())
        return true;
    PriceOfBuiltin price;
    return take(value, { GrowthOfWork::WithWhatIsHanded }, price, 0) ? pay(price) : payForWalkThatEndedInRefusal(price);
}

bool RehearsalOfRegExpTexts::payForConversion(JSValue value)
{
    if (!value.isObject() && !value.isHeapBigInt())
        return true;
    PriceOfBuiltin price;
    return take(value, { GrowthOfWork::WithWhatIsHanded }, price, 0) ? pay(price) : payForWalkThatEndedInRefusal(price);
}

JSValue RehearsalOfRegExpTexts::noteMadeByBuiltin(JSValue made, uint64_t charactersHanded)
{
    if (auto* list = dynamicDowncast<JSArray>(made)) {
        m_bytesMade += static_cast<size_t>(list->length()) * sizeof(JSValue);
        payForPlacesMadeOutOfCharacters(list->length(), charactersHanded);
    } else if (auto* map = dynamicDowncast<JSMap>(made)) {
        m_bytesMade += placesInEntriesOf(map) * sizeof(JSValue);
        payForPlacesMadeOutOfCharacters(map->size(), charactersHanded);
    } else if (auto* set = dynamicDowncast<JSSet>(made)) {
        m_bytesMade += placesInEntriesOf(set) * sizeof(JSValue);
        payForPlacesMadeOutOfCharacters(set->size(), charactersHanded);
    } else if (dynamicDowncast<ErrorInstance>(made) && m_realm->stackTraceLimit().value_or(0))
        payForFramesThatEngineWalks();
    return made;
}

void RehearsalOfRegExpTexts::noteThrown(Exception* exception)
{
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    if (m_vm->isTerminationException(exception) || (error && error->isOutOfMemoryError()))
        end();
}

JSValue RehearsalOfRegExpTexts::construct(JSValue callee, JSValue newTarget, std::span<const JSValue> arguments)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    if (JSObject* original = originalOf(callee))
        callee = original;
    if (JSObject* original = originalOf(newTarget))
        newTarget = original;
    if (!isKnown(callee) || !callee.isCell() || m_functionsThatAskTheWorld.contains(callee.asCell()))
        return unknown();
    if (!isKnown(newTarget) || !newTarget.isObject())
        newTarget = callee;
    if (ClosureOfProgram closure = closureIn(callee); closure.function) {
        FunctionID function = closure.function;
        if (m_program.at(function).isCoroutine() || !m_program.at(function).hasBody())
            return unknown();
        Values captures;
        appendElementsOf(closure.captures, captures);
        switch (m_program.at(function).kind) {
        case FunctionKind::BaseConstructor:
        case FunctionKind::DerivedConstructor:
            return enter(function, captures.span(), jsUndefined(), callee, newTarget, arguments);
        case FunctionKind::Plain: {
            noteObjectMade();
            JSObject* made = constructEmptyObject(realm, asObject(prototypeFor(newTarget)));
            JSValue returned = enter(function, captures.span(), made, callee, newTarget, arguments);
            return isKnown(returned) && returned.isObject() ? returned : JSValue(made);
        }
        default:
            return unknown();
        }
    }
    if (callee.inherits<JSNativeStdFunction>())
        return unknown();
    if (callee == m_regExpConstructor && !noteTexts(arguments))
        return unknown();
    if (asksTheWorld(newTarget) || std::ranges::any_of(arguments, [&](JSValue argument) { return asksTheWorld(argument); }))
        return unknown();
    CallData constructData = getConstructData(callee);
    if (constructData.type == CallData::Type::None)
        return unknown();

    uint64_t charactersHanded = 0;
    Values ropesHanded;
    if (workOf(callee).isEmpty() || !payForBuiltin(callee, newTarget, arguments, charactersHanded, ropesHanded))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* result = JSC::construct(realm, callee, constructData, list, newTarget);
    noteLaidOutByBuiltin(ropesHanded);
    if (Exception* exception = scope.exception()) {
        noteThrown(exception);
        letGoOfWhatWasThrown(scope);
        return unknown();
    }
    if (!result)
        return unknown();
    return callee == m_regExpConstructor ? notePatternMadeHere(result) : noteMadeByBuiltin(result, charactersHanded);
}

bool RehearsalOfRegExpTexts::appendElementsOf(JSValue list, Values& elements)
{
    JSGlobalObject* realm = m_realm.get();
    auto* array = dynamicDowncast<JSArray>(list);
    if (!array || array->length() > m_longestText)
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    for (unsigned index = 0; index < array->length(); ++index) {
        JSValue element = array->getIndex(realm, index);
        if (scope.exception()) {
            letGoOfWhatWasThrown(scope);
            return false;
        }
        elements.append(element);
    }
    return true;
}

JSValue RehearsalOfRegExpTexts::execute(Activation& activation, const BodyValue& value)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto operand = [&](ValueID id) {
        return at(activation, id);
    };
    auto settle = [&](JSValue result) -> JSValue {
        if (hasThrown(scope))
            return unknown();
        if (!result || (result.isString() && asString(result)->length() > m_longestText))
            return unknown();
        return result;
    };
    auto operandsOf = [&](std::span<const ValueID> ids) {
        Values values;
        for (ValueID id : ids)
            values.append(operand(id));
        return values;
    };
    auto number = [&](JSValue of) {
        return of.toNumber(realm);
    };

    m_operation = static_cast<uint64_t>(activation.function.index()) << 32 | value.id().index();
    Opcode opcode = value.opcode();
    switch (opcode) {
    case Opcode::Undefined:
        return jsUndefined();
    case Opcode::Null:
        return jsNull();
    case Opcode::Absent:
        return m_absent.get();
    case Opcode::Boolean:
        return jsBoolean(value.as<Opcode::Boolean>().boolean());
    case Opcode::Integer:
        return jsNumber(static_cast<double>(value.as<Opcode::Integer>().integer()));
    case Opcode::Double:
        return jsNumber(value.as<Opcode::Double>().number());
    case Opcode::String:
        return textOf(value.as<Opcode::String>().string());
    case Opcode::Builtin:
        return builtinInRealm(value.as<Opcode::Builtin>().builtin());
    case Opcode::Data: {
        DataID data = value.as<Opcode::Data>().data();
        PriceOfBuiltin price;
        price.handed = m_nodesOfData.ensure(data.index(), [&] {
            return numberOfNodesIn(m_program.at(data).root);
        }).iterator->value;
        price.laidOut = price.handed * sizeof(JSValue);
        return pay(price) ? settle(valueOfData(m_program.at(data).root)) : unknown();
    }
    case Opcode::NewClosure: {
        auto closure = value.as<Opcode::NewClosure>();
        return closureOf(closure.function(), operandsOf(closure.capturesOperands()).span());
    }
    case Opcode::GetModuleVariable:
        return variable(value.as<Opcode::GetModuleVariable>().variable());
    case Opcode::PutModuleVariable: {
        auto store = value.as<Opcode::PutModuleVariable>();
        m_variables[store.variable().index()] = operand(store.valueOperand());
        return { };
    }

    case Opcode::Upcast:
    case Opcode::NarrowByField:
    case Opcode::NarrowByTest:
    case Opcode::Check:
    case Opcode::Convert:
    case Opcode::RequireObjectCoercible:
    case Opcode::RequireObject:
    case Opcode::FinishObject:
    case Opcode::FinishClass:
        return operand(value.operand(0));
    case Opcode::CheckInitialized: {
        JSValue held = operand(value.operand(0));
        return held == JSValue(m_absent.get()) ? unknown() : held;
    }
    case Opcode::LoopHint:
    case Opcode::RecordRequire:
    case Opcode::SetFunctionName:
    case Opcode::DefineSetter:
    case Opcode::IteratorClose:
        return { };

    case Opcode::NewObject: {
        auto allocation = value.as<Opcode::NewObject>();
        const Layout& layout = m_program.at(allocation.layout());
        JSValue prototype = allocation.prototypeOperand() ? operand(allocation.prototypeOperand()) : JSValue();
        noteObjectMade();
        JSObject* made = prototype && isKnown(prototype) && prototype.isObject() ? constructEmptyObject(realm, asObject(prototype)) : constructEmptyObject(realm);
        std::span<const ValueID> values = allocation.valuesOperands();
        unsigned index = 0;
        for (auto field : allocation.fields()) {
            if (index >= values.size())
                break;
            JSValue stored = operand(values[index++]);
            if (isEnvironmentKind(layout.kind)) {
                if (!storeAt(made, static_cast<unsigned>(field), stored))
                    return unknown();
            } else if (PropertyKeyID key = layout.fields[static_cast<unsigned>(field)].key) {
                if (Identifier name = nameOf(key); !name.isNull() && !storeUnder(made, name, stored))
                    return unknown();
            }
        }
        return made;
    }
    case Opcode::NewDictionary:
        noteObjectMade();
        return constructEmptyObject(realm);
    case Opcode::GetEnvironmentField: {
        auto read = value.as<Opcode::GetEnvironmentField>();
        JSValue environment = operand(read.environmentOperand());
        if (!isKnown(environment) || !environment.isObject())
            return unknown();
        JSValue held = asObject(environment)->getDirectIndex(realm, static_cast<unsigned>(read.field()));
        return settle(held);
    }
    case Opcode::PutEnvironmentField: {
        auto store = value.as<Opcode::PutEnvironmentField>();
        JSValue environment = operand(store.environmentOperand());
        if (isKnown(environment) && environment.isObject())
            asObject(environment)->putDirectIndex(realm, static_cast<unsigned>(store.field()), operand(store.valueOperand()));
        letGoOfWhatWasThrown(scope);
        return { };
    }

    case Opcode::NewArray: {
        auto allocation = value.as<Opcode::NewArray>();
        JSArray* list = newList();
        if (!list)
            return unknown();
        auto holes = allocation.holes();
        auto nextHole = holes.begin();
        unsigned index = 0;
        for (ValueID element : allocation.elementsOperands()) {
            while (nextHole != holes.end() && static_cast<unsigned>(*nextHole) == index) {
                ++index;
                ++nextHole;
            }
            if (!storeAt(list, index++, operand(element)))
                return unknown();
        }
        for (; nextHole != holes.end(); ++nextHole)
            ++index;
        return setLengthOf(list, index) ? JSValue(list) : unknown();
    }
    case Opcode::AppendElement: {
        auto append = value.as<Opcode::AppendElement>();
        notePlaceFilled();
        if (auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand()))) {
            StoreThatMayMakeRoom store(*this, list);
            list->push(realm, operand(append.valueOperand()));
        }
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::AppendHole: {
        if (auto* list = dynamicDowncast<JSArray>(operand(value.operand(0)))) {
            StoreThatMayMakeRoom store(*this, list);
            list->setLength(realm, list->length() + 1);
        }
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::AppendSpread: {
        auto append = value.as<Opcode::AppendSpread>();
        auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand()));
        JSValue iterable = operand(append.iterableOperand());
        if (!list)
            return { };
        PriceOfBuiltin pieces;
        bool isPaid = isKnown(iterable);
        if (isPaid && iterable.isString()) {
            pieces.handed = asString(iterable)->length();
            isPaid = pay(pieces);
        } else if (isPaid && !iterable.isNumber())
            isPaid = take(iterable, { GrowthOfWork::WithWhatIsHanded }, pieces, 0) ? pay(pieces) : payForWalkThatEndedInRefusal(pieces);
        if (!isPaid) {
            list->push(realm, unknown());
            letGoOfWhatWasThrown(scope);
            return { };
        }
        StoreThatMayMakeRoom store(*this, list);
        uint64_t elementsPaidFor = pieces.handed;
        forEachInIterable(realm, iterable, [&](VM& ofRealm, JSGlobalObject* realm, JSValue element) {
            if (elementsPaidFor)
                --elementsPaidFor;
            else if (!m_operationsLeft || !m_operationsLeftInAll || !hasRoomLeft()) {
                auto scopeOfStep = DECLARE_THROW_SCOPE(ofRealm);
                throwVMTypeError(realm, scopeOfStep, "nobody knows the value"_s);
                return;
            } else {
                --m_operationsLeft;
                --m_operationsLeftInAll;
                m_bytesMade += sizeof(JSValue);
            }
            list->push(realm, element);
        });
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::NewRegExp: {
        auto literal = value.as<Opcode::NewRegExp>();
        RegExp* regExp = RegExp::create(vm, charactersOf(literal.pattern()), toYarrFlags(literal.regExpFlags()));
        if (!regExp->isValid())
            return unknown();
        noteObjectMade();
        return RegExpObject::create(vm, realm->regExpStructure(), regExp);
    }
    case Opcode::NewTemplateObject: {
        JSArray* cooked = newList();
        JSArray* raw = cooked ? newList() : nullptr;
        if (!raw)
            return unknown();
        for (TemplateString part : value.as<Opcode::NewTemplateObject>().templateStrings()) {
            if (!append(cooked, part.cooked ? textOf(part.cooked) : jsUndefined()) || !append(raw, textOf(part.raw)))
                return unknown();
        }
        cooked->putDirect(vm, vm.propertyNames->raw, raw);
        return cooked;
    }

    case Opcode::PropGet: {
        auto read = value.as<Opcode::PropGet>();
        JSValue object = operand(read.objectOperand());
        Identifier name = nameOf(read.key());
        if (!isKnown(object) || object.isUndefinedOrNull() || name.isNull())
            return unknown();
        if (name == vm.propertyNames->prototype)
            givePrototypeTo(object);
        JSValue found = settle(readProperty(object, name));
        if (found.isUndefined() && isGlobalObject(object))
            return unknown();
        return found;
    }
    case Opcode::ElemGet: {
        auto read = value.as<Opcode::ElemGet>();
        JSValue object = operand(read.objectOperand());
        JSValue property = operand(read.propertyOperand());
        if (isKnown(object) && object.isObject() && isNumberThatNobodyKnows(property))
            return findUnderNumberThatNobodyKnows(asObject(object), property);
        if (!isKnown(object) || !mayBeMadeKey(property) || object.isUndefinedOrNull() || !payToLayOut(object))
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
        return settle(readProperty(object, name));
    }
    case Opcode::PropSet:
    case Opcode::PropDefine: {
        JSValue object = operand(value.operand(0));
        PropertyKeyID key = opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().key() : value.as<Opcode::PropDefine>().key();
        JSValue stored = operand(opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().valueOperand() : value.as<Opcode::PropDefine>().valueOperand());
        Identifier name = nameOf(key);
        if (opcode == Opcode::PropSet && !payForHoldersPassed(object))
            return { };
        if (!isKnown(object) || !object.isObject() || isGlobalObject(object) || name.isNull())
            return { };
        StoreThatMayMakeRoom store(*this, asObject(object));
        if (opcode == Opcode::PropDefine)
            asObject(object)->putDirectMayBeIndex(realm, name, stored);
        else {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::ElemSet:
    case Opcode::ElemDefine: {
        JSValue object = operand(value.operand(0));
        JSValue property = operand(value.operand(1));
        JSValue stored = operand(opcode == Opcode::ElemSet ? value.as<Opcode::ElemSet>().valueOperand() : value.as<Opcode::ElemDefine>().valueOperand());
        if (opcode == Opcode::ElemSet && !payForHoldersPassed(object))
            return { };
        if (isKnown(object) && object.isObject() && !isGlobalObject(object) && isNumberThatNobodyKnows(property)) {
            layUnderNumberThatNobodyKnows(asObject(object), property, stored);
            return { };
        }
        if (!isKnown(object) || !mayBeMadeKey(property) || !object.isObject() || isGlobalObject(object))
            return { };
        StoreThatMayMakeRoom store(*this, asObject(object));
        if (property.isUInt32())
            object.putByIndex(realm, property.asUInt32(), stored, false);
        else if (Identifier name = property.toPropertyKey(realm); !scope.exception()) {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::DefineGetter: {
        auto definition = value.as<Opcode::DefineGetter>();
        JSValue object = operand(definition.objectOperand());
        Identifier name = definition.key() ? nameOf(definition.key()) : Identifier();
        if (isKnown(object) && object.isObject() && !isGlobalObject(object) && !name.isNull())
            asObject(object)->putDirectMayBeIndex(realm, name, unknown());
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::ElemHas: {
        auto test = value.as<Opcode::ElemHas>();
        JSValue object = operand(test.objectOperand());
        JSValue property = operand(test.propertyOperand());
        if (!isKnown(object) || !mayBeMadeKey(property) || !object.isObject() || isGlobalObject(object))
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
        if (!payForHoldersPassed(object))
            return unknown();
        return settle(jsBoolean(asObject(object)->hasProperty(realm, name)));
    }
    case Opcode::PropDelete:
    case Opcode::ElemDelete: {
        JSValue object = operand(value.operand(0));
        if (!isKnown(object) || !object.isObject() || isGlobalObject(object))
            return unknown();
        Identifier name;
        if (opcode == Opcode::PropDelete)
            name = nameOf(value.as<Opcode::PropDelete>().key());
        else if (JSValue property = operand(value.as<Opcode::ElemDelete>().propertyOperand()); mayBeMadeKey(property))
            name = property.toPropertyKey(realm);
        if (scope.exception() || name.isNull())
            return settle({ });
        return settle(jsBoolean(JSCell::deleteProperty(asObject(object), realm, name)));
    }
    case Opcode::SetLiteralPrototype: {
        auto store = value.as<Opcode::SetLiteralPrototype>();
        JSValue object = operand(store.objectOperand());
        JSValue prototype = operand(store.prototypeOperand());
        if (isKnown(object) && object.isObject() && isKnown(prototype) && (prototype.isObject() || prototype.isNull()) && payForHoldersPassed(prototype))
            asObject(object)->setPrototype(vm, realm, prototype);
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::PropHas: {
        auto test = value.as<Opcode::PropHas>();
        JSValue object = operand(test.objectOperand());
        Identifier name = nameOf(test.key());
        if (!isKnown(object) || !object.isObject() || isGlobalObject(object) || name.isNull())
            return unknown();
        if (!payForHoldersPassed(object))
            return unknown();
        return settle(jsBoolean(asObject(object)->hasProperty(realm, name)));
    }
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject: {
        bool copiesInto = opcode == Opcode::CopyDataProperties;
        if (!copiesInto)
            noteObjectMade();
        JSValue target = copiesInto ? operand(value.as<Opcode::CopyDataProperties>().targetOperand()) : JSValue(constructEmptyObject(realm));
        JSValue source = operand(copiesInto ? value.as<Opcode::CopyDataProperties>().sourceOperand() : value.as<Opcode::CopyObject>().sourceOperand());
        bool leavesSomeOut = copiesInto ? !value.as<Opcode::CopyDataProperties>().excludedKeys().isEmpty() || !value.as<Opcode::CopyDataProperties>().excludedOperands().empty()
            : !value.as<Opcode::CopyObject>().excludedKeys().isEmpty() || !value.as<Opcode::CopyObject>().excludedOperands().empty();
        if (!isKnown(target) || !isKnown(source) || leavesSomeOut)
            return copiesInto ? JSValue() : unknown();
        JSValue arguments[] { target, source };
        call(m_objectAssign, jsUndefined(), arguments);
        return copiesInto ? JSValue() : target;
    }

    case Opcode::Add: {
        JSValue left = operand(value.operand(0));
        JSValue right = operand(value.operand(1));
        if (!isKnown(left) || !isKnown(right))
            return isNumberKnownOrNot(left) && isNumberKnownOrNot(right) ? makeNumberThatNobodyKnows() : unknown();
        if (!payForConversion(left) || !payForConversion(right))
            return unknown();
        JSValue sum = jsAdd(realm, left, right);
        noteTextMade(sum);
        return settle(sum);
    }
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::Eq:
    case Opcode::StrictEq: {
        JSValue left = operand(value.operand(0));
        JSValue right = operand(value.operand(1));
        bool isArithmetic = opcode != Opcode::Less && opcode != Opcode::LessEq && opcode != Opcode::Greater && opcode != Opcode::GreaterEq && opcode != Opcode::Eq && opcode != Opcode::StrictEq;
        if (!isKnown(left) || !isKnown(right))
            return isArithmetic ? makeNumberThatNobodyKnows() : unknown();
        if (isArithmetic && (left.isBigInt() || right.isBigInt() || left.isObject() || right.isObject()))
            return unknown();
        if (!isArithmetic && (opcode != Opcode::StrictEq || (left.isString() && right.isString())) && (!payFor(left) || !payFor(right)))
            return unknown();
        if (isArithmetic && (!payFor(left) || !payFor(right)))
            return makeNumberThatNobodyKnows();
        double first = 0;
        double second = 0;
        if (isArithmetic) {
            first = number(left);
            if (hasThrown(scope))
                return unknown();
            second = number(right);
            if (hasThrown(scope))
                return unknown();
        }
        switch (opcode) {
        case Opcode::Mod:
            return jsNumber(fmod(first, second));
        case Opcode::Pow:
            return jsNumber(operationMathPow(first, second));
        case Opcode::BitAnd:
            return jsNumber(toInt32(first) & toInt32(second));
        case Opcode::BitOr:
            return jsNumber(toInt32(first) | toInt32(second));
        case Opcode::BitXor:
            return jsNumber(toInt32(first) ^ toInt32(second));
        case Opcode::LShift:
            return jsNumber(static_cast<int32_t>(toUInt32(first) << (toUInt32(second) & 31)));
        case Opcode::RShift:
            return jsNumber(toInt32(first) >> (toUInt32(second) & 31));
        case Opcode::URShift:
            return jsNumber(toUInt32(first) >> (toUInt32(second) & 31));
        case Opcode::Sub:
            return jsNumber(first - second);
        case Opcode::Mul:
            return jsNumber(first * second);
        case Opcode::Div:
            return jsNumber(first / second);
        case Opcode::Less:
            return settle(jsBoolean(jsLess<true>(realm, left, right)));
        case Opcode::LessEq:
            return settle(jsBoolean(jsLessEq<true>(realm, left, right)));
        case Opcode::Greater:
            return settle(jsBoolean(jsLess<false>(realm, right, left)));
        case Opcode::GreaterEq:
            return settle(jsBoolean(jsLessEq<false>(realm, right, left)));
        case Opcode::Eq:
            return settle(jsBoolean(JSValue::equal(realm, left, right)));
        default:
            return settle(jsBoolean(JSValue::strictEqual(realm, left, right)));
        }
    }
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::Negate: {
        JSValue of = operand(value.operand(0));
        if (isNumberThatNobodyKnows(of) && (opcode == Opcode::ToNumber || opcode == Opcode::ToNumeric))
            return of;
        if (!isKnown(of))
            return makeNumberThatNobodyKnows();
        if (of.isBigInt())
            return unknown();
        if (!payFor(of))
            return unknown();
        double converted = number(of);
        return settle(jsNumber(opcode == Opcode::Increment ? converted + 1 : opcode == Opcode::Decrement ? converted - 1 : opcode == Opcode::Negate ? -converted : converted));
    }
    case Opcode::BitNot: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isBigInt() && !of.isObject() && payToLayOut(of) ? settle(jsNumber(~of.toInt32(realm))) : unknown();
    }
    case Opcode::ToObject: {
        JSValue of = operand(value.operand(0));
        if (!of.isObject())
            noteObjectMade();
        return isKnown(of) && !of.isUndefinedOrNull() ? settle(of.toObject(realm)) : unknown();
    }
    case Opcode::Await: {
        JSValue awaited = operand(value.operand(0));
        if (!isKnown(awaited))
            return unknown();
        if (auto* promise = dynamicDowncast<JSPromise>(awaited))
            return promise->status() == JSPromise::Status::Fulfilled ? promise->result() : unknown();
        return awaited.isObject() ? unknown() : awaited;
    }
    case Opcode::ToThis: {
        JSValue of = operand(value.operand(0));
        if (!isKnown(of))
            return unknown();
        return of.isUndefinedOrNull() ? JSValue(realm) : settle(of.toObject(realm));
    }
    case Opcode::ToLength: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isObject() && payToLayOut(of) ? settle(jsNumber(of.toLength(realm))) : unknown();
    }
    case Opcode::ToBoolean: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? jsBoolean(of.toBoolean(realm)) : unknown();
    }
    case Opcode::LogicalNot: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? jsBoolean(!of.toBoolean(realm)) : unknown();
    }
    case Opcode::TypeOf: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? JSValue(jsTypeStringForValue(realm, of)) : unknown();
    }
    case Opcode::TypeTest: {
        auto test = value.as<Opcode::TypeTest>();
        JSValue of = operand(test.valueOperand());
        if (!isKnown(of))
            return unknown();
        std::optional<bool> answer = isOfType(of, test.testedType());
        return answer ? jsBoolean(*answer) : unknown();
    }
    case Opcode::InstanceOf: {
        JSValue of = operand(value.operand(0));
        JSValue constructor = operand(value.operand(1));
        if (!isKnown(of) || !isKnown(constructor) || !constructor.isObject() || !payForHoldersPassed(of))
            return unknown();
        givePrototypeTo(constructor);
        return settle(jsBoolean(asObject(constructor)->hasInstance(realm, of)));
    }
    case Opcode::ToString: {
        JSValue of = operand(value.operand(0));
        if (!mayBeConverted(of))
            return unknown();
        JSValue converted = of.toString(realm);
        if (!of.isString())
            noteTextMade(converted);
        return settle(converted);
    }
    case Opcode::ToPropertyKey: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isObject() ? of : unknown();
    }
    case Opcode::Strcat: {
        StringBuilder builder;
        for (ValueID part : value.as<Opcode::Strcat>().partsOperands()) {
            JSValue of = operand(part);
            if (!isKnown(of) || !payFor(of))
                return unknown();
            builder.append(of.toWTFString(realm));
            if (scope.exception() || builder.length() > m_longestText)
                return settle({ });
        }
        JSValue joined = jsString(vm, builder.toString());
        noteTextMade(joined);
        return settle(joined);
    }

    case Opcode::Call: {
        auto written = value.as<Opcode::Call>();
        return settle(call(operand(written.calleeOperand()), operand(written.thisValueOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::Construct: {
        auto written = value.as<Opcode::Construct>();
        return settle(construct(operand(written.calleeOperand()), operand(written.newTargetOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::CallList: {
        auto written = value.as<Opcode::CallList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(call(operand(written.calleeOperand()), operand(written.thisValueOperand()), arguments.span()));
    }
    case Opcode::ConstructList: {
        auto written = value.as<Opcode::ConstructList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(construct(operand(written.calleeOperand()), operand(written.newTargetOperand()), arguments.span()));
    }

    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments: {
        bool isMapped = opcode == Opcode::CreateMappedArguments;
        JSValue count = operand(isMapped ? value.as<Opcode::CreateMappedArguments>().countOperand() : value.as<Opcode::CreateUnmappedArguments>().countOperand());
        JSValue rest = operand(isMapped ? value.as<Opcode::CreateMappedArguments>().restOperand() : value.as<Opcode::CreateUnmappedArguments>().restOperand());
        std::span<const ValueID> parameters = isMapped ? value.as<Opcode::CreateMappedArguments>().parametersOperands() : value.as<Opcode::CreateUnmappedArguments>().parametersOperands();
        if (!isKnown(count) || !count.isNumber())
            return unknown();
        Values all;
        for (size_t index = 0; index < parameters.size() && index < count.asNumber(); ++index)
            all.append(operand(parameters[index]));
        if (!appendElementsOf(rest, all))
            return unknown();
        return settle(listOf(all.span()));
    }
    case Opcode::GetMappedArgument: {
        auto read = value.as<Opcode::GetMappedArgument>();
        auto* list = dynamicDowncast<JSArray>(operand(read.argumentsOperand()));
        return list ? settle(list->getIndex(realm, read.index())) : unknown();
    }
    case Opcode::SetMappedArgument: {
        auto store = value.as<Opcode::SetMappedArgument>();
        if (auto* list = dynamicDowncast<JSArray>(operand(store.argumentsOperand()))) {
            StoreThatMayMakeRoom room(*this, list);
            list->putDirectIndex(realm, store.index(), operand(store.valueOperand()));
        }
        letGoOfWhatWasThrown(scope);
        return { };
    }

    case Opcode::ResolveHeritage: {
        JSValue heritage = operand(value.operand(0));
        if (heritage.isNull())
            return jsNull();
        if (!isKnown(heritage) || !heritage.isObject())
            return unknown();
        return settle(heritage.get(realm, vm.propertyNames->prototype));
    }
    case Opcode::NewClass: {
        auto written = value.as<Opcode::NewClass>();
        JSValue constructor = operand(written.constructorOperand());
        if (!isKnown(constructor) || !constructor.isObject())
            return unknown();
        noteObjectMade();
        JSValue parent = written.prototypeParentOperand() ? operand(written.prototypeParentOperand()) : JSValue(realm->objectPrototype());
        JSObject* prototype = isKnown(parent) && parent.isObject() ? constructEmptyObject(realm, asObject(parent)) : parent.isNull() ? constructEmptyObject(vm, realm->nullPrototypeObjectStructure()) : constructEmptyObject(realm);
        prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(constructor)->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
        if (JSValue heritage = operand(written.heritageOperand()); written.heritageOperand() && isKnown(heritage) && heritage.isObject() && payForHoldersPassed(heritage))
            asObject(constructor)->setPrototype(vm, realm, heritage);
        return settle(constructor);
    }
    case Opcode::GetClassPrototype: {
        JSValue classObject = operand(value.operand(0));
        return isKnown(classObject) && classObject.isObject() ? settle(classObject.get(realm, vm.propertyNames->prototype)) : unknown();
    }
    case Opcode::DefineMethod: {
        auto definition = value.as<Opcode::DefineMethod>();
        JSValue target = operand(definition.classObjectOperand());
        if (!isKnown(target) || !target.isObject())
            return { };
        if (definition.side() == ClassSide::Prototype)
            target = settle(target.get(realm, vm.propertyNames->prototype));
        Identifier name;
        if (definition.key())
            name = nameOf(definition.key());
        else if (JSValue computed = operand(definition.computedKeyOperand()); mayBeMadeKey(computed))
            name = computed.toPropertyKey(realm);
        if (!scope.exception() && isKnown(target) && target.isObject() && !name.isNull() && definition.methodKind() != MethodKind::Setter)
            asObject(target)->putDirectMayBeIndex(realm, name, definition.methodKind() == MethodKind::Method ? operand(definition.functionOperand()) : unknown());
        letGoOfWhatWasThrown(scope);
        return { };
    }
    case Opcode::NewClassToken:
        noteObjectMade();
        return constructEmptyObject(realm);
    case Opcode::GetConstructPrototype:
        return prototypeFor(operand(value.operand(0)));
    case Opcode::GetSuperBase: {
        auto read = value.as<Opcode::GetSuperBase>();
        JSValue home = operand(read.homeOperand());
        if (isKnown(home) && home.isObject() && read.homeKind() == HomeObjectKind::ClassPrototype)
            home = home.get(realm, vm.propertyNames->prototype);
        if (scope.exception() || !isKnown(home) || !home.isObject())
            return settle({ });
        return settle(asObject(home)->getPrototype(realm));
    }
    case Opcode::SuperGet: {
        auto read = value.as<Opcode::SuperGet>();
        JSValue base = operand(read.baseOperand());
        JSValue receiver = operand(read.receiverOperand());
        Identifier name;
        if (read.key())
            name = nameOf(read.key());
        else if (JSValue computed = operand(read.computedKeyOperand()); mayBeMadeKey(computed))
            name = computed.toPropertyKey(realm);
        if (scope.exception() || !isKnown(base) || !base.isObject() || name.isNull() || !payForHoldersPassed(base))
            return settle({ });
        PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
        bool isFound = asObject(base)->getPropertySlot(realm, name, slot);
        if (hasThrown(scope))
            return unknown();
        return isFound ? settle(slot.getValue(realm, name)) : jsUndefined();
    }
    case Opcode::SuperConstruct: {
        auto written = value.as<Opcode::SuperConstruct>();
        return settle(construct(operand(written.parentOperand()), operand(written.newTargetOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::SuperConstructList: {
        auto written = value.as<Opcode::SuperConstructList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(construct(operand(written.parentOperand()), operand(written.newTargetOperand()), arguments.span()));
    }
    case Opcode::PrivateGet: {
        auto read = value.as<Opcode::PrivateGet>();
        JSValue object = operand(read.objectOperand());
        if (!isKnown(object) || !object.isObject())
            return unknown();
        return settle(asObject(object)->getDirect(vm, privateNameOf(read.layout(), static_cast<uint32_t>(read.field()))));
    }
    case Opcode::PrivateHas: {
        auto test = value.as<Opcode::PrivateHas>();
        JSValue object = operand(test.objectOperand());
        if (!isKnown(object) || !object.isObject())
            return unknown();
        return jsBoolean(!!asObject(object)->getDirect(vm, privateNameOf(test.layout(), static_cast<uint32_t>(test.field()))));
    }
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine: {
        bool defines = opcode == Opcode::PrivateDefine;
        JSValue object = operand(value.operand(0));
        if (isKnown(object) && object.isObject() && !isGlobalObject(object)) {
            Identifier name = defines ? privateNameOf(value.as<Opcode::PrivateDefine>().layout(), static_cast<uint32_t>(value.as<Opcode::PrivateDefine>().field())) : privateNameOf(value.as<Opcode::PrivateSet>().layout(), static_cast<uint32_t>(value.as<Opcode::PrivateSet>().field()));
            asObject(object)->putDirect(vm, name, operand(defines ? value.as<Opcode::PrivateDefine>().valueOperand() : value.as<Opcode::PrivateSet>().valueOperand()));
        }
        return { };
    }

    case Opcode::ForInOpen: {
        JSValue object = operand(value.operand(0));
        if (!isKnown(object))
            return unknown();
        JSArray* state = newList();
        if (!state || !append(state, jsNumber(1)))
            return unknown();
        if (object.isObject() && !isGlobalObject(object)) {
            if (!payForNamesListedOf(asObject(object)))
                return unknown();
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            asObject(object)->getPropertyNames(realm, names, DontEnumPropertiesMode::Exclude);
            if (scope.exception())
                return settle({ });
            for (const Identifier& name : names) {
                if (!append(state, jsString(vm, name.string())))
                    return unknown();
            }
        }
        return state;
    }
    case Opcode::ForInStep: {
        auto* state = dynamicDowncast<JSArray>(operand(value.operand(0)));
        if (!state)
            return unknown();
        JSValue place = elementAt(state, 0);
        if (!place.isUInt32())
            return unknown();
        unsigned next = place.asUInt32();
        bool isDone = next >= state->length();
        JSValue parts[] { jsBoolean(isDone), isDone ? jsUndefined() : elementAt(state, next) };
        return storeAt(state, 0, jsNumber(next + 1)) ? listOf(parts) : unknown();
    }
    case Opcode::CallDirect: {
        auto written = value.as<Opcode::CallDirect>();
        return settle(enterWithEntryParameters(written.function(), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::Results:
        return settle(listOf(operandsOf(value.as<Opcode::Results>().valuesOperands()).span()));
    case Opcode::ResultAt: {
        auto read = value.as<Opcode::ResultAt>();
        auto* results = dynamicDowncast<JSArray>(operand(read.resultsOperand()));
        return results ? settle(results->getIndex(realm, read.index())) : unknown();
    }

    case Opcode::IteratorOpen: {
        auto open = value.as<Opcode::IteratorOpen>();
        JSValue iterable = operand(open.iterableOperand());
        if (!isKnown(iterable) || open.iteratorKind() != IteratorKind::Sync)
            return unknown();
        IterationRecord record = iteratorForIterable(realm, iterable);
        if (scope.exception())
            return settle({ });
        JSValue parts[] { record.iterator, record.nextMethod };
        return settle(listOf(parts));
    }
    case Opcode::IteratorResume: {
        auto resume = value.as<Opcode::IteratorResume>();
        auto* state = dynamicDowncast<JSArray>(operand(resume.iteratorOperand()));
        if (!state || resume.resumeMode() != ResumeMode::Next)
            return unknown();
        JSValue iterator = elementAt(state, 0);
        IterationRecord record { iterator, elementAt(state, 1) };
        JSValue step = iteratorStep(realm, record);
        if (scope.exception())
            return settle({ });
        bool isDone = step.isFalse();
        JSValue parts[] { jsBoolean(isDone), isDone ? jsUndefined() : iteratorValue(realm, step) };
        if (scope.exception())
            return settle({ });
        return settle(listOf(parts));
    }

    case Opcode::EvaluateModule:
        runModule(value.as<Opcode::EvaluateModule>().module());
        return { };
    case Opcode::ModuleBuiltin: {
        auto read = value.as<Opcode::ModuleBuiltin>();
        return read.builtin() == Builtins::ModuleObject ? moduleObjectOf(read.module()) : unknown();
    }
    case Opcode::ModuleNamespace:
        return settle(namespaceOf(value.as<Opcode::ModuleNamespace>().module()));
    case Opcode::RequireBuiltinModule: {
        RuntimeModuleID wanted = value.as<Opcode::RequireBuiltinModule>().runtimeModule();
        ModuleID module = wanted && wanted.index() < m_modulesOfRuntimeModules.size() ? m_modulesOfRuntimeModules[wanted.index()] : ModuleID();
        if (!module)
            return unknown();
        runModule(module);
        JSValue result = m_resultsOfModules[module.index()];
        return result ? result : unknown();
    }
    case Opcode::RequireModule: {
        ModuleID module = value.as<Opcode::RequireModule>().module();
        if (!module || !requireAnswersWithResultOfBody(m_program, value.as<Opcode::RequireModule>().referrer(), module))
            return unknown();
        runModule(module);
        JSValue result = m_resultsOfModules[module.index()];
        return result ? result : unknown();
    }

    default:
        return unknown();
    }
}

bool RehearsalOfRegExpTexts::takeEdge(Activation& activation, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    BodyBlock block = activation.index.block(target.block());
    std::span<const ValueID> arguments = target.arguments();
    Values passed;
    for (ValueID argument : arguments)
        passed.append(at(activation, argument));
    auto isSame = [&](JSValue held, JSValue brought) {
        return held == brought || (held.isString() && brought.isString() && areStrictlyEqual(held, brought));
    };
    bool arrivesAtJoin = activation.goesBothWays() && activation.regions.last().join == block.id();
    bool bringsOtherValues = false;
    for (uint32_t index = 0; index < passed.size(); ++index) {
        JSValue& held = activation.values[block.parameter(index).index()];
        bringsOtherValues |= !held || !isSame(held, passed[index]);
        if (!arrivesAtJoin)
            held = passed[index];
    }
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
        if (std::optional<BodyEdgeResult> result = activation.index.edgeResult(terminator, targetIndex, target, proofIndex); result && result->subject)
            activation.values[result->parameter.index()] = at(activation, result->subject);
    }
    if (arrivesAtJoin) {
        OpenRegion& region = activation.regions.last();
        region.hasBroughtOtherValues |= bringsOtherValues;
        bool isNew = true;
        payForWhatIsGoneOver(static_cast<uint64_t>(region.numberOfArrivals) * passed.size());
        for (uint32_t arrival = 0; arrival < region.numberOfArrivals && isNew; ++arrival) {
            isNew = false;
            for (uint32_t index = 0; index < passed.size() && !isNew; ++index)
                isNew = !isSame(activation.arrivals[region.firstArrival + arrival * passed.size() + index], passed[index]);
        }
        if (isNew) {
            ++region.numberOfArrivals;
            for (JSValue brought : passed)
                activation.arrivals.append(brought);
        }
        return false;
    }
    if (!activation.goesBothWays())
        return true;
    uint8_t& times = activation.timesEnteredWhileGoingBothWays[block.id().index()];
    if (times && (!bringsOtherValues || times >= timesThroughBlockWhileGoingBothWays))
        return false;
    if (!times++)
        activation.blocksCounted.append(block.id());
    return true;
}

BlockID RehearsalOfRegExpTexts::joinBehind(Activation& activation, BlockID block)
{
    if (activation.joins)
        return (*activation.joins)[block.index()];
    const Vector<BlockID>& joins = m_joins.ensure(activation.function.index(), [&] {
        const BodyIndex& index = activation.index;
        uint32_t numberOfBlocks = index.numberOfBlocks();
        uint32_t end = numberOfBlocks;
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<Vector<uint32_t, 2>> successors(numberOfBlocks + 1);
        Vector<Vector<uint32_t, 2>> predecessors(numberOfBlocks + 1);
        for (uint32_t from = 0; from < numberOfBlocks; ++from) {
            for (BodyTarget target : index.block(BlockID(from)).terminator().targets())
                successors[from].append(target.block().index());
            if (successors[from].isEmpty())
                successors[from].append(end);
            for (uint32_t to : successors[from])
                predecessors[to].append(from);
        }
        Vector<uint32_t> order;
        Vector<uint32_t> number;
        number.fill(none, numberOfBlocks + 1);
        Vector<std::pair<uint32_t, uint32_t>> stack { { end, 0 } };
        BitVector seen(numberOfBlocks + 1);
        seen.set(end);
        while (!stack.isEmpty()) {
            auto& [node, next] = stack.last();
            if (next < predecessors[node].size()) {
                uint32_t before = predecessors[node][next++];
                if (!seen.set(before))
                    stack.append({ before, 0 });
                continue;
            }
            order.append(node);
            stack.removeLast();
        }
        order.reverse();
        for (uint32_t position = 0; position < order.size(); ++position)
            number[order[position]] = position;
        Vector<uint32_t> nearest;
        nearest.fill(none, numberOfBlocks + 1);
        nearest[end] = end;
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            for (uint32_t node : order) {
                if (node == end)
                    continue;
                uint32_t found = none;
                for (uint32_t behind : successors[node]) {
                    if (nearest[behind] == none)
                        continue;
                    uint32_t other = behind;
                    if (found == none) {
                        found = other;
                        continue;
                    }
                    while (found != other) {
                        while (number[found] > number[other])
                            found = nearest[found];
                        while (number[other] > number[found])
                            other = nearest[other];
                    }
                }
                if (found != nearest[node]) {
                    nearest[node] = found;
                    hasChanged = true;
                }
            }
        }
        Vector<BlockID> joins(numberOfBlocks);
        for (uint32_t from = 0; from < numberOfBlocks; ++from) {
            if (nearest[from] != none && nearest[from] != end)
                joins[from] = BlockID(nearest[from]);
        }
        return joins;
    }).iterator->value;
    return joins[block.index()];
}

BlockID RehearsalOfRegExpTexts::goEachWayFrom(Activation& activation, BodyBlock block, JSValue& result)
{
    if (m_openRegions >= mostOpenRegions)
        return { };
    if (activation.timesEnteredWhileGoingBothWays.isEmpty())
        activation.timesEnteredWhileGoingBothWays.fill(0, activation.index.numberOfBlocks());
    if (uint8_t& times = activation.timesEnteredWhileGoingBothWays[block.id().index()]; !times) {
        times = 1;
        activation.blocksCounted.append(block.id());
    }
    BlockID join = joinBehind(activation, block.id());
    bool sharesJoin = join && activation.goesBothWays() && activation.regions.last().join == join;
    activation.regions.append(OpenRegion { join, sharesJoin ? activation.regions.last().firstArrival : activation.arrivals.size(), sharesJoin ? activation.regions.last().numberOfArrivals : 0, false });
    ++m_openRegions;
    BodyTerminator terminator = block.terminator();
    payForPlacesMadeOutOfCharacters(terminator.numberOfTargets(), terminator.numberOfTargets());
    Vector<BodyTarget, 2> ways;
    for (BodyTarget way : terminator.targets())
        ways.append(way);
    for (uint32_t index = ways.size(); index--;) {
        BodyTarget target = ways[index];
        if (takeEdge(activation, terminator, index, target))
            runFrom(activation, target.block(), result);
    }
    OpenRegion closed = activation.regions.takeLast();
    --m_openRegions;
    if (!activation.goesBothWays()) {
        for (BlockID counted : activation.blocksCounted)
            activation.timesEnteredWhileGoingBothWays[counted.index()] = 0;
        activation.blocksCounted.shrink(0);
    }
    if (sharesJoin) {
        activation.regions.last().hasBroughtOtherValues |= closed.hasBroughtOtherValues;
        activation.regions.last().numberOfArrivals = closed.numberOfArrivals;
        return { };
    }
    Values arrivals;
    for (size_t arrival = closed.firstArrival; arrival < activation.arrivals.size(); ++arrival)
        arrivals.append(activation.arrivals[arrival]);
    while (activation.arrivals.size() > closed.firstArrival)
        activation.arrivals.removeLast();
    if (!closed.numberOfArrivals)
        return { };
    if (activation.goesBothWays()) {
        uint8_t& times = activation.timesEnteredWhileGoingBothWays[join.index()];
        if (times && (!closed.hasBroughtOtherValues || times >= timesThroughBlockWhileGoingBothWays))
            return { };
        if (!times++)
            activation.blocksCounted.append(join);
    }

    BodyBlock joined = activation.index.block(join);
    uint32_t numberBrought = static_cast<uint32_t>(arrivals.size() / closed.numberOfArrivals);
    Values agreed;
    bool differ = false;
    for (uint32_t index = 0; index < numberBrought; ++index) {
        JSValue known;
        bool differsHere = false;
        for (uint32_t arrival = 0; arrival < closed.numberOfArrivals; ++arrival) {
            JSValue brought = arrivals[arrival * numberBrought + index];
            if (!isKnown(brought))
                continue;
            if (!known)
                known = brought;
            else if (known != brought && !(known.isString() && brought.isString() && areStrictlyEqual(known, brought)))
                differsHere = true;
        }
        agreed.append(known && !differsHere ? known : unknown());
        differ |= differsHere;
    }
    auto bring = [&](uint32_t arrival) {
        for (uint32_t index = 0; index < numberBrought; ++index) {
            JSValue brought = arrivals[arrival * numberBrought + index];
            activation.values[joined.parameter(index).index()] = isKnown(brought) ? brought : agreed[index];
        }
    };
    if (differ && !activation.index.loopOf(join) && activation.numberOfWaysGoneSeparately + closed.numberOfArrivals <= timesThroughBlockWhileGoingBothWays) {
        activation.numberOfWaysGoneSeparately += closed.numberOfArrivals - 1;
        bring(0);
        for (uint32_t arrival = 1; arrival < closed.numberOfArrivals; ++arrival) {
            runOneOfWays(activation, join, result, closed.numberOfArrivals - arrival + 1);
            bring(arrival);
        }
        return join;
    }
    for (uint32_t index = 0; index < numberBrought; ++index)
        activation.values[joined.parameter(index).index()] = agreed[index];
    return join;
}

void RehearsalOfRegExpTexts::runOneOfWays(Activation& activation, BlockID block, JSValue& result, uint32_t numberOfWaysLeft)
{
    uint32_t kept = m_operationsLeft - m_operationsLeft / numberOfWaysLeft;
    m_operationsLeft -= kept;
    runFrom(activation, block, result);
    m_operationsLeft = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(m_operationsLeft) + kept, std::numeric_limits<uint32_t>::max()));
}

void RehearsalOfRegExpTexts::runFrom(Activation& activation, BlockID current, JSValue& result)
{
    JSGlobalObject* realm = m_realm.get();
    const BodyIndex& index = activation.index;
    auto answer = [&](JSValue returned) {
        if (!result || !isKnown(result))
            result = activation.waysDifferInWhatTheyReturn ? unknown() : returned;
        else if (isKnown(returned) && !areStrictlyEqual(result, returned)) {
            activation.waysDifferInWhatTheyReturn = true;
            result = unknown();
        }
    };

    while (current) {
        BodyBlock block = index.block(current);
        for (BodyValue value : block.values()) {
            if (!hasRoomLeft())
                end();
            if (!m_operationsLeft || !m_operationsLeftInAll) {
                result = unknown();
                return;
            }
            --m_operationsLeft;
            --m_operationsLeftInAll;
            activation.values[value.id().index()] = inHandsOfProgram(execute(activation, value));
        }

        BodyTerminator terminator = block.terminator();
        current = { };
        switch (terminator.kind()) {
        case TerminatorKind::Jump:
            if (takeEdge(activation, terminator, 0, terminator.next()))
                current = terminator.next().block();
            break;
        case TerminatorKind::Branch: {
            JSValue condition = at(activation, terminator.operand(0));
            if (!isKnown(condition)) {
                current = goEachWayFrom(activation, block, result);
                break;
            }
            bool isTaken = condition.toBoolean(realm);
            BodyTarget target = isTaken ? terminator.taken() : terminator.notTaken();
            if (takeEdge(activation, terminator, isTaken ? 0 : 1, target))
                current = target.block();
            break;
        }
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString: {
            JSValue subject = at(activation, terminator.operand(0));
            PriceOfBuiltin priceOfCases;
            priceOfCases.characters = terminator.numberOfTargets();
            if (!isKnown(subject) || !payToLayOut(subject) || !pay(priceOfCases)) {
                current = goEachWayFrom(activation, block, result);
                break;
            }
            uint32_t chosen = terminator.numberOfTargets() - 1;
            uint32_t position = 0;
            if (terminator.kind() == TerminatorKind::Switch && subject.isNumber()) {
                for (int64_t written : terminator.as<TerminatorKind::Switch>().cases()) {
                    if (static_cast<double>(written) == subject.asNumber()) {
                        chosen = position;
                        break;
                    }
                    ++position;
                }
            } else if (terminator.kind() == TerminatorKind::SwitchString && subject.isString()) {
                auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
                String text = asString(subject)->value(realm);
                bool isRead = !hasThrown(scope);
                Vector<char16_t> buffer;
                for (StringID written : terminator.as<TerminatorKind::SwitchString>().strings()) {
                    if (isRead && m_program.strings.viewOf(written, buffer) == text) {
                        chosen = position;
                        break;
                    }
                    ++position;
                }
            }
            BodyTarget target = terminator.targets()[chosen];
            if (takeEdge(activation, terminator, chosen, target))
                current = target.block();
            break;
        }
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
            answer(at(activation, terminator.operand(0)));
            break;
        default:
            break;
        }
    }
}

JSValue RehearsalOfRegExpTexts::runBlocks(Activation& activation)
{
    JSValue result;
    runFrom(activation, entryBlock, result);
    return result ? result : unknown();
}

JSValue RehearsalOfRegExpTexts::enterWithEntryParameters(FunctionID function, std::span<const JSValue> entryParameters)
{
    const BodyIndex* index = m_bodies.find(function);
    if (!index || m_depth >= deepestCall || !m_operationsLeft || !m_operationsLeftInAll)
        return unknown();
    uint32_t price = index->numberOfValues() / valuesMadeRoomForAtPriceOfOneOperation;
    if (price >= m_operationsLeft || price >= m_operationsLeftInAll)
        return unknown();
    unsigned& regionsOpenAtEntryPlusOne = m_regionsOpenAtEntryPlusOne[function.index()];
    if (regionsOpenAtEntryPlusOne && regionsOpenAtEntryPlusOne <= m_openRegions)
        return unknown();
    SetForScope entered(regionsOpenAtEntryPlusOne, m_openRegions + 1);
    m_operationsLeft -= price;
    m_operationsLeftInAll -= price;
    SetForScope deeper(m_depth, m_depth + 1);
    Activation activation { *index, function, { }, { }, { } };
    activation.values.fill(*m_vm, index->numberOfValues(), [](JSValue*) { });
    BodyBlock entry = index->block(entryBlock);
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
        activation.values[entry.parameter(parameter).index()] = parameter < entryParameters.size() ? inHandsOfProgram(entryParameters[parameter]) : unknown();
    JSValue result = runBlocks(activation);
    const Function& record = m_program.at(function);
    if (!record.has(FunctionFlags::IsAsync) || record.has(FunctionFlags::IsGenerator) || !isKnown(result))
        return result;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    noteObjectMade();
    JSValue promise = JSPromise::resolvedPromise(m_realm.get(), result);
    if (scope.exception()) {
        letGoOfWhatWasThrown(scope);
        return unknown();
    }
    return promise;
}

JSValue RehearsalOfRegExpTexts::enterWithNothingKnown(FunctionID function)
{
    return enterWithEntryParameters(function, { });
}

JSValue RehearsalOfRegExpTexts::enter(FunctionID function, std::span<const JSValue> captures, JSValue thisValue, JSValue callee, JSValue newTarget, std::span<const JSValue> arguments)
{
    const Function& record = m_program.at(function);
    Values entryParameters;
    unsigned numberOfEntryParameters = record.numberOfEntryParameters();
    unsigned positional = record.numberOfPositionalParameters();
    auto behind = [&](unsigned first) {
        return listOf(arguments.subspan(std::min<size_t>(first, arguments.size())));
    };
    unsigned nextCapture = 0;
    unsigned nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        switch (record.entryParameterKind(index)) {
        case EntryParameter::Capture:
            entryParameters.append(nextCapture < captures.size() ? captures[nextCapture] : unknown());
            ++nextCapture;
            break;
        case EntryParameter::This:
            entryParameters.append(thisValue);
            break;
        case EntryParameter::Callee:
            entryParameters.append(callee);
            break;
        case EntryParameter::NewTarget:
            entryParameters.append(newTarget);
            break;
        case EntryParameter::ArgumentCount:
            entryParameters.append(jsNumber(static_cast<uint32_t>(arguments.size())));
            break;
        case EntryParameter::Parameter:
            if (nextParameter < positional)
                entryParameters.append(nextParameter < arguments.size() ? arguments[nextParameter] : jsUndefined());
            else
                entryParameters.append(behind(positional));
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            entryParameters.append(behind(positional));
            break;
        }
    }
    return enterWithEntryParameters(function, entryParameters.span());
}

void RehearsalOfRegExpTexts::runModule(ModuleID id)
{
    StateOfModule& state = m_statesOfModules[id.index()];
    FunctionID body = m_program.at(id).body;
    if (state != StateOfModule::IsNotEntered || !body || m_depth >= deepestCall || m_isOver || !m_operationsLeftInAllOfModules[id.index()])
        return;
    state = StateOfModule::IsRunning;
    m_rehearsed.set(body.index());
    if (m_goesThroughStartOfProgram) {
        for (const ModuleRequest& request : m_program.at(id).requests) {
            if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation && !m_program.at(request.target).flags.contains(ModuleFlags::IsLibrary))
                runModule(request.target);
        }
    }
    if (VariableID exported = m_program.at(id).exportsVariable; exported && !m_variables[exported.index()])
        m_variables[exported.index()] = constructEmptyObject(m_realm.get());
    StartThatDrawsOnModule start(*this, id, m_goesThroughStartOfProgram ? std::numeric_limits<uint32_t>::max() : operationsFromOneStart);
    m_resultsOfModules[id.index()] = enterWithNothingKnown(body);
    state = StateOfModule::HasRun;
}

bool RehearsalOfRegExpTexts::rehearse(FunctionID function)
{
    const Function& record = m_program.at(function);
    m_sawTextThatNobodyKnows = false;
    if (record.isModuleBody())
        runModule(record.module);
    else if (!m_rehearsed.set(function.index())) {
        StartThatDrawsOnModule start(*this, record.module, operationsFromOneStart);
        enterWithNothingKnown(function);
    }
    return m_sawTextThatNobodyKnows;
}

void RehearsalOfRegExpTexts::run(std::span<const FunctionID> functionsThatCallRegExp)
{
    if (!isKnown(m_regExpConstructor))
        return;
    m_operationsLeft = operationsFromOneStart;

    for (const Entry& entry : m_program.entries) {
        SetForScope goesThroughStartOfProgram(m_goesThroughStartOfProgram, true);
        runModule(entry.module);
    }

    for (size_t module = 0; module < m_operationsLeftInAllOfModules.size(); ++module)
        m_operationsLeftInAllOfModules[module] = std::min(m_operationsLeftInAllOfModules[module], m_operationsInAllOfModules[module]);

    for (size_t index = 0; index < m_handlers.size(); ++index) {
        ClosureOfProgram handler = closureIn(m_handlers[index]);
        StartThatDrawsOnModule start(*this, m_program.at(handler.function).module, operationsFromOneStart);
        Values captures;
        appendElementsOf(handler.captures, captures);
        Values arguments;
        for (unsigned parameter = m_program.at(handler.function).numberOfPositionalParameters(); parameter--;)
            arguments.append(unknown());
        enter(handler.function, captures.span(), unknown(), m_handlers[index], jsUndefined(), arguments.span());
    }

    if (m_isOver)
        return;
    m_operationsLeftInAllOfModules = m_operationsInAllOfModules;

    size_t numberOfFunctions = m_program.functions.size();
    BitVector lacksTexts(numberOfFunctions);
    for (FunctionID function : functionsThatCallRegExp) {
        if (rehearse(function))
            lacksTexts.set(function.index());
    }

    if (lacksTexts.isEmpty())
        return;
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID function(static_cast<uint32_t>(index));
        if (m_rehearsed.get(index))
            continue;
        bool namesOne = std::ranges::any_of(m_summaries.at(function).functionsNamed, [&](FunctionID named) {
            return lacksTexts.get(named.index());
        });
        if (namesOne)
            rehearse(function);
    }
}

void RehearsalOfRegExpTexts::storeTexts(ProgramFactTables& program)
{
    struct TextWithBytes {
        Vector<uint8_t> bytes;
        RegExpTextWorkedOut text;
    };

    Vector<TextWithBytes> textsWorkedOut;
    for (RegExpTextWorkedOut& text : m_found) {
        Vector<uint8_t> bytes = encodeProgramString(StringView(text.pattern));
        if (StringID spelled = m_program.strings.find(bytes.span()))
            program.regExpTextsInSight.append(RegExpTextInSight { spelled, text.flags, text.isOneOfAllAtSomeCall });
        else
            textsWorkedOut.append(TextWithBytes { WTF::move(bytes), WTF::move(text) });
    }
    m_found.clear();

    AOT::sort(program.regExpTextsInSight, { }, [](const RegExpTextInSight& text) {
        return std::tuple(text.pattern, text.flags);
    });
    AOT::sort(textsWorkedOut, [](const TextWithBytes& a, const TextWithBytes& b) {
        std::strong_ordering order = compareProgramStrings(a.bytes.span(), b.bytes.span());
        return order != std::strong_ordering::equal ? order == std::strong_ordering::less : a.text.flags < b.text.flags;
    });
    program.regExpTextsWorkedOut.reserveInitialCapacity(textsWorkedOut.size());
    for (TextWithBytes& text : textsWorkedOut)
        program.regExpTextsWorkedOut.append(WTF::move(text.text));

    for (auto& [site, hasText] : m_callsOfRegExp) {
        FunctionID function(static_cast<uint32_t>(site >> 32));
        if (!hasText && !m_program.admitsReplacedBuiltins && !isLibraryCode(m_program, m_program.at(function)))
            program.regExpCallsWithoutText.append(RegExpCallWithoutText { function, ValueID(static_cast<uint32_t>(site)) });
    }
    AOT::sort(program.regExpCallsWithoutText, { }, [](const RegExpCallWithoutText& call) {
        return std::tuple(call.function, call.site);
    });
}

static Vector<FunctionID> functionsThatCallRegExpWithTextsToWorkOut(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID regExp = table.canonicalID(Builtins::RegExp);
    Vector<FunctionID> functions;
    for (size_t number = 0; number < program.functions.size(); ++number) {
        FunctionID function(static_cast<uint32_t>(number));
        const BodyIndex* index = bodies.find(function);
        if (!index || isLibraryCode(program, program.at(function)))
            continue;
        auto isWrittenAs = [&](ValueID operand, Opcode opcode) {
            ValueID behindCasts = index->stripCasts(operand);
            return !index->isParameter(behindCasts) && index->opcodeOf(behindCasts) == opcode;
        };
        auto namesRegExp = [&](BodyValue written) {
            ValueID callee;
            written.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (written.operandUses(operandIndex).contains(OperandUse::Calls))
                    callee = operand;
            });
            auto builtinWrittenAt = [&](ValueID operand) {
                return operand && isWrittenAs(operand, Opcode::Builtin) ? index->value(index->stripCasts(operand)).as<Opcode::Builtin>().builtin() : BuiltinID();
            };
            BuiltinID named = builtinWrittenAt(callee);
            if (callee && isWrittenAs(callee, Opcode::PropGet)) {
                auto read = index->value(index->stripCasts(callee)).as<Opcode::PropGet>();
                if (BuiltinID holder = builtinWrittenAt(read.objectOperand()); holder && read.key())
                    named = findOwnMember(program, table.canonicalID(holder), read.key());
            }
            return named && table.canonicalID(named) == regExp;
        };
        bool hasTextsToWorkOut = std::ranges::any_of(summaries.at(function).calls, [&](const SummaryCall& call) {
            bool isCalledByBinding = call.callee == SummaryCall::Callee::Builtin;
            if (isCalledByBinding ? table.canonicalID(call.builtin) != regExp : !program.admitsReplacedBuiltins || call.isMadeByProtocol || !call.site)
                return false;
            BodyValue written = index->value(call.site);
            if (!isCalledByBinding && !namesRegExp(written))
                return false;
            if (written.opcode() != Opcode::Call && written.opcode() != Opcode::Construct)
                return true;
            std::span<const ValueID> arguments = written.variadicOperands();
            if (arguments.empty() || !isWrittenAs(arguments[0], Opcode::String))
                return true;
            return arguments.size() > 1 && !isWrittenAs(arguments[1], Opcode::String) && !isWrittenAs(arguments[1], Opcode::Undefined);
        });
        if (hasTextsToWorkOut)
            functions.append(function);
    }
    return functions;
}

void workOutRegExpTexts(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, ProgramFactTables& tables)
{
    if (Options::crashIfInterpreterIsUsed())
        return;
    Vector<FunctionID> functions = functionsThatCallRegExpWithTextsToWorkOut(program, bodies, summaries);
    if (functions.isEmpty())
        return;
    RuntimeHooks::NoHelperThreadsScope noHelperThreads;
    RefPtr<VM> vm = VM::create(HeapType::Small);
    {
        JSLockHolder locker(*vm);
        {
            RehearsalOfRegExpTexts rehearsal(*vm, program, bodies, summaries);
            rehearsal.run(functions.span());
            rehearsal.storeTexts(tables);
        }
        vm = nullptr;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
