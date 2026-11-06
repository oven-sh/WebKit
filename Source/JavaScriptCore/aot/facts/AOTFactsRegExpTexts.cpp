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
#include "IteratorOperations.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSArrayIterator.h"
#include "JSBoundFunctionInlines.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "JSSetIterator.h"
#include "JSStringIterator.h"
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
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t operationsFromOneStart = 1 << 17;
static constexpr uint64_t operationsInAllForEachOfProgram = 4;
static constexpr uint64_t operationsForText = 1 << 8;
static constexpr uint64_t operationsInAllOfSmallestProgram = 8 * operationsFromOneStart;
static constexpr unsigned deepestCall = 256;
static constexpr unsigned mostOpenRegions = 1024;
static constexpr uint32_t valuesMadeRoomForAtPriceOfOneOperation = 16;
static constexpr uint8_t timesThroughBlockWhileGoingBothWays = 4;
static constexpr uint32_t operationsForBuiltinHandedRegExp = 1 << 10;
static constexpr size_t mostBytesMade = 256 * MB;

enum class GrowthOfWork : uint8_t {
    WithWhatIsHanded = 1 << 0,
    WithNumbersHanded = 1 << 1,
    WithPropertiesOfWhatIsHanded = 1 << 2,
    WithAllThatIsReached = 1 << 3,
    CopiesWhatIsHeld = 1 << 4,
    CopiesWhatIsHeldIntoStructuresOfStructures = 1 << 5,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithWhatIsHanded[] = {
    "ObjectDefineProperty"_s, "ObjectGetOwnPropertyDescriptor"_s, "ObjectGetPrototypeOf"_s, "ObjectSetPrototypeOf"_s, "ObjectHasOwn"_s, "ObjectPrototypeHasOwnProperty"_s,
    "ObjectPrototypeToString"_s, "ObjectPrototypeValueOf"_s, "ArrayPrototypeToString"_s, "ErrorPrototypeToString"_s, "ObjectPrototype__lookupGetter__"_s,
    "ObjectPrototype__defineGetter__"_s, "ObjectPrototype__defineSetter__"_s, "ObjectFromEntries"_s, "Object"_s, "ReflectDefineProperty"_s, "ReflectGetOwnPropertyDescriptor"_s,
    "ReflectGetPrototypeOf"_s, "FunctionPrototypeBind"_s, "FunctionPrototypeToString"_s, "Symbol"_s, "SymbolFor"_s, "String"_s, "Number"_s, "Boolean"_s, "BigInt"_s, "RegExp"_s,
    "Error"_s, "TypeError"_s, "RangeError"_s, "ErrorCaptureStackTrace"_s, "ErrorIsError"_s, "Proxy"_s, "StringPrototypeCharCodeAt"_s, "StringPrototypeToUpperCase"_s,
    "StringPrototypeToLowerCase"_s, "StringPrototypeSplit"_s, "StringPrototypeSlice"_s, "StringPrototypeIncludes"_s, "StringPrototypeIndexOf"_s, "StringPrototypeLastIndexOf"_s,
    "StringPrototypeCharAt"_s, "StringPrototypeStartsWith"_s, "StringPrototypeEndsWith"_s, "StringPrototypeTrim"_s, "StringPrototypeTrimLeft"_s, "StringPrototypeAt"_s,
    "StringPrototypeMatch"_s, "StringPrototypeToString"_s, "StringPrototypeSymbolIterator"_s, "NumberPrototypeToString"_s, "NumberIsSafeInteger"_s, "NumberIsInteger"_s,
    "NumberIsFinite"_s, "ParseInt"_s, "IsFinite"_s, "MathMin"_s, "MathMax"_s, "MathFloor"_s, "MathCeil"_s, "MathAbs"_s, "MathPow"_s, "ArrayIsArray"_s, "ArrayPrototypePush"_s,
    "ArrayPrototypePop"_s, "ArrayPrototypeUnshift"_s, "ArrayPrototypeJoin"_s, "ArrayPrototypeIncludes"_s, "ArrayPrototypeIndexOf"_s, "ArrayPrototypeSlice"_s,
    "ArrayPrototypeConcat"_s, "ArrayPrototypeValues"_s, "ArrayPrototypeReverse"_s, "Map"_s, "MapPrototypeSet"_s, "MapPrototypeGet"_s, "MapPrototypeHas"_s, "MapPrototypeDelete"_s,
    "MapPrototypeClear"_s, "MapPrototypeEntries"_s, "Set"_s, "SetPrototypeAdd"_s, "SetPrototypeHas"_s, "SetPrototypeEntries"_s, "WeakSet"_s, "WeakSetPrototypeAdd"_s,
    "WeakSetPrototypeHas"_s, "WeakSetPrototypeDelete"_s, "WeakMap"_s, "WeakMapPrototypeGet"_s, "WeakMapPrototypeSet"_s, "WeakRef"_s, "WeakRefPrototypeDeref"_s,
    "FinalizationRegistry"_s, "FinalizationRegistryPrototypeRegister"_s, "EncodeURIComponent"_s, "EncodeURI"_s, "DecodeURI"_s, "RegExpPrototypeTest"_s, "RegExpPrototypeExec"_s,
    "PromiseResolve"_s, "PromiseAll"_s, "DataView"_s, "ArrayBufferIsView"_s, "MathImul"_s, "NumberIsNaN"_s, "IsNaN"_s, "SetPrototypeKeys"_s, "MapPrototypeKeys"_s,
    "MapPrototypeValues"_s, "StringFromCharCode"_s, "ArrayPrototypeAt"_s, "MathRound"_s, "StringPrototypeSubstring"_s, "StringFromCodePoint"_s, "StringPrototypeNormalize"_s,
    "StringPrototypeMatchAll"_s, "PromiseRace"_s, "ObjectPrototypePropertyIsEnumerable"_s, "StringPrototypeTrimEnd"_s, "ArrayPrototypeEntries"_s, "SyntaxError"_s,
    "ReferenceError"_s, "ObjectIsExtensible"_s, "ArrayPrototypeSplice"_s, "ArrayPrototypeShift"_s, "NumberPrototypeToFixed"_s, "DecodeURIComponent"_s, "ArrayPrototypeFill"_s,
    "StringPrototypeSearch"_s, "SetPrototypeClear"_s, "PromiseWithResolvers"_s, "ArrayPrototypeToReversed"_s, "ArrayPrototypeMap"_s, "ArrayPrototypeFilter"_s,
    "ArrayPrototypeSort"_s, "ArrayPrototypeFind"_s, "ArrayPrototypeForEach"_s, "ArrayPrototypeSome"_s, "ArrayPrototypeEvery"_s, "ArrayPrototypeReduce"_s,
    "ArrayPrototypeReduceRight"_s, "MapPrototypeForEach"_s, "StringPrototypeReplace"_s, "StringPrototypeReplaceAll"_s, "Promise"_s, "PromisePrototypeThen"_s, "JSONParse"_s,
    "ArrayPrototypeFlatMap"_s, "ArrayPrototypeFindIndex"_s, "ArrayPrototypeFindLastIndex"_s, "ArrayPrototypeToSorted"_s, "ArrayFrom"_s, "MapGroupBy"_s, "SetPrototypeForEach"_s,
    "PromisePrototypeCatch"_s, "PromisePrototypeFinally"_s, "FunctionPrototypeCall"_s, "FunctionPrototypeApply"_s, "ReflectApply"_s,
};

static constexpr ASCIILiteral builtinsThatCopyWhatIsHeld[] = {
    "ObjectGetOwnPropertyDescriptor"_s, "ReflectGetOwnPropertyDescriptor"_s, "ObjectValues"_s,
};

static constexpr ASCIILiteral builtinsThatCopyWhatIsHeldIntoStructuresOfStructures[] = {
    "ObjectGetOwnPropertyDescriptors"_s, "ObjectEntries"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithNumbersHanded[] = {
    "StringPrototypeRepeat"_s, "StringPrototypePadStart"_s, "StringPrototypePadEnd"_s, "Array"_s, "ArrayBuffer"_s, "Float64Array"_s, "Uint8Array"_s, "Int8Array"_s, "Int32Array"_s, "Uint16Array"_s, "Uint32Array"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithPropertiesOfWhatIsHanded[] = {
    "ObjectAssign"_s, "ObjectKeys"_s, "ObjectValues"_s, "ObjectEntries"_s, "ObjectFreeze"_s, "ObjectSeal"_s, "ObjectIsFrozen"_s, "ObjectGetOwnPropertyDescriptors"_s,
    "ObjectGetOwnPropertyNames"_s, "ObjectDefineProperties"_s, "ObjectCreate"_s, "ReflectOwnKeys"_s, "ObjectGetOwnPropertySymbols"_s,
};

static constexpr ASCIILiteral builtinsWhoseWorkGrowsWithAllThatIsReached[] = {
    "JSONStringify"_s, "ArrayPrototypeFlat"_s, "StringRaw"_s,
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

static unsigned mostTermsThatVaryInOneAlternative(const Yarr::PatternDisjunction& disjunction)
{
    unsigned most = 0;
    for (const auto& alternative : disjunction.m_alternatives) {
        unsigned inThisOne = 0;
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            if (term.quantityType != Yarr::QuantifierType::FixedCount)
                ++inThisOne;
            if (term.type == Yarr::PatternTerm::Type::ParenthesesSubpattern || term.type == Yarr::PatternTerm::Type::ParentheticalAssertion)
                inThisOne += mostTermsThatVaryInOneAlternative(*term.parentheses.disjunction);
        }
        most = std::max(most, inThisOne);
    }
    return most;
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

    bool isKnown(JSValue value) const { return !value || !value.isObject() || asObject(value)->structure() != m_unknown->structure(); }
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

    void noteTexts(std::span<const JSValue> arguments);
    JSValue call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments);
    JSValue construct(JSValue callee, JSValue newTarget, std::span<const JSValue> arguments);
    bool appendElementsOf(JSValue list, Values&);
    struct PriceOfBuiltin {
        uint64_t handed { 0 };
        uint64_t walked { 0 };
        unsigned termsThatVary { 0 };
        bool mayMatch { false };
    };
    OptionSet<GrowthOfWork> workOf(JSValue builtin) const
    {
        if (!builtin || !builtin.isCell())
            return { };
        if (builtin.inherits<JSBoundFunction>())
            return { GrowthOfWork::WithWhatIsHanded };
        auto listed = m_workOfBuiltins.find(builtin.asCell());
        return listed == m_workOfBuiltins.end() ? OptionSet<GrowthOfWork>() : listed->value;
    }
    bool passesWhatIsLeft(const PriceOfBuiltin& price) const { return std::max(price.handed, price.walked) >= std::min<uint64_t>(m_operationsLeft, m_operationsLeftInAll); }
    bool take(JSValue, OptionSet<GrowthOfWork>, PriceOfBuiltin&, unsigned depth);
    JSObject* originalOf(JSValue wrapper) const { return wrapper && wrapper.isCell() ? m_originalsOfWrappers.get(wrapper.asCell()) : nullptr; }
    JSValue inHandsOfProgram(JSValue);
    void exchangeBuiltinsHeldBy(JSValue made, unsigned levels);
    bool pay(const PriceOfBuiltin&);
    bool payForWalkThatEndedInRefusal(const PriceOfBuiltin&);
    bool payForBuiltin(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments);
    bool payForConversion(JSValue);
    bool payFor(JSValue);
    bool payForNamesListedOf(JSObject*);
    unsigned termsThatVaryIn(RegExp*);
    bool mayBeConverted(JSValue value) { return isKnown(value) && payForConversion(value); }
    std::optional<uint64_t> elementsOfObjectHandedToBuiltin(JSObject*) const;
    JSValue noteMadeByBuiltin(JSValue);
    bool mayMatchWithoutEnd(RegExp*);
    bool isAsRealmMadeIt(RegExpObject* pattern) { return pattern->structure() == m_realm->regExpStructure() && pattern->getLastIndex().isNumber() && m_realm.get()->regExpPrimordialPropertiesWatchpointSet().isStillValid(); }
    void noteThrown(Exception*);
    bool hasThrown(TopExceptionScope&);
    JSArray* newList();
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
    JSValue m_regExpConstructor;
    JSValue m_objectAssign;
    UncheckedKeyHashSet<JSCell*> m_functionsThatAskTheWorld;
    UncheckedKeyHashMap<JSCell*, OptionSet<GrowthOfWork>> m_workOfBuiltins;
    UncheckedKeyHashMap<JSCell*, JSObject*> m_originalsOfWrappers;
    UncheckedKeyHashMap<JSCell*, JSObject*> m_wrappersOfOriginals;
    Values m_wrappersMade;
    JSValue m_reflectConstruct;
    JSValue m_functionPrototypeBind;
    UncheckedKeyHashSet<JSCell*> m_handsOnItsReceiver;
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
    Identifier m_laidUnderNumbersThatNobodyKnows;
    UncheckedKeyHashMap<uint64_t, Identifier> m_privateNames;
    UncheckedKeyHashMap<uint32_t, Vector<BlockID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_joins;
    Vector<StateOfModule> m_statesOfModules;
    Vector<ModuleID> m_modulesOfRuntimeModules;
    BitVector m_rehearsed;

    bool hasRoomLeft() const { return m_bytesMade <= mostBytesMade; }

    uint32_t m_operationsLeft { 0 };
    uint64_t m_operationsLeftInAll { operationsInAllOfSmallestProgram };
    bool m_isOver { false };
    uint64_t m_operationsLeftInAllAtLastText { 0 };
    size_t m_bytesMade { 0 };
    unsigned m_depth { 0 };
    unsigned m_openRegions { 0 };
    Vector<unsigned> m_regionsOpenAtEntryPlusOne;
    bool m_sawTextThatNobodyKnows { false };
    uint64_t m_operation { 0 };
    UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_callsOfRegExp;

    UncheckedKeyHashMap<std::pair<String, uint32_t>, bool> m_patternsThatMayMatchWithoutEnd;
    UncheckedKeyHashMap<std::pair<String, uint32_t>, unsigned> m_termsThatVaryInPatterns;
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

    m_unknown.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
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
    for (size_t function = 0; function < program.functions.size(); ++function)
        m_operationsLeftInAll += operationsInAllForEachOfProgram * summaries.at(FunctionID(static_cast<uint32_t>(function))).numberOfOperations;

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
    for (ASCIILiteral token : builtinsThatCopyWhatIsHeld) {
        if (JSValue function = inRealmByToken(token); isKnown(function) && function.isCell())
            m_workOfBuiltins.find(function.asCell())->value.add(GrowthOfWork::CopiesWhatIsHeld);
    }
    for (ASCIILiteral token : builtinsThatCopyWhatIsHeldIntoStructuresOfStructures) {
        if (JSValue function = inRealmByToken(token); isKnown(function) && function.isCell())
            m_workOfBuiltins.find(function.asCell())->value.add(GrowthOfWork::CopiesWhatIsHeldIntoStructuresOfStructures);
    }
    m_reflectConstruct = inRealmByToken("ReflectConstruct"_s);
    m_functionPrototypeBind = inRealmByToken("FunctionPrototypeBind"_s);
    for (ASCIILiteral token : { "FunctionPrototypeCall"_s, "FunctionPrototypeApply"_s, "ReflectApply"_s }) {
        if (JSValue function = inRealmByToken(token); isKnown(function) && function.isCell())
            m_handsOnItsReceiver.add(function.asCell());
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
        scope.clearException();
        found = cell && *cell ? JSValue(*cell) : unknown();
    }
    return found;
}

bool RehearsalOfRegExpTexts::hasThrown(TopExceptionScope& scope)
{
    Exception* exception = scope.exception();
    if (!exception)
        return false;
    noteThrown(exception);
    scope.clearException();
    return true;
}

JSArray* RehearsalOfRegExpTexts::newList()
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSArray* list = constructEmptyArray(m_realm.get(), nullptr);
    return hasThrown(scope) ? nullptr : list;
}

bool RehearsalOfRegExpTexts::append(JSArray* list, JSValue element)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    list->push(m_realm.get(), element);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::storeAt(JSObject* object, unsigned index, JSValue stored)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    object->putDirectIndex(m_realm.get(), index, stored);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::storeUnder(JSObject* object, const Identifier& name, JSValue stored)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    object->putDirectMayBeIndex(m_realm.get(), name, stored);
    return !hasThrown(scope);
}

bool RehearsalOfRegExpTexts::setLengthOf(JSArray* list, unsigned length)
{
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
    JSArray* held = asArray(list);
    bool isGoneThrough = !record.has(FunctionFlags::IsGenerator) && record.hasBody();
    JSObject* made = JSNativeStdFunction::create(vm, realm, record.length, String(), [this, function, held, isGoneThrough](JSGlobalObject* caller, CallFrame* frame) {
        if (!m_operationsLeft || !m_operationsLeftInAll) {
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
        return JSValue::encode(enter(function, captures.span(), frame->thisValue(), frame->jsCallee(), jsUndefined(), arguments.span()));
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
            scope.clearException();
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
    JSObject* made = constructEmptyObject(m_realm.get());
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
        scope.clearException();
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

void RehearsalOfRegExpTexts::noteTexts(std::span<const JSValue> arguments)
{
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue pattern = arguments.size() > 0 ? arguments[0] : jsUndefined();
    JSValue flags = arguments.size() > 1 ? arguments[1] : jsUndefined();
    uint64_t site = m_operation;
    m_callsOfRegExp.add(site, false);
    if (!isKnown(pattern) || (!pattern.inherits<RegExpObject>() && !payFor(pattern)) || (isKnown(flags) && !payFor(flags))) {
        m_sawTextThatNobodyKnows = true;
        return;
    }

    String text = emptyString();
    std::optional<OptionSet<Yarr::Flags>> flagsOfText = OptionSet<Yarr::Flags>();
    if (auto* copied = dynamicDowncast<RegExpObject>(pattern)) {
        text = copied->regExp()->pattern();
        flagsOfText = copied->regExp()->flags();
    } else if (!pattern.isUndefined())
        text = pattern.toWTFString(realm);
    if (scope.exception()) {
        scope.clearException();
        m_sawTextThatNobodyKnows = true;
        return;
    }

    bool knowsFlags = isKnown(flags);
    if (!knowsFlags) {
        m_sawTextThatNobodyKnows = true;
        flagsOfText = OptionSet<Yarr::Flags>();
    } else if (!flags.isUndefined()) {
        String written = flags.toWTFString(realm);
        if (scope.exception()) {
            scope.clearException();
            return;
        }
        flagsOfText = Yarr::parseFlags(written);
    }
    m_callsOfRegExp.set(site, true);
    if (!flagsOfText || text.isNull() || text.length() > m_longestText)
        return;

    uint16_t significant = ImageRegExp::significantFlags(*flagsOfText).toRaw();
    if (m_seen.add({ text, significant }).isNewEntry) {
        m_found.append(RegExpTextWorkedOut { text.isolatedCopy(), significant, knowsFlags });
        m_operationsLeft = std::max(m_operationsLeft, operationsFromOneStart);
        if (m_operationsLeftInAll) {
            m_operationsLeftInAll = m_operationsLeftInAllAtLastText > operationsForText ? m_operationsLeftInAllAtLastText - operationsForText : 0;
            m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        }
    }
}

JSValue RehearsalOfRegExpTexts::makeNumberThatNobodyKnows()
{
    VM& vm = *m_vm;
    JSObject* number = constructEmptyObject(vm, m_realm->nullPrototypeObjectStructure());
    number->putDirect(vm, vm.propertyNames->toPrimitiveSymbol, m_unknown->getDirect(vm, vm.propertyNames->toPrimitiveSymbol));
    return number;
}

void RehearsalOfRegExpTexts::layUnderNumberThatNobodyKnows(JSObject* object, JSValue number, JSValue laid)
{
    VM& vm = *m_vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue found = object->getDirect(vm, m_laidUnderNumbersThatNobodyKnows);
    JSMap* all = found ? uncheckedDowncast<JSMap>(found) : JSMap::create(vm, m_realm->mapStructure());
    if (!found)
        object->putDirect(vm, m_laidUnderNumbersThatNobodyKnows, all);
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
        options->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            list(options->getDirect(entry.offset()));
            return true;
        });
    }
}

JSValue RehearsalOfRegExpTexts::inHandsOfProgram(JSValue value)
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
    scope.clearException();
    JSValue length = original->get(realm, vm.propertyNames->length);
    scope.clearException();
    String nameOfWrapper = name && name.isString() ? name.toWTFString(realm) : String();
    scope.clearException();
    JSObject* wrapper = JSNativeStdFunction::create(vm, realm, length && length.isUInt32() ? length.asUInt32() : 0, nameOfWrapper, [this, original](JSGlobalObject* caller, CallFrame* frame) {
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
    scope.clearException();
    for (const Identifier& name : names) {
        if (name == vm.propertyNames->name || name == vm.propertyNames->length)
            continue;
        PropertySlot slot(original, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = original->methodTable()->getOwnPropertySlot(original, realm, name, slot);
        scope.clearException();
        if (!isFound)
            continue;
        if (slot.isAccessor())
            wrapper->putDirectAccessor(realm, name, slot.getterSetter(), slot.attributes());
        else if (slot.isValue())
            wrapper->putDirect(vm, name, slot.getValue(realm, name), slot.attributes());
        scope.clearException();
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
            if (held && held.isCallable())
                list->setIndexQuickly(vm, index, inHandsOfProgram(held));
            else if (held)
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
    if (JSObject* original = originalOf(thisValue); original && original->isConstructor() && !callee.inherits<JSNativeStdFunction>() && callee != m_functionPrototypeBind && !m_handsOnItsReceiver.contains(callee.asCell()))
        thisValue = original;
    if (!isKnown(callee) || !callee.isCell() || m_functionsThatAskTheWorld.contains(callee.asCell())) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    if (callee == m_reflectConstruct) {
        Values spread;
        if (arguments.size() < 2 || !isKnown(arguments[1]) || !payFor(arguments[1]) || !appendElementsOf(arguments[1], spread) || m_depth >= deepestCall) {
            noteHandedToCodeThatNobodyKnows(arguments);
            return unknown();
        }
        SetForScope deeper(m_depth, m_depth + 1);
        return construct(arguments[0], arguments.size() > 2 ? arguments[2] : arguments[0], spread.span());
    }
    if (callee == m_regExpConstructor)
        noteTexts(arguments);
    bool isOfProgram = callee.inherits<JSNativeStdFunction>();
    if (!isOfProgram && workOf(callee).isEmpty()) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    if (!isOfProgram && !isKnown(thisValue))
        return unknown();
    if (isOfProgram && !arguments.empty() && (!isKnown(thisValue) || thisValue.isUndefined()) && std::ranges::none_of(arguments, [&](JSValue argument) { return isKnown(argument); }))
        return unknown();
    bool onlyHandsOn = callee.inherits<JSBoundFunction>() || m_handsOnItsReceiver.contains(callee.asCell());
    if (!isOfProgram && !onlyHandsOn && callee != m_regExpConstructor && (isPatternMadeHere(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return isPatternMadeHere(argument); })))
        return unknown();
    if (!isOfProgram && (asksTheWorld(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return asksTheWorld(argument); }))) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    CallData callData = getCallData(callee);
    if (callData.type == CallData::Type::None)
        return unknown();

    if (onlyHandsOn) {
        PriceOfBuiltin price;
        price.handed = arguments.size();
        for (JSValue argument : arguments) {
            if (auto* handedOn = dynamicDowncast<JSArray>(argument))
                price.handed += handedOn->length();
        }
        if (!pay(price))
            return unknown();
    } else if (!isOfProgram && !payForBuiltin(callee, thisValue, arguments))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(realm, callee, callData, thisValue, list, exception);
    scope.clearException();
    if (exception)
        noteThrown(exception.get());
    if (exception || !result)
        return unknown();
    if (callee == m_regExpConstructor)
        return notePatternMadeHere(result);
    if (isOfProgram)
        return result;
    if (workOf(callee).contains(GrowthOfWork::CopiesWhatIsHeld))
        exchangeBuiltinsHeldBy(result, 1);
    else if (workOf(callee).contains(GrowthOfWork::CopiesWhatIsHeldIntoStructuresOfStructures))
        exchangeBuiltinsHeldBy(result, 2);
    return noteMadeByBuiltin(result);
}

bool RehearsalOfRegExpTexts::mayMatchWithoutEnd(RegExp* regExp)
{
    return m_patternsThatMayMatchWithoutEnd.ensure({ regExp->pattern(), regExp->flags().toRaw() }, [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern pattern(regExp->pattern(), regExp->flags(), error);
        return Yarr::hasError(error) || repeatsInsideRepeat(*pattern.m_body, false);
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

std::optional<uint64_t> RehearsalOfRegExpTexts::elementsOfObjectHandedToBuiltin(JSObject* object) const
{
    for (JSObject* holder = object; holder;) {
        if (holder->inherits<ProxyObject>())
            return std::nullopt;
        if (JSValue length = holder->getDirect(*m_vm, m_vm->propertyNames->length)) {
            if (!length.isNumber() || !(length.asNumber() <= m_longestText))
                return std::nullopt;
            return length.asNumber() > 0 ? static_cast<uint64_t>(length.asNumber()) : 0;
        }
        JSValue above = holder->getPrototypeDirect();
        holder = above.isObject() ? asObject(above) : nullptr;
    }
    return 0;
}

bool RehearsalOfRegExpTexts::take(JSValue value, OptionSet<GrowthOfWork> work, PriceOfBuiltin& price, unsigned depth)
{
    if (value.isString()) {
        price.handed += asString(value)->length();
        return true;
    }
    if (value.isNumber())
        return value.asNumber() <= m_longestText;
    if (value.isHeapBigInt()) {
        uint64_t words = value.asHeapBigInt()->length();
        price.handed += words * words;
        return true;
    }
    if (!value.isObject())
        return true;
    if (!isKnown(value))
        return true;
    if (depth >= deepestCall)
        return false;
    JSObject* object = asObject(value);
    if (auto* pattern = dynamicDowncast<RegExpObject>(object)) {
        if ((depth && isPatternMadeHere(value)) || !isAsRealmMadeIt(pattern) || mayMatchWithoutEnd(pattern->regExp()))
            return false;
        price.mayMatch = true;
        price.termsThatVary = std::max(price.termsThatVary, termsThatVaryIn(pattern->regExp()));
        return true;
    }
    if (auto* map = dynamicDowncast<JSMap>(object))
        price.handed += map->size();
    else if (auto* set = dynamicDowncast<JSSet>(object))
        price.handed += set->size();
    if (object->isCallable())
        return true;
    if (auto* iterator = dynamicDowncast<JSArrayIterator>(object))
        return take(iterator->iteratedObject(), { GrowthOfWork::WithWhatIsHanded }, price, depth + 1);
    if (auto* iterator = dynamicDowncast<JSMapIterator>(object))
        return take(iterator->iteratedObject(), { GrowthOfWork::WithWhatIsHanded }, price, depth + 1);
    if (auto* iterator = dynamicDowncast<JSSetIterator>(object))
        return take(iterator->iteratedObject(), { GrowthOfWork::WithWhatIsHanded }, price, depth + 1);
    if (auto* iterator = dynamicDowncast<JSStringIterator>(object))
        return take(iterator->iteratedString(), { GrowthOfWork::WithWhatIsHanded }, price, depth + 1);
    if (auto* view = dynamicDowncast<JSArrayBufferView>(object)) {
        price.handed += view->byteLength();
        return true;
    }
    bool walksAll = work.contains(GrowthOfWork::WithAllThatIsReached);
    auto takePlaces = [&](uint64_t places) {
        price.handed += places;
        if (passesWhatIsLeft(price))
            return false;
        if (!hasContiguous(object->indexingType()) && !hasAnyArrayStorage(object->indexingType()))
            return true;
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
    if (auto* list = dynamicDowncast<JSArray>(object))
        return takePlaces(list->length());
    std::optional<uint64_t> elements = elementsOfObjectHandedToBuiltin(object);
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

bool RehearsalOfRegExpTexts::payForWalkThatEndedInRefusal(const PriceOfBuiltin& asked)
{
    m_operationsLeft -= static_cast<uint32_t>(std::min<uint64_t>(asked.walked, m_operationsLeft));
    m_operationsLeftInAll -= std::min(asked.walked, m_operationsLeftInAll);
    return false;
}

bool RehearsalOfRegExpTexts::pay(const PriceOfBuiltin& asked)
{
    uint64_t worstCase = asked.handed;
    uint64_t most = std::max<uint64_t>(m_operationsLeft, 1);
    for (unsigned term = asked.mayMatch ? asked.termsThatVary : 0; term-- && worstCase < most;)
        worstCase *= std::max<uint64_t>(asked.handed, 1);
    if (asked.mayMatch)
        worstCase += operationsForBuiltinHandedRegExp;
    if (worstCase >= m_operationsLeft || worstCase >= m_operationsLeftInAll)
        return payForWalkThatEndedInRefusal(asked);
    uint64_t price = asked.handed + (asked.mayMatch ? operationsForBuiltinHandedRegExp : 0);
    m_operationsLeft -= static_cast<uint32_t>(price);
    m_operationsLeftInAll -= price;
    return true;
}

bool RehearsalOfRegExpTexts::payForBuiltin(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments)
{
    OptionSet<GrowthOfWork> work = workOf(callee);
    PriceOfBuiltin price;
    if (!take(thisValue, work, price, 0))
        return payForWalkThatEndedInRefusal(price);
    price.handed += arguments.size();
    for (JSValue argument : arguments) {
        if (!take(argument, work, price, 0) || passesWhatIsLeft(price))
            return payForWalkThatEndedInRefusal(price);
    }
    return pay(price);
}

bool RehearsalOfRegExpTexts::payForNamesListedOf(JSObject* object)
{
    PriceOfBuiltin price;
    for (JSObject* holder = object; holder;) {
        if (holder->inherits<ProxyObject>())
            return false;
        if (auto* list = dynamicDowncast<JSArray>(holder))
            price.handed += list->length();
        else if (auto* view = dynamicDowncast<JSArrayBufferView>(holder))
            price.handed += view->length();
        else if (auto* text = dynamicDowncast<StringObject>(holder))
            price.handed += text->internalValue()->length();
        price.handed += holder->structure()->maxOffset() + 1;
        if (passesWhatIsLeft(price))
            return false;
        JSValue above = holder->getPrototypeDirect();
        holder = above.isObject() ? asObject(above) : nullptr;
    }
    return pay(price);
}

bool RehearsalOfRegExpTexts::payFor(JSValue value)
{
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

JSValue RehearsalOfRegExpTexts::noteMadeByBuiltin(JSValue made)
{
    if (auto* list = dynamicDowncast<JSArray>(made))
        m_bytesMade += static_cast<size_t>(list->length()) * sizeof(JSValue);
    else if (auto* view = dynamicDowncast<JSArrayBufferView>(made); view && !view->hasArrayBuffer())
        m_bytesMade += view->byteLength();
    else if (auto* buffer = dynamicDowncast<JSArrayBuffer>(made))
        m_bytesMade += buffer->impl()->byteLength();
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
    if (auto* bound = dynamicDowncast<JSBoundFunction>(callee)) {
        if (m_depth >= deepestCall)
            return unknown();
        SetForScope deeper(m_depth, m_depth + 1);
        Values all;
        bound->forEachBoundArg([&](JSValue argument) {
            all.append(argument);
            return IterationStatus::Continue;
        });
        for (JSValue argument : arguments)
            all.append(argument);
        return all.hasOverflowed() ? unknown() : construct(bound->targetFunction(), newTarget == callee ? JSValue(bound->targetFunction()) : newTarget, all.span());
    }
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
    if (callee == m_regExpConstructor)
        noteTexts(arguments);
    if (asksTheWorld(newTarget) || std::ranges::any_of(arguments, [&](JSValue argument) { return asksTheWorld(argument); }))
        return unknown();
    CallData constructData = getConstructData(callee);
    if (constructData.type == CallData::Type::None)
        return unknown();

    if (workOf(callee).isEmpty() || !payForBuiltin(callee, newTarget, arguments))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* result = JSC::construct(realm, callee, constructData, list, newTarget);
    if (Exception* exception = scope.exception()) {
        noteThrown(exception);
        scope.clearException();
        return unknown();
    }
    if (!result)
        return unknown();
    return callee == m_regExpConstructor ? notePatternMadeHere(result) : noteMadeByBuiltin(result);
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
            scope.clearException();
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
        if (result.isString())
            m_bytesMade += asString(result)->length();
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
    case Opcode::Data:
        return settle(valueOfData(m_program.at(value.as<Opcode::Data>().data()).root));
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
    case Opcode::ReleaseHint:
    case Opcode::RecordRequire:
    case Opcode::SetFunctionName:
    case Opcode::DefineSetter:
    case Opcode::IteratorClose:
        return { };

    case Opcode::NewObject: {
        auto allocation = value.as<Opcode::NewObject>();
        const Layout& layout = m_program.at(allocation.layout());
        JSValue prototype = allocation.prototypeOperand() ? operand(allocation.prototypeOperand()) : JSValue();
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
        scope.clearException();
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
        if (auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand())))
            list->push(realm, operand(append.valueOperand()));
        scope.clearException();
        return { };
    }
    case Opcode::AppendHole: {
        if (auto* list = dynamicDowncast<JSArray>(operand(value.operand(0))))
            list->setLength(realm, list->length() + 1);
        scope.clearException();
        return { };
    }
    case Opcode::AppendSpread: {
        auto append = value.as<Opcode::AppendSpread>();
        auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand()));
        JSValue iterable = operand(append.iterableOperand());
        if (!list)
            return { };
        if (!isKnown(iterable) || !payFor(iterable)) {
            list->push(realm, unknown());
            scope.clearException();
            return { };
        }
        forEachInIterable(realm, iterable, [&](VM&, JSGlobalObject* realm, JSValue element) {
            list->push(realm, element);
        });
        scope.clearException();
        return { };
    }
    case Opcode::NewRegExp: {
        auto literal = value.as<Opcode::NewRegExp>();
        RegExp* regExp = RegExp::create(vm, charactersOf(literal.pattern()), toYarrFlags(literal.regExpFlags()));
        if (!regExp->isValid())
            return unknown();
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
        JSValue found = settle(object.get(realm, name));
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
        if (!isKnown(object) || !mayBeConverted(property) || object.isUndefinedOrNull())
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
        return settle(object.get(realm, name));
    }
    case Opcode::PropSet:
    case Opcode::PropDefine: {
        JSValue object = operand(value.operand(0));
        PropertyKeyID key = opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().key() : value.as<Opcode::PropDefine>().key();
        JSValue stored = operand(opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().valueOperand() : value.as<Opcode::PropDefine>().valueOperand());
        Identifier name = nameOf(key);
        if (!isKnown(object) || !object.isObject() || isGlobalObject(object) || name.isNull())
            return { };
        if (opcode == Opcode::PropDefine)
            asObject(object)->putDirectMayBeIndex(realm, name, stored);
        else {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        scope.clearException();
        return { };
    }
    case Opcode::ElemSet:
    case Opcode::ElemDefine: {
        JSValue object = operand(value.operand(0));
        JSValue property = operand(value.operand(1));
        JSValue stored = operand(opcode == Opcode::ElemSet ? value.as<Opcode::ElemSet>().valueOperand() : value.as<Opcode::ElemDefine>().valueOperand());
        if (isKnown(object) && object.isObject() && !isGlobalObject(object) && isNumberThatNobodyKnows(property)) {
            layUnderNumberThatNobodyKnows(asObject(object), property, stored);
            return { };
        }
        if (!isKnown(object) || !mayBeConverted(property) || !object.isObject() || isGlobalObject(object))
            return { };
        Identifier name = property.toPropertyKey(realm);
        if (!scope.exception()) {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        scope.clearException();
        return { };
    }
    case Opcode::DefineGetter: {
        auto definition = value.as<Opcode::DefineGetter>();
        JSValue object = operand(definition.objectOperand());
        Identifier name = definition.key() ? nameOf(definition.key()) : Identifier();
        if (isKnown(object) && object.isObject() && !isGlobalObject(object) && !name.isNull())
            asObject(object)->putDirectMayBeIndex(realm, name, unknown());
        scope.clearException();
        return { };
    }
    case Opcode::ElemHas: {
        auto test = value.as<Opcode::ElemHas>();
        JSValue object = operand(test.objectOperand());
        JSValue property = operand(test.propertyOperand());
        if (!isKnown(object) || !mayBeConverted(property) || !object.isObject() || isGlobalObject(object))
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
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
        else if (JSValue property = operand(value.as<Opcode::ElemDelete>().propertyOperand()); mayBeConverted(property))
            name = property.toPropertyKey(realm);
        if (scope.exception() || name.isNull())
            return settle({ });
        return settle(jsBoolean(JSCell::deleteProperty(asObject(object), realm, name)));
    }
    case Opcode::SetLiteralPrototype: {
        auto store = value.as<Opcode::SetLiteralPrototype>();
        JSValue object = operand(store.objectOperand());
        JSValue prototype = operand(store.prototypeOperand());
        if (isKnown(object) && object.isObject() && isKnown(prototype) && (prototype.isObject() || prototype.isNull()))
            asObject(object)->setPrototype(vm, realm, prototype);
        scope.clearException();
        return { };
    }
    case Opcode::PropHas: {
        auto test = value.as<Opcode::PropHas>();
        JSValue object = operand(test.objectOperand());
        Identifier name = nameOf(test.key());
        if (!isKnown(object) || !object.isObject() || isGlobalObject(object) || name.isNull())
            return unknown();
        return settle(jsBoolean(asObject(object)->hasProperty(realm, name)));
    }
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject: {
        bool copiesInto = opcode == Opcode::CopyDataProperties;
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
        return settle(jsAdd(realm, left, right));
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
        return isKnown(of) && !of.isBigInt() && !of.isObject() ? settle(jsNumber(~of.toInt32(realm))) : unknown();
    }
    case Opcode::ToObject: {
        JSValue of = operand(value.operand(0));
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
        return isKnown(of) && !of.isObject() ? settle(jsNumber(of.toLength(realm))) : unknown();
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
        if (!isKnown(of) || !isKnown(constructor) || !constructor.isObject())
            return unknown();
        givePrototypeTo(constructor);
        return settle(jsBoolean(asObject(constructor)->hasInstance(realm, of)));
    }
    case Opcode::ToString: {
        JSValue of = operand(value.operand(0));
        return mayBeConverted(of) ? settle(of.toString(realm)) : unknown();
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
        return settle(jsString(vm, builder.toString()));
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
        if (auto* list = dynamicDowncast<JSArray>(operand(store.argumentsOperand())))
            list->putDirectIndex(realm, store.index(), operand(store.valueOperand()));
        scope.clearException();
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
        JSValue parent = written.prototypeParentOperand() ? operand(written.prototypeParentOperand()) : JSValue(realm->objectPrototype());
        JSObject* prototype = isKnown(parent) && parent.isObject() ? constructEmptyObject(realm, asObject(parent)) : parent.isNull() ? constructEmptyObject(vm, realm->nullPrototypeObjectStructure()) : constructEmptyObject(realm);
        prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(constructor)->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
        if (JSValue heritage = operand(written.heritageOperand()); written.heritageOperand() && isKnown(heritage) && heritage.isObject())
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
        else if (JSValue computed = operand(definition.computedKeyOperand()); mayBeConverted(computed))
            name = computed.toPropertyKey(realm);
        if (!scope.exception() && isKnown(target) && target.isObject() && !name.isNull() && definition.methodKind() != MethodKind::Setter)
            asObject(target)->putDirectMayBeIndex(realm, name, definition.methodKind() == MethodKind::Method ? operand(definition.functionOperand()) : unknown());
        scope.clearException();
        return { };
    }
    case Opcode::NewClassToken:
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
        else if (JSValue computed = operand(read.computedKeyOperand()); mayBeConverted(computed))
            name = computed.toPropertyKey(realm);
        if (scope.exception() || !isKnown(base) || !base.isObject() || name.isNull())
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
    for (uint32_t index = terminator.numberOfTargets(); index--;) {
        BodyTarget target = terminator.targets()[index];
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
        if (!hasRoomLeft())
            end();
        for (BodyValue value : block.values()) {
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
            if (!isKnown(subject)) {
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
                for (StringID written : terminator.as<TerminatorKind::SwitchString>().strings()) {
                    if (isRead && charactersOf(written) == text) {
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
    JSValue promise = JSPromise::resolvedPromise(m_realm.get(), result);
    if (scope.exception()) {
        scope.clearException();
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
    if (state != StateOfModule::IsNotEntered || !body || m_depth >= deepestCall || !m_operationsLeft || !m_operationsLeftInAll)
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
    m_resultsOfModules[id.index()] = enterWithNothingKnown(body);
    state = StateOfModule::HasRun;
}

bool RehearsalOfRegExpTexts::rehearse(FunctionID function)
{
    const Function& record = m_program.at(function);
    m_operationsLeft = operationsFromOneStart;
    m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
    m_sawTextThatNobodyKnows = false;
    if (record.isModuleBody())
        runModule(record.module);
    else if (!m_rehearsed.set(function.index()))
        enterWithNothingKnown(function);
    return m_sawTextThatNobodyKnows;
}

void RehearsalOfRegExpTexts::run(std::span<const FunctionID> functionsThatCallRegExp)
{
    if (!isKnown(m_regExpConstructor))
        return;
    uint64_t operationsInAll = m_operationsLeftInAll;

    for (const Entry& entry : m_program.entries) {
        m_operationsLeft = static_cast<uint32_t>(std::min<uint64_t>(m_operationsLeftInAll, std::numeric_limits<uint32_t>::max()));
        m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        SetForScope goesThroughStartOfProgram(m_goesThroughStartOfProgram, true);
        runModule(entry.module);
    }

    for (size_t index = 0; index < m_handlers.size(); ++index) {
        ClosureOfProgram handler = closureIn(m_handlers[index]);
        m_operationsLeft = operationsFromOneStart;
        m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        Values captures;
        appendElementsOf(handler.captures, captures);
        Values arguments;
        for (unsigned parameter = m_program.at(handler.function).numberOfPositionalParameters(); parameter--;)
            arguments.append(unknown());
        enter(handler.function, captures.span(), unknown(), m_handlers[index], jsUndefined(), arguments.span());
    }

    if (m_isOver)
        return;
    m_operationsLeftInAll = operationsInAll;

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
            ValueID behindCasts = index->valueBehindCasts(operand);
            return !index->isParameter(behindCasts) && index->opcodeOf(behindCasts) == opcode;
        };
        auto namesRegExp = [&](BodyValue written) {
            ValueID callee;
            written.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (written.operandUses(operandIndex).contains(OperandUse::Calls))
                    callee = operand;
            });
            auto builtinWrittenAt = [&](ValueID operand) {
                return operand && isWrittenAs(operand, Opcode::Builtin) ? index->value(index->valueBehindCasts(operand)).as<Opcode::Builtin>().builtin() : BuiltinID();
            };
            BuiltinID named = builtinWrittenAt(callee);
            if (callee && isWrittenAs(callee, Opcode::PropGet)) {
                auto read = index->value(index->valueBehindCasts(callee)).as<Opcode::PropGet>();
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
