/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFacts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLargeList.h"
#include "AOTLocalesPassedAtCalls.h"
#include "AOTObjects.h"
#include "AOTOpcodeSets.h"
#include "AOTRemovableFeature.h"
#include "AOTResultRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet namersOfBuiltinsInProgramFacts { Opcode::Builtin, Opcode::ModuleBuiltin, Opcode::NativeBinding, Opcode::RequireBuiltinModule };
static constexpr OpcodeSet giversOfBuiltinsInProgramFacts = namersOfBuiltinsInProgramFacts | OpcodeSet { Opcode::RequireModule };
static constexpr OpcodeSet loadsByNameInProgramFacts { Opcode::ImportDynamic, Opcode::RequireDynamic };
static constexpr OpcodeSet loadsAndEvaluationsInProgramFacts { Opcode::ModuleNamespace, Opcode::ImportModule, Opcode::RequireModule, Opcode::EvaluateModule, Opcode::ImportDynamic, Opcode::RequireDynamic, Opcode::ResolveDynamic, Opcode::DirectEval };
static constexpr OpcodeSet accessesUnderConstantKeysInProgramFacts { Opcode::PropGet, Opcode::PropHas, Opcode::PropSet, Opcode::PropDefine, Opcode::PropDelete };
static constexpr OpcodeSet computedAccessesInProgramFacts { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine };
static constexpr OpcodeSet callsByValueInProgramFacts { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet constructionsInProgramFacts { Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet callsThatSpellOutArgumentsInProgramFacts { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct };
static constexpr Protocols protocolsThatShowNothingInProgramFacts { Protocol::HasInstance, Protocol::PrototypeChain };

static constexpr OptionSet<BuiltinUse> everyBuiltinUseInProgramFacts { BuiltinUse::Tested, BuiltinUse::Read, BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Written, BuiltinUse::Escaped, BuiltinUse::StoredByName };
static constexpr OptionSet<BuiltinUse> callingUsesInProgramFacts { BuiltinUse::Called, BuiltinUse::Constructed };
static constexpr OptionSet<BuiltinUse> usesOfObjectItselfInProgramFacts { BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };

static bool isObjectOfClassInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    return builtins.isObjectOfClass(canonical);
}

static ALWAYS_INLINE BuiltinID objectAnsweredByBindingInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    if (builtins.at(canonical).kind != BuiltinKind::NativeBinding)
        return canonical;
    BuiltinID answered;
    for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (overload.isUndeclared() || overload.resultIsDeclaredOnly() || !overload.result.type)
            return canonical;
        const BuiltinType& written = builtins.at(overload.result.type);
        if (written.isAnything() || written.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }) || !written.primitives.isEmpty() || !written.stringLiterals.empty())
            return canonical;
        for (const BuiltinTypePart& part : written.parts) {
            if (part.kind != BuiltinTypePartKind::Builtin || (answered && answered != builtins.canonicalID(part.builtin)))
                return canonical;
            answered = builtins.canonicalID(part.builtin);
        }
    }
    return answered ? answered : canonical;
}

static bool standsEarlierInTextInProgramFacts(const Witness& witness, const Witness& other)
{
    if (!hasFunction(witness) || !hasFunction(other))
        return hasFunction(witness);
    if (witness.function == other.function)
        return witness.value < other.value;
    return std::tie(witness.location.module, witness.location.position, witness.function, witness.value) < std::tie(other.location.module, other.location.position, other.function, other.value);
}

static void keepLowerWitnessInProgramFacts(Witness& lowest, const Witness& candidate)
{
    if (standsEarlierInTextInProgramFacts(candidate, lowest))
        lowest = candidate;
}

static BuiltinClassID classOfWrittenPrimitiveInProgramFacts(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Integer:
    case Opcode::Double:
        return BuiltinClasses::Number;
    case Opcode::String:
        return BuiltinClasses::String;
    case Opcode::Boolean:
        return BuiltinClasses::Boolean;
    case Opcode::BigInt:
        return BuiltinClasses::BigInt;
    default:
        return { };
    }
}

static constexpr bool admitsUnknownJavaScriptInProgramFacts(UnseenCodeCause cause)
{
    switch (cause) {
    case UnseenCodeCause::EvaluatesText:
    case UnseenCodeCause::ImportsUncompiledModule:
    case UnseenCodeCause::RunsUnseenCode:
    case UnseenCodeCause::OptionAdmitsCode:
    case UnseenCodeCause::LoadsCodeByName:
        return true;
    case UnseenCodeCause::Unknown:
    case UnseenCodeCause::LoadsModuleByName:
    case UnseenCodeCause::FlowResultDisabled:
    case UnseenCodeCause::RunsMachineCode:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr bool admitsScriptsInProgramFacts(UnseenCodeCause cause)
{
    return admitsUnknownJavaScriptInProgramFacts(cause);
}

static bool isCodeOutsideImageInProgramFacts(const Module& module)
{
    return module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode;
}

static bool isLoaderOfMachineCodeInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    return std::ranges::any_of(builtins.loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
        return builtins.canonicalID(loader.callee) == canonical || (loader.named && builtins.canonicalID(loader.named) == canonical);
    });
}

static bool standsForObjectInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    const Builtin& entry = builtins.at(canonical);
    return isObjectKind(entry.kind) || entry.memberCount;
}

static ALWAYS_INLINE bool mayHoldObjectInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    if (standsForObjectInProgramFacts(builtins, canonical))
        return true;
    std::span<const BuiltinOverload> rows = builtins.overloadsOf(canonical);
    return rows.empty() || std::ranges::any_of(rows, [&](const BuiltinOverload& row) {
        if (row.isUndeclared() || !row.result.type)
            return true;
        const BuiltinType& held = builtins.at(row.result.type);
        bool namesEveryObject = std::ranges::all_of(held.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Builtin && builtins.at(builtins.canonicalID(part.builtin)).flags.contains(BuiltinFlag::ListsAllMembers);
        });
        return held.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }) || !namesEveryObject;
    });
}

static const BuiltinCondition* optionThatRunsCodeInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    const BuiltinUnseenCode* row = builtins.unseenCodeOf(canonical);
    return row && row->condition && row->condition->hasProperty ? row->condition : nullptr;
}

static uint64_t wordOfConditionInProgramFacts(BuiltinID canonical, BuiltinName toldUnder);

static BuiltinID outermostHolderWithFeatureInProgramFacts(const BuiltinTable& builtins, BuiltinID builtin, uint32_t feature)
{
    BuiltinID outermost = builtin;
    for (BuiltinID holder = builtins.at(builtin).holder; holder; holder = builtins.at(holder).holder) {
        holder = builtins.canonicalID(holder);
        bool hasFeature = std::ranges::any_of(builtins.featureUsesOf(holder), [&](const BuiltinFeatureUse& line) {
            return static_cast<uint32_t>(line.feature) == feature;
        });
        if (hasFeature)
            outermost = holder;
    }
    return outermost;
}

static bool isCharacterOfWordInProgramFacts(uint8_t character)
{
    static constexpr std::array<bool, 256> answers = [] consteval {
        std::array<bool, 256> table { };
        for (unsigned each = 0; each < table.size(); ++each)
            table[each] = isASCIIAlphanumeric(static_cast<uint8_t>(each)) || each == '_' || each == '$';
        return table;
    }();
    return answers[character];
}

static void forEachWordInProgramFacts(std::span<const uint8_t> text, NOESCAPE const Invocable<void(std::span<const uint8_t>)> auto& functor)
{
    for (size_t start = 0; start < text.size();) {
        if (!isCharacterOfWordInProgramFacts(text[start])) {
            ++start;
            continue;
        }
        size_t end = start + 1;
        while (end < text.size() && isCharacterOfWordInProgramFacts(text[end]))
            ++end;
        functor(text.subspan(start, end - start));
        start = end;
    }
}

static unsigned placeInFilterOfWordsInProgramFacts(std::span<const uint8_t> word)
{
    return (word.size() * 131 + word.front() * 31 + word.back()) & 4095;
}

struct BuiltinSpelledInProgramFacts {
    std::span<const uint8_t> text;
    BuiltinID builtin;
    bool isMethod { false };
};

struct ModuleSpelledInProgramFacts {
    std::span<const uint8_t> specifier;
    ModuleID module;
};

static bool standsAsSpecifierInProgramFacts(std::span<const uint8_t> text, std::span<const uint8_t> lastWord, std::span<const uint8_t> specifier)
{
    size_t end = lastWord.data() + lastWord.size() - text.data();
    if (specifier.size() > end || !std::ranges::equal(text.subspan(end - specifier.size(), specifier.size()), specifier))
        return false;
    size_t start = end - specifier.size();
    if (!start && end == text.size())
        return true;
    if (!start || end == text.size() || text[start - 1] != text[end])
        return false;
    return text[end] == '"' || text[end] == '\'' || text[end] == '`';
}

struct RowsOfProgramFacts {
    RowsOfProgramFacts(const BuiltinTable&, const std::array<uint64_t, 8>& featuresRetainedByName);
    void listSpecifiersOfLibrary(const Program&);

    static constexpr uint32_t bitOfArgument(size_t argument) { return 1u << std::min<size_t>(argument, 31); }

    BitVector readLocalesWithoutDescription;
    BitVector startWorkers;
    uint32_t fileArguments { 0 };
    BuiltinID constructorOfURLs;
    BuiltinName href;
    BitVector areEvents;
    BitVector writeUnderKeysOfCalls;
    Vector<BuiltinSpelledInProgramFacts> builtinsKeptOnlyWhenNamed;
    Vector<ModuleSpelledInProgramFacts> modulesOfLibrary;
    BitVector filterOfWords;
    mutable BytesSharedByThreads stringsWithoutWordOfFilter;
    Vector<const BuiltinCondition*, 8> conditionsOnLightForms;
    BitVector haveLightForms;

    const BuiltinCondition* lightFormOf(const BuiltinTable&, BuiltinID canonical) const;
};

const BuiltinCondition* RowsOfProgramFacts::lightFormOf(const BuiltinTable& builtins, BuiltinID canonical) const
{
    if (!haveLightForms.quickGet(canonical.index()))
        return nullptr;
    return *std::ranges::find_if(conditionsOnLightForms, [&](const BuiltinCondition* condition) {
        return builtins.canonicalID(condition->isAskedForOtherNames) == canonical;
    });
}

static bool writesUnderKeyOfCallInProgramFacts(const BuiltinPropertyAction& action)
{
    switch (action.kind) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
    case BuiltinPropertyActionKind::DefinesFrom:
        return action.key.kind != BuiltinKeyKind::Names && action.key.kind != BuiltinKeyKind::Indexes;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool readsDataOfLocalesInProgramFacts(RemovableFeature feature)
{
    using enum RemovableFeature;
    return feature == IntlCollation || feature == IntlNumberFormat || feature == IntlDateTimeFormat || feature == IntlUnits || feature == IntlSegmenter || feature == IntlDisplayNames || feature == IntlOther;
}

RowsOfProgramFacts::RowsOfProgramFacts(const BuiltinTable& builtins, const std::array<uint64_t, 8>& featuresRetainedByName)
    : readLocalesWithoutDescription(builtins.builtinCount())
    , startWorkers(builtins.builtinCount())
    , areEvents(builtins.builtinCount())
    , writeUnderKeysOfCalls(builtins.builtinCount())
    , conditionsOnLightForms(builtins.conditionsOnLightForms())
    , haveLightForms(builtins.builtinCount())
{
    for (const BuiltinCondition* condition : conditionsOnLightForms)
        haveLightForms.quickSet(builtins.canonicalID(condition->isAskedForOtherNames).index());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID entry(index);
        BuiltinID row = builtins.canonicalID(entry);
        if (row == entry && callOrReadOfBuiltinIsEvent(builtins, row))
            areEvents.quickSet(index);

        auto noteParameter = [&](const BuiltinParameter& parameter) {
            if (!parameter.roles.contains(BuiltinOperandRole::WorkerEntryPath))
                return;
            startWorkers.quickSet(row.index());
            switch (parameter.operand.kind()) {
            case BuiltinOperand::Kind::Receiver:
                return;
            case BuiltinOperand::Kind::Argument:
                fileArguments |= bitOfArgument(parameter.operand.argumentIndex());
                return;
            case BuiltinOperand::Kind::ArgumentsFrom:
                fileArguments |= ~(bitOfArgument(parameter.operand.argumentIndex()) - 1);
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        };
        for (const BuiltinOverload& overload : builtins.overloadsOf(entry)) {
            if (overload.isUndeclared() && std::ranges::any_of(builtins.featureUsesOf(row), readsDataOfLocalesInProgramFacts, &BuiltinFeatureUse::feature))
                readLocalesWithoutDescription.quickSet(row.index());
            if (std::ranges::any_of(overload.flow.propertyActions, writesUnderKeyOfCallInProgramFacts) || std::ranges::contains(overload.flow.propertyActions, BuiltinPropertyActionKind::ChangesPrototype, &BuiltinPropertyAction::kind))
                writeUnderKeysOfCalls.quickSet(row.index());
            if (const BuiltinParameter* receiver = overload.receiver.get())
                noteParameter(*receiver);
            for (const BuiltinParameter& parameter : overload.parameters)
                noteParameter(parameter);
        }
    }

    BuiltinClassID classOfURLs = BuiltinClasses::URL;
    if (BuiltinID constructor = classOfURLs ? builtins.at(classOfURLs).constructor : BuiltinID())
        constructorOfURLs = builtins.canonicalID(constructor);
    href = builtins.nameOf(byteCast<uint8_t>("href"_s.span()));

    BitVector standForFeature(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index))) {
            auto feature = static_cast<uint32_t>(line.feature);
            if (featuresRetainedByName[feature / 64] >> feature % 64 & 1)
                standForFeature.quickSet(outermostHolderWithFeatureInProgramFacts(builtins, builtins.canonicalID(BuiltinID(index)), feature).index());
        }
    }
    filterOfWords.ensureSize(4096);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        BuiltinID canonical = builtins.canonicalID(BuiltinID(index));
        if (!standForFeature.quickGet(canonical.index()) || !entry.name || entry.kind == BuiltinKind::Prototype || builtins.isSymbolName(entry.name))
            continue;
        bool hangsOnPrototype = entry.holder && builtins.at(entry.holder).kind == BuiltinKind::Prototype;
        std::span<const uint8_t> text = builtins.textOf(entry.name);
        if (text.empty() || (entry.kind == BuiltinKind::Constructor && hangsOnPrototype))
            continue;
        builtinsKeptOnlyWhenNamed.append(BuiltinSpelledInProgramFacts { text, canonical, hangsOnPrototype });
        filterOfWords.quickSet(placeInFilterOfWordsInProgramFacts(text));
    }
}


void RowsOfProgramFacts::listSpecifiersOfLibrary(const Program& program)
{
    static constexpr auto prefixOfNode = "node:"_s;
    auto list = [&](std::span<const uint8_t> specifier, ModuleID module) {
        std::span<const uint8_t> lastWord;
        forEachWordInProgramFacts(specifier, [&](std::span<const uint8_t> word) {
            lastWord = word;
        });
        if (lastWord.empty() || lastWord.data() + lastWord.size() != specifier.data() + specifier.size())
            return;
        modulesOfLibrary.append(ModuleSpelledInProgramFacts { specifier, module });
        filterOfWords.quickSet(placeInFilterOfWordsInProgramFacts(lastWord));
    };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary))
            continue;
        std::span<const uint8_t> path = program.string(module.path);
        list(path, ModuleID(index));
        if (path.size() > prefixOfNode.length() && std::ranges::equal(path.first(prefixOfNode.length()), byteCast<uint8_t>(prefixOfNode.span())))
            list(path.subspan(prefixOfNode.length()), ModuleID(index));
        for (StringID other : module.otherNames)
            list(program.string(other), ModuleID(index));
    }
}

struct FindingOfProgramFacts {
    enum class Kind : uint8_t { UseOfBuiltin, ReadOfAnyMember, WriteOnBuiltin, PrototypeChangeOfBuiltin, NameReadOffUnknownValue, ReadThatNoRowAnswers, PartOfClassIsUsed, KeysOfBuiltinListed, OptionThatRunsCode, InstanceThatMayAnswerEveryName, UnseenCode, WorkerEntry, WorkerStartWithoutName, WorkerStartFromURL, NamedModule, LoaderHandedToText, CallOfEvent, BuiltinSpelledInText, LightFormIsAskedForMore, CallUnderComputedKey, StandsAboveClass, NamespaceIsGivenByImportCall };

    Kind kind { Kind::UseOfBuiltin };
    uint8_t detail { 0 };
    bool isOfProgram : 1 { false };
    bool isCalledOutOfSightOfLink : 1 { false };
    bool isCalledWithoutRow : 1 { false };
    bool waitsForLink : 1 { false };
    bool isCalledWhereLinkTakesAnotherCallee : 1 { false };
    uint32_t subject { Wire::none };
    uint32_t key { Wire::none };
    Witness witness { noWitness };
};
static_assert(sizeof(FindingOfProgramFacts) == 3 * sizeof(uint32_t) + sizeof(Witness));

struct VariableStoredInFieldInProgramFacts {
    LayoutID declarer;
    PropertyKeyID key;
    VariableID ofModule;
    FieldReference ofEnvironment;
};

struct FieldUsedOutsideLoopsInProgramFacts {
    FunctionID function;
    LayoutID declarer;
    PropertyKeyID key;
};

class NamesOfNumbersInProgramFacts {
    WTF_MAKE_NONCOPYABLE(NamesOfNumbersInProgramFacts);
public:
    explicit NamesOfNumbersInProgramFacts(size_t numberOfKeys)
        : m_storedWhatIsNoInt32(numberOfKeys)
    {
    }

    void merge(NamesOfNumbersInProgramFacts& other)
    {
        m_storedWhatIsNoInt32.merge(other.m_storedWhatIsNoInt32);
        m_fieldsReadAsIntegers.merge(other.m_fieldsReadAsIntegers);
        m_namesReadAsIntegers.merge(other.m_namesReadAsIntegers);
        m_fieldsReadForArithmetic.merge(other.m_fieldsReadForArithmetic);
        m_fieldsStoredWhatIsNoInt32.merge(other.m_fieldsStoredWhatIsNoInt32);
        m_fieldsUsedInProducts.merge(other.m_fieldsUsedInProducts);
        m_fieldsUsedOutsideLoops.appendVector(other.m_fieldsUsedOutsideLoops);
        m_iterationCallbacks.appendVector(other.m_iterationCallbacks);
        m_variablesStoredInFields.appendVector(other.m_variablesStoredInFields);
    }

    void noteReadAsInteger(LayoutID declarer, PropertyKeyID key) { m_fieldsReadAsIntegers.add(declarer, key); }
    void noteNameReadAsInteger(ModuleID module, PropertyKeyID key) { m_namesReadAsIntegers.add(module, key); }
    void noteReadForArithmetic(LayoutID declarer, PropertyKeyID key) { m_fieldsReadForArithmetic.add(declarer, key); }
    void noteStoredWhatIsNoInt32(LayoutID declarer, PropertyKeyID key) { m_fieldsStoredWhatIsNoInt32.add(declarer, key); }
    void noteUsedInProduct(LayoutID declarer, PropertyKeyID key) { m_fieldsUsedInProducts.add(declarer, key); }
    void noteUsedInProductOutsideLoops(FieldUsedOutsideLoopsInProgramFacts use) { m_fieldsUsedOutsideLoops.append(use); }
    void noteIterationCallback(FunctionID callback) { m_iterationCallbacks.append(callback); }
    void noteStoredWhatIsNoInt32(PropertyKeyID key) { m_storedWhatIsNoInt32.set(key.index()); }
    void noteStoredVariable(VariableStoredInFieldInProgramFacts store) { m_variablesStoredInFields.append(store); }
    Vector<VariableStoredInFieldInProgramFacts> takeVariablesStoredInFields() { return WTF::move(m_variablesStoredInFields); }
    FieldsOfNumbers takeFieldsReadAsIntegers() { return WTF::move(m_fieldsReadAsIntegers); }
    NamesOfModules takeNamesReadAsIntegers() { return WTF::move(m_namesReadAsIntegers); }
    FieldsOfNumbers takeFieldsReadForArithmetic() { return WTF::move(m_fieldsReadForArithmetic); }
    FieldsOfNumbers takeFieldsStoredWhatIsNoInt32() { return WTF::move(m_fieldsStoredWhatIsNoInt32); }
    FieldsOfNumbers takeFieldsUsedInProducts() { return WTF::move(m_fieldsUsedInProducts); }
    Vector<FieldUsedOutsideLoopsInProgramFacts> takeFieldsUsedOutsideLoops() { return WTF::move(m_fieldsUsedOutsideLoops); }
    Vector<FunctionID> takeIterationCallbacks() { return WTF::move(m_iterationCallbacks); }
    BitVector takeKeysStoredWhatIsNoInt32() { return WTF::move(m_storedWhatIsNoInt32); }

private:
    BitVector m_storedWhatIsNoInt32;
    FieldsOfNumbers m_fieldsReadAsIntegers;
    NamesOfModules m_namesReadAsIntegers;
    FieldsOfNumbers m_fieldsReadForArithmetic;
    FieldsOfNumbers m_fieldsStoredWhatIsNoInt32;
    FieldsOfNumbers m_fieldsUsedInProducts;
    Vector<FieldUsedOutsideLoopsInProgramFacts> m_fieldsUsedOutsideLoops;
    Vector<FunctionID> m_iterationCallbacks;
    Vector<VariableStoredInFieldInProgramFacts> m_variablesStoredInFields;
};

static constexpr OpcodeSet storesUnderConstantKeysInProgramFacts { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesUnderComputedKeysInProgramFacts { Opcode::ElemGet, Opcode::ElemSet };
static constexpr OpcodeSet readsOfVariablesInProgramFacts { Opcode::CheckInitialized, Opcode::GetModuleVariable, Opcode::GetEnvironmentField };
static constexpr OpcodeSet operatorsOnNumbersAloneInProgramFacts = operatorsOfKinds({ OperatorKind::Multiplicative }) | allRelations | OpcodeSet { Opcode::Sub, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet operationsThatTellOfNumbersInProgramFacts = allBitwiseOperators | operatorsOnNumbersAloneInProgramFacts | storesUnderConstantKeysInProgramFacts | accessesUnderComputedKeysInProgramFacts | OpcodeSet { Opcode::Call, Opcode::NewObject };
struct StepOfLookInProgramFacts {
    static constexpr uint16_t isText = 1u << 0;
    static constexpr uint16_t isData = 1u << 1;
    static constexpr uint16_t definesUnderName = 1u << 2;
    static constexpr uint16_t storesUnderName = 1u << 3;
    static constexpr uint16_t makesObject = 1u << 4;
    static constexpr uint16_t tellsOfNumbers = 1u << 5;
    static constexpr uint16_t readsUnderName = 1u << 6;
    static constexpr uint16_t callsByValue = 1u << 7;
    static constexpr uint16_t loadsOrEvaluates = 1u << 8;
    static constexpr uint16_t readsModuleVariable = 1u << 9;
    static constexpr uint16_t takenInLibraryToo = loadsOrEvaluates | readsModuleVariable;
};
static constexpr std::array<uint16_t, numberOfOpcodes> stepsOfLookByOpcodeInProgramFacts = []() consteval {
    std::array<uint16_t, numberOfOpcodes> steps { };
    auto take = [&](OpcodeSet operations, uint16_t step) {
        operations.forEach([&](Opcode opcode) {
            steps[static_cast<unsigned>(opcode)] |= step;
        });
    };
    take({ Opcode::String }, StepOfLookInProgramFacts::isText);
    take({ Opcode::Data }, StepOfLookInProgramFacts::isData);
    take({ Opcode::PropDefine }, StepOfLookInProgramFacts::definesUnderName);
    take({ Opcode::PropSet }, StepOfLookInProgramFacts::storesUnderName);
    take({ Opcode::NewObject }, StepOfLookInProgramFacts::makesObject);
    take(operationsThatTellOfNumbersInProgramFacts, StepOfLookInProgramFacts::tellsOfNumbers);
    take({ Opcode::PropGet }, StepOfLookInProgramFacts::readsUnderName);
    take(callsByValueInProgramFacts, StepOfLookInProgramFacts::callsByValue);
    take(loadsAndEvaluationsInProgramFacts, StepOfLookInProgramFacts::loadsOrEvaluates);
    take({ Opcode::GetModuleVariable }, StepOfLookInProgramFacts::readsModuleVariable);
    return steps;
}();
static constexpr OperandUses usesThatOnlyWriteOnOperandInProgramFacts {
    OperandUse::Inspects, OperandUse::Returns, OperandUse::WritesContents, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
};
static constexpr uint8_t writeIsNamedByRowInProgramFacts = 1;
static constexpr uint8_t writeOnlyDeletesInProgramFacts = 2;

struct BuiltinsHeldByVariablesInProgramFacts {
    Vector<BuiltinID> held;
    BitVector areFollowedByLinkAsTheObject;
};

class BodyLookOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(BodyLookOfProgramFacts);
public:
    BodyLookOfProgramFacts(const Program& program, const RowsOfProgramFacts& rows, RefusedCode refused, FunctionID id, const BodyIndex& index, const ThinSummaries& summaries, Vector<FindingOfProgramFacts>& found, NamesOfNumbersInProgramFacts& namesOfNumbers, const BuiltinsHeldByVariablesInProgramFacts& builtinsHeldByVariables)
        : m_namesOfNumbers(namesOfNumbers)
        , m_program(program)
        , m_builtins(program.builtins())
        , m_rows(rows)
        , m_refused(refused)
        , m_id(id)
        , m_function(program.at(id))
        , m_isOfProgram(!isLibraryCode(program, m_function))
        , m_index(index)
        , m_summaries(summaries)
        , m_summary(summaries.at(id))
        , m_blocksNeverEntered(m_summary.blocksNeverEntered.span())
        , m_found(found)
        , m_builtinsHeldByVariables(builtinsHeldByVariables.held.span())
        , m_variablesThatLinkFollowsAsTheObject(builtinsHeldByVariables.areFollowedByLinkAsTheObject)
    {
    }

    void run();

private:
    using Finding = FindingOfProgramFacts;
    enum class Fate : uint8_t { IsInspected, IsWrittenOn, IsLeftToLink, IsFollowedByLink, IsCalled, IsConstructed, StandsAboveClass, IsAskedForAnyMember, IsHeldByVariable, IsLetGo };

    Witness witnessAt(const BodyValue& value) const { return { m_id.index(), value.id().index(), m_function.locationOf(value) }; }
    Witness witnessAt(const BodyTerminator& terminator) const { return { m_id.index(), Wire::none, m_function.locationOf(terminator) }; }

    BuiltinID builtinAt(ValueID value) const { return value ? m_builtinOfValue[value.index()] : BuiltinID(); }
    BuiltinID builtinSurelyAt(ValueID value) const { return value && !m_mayBeSomethingElse.get(value.index()) ? m_builtinOfValue[value.index()] : BuiltinID(); }
    BuiltinID builtinHeldBy(VariableID variable) const { return variable.index() < m_builtinsHeldByVariables.size() ? m_builtinsHeldByVariables[variable.index()] : BuiltinID(); }
    ValueID originOfURL(ValueID value) const { return value && !m_originOfURLOfValue.isEmpty() ? m_originOfURLOfValue[value.index()] : ValueID(); }
    bool isURLOrURLString(ValueID) const;
    BuiltinID holderOfMethodServed(ValueID) const;
    BuiltinID classOfPartAt(ValueID value) const { return value && !m_classOfPartOfValue.isEmpty() ? m_classOfPartOfValue[value.index()] : BuiltinID(); }
    BuiltinID builtinThatLinkLedToAt(ValueID value) const { return value && !m_builtinThatLinkLedToOfValue.isEmpty() ? m_builtinThatLinkLedToOfValue[value.index()] : BuiltinID(); }
    void followAsComeByThroughLink(ValueID, BuiltinID canonicalAbove);
    const BuiltinCondition* lightFormAt(ValueID value) const { return builtinAt(value) ? m_rows.lightFormOf(m_builtins, m_builtins.canonicalID(builtinAt(value))) : nullptr; }
    ALWAYS_INLINE bool isFollowed(ValueID value) const { return builtinAt(value) || originOfURL(value) || holderOfMethodServed(value) || classOfPartAt(value); }
    BodyValue stripCasts(ValueID value) const { return m_index.value(m_index.stripCasts(value)); }

    void note(Finding::Kind, uint8_t detail, uint32_t subject, uint32_t key, const Witness&);
    void note(Finding::Kind kind, uint8_t detail, uint32_t subject, const Witness& witness) { note(kind, detail, subject, Wire::none, witness); }
    void noteWrite(BuiltinID holder, PropertyKeyID key, const BodyValue& place, uint8_t detail = 0) { note(Finding::Kind::WriteOnBuiltin, detail, holder.index(), key ? key.index() : Wire::none, witnessAt(place)); }
    bool linkSeesCallAt(const Witness&, BuiltinID canonical);
    bool linkHasCallAt(const Witness&) const;
    bool usesFeaturesUnderConditionsOnOperands(BuiltinID canonical) const;
    bool linkTakesCalleeAt(const Witness&, BuiltinID canonical) const;
    void noteUse(BuiltinID builtin, BuiltinUse use, const auto& place) { note(Finding::Kind::UseOfBuiltin, static_cast<uint8_t>(OptionSet<BuiltinUse>(use).toRaw()), builtin.index(), witnessAt(place)); }
    void noteFate(ValueID, Fate, const auto& place);
    void handOn(ValueID from, ValueID to);
    bool passOn(ValueID argument, ValueID parameter, const BodyBlock& from);

    ALWAYS_INLINE uint32_t stepsOf(Opcode opcode) const { return stepsOfLookByOpcodeInProgramFacts[static_cast<unsigned>(opcode)] & m_stepsTaken; }
    void look(const BodyValue&);
    void noteBuiltinsSpelledIn(StringID, bool standsInData, const BodyValue&);
    void noteBuiltinsSpelledIn(const DataNode&, const BodyValue&);
    void noteModuleSpelledBy(PropertyKeyID, const BodyValue&);
    TypeID writtenTypeOf(ValueID) const;
    PropertyKeyID keyOfRead(ValueID) const;
    bool computesFraction(const BodyValue&) const;
    void noteUsedInProduct(const BodyValue& operation, LayoutID declarer, PropertyKeyID);
    void noteReadsInProduct(const BodyValue&);
    TypeID declaredTypeOf(ValueID) const;
    TypeID declaredTypeOfField(TypeID receiver, PropertyKeyID) const;
    TypeID declaredTypeOfElement(TypeID list) const;
    void forEachDeclarerOfFieldOf(ValueID receiver, PropertyKeyID, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    bool isTruncation(const BodyValue&) const;
    void noteReadUsedAsInteger(ValueID, bool isTruncatedAlone = false);
    void noteReadUsedForArithmetic(ValueID);
    void noteWhatIsStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID, ValueID stored);
    TypeID writtenTypeWhereValueIsHandedOn(ValueID) const;
    void noteVariableStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID, ValueID stored);
    void noteWhatIsToldOfNumbers(const BodyValue&);
    void nameBuiltin(const BodyValue&, BuiltinID);
    void lookAtLoad(const BodyValue&);
    void noteObjectOfModule(RuntimeModuleID, const BodyValue& load);
    void accessMember(const BodyValue&, BuiltinID holder);
    PropertyKeyID provenSymbolOf(ValueID) const;
    PropertyKeyID symbolKeyOfAccess(const BodyValue&) const;
    bool followPartOfClass(const BodyValue&, BuiltinID holder, BuiltinID member);
    void followAsPartOfClass(ValueID, BuiltinID objectOfClass);
    void accessMemberOfPartOfClass(const BodyValue&, BuiltinID objectOfClass);
    void followWhatReadAnswers(const BodyValue& read, BuiltinID member);
    void letGoObjectsNamedIn(BuiltinTypeID, const BodyValue& read);
    void lookAtCall(const BodyValue&);
    void noteLoaderHandedToText(const BodyValue&);
    void noteStartOfWorker(const BodyValue& call, bool spellsOutArguments);
    void noteConstructionWhoseOperandsNobodySees(BuiltinID row, const Witness&);
    bool listKeysOfLiteral(ValueID, Vector<PropertyKeyID, 4>&) const;
    bool mayPass(const BodyValue& call, const BuiltinCondition& option, bool spellsOutArguments) const;
    bool operandIsDataWithoutMember(const BodyValue& construction, const BuiltinCondition& member, bool spellsOutArguments) const;
    std::optional<FunctionID> functionUnderMemberOfLiteralThatNobodyElseHolds(const BodyValue& construction, const BuiltinCondition& member, bool spellsOutArguments) const;
    void noteWritesOfRow(const BodyValue& call, BuiltinID row, BuiltinCallMode, bool spellsOutArguments);
    void lookAtEnd(const BodyBlock&);

    BuiltinID memberFoundOn(BuiltinID holder, PropertyKeyID) const;
    bool tableRulesOutMemberFoundFrom(BuiltinID holder, PropertyKeyID) const;
    bool readGivesNothingThatNobodyDescribes(BuiltinID holder, PropertyKeyID) const;
    bool mayGoThroughOwnKeysOf(const BodyValue& user, uint32_t operandIndex) const;
    bool rowTakesEveryCallOn(const BuiltinOverload&, BuiltinID receiver) const;
    bool rowOnlyInspects(BuiltinID callee, BuiltinCallMode, BuiltinOperand, BuiltinID receiver, OperandUses usesBeside = { }) const;
    bool rowsOnlyBindAsReceiver(BuiltinID callee, BuiltinOperand) const;
    bool linkFollowsWritesOn(ValueID written, ValueID site) const;
    bool linkHoldsAsTheObject(ValueID) const;
    bool rowsOnlyConstructReceiver(BuiltinID callee, BuiltinID receiver) const;
    bool rowsOnlyCallReceiver(BuiltinID callee, BuiltinID receiver) const;
    Fate fateOf(const BodyValue& user, uint32_t operandIndex) const;

    NamesOfNumbersInProgramFacts& m_namesOfNumbers;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const RowsOfProgramFacts& m_rows;
    const RefusedCode m_refused;
    const FunctionID m_id;
    const Function& m_function;
    const bool m_isOfProgram;
    const uint16_t m_stepsTaken { static_cast<uint16_t>(m_isOfProgram ? UINT16_MAX : StepOfLookInProgramFacts::takenInLibraryToo) };
    bool m_readsOffNodeVm { false };
    const BodyIndex& m_index;
    const ThinSummaries& m_summaries;
    const ThinSummary& m_summary;
    std::span<const BlockID> m_blocksNeverEntered;
    Vector<Finding>& m_found;
    std::span<const BuiltinID> m_builtinsHeldByVariables;
    const BitVector& m_variablesThatLinkFollowsAsTheObject;
    Vector<uint64_t> m_callsBoundToBuiltins;
    Vector<uint32_t> m_callSites;
    uint32_t m_numberOfQuestionsAboutCalls { 0 };
    static constexpr uint32_t mostQuestionsAnsweredByGoingThroughCalls = 8;
    Vector<BuiltinID> m_builtinOfValue;
    BitVector m_mayBeSomethingElse;
    Vector<PropertyKeyID, 8> m_namesReadOffUnknownValues;
    Vector<BuiltinID, 4> m_builtinsSpelledInTexts;
    Vector<ValueID> m_originOfURLOfValue;
    Vector<BuiltinID> m_classOfPartOfValue;
    Vector<BuiltinID> m_builtinThatLinkLedToOfValue;
    struct MethodServed {
        ValueID value;
        BuiltinID holder;
    };
    Vector<MethodServed, 2> m_methodsServed;
};

void BodyLookOfProgramFacts::note(Finding::Kind kind, uint8_t detail, uint32_t subject, uint32_t key, const Witness& witness)
{
    bool isUse = kind == Finding::Kind::UseOfBuiltin || kind == Finding::Kind::ReadOfAnyMember;
    bool isCalledOutOfSightOfLink = isUse && OptionSet<BuiltinUse>::fromRaw(detail).containsAny(callingUsesInProgramFacts) && !linkSeesCallAt(witness, m_builtins.canonicalID(BuiltinID(subject)));
    bool isCalledWithoutRow = isCalledOutOfSightOfLink && !linkHasCallAt(witness);
    bool isCalledWhereLinkTakesAnotherCallee = isCalledOutOfSightOfLink && usesFeaturesUnderConditionsOnOperands(m_builtins.canonicalID(BuiltinID(subject))) && !linkTakesCalleeAt(witness, m_builtins.canonicalID(BuiltinID(subject)));
    m_found.append(Finding { kind, detail, m_isOfProgram, isCalledOutOfSightOfLink, isCalledWithoutRow, false, isCalledWhereLinkTakesAnotherCallee, subject, key, witness });
}

static BuiltinID calleeThatLinkTakesInProgramFacts(const SummaryCall& call)
{
    return call.builtin ? call.builtin : call.expectsCalleeItself ? call.expectedBuiltin : BuiltinID();
}

bool BodyLookOfProgramFacts::linkSeesCallAt(const Witness& witness, BuiltinID canonical)
{
    RELEASE_ASSERT(witness.function == m_id.index());
    const auto& calls = m_summary.calls;
    if (m_numberOfQuestionsAboutCalls < mostQuestionsAnsweredByGoingThroughCalls) {
        ++m_numberOfQuestionsAboutCalls;
        return std::ranges::any_of(calls, [&](const SummaryCall& call) {
            return call.site.index() == witness.value && call.callee == SummaryCall::Callee::Builtin && m_builtins.canonicalID(call.builtin) == canonical;
        });
    }
    if (m_numberOfQuestionsAboutCalls == mostQuestionsAnsweredByGoingThroughCalls) {
        ++m_numberOfQuestionsAboutCalls;
        for (const SummaryCall& call : calls) {
            m_callSites.append(call.site.index());
            if (call.callee == SummaryCall::Callee::Builtin)
                m_callsBoundToBuiltins.append(static_cast<uint64_t>(call.site.index()) << 32 | m_builtins.canonicalID(call.builtin).index());
        }
        std::ranges::sort(m_callsBoundToBuiltins);
        std::ranges::sort(m_callSites);
    }
    return std::ranges::binary_search(m_callsBoundToBuiltins, static_cast<uint64_t>(witness.value) << 32 | canonical.index());
}

bool BodyLookOfProgramFacts::linkHasCallAt(const Witness& witness) const
{
    RELEASE_ASSERT(witness.function == m_id.index());
    if (m_numberOfQuestionsAboutCalls > mostQuestionsAnsweredByGoingThroughCalls)
        return std::ranges::binary_search(m_callSites, witness.value);
    return std::ranges::contains(m_summary.calls, witness.value, [](const SummaryCall& call) {
        return call.site.index();
    });
}

bool BodyLookOfProgramFacts::usesFeaturesUnderConditionsOnOperands(BuiltinID canonical) const
{
    return std::ranges::any_of(m_builtins.featureUsesOf(canonical), [](const BuiltinFeatureUse& line) {
        return line.condition && line.condition->nameToldUnder();
    });
}

bool BodyLookOfProgramFacts::linkTakesCalleeAt(const Witness& witness, BuiltinID canonical) const
{
    return std::ranges::any_of(m_summary.calls, [&](const SummaryCall& call) {
        BuiltinID callee = calleeThatLinkTakesInProgramFacts(call);
        return call.site.index() == witness.value && callee && m_builtins.canonicalID(callee) == canonical;
    });
}

BuiltinID BodyLookOfProgramFacts::holderOfMethodServed(ValueID value) const
{
    for (const MethodServed& served : m_methodsServed) {
        if (served.value == value)
            return served.holder;
    }
    return { };
}

void BodyLookOfProgramFacts::noteFate(ValueID value, Fate fate, const auto& place)
{
    if (BuiltinID holder = holderOfMethodServed(value); holder && fate != Fate::IsInspected)
        note(Finding::Kind::LightFormIsAskedForMore, 0, holder.index(), witnessAt(place));
    if (fate != Fate::IsInspected && lightFormAt(value))
        note(Finding::Kind::LightFormIsAskedForMore, 0, builtinAt(value).index(), witnessAt(place));
    if (BuiltinID builtin = builtinAt(value)) {
        switch (fate) {
        case Fate::IsInspected:
        case Fate::IsWrittenOn:
        case Fate::IsLeftToLink:
            noteUse(builtin, BuiltinUse::Tested, place);
            break;
        case Fate::IsCalled:
            noteUse(builtin, BuiltinUse::Called, place);
            break;
        case Fate::IsConstructed:
            noteUse(builtin, BuiltinUse::Constructed, place);
            break;
        case Fate::StandsAboveClass:
            noteUse(builtin, BuiltinUse::Constructed, place);
            note(Finding::Kind::StandsAboveClass, 0, builtin.index(), witnessAt(place));
            noteConstructionWhoseOperandsNobodySees(m_builtins.canonicalID(builtin), witnessAt(place));
            break;
        case Fate::IsAskedForAnyMember:
            note(Finding::Kind::ReadOfAnyMember, static_cast<uint8_t>(OptionSet<BuiltinUse>(BuiltinUse::Escaped).toRaw()), builtin.index(), witnessAt(place));
            break;
        case Fate::IsHeldByVariable:
            break;
        case Fate::IsFollowedByLink:
            noteUse(builtin, BuiltinUse::Escaped, place);
            m_found.last().waitsForLink = true;
            break;
        case Fate::IsLetGo:
            noteUse(builtin, BuiltinUse::Escaped, place);
            break;
        }
    }
    if (BuiltinID objectOfClass = classOfPartAt(value); objectOfClass && fate != Fate::IsInspected)
        note(Finding::Kind::PartOfClassIsUsed, 0, objectOfClass.index(), witnessAt(place));
    if (BuiltinID above = builtinThatLinkLedToAt(value); above && fate != Fate::IsInspected)
        noteUse(above, BuiltinUse::Escaped, place);
}

void BodyLookOfProgramFacts::handOn(ValueID from, ValueID to)
{
    if (BuiltinID builtin = builtinAt(from)) {
        m_builtinOfValue[to.index()] = builtin;
        if (m_mayBeSomethingElse.get(from.index()))
            m_mayBeSomethingElse.set(to.index());
    }
    if (BuiltinID objectOfClass = classOfPartAt(from))
        followAsPartOfClass(to, objectOfClass);
    if (BuiltinID above = builtinThatLinkLedToAt(from))
        followAsComeByThroughLink(to, above);
    if (ValueID origin = originOfURL(from))
        m_originOfURLOfValue[to.index()] = origin;
    if (BuiltinID holder = holderOfMethodServed(from))
        m_methodsServed.append(MethodServed { to, holder });
}

bool BodyLookOfProgramFacts::passOn(ValueID argument, ValueID parameter, const BodyBlock& from)
{
    BuiltinID builtin = builtinAt(argument);
    if (!builtin || originOfURL(argument) || parameter.index() < from.endOfValues().index())
        return false;
    BuiltinID held = builtinAt(parameter);
    if (held && m_builtins.canonicalID(held) != m_builtins.canonicalID(builtin))
        return false;
    m_builtinOfValue[parameter.index()] = builtin;
    m_mayBeSomethingElse.set(parameter.index());
    return true;
}

void BodyLookOfProgramFacts::noteObjectOfModule(RuntimeModuleID module, const BodyValue& load)
{
    if (BuiltinID object = module ? m_builtins.namespaceOf(module) : BuiltinID())
        noteUse(object, BuiltinUse::Escaped, load);
}

void BodyLookOfProgramFacts::nameBuiltin(const BodyValue& value, BuiltinID builtin)
{
    if (!builtin)
        return;
    m_builtinOfValue[value.id().index()] = builtin;
    noteUse(builtin, BuiltinUse::Read, value);
}

void BodyLookOfProgramFacts::lookAtLoad(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::DirectEval) {
        if (!m_refused.evaluatedText)
            note(Finding::Kind::UnseenCode, static_cast<uint8_t>(UnseenCodeCause::EvaluatesText), Wire::none, witnessAt(value));
        return;
    }
    if (opcode == Opcode::ResolveDynamic) {
        if (auto resolve = value.as<Opcode::ResolveDynamic>(); !resolve.isURL() && !resolve.isForRequire())
            noteUse(Builtins::ModuleRequireResolve, BuiltinUse::Called, value);
        return;
    }
    if (loadsByNameInProgramFacts.contains(opcode)) {
        if (m_function.role == FunctionRole::Source) {
            note(Finding::Kind::UnseenCode, static_cast<uint8_t>(m_refused.codeOutsideImage ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName), Wire::none, witnessAt(value));
            note(Finding::Kind::NamespaceIsGivenByImportCall, 0, Wire::none, witnessAt(value));
        }
        return;
    }
    ModuleID module;
    if (opcode == Opcode::ModuleNamespace)
        module = value.as<Opcode::ModuleNamespace>().module();
    else if (opcode == Opcode::ImportModule)
        module = value.as<Opcode::ImportModule>().module();
    else if (opcode == Opcode::RequireModule)
        module = value.as<Opcode::RequireModule>().module();
    else
        module = value.as<Opcode::EvaluateModule>().module();
    if (!module)
        return;
    note(Finding::Kind::NamedModule, 0, module.index(), witnessAt(value));
    bool isNamespaceThatLinkFollows = opcode == Opcode::ModuleNamespace && value.as<Opcode::ModuleNamespace>().phase() != ImportPhase::Defer && (m_program.layoutOfWholeNamespace(module) || m_program.layoutOfNamespaceThatListsNoName(module));
    bool mayGiveNamespace = opcode == Opcode::ImportModule || (opcode == Opcode::ModuleNamespace && !isNamespaceThatLinkFollows) || (opcode == Opcode::RequireModule && m_program.at(module).kind == ModuleKind::SourceText);
    if (mayGiveNamespace && (opcode == Opcode::ImportModule || m_function.role == FunctionRole::Source))
        note(Finding::Kind::NamespaceIsGivenByImportCall, 0, module.index(), witnessAt(value));
    if (opcode == Opcode::RequireModule)
        nameBuiltin(value, m_program.at(module).runtimeModule ? m_builtins.namespaceOf(m_program.at(module).runtimeModule) : BuiltinID());
    else if (opcode != Opcode::EvaluateModule)
        noteObjectOfModule(m_program.at(module).runtimeModule, value);
}

static BuiltinID memberFoundOnInProgramFacts(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID canonical = objectAnsweredByBindingInProgramFacts(builtins, builtins.canonicalID(holder));
    if (BuiltinID own = findOwnMember(program, canonical, key))
        return own;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    if (!name)
        return { };
    BuiltinMemberLookup lookup = builtins.findMember(canonical, name.name);
    return lookup.outcome == BuiltinMemberLookup::Outcome::Found ? lookup.member : BuiltinID();
}

BuiltinID BodyLookOfProgramFacts::memberFoundOn(BuiltinID holder, PropertyKeyID key) const
{
    return memberFoundOnInProgramFacts(m_program, holder, key);
}

bool BodyLookOfProgramFacts::tableRulesOutMemberFoundFrom(BuiltinID holder, PropertyKeyID key) const
{
    BuiltinID current = m_builtins.canonicalID(holder);
    while (listsEveryMemberUnder(m_program, current, key)) {
        const Builtin& object = m_builtins.at(current);
        if (object.prototypeLink != BuiltinPrototypeLink::Builtin)
            return object.prototypeLink == BuiltinPrototypeLink::Null;
        current = m_builtins.canonicalID(object.inheritsFrom);
    }
    return false;
}

bool BodyLookOfProgramFacts::readGivesNothingThatNobodyDescribes(BuiltinID holder, PropertyKeyID key) const
{
    BuiltinID canonical = m_builtins.canonicalID(holder);
    if (compiledModuleWithObject(m_program, canonical))
        return true;
    if (m_program.at(key).kind != PropertyKeyKind::EnginePrivateName || canonical != m_builtins.canonicalID(Builtins::GlobalThis))
        return false;
    BuiltinMemberName name = builtinMemberNameOf(m_program, key);
    BuiltinID member = name ? m_builtins.findOwnMember(Builtins::PrivateGlobals, name.name) : BuiltinID();
    return member && std::ranges::all_of(m_builtins.overloadsOf(m_builtins.canonicalID(member)), [&](const BuiltinOverload& row) {
        if (row.isUndeclared()) {
            const RuntimeFunctionEntry* code = findRuntimeFunction(m_program, row.code.implementation.function);
            return code && code->function && !entersNativeCode(row.code.implementation);
        }
        if (!row.result.type)
            return true;
        const BuiltinType& given = m_builtins.at(row.result.type);
        return !given.isAnything() && !given.primitives.contains(BuiltinPrimitive::Symbol);
    });
}

void BodyLookOfProgramFacts::followAsPartOfClass(ValueID value, BuiltinID objectOfClass)
{
    if (m_classOfPartOfValue.isEmpty())
        m_classOfPartOfValue.fill(BuiltinID(), m_index.numberOfValues());
    m_classOfPartOfValue[value.index()] = objectOfClass;
}

void BodyLookOfProgramFacts::followAsComeByThroughLink(ValueID value, BuiltinID canonicalAbove)
{
    if (m_builtinThatLinkLedToOfValue.isEmpty())
        m_builtinThatLinkLedToOfValue.fill(BuiltinID(), m_index.numberOfValues());
    m_builtinThatLinkLedToOfValue[value.index()] = canonicalAbove;
}

bool BodyLookOfProgramFacts::followPartOfClass(const BodyValue& value, BuiltinID holder, BuiltinID member)
{
    if (value.opcode() != Opcode::PropGet || !isObjectOfClassInProgramFacts(m_builtins, m_builtins.canonicalID(holder)))
        return false;
    if (member) {
        BuiltinID canonical = m_builtins.canonicalID(member);
        BuiltinKind kind = m_builtins.at(canonical).kind;
        if ((kind != BuiltinKind::DataProperty && kind != BuiltinKind::Accessor) || standsForObjectInProgramFacts(m_builtins, canonical) || !mayHoldObjectInProgramFacts(m_builtins, canonical))
            return false;
    }
    followAsPartOfClass(value.id(), holder);
    const Builtin& below = m_builtins.at(m_builtins.canonicalID(holder));
    if (m_isOfProgram && below.prototypeLink == BuiltinPrototypeLink::Builtin && builtinMemberNameOf(m_program, value.as<Opcode::PropGet>().key()).name == BuiltinNames::__proto__)
        followAsComeByThroughLink(value.id(), m_builtins.canonicalID(below.inheritsFrom));
    return true;
}

void BodyLookOfProgramFacts::accessMemberOfPartOfClass(const BodyValue& value, BuiltinID objectOfClass)
{
    BuiltinID above = builtinThatLinkLedToAt(value.operand(0));
    if (value.opcode() == Opcode::PropGet) {
        followAsPartOfClass(value.id(), objectOfClass);
        if (above)
            followAsComeByThroughLink(value.id(), above);
    } else if (value.opcode() != Opcode::PropHas) {
        note(Finding::Kind::PartOfClassIsUsed, 0, objectOfClass.index(), witnessAt(value));
        if (above)
            noteUse(above, BuiltinUse::Escaped, value);
    }
}

PropertyKeyID BodyLookOfProgramFacts::provenSymbolOf(ValueID value) const
{
    if (BuiltinID constant = builtinSurelyAt(value))
        return keyOfWellKnownSymbol(m_program, m_builtins.canonicalID(constant));
    return provenSymbolAt(m_program, m_summaries, m_id, m_index, value);
}

ALWAYS_INLINE PropertyKeyID BodyLookOfProgramFacts::symbolKeyOfAccess(const BodyValue& value) const
{
    return computedAccessesInProgramFacts.contains(value.opcode()) ? provenSymbolOf(value.operand(1)) : PropertyKeyID();
}

void BodyLookOfProgramFacts::accessMember(const BodyValue& value, BuiltinID holder)
{
    Opcode opcode = value.opcode();
    PropertyKeyID key;
    BuiltinUse use = BuiltinUse::Written;
    bool isRead = opcode == Opcode::PropGet || opcode == Opcode::ElemGet;
    if (isRead) {
        key = opcode == Opcode::PropGet ? value.as<Opcode::PropGet>().key() : symbolKeyOfAccess(value);
        use = BuiltinUse::Read;
    } else if (opcode == Opcode::PropHas) {
        key = value.as<Opcode::PropHas>().key();
        use = BuiltinUse::Tested;
    } else if (opcode == Opcode::PropSet)
        key = value.as<Opcode::PropSet>().key();
    else if (opcode == Opcode::PropDelete)
        key = value.as<Opcode::PropDelete>().key();
    else if (opcode == Opcode::PropDefine)
        key = value.as<Opcode::PropDefine>().key();
    else
        key = symbolKeyOfAccess(value);
    BuiltinName nameRead = opcode == Opcode::PropGet ? builtinMemberNameOf(m_program, key).name : BuiltinName();
    if ((nameRead == BuiltinNames::__proto__ || nameRead == BuiltinNames::constructor) && !isObjectOfClassInProgramFacts(m_builtins, m_builtins.canonicalID(holder)))
        noteFate(value.operand(0), Fate::IsLetGo, value);

    if (const BuiltinCondition* light = m_rows.lightFormOf(m_builtins, m_builtins.canonicalID(holder))) {
        BuiltinName name = key ? m_program.builtinNameOf(key) : BuiltinName();
        bool isMethod = name && std::ranges::contains(light->methodsServed, name);
        if (opcode == Opcode::PropGet && (isMethod || (name && std::ranges::contains(light->valuesServed, name)))) {
            if (isMethod)
                m_methodsServed.append(MethodServed { value.id(), m_builtins.canonicalID(holder) });
            return;
        }
        note(Finding::Kind::LightFormIsAskedForMore, 0, holder.index(), witnessAt(value));
    }
    if (use == BuiltinUse::Written)
        noteWrite(holder, key, value, opcode == Opcode::PropDelete ? writeOnlyDeletesInProgramFacts : 0);
    m_readsOffNodeVm |= use == BuiltinUse::Read && m_builtins.canonicalID(holder) == m_builtins.canonicalID(Builtins::NodeVm);
    BuiltinID member = memberFoundOn(holder, key);
    bool mayRunAccessorAbove = use == BuiltinUse::Written || (use == BuiltinUse::Read && (!member || (m_builtins.at(m_builtins.canonicalID(member)).kind == BuiltinKind::Accessor && m_builtins.canonicalID(m_builtins.at(member).holder) != m_builtins.canonicalID(holder))));
    if (mayRunAccessorAbove)
        note(Finding::Kind::KeysOfBuiltinListed, 0, holder.index(), witnessAt(value));
    if (!member) {
        if (isRead && key && m_builtins.at(m_builtins.rootOf(m_builtins.canonicalID(holder))).scope != BuiltinScope::Realm)
            note(Finding::Kind::ReadThatNoRowAnswers, 1, holder.index(), key.index(), witnessAt(value));
        if (isRead && !tableRulesOutMemberFoundFrom(holder, key) && !readGivesNothingThatNobodyDescribes(holder, key))
            note(Finding::Kind::ReadThatNoRowAnswers, 0, holder.index(), witnessAt(value));
        followPartOfClass(value, holder, { });
        return;
    }
    noteUse(member, use, value);
    if (!isRead)
        return;
    if (!standsForObjectInProgramFacts(m_builtins, m_builtins.canonicalID(member)) && !m_rows.haveLightForms.quickGet(m_builtins.canonicalID(member).index()) && !m_builtins.nameOfSymbol(m_builtins.canonicalID(member))) {
        followWhatReadAnswers(value, member);
        if (!builtinAt(value.id()))
            followPartOfClass(value, holder, member);
        return;
    }
    m_builtinOfValue[value.id().index()] = member;
    if (m_mayBeSomethingElse.get(value.operand(0).index()))
        m_mayBeSomethingElse.set(value.id().index());
}

void BodyLookOfProgramFacts::letGoObjectsNamedIn(BuiltinTypeID type, const BodyValue& read)
{
    if (!type)
        return;
    for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
        if (part.kind == BuiltinTypePartKind::Builtin)
            noteUse(m_builtins.canonicalID(part.builtin), BuiltinUse::Escaped, read);
        for (const BuiltinSlotContents& contents : part.contents)
            letGoObjectsNamedIn(contents.type, read);
    }
}

void BodyLookOfProgramFacts::followWhatReadAnswers(const BodyValue& read, BuiltinID member)
{
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get || !overload.result.type)
            continue;
        for (const BuiltinTypePart& part : m_builtins.at(overload.result.type).parts) {
            BuiltinID& followed = m_builtinOfValue[read.id().index()];
            if (part.kind == BuiltinTypePartKind::Builtin && (!followed || followed == m_builtins.canonicalID(part.builtin)))
                followed = m_builtins.canonicalID(part.builtin);
            else if (part.kind == BuiltinTypePartKind::Builtin)
                noteUse(m_builtins.canonicalID(part.builtin), BuiltinUse::Escaped, read);
            for (const BuiltinSlotContents& contents : part.contents)
                letGoObjectsNamedIn(contents.type, read);
        }
    }
    if (builtinAt(read.id()))
        m_mayBeSomethingElse.set(read.id().index());
}

bool BodyLookOfProgramFacts::rowTakesEveryCallOn(const BuiltinOverload& overload, BuiltinID receiver) const
{
    auto takesWhateverIsPassed = [&](const BuiltinParameter& parameter) {
        return !parameter.accepted || parameter.conversion != BuiltinConversion::None || m_builtins.at(parameter.accepted).isAnything();
    };
    const BuiltinParameter* taken = overload.receiver.get();
    if (taken && !takesWhateverIsPassed(*taken)) {
        bool acceptsReceiver = false;
        for (const BuiltinTypePart& part : m_builtins.at(taken->accepted).parts)
            acceptsReceiver |= receiver && part.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(part.builtin) == m_builtins.canonicalID(receiver);
        if (!acceptsReceiver)
            return false;
    }
    return std::ranges::all_of(overload.parameters, takesWhateverIsPassed);
}

bool BodyLookOfProgramFacts::linkFollowsWritesOn(ValueID written, ValueID site) const
{
    return linkHoldsAsTheObject(written) && (std::ranges::contains(m_summary.calls, site, &SummaryCall::site) || std::ranges::contains(m_summary.accesses, site, &SummaryAccess::site) || std::ranges::contains(m_summary.changes, site, &SummaryChange::site));
}

bool BodyLookOfProgramFacts::linkHoldsAsTheObject(ValueID written) const
{
    if (!builtinAt(written))
        return false;
    BuiltinID canonical = m_builtins.canonicalID(builtinAt(written));
    BuiltinID root = m_builtins.canonicalID(m_builtins.rootOf(canonical));
    if (m_builtins.at(root).scope != BuiltinScope::Realm || compiledModuleWithObject(m_program, canonical) || compiledModuleWithObject(m_program, root))
        return false;
    static constexpr std::array kindsThatLinkHoldsAsTheObject { BuiltinKind::Namespace, BuiltinKind::Constructor, BuiltinKind::Function, BuiltinKind::Prototype, BuiltinKind::Method };
    if (!std::ranges::contains(kindsThatLinkHoldsAsTheObject, m_builtins.at(canonical).kind))
        return false;
    ValueID read = written;
    while (true) {
        read = m_index.stripCasts(read);
        if (!read || m_index.isParameter(read))
            return false;
        BodyValue definition = m_index.value(read);
        if (definition.opcode() == Opcode::Builtin) {
            auto named = definition.as<Opcode::Builtin>();
            const Builtin& entry = m_builtins.at(named.builtin());
            if (!named.isNotReadFromHolder() && entry.mutability != BuiltinMutability::AlwaysLocked && !m_builtins.isLocationOfLockedObject(named.builtin()) && entry.holder)
                return false;
            break;
        }
        if (definition.opcode() == Opcode::CheckInitialized) {
            read = definition.operand(0);
            continue;
        }
        if (definition.opcode() == Opcode::GetModuleVariable) {
            if (!m_variablesThatLinkFollowsAsTheObject.get(definition.as<Opcode::GetModuleVariable>().variable().index()))
                return false;
            break;
        }
        if (definition.opcode() != Opcode::PropGet)
            return false;
        read = definition.as<Opcode::PropGet>().objectOperand();
    }
    return true;
}

bool BodyLookOfProgramFacts::rowOnlyInspects(BuiltinID callee, BuiltinCallMode mode, BuiltinOperand position, BuiltinID receiver, OperandUses usesBeside) const
{
    bool hasOverload = false;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(callee))) {
        if (overload.mode != mode)
            continue;
        hasOverload = true;
        if (overload.isUndeclared())
            return false;
        const BuiltinParameter* parameter = position.isReceiver() ? overload.receiver.get() : m_builtins.parameterFor(overload, position);
        if (!parameter && !position.isReceiver())
            return false;
        if (parameter && !(parameter->uses - (usesBeside | OperandUse::Inspects)).isEmpty())
            return false;
        if (rowTakesEveryCallOn(overload, receiver))
            return true;
    }
    return hasOverload;
}

bool BodyLookOfProgramFacts::rowsOnlyBindAsReceiver(BuiltinID callee, BuiltinOperand position) const
{
    BuiltinSlotID boundReceiver = m_builtins.findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Key);
    bool binds = false;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(callee))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        if (overload.isUndeclared() || !overload.flow.callbacks.empty() || !overload.flow.propertyActions.empty() || !overload.flow.handlerCalls.empty())
            return false;
        const BuiltinParameter* parameter = m_builtins.parameterFor(overload, position);
        if (!parameter || !(parameter->uses - OperandUses { OperandUse::Inspects, OperandUse::Retains }).isEmpty())
            return false;
        for (const BuiltinTransfer& transfer : overload.flow.transfers) {
            if (transfer.from.kind != BuiltinPlaceKind::Operand || transfer.from.operand != position)
                continue;
            if (transfer.to.kind != BuiltinPlaceKind::ResultSlot || transfer.to.slot != boundReceiver)
                return false;
            binds = true;
        }
    }
    return binds;
}

bool BodyLookOfProgramFacts::rowsOnlyConstructReceiver(BuiltinID callee, BuiltinID receiver) const
{
    static constexpr OperandUses usesOfWhatIsOnlyConstructed { OperandUse::Inspects, OperandUse::Calls };
    if (!receiver)
        return false;
    BuiltinID canonicalCallee = m_builtins.canonicalID(callee);
    BuiltinMemberLookup inherited = m_builtins.findMember(m_builtins.canonicalID(receiver), m_builtins.at(canonicalCallee).name);
    if (inherited.outcome != BuiltinMemberLookup::Outcome::Found || m_builtins.canonicalID(inherited.member) != canonicalCallee)
        return false;
    bool isConstructed = false;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(canonicalCallee)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        if (overload.isUndeclared() || !overload.receiver || !(overload.receiver->uses - usesOfWhatIsOnlyConstructed).isEmpty() || !(overload.receiver->protocols - protocolsThatShowNothingInProgramFacts).isEmpty())
            return false;
        bool rowConstructs = false;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            if (!callback.callee.isReceiver())
                continue;
            bool handsOnSomethingOfCall = callback.newTarget.kind != BuiltinPlaceKind::None || callback.spreadArguments.kind != BuiltinPlaceKind::None
                || std::ranges::any_of(callback.arguments, [](const BuiltinPlace& place) { return place.kind != BuiltinPlaceKind::NewValue; });
            if (!callback.constructs || handsOnSomethingOfCall)
                return false;
            rowConstructs = true;
        }
        if (overload.receiver->uses.contains(OperandUse::Calls) && !rowConstructs)
            return false;
        isConstructed |= rowConstructs;
    }
    return isConstructed;
}

bool BodyLookOfProgramFacts::rowsOnlyCallReceiver(BuiltinID callee, BuiltinID receiver) const
{
    static constexpr OperandUses usesOfWhatIsOnlyCalled { OperandUse::Inspects, OperandUse::Calls };
    if (!receiver)
        return false;
    bool isCalled = false;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(callee))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        if (overload.isUndeclared() || !overload.receiver || !(overload.receiver->uses - usesOfWhatIsOnlyCalled).isEmpty() || !(overload.receiver->protocols - protocolsThatShowNothingInProgramFacts).isEmpty())
            return false;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            if (callback.callee.isReceiver() && callback.constructs)
                return false;
            isCalled |= callback.callee.isReceiver();
        }
    }
    return isCalled;
}

bool BodyLookOfProgramFacts::mayGoThroughOwnKeysOf(const BodyValue& user, uint32_t operandIndex) const
{
    Opcode opcode = user.opcode();
    if (!callsByValueInProgramFacts.contains(opcode))
        return !(operandInfoOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).protocols - protocolsThatShowNothingInProgramFacts).isEmpty();
    bool isConstruction = constructionsInProgramFacts.contains(opcode);
    if (!operandIndex || (isConstruction && operandIndex == 1))
        return false;
    BuiltinID callee = builtinSurelyAt(user.operand(0));
    if (!callee)
        return true;
    BuiltinOperand position = operandIndex == 1 ? BuiltinOperand::receiver() : BuiltinOperand::argument(operandIndex - numberOfNamedOperandsOf(opcode));
    BuiltinCallMode mode = isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    return std::ranges::any_of(m_builtins.overloadsOf(m_builtins.canonicalID(callee)), [&](const BuiltinOverload& overload) {
        if (overload.mode != mode)
            return false;
        const BuiltinParameter* parameter = position.isReceiver() ? overload.receiver.get() : m_builtins.parameterFor(overload, position);
        return parameter && !(parameter->protocols - protocolsThatShowNothingInProgramFacts).isEmpty();
    });
}

BodyLookOfProgramFacts::Fate BodyLookOfProgramFacts::fateOf(const BodyValue& user, uint32_t operandIndex) const
{
    Opcode opcode = user.opcode();
    if (opcode == Opcode::NewClass && user.as<Opcode::NewClass>().heritageOperand() == user.operand(operandIndex)) {
        BuiltinID heritage = builtinSurelyAt(user.operand(operandIndex));
        BuiltinClassID written = user.as<Opcode::NewClass>().layout() ? m_program.at(user.as<Opcode::NewClass>().layout()).writtenBuiltinBase : BuiltinClassID();
        BuiltinID constructorOfWritten = written ? m_builtins.at(written).constructor : BuiltinID();
        if (heritage && constructorOfWritten && m_builtins.canonicalID(constructorOfWritten) == m_builtins.canonicalID(heritage) && !m_rows.writeUnderKeysOfCalls.quickGet(m_builtins.canonicalID(heritage).index()))
            return Fate::StandsAboveClass;
        BuiltinID possible = written ? BuiltinID() : builtinAt(user.operand(operandIndex));
        if (possible && m_builtins.at(m_builtins.canonicalID(possible)).kind == BuiltinKind::Constructor && !m_rows.writeUnderKeysOfCalls.quickGet(m_builtins.canonicalID(possible).index()))
            return Fate::StandsAboveClass;
    }
    if (opcode == Opcode::ResolveHeritage)
        return Fate::IsInspected;
    if (opcode == Opcode::PutModuleVariable) {
        BuiltinID held = builtinHeldBy(user.as<Opcode::PutModuleVariable>().variable());
        BuiltinID stored = builtinSurelyAt(user.operand(operandIndex));
        if (held && stored && m_builtins.canonicalID(held) == m_builtins.canonicalID(stored))
            return Fate::IsHeldByVariable;
    }
    if (!callsByValueInProgramFacts.contains(opcode)) {
        bool isOnlyInspected = (user.operandUses(operandIndex) - OperandUses { OperandUse::Inspects }).isEmpty();
        if (opcode == Opcode::ElemHas && isOnlyInspected && receiverOf(opcode) == operandIndex)
            return Fate::IsInspected;
        if (accessClassOf(opcode) == AccessClass::ByName && receiverOf(opcode) == operandIndex && !isOnlyInspected)
            return (user.operandUses(operandIndex) - usesThatOnlyWriteOnOperandInProgramFacts).isEmpty() && linkFollowsWritesOn(user.operand(operandIndex), user.id()) ? Fate::IsWrittenOn : Fate::IsLetGo;
        if (accessClassOf(opcode) == AccessClass::ByName)
            return isOnlyInspected && receiverOf(opcode) == operandIndex ? Fate::IsAskedForAnyMember : Fate::IsLetGo;
        if ((opcode == Opcode::NewArray || opcode == Opcode::NewClosure || opcode == Opcode::PutEnvironmentField || (opcode == Opcode::NewObject && operandIndex != OperandIndex::NewObject::prototype)) && linkHoldsAsTheObject(user.operand(operandIndex)))
            return Fate::IsFollowedByLink;
        bool isHandedToCodeOfProgram = (opcode == Opcode::InstanceOf && !operandIndex && !builtinSurelyAt(user.operand(1))) || operandInfoOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).protocols.contains(Protocol::ToPrimitive);
        if (isHandedToCodeOfProgram && !linkFollowsWritesOn(user.operand(operandIndex), user.id()))
            return Fate::IsLetGo;
        return isOnlyInspected ? Fate::IsInspected : Fate::IsLetGo;
    }

    bool isConstruction = constructionsInProgramFacts.contains(opcode);
    if (BuiltinID holder = holderOfMethodServed(user.operand(0)); holder && !isConstruction && operandIndex <= 1 && builtinSurelyAt(user.operand(1)) && m_builtins.canonicalID(builtinSurelyAt(user.operand(1))) == holder)
        return Fate::IsInspected;
    if (!operandIndex)
        return isConstruction ? Fate::IsConstructed : Fate::IsCalled;
    if (isConstruction && operandIndex == 1 && user.operand(1) == user.operand(0))
        return Fate::IsInspected;
    BuiltinID callee = builtinSurelyAt(user.operand(0));
    if (!callee)
        return linkFollowsWritesOn(user.operand(operandIndex), user.id()) ? Fate::IsFollowedByLink : Fate::IsLetGo;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    BuiltinCallMode mode = isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    BuiltinID receiver = isConstruction ? BuiltinID() : builtinSurelyAt(user.operand(1));
    if (operandIndex == 1 && !isConstruction && rowsOnlyConstructReceiver(callee, receiver))
        return Fate::IsConstructed;
    if (operandIndex == 1 && !isConstruction && rowsOnlyCallReceiver(callee, receiver))
        return Fate::IsCalled;
    auto fateAt = [&](BuiltinOperand position) {
        if (rowOnlyInspects(callee, mode, position, receiver))
            return Fate::IsInspected;
        if (position.isReceiver() && rowOnlyInspects(callee, mode, position, receiver, { OperandUse::Returns, OperandUse::PassesToUserCode }) && linkFollowsWritesOn(user.operand(operandIndex), user.id()))
            return Fate::IsLeftToLink;
        return rowOnlyInspects(callee, mode, position, receiver, usesThatOnlyWriteOnOperandInProgramFacts) && linkFollowsWritesOn(user.operand(operandIndex), user.id()) ? Fate::IsWrittenOn : Fate::IsLetGo;
    };
    if (operandIndex == 1 && !isConstruction)
        return fateAt(BuiltinOperand::receiver());
    if (operandIndex < numberOfNamedOperands || !callsThatSpellOutArgumentsInProgramFacts.contains(opcode))
        return Fate::IsLetGo;
    uint32_t argument = operandIndex - numberOfNamedOperands;
    if (argument > BuiltinOperand::maxArgumentIndex)
        return Fate::IsLetGo;
    if (!isConstruction && receiver && m_builtins.ignoresReceiver(receiver) && rowsOnlyBindAsReceiver(callee, BuiltinOperand::argument(argument)))
        return Fate::IsInspected;
    return fateAt(BuiltinOperand::argument(argument));
}

bool BodyLookOfProgramFacts::isURLOrURLString(ValueID value) const
{
    if (originOfURL(value))
        return true;
    BodyValue read = stripCasts(value);
    if (!m_rows.href || read.opcode() != Opcode::PropGet || builtinMemberNameOf(m_program, read.as<Opcode::PropGet>().key()).name != m_rows.href)
        return false;
    ValueID origin = originOfURL(read.operand(0));
    if (!origin || !callsThatSpellOutArgumentsInProgramFacts.contains(m_index.opcodeOf(origin)))
        return false;
    std::span<const ValueID> written = m_index.value(origin).variadicOperands();
    return !written.empty() && stripCasts(written[0]).opcode() == Opcode::String;
}

void BodyLookOfProgramFacts::noteConstructionWhoseOperandsNobodySees(BuiltinID row, const Witness& witness)
{
    if (m_rows.areEvents.quickGet(row.index()) || isLoaderOfMachineCodeInProgramFacts(m_builtins, row))
        note(Finding::Kind::CallOfEvent, 0, row.index(), witness);
    if (m_rows.startWorkers.quickGet(row.index()))
        note(Finding::Kind::WorkerStartWithoutName, 0, Wire::none, witness);
    if (optionThatRunsCodeInProgramFacts(m_builtins, row))
        note(Finding::Kind::OptionThatRunsCode, 0, row.index(), witness);
    if (m_builtins.conditionOnInstancesThatAnswerEveryName(row))
        note(Finding::Kind::InstanceThatMayAnswerEveryName, 0, row.index(), Wire::none, witness);
}

void BodyLookOfProgramFacts::noteStartOfWorker(const BodyValue& call, bool spellsOutArguments)
{
    if (!spellsOutArguments)
        return note(Finding::Kind::WorkerStartWithoutName, 0, Wire::none, witnessAt(call));

    bool mayHaveOptions = !m_rows.fileArguments;
    bool isFromURL = false;
    bool mayGetUnknownFile = false;
    Vector<StringID, 1> paths;
    std::span<const ValueID> arguments = call.variadicOperands();
    for (size_t argument = 0; argument < arguments.size(); ++argument) {
        BodyValue passed = stripCasts(arguments[argument]);
        Opcode opcode = passed.opcode();
        bool isNullish = opcode == Opcode::Undefined || opcode == Opcode::Null;
        if (m_rows.fileArguments && !(m_rows.fileArguments & RowsOfProgramFacts::bitOfArgument(argument))) {
            mayHaveOptions |= !isNullish;
            continue;
        }
        if (opcode == Opcode::String)
            paths.append(passed.as<Opcode::String>().string());
        else if (isURLOrURLString(arguments[argument]))
            isFromURL = true;
        else
            mayGetUnknownFile |= !isNullish;
    }
    for (StringID path : paths)
        note(Finding::Kind::WorkerEntry, mayHaveOptions, path.index(), witnessAt(call));
    if (mayGetUnknownFile || (paths.isEmpty() && !isFromURL))
        note(Finding::Kind::WorkerStartWithoutName, 0, Wire::none, witnessAt(call));
    else if (isFromURL)
        note(Finding::Kind::WorkerStartFromURL, 0, Wire::none, witnessAt(call));
}

bool BodyLookOfProgramFacts::listKeysOfLiteral(ValueID value, Vector<PropertyKeyID, 4>& keys) const
{
    ValueID finished = m_index.stripCasts(value);
    if (finished && !m_index.isParameter(finished) && m_index.opcodeOf(finished) == Opcode::Data) {
        DataID data = m_index.value(finished).as<Opcode::Data>().data();
        const auto* written = data ? m_program.at(data).root.tryAs<DataNode::Object>() : nullptr;
        if (!written || !m_index.onlyUsingValue(finished))
            return false;
        for (const DataMember& member : written->members)
            keys.append(member.key);
        return true;
    }
    if (!finished || m_index.isParameter(finished) || m_index.opcodeOf(finished) != Opcode::FinishObject)
        return false;
    ValueID literal = m_index.stripCasts(m_index.value(finished).operand(0));
    if (!literal || m_index.isParameter(literal) || m_index.opcodeOf(literal) != Opcode::NewObject || m_index.blockOf(literal) != m_index.blockOf(finished))
        return false;
    auto made = m_index.value(literal).as<Opcode::NewObject>();
    if (!made.layout())
        return false;
    for (uint32_t field : made.fields())
        keys.append(m_program.at(made.layout()).fields[field].key);
    bool listsEveryKey = true;
    for (uint32_t number = literal.index() + 1; number < finished.index(); ++number) {
        BodyValue part = m_index.value(ValueID(number));
        bool touchesLiteral = false;
        part.forEachOperand([&](ValueID operand, uint32_t) {
            touchesLiteral |= operand && m_index.stripCasts(operand) == literal;
        });
        if (!touchesLiteral)
            continue;
        if (part.opcode() == Opcode::PropDefine && m_index.stripCasts(part.operand(0)) == literal)
            keys.append(part.as<Opcode::PropDefine>().key());
        else
            listsEveryKey = false;
    }
    return listsEveryKey;
}

void BodyLookOfProgramFacts::noteWritesOfRow(const BodyValue& call, BuiltinID row, BuiltinCallMode mode, bool spellsOutArguments)
{
    std::span<const ValueID> arguments = call.variadicOperands();
    auto passedAs = [&](std::optional<BuiltinOperand> operand) -> ValueID {
        if (!operand)
            return { };
        if (operand->isReceiver())
            return mode == BuiltinCallMode::Call ? call.operand(1) : ValueID();
        bool isSpelledOut = spellsOutArguments && operand->kind() == BuiltinOperand::Kind::Argument && operand->argumentIndex() < arguments.size();
        return isSpelledOut ? arguments[operand->argumentIndex()] : ValueID();
    };
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(row)) {
        if (overload.mode != mode)
            continue;
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (action.kind == BuiltinPropertyActionKind::ChangesPrototype) {
                if (BuiltinID target = builtinAt(passedAs(action.object)))
                    note(Finding::Kind::PrototypeChangeOfBuiltin, 0, target.index(), witnessAt(call));
                continue;
            }
            if (!writesUnderKeyOfCallInProgramFacts(action))
                continue;
            BuiltinID target = builtinAt(passedAs(action.object));
            if (!target)
                continue;
            const BuiltinParameter* taker = action.object.isReceiver() ? overload.receiver.get() : m_builtins.parameterFor(overload, action.object);
            if (taker && taker->accepted && taker->conversion == BuiltinConversion::None && !m_builtins.at(taker->accepted).mayBeObject() && isObjectKind(m_builtins.at(m_builtins.canonicalID(target)).kind))
                continue;
            if (action.key.kind == BuiltinKeyKind::None && action.value.operand && !action.value.operand->isReceiver() && spellsOutArguments) {
                Vector<PropertyKeyID, 4> keys;
                bool listsEveryKey = true;
                size_t end = action.value.operand->kind() == BuiltinOperand::Kind::Argument ? action.value.operand->argumentIndex() + 1 : arguments.size();
                for (size_t argument = action.value.operand->argumentIndex(); argument < std::min(end, arguments.size()); ++argument)
                    listsEveryKey &= listKeysOfLiteral(arguments[argument], keys);
                for (PropertyKeyID listed : keys)
                    noteWrite(target, listed, call);
                if (listsEveryKey)
                    continue;
            }
            PropertyKeyID key;
            ValueID passedKey = action.key.kind == BuiltinKeyKind::Operand ? passedAs(action.key.operand) : ValueID();
            if (passedKey && stripCasts(passedKey).opcode() == Opcode::String) {
                std::optional<PropertyKeyID> spelled = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = stripCasts(passedKey).as<Opcode::String>().string() });
                if (!spelled)
                    continue;
                key = *spelled;
            } else if (passedKey)
                key = provenSymbolOf(passedKey);
            note(Finding::Kind::WriteOnBuiltin, static_cast<uint8_t>((key ? writeIsNamedByRowInProgramFacts : 0) | (action.kind == BuiltinPropertyActionKind::DeletesByKey ? writeOnlyDeletesInProgramFacts : 0)), target.index(), key ? key.index() : Wire::none, witnessAt(call));
        }
    }
}

bool BodyLookOfProgramFacts::mayPass(const BodyValue& call, const BuiltinCondition& option, bool spellsOutArguments) const
{
    if (!spellsOutArguments || option.operand.kind() != BuiltinOperand::Kind::Argument)
        return true;
    std::span<const ValueID> arguments = call.variadicOperands();
    if (option.operand.argumentIndex() >= arguments.size())
        return false;
    PropertyKeyID key = m_program.keyOf(option.hasProperty);
    if (!key)
        return false;
    Vector<PropertyKeyID, 4> keys;
    listKeysOfLiteral(arguments[option.operand.argumentIndex()], keys);
    if (keys.contains(key))
        return true;
    ValueID passed = m_index.stripCasts(arguments[option.operand.argumentIndex()]);
    if (!passed || m_index.isParameter(passed))
        return false;
    for (uint32_t number = passed.index() + 1; number < call.id().index(); ++number) {
        if (m_index.isParameter(ValueID(number)))
            continue;
        BodyValue part = m_index.value(ValueID(number));
        bool storesOption = (part.opcode() == Opcode::PropDefine && part.as<Opcode::PropDefine>().key() == key) || (part.opcode() == Opcode::PropSet && part.as<Opcode::PropSet>().key() == key);
        if (storesOption && m_index.stripCasts(part.operand(0)) == passed)
            return true;
    }
    return false;
}

bool BodyLookOfProgramFacts::operandIsDataWithoutMember(const BodyValue& construction, const BuiltinCondition& member, bool spellsOutArguments) const
{
    std::span<const ValueID> arguments = construction.variadicOperands();
    if (!spellsOutArguments || member.operand.kind() != BuiltinOperand::Kind::Argument || member.operand.argumentIndex() >= arguments.size() || m_program.testsReplaceBuiltins())
        return false;
    ValueID operand = arguments[member.operand.argumentIndex()];
    if (m_index.isParameter(operand) || m_index.opcodeOf(operand) != Opcode::Data || m_index.onlyUsingValue(operand) != construction.id())
        return false;
    DataID data = m_index.value(operand).as<Opcode::Data>().data();
    const auto* written = data ? m_program.at(data).root.tryAs<DataNode::Object>() : nullptr;
    PropertyKeyID key = m_program.keyOf(member.hasProperty);
    return written && std::ranges::none_of(written->members, [&](const DataMember& part) { return part.key == key; });
}

std::optional<FunctionID> BodyLookOfProgramFacts::functionUnderMemberOfLiteralThatNobodyElseHolds(const BodyValue& construction, const BuiltinCondition& member, bool spellsOutArguments) const
{
    std::span<const ValueID> arguments = construction.variadicOperands();
    if (!spellsOutArguments || member.operand.kind() != BuiltinOperand::Kind::Argument || member.operand.argumentIndex() >= arguments.size() || m_program.testsReplaceBuiltins())
        return std::nullopt;
    ValueID finished = arguments[member.operand.argumentIndex()];
    if (m_index.isParameter(finished) || m_index.opcodeOf(finished) != Opcode::FinishObject || m_index.onlyUsingValue(finished) != construction.id())
        return std::nullopt;
    ValueID literal = m_index.value(finished).operand(0);
    if (m_index.isParameter(literal) || m_index.opcodeOf(literal) != Opcode::NewObject || m_index.blockOf(literal) != m_index.blockOf(finished))
        return std::nullopt;
    auto made = m_index.value(literal).as<Opcode::NewObject>();
    if (!made.layout() || made.prototypeOperand())
        return std::nullopt;

    PropertyKeyID key = m_program.keyOf(member.hasProperty);
    FunctionID answering;
    auto keepsMembersAndIsHandedNoName = [&](PropertyKeyID under, ValueID stored) {
        ValueID written = m_index.stripCasts(stored);
        if (m_index.isParameter(written))
            return false;
        FunctionID function;
        if (m_index.opcodeOf(written) == Opcode::NewClosure)
            function = m_index.value(written).as<Opcode::NewClosure>().function();
        else if (!primitiveConstants.contains(m_index.opcodeOf(written)))
            return false;
        if (under == key)
            answering = function;
        if (!function)
            return true;
        const Function& code = m_program.at(function);
        return code.parameters.size() <= 1 && !code.hasAny({ FunctionFlags::HasThis, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest });
    };
    std::span<const ValueID> values = made.valuesOperands();
    size_t place = 0;
    for (uint32_t field : made.fields()) {
        if (place >= values.size() || !keepsMembersAndIsHandedNoName(m_program.at(made.layout()).fields[field].key, values[place++]))
            return std::nullopt;
    }
    uint32_t numberOfUses = 1;
    for (uint32_t number = literal.index() + 1; number < finished.index(); ++number) {
        BodyValue part = m_index.value(ValueID(number));
        bool touchesLiteral = false;
        part.forEachOperand([&](ValueID operand, uint32_t) {
            touchesLiteral |= operand && m_index.stripCasts(operand) == literal;
        });
        if (!touchesLiteral)
            continue;
        if (part.opcode() != Opcode::PropDefine || part.as<Opcode::PropDefine>().objectOperand() != literal || !keepsMembersAndIsHandedNoName(part.as<Opcode::PropDefine>().key(), part.as<Opcode::PropDefine>().valueOperand()))
            return std::nullopt;
        ++numberOfUses;
    }
    if (numberOfUses >= BodyIndex::maxCountedUses || m_index.useCount(literal) != numberOfUses)
        return std::nullopt;
    return answering;
}

void BodyLookOfProgramFacts::noteLoaderHandedToText(const BodyValue& call)
{
    auto definitionBy = [&](Opcode opcode, ValueID value) {
        ValueID behind = value ? m_index.stripCasts(value) : ValueID();
        return behind && !m_index.isParameter(behind) && m_index.opcodeOf(behind) == opcode ? behind : ValueID();
    };
    auto isAt = [&](BuiltinID wanted, ValueID value) {
        BuiltinID found = builtinAt(value);
        return found && m_builtins.canonicalID(found) == m_builtins.canonicalID(wanted);
    };
    ValueID read = m_isOfProgram && callsThatSpellOutArgumentsInProgramFacts.contains(call.opcode()) ? definitionBy(Opcode::PropGet, call.operand(0)) : ValueID();
    if (!read || !isAt(Builtins::NodeVm, m_index.value(read).operand(0)))
        return;
    for (ValueID argument : call.variadicOperands()) {
        ValueID finished = definitionBy(Opcode::FinishObject, argument);
        ValueID literal = definitionBy(Opcode::NewObject, finished ? m_index.value(finished).operand(0) : argument);
        if (!literal)
            continue;
        m_index.value(literal).forEachOperand([&](ValueID field, uint32_t) {
            ValueID made = definitionBy(Opcode::Call, field);
            if (made ? isAt(Builtins::NodeModuleCreateRequire, m_index.value(made).operand(0)) : isAt(Builtins::ModuleRequire, field))
                note(Finding::Kind::LoaderHandedToText, 0, Wire::none, witnessAt(call));
        });
    }
}

void BodyLookOfProgramFacts::lookAtCall(const BodyValue& call)
{
    if (m_readsOffNodeVm)
        noteLoaderHandedToText(call);
    BuiltinID callee = builtinAt(call.operand(0));
    if (!callee)
        return;
    BuiltinID row = m_builtins.canonicalID(callee);
    Opcode opcode = call.opcode();
    bool isConstruction = constructionsInProgramFacts.contains(opcode);
    bool spellsOutArguments = callsThatSpellOutArgumentsInProgramFacts.contains(opcode);
    if (m_rows.areEvents.quickGet(row.index()) || isLoaderOfMachineCodeInProgramFacts(m_builtins, row))
        note(Finding::Kind::CallOfEvent, 0, row.index(), witnessAt(call));

    if (m_rows.startWorkers.quickGet(row.index()))
        noteStartOfWorker(call, spellsOutArguments);
    if (m_rows.writeUnderKeysOfCalls.quickGet(row.index()))
        noteWritesOfRow(call, row, isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, spellsOutArguments);
    if (const BuiltinCondition* option = optionThatRunsCodeInProgramFacts(m_builtins, row)) {
        if (mayPass(call, *option, spellsOutArguments))
            note(Finding::Kind::OptionThatRunsCode, 0, row.index(), witnessAt(call));
    }
    if (const BuiltinCondition* member = isConstruction ? m_builtins.conditionOnInstancesThatAnswerEveryName(row) : nullptr) {
        if (!operandIsDataWithoutMember(call, *member, spellsOutArguments)) {
            std::optional<FunctionID> answering = functionUnderMemberOfLiteralThatNobodyElseHolds(call, *member, spellsOutArguments);
            if (!answering || *answering)
                note(Finding::Kind::InstanceThatMayAnswerEveryName, 0, row.index(), answering ? answering->index() : Wire::none, witnessAt(call));
        }
    }
    if (isConstruction && row == m_rows.constructorOfURLs) {
        if (m_originOfURLOfValue.isEmpty())
            m_originOfURLOfValue.fill(ValueID(), m_index.numberOfValues());
        m_originOfURLOfValue[call.id().index()] = call.id();
    }
}

TypeID BodyLookOfProgramFacts::writtenTypeOf(ValueID value) const
{
    while (value) {
        if (m_index.isParameter(value)) {
            if (m_index.blockOf(value).index() || value.index() >= m_function.numberOfEntryParameters())
                return { };
            const DeclaredType* declared = m_function.declaredEntryParameter(value.index());
            return declared && declared->isGuarded() ? declared->written() : TypeID();
        }
        BodyValue definition = m_index.value(value);
        if (definition.opcode() == Opcode::Convert && definition.as<Opcode::Convert>().declared().isGuarded())
            return definition.as<Opcode::Convert>().declared().written();
        if (!opcodesThatStateTypes.contains(definition.opcode()))
            return { };
        value = definition.operand(0);
    }
    return { };
}

TypeID BodyLookOfProgramFacts::declaredTypeOfField(TypeID receiver, PropertyKeyID key) const
{
    if (!receiver || !key)
        return { };
    TypeID found;
    for (LayoutID layout : membersOf(TypeReader(m_program), m_program.types.at(receiver))) {
        std::optional<FieldReference> field = findField(m_program, layout, key);
        TypeID declared = field ? fieldAt(m_program, declarationOf(m_program, *field)).declared.written() : TypeID();
        if (!declared || (found && found != declared))
            return { };
        found = declared;
    }
    return found;
}

TypeID BodyLookOfProgramFacts::declaredTypeOfElement(TypeID list) const
{
    if (!list)
        return { };
    TypeID found;
    for (const ClassPart& part : classPartsOf(TypeReader(m_program), m_program.types.at(list))) {
        TypeID contents = part.writtenContentsAt(m_program.builtins(), ContentsSlot::Element);
        if (!contents || (found && found != contents))
            return { };
        found = contents;
    }
    return found;
}

TypeID BodyLookOfProgramFacts::declaredTypeOf(ValueID value) const
{
    Vector<PropertyKeyID, 4> reads;
    auto typeOfReads = [&](TypeID type) {
        while (!reads.isEmpty()) {
            PropertyKeyID key = reads.takeLast();
            type = key ? declaredTypeOfField(type, key) : declaredTypeOfElement(type);
        }
        return type;
    };
    while (value) {
        if (m_index.isParameter(value)) {
            if (m_index.blockOf(value).index() || value.index() >= m_function.numberOfEntryParameters())
                return { };
            const DeclaredType* declared = m_function.declaredEntryParameter(value.index());
            return declared ? typeOfReads(declared->written()) : TypeID();
        }
        BodyValue definition = m_index.value(value);
        switch (definition.opcode()) {
        case Opcode::Convert:
            if (TypeID declared = definition.as<Opcode::Convert>().declared().written(); declared && declared != TypeTable::anything)
                return typeOfReads(declared);
            break;
        case Opcode::GetModuleVariable:
            return typeOfReads(m_program.at(definition.as<Opcode::GetModuleVariable>().variable()).declared.written());
        case Opcode::GetEnvironmentField:
            return typeOfReads(m_program.at(definition.as<Opcode::GetEnvironmentField>().layout()).fields[definition.as<Opcode::GetEnvironmentField>().field()].declared.written());
        case Opcode::PropGet:
            if (definition.isFreeName())
                return { };
            reads.append(definition.as<Opcode::PropGet>().key());
            break;
        case Opcode::ElemGet:
            reads.append(PropertyKeyID());
            break;
        default:
            if (definition.opcode() != Opcode::CheckInitialized && !opcodesThatStateTypes.contains(definition.opcode()))
                return { };
            break;
        }
        value = definition.operand(0);
    }
    return { };
}

void BodyLookOfProgramFacts::forEachDeclarerOfFieldOf(ValueID receiver, PropertyKeyID key, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    TypeID declared = declaredTypeOf(receiver);
    if (!declared)
        return;
    for (LayoutID layout : membersOf(TypeReader(m_program), m_program.types.at(declared))) {
        if (std::optional<FieldReference> field = findField(m_program, layout, key))
            functor(declarationOf(m_program, *field).layout);
    }
}

bool BodyLookOfProgramFacts::isTruncation(const BodyValue& operation) const
{
    static constexpr OpcodeSet operatorsWithZeroAsIdentity { Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    if (operation.opcode() == Opcode::BitNot) {
        ValueID user = m_index.onlyUsingValue(operation.id());
        return user && !m_index.isParameter(user) && m_index.opcodeOf(user) == Opcode::BitNot;
    }
    if (!operatorsWithZeroAsIdentity.contains(operation.opcode()))
        return false;
    ValueID right = operation.operand(1);
    return right && !m_index.isParameter(right) && m_index.opcodeOf(right) == Opcode::Integer && !m_index.value(right).as<Opcode::Integer>().integer();
}

void BodyLookOfProgramFacts::noteReadUsedAsInteger(ValueID used, bool isTruncatedAlone)
{
    if (!used)
        return;
    auto noteRead = [&](const BodyValue& definition) {
        if (definition.opcode() != Opcode::PropGet)
            return false;
        if (PropertyKeyID key = definition.as<Opcode::PropGet>().key()) {
            bool isOfKnownLayout = false;
            forEachDeclarerOfFieldOf(definition.as<Opcode::PropGet>().objectOperand(), key, [&](LayoutID declarer) {
                m_namesOfNumbers.noteReadAsInteger(declarer, key);
                isOfKnownLayout = true;
            });
            if (!isOfKnownLayout && m_function.module && !definition.isFreeName())
                m_namesOfNumbers.noteNameReadAsInteger(m_function.module, key);
        }
        return true;
    };
    ValueID behindCasts = m_index.stripCasts(used);
    if (!behindCasts || m_index.isParameter(behindCasts))
        return;
    BodyValue definition = m_index.value(behindCasts);
    if ((!isTruncatedAlone && noteRead(definition)) || !allSums.contains(definition.opcode()))
        return;
    definition.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID part = operand ? m_index.stripCasts(operand) : ValueID();
        if (part && !m_index.isParameter(part))
            noteRead(m_index.value(part));
    });
}

TypeID BodyLookOfProgramFacts::writtenTypeWhereValueIsHandedOn(ValueID value) const
{
    value = m_index.stripCasts(value);
    if (!value)
        return { };
    for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
        for (BodyValue user : m_index.block(BlockID(number)).values()) {
            if (user.opcode() == Opcode::Convert && user.as<Opcode::Convert>().declared().isGuarded() && m_index.stripCasts(user.operand(0)) == value)
                return user.as<Opcode::Convert>().declared().written();
        }
    }
    return { };
}

void BodyLookOfProgramFacts::noteVariableStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID key, ValueID stored)
{
    while (stored && !m_index.isParameter(stored) && m_index.opcodeOf(stored) == Opcode::CheckInitialized)
        stored = m_index.stripCasts(m_index.value(stored).operand(0));
    if (!stored || m_index.isParameter(stored))
        return;
    BodyValue definition = m_index.value(stored);
    VariableStoredInFieldInProgramFacts store;
    store.key = key;
    const DeclaredType* declared = nullptr;
    if (definition.opcode() == Opcode::GetModuleVariable) {
        store.ofModule = definition.as<Opcode::GetModuleVariable>().variable();
        declared = &m_program.at(store.ofModule).declared;
    } else if (definition.opcode() == Opcode::GetEnvironmentField) {
        store.ofEnvironment = { definition.as<Opcode::GetEnvironmentField>().layout(), definition.as<Opcode::GetEnvironmentField>().field() };
        declared = &m_program.at(store.ofEnvironment.layout).fields[store.ofEnvironment.field].declared;
    }
    if (!declared || !declared->isGuarded() || !declared->written())
        return;
    Type writtenOfVariable = m_program.types.at(declared->written());
    if (!(writtenOfVariable.primitives & Primitive::Number) || !writtenOfVariable.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
        return;
    auto note = [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(m_program, layout, key);
        store.declarer = field ? declarationOf(m_program, *field).layout : layout;
        m_namesOfNumbers.noteStoredVariable(store);
    };
    if (allocated) {
        note(allocated);
        return;
    }
    TypeID written = writtenTypeOf(receiver);
    if (!written)
        written = writtenTypeWhereValueIsHandedOn(receiver);
    if (!written)
        return;
    for (LayoutID layout : membersOf(TypeReader(m_program), m_program.types.at(written)))
        note(layout);
}

void BodyLookOfProgramFacts::noteReadUsedForArithmetic(ValueID used)
{
    ValueID behindCasts = used ? m_index.stripCasts(used) : ValueID();
    if (!behindCasts || m_index.isParameter(behindCasts) || m_index.opcodeOf(behindCasts) != Opcode::PropGet)
        return;
    auto read = m_index.value(behindCasts).as<Opcode::PropGet>();
    if (PropertyKeyID key = read.key()) {
        forEachDeclarerOfFieldOf(read.objectOperand(), key, [&](LayoutID declarer) {
            m_namesOfNumbers.noteReadForArithmetic(declarer, key);
        });
    }
}

static constexpr OpcodeSet productsInProgramFacts { Opcode::Mul, Opcode::Div, Opcode::Pow };
static constexpr OpcodeSet otherTermsInProgramFacts = productsInProgramFacts | OpcodeSet { Opcode::Double };

PropertyKeyID BodyLookOfProgramFacts::keyOfRead(ValueID value) const
{
    ValueID behindCasts = value ? m_index.stripCasts(value) : ValueID();
    bool isRead = behindCasts && !m_index.isParameter(behindCasts) && m_index.opcodeOf(behindCasts) == Opcode::PropGet;
    return isRead ? m_index.value(behindCasts).as<Opcode::PropGet>().key() : PropertyKeyID();
}

bool BodyLookOfProgramFacts::computesFraction(const BodyValue& operation) const
{
    if (productsInProgramFacts.contains(operation.opcode()))
        return true;
    if (operation.opcode() != Opcode::Add && operation.opcode() != Opcode::Sub)
        return false;
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        ValueID operand = m_index.stripCasts(operation.operand(operandIndex));
        if (operand && !m_index.isParameter(operand) && otherTermsInProgramFacts.contains(m_index.opcodeOf(operand)))
            return true;
    }
    return false;
}

void BodyLookOfProgramFacts::noteUsedInProduct(const BodyValue& operation, LayoutID declarer, PropertyKeyID key)
{
    if (m_index.loopOf(m_index.blockOf(operation.id())))
        m_namesOfNumbers.noteUsedInProduct(declarer, key);
    else
        m_namesOfNumbers.noteUsedInProductOutsideLoops({ m_id, declarer, key });
}

void BodyLookOfProgramFacts::noteReadsInProduct(const BodyValue& operation)
{
    auto noteRead = [&](ValueID operand) {
        PropertyKeyID key = keyOfRead(operand);
        if (!key)
            return;
        noteReadUsedForArithmetic(operand);
        forEachDeclarerOfFieldOf(m_index.value(m_index.stripCasts(operand)).as<Opcode::PropGet>().objectOperand(), key, [&](LayoutID declarer) {
            noteUsedInProduct(operation, declarer, key);
        });
    };
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        noteRead(operand);
        ValueID term = m_index.stripCasts(operand);
        if (!term || m_index.isParameter(term) || (m_index.opcodeOf(term) != Opcode::Add && m_index.opcodeOf(term) != Opcode::Sub))
            return;
        m_index.value(term).forEachOperand([&](ValueID part, uint32_t) {
            noteRead(part);
        });
    });
}

void BodyLookOfProgramFacts::noteWhatIsStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID key, ValueID stored)
{
    ValueID behindCasts = key && stored ? m_index.stripCasts(stored) : ValueID();
    if (!behindCasts || m_index.isParameter(behindCasts))
        return;
    BodyValue definition = m_index.value(behindCasts);
    Opcode opcode = definition.opcode();
    if (readsOfVariablesInProgramFacts.contains(opcode)) {
        noteVariableStoredUnder(receiver, allocated, key, behindCasts);
        return;
    }
    auto receiverOfRead = [&](ValueID read) {
        return m_index.stripCasts(m_index.value(m_index.stripCasts(read)).as<Opcode::PropGet>().objectOperand());
    };
    bool isSumOrDifference = opcode == Opcode::Add || opcode == Opcode::Sub;
    bool accumulatesField = isSumOrDifference && keyOfRead(definition.operand(0)) == key && keyOfRead(definition.operand(1)) && keyOfRead(definition.operand(1)) != key
        && receiverOfRead(definition.operand(0)) == receiverOfRead(definition.operand(1));
    if (accumulatesField)
        noteReadsInProduct(definition);
    if (accumulatesField || computesFraction(definition)) {
        auto noteStored = [&](LayoutID declarer) {
            noteUsedInProduct(definition, declarer, key);
        };
        if (!allocated)
            forEachDeclarerOfFieldOf(receiver, key, noteStored);
        else if (std::optional<FieldReference> field = findField(m_program, allocated, key))
            noteStored(declarationOf(m_program, *field).layout);
    }
    bool isNoInt32 = opcode == Opcode::Double || opcode == Opcode::Div || opcode == Opcode::Pow;
    if (opcode == Opcode::Integer) {
        int64_t integer = definition.as<Opcode::Integer>().integer();
        isNoInt32 = integer < minInlineInt32 || integer > INT32_MAX;
    }
    if (!isNoInt32)
        return;
    bool isKnownWhere = false;
    auto note = [&](LayoutID declarer) {
        m_namesOfNumbers.noteStoredWhatIsNoInt32(declarer, key);
        isKnownWhere = true;
    };
    if (allocated) {
        if (std::optional<FieldReference> field = findField(m_program, allocated, key))
            note(declarationOf(m_program, *field).layout);
    } else
        forEachDeclarerOfFieldOf(receiver, key, note);
    if (!isKnownWhere)
        m_namesOfNumbers.noteStoredWhatIsNoInt32(key);
}

void BodyLookOfProgramFacts::noteWhatIsToldOfNumbers(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (allBitwiseOperators.contains(opcode)) {
        bool truncates = isTruncation(value);
        value.forEachOperand([&](ValueID operand, uint32_t) {
            noteReadUsedAsInteger(operand, truncates);
        });
        return;
    }
    bool isFraction = computesFraction(value);
    if (isFraction)
        noteReadsInProduct(value);
    if (isFraction || operatorsOnNumbersAloneInProgramFacts.contains(opcode)) {
        value.forEachOperand([&](ValueID operand, uint32_t) {
            noteReadUsedForArithmetic(operand);
        });
        return;
    }
    if (accessesUnderComputedKeysInProgramFacts.contains(opcode)) {
        noteReadUsedAsInteger(value.operand(1));
        return;
    }
    if (opcode == Opcode::PropSet) {
        noteWhatIsStoredUnder(value.as<Opcode::PropSet>().objectOperand(), { }, value.as<Opcode::PropSet>().key(), value.as<Opcode::PropSet>().valueOperand());
        return;
    }
    if (opcode == Opcode::PropDefine) {
        noteWhatIsStoredUnder(value.as<Opcode::PropDefine>().objectOperand(), { }, value.as<Opcode::PropDefine>().key(), value.as<Opcode::PropDefine>().valueOperand());
        return;
    }
    if (opcode == Opcode::NewObject) {
        auto allocation = value.as<Opcode::NewObject>();
        if (!allocation.layout())
            return;
        const Layout& layout = m_program.at(allocation.layout());
        std::span<const ValueID> values = value.variadicOperands();
        size_t place = 0;
        for (uint32_t field : allocation.fields()) {
            if (place < values.size() && field < layout.fields.size())
                noteWhatIsStoredUnder({ }, allocation.layout(), layout.fields[field].key, values[place]);
            ++place;
        }
        return;
    }

    if (ValueID callee = m_index.stripCasts(value.as<Opcode::Call>().calleeOperand()); callee && !m_index.isParameter(callee) && m_index.opcodeOf(callee) == Opcode::Builtin) {
        BuiltinID member = m_builtins.canonicalID(m_index.value(callee).as<Opcode::Builtin>().builtin());
        if (BuiltinID holder = m_builtins.at(member).holder; holder && m_builtins.canonicalID(holder) == m_builtins.canonicalID(Builtins::Math)) {
            for (ValueID argument : value.variadicOperands())
                noteReadUsedForArithmetic(argument);
        }
        return;
    }

    PropertyKeyID calleeKey = value.as<Opcode::Call>().calleeKey();
    std::span<const ValueID> arguments = value.variadicOperands();
    if (!calleeKey || arguments.empty() || !m_program.builtinNameOf(calleeKey))
        return;
    if (ValueID callback = m_index.stripCasts(arguments[0]); callback && !m_index.isParameter(callback) && m_index.opcodeOf(callback) == Opcode::NewClosure && findOwnMember(m_program, m_builtins.canonicalID(Builtins::ArrayPrototype), calleeKey))
        m_namesOfNumbers.noteIterationCallback(m_index.value(callback).function());
    bool passesRead = std::ranges::any_of(arguments, [&](ValueID argument) {
        ValueID behindCasts = argument ? m_index.stripCasts(argument) : ValueID();
        return behindCasts && !m_index.isParameter(behindCasts) && (m_index.opcodeOf(behindCasts) == Opcode::PropGet || allSums.contains(m_index.opcodeOf(behindCasts)));
    });
    if (!passesRead)
        return;
    for (BuiltinID holder : { Builtins::StringPrototype, Builtins::ArrayPrototype }) {
        BuiltinID member = findOwnMember(m_program, m_builtins.canonicalID(holder), calleeKey);
        if (!member)
            continue;
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(member))) {
            if (overload.mode != BuiltinCallMode::Call)
                continue;
            for (const BuiltinParameter& parameter : overload.parameters) {
                bool convertsToInteger = parameter.conversion == BuiltinConversion::ToIntegerOrInfinity || parameter.conversion == BuiltinConversion::ToLength || parameter.conversion == BuiltinConversion::ToIndex
                    || parameter.conversion == BuiltinConversion::ToInt32 || parameter.conversion == BuiltinConversion::ToUint32;
                if (convertsToInteger && parameter.operand.kind() == BuiltinOperand::Kind::Argument && parameter.operand.argumentIndex() < arguments.size())
                    noteReadUsedAsInteger(arguments[parameter.operand.argumentIndex()]);
            }
        }
    }
}
void BodyLookOfProgramFacts::noteBuiltinsSpelledIn(StringID id, bool standsInData, const BodyValue& value)
{
    if (m_rows.stringsWithoutWordOfFilter.at(id.index()))
        return;
    std::span<const uint8_t> text = m_program.string(id);
    bool holdsWordOfFilter = false;
    forEachWordInProgramFacts(text, [&](std::span<const uint8_t> word) {
        if (!m_rows.filterOfWords.quickGet(placeInFilterOfWordsInProgramFacts(word)))
            return;
        holdsWordOfFilter = true;
        for (const ModuleSpelledInProgramFacts& spelled : m_rows.modulesOfLibrary) {
            if (standsAsSpecifierInProgramFacts(text, word, spelled.specifier))
                note(Finding::Kind::NamedModule, 1, spelled.module.index(), witnessAt(value));
        }
        bool isKeyInList = standsInData && word.size() == text.size();
        for (const BuiltinSpelledInProgramFacts& spelled : m_rows.builtinsKeptOnlyWhenNamed) {
            if ((isKeyInList && !spelled.isMethod) || !std::ranges::equal(spelled.text, word) || m_builtinsSpelledInTexts.contains(spelled.builtin))
                continue;
            m_builtinsSpelledInTexts.append(spelled.builtin);
            note(Finding::Kind::BuiltinSpelledInText, 0, spelled.builtin.index(), witnessAt(value));
        }
    });
    if (!holdsWordOfFilter)
        m_rows.stringsWithoutWordOfFilter.set(id.index(), 1);
}

void BodyLookOfProgramFacts::noteBuiltinsSpelledIn(const DataNode& node, const BodyValue& value)
{
    if (const auto* text = node.tryAs<DataNode::String>())
        noteBuiltinsSpelledIn(text->value, true, value);
    else if (const auto* array = node.tryAs<DataNode::Array>()) {
        for (const DataNode& element : array->elements)
            noteBuiltinsSpelledIn(element, value);
    } else if (const auto* object = node.tryAs<DataNode::Object>()) {
        for (const DataMember& member : object->members) {
            noteModuleSpelledBy(member.key, value);
            noteBuiltinsSpelledIn(member.value, value);
        }
    }
}

void BodyLookOfProgramFacts::noteModuleSpelledBy(PropertyKeyID id, const BodyValue& value)
{
    if (!id || m_rows.modulesOfLibrary.isEmpty())
        return;
    const PropertyKey& key = m_program.at(id);
    if (key.kind != PropertyKeyKind::Text)
        return;
    std::span<const uint8_t> text = m_program.string(key.text);
    if (std::ranges::all_of(text, isASCIIAlphanumeric<uint8_t>))
        return;
    for (const ModuleSpelledInProgramFacts& spelled : m_rows.modulesOfLibrary) {
        if (std::ranges::equal(text, spelled.specifier))
            note(Finding::Kind::NamedModule, 1, spelled.module.index(), witnessAt(value));
    }
}

ALWAYS_INLINE void BodyLookOfProgramFacts::look(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    using Step = StepOfLookInProgramFacts;
    const uint32_t steps = stepsOf(opcode);
    if (steps & Step::isText)
        noteBuiltinsSpelledIn(value.as<Opcode::String>().string(), false, value);
    if (steps & Step::isData) {
        if (DataID data = value.as<Opcode::Data>().data())
            noteBuiltinsSpelledIn(m_program.at(data).root, value);
    }
    if (steps & Step::definesUnderName)
        noteModuleSpelledBy(value.as<Opcode::PropDefine>().key(), value);
    if (steps & Step::storesUnderName)
        noteModuleSpelledBy(value.as<Opcode::PropSet>().key(), value);
    if ((steps & Step::makesObject) && !m_rows.modulesOfLibrary.isEmpty()) {
        auto allocation = value.as<Opcode::NewObject>();
        if (allocation.layout()) {
            const Layout& layout = m_program.at(allocation.layout());
            for (uint32_t field : allocation.fields()) {
                if (field < layout.fields.size())
                    noteModuleSpelledBy(layout.fields[field].key, value);
            }
        }
    }
    if (steps & Step::tellsOfNumbers)
        noteWhatIsToldOfNumbers(value);
    if (steps & Step::loadsOrEvaluates)
        lookAtLoad(value);
    if ((steps & Step::readsUnderName) && (m_builtinOfValue.isEmpty() || !builtinAt(value.operand(0)))) {
        PropertyKeyID key = value.as<Opcode::PropGet>().key();
        if (key && m_program.builtinNameOf(key) && !m_namesReadOffUnknownValues.contains(key)) {
            m_namesReadOffUnknownValues.append(key);
            note(Finding::Kind::NameReadOffUnknownValue, 0, key.index(), witnessAt(value));
        }
    }
    if ((steps & Step::callsByValue) && !m_index.isParameter(value.operand(0))) {
        BodyValue callee = m_index.value(value.operand(0));
        if (callee.opcode() == Opcode::ElemGet) {
            BuiltinClassID classOfReceiver = m_index.isParameter(callee.operand(0)) ? BuiltinClassID() : classOfWrittenPrimitiveInProgramFacts(m_index.value(callee.operand(0)).opcode());
            note(Finding::Kind::CallUnderComputedKey, 0, classOfReceiver ? classOfReceiver.index() : Wire::none, witnessAt(callee));
        }
    }
    if (steps & Step::readsModuleVariable) {
        if (BuiltinID held = builtinHeldBy(value.as<Opcode::GetModuleVariable>().variable())) {
            if (m_builtinOfValue.isEmpty())
                m_builtinOfValue.fill(BuiltinID(), m_index.numberOfValues());
            nameBuiltin(value, held);
        }
        return;
    }
    if (m_builtinOfValue.isEmpty())
        return;

    if (namersOfBuiltinsInProgramFacts.contains(opcode)) {
        BuiltinID builtin;
        if (opcode == Opcode::Builtin)
            builtin = value.as<Opcode::Builtin>().builtin();
        else if (opcode == Opcode::ModuleBuiltin)
            builtin = value.as<Opcode::ModuleBuiltin>().builtin();
        else if (opcode == Opcode::NativeBinding)
            builtin = value.as<Opcode::NativeBinding>().builtin();
        else if (RuntimeModuleID module = value.as<Opcode::RequireBuiltinModule>().runtimeModule())
            builtin = m_builtins.namespaceOf(module);
        nameBuiltin(value, builtin);
        return;
    }

    uint32_t operandAccountedFor = UINT32_MAX;
    if (inputPassThroughOpcodes.contains(opcode)) {
        handOn(value.operand(0), value.id());
        operandAccountedFor = 0;
    } else if (accessesUnderConstantKeysInProgramFacts.contains(opcode)) {
        if (BuiltinID holder = builtinAt(value.operand(0)))
            accessMember(value, holder);
        else if (BuiltinID objectOfClass = classOfPartAt(value.operand(0)))
            accessMemberOfPartOfClass(value, objectOfClass);
        else if (BuiltinID holder = holderOfMethodServed(value.operand(0)))
            note(Finding::Kind::LightFormIsAskedForMore, 0, holder.index(), witnessAt(value));
        operandAccountedFor = 0;
    } else if (BuiltinID holder = symbolKeyOfAccess(value) ? builtinAt(value.operand(0)) : BuiltinID()) {
        accessMember(value, holder);
        operandAccountedFor = 0;
    } else if (callsByValueInProgramFacts.contains(opcode))
        lookAtCall(value);

    auto lookAtOperandThatIsFollowed = [&](ValueID operand, uint32_t operandIndex) NEVER_INLINE {
        Fate fate = fateOf(value, operandIndex);
        noteFate(operand, fate, value);
        if (fate == Fate::IsInspected && lightFormAt(operand)) {
            bool asksForMore = callsByValueInProgramFacts.contains(opcode) ? !holderOfMethodServed(value.operand(0)) : opcode == Opcode::ElemHas || !operandInfoOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).protocols.isEmpty();
            if (asksForMore)
                note(Finding::Kind::LightFormIsAskedForMore, 0, builtinAt(operand).index(), witnessAt(value));
            return;
        }
        if ((fate == Fate::IsInspected || fate == Fate::IsWrittenOn) && builtinAt(operand) && (opcode == Opcode::ElemHas || fate == Fate::IsWrittenOn || mayGoThroughOwnKeysOf(value, operandIndex)))
            note(Finding::Kind::KeysOfBuiltinListed, 0, builtinAt(operand).index(), witnessAt(value));
        if ((fate == Fate::IsLetGo || fate == Fate::IsWrittenOn) && builtinAt(operand) && accessClassOf(opcode) == AccessClass::ByName && receiverOf(opcode) == operandIndex)
            noteWrite(builtinAt(operand), { }, value, opcode == Opcode::ElemDelete ? writeOnlyDeletesInProgramFacts : 0);
        if ((fate == Fate::IsWrittenOn || fate == Fate::IsLeftToLink) && builtinAt(operand) && callsByValueInProgramFacts.contains(opcode) && !builtinAt(value.id())) {
            m_builtinOfValue[value.id().index()] = builtinAt(operand);
            m_mayBeSomethingElse.set(value.id().index());
        }
    };
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) ALWAYS_INLINE_LAMBDA {
        if (operandIndex != operandAccountedFor && isFollowed(operand))
            lookAtOperandThatIsFollowed(operand, operandIndex);
    });
}

ALWAYS_INLINE void BodyLookOfProgramFacts::lookAtEnd(const BodyBlock& block)
{
    BodyTerminator terminator = block.terminator();
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!isFollowed(operand))
            return;
        bool isInspected = (terminator.operandUses(operandIndex) - OperandUses { OperandUse::Inspects }).isEmpty();
        noteFate(operand, isInspected ? Fate::IsInspected : Fate::IsLetGo, terminator);
    });
    block.forEachOutgoingEdge([&](BodyTarget target, bool isHandlerEdge) ALWAYS_INLINE_LAMBDA {
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t position = 0; position < arguments.size(); ++position) {
            if (isFollowed(arguments[position]) && (isHandlerEdge || !passOn(arguments[position], m_index.block(target.block()).parameter(position), block)))
                noteFate(arguments[position], Fate::IsLetGo, terminator);
        }
    });
    m_index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) ALWAYS_INLINE_LAMBDA {
        if (!isFollowed(result.subject))
            return;
        if (result.test && !result.isOnTakenEdge && m_index.opcodeOf(result.test) == Opcode::ToBoolean && !originOfURL(result.subject))
            return;
        if (result.parameter.index() >= block.endOfValues().index() && m_index.onlyIncomingEdge(m_index.blockOf(result.parameter)))
            handOn(result.subject, result.parameter);
        else
            noteFate(result.subject, Fate::IsLetGo, terminator);
    });
}

void BodyLookOfProgramFacts::run()
{
    bool namesBuiltins = m_index.holdsAnyOf(giversOfBuiltinsInProgramFacts);
    bool mayReadNameOffUnknownValue = m_isOfProgram && m_index.holdsAnyOf(OpcodeSet { Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::NewObject });
    bool maySpellBuiltins = m_isOfProgram && m_index.holdsAnyOf(OpcodeSet { Opcode::String, Opcode::Data });
    bool mayReadBuiltinOutOfVariable = !m_builtinsHeldByVariables.empty() && m_index.holdsAnyOf(OpcodeSet { Opcode::GetModuleVariable });
    if (!namesBuiltins && !mayReadNameOffUnknownValue && !maySpellBuiltins && !mayReadBuiltinOutOfVariable && !m_index.holdsAnyOf(loadsAndEvaluationsInProgramFacts))
        return;
    if (namesBuiltins)
        m_builtinOfValue.fill(BuiltinID(), m_index.numberOfValues());
    for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
        if (!m_blocksNeverEntered.empty() && m_blocksNeverEntered.front().index() == number) {
            m_blocksNeverEntered = m_blocksNeverEntered.subspan(1);
            continue;
        }
        BodyBlock block = m_index.block(BlockID(number));
        for (BodyValue value : block.values()) {
            if (stepsOf(value.opcode()) || !m_builtinOfValue.isEmpty())
                look(value);
        }
        if (!m_builtinOfValue.isEmpty())
            lookAtEnd(block);
    }
}

class LooksAtBodies {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LooksAtBodies);
public:
    explicit LooksAtBodies(const BodyIndexTable& bodies)
        : bodies(bodies)
    {
    }

    std::span<const FindingOfProgramFacts> of(FunctionID id) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(isFilled);
        size_t function = id.index();
        if (function + 1 >= firstFindingOfFunction.size())
            return { };
        return findings.span().subspan(firstFindingOfFunction[function], firstFindingOfFunction[function + 1] - firstFindingOfFunction[function]);
    }

    void prefetchPlaceOfFindingsOf(FunctionID id) const { __builtin_prefetch(&firstFindingOfFunction[id.index()]); }
    void prefetchFindingsOf(FunctionID id) const
    {
        static constexpr size_t bytesOfLine = 64;
        std::span<const FindingOfProgramFacts> found = of(id);
        const char* address = reinterpret_cast<const char*>(found.data());
        for (const char* end = address + found.size_bytes(); address < end; address += bytesOfLine)
            __builtin_prefetch(address);
    }

    const BodyIndexTable& bodies;
    Vector<uint32_t> firstFindingOfFunction;
    LargeList<FindingOfProgramFacts> findings;
    FieldsOfNumbers fieldsReadAsIntegers;
    NamesOfModules namesReadAsIntegers;
    FieldsOfNumbers fieldsReadForArithmetic;
    FieldsOfNumbers fieldsStoredWhatIsNoInt32;
    FieldsOfNumbers fieldsUsedInProducts;
    Vector<FieldUsedOutsideLoopsInProgramFacts> fieldsUsedOutsideLoops;
    Vector<FunctionID> iterationCallbacks;
    Vector<VariableStoredInFieldInProgramFacts> variablesStoredInFields;
    BitVector keysStoredWhatIsNoInt32;
    bool isFilled { false };
};

const FieldsOfNumbers& fieldsReadAsIntegersIn(const LooksAtBodies& looks)
{
    RELEASE_ASSERT(looks.isFilled);
    return looks.fieldsReadAsIntegers;
}

const NamesOfModules& namesReadAsIntegersIn(const LooksAtBodies& looks)
{
    RELEASE_ASSERT(looks.isFilled);
    return looks.namesReadAsIntegers;
}

const FieldsOfNumbers& fieldsReadForArithmeticIn(const LooksAtBodies& looks)
{
    RELEASE_ASSERT(looks.isFilled);
    return looks.fieldsReadForArithmetic;
}

const FieldsOfNumbers& fieldsStoredWhatIsNoInt32In(const LooksAtBodies& looks)
{
    RELEASE_ASSERT(looks.isFilled);
    return looks.fieldsStoredWhatIsNoInt32;
}

BitVector functionsCalledInLoopsIn(const LooksAtBodies& looks, const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, BitVector& keysCalledInLoops, bool& elementsAreCalledInLoops)
{
    RELEASE_ASSERT(looks.isFilled);
    BitVector functions(program.functions.size());
    BitVector& methodsCalled = keysCalledInLoops;
    for (FunctionID callback : looks.iterationCallbacks)
        functions.quickSet(callback.index());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID caller(index);
        const auto& summary = summaries.at(caller);
        if (!linked.function(caller).isReached)
            continue;
        bool callsItself = std::ranges::any_of(summary.calls, [&](const SummaryCall& call) {
            return call.callee == SummaryCall::Callee::Binding && call.function == caller;
        });
        if (!callsItself && !summary.has(SummaryTrait::HasLoops))
            continue;
        for (const SummaryCall& call : summary.calls) {
            if (!callsItself && !call.isInLoop)
                continue;
            FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : linked.calleeAt(caller, call.site);
            if (!callee)
                callee = held.calleeAt(caller, call.site);
            if (callee)
                functions.quickSet(callee.index());
            else if (call.callee == SummaryCall::Callee::Name && call.key)
                methodsCalled.set(call.key.index());
            else if (call.callee == SummaryCall::Callee::Value && call.subject != noSummaryNode && summary.nodes[call.subject].kind == SummaryNode::Kind::ResultOfAccess)
                elementsAreCalledInLoops |= !summary.accesses[summary.nodes[call.subject].first].key;
        }
    }
    for (const LinkedMember& member : linked.members) {
        if (!member.key || !methodsCalled.get(member.key.index()))
            continue;
        for (FunctionID method : { member.function, member.written, member.replacement }) {
            if (method)
                functions.quickSet(method.index());
        }
    }
    return functions;
}

BitVector functionsCalledInLoopsOfOwnFileIn(const LooksAtBodies& looks, const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held)
{
    RELEASE_ASSERT(looks.isFilled);
    BitVector functions(program.functions.size());
    for (FunctionID callback : looks.iterationCallbacks)
        functions.quickSet(callback.index());
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> methodsCalled;
    auto methodOfFile = [](ModuleID file, PropertyKeyID key) {
        return static_cast<uint64_t>(file.index()) << 32 | key.bits();
    };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID caller(index);
        const ThinSummary& summary = summaries.at(caller);
        ModuleID file = program.at(caller).module;
        if (!file || !linked.function(caller).isReached || !summary.has(SummaryTrait::HasLoops))
            continue;
        for (const SummaryCall& call : summary.calls) {
            if (!call.isInLoop)
                continue;
            FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : linked.calleeAt(caller, call.site);
            if (!callee)
                callee = held.calleeAt(caller, call.site);
            if (callee && program.at(callee).module == file)
                functions.quickSet(callee.index());
            else if (!callee && call.callee == SummaryCall::Callee::Name && call.key)
                methodsCalled.add(methodOfFile(file, call.key));
        }
    }
    for (const LinkedMember& member : linked.members) {
        for (FunctionID method : { member.function, member.written, member.replacement }) {
            ModuleID file = method && member.key ? program.at(method).module : ModuleID();
            if (file && methodsCalled.contains(methodOfFile(file, member.key)))
                functions.quickSet(method.index());
        }
    }
    return functions;
}

BitVector functionsInElementsIn(const Program& program, const ThinSummaries& summaries, const LinkTables& linked)
{
    BitVector functions(program.functions.size());
    if (!linked.elementsAreCalledInLoops)
        return functions;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID holder(index);
        if (!linked.function(holder).isReached || isLibraryCode(program, program.at(holder)))
            continue;
        const ThinSummary& summary = summaries.at(holder);
        auto functionAt = [&](uint32_t node) -> FunctionID {
            return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Closure ? FunctionID(summary.nodes[node].first) : FunctionID();
        };
        for (const SummaryEdge& edge : summary.edges) {
            FunctionID function = functionAt(edge.from);
            if (function && summary.nodes[edge.to].kind == SummaryNode::Kind::Contents)
                functions.quickSet(function.index());
        }
    }
    return functions;
}

BitVector constructorsCalledInLoopsIn(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held)
{
    BitVector constructors(program.functions.size());
    Vector<FunctionID> waiting;
    for (size_t function : linked.functionsCalledInLoops)
        waiting.append(FunctionID(static_cast<uint32_t>(function)));
    for (size_t function : linked.functionsDoneInIterations) {
        if (!linked.functionsCalledInLoops.quickGet(function))
            waiting.append(FunctionID(static_cast<uint32_t>(function)));
    }
    while (!waiting.isEmpty()) {
        FunctionID caller = waiting.takeLast();
        if (program.at(caller).kind == FunctionKind::BaseConstructor)
            constructors.quickSet(caller.index());
        for (const SummaryCall& call : summaries.at(caller).calls) {
            if (!call.isConstruction)
                continue;
            FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : linked.calleeAt(caller, call.site);
            if (!callee)
                callee = held.calleeAt(caller, call.site);
            if (callee && program.at(callee).kind == FunctionKind::BaseConstructor && !constructors.quickSet(callee.index()))
                waiting.append(callee);
        }
    }
    return constructors;
}

BitVector functionsCalledByValueInLoopsIn(const Program& program, const ThinSummaries& summaries, const LinkTables& linked)
{
    BitVector functions(program.functions.size());
    auto isCalledUnder = [&](PropertyKeyID key) {
        return key ? linked.keysCalledInLoops.get(key.index()) : linked.elementsAreCalledInLoops;
    };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID holder(index);
        if (!linked.function(holder).isReached)
            continue;
        const ThinSummary& summary = summaries.at(holder);
        bool makesClosures = std::ranges::any_of(summary.nodes, [](const SummaryNode& node) { return node.kind == SummaryNode::Kind::Closure; });
        if (!makesClosures)
            continue;
        auto isClosure = [&](uint32_t node) {
            return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Closure;
        };
        for (const SummaryAccess& access : summary.accesses) {
            if (access.laysValueInSightUnderName() && isClosure(access.value) && isCalledUnder(access.key))
                functions.quickSet(summary.nodes[access.value].first);
        }
        for (const SummaryEdge& edge : summary.edges) {
            if (!isClosure(edge.from))
                continue;
            const SummaryNode& to = summary.nodes[edge.to];
            bool isInArray = to.kind == SummaryNode::Kind::Contents && linked.elementsAreCalledInLoops;
            bool isInField = to.kind == SummaryNode::Kind::Field && to.first < program.layouts.size() && to.second < program.layouts[to.first].fields.size() && isCalledUnder(program.layouts[to.first].fields[to.second].key);
            if (isInArray || isInField)
                functions.quickSet(summary.nodes[edge.from].first);
        }
    }
    return functions;
}

BitVector functionsDoneInIterationsIn(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held)
{
    HashMap<uint64_t, Vector<FunctionID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> methodsOfFiles;
    auto methodOfFile = [](ModuleID file, PropertyKeyID key) {
        return static_cast<uint64_t>(file.index()) << 32 | key.bits();
    };
    auto noteMethod = [&](PropertyKeyID key, FunctionID method) {
        if (ModuleID file = key && method ? program.at(method).module : ModuleID())
            methodsOfFiles.add(methodOfFile(file, key), Vector<FunctionID, 1>()).iterator->value.appendIfNotContains(method);
    };
    for (const LinkedMember& member : linked.members) {
        for (FunctionID method : { member.function, member.written, member.replacement })
            noteMethod(member.key, method);
    }
    for (const LinkedAssignment& assignment : held.lastAssignments)
        noteMethod(assignment.key, assignment.method);
    BitVector functions = linked.functionsCalledInLoopsOfOwnFile;
    Vector<FunctionID> waiting;
    auto add = [&](FunctionID function) {
        if (!functions.quickSet(function.index()))
            waiting.append(function);
    };
    auto addCallees = [&](FunctionID caller, bool isDoneInIterations) {
        ModuleID file = program.at(caller).module;
        if (!file)
            return;
        for (const SummaryCall& call : summaries.at(caller).calls) {
            if (!isDoneInIterations && !call.isInLoop)
                continue;
            if (call.callee == SummaryCall::Callee::Binding) {
                if (call.function && program.at(call.function).module == file)
                    add(call.function);
                continue;
            }
            if (call.callee != SummaryCall::Callee::Name || !call.key)
                continue;
            auto methods = methodsOfFiles.find(methodOfFile(file, call.key));
            if (methods != methodsOfFiles.end()) {
                for (FunctionID method : methods->value)
                    add(method);
            }
        }
    };
    for (size_t function : linked.functionsCalledInLoopsOfOwnFile)
        waiting.append(FunctionID(static_cast<uint32_t>(function)));
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (linked.function(FunctionID(index)).isReached && summaries.at(FunctionID(index)).has(SummaryTrait::HasLoops))
            addCallees(FunctionID(index), false);
    }
    while (!waiting.isEmpty())
        addCallees(waiting.takeLast(), true);
    return functions;
}

void markLiteralsStoredByCallees(ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held)
{
    ThinSummaries::ElementStores stores = summaries.takeElementStores();
    auto calleeAt = [&](FunctionID caller, ValueID site) {
        FunctionID callee = linked.calleeAt(caller, site);
        return callee ? callee : held.calleeAt(caller, site);
    };
    auto order = [](std::pair<FunctionID, uint32_t> parameter) {
        return std::pair { parameter.first.index(), parameter.second };
    };
    std::ranges::sort(stores.parameters, { }, order);
    for (const auto& passed : stores.literalArguments) {
        FunctionID callee = calleeAt(passed.caller, passed.site);
        if (callee && std::ranges::binary_search(stores.parameters, order({ callee, passed.argument }), { }, order))
            summaries.noteStoredByElement(passed.literal);
    }
    if (stores.literalResults.isEmpty())
        return;
    BitVector functions;
    for (auto [caller, site] : stores.results) {
        if (FunctionID callee = calleeAt(caller, site))
            functions.set(callee.index());
    }
    for (auto [function, literal] : stores.literalResults) {
        if (functions.get(function.index()))
            summaries.noteStoredByElement(literal);
    }
}

FieldsOfNumbers fieldsUsedInProductsIn(const LooksAtBodies& looks, const BitVector& functionsCalledInLoops)
{
    RELEASE_ASSERT(looks.isFilled);
    FieldsOfNumbers fields = looks.fieldsUsedInProducts;
    for (const FieldUsedOutsideLoopsInProgramFacts& use : looks.fieldsUsedOutsideLoops) {
        if (functionsCalledInLoops.quickGet(use.function.index()))
            fields.add(use.declarer, use.key);
    }
    return fields;
}

FieldsOfNumbers fieldsStoredCountsIn(const LooksAtBodies& looks, const Program& program, const LinkTables& linked)
{
    RELEASE_ASSERT(looks.isFilled);
    FieldsOfNumbers counts;
    for (const VariableStoredInFieldInProgramFacts& store : looks.variablesStoredInFields) {
        const LinkedCell* cell = store.ofModule ? &linked.variables[store.ofModule.index()] : linked.findCellOfField(store.ofEnvironment);
        if (!cell || !cell->proven)
            continue;
        Type proven = program.types.at(cell->proven);
        if (proven.primitives & Primitive::NumberWithoutFraction && proven.hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::UndefinedOrNull | Primitive::Absent))
            counts.add(store.declarer, store.key);
    }
    return counts;
}

const BitVector& keysStoredWhatIsNoInt32In(const LooksAtBodies& looks)
{
    RELEASE_ASSERT(looks.isFilled);
    return looks.keysStoredWhatIsNoInt32;
}
void DeleterOfLooksAtBodies::operator()(LooksAtBodies* looks) const
{
    delete looks;
}

const BodyIndexTable& bodiesLookedAt(const LooksAtBodies& looks)
{
    return looks.bodies;
}

static RefusedCode codeRefusedInProgramFacts(const Program& program, BuildContext& build)
{
    const AOT::CompileOptions& options = build.options();
    RefusedCode refused = refusedCodeOf(program);
    refused.evaluatedText |= options.excludesEvaluatedText();
    refused.hostEvaluatedText |= options.excludesHostEvaluatedText();
    refused.codeOutsideImage |= !options.admitsUnseenCode() && options.excludesCodeOutsideImage();
    return refused;
}

static BuiltinID builtinWrittenOutAtInProgramFacts(const Program& program, const RowsOfProgramFacts& rows, const BodyIndex& index, ValueID value, bool& beginsAtWhatNobodyReplaces)
{
    const BuiltinTable& builtins = program.builtins();
    ValueID written = value ? index.stripCasts(value) : ValueID();
    while (written && index.isParameter(written)) {
        ValueID narrowed = index.valueNarrowedInto(written);
        written = narrowed ? index.stripCasts(narrowed) : ValueID();
    }
    if (!written)
        return { };
    BodyValue definition = index.value(written);
    if (definition.opcode() == Opcode::Builtin) {
        auto named = definition.as<Opcode::Builtin>();
        const Builtin& entry = builtins.at(named.builtin());
        beginsAtWhatNobodyReplaces = named.isNotReadFromHolder() || entry.mutability == BuiltinMutability::AlwaysLocked || builtins.isLocationOfLockedObject(named.builtin()) || !entry.holder;
        return named.builtin();
    }
    if (definition.opcode() == Opcode::RequireModule || definition.opcode() == Opcode::RequireBuiltinModule) {
        RuntimeModuleID required;
        if (definition.opcode() == Opcode::RequireBuiltinModule)
            required = definition.as<Opcode::RequireBuiltinModule>().runtimeModule();
        else if (auto require = definition.as<Opcode::RequireModule>(); require.module() && !require.onlyIfImported())
            required = program.at(require.module()).runtimeModule;
        beginsAtWhatNobodyReplaces = false;
        return required ? builtins.namespaceOf(required) : BuiltinID();
    }
    if (definition.opcode() != Opcode::PropGet)
        return { };
    auto read = definition.as<Opcode::PropGet>();
    BuiltinID holder = builtinWrittenOutAtInProgramFacts(program, rows, index, read.objectOperand(), beginsAtWhatNobodyReplaces);
    if (!holder || !read.key() || rows.lightFormOf(builtins, builtins.canonicalID(holder)))
        return { };
    BuiltinID member = memberFoundOnInProgramFacts(program, holder, read.key());
    return member && standsForObjectInProgramFacts(builtins, builtins.canonicalID(member)) ? member : BuiltinID();
}

static BuiltinsHeldByVariablesInProgramFacts builtinsHeldByVariablesStoredOnceInProgramFacts(const Program& program, const RowsOfProgramFacts& rows, const BodyIndexTable& bodies)
{
    BuiltinsHeldByVariablesInProgramFacts table;
    Vector<BuiltinID>& held = table.held;
    BitVector isStored;
    for (uint32_t number = 0; number < program.modules.size(); ++number) {
        const Module& module = program.modules[number];
        const BodyIndex* index = module.body ? bodies.find(module.body) : nullptr;
        if (!index || !index->holdsAnyOf(OpcodeSet { Opcode::PutModuleVariable }) || !index->holdsAnyOf(OpcodeSet { Opcode::Builtin, Opcode::RequireModule, Opcode::RequireBuiltinModule }))
            continue;
        for (uint32_t place = 0; place < index->numberOfValues(); ++place) {
            if (index->isParameter(ValueID(place)) || index->opcodeOf(ValueID(place)) != Opcode::PutModuleVariable)
                continue;
            auto store = index->value(ValueID(place)).as<Opcode::PutModuleVariable>();
            const Variable& variable = program.at(store.variable());
            if (variable.owner || (variable.module != ModuleID(number)) || (!variable.flags.contains(VariableFlags::IsImmutable)) || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
                continue;
            bool isStoredAgain = isStored.set(store.variable().index());
            bool beginsAtWhatNobodyReplaces = false;
            BuiltinID stored = isStoredAgain ? BuiltinID() : builtinWrittenOutAtInProgramFacts(program, rows, *index, store.valueOperand(), beginsAtWhatNobodyReplaces);
            if (!stored && held.isEmpty())
                continue;
            if (held.isEmpty())
                held.fill(BuiltinID(), program.variables.size());
            held[store.variable().index()] = stored;
            table.areFollowedByLinkAsTheObject.set(store.variable().index(), stored && beginsAtWhatNobodyReplaces);
        }
    }
    if (held.isEmpty())
        return table;
    auto takeOffTable = [&](VariableID variable) {
        if (!variable)
            return;
        held[variable.index()] = { };
        table.areFollowedByLinkAsTheObject.clear(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& exported : module.exports)
            takeOffTable(exported.variable);
        for (const SyntheticExport& exported : module.syntheticExports)
            takeOffTable(exported.variable);
        takeOffTable(module.exportsVariable);
    }
    return table;
}

OwnedLooksAtBodies makeEmptyLooksAtBodies(const BodyIndexTable& bodies)
{
    return OwnedLooksAtBodies(new LooksAtBodies(bodies));
}

void lookAtBodies(LooksAtBodies& looks, const Program& program, const ThinSummaries& summaries, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    RowsOfProgramFacts rows(program.builtins(), build.options().featuresRetainedByName);
    if (build.options().excludesFeaturesNamedAtRunTime())
        rows.listSpecifiersOfLibrary(program);
    rows.stringsWithoutWordOfFilter = BytesSharedByThreads(program.strings.size());
    RefusedCode refused = codeRefusedInProgramFacts(program, build);
    auto numberOfFunctions = static_cast<unsigned>(program.functions.size());

    const BodyIndexTable& bodies = looks.bodies;
    Vector<uint32_t>& firstFindingOfFunction = looks.firstFindingOfFunction;
    firstFindingOfFunction.grow(numberOfFunctions + 1);
    firstFindingOfFunction[0] = 0;
    Vector<std::unique_ptr<NamesOfNumbersInProgramFacts>> namesOfWorkers(loops.width());
    BuiltinsHeldByVariablesInProgramFacts builtinsHeldByVariables = builtinsHeldByVariablesStoredOnceInProgramFacts(program, rows, bodies);
    ChunksOfIndices division = divideIntoChunks(numberOfFunctions);
    Vector<Vector<FindingOfProgramFacts>> chunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned worker) {
        if (!namesOfWorkers[worker])
            namesOfWorkers[worker] = makeUniqueWithoutFastMallocCheck<NamesOfNumbersInProgramFacts>(program.types.numberOfPropertyKeys());
        Vector<FindingOfProgramFacts>& found = chunks[chunk];
        for (unsigned number = division.firstIndexOf[chunk]; number < division.firstIndexOf[chunk + 1]; ++number) {
            size_t numberFoundInFront = found.size();
            FunctionID id(number);
            if (const BodyIndex* index = bodies.find(id))
                BodyLookOfProgramFacts(program, rows, refused, id, *index, summaries, found, *namesOfWorkers[worker], builtinsHeldByVariables).run();
            firstFindingOfFunction[number + 1] = static_cast<uint32_t>(found.size() - numberFoundInFront);
        }
    });
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstFindingOfFunction[index + 1] += firstFindingOfFunction[index];

    Vector<size_t> firstFindingOfChunk;
    firstFindingOfChunk.reserveInitialCapacity(chunks.size());
    size_t numberOfFindings = 0;
    for (const Vector<FindingOfProgramFacts>& chunk : chunks) {
        firstFindingOfChunk.append(numberOfFindings);
        numberOfFindings += chunk.size();
    }
    RELEASE_ASSERT(numberOfFindings == firstFindingOfFunction.last());
    looks.findings.grow(loops, numberOfFindings);
    forEachIndexInParallel(loops, static_cast<unsigned>(chunks.size()), [&](unsigned chunk) {
        std::ranges::copy(chunks[chunk], looks.findings.mutableSpan().subspan(firstFindingOfChunk[chunk]).begin());
        chunks[chunk] = { };
    });

    NamesOfNumbersInProgramFacts namesOfNumbers(program.types.numberOfPropertyKeys());
    for (const auto& namesOfWorker : namesOfWorkers) {
        if (namesOfWorker)
            namesOfNumbers.merge(*namesOfWorker);
    }
    looks.fieldsReadAsIntegers = namesOfNumbers.takeFieldsReadAsIntegers();
    looks.namesReadAsIntegers = namesOfNumbers.takeNamesReadAsIntegers();
    looks.fieldsReadForArithmetic = namesOfNumbers.takeFieldsReadForArithmetic();
    looks.fieldsStoredWhatIsNoInt32 = namesOfNumbers.takeFieldsStoredWhatIsNoInt32();
    looks.fieldsUsedInProducts = namesOfNumbers.takeFieldsUsedInProducts();
    looks.fieldsUsedOutsideLoops = namesOfNumbers.takeFieldsUsedOutsideLoops();
    looks.iterationCallbacks = namesOfNumbers.takeIterationCallbacks();
    looks.keysStoredWhatIsNoInt32 = namesOfNumbers.takeKeysStoredWhatIsNoInt32();
    looks.variablesStoredInFields = namesOfNumbers.takeVariablesStoredInFields();
    looks.isFilled = true;
}

static std::span<const BuiltinFeatureUse> featureLinesWithGuardInProgramFacts(const BuiltinTable& builtins, BuiltinID builtin)
{
    for (BuiltinID guarded = builtin; guarded; guarded = builtins.at(guarded).holder) {
        std::span<const BuiltinFeatureUse> lines = builtins.featureUsesOf(builtins.canonicalID(guarded));
        if (std::ranges::any_of(lines, &BuiltinFeatureUse::isGuard))
            return lines;
        if (builtins.at(guarded).origin != BuiltinOrigin::Host)
            break;
    }
    return { };
}

bool hasFeatureGuard(const BuiltinTable& builtins, BuiltinID builtin)
{
    return std::ranges::any_of(featureLinesWithGuardInProgramFacts(builtins, builtin), [](const BuiltinFeatureUse& line) {
        return !line.condition && !line.overload;
    });
}

class FeaturesOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(FeaturesOfProgramFacts);
public:
    FeaturesOfProgramFacts(const InputsOfProgramFacts&, RefusedCode, const BitVector& builtinsWhoseOperandsNobodyKnows, const BitVector& builtinsFoundByNameAlone, const BitVector& builtinsThatCodeInSightHandsOn, bool unseenCodeMayRun);

    Vector<FeatureUseRecord> run(std::span<const BuiltinUseRecord>);

private:
    static constexpr OptionSet<BuiltinUse> usesThatNameNothing { BuiltinUse::Tested, BuiltinUse::Escaped };

    void collectUses(std::span<const BuiltinUseRecord>, bool namedAtRunTime);
    void visitRowsThatWait();
    void noteNamesOfHolders(BuiltinID);
    void noteCodeMayRun(BuiltinID, const Witness&);
    void noteInstancesAreMade(BuiltinClassID, const Witness&);
    void noteTypeIsMade(BuiltinTypeID, const Witness&);
    void noteTypesThatOverloadMakes(const BuiltinOverload&, const Witness&);
    void noteClassesThatFunctionsMake();
    void findClassesRootedByMakers();
    void visitRow(BuiltinID);
    void appendRecordsOf(BuiltinID);
    void sortRecordsAndMarkUnnamed();
    BuiltinID entryWithFeaturesOf(BuiltinClassID) const;
    BuiltinID outermostHolderWithFeature(BuiltinID, uint32_t feature) const;
    bool mayHold(const BuiltinCondition*, BuiltinID) const;
    bool isSeenToHold(const BuiltinCondition*, BuiltinID) const;
    bool runsFromTextOfLibrary(BuiltinID) const;
    bool needsWithoutCondition(BuiltinID, RemovableFeature) const;
    bool makersListFeature(BuiltinClassID, RemovableFeature) const;
    bool hasPlaceInStatement(BuiltinID canonical) const { return hasFunction(m_witnesses[canonical.index()]) || m_isBoundAtSite.get(canonical.index()); }
    bool isNamedByStatement(BuiltinID canonical) const { return m_isNamed.quickGet(canonical.index()) && (m_unseenCodeMayRun || hasPlaceInStatement(canonical)); }
    BuiltinClassID classWhoseMakersRoot(BuiltinID) const;

    const Program& m_program;
    const LinkTables& m_linked;
    const BuiltinTable& m_builtins;
    const RefusedCode m_refused;
    const BitVector& m_operandsNobodyKnows;
    const BitVector& m_isFoundByNameAlone;
    const BitVector& m_isHandedOnByCodeInSight;
    const bool m_admitsTextOfLibrary;
    const bool m_unseenCodeMayRun;
    Vector<BuiltinClassID> m_classOfPrototype;
    Vector<Witness> m_witnesses;
    BitVector m_isNamed;
    BitVector m_codeMayRun;
    BitVector m_codeMayRunWithoutNamesFromRunTime;
    Vector<BuiltinID> m_rowsToVisit;
    BitVector m_madeClasses;
    BitVector m_madeTypes;
    Vector<BuiltinClassID> m_classOfMember;
    BitVector m_makersNameFeatures;
    BitVector m_makersNameFeaturesOfConstructor;
    BitVector m_isLetGo;
    BitVector m_isBoundAtSite;
    Vector<FeatureUseRecord> m_records;
};

FeaturesOfProgramFacts::FeaturesOfProgramFacts(const InputsOfProgramFacts& inputs, RefusedCode refused, const BitVector& builtinsWhoseOperandsNobodyKnows, const BitVector& builtinsFoundByNameAlone, const BitVector& builtinsThatCodeInSightHandsOn, bool unseenCodeMayRun)
    : m_program(inputs.program)
    , m_linked(inputs.linked)
    , m_builtins(inputs.program.builtins())
    , m_refused(refused)
    , m_operandsNobodyKnows(builtinsWhoseOperandsNobodyKnows)
    , m_isFoundByNameAlone(builtinsFoundByNameAlone)
    , m_isHandedOnByCodeInSight(builtinsThatCodeInSightHandsOn)
    , m_admitsTextOfLibrary(!inputs.build.options().excludesLibraryText())
    , m_unseenCodeMayRun(unseenCodeMayRun)
    , m_isNamed(m_builtins.builtinCount())
    , m_codeMayRun(m_builtins.builtinCount())
    , m_madeClasses(m_builtins.classCount())
{
    m_witnesses.fill(noWitness, m_builtins.builtinCount());
    m_classOfPrototype.fill(BuiltinClassID(), m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
        if (BuiltinID prototype = m_builtins.at(BuiltinClassID(index)).prototype)
            m_classOfPrototype[m_builtins.canonicalID(prototype).index()] = BuiltinClassID(index);
    }
}

void FeaturesOfProgramFacts::noteNamesOfHolders(BuiltinID builtin)
{
    for (BuiltinID holder = m_builtins.at(builtin).holder; holder; holder = m_builtins.at(holder).holder) {
        holder = m_builtins.canonicalID(holder);
        m_isNamed.quickSet(holder.index());
    }
}

void FeaturesOfProgramFacts::noteCodeMayRun(BuiltinID builtin, const Witness& witness)
{
    if (!builtin)
        return;
    if (!hasFunction(m_witnesses[builtin.index()]))
        m_witnesses[builtin.index()] = witness;
    if (m_codeMayRun.quickSet(builtin.index()))
        return;
    m_rowsToVisit.append(builtin);
}

void FeaturesOfProgramFacts::collectUses(std::span<const BuiltinUseRecord> records, bool namedAtRunTime)
{
    for (const BuiltinUseRecord& record : records) {
        BuiltinID builtin = m_builtins.canonicalID(BuiltinID(record.builtin));
        auto uses = OptionSet<BuiltinUse>::fromRaw(static_cast<OptionSet<BuiltinUse>::StorageType>(record.uses));
        if (uses.contains(BuiltinUse::NamedAtRunTime) != namedAtRunTime)
            continue;
        uses.remove(BuiltinUse::NamedAtRunTime);
        keepLowerWitnessInProgramFacts(m_witnesses[builtin.index()], record.witness);
        if ((uses - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        noteCodeMayRun(builtin, record.witness);
        if (uses.contains(BuiltinUse::Escaped))
            m_isLetGo.set(builtin.index());
        if (namedAtRunTime || (uses - usesThatNameNothing).isEmpty())
            continue;
        m_isNamed.quickSet(builtin.index());
        if (uses.contains(BuiltinUse::Read) && hasFunction(record.witness))
            noteNamesOfHolders(builtin);
    }
}

BuiltinID FeaturesOfProgramFacts::entryWithFeaturesOf(BuiltinClassID builtinClass) const
{
    const BuiltinClass& record = m_builtins.at(builtinClass);
    BuiltinID prototype = record.prototype ? m_builtins.canonicalID(record.prototype) : BuiltinID();
    BuiltinID constructor = record.constructor ? m_builtins.canonicalID(record.constructor) : BuiltinID();
    if (prototype && (!constructor || !m_builtins.featureUsesOf(prototype).empty()))
        return prototype;
    if (constructor && m_builtins.at(constructor).flags.contains(BuiltinFlag::EvaluatesText))
        return BuiltinID();
    return constructor;
}

void FeaturesOfProgramFacts::noteInstancesAreMade(BuiltinClassID builtinClass, const Witness& witness)
{
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (m_madeClasses.quickSet(current.index()))
            return;
        noteCodeMayRun(entryWithFeaturesOf(current), witness);
    }
}

void FeaturesOfProgramFacts::noteTypeIsMade(BuiltinTypeID type, const Witness& witness)
{
    if (!type || m_madeTypes.set(type.index()))
        return;
    for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
        if (part.builtinClass)
            noteInstancesAreMade(part.builtinClass, witness);
        if (part.kind == BuiltinTypePartKind::Layout) {
            for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
                noteTypeIsMade(field.type, witness);
        }
        for (const BuiltinSlotContents& contents : part.contents)
            noteTypeIsMade(contents.type, witness);
    }
}

void FeaturesOfProgramFacts::noteTypesThatOverloadMakes(const BuiltinOverload& overload, const Witness& witness)
{
    auto notePlace = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue)
            noteTypeIsMade(place.newValueType, witness);
    };
    noteTypeIsMade(overload.result.type, witness);
    for (const BuiltinResultProperty& property : overload.result.properties)
        noteTypeIsMade(property.type, witness);
    for (const BuiltinTransfer& transfer : overload.flow.transfers)
        notePlace(transfer.from);
    for (const BuiltinCallback& callback : overload.flow.callbacks) {
        notePlace(callback.newTarget);
        notePlace(callback.thisValue);
        notePlace(callback.spreadArguments);
        std::ranges::for_each(callback.arguments, notePlace);
    }
    for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
        notePlace(action.value);
    for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
        notePlace(call.thisValue);
        std::ranges::for_each(call.arguments, notePlace);
    }
}

void FeaturesOfProgramFacts::noteClassesThatFunctionsMake()
{
    const ObjectTable& objects = m_program.types.objects();
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = m_program.at(id);
        if (!m_linked.function(id).isReached)
            continue;
        if (function.isCoroutine()) {
            if (BuiltinClassID made = objects.at(objects.coroutineResultOf(id)).builtinClass)
                noteInstancesAreMade(made, noWitness);
        }
        if (function.has(FunctionFlags::HasRestParameter)) {
            if (BuiltinClassID made = objects.at(objects.restArrayOf(id)).builtinClass)
                noteInstancesAreMade(made, noWitness);
        }
    }
}

bool FeaturesOfProgramFacts::needsWithoutCondition(BuiltinID builtin, RemovableFeature feature) const
{
    return std::ranges::any_of(m_builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) {
        return line.feature == feature && !line.condition;
    });
}

bool FeaturesOfProgramFacts::makersListFeature(BuiltinClassID builtinClass, RemovableFeature feature) const
{
    const BuiltinClass& record = m_builtins.at(builtinClass);
    if (needsWithoutCondition(m_builtins.canonicalID(record.prototype), feature))
        return true;
    return m_makersNameFeaturesOfConstructor.quickGet(builtinClass.index()) && needsWithoutCondition(m_builtins.canonicalID(record.constructor), feature);
}

BuiltinClassID FeaturesOfProgramFacts::classWhoseMakersRoot(BuiltinID builtin) const
{
    BuiltinClassID owner = m_classOfMember[builtin.index()];
    return owner && m_makersNameFeatures.quickGet(owner.index()) ? owner : BuiltinClassID();
}

void FeaturesOfProgramFacts::findClassesRootedByMakers()
{
    auto canonical = [&](BuiltinID builtin) {
        return m_builtins.canonicalID(builtin).index();
    };
    m_classOfMember.fill(BuiltinClassID(), m_builtins.builtinCount());
    m_makersNameFeatures.ensureSize(m_builtins.classCount());
    m_makersNameFeaturesOfConstructor.ensureSize(m_builtins.classCount());
    for (const LinkedCallOfBuiltin& call : m_linked.lookups.callsOfBuiltinsAtSites)
        m_isBoundAtSite.set(canonical(call.row));
    BitVector hangsOutsidePrototypeObjects(m_builtins.builtinCount());
    BitVector hangsOnWhatIsLetGo(m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID holder = m_builtins.at(BuiltinID(index)).holder;
        if (!holder || m_builtins.at(holder).kind == BuiltinKind::Prototype)
            continue;
        hangsOutsidePrototypeObjects.quickSet(canonical(BuiltinID(index)));
        if (m_isLetGo.get(canonical(holder)))
            hangsOnWhatIsLetGo.quickSet(canonical(BuiltinID(index)));
    }
    auto standsBelowErrors = [&](BuiltinID prototype) {
        BuiltinID current = m_builtins.canonicalID(prototype);
        while (m_builtins.at(current).prototypeLink == BuiltinPrototypeLink::Builtin) {
            current = m_builtins.canonicalID(m_builtins.at(current).inheritsFrom);
            if (current == m_builtins.canonicalID(Builtins::ErrorPrototype))
                return true;
        }
        return m_builtins.at(current).prototypeLink == BuiltinPrototypeLink::Unknown;
    };
    BitVector hasInstanceAmongEntries(m_builtins.classCount());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        const Builtin& entry = m_builtins.at(BuiltinID(index));
        if (entry.kind == BuiltinKind::Prototype || entry.prototypeLink != BuiltinPrototypeLink::Builtin)
            continue;
        if (BuiltinClassID above = m_classOfPrototype[canonical(entry.inheritsFrom)])
            hasInstanceAmongEntries.quickSet(above.index());
    }
    BitVector hasMakerInTable(m_builtins.classCount());
    Vector<Vector<BuiltinClassID, 2>> classesBelow(m_builtins.classCount());
    for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
        const BuiltinClass& builtinClass = m_builtins.at(BuiltinClassID(index));
        for (BuiltinClassID above = builtinClass.parent; above; above = m_builtins.at(above).parent)
            classesBelow[above.index()].append(BuiltinClassID(index));
        if (builtinClass.origin != BuiltinOrigin::Host || !builtinClass.prototype || builtinClass.instancesAreEntries || hasInstanceAmongEntries.quickGet(index) || standsBelowErrors(builtinClass.prototype))
            continue;
        bool canBeNamed = builtinClass.constructor && hangsOutsidePrototypeObjects.quickGet(canonical(builtinClass.constructor));
        if (canBeNamed && (isNamedByStatement(m_builtins.canonicalID(builtinClass.constructor)) || isNamedByStatement(m_builtins.canonicalID(builtinClass.prototype))))
            continue;
        m_makersNameFeatures.quickSet(index);
        m_classOfMember[canonical(builtinClass.prototype)] = BuiltinClassID(index);
        bool isComeToThroughInstancesAlone = canBeNamed && !m_unseenCodeMayRun && !hangsOnWhatIsLetGo.quickGet(canonical(builtinClass.constructor)) && !hasPlaceInStatement(m_builtins.canonicalID(builtinClass.constructor));
        if (builtinClass.constructor && (!canBeNamed || isComeToThroughInstancesAlone))
            m_classOfMember[canonical(builtinClass.constructor)] = BuiltinClassID(index);
        if (isComeToThroughInstancesAlone)
            m_makersNameFeaturesOfConstructor.quickSet(index);
    }
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID holder = m_builtins.at(BuiltinID(index)).holder;
        if (m_builtins.at(BuiltinID(index)).kind == BuiltinKind::Constructor && hangsOutsidePrototypeObjects.quickGet(canonical(BuiltinID(index))))
            continue;
        if (holder && !m_classOfMember[canonical(BuiltinID(index))])
            m_classOfMember[canonical(BuiltinID(index))] = m_classOfMember[canonical(holder)];
    }
    auto prototypeOf = [&](BuiltinClassID builtinClass) {
        return m_builtins.canonicalID(m_builtins.at(builtinClass).prototype);
    };
    auto checkMakerOf = [&](BuiltinClassID made, BuiltinID maker) {
        BuiltinClassID owner = m_classOfMember[maker.index()];
        if (m_builtins.at(maker).kind == BuiltinKind::Constructor && hangsOutsidePrototypeObjects.quickGet(maker.index()))
            owner = BuiltinClassID();
        if (!m_makersNameFeatures.quickGet(made.index()) || owner == made)
            return;
        hasMakerInTable.quickSet(made.index());
        bool namesFeatures = std::ranges::all_of(m_builtins.featureUsesOf(prototypeOf(made)), [&](const BuiltinFeatureUse& line) {
            return line.condition || needsWithoutCondition(maker, line.feature) || (owner && needsWithoutCondition(prototypeOf(owner), line.feature));
        });
        m_makersNameFeatures.quickSet(made.index(), namesFeatures);
        if (!m_makersNameFeaturesOfConstructor.quickGet(made.index()))
            return;
        bool namesFeaturesOfConstructor = std::ranges::all_of(m_builtins.featureUsesOf(m_builtins.canonicalID(m_builtins.at(made).constructor)), [&](const BuiltinFeatureUse& line) {
            return line.condition || needsWithoutCondition(maker, line.feature) || (owner && needsWithoutCondition(prototypeOf(owner), line.feature));
        });
        m_makersNameFeaturesOfConstructor.quickSet(made.index(), namesFeaturesOfConstructor);
    };
    Vector<BuiltinTypeID, 8> types;
    auto add = [&](BuiltinTypeID type) {
        if (type)
            types.appendIfNotContains(type);
    };
    auto addPlace = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue)
            add(place.newValueType);
    };
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID maker = m_builtins.canonicalID(BuiltinID(index));
        types.shrink(0);
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(BuiltinID(index))) {
            add(overload.result.type);
            for (const BuiltinResultProperty& property : overload.result.properties)
                add(property.type);
            for (const BuiltinTransfer& transfer : overload.flow.transfers)
                addPlace(transfer.from);
            for (const BuiltinCallback& callback : overload.flow.callbacks) {
                addPlace(callback.newTarget);
                addPlace(callback.thisValue);
                addPlace(callback.spreadArguments);
                std::ranges::for_each(callback.arguments, addPlace);
            }
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
                addPlace(action.value);
            for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
                addPlace(call.thisValue);
                std::ranges::for_each(call.arguments, addPlace);
            }
        }
        for (size_t position = 0; position < types.size(); ++position) {
            for (const BuiltinTypePart& part : m_builtins.at(types[position]).parts) {
                for (BuiltinClassID current = part.builtinClass; current; current = m_builtins.at(current).parent)
                    checkMakerOf(current, maker);
                if (part.builtinClass && !part.isOfClassItself) {
                    for (BuiltinClassID below : classesBelow[part.builtinClass.index()])
                        checkMakerOf(below, maker);
                }
                if (part.kind == BuiltinTypePartKind::Layout) {
                    for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
                        add(field.type);
                }
                for (const BuiltinSlotContents& contents : part.contents)
                    add(contents.type);
            }
        }
    }
    BitVector isListedOutsideRule(removableFeatureIndexLimit);
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinClassID owner = m_classOfMember[canonical(BuiltinID(index))];
        bool isConstructorThatNameLeadsTo = m_builtins.at(BuiltinID(index)).kind == BuiltinKind::Constructor && hangsOutsidePrototypeObjects.quickGet(canonical(BuiltinID(index)));
        if (owner && !isConstructorThatNameLeadsTo && m_makersNameFeatures.quickGet(owner.index()))
            continue;
        for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(BuiltinID(index))) {
            if (!line.condition)
                isListedOutsideRule.quickSet(static_cast<uint32_t>(line.feature));
        }
    }
    for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
        if (!m_makersNameFeatures.quickGet(index) || hasMakerInTable.quickGet(index))
            continue;
        BuiltinID entry = entryWithFeaturesOf(BuiltinClassID(index));
        bool isListedOutsideClass = entry && std::ranges::all_of(m_builtins.featureUsesOf(entry), [&](const BuiltinFeatureUse& line) {
            return line.condition || isListedOutsideRule.quickGet(static_cast<uint32_t>(line.feature));
        });
        m_makersNameFeatures.quickSet(index, isListedOutsideClass);
    }
}

void FeaturesOfProgramFacts::visitRow(BuiltinID builtin)
{
    const Builtin& entry = m_builtins.at(builtin);
    Witness witness = m_witnesses[builtin.index()];
    BuiltinID prototype = entry.kind == BuiltinKind::Prototype ? builtin : BuiltinID();
    if ((entry.kind == BuiltinKind::Method || entry.kind == BuiltinKind::Accessor) && entry.holder)
        prototype = m_builtins.canonicalID(entry.holder);
    bool makersRoot = !!classWhoseMakersRoot(builtin);
    if (prototype && (!makersRoot || isNamedByStatement(prototype))) {
        if (BuiltinClassID owner = m_classOfPrototype[prototype.index()])
            noteCodeMayRun(entryWithFeaturesOf(owner), witness);
    }
    if (!makersRoot && (entry.kind == BuiltinKind::Function || entry.kind == BuiltinKind::Accessor || entry.kind == BuiltinKind::Prototype) && entry.holder) {
        const Builtin& holder = m_builtins.at(m_builtins.canonicalID(entry.holder));
        if (holder.kind == BuiltinKind::Constructor && !holder.flags.contains(BuiltinFlag::EvaluatesText))
            noteCodeMayRun(m_builtins.canonicalID(entry.holder), witness);
    }
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(builtin))
        noteTypesThatOverloadMakes(overload, witness);
}

void FeaturesOfProgramFacts::appendRecordsOf(BuiltinID builtin)
{
    if (refusesTextAsUnseenCode(m_builtins, builtin, m_refused))
        return;
    BuiltinClassID owner = classWhoseMakersRoot(builtin);
    for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(builtin)) {
        if (!mayHold(line.condition, builtin))
            continue;
        if (owner && makersListFeature(owner, line.feature))
            continue;
        m_records.append(FeatureUseRecord { static_cast<uint32_t>(line.feature), builtin.index(), m_witnesses[builtin.index()], isSeenToHold(line.condition, builtin) ? FeatureUseKind::Named : FeatureUseKind::Unnamed });
    }
    if (!runsFromTextOfLibrary(builtin))
        return;
    for (RemovableFeature feature : { RemovableFeature::Interpreter, RemovableFeature::BuiltinSourceText })
        m_records.append(FeatureUseRecord { static_cast<uint32_t>(feature), builtin.index(), m_witnesses[builtin.index()], FeatureUseKind::Named });
}

bool FeaturesOfProgramFacts::mayHold(const BuiltinCondition* condition, BuiltinID builtin) const
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    if (!condition->nameToldUnder() || m_operandsNobodyKnows.quickGet(builtin.index()))
        return true;
    return m_linked.lookups.conditionsThatOperandsMayMeet.contains(wordOfConditionInProgramFacts(builtin, condition->nameToldUnder()));
}

bool FeaturesOfProgramFacts::isSeenToHold(const BuiltinCondition* condition, BuiltinID builtin) const
{
    if (!condition || !condition->nameToldUnder() || m_isHandedOnByCodeInSight.quickGet(builtin.index()))
        return true;
    return m_linked.lookups.conditionsThatOperandsMayMeet.contains(wordOfConditionInProgramFacts(builtin, condition->nameToldUnder()));
}

bool FeaturesOfProgramFacts::runsFromTextOfLibrary(BuiltinID builtin) const
{
    if (!m_admitsTextOfLibrary)
        return false;
    auto isLeftOut = [&](RuntimeFunctionID function) {
        if (!function)
            return false;
        const RuntimeFunctionEntry* held = findRuntimeFunction(m_program, function);
        return !held || !held->function;
    };
    return std::ranges::any_of(m_builtins.overloadsOf(builtin), [&](const BuiltinOverload& overload) {
        return isLeftOut(overload.code.implementation.function);
    }) || std::ranges::any_of(m_builtins.calledFunctionsOf(builtin), [&](const BuiltinCalledFunction& called) {
        return mayHold(called.condition, builtin) && isLeftOut(called.function);
    });
}

BuiltinID FeaturesOfProgramFacts::outermostHolderWithFeature(BuiltinID builtin, uint32_t feature) const
{
    return outermostHolderWithFeatureInProgramFacts(m_builtins, builtin, feature);
}

void FeaturesOfProgramFacts::sortRecordsAndMarkUnnamed()
{
    auto orderOf = [](const FeatureUseRecord& record) {
        return std::tuple(record.feature, record.builtin);
    };
    AOT::sort(m_records, { }, [](const FeatureUseRecord& record) {
        return std::tuple(record.feature, record.builtin, record.kind);
    });
    m_records.shrink(std::ranges::unique(m_records, { }, orderOf).begin() - m_records.begin());
    for (FeatureUseRecord& record : m_records) {
        if (record.builtin == Wire::none)
            continue;
        BuiltinID builtin(record.builtin);
        if (!m_codeMayRunWithoutNamesFromRunTime.quickGet(builtin.index()))
            record.kind = FeatureUseKind::NamedAtRunTime;
        else if (record.kind == FeatureUseKind::Unnamed || !m_isNamed.quickGet(outermostHolderWithFeature(builtin, record.feature).index()))
            record.kind = FeatureUseKind::Unnamed;
        else if (m_isFoundByNameAlone.quickGet(outermostHolderWithFeature(builtin, record.feature).index()))
            record.kind = FeatureUseKind::FoundByName;
    }
}

void FeaturesOfProgramFacts::visitRowsThatWait()
{
    while (!m_rowsToVisit.isEmpty())
        visitRow(m_rowsToVisit.takeLast());
}

Vector<FeatureUseRecord> FeaturesOfProgramFacts::run(std::span<const BuiltinUseRecord> uses)
{
    if (m_program.admitsInspector) {
        BitVector features(removableFeatureIndexLimit);
        for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
            for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(BuiltinID(index)))
                features.set(static_cast<uint32_t>(line.feature));
        }
        for (size_t feature : features)
            m_records.append(FeatureUseRecord { static_cast<uint32_t>(feature), Wire::none, noWitness, FeatureUseKind::Named });
        return WTF::move(m_records);
    }

    collectUses(uses, false);
    findClassesRootedByMakers();
    noteClassesThatFunctionsMake();
    visitRowsThatWait();
    m_codeMayRunWithoutNamesFromRunTime = m_codeMayRun;
    collectUses(uses, true);
    visitRowsThatWait();
    for (size_t index : m_codeMayRun)
        appendRecordsOf(BuiltinID(static_cast<uint32_t>(index)));
    sortRecordsAndMarkUnnamed();
    return WTF::move(m_records);
}

class WritesOnBuiltinsInProgramFacts {
    WTF_MAKE_NONCOPYABLE(WritesOnBuiltinsInProgramFacts);
public:
    WritesOnBuiltinsInProgramFacts(const Program&, RefusedCode, const LookupsPastProgramObjects&);

    void take(const FindingOfProgramFacts&);
    void settle();

    bool canLockAtStartUp(BuiltinID member) const;
    ReasonBuiltinStaysOpen reasonToLeaveOpen(BuiltinID member) const { return reasonToLeaveOpen(member, m_isLostOutOfPlaceThatHolds); }
    bool isWrittenAsFarAsLinkGoes(BuiltinID member) const { return reasonToLeaveOpen(member, m_isLost) != ReasonBuiltinStaysOpen::None; }
    bool mayHaveNameLaidAsFarAsLinkGoes(BuiltinID canonicalHolder, PropertyKeyID key) const { return reasonNoLockHolds() != ReasonBuiltinStaysOpen::None || isReachedThroughUnseen(canonicalHolder) || reasonToLeaveOpenUnder(canonicalHolder, key, false, m_isLost) != ReasonBuiltinStaysOpen::None; }

private:
    ReasonBuiltinStaysOpen reasonToLeaveOpen(BuiltinID member, const BitVector& lost) const;
    ReasonBuiltinStaysOpen reasonNoLockHolds() const;
    ReasonBuiltinStaysOpen reasonToLeaveOpenUnder(BuiltinID canonicalHolder, PropertyKeyID, bool isLockedForProduct, const BitVector& lost) const;
    void closeOverNamesReadThroughLostBuiltins(BitVector&) const;
    void carryOut(BuiltinID canonical, bool onlyWhatLiesOnIt, bool isByFileOfProduct);
    bool runsCodeThatNoLockIsLaidBeside(BuiltinID canonical, bool mayPassOptionThatRunsCode) const;
    bool isReachedThroughUnseen(BuiltinID canonicalHolder) const;
    bool isReadOffUnseen(BuiltinName) const;
    template<typename Functor> void closeOverMembers(BitVector&, const Functor& isRead) const;

    const Program& m_source;
    const BuiltinTable& m_builtins;
    const RefusedCode m_refused;
    const LookupsPastProgramObjects& m_lookups;

    BitVector m_isWritten;
    BitVector m_isCarriedOutByProgram;
    BitVector m_isCarriedOutBesideProduct;
    BitVector m_isWrittenUnderAnyName;
    BitVector m_namesReadByProgram;
    BitVector m_isLost;
    BitVector m_isLostOutOfPlaceThatHolds;
    BitVector m_isWithinReachOfUnseen;
    bool m_locksAreLaid { true };
};

WritesOnBuiltinsInProgramFacts::WritesOnBuiltinsInProgramFacts(const Program& program, RefusedCode refused, const LookupsPastProgramObjects& lookups)
    : m_source(program)
    , m_builtins(program.builtins())
    , m_refused(refused)
    , m_lookups(lookups)
    , m_isWritten(m_builtins.builtinCount())
    , m_isCarriedOutByProgram(m_builtins.builtinCount())
    , m_isCarriedOutBesideProduct(m_builtins.builtinCount())
    , m_isWrittenUnderAnyName(m_builtins.builtinCount())
    , m_isLost(m_builtins.builtinCount())
    , m_isLostOutOfPlaceThatHolds(m_builtins.builtinCount())
    , m_isWithinReachOfUnseen(m_builtins.builtinCount())
{
}

void WritesOnBuiltinsInProgramFacts::carryOut(BuiltinID canonical, bool onlyWhatLiesOnIt, bool isByFileOfProduct)
{
    auto carry = [&](BuiltinID object) {
        m_isCarriedOutByProgram.quickSet(object.index());
        if (!isByFileOfProduct)
            m_isCarriedOutBesideProduct.quickSet(object.index());
    };
    if (!onlyWhatLiesOnIt)
        return carry(canonical);
    for (BuiltinID member : m_builtins.membersOf(canonical))
        carry(m_builtins.canonicalID(member));
}

template<typename Functor>
void WritesOnBuiltinsInProgramFacts::closeOverMembers(BitVector& objects, const Functor& isRead) const
{
    Vector<BuiltinID> notLookedBelow;
    for (size_t index : objects)
        notLookedBelow.append(BuiltinID(static_cast<uint32_t>(index)));
    while (!notLookedBelow.isEmpty()) {
        for (BuiltinID member : m_builtins.membersOf(notLookedBelow.takeLast())) {
            BuiltinID object = m_builtins.canonicalID(member);
            if (isObjectKind(m_builtins.at(object).kind) && !objects.quickGet(object.index()) && isRead(member)) {
                objects.quickSet(object.index());
                notLookedBelow.append(object);
            }
        }
    }
}

void WritesOnBuiltinsInProgramFacts::settle()
{
    auto mergeInto = [&](BitVector& into, const BitVector& from) {
        for (size_t index : from)
            into.quickSet(index);
    };
    auto isReadByProgram = [&](BuiltinID member) {
        PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
        return key && m_namesReadByProgram.get(key.index());
    };
    closeOverMembers(m_isCarriedOutByProgram, isReadByProgram);
    if (m_source.admitsReplacedBuiltins)
        closeOverMembers(m_isCarriedOutBesideProduct, isReadByProgram);

    mergeInto(m_isLost, m_lookups.lostBuiltins);
    closeOverNamesReadThroughLostBuiltins(m_isLost);

    mergeInto(m_isWithinReachOfUnseen, m_lookups.builtinsAtUnseen);
    for (size_t index : m_lookups.classesWithInstancesAtUnseen) {
        for (BuiltinClassID current(static_cast<uint16_t>(index)); current; current = m_builtins.at(current).parent) {
            BuiltinID prototype = m_builtins.at(current).prototype;
            if (!prototype)
                continue;
            if (m_builtins.at(prototype).origin == BuiltinOrigin::Host)
                m_isWithinReachOfUnseen.quickSet(m_builtins.canonicalID(prototype).index());
            break;
        }
    }
    m_isWithinReachOfUnseen.quickClear(m_builtins.canonicalID(Builtins::GlobalThis).index());
    closeOverMembers(m_isWithinReachOfUnseen, [&](BuiltinID member) {
        return m_builtins.canonicalID(member) != m_builtins.canonicalID(Builtins::GlobalThis);
    });

    m_isLostOutOfPlaceThatHolds.clearAll();
    for (size_t index : m_lookups.lostBuiltins) {
        BuiltinID lost(static_cast<uint32_t>(index));
        if (!canLockAtStartUp(lost) || !isWrittenAsFarAsLinkGoes(lost))
            m_isLostOutOfPlaceThatHolds.quickSet(index);
    }
    closeOverNamesReadThroughLostBuiltins(m_isLostOutOfPlaceThatHolds);
}

void WritesOnBuiltinsInProgramFacts::closeOverNamesReadThroughLostBuiltins(BitVector& lost) const
{
    bool hasGrown = true;
    while (std::exchange(hasGrown, false)) {
        closeOverMembers(lost, [&](BuiltinID member) {
            PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
            return key && m_lookups.namesReadThroughLostBuiltins.get(key.index());
        });
        if (!m_lookups.climbsFromLostBuiltins)
            break;
        for (size_t index : lost) {
            const Builtin& entry = m_builtins.at(BuiltinID(static_cast<uint32_t>(index)));
            if (entry.prototypeLink == BuiltinPrototypeLink::Builtin && entry.inheritsFrom)
                hasGrown |= !lost.quickSet(m_builtins.canonicalID(entry.inheritsFrom).index());
        }
    }
}

bool WritesOnBuiltinsInProgramFacts::runsCodeThatNoLockIsLaidBeside(BuiltinID canonical, bool mayPassOptionThatRunsCode) const
{
    const Builtin& entry = m_builtins.at(canonical);
    if (m_builtins.has(BuiltinMark::ReadLoadsCodeByName, canonical))
        return false;
    std::optional<UnseenCodeCause> cause = eventOfCalling(m_builtins, canonical, m_refused);
    if (!cause && mayPassOptionThatRunsCode && optionThatRunsCodeInProgramFacts(m_builtins, canonical) && !refusesTextAsUnseenCode(m_builtins, canonical, m_refused))
        cause = UnseenCodeCause::RunsUnseenCode;
    if (!cause)
        return false;
    switch (*cause) {
    case UnseenCodeCause::EvaluatesText:
    case UnseenCodeCause::RunsUnseenCode:
        break;
    case UnseenCodeCause::LoadsModuleByName:
    case UnseenCodeCause::LoadsCodeByName:
        if (!entry.flags.contains(BuiltinFlag::EvaluatesText))
            return false;
        break;
    case UnseenCodeCause::Unknown:
    case UnseenCodeCause::ImportsUncompiledModule:
    case UnseenCodeCause::OptionAdmitsCode:
    case UnseenCodeCause::FlowResultDisabled:
    case UnseenCodeCause::RunsMachineCode:
        return false;
    }
    bool runsOnlyText = entry.origin == BuiltinOrigin::Engine && entry.flags.contains(BuiltinFlag::EvaluatesText) && !entry.flags.contains(BuiltinFlag::RunsUnseenCode) && !m_builtins.unseenCodeOf(canonical);
    return !runsOnlyText && !m_builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, canonical);
}

void WritesOnBuiltinsInProgramFacts::take(const FindingOfProgramFacts& finding)
{
    using Kind = FindingOfProgramFacts::Kind;
    static constexpr OptionSet<BuiltinUse> writingUses { BuiltinUse::Written, BuiltinUse::StoredByName };
    auto noteWritten = [&](BuiltinID member) {
        if (writeOpensNothingInTestRun(m_source, FunctionID(finding.witness.function), member))
            return;
        m_isWritten.quickSet(member.index());
        m_isWritten.quickSet(m_builtins.canonicalID(member).index());
    };
    bool isOfProgram = finding.isOfProgram;

    switch (finding.kind) {
    case Kind::UseOfBuiltin:
    case Kind::ReadOfAnyMember: {
        auto uses = OptionSet<BuiltinUse>::fromRaw(finding.detail);
        BuiltinID canonical = m_builtins.canonicalID(BuiltinID(finding.subject));
        if (uses.containsAny(writingUses))
            noteWritten(BuiltinID(finding.subject));
        if (isOfProgram && uses.containsAny(usesOfObjectItselfInProgramFacts))
            m_locksAreLaid &= !runsCodeThatNoLockIsLaidBeside(canonical, uses.contains(BuiltinUse::Escaped));
        if (isOfProgram && uses.contains(BuiltinUse::Escaped))
            carryOut(canonical, finding.kind == Kind::ReadOfAnyMember, isFileOfProductInTestRun(m_source, FunctionID(finding.witness.function)));
        break;
    }
    case Kind::OptionThatRunsCode:
        if (isOfProgram)
            m_locksAreLaid &= !runsCodeThatNoLockIsLaidBeside(m_builtins.canonicalID(BuiltinID(finding.subject)), true);
        break;
    case Kind::WriteOnBuiltin: {
        BuiltinID holder = m_builtins.canonicalID(BuiltinID(finding.subject));
        if (finding.key == Wire::none && isFileOfProductInTestRun(m_source, FunctionID(finding.witness.function))) {
            for (BuiltinID member : m_builtins.membersOf(holder))
                noteWritten(member);
        } else if (finding.key == Wire::none)
            m_isWrittenUnderAnyName.quickSet(holder.index());
        else if (BuiltinID member = findOwnMember(m_source, holder, PropertyKeyID(finding.key)))
            noteWritten(member);
        break;
    }
    case Kind::NameReadOffUnknownValue:
        m_namesReadByProgram.set(finding.subject);
        break;
    case Kind::StandsAboveClass: {
        if (!isOfProgram)
            break;
        BuiltinID canonical = m_builtins.canonicalID(BuiltinID(finding.subject));
        bool isByFileOfProduct = isFileOfProductInTestRun(m_source, FunctionID(finding.witness.function));
        carryOut(canonical, false, isByFileOfProduct);
        if (BuiltinID prototype = m_builtins.findOwnMember(canonical, BuiltinNames::prototype))
            carryOut(m_builtins.canonicalID(prototype), false, isByFileOfProduct);
        break;
    }
    case Kind::PrototypeChangeOfBuiltin:
    case Kind::ReadThatNoRowAnswers:
    case Kind::PartOfClassIsUsed:
    case Kind::NamespaceIsGivenByImportCall:
    case Kind::KeysOfBuiltinListed:
    case Kind::InstanceThatMayAnswerEveryName:
    case Kind::UnseenCode:
    case Kind::CallOfEvent:
    case Kind::BuiltinSpelledInText:
    case Kind::WorkerEntry:
    case Kind::WorkerStartWithoutName:
    case Kind::WorkerStartFromURL:
    case Kind::NamedModule:
    case Kind::LoaderHandedToText:
    case Kind::LightFormIsAskedForMore:
    case Kind::CallUnderComputedKey:
        break;
    }
}

bool WritesOnBuiltinsInProgramFacts::canLockAtStartUp(BuiltinID member) const
{
    const Builtin& entry = m_builtins.at(member);
    if (!entry.holder || !entry.name || entry.mutability != BuiltinMutability::ConstantUnlessWritten || m_builtins.isLocationOfLockedObject(member))
        return false;
    if (entry.scope != BuiltinScope::Realm || (!m_builtins.isUnder(member, Builtins::GlobalThis) && !m_source.isLockLeftToProgram(member)))
        return false;
    return m_builtins.findOwnMember(entry.holder, entry.name) == member;
}

bool WritesOnBuiltinsInProgramFacts::isReadOffUnseen(BuiltinName name) const
{
    PropertyKeyID key = m_source.keyOf(name);
    return m_lookups.readsUnderKeysNobodyKnows || !m_lookups.holdersReadUnderKeysNobodyKnows.isEmpty() || m_lookups.readsUnderNamesThatLoopsGave || (key && m_lookups.namesRead.get(key.index()));
}

bool WritesOnBuiltinsInProgramFacts::isReachedThroughUnseen(BuiltinID canonicalHolder) const
{
    if (m_isWithinReachOfUnseen.quickGet(canonicalHolder.index()) || canonicalHolder == m_builtins.canonicalID(Builtins::GlobalThis))
        return true;
    const Builtin& entry = m_builtins.at(canonicalHolder);
    switch (entry.kind) {
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return entry.name && isReadOffUnseen(entry.name);
    case BuiltinKind::Prototype:
        return m_lookups.climbsFromUnseenObjects || isReadOffUnseen(BuiltinNames::__proto__) || (isReadOffUnseen(BuiltinNames::constructor) && isReadOffUnseen(BuiltinNames::prototype));
    case BuiltinKind::Constructor: {
        BuiltinID prototype = m_builtins.findOwnMember(canonicalHolder, BuiltinNames::prototype);
        return prototype && m_lookups.holdersReadUnderKeysNobodyKnows.get(m_builtins.canonicalID(prototype).index());
    }
    case BuiltinKind::Namespace:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ReasonBuiltinStaysOpen WritesOnBuiltinsInProgramFacts::reasonNoLockHolds() const
{
    if (!m_locksAreLaid)
        return ReasonBuiltinStaysOpen::NoLockIsLaid;
    for (size_t passed : m_lookups.builtinsPassedOptionThatRunsCode) {
        if (runsCodeThatNoLockIsLaidBeside(BuiltinID(static_cast<uint32_t>(passed)), true))
            return ReasonBuiltinStaysOpen::NoLockIsLaid;
    }
    return m_source.admitsInspector ? ReasonBuiltinStaysOpen::IsOpenToDebugger : ReasonBuiltinStaysOpen::None;
}

ReasonBuiltinStaysOpen WritesOnBuiltinsInProgramFacts::reasonToLeaveOpen(BuiltinID member, const BitVector& lost) const
{
    if (ReasonBuiltinStaysOpen reason = reasonNoLockHolds(); reason != ReasonBuiltinStaysOpen::None)
        return reason;
    BuiltinID canonical = m_builtins.canonicalID(member);
    if (m_source.isUnlocked(member) || m_source.isUnlocked(canonical))
        return ReasonBuiltinStaysOpen::IsLeftOpenForTests;
    if (!m_refused.codeOutsideImage && m_builtins.has(BuiltinMark::IsDeletedWhenModuleOfLibraryLoads, member))
        return ReasonBuiltinStaysOpen::IsWrittenUnderItsName;
    if (m_isWritten.quickGet(member.index()) || m_isWritten.quickGet(canonical.index()) || m_lookups.membersWritten.get(member.index()))
        return ReasonBuiltinStaysOpen::IsWrittenUnderItsName;
    return reasonToLeaveOpenUnder(m_builtins.canonicalID(m_builtins.at(member).holder), m_source.keyOf(m_builtins.at(member).name), m_source.isLockLeftToProgram(member), lost);
}

ReasonBuiltinStaysOpen WritesOnBuiltinsInProgramFacts::reasonToLeaveOpenUnder(BuiltinID holder, PropertyKeyID key, bool isLockedForProduct, const BitVector& lost) const
{
    if (m_isWrittenUnderAnyName.quickGet(holder.index()) || m_lookups.holdersWrittenUnderAnyName.get(holder.index()))
        return ReasonBuiltinStaysOpen::HolderIsWrittenUnderAnyName;
    bool isWrittenThroughLostBuiltins = isLockedForProduct
        ? m_lookups.writesUnderAnyNameThroughLostBuiltinsBesideProduct || (key && m_lookups.namesWrittenThroughLostBuiltinsBesideProduct.get(key.index()))
        : m_lookups.writesUnderAnyNameThroughLostBuiltins || (key && m_lookups.namesWrittenThroughLostBuiltins.get(key.index()));
    bool isNamedThroughUnseen = key && (isLockedForProduct ? m_lookups.namesWrittenThroughUnseenBesideProduct : m_lookups.namesWrittenThroughUnseenByProgram).get(key.index());
    bool isWrittenThroughUnseen = isNamedThroughUnseen || (isLockedForProduct ? m_lookups.writesUnderAnyNameThroughUnseenBesideProduct : m_lookups.writesUnderAnyNameThroughUnseenByProgram);
    if (lost.quickGet(holder.index())) {
        if (isWrittenThroughLostBuiltins)
            return ReasonBuiltinStaysOpen::IsWrittenThroughLostBuiltins;
        if (m_lookups.lostBuiltinsReachUnseen && isWrittenThroughUnseen)
            return ReasonBuiltinStaysOpen::IsWrittenThroughLostBuiltins;
    }
    if (isNamedThroughUnseen && isReachedThroughUnseen(holder))
        return ReasonBuiltinStaysOpen::IsSpelledThroughUnseen;
    if (!isLockedForProduct && key && m_lookups.namesWrittenThroughUnseenByLibrary.get(key.index()) && m_builtins.at(holder).origin == BuiltinOrigin::Host && m_builtins.at(holder).kind == BuiltinKind::Prototype)
        return ReasonBuiltinStaysOpen::IsSpelledThroughUnseen;
    if ((isLockedForProduct ? m_isCarriedOutBesideProduct : m_isCarriedOutByProgram).quickGet(holder.index()))
        return ReasonBuiltinStaysOpen::HolderIsLetGoByProgram;
    return ReasonBuiltinStaysOpen::None;
}

static bool handsBackWhatProgramHandedInInProgramFacts(const BuiltinOverload& overload)
{
    auto goesIntoResult = [](const BuiltinTransfer& transfer) { return transfer.to.kind == BuiltinPlaceKind::Result; };
    auto answers = overload.flow.transfers | std::views::filter(goesIntoResult);
    return !answers.empty() && std::ranges::all_of(answers, [](const BuiltinTransfer& transfer) {
        return transfer.from.kind == BuiltinPlaceKind::NativeCell && transfer.from.holdsOnlyWhatIsStored;
    });
}

class BuilderOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(BuilderOfProgramFacts);
public:
    BuilderOfProgramFacts(const InputsOfProgramFacts&, FactTables&);

    void run();

    void noteReached(FunctionID);
    void leaveOutDirectEvals() { m_leavesOutDirectEvals = true; }
    void noteFindingsIn(FunctionID id) { take(m_inputs.looks.of(id)); }
    void noteFindingsIn(std::span<const FunctionID>);
    void noteObjectsThatBodyMakes(FunctionID id) { noteWhatBodyMakes(id); }
    bool hasNoFindingsIn(FunctionID id) const { return m_inputs.looks.of(id).empty(); }
    bool knowsWhatBodyMakes(FunctionID) const;
    WritesOnBuiltinsInProgramFacts& writes() LIFETIME_BOUND { return m_writes; }
    void noteEvaluated(ModuleID);
    void setNativeCodeMayHoldObjects(bool mayHold) { m_nativeCodeMayHoldObjects = mayHold; }
    void noteRegistryMayGiveKeyOfPrinter() { m_registryMayGiveKeyOfPrinter = true; }
    void take(const LookupsPastProgramObjects&);
    void noteNameRead(PropertyKeyID key) { m_namesRead.set(key.index()); }
    void noteNameCalled(PropertyKeyID key, bool mayPassOperands)
    {
        m_namesCalled.set(key.index());
        if (mayPassOperands)
            m_namesCalledWithOperands.set(key.index());
    }
    void noteNameCalledByNativeCodeOffUnseenObjects(PropertyKeyID key) { m_namesCalledByNativeCodeOffUnseenObjects.set(key.index()); }
    void noteNameSpelledByProgram(PropertyKeyID key) { m_namesSpelledByProgram.set(key.index()); }
    void noteReadUnderKeyNobodyKnows() { m_readsUnderComputedKeys = true; }
    void noteReadUnderKeyNobodyKnowsOff(BuiltinID canonicalHolder)
    {
        m_isReadUnderComputedKeys.quickSet(canonicalHolder.index());
        m_isLookedThrough.quickSet(canonicalHolder.index());
        m_holdersToLookThrough.append(canonicalHolder);
    }
    void noteReadUnderNameThatLoopGave() { m_readsUnderNamesThatLoopsGave = true; }
    void noteInstancesAnswerEveryNameWithObjects() { m_objectsMayHaveMethodCalledWithStartupLoads = true; }
    std::span<const FunctionID> functionsThatAnswerEveryNameOnInstances() const { return m_functionsThatAnswerEveryNameOnInstances.span(); }
    void noteUse(BuiltinID builtin, OptionSet<BuiltinUse> uses) { m_uses[builtin.index()].add(uses); }
    void noteCall(BuiltinID builtin) { noteUse(m_builtins.canonicalID(builtin), callingUsesOf(m_builtins.at(m_builtins.canonicalID(builtin)).kind) | OptionSet<BuiltinUse> { BuiltinUse::Read }); }
    void noteReadOfMember(BuiltinID);
    void noteStoreUnderKeyNobodyKnowsOn(BuiltinID canonicalHolder);
    void noteDeletionOfMemberOf(BuiltinID canonicalHolder);
    void noteLossOf(BuiltinID, const Witness& = noWitness);
    void noteCallOutOfSight(BuiltinID builtin) { m_isCalledOutOfSightOfLink.quickSet(m_builtins.canonicalID(builtin).index()); }
    void noteCallWithoutRow(BuiltinID builtin) { m_isCalledWithoutRow.quickSet(m_builtins.canonicalID(builtin).index()); }
    void noteRowApplied(BuiltinID canonical, BuiltinOverloadID row)
    {
        if (!m_rowsThatLinkApplies.set(row.index()))
            m_stateWhenRowsWereRead[canonical.index()] = 0;
    }
    static uint64_t wordOfCall(uint32_t caller, uint32_t site) { return static_cast<uint64_t>(caller) << 32 | site; }
    Vector<uint64_t> takeCallsWhoseRowsLinkTells() { return std::exchange(m_callsListedSinceLinkTookThemOver, { }); }
    bool noteCallLeftToUnseen(uint64_t call);
    const BitVector& builtinsWithCallsLeftToUnseen() const { return m_hasCallThatLinkLeftToUnseen; }
    void noteCallWhoseRowsLinkTells(uint64_t call, BuiltinID canonical);
    bool noteEveryRowMayApplyTo(BuiltinID canonical);
    void noteObjectHandedToFormatter(bool isWalked);
    void noteInstancesLoseTheirConstructor()
    {
        if (!std::exchange(m_instancesLoseTheirConstructor, true))
            noteObjectHandedToFormatter(true);
    }
    void noteTypeHandedToHandler(BuiltinTypeID);
    void noteLetGoByLink(BuiltinID);
    void noteStandsAboveClass(BuiltinID, FunctionID constructor);
    void noteFoundWhereFlowBroughtHolder(BuiltinID, FunctionID reader);
    void noteInstancesMayReachFormatters(BuiltinClassID);
    void noteInstancesOfAnyClassMayReachFormatters();
    void noteInstanceHasEscaped(BuiltinClassID);
    void noteWriteThroughUnseenByProgram(PropertyKeyID);
    bool isMarked(BuiltinClassID, BuiltinMark) const;
    void noteHalfOfWhatShowsAnswersOfMethods(bool& half);
    bool linkTellsWhatHandlersAreHanded(BuiltinID canonical) const { return !anyRowMayApply(canonical) && !callingUsesOf(m_builtins.at(canonical).kind).isEmpty() && !m_isCalledByHost.quickGet(canonical.index()); }
    bool linkTellsWhichRowsApply(BuiltinID canonical) const { return m_builtins.at(canonical).overloadCount > 1 && linkTellsWhatHandlersAreHanded(canonical) && !m_hasCallThatLinkLeftToUnseen.get(canonical.index()); }
    bool noCallAppliesRow(BuiltinID canonical, BuiltinOverloadID row) const
    {
        BuiltinCallMode mode = m_builtins.at(row).mode;
        return (mode == BuiltinCallMode::Call || mode == BuiltinCallMode::Construct) && linkTellsWhichRowsApply(canonical) && !m_rowsThatLinkApplies.get(row.index());
    }
    bool anyRowMayApply(BuiltinID canonical) const { return m_isOutOfSight.quickGet(canonical.index()) || m_isCalledWithoutRow.quickGet(canonical.index()) || m_uses[canonical.index()].contains(BuiltinUse::Escaped); }
    void noteConstruction(BuiltinID builtin) { noteUse(m_builtins.canonicalID(builtin), BuiltinUse::Constructed); }
    bool mayRun(BuiltinID canonical) const;
    bool mayRunOrHasObjects(BuiltinID canonical) const;
    bool liesInModuleThatProgramHolds(BuiltinID canonical) const;
    void takeBuiltinsThatCameOutOfSight(Vector<BuiltinID>&);
    bool objectsMayHaveMethodCalledWithStartupLoads() const { return m_objectsMayHaveMethodCalledWithStartupLoads; }
    bool climberMayBeHandedWhatNobodyKnows() const { return m_climberMayBeHandedWhatNobodyKnows; }
    bool isOutOfSight(BuiltinID canonical) const { return m_uses[canonical.index()].contains(BuiltinUse::Escaped) || m_isOutOfSight.quickGet(canonical.index()); }
    bool machineCodeMayBeLoaded() const;
    bool isAskedForOtherNames(BuiltinID canonical) const { return m_lightFormsAskedForMore.get(canonical.index()) || m_uses[canonical.index()].contains(BuiltinUse::Escaped) || m_isOutOfSight.quickGet(canonical.index()); }
    const BuiltinCondition* lightFormOf(BuiltinID canonical) const { return m_source.testsReplaceBuiltins() ? nullptr : m_rows.lightFormOf(m_builtins, canonical); }
    void settle();
    void takeBuiltinsThatCameIntoReach(Vector<BuiltinID>&);
    bool hasRecordOfUnseenCode() const { return !m_program.unseenCode.isEmpty(); }
    bool loaderOfHostMayRun() const
    {
        return std::ranges::any_of(m_program.unseenCode, [&](const UnseenCodeRecord& record) {
            return !m_defersRuntimeNames || record.cause != UnseenCodeCause::EvaluatesText;
        });
    }
    bool hostIsAskedForValueOfModule() const { return m_hostIsAskedForValueOfModule; }
    bool unseenCodeCanRun() const { return hasRecordThat(admitsUnknownJavaScriptInProgramFacts); }
    bool loaderOfMachineCodeIsInUse() const
    {
        auto isInUse = [&](BuiltinID builtin) {
            return builtin && m_uses[m_builtins.canonicalID(builtin).index()].containsAny(usesOfObjectItselfInProgramFacts);
        };
        return std::ranges::any_of(m_builtins.loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
            return isInUse(loader.callee) || isInUse(loader.named);
        });
    }
    bool m_leavesOutDirectEvals { false };
    void deferRuntimeNames() { m_defersRuntimeNames = true; }
    bool defersRuntimeNames() const { return m_defersRuntimeNames; }
    bool defersMembersOf(BuiltinID canonicalHolder) const { return m_defersRuntimeNames && (canonicalHolder == m_globalObject); }
    bool hasDeferredMembers() const { return m_defersRuntimeNames && m_hasDeferredMembers; }
    BitVector classesWithObjects() const
    {
        BitVector classes(m_builtins.classCount());
        for (unsigned index = 1; index < m_builtins.classCount(); ++index) {
            if (objectsMayExist(BuiltinClassID(index)))
                classes.quickSet(index);
        }
        return classes;
    }
    Vector<ModuleID> takeModulesSpelledInTexts() { return std::exchange(m_modulesSpelledInTexts, { }); }
    BitVector allowRuntimeNames();
    BitVector builtinsThatCodeInSightUses() const;
    bool isReachableByName(BuiltinID member) const;
    template<typename Functor>
    bool programLetsUnseenCodeRunWhere(const Functor& counts) const
    {
        return std::ranges::any_of(m_program.unseenCode, [&](const UnseenCodeRecord& record) {
            if (!admitsUnknownJavaScriptInProgramFacts(record.cause))
                return false;
            if (record.witness.function == Wire::none)
                return record.cause == UnseenCodeCause::OptionAdmitsCode;
            return !isLibraryCode(m_source, m_source.at(FunctionID(record.witness.function))) && counts(record.witness);
        });
    }
    bool programLetsUnseenCodeRun() const
    {
        return programLetsUnseenCodeRunWhere([](const Witness&) {
            return true;
        });
    }

private:
    using Finding = FindingOfProgramFacts;

    static OptionSet<BuiltinUse> callingUsesOf(BuiltinKind);

    bool hasRecordThat(bool (*holdsOf)(UnseenCodeCause)) const { return std::ranges::any_of(m_program.unseenCode, holdsOf, &UnseenCodeRecord::cause); }
    bool isReached(FunctionID id) const { return m_inputs.linked.function(id).isReached; }

    void take(std::span<const Finding>);
    void takeNamesOf(FunctionID);

    void noteObjectsMayExist(BuiltinClassID);
    void noteTypeIsMade(BuiltinTypeID, BuiltinOrigin);
    bool hostMakesInstancesOf(BuiltinClassID) const;
    bool objectsMayExist(BuiltinClassID) const;
    void noteClassesBelowWhoseFeaturesMakerNames(BuiltinID maker, BuiltinTypeID);
    void noteRowOfHostNames(BuiltinClassID);
    void noteWhatRowsMake(BuiltinID canonical);
    void noteKeysThatRowNames(const BuiltinOverload&);
    uint16_t stateOfRowsOf(BuiltinID canonical) const;
    void noteWhatBodyMakes(FunctionID);
    void lookThroughPrototypesOfClassesWithObjects();
    bool mayExport(const Module&, StringID name) const;
    void useBuiltinsThatHostHandsExportsTo();
    void followModules();
    void followRequestsOfEvaluatedModules();
    void evaluate(ModuleID, const Witness&);

    void letGo(BuiltinID canonical, const Witness&);
    void lookThrough(BuiltinID canonicalHolder);
    void spreadBelow(BuiltinID canonicalHolder);
    void findMembersOffUnknownValues(BuiltinID canonicalHolder);
    void noteWhereNameOfRunIsCalled(const Finding&);
    bool formatterWalksPrototype(BuiltinID canonicalHolder) const;
    bool formatterMayComeByWhatStandsBelow(BuiltinID canonicalHolder) const;
    void noteGettersThatFormatterRuns(BuiltinID canonicalHolder);
    void followWhatIsLetGo();
    bool standsInTextOfProgram(const Witness& witness) const { return hasFunction(witness) && !isLibraryCode(m_source, m_source.at(FunctionID(witness.function))); }
    bool isPreferredWitness(const Witness& witness, const Witness& other) const
    {
        bool isOfProgram = standsInTextOfProgram(witness);
        return isOfProgram != standsInTextOfProgram(other) ? isOfProgram : standsEarlierInTextInProgramFacts(witness, other);
    }
    void keepPlaceOf(BuiltinID canonical, Witness& kept, const Witness& candidate) const
    {
        if (!m_rows.areEvents.quickGet(canonical.index()))
            keepLowerWitnessInProgramFacts(kept, candidate);
        else if (isPreferredWitness(candidate, kept))
            kept = candidate;
    }
    void uniteUsesOfSameObject();

    void add(Vector<UnseenCodeRecord>&, UnseenCodeCause, BuiltinID, const Witness&) const;
    void forEachCallInTextOfProgram(BuiltinID canonicalCallee, NOESCAPE const Invocable<void(const Witness&)> auto&) const;
    void addAtCallsInTextOfProgram(Vector<UnseenCodeRecord>&, UnseenCodeCause, BuiltinID named, BuiltinID canonicalCallee) const;
    std::optional<UnseenCodeCause> causeOfLettingCodeIn(BuiltinID canonical) const;
    void listBuiltinsThatLetCodeIn();
    void listLoadsOfUnseenCode();
    void listLoadsOfMachineCode();
    void listBuiltinUses();
    Vector<BuiltinUseRecord> usesWithWhatNobodyNamesAsEscaped() const;
    BitVector builtinsFoundByNameAlone() const;
    BitVector builtinsThatCodeInSightHandsOn() const;
    void layPlacesWhereProgramReadsNames();
    void listHoldersWhoseKeysAreNeverListed();
    void lockMembersThatNobodyWrites();

    bool isConstantLocation(BuiltinID member) const;
    BuiltinID builtinReadByPath(const BodyIndex&, ValueID) const;
    BuiltinID builtinSpelledByPath(const BodyIndex&, ValueID) const;
    bool isSurelyNot(const BodyIndex&, ValueID, BuiltinID member) const;
    bool isGuardedByFailingTest(const BodyIndex&, ValueID site, BuiltinID member) const;
    bool changesMember(const BodyIndex&, ValueID descriptor, BuiltinID member, bool& isCertain) const;
    bool holdsNoCodeOfProgram(const BodyIndex&, ValueID) const;
    bool namesBuiltinThatEndsProcess(const BodyIndex&, const BodyValue&) const;
    bool codeThatNobodyNamesMayLeave();
    enum class ThrowsOfOperations : bool { AreLeftOut, Count };
    bool mayLeave(FunctionID, const BodyIndex&, const BodyValue&, ThrowsOfOperations);
    bool mayThrowByItself(const BodyIndex&, const BodyValue&) const;
    bool bodyMayLeave(FunctionID, ThrowsOfOperations = ThrowsOfOperations::AreLeftOut);
    Vector<ModuleID> evaluationOrder() const;
    bool throwInFrontMayBeSurvived(const Witness&);
    bool everyRunComesTo(const Witness&);
    std::optional<UserDiagnosticKind> refusalOf(const BodyIndex&, const Witness&, BuiltinID holder, PropertyKeyID, BuiltinPropertyActionKind, ValueID brought, bool throwsWhereRefused);
    std::optional<UserDiagnosticKind> refusalOfWriteAt(const Witness&, BuiltinID followed = { });
    std::optional<UserDiagnosticKind> refusalOfCopy(const BodyIndex&, const Witness&, BuiltinID holder, BuiltinPropertyActionKind, ValueID source, bool throwsWhereRefused);
    template<typename Functor> bool forEachPropertyOfWrittenLiteral(const BodyIndex&, ValueID, ValueID handedOverAt, const Functor&) const;
    const DataNode::Object* objectOfConstantsBehind(const BodyIndex&, ValueID) const;
    bool isHandedOverOnce(const BodyIndex&, ValueID made, Vector<PropertyKeyID, 2>& namesTakenAway) const;
    bool isPartThatBringsValueOrAccessor(PropertyKeyID) const;
    bool bringsValueOrAccessor(const DataNode::Object& descriptor) const;
    void reportWritesThatLocksRefuse();
    bool isNameOfModuleWrittenOut(const BodyIndex&, const BodyValue& call, BuiltinID loader) const;
    void reportLoadsUnderNamesThatNobodyHasRead();
    bool isMainModulePathReplaceable() const;
    bool needsOneInstanceAtMost() const;
    void listLibraryCodeLacked();
    void listLocaleUses();
    void noteStartWithoutName(const Witness&);
    void listWorkers();
    bool mayReadRecordOfLastMatch() const;

    const InputsOfProgramFacts& m_inputs;
    const Program& m_source;
    const BuiltinTable& m_builtins;
    const RowsOfProgramFacts m_rows;
    const RefusedCode m_refused;
    const BuiltinID m_globalObject;
    FactTables& m_tables;
    ProgramFactTables& m_program;

    Vector<OptionSet<BuiltinUse>> m_uses;
    Vector<Witness> m_witnesses;
    BitVector m_isNamedByOperation;
    Vector<Witness> m_witnessesOfEscapes;
    BitVector m_isLetGo;
    bool m_followsClimbsFromWhatIsLetGo { false };
    bool m_climberMayBeHandedWhatNobodyKnows { false };
    BitVector m_isOutOfSight;
    BitVector m_isCalledOutOfSightOfLink;
    BitVector m_isCalledWithoutRow;
    BitVector m_isLetGoByOperation;
    BitVector m_isCalledWhereLinkHasNoCall;
    BitVector m_mayBeHandedOperandsNobodyKnows;
    BitVector m_isCalledByHost;
    bool m_formattersAreHandedObjects { false };
    bool m_instancesLoseTheirConstructor { false };
    bool m_formattersMayCallMethodsOfInstances { false };
    BitVector m_classesWhoseInstancesMayReachFormatters;
    bool m_instancesOfAnyClassMayReachFormatters { false };
    bool m_programMayLayMethodOnWhatNobodyFollows { false };
    bool m_objectsThatShowWhatTheirMethodAnswersMayExist { false };
    bool m_makesWhatLinkFollows { false };
    BitVector m_isLetGoByLink;
    HashMap<uint32_t, Vector<Finding, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_findingsThatWaitForLink;
    BitVector m_typesMadeOutOfSight;

    BitVector m_hasComeOutOfSight;
    BytesSharedByThreads m_classesWithObjects;
    BitVector m_typesMade;
    BitVector m_typesMadeByHost;
    BitVector m_typesMadeByConstructionsInSight;
    Vector<uint16_t> m_stateWhenRowsWereRead;
    BitVector m_rowsThatLinkApplies;
    BitVector m_hasCallThatLinkLeftToUnseen;
    bool m_linkIsAtWork { true };

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_builtinsWithSeveralRowsCalledAt;
    Vector<uint64_t> m_callsListedSinceLinkTookThemOver;
    Vector<BuiltinClassID> m_classOfPrototype;
    bool m_objectsOfAnyClassMayExist { false };
    bool m_classesWithObjectsHaveGrown { true };
    bool m_objectsMayHaveMethodCalledWithStartupLoads { false };
    bool m_notesWhatConstructionsInSightMake { false };
    BitVector m_lightFormsAskedForMore;
    BitVector m_isLookedThrough;
    BitVector m_isReadUnderComputedKeys;
    BitVector m_mayBePassedOptionThatRunsCode;
    Vector<BuiltinID> m_holdersToSpreadBelow;
    Vector<BuiltinID> m_holdersToLookThrough;
    BitVector m_isFoundByName;
    BitVector m_isFoundOffValuesNobodyKnows;
    BitVector m_isReadWhereLinkSees;
    BitVector m_isFoundByWhatCodeWrote;
    BitVector m_isCalledByCodeInSight;
    BitVector m_isSpelledInTexts;
    BitVector m_modulesImportedByProgram;
    bool m_eventsAreWithheld { true };
    bool m_defersRuntimeNames { false };
    bool m_hasDeferredMembers { false };
    Vector<ModuleID> m_modulesSpelledInTexts;
    BitVector m_keysAreListedInSight;
    Vector<BuiltinID, 32> m_holdersOfKeysThatPrinterLooksUnder;
    BitVector m_namesThatRowsLayOnBuiltins;
    Vector<Finding> m_readsOffObjectsOfModulesThatNoMemberAnswers;

    BitVector m_namesRead;
    BitVector m_namesCalled;
    BitVector m_namesCalledWithOperands;
    BitVector m_namesCalledByNativeCodeOffUnseenObjects;
    BitVector m_classesThatRowsOfHostName;
    BitVector m_namesSpelledByProgram;
    bool m_readsUnderComputedKeys { false };
    bool m_readsUnderNamesThatLoopsGave { false };
    Vector<BuiltinID, 2> m_makersOfObjectsThatListWhatTheyLike;
    bool m_loopsMayGiveAnyName { false };
    Vector<FunctionID> m_functionsThatAnswerEveryNameOnInstances;
    WritesOnBuiltinsInProgramFacts m_writes;
    BitVector m_namesOfProtocols;
    BitVector m_keysReadOnlyByRowsThatNameThem;
    BitVector m_keysNamedByRowsThatMayRun;
    bool m_keysNamedByRowsThatMayRunHaveGrown { false };
    bool m_nativeCodeMayHoldObjects { false };
    bool m_registryMayGiveKeyOfPrinter { false };
    BitVector m_hasComeIntoReach;
    BitVector m_constructorIsListedForObjects;

    BitVector m_modulesEvaluated;
    Vector<ModuleID> m_modulesToFollow;

    Vector<Witness> m_writesOfProgramOnBuiltins;
    Vector<Witness> m_loadsOfProgramByName;
    BitVector m_bodiesBeingAsked;
    std::array<BitVector, 2> m_bodiesThatStay;
    std::optional<bool> m_codeThatNobodyNamesMayLeave;
    BitVector m_partOfClassIsUsed;
    Vector<Finding> m_callsOfEventsNamedInBodies;
    Vector<Witness> m_whereProgramReadsNames;
    Vector<Finding> m_callsUnderComputedKeys;
    Witness m_whereNameOfRunIsCalled { noWitness };
    Vector<std::pair<BuiltinID, Witness>, 4> m_whereNameOfRunIsCalledOffWrittenPrimitives;
    Vector<Witness> m_startsWithoutName;
    bool m_loaderIsHandedToText { false };
    bool m_hostIsAskedForValueOfModule { false };
    Witness m_whereConstructorOfWorkersIsLetGo { noWitness };
};

OptionSet<BuiltinUse> BuilderOfProgramFacts::callingUsesOf(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Constructor:
        return callingUsesInProgramFacts;
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return BuiltinUse::Called;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuilderOfProgramFacts::BuilderOfProgramFacts(const InputsOfProgramFacts& inputs, FactTables& tables)
    : m_inputs(inputs)
    , m_source(inputs.program)
    , m_builtins(inputs.program.builtins())
    , m_rows(m_builtins, inputs.build.options().featuresRetainedByName)
    , m_refused(codeRefusedInProgramFacts(inputs.program, inputs.build))
    , m_globalObject(m_builtins.canonicalID(Builtins::GlobalThis))
    , m_tables(tables)
    , m_program(tables.program)
    , m_isNamedByOperation(m_builtins.builtinCount())
    , m_isLetGo(m_builtins.builtinCount())
    , m_isOutOfSight(m_builtins.builtinCount())
    , m_isCalledOutOfSightOfLink(m_builtins.builtinCount())
    , m_isCalledWithoutRow(m_builtins.builtinCount())
    , m_isLetGoByOperation(m_builtins.builtinCount())
    , m_isCalledWhereLinkHasNoCall(m_builtins.builtinCount())
    , m_mayBeHandedOperandsNobodyKnows(m_builtins.builtinCount())
    , m_isCalledByHost(m_builtins.builtinCount())
    , m_hasComeOutOfSight(m_builtins.builtinCount())
    , m_classesWithObjects(m_builtins.classCount())
    , m_keysAreListedInSight(m_builtins.builtinCount())
    , m_isLookedThrough(m_builtins.builtinCount())
    , m_isReadUnderComputedKeys(m_builtins.builtinCount())
    , m_mayBePassedOptionThatRunsCode(m_builtins.builtinCount())
    , m_isFoundByName(m_builtins.builtinCount())
    , m_isFoundOffValuesNobodyKnows(m_builtins.builtinCount())
    , m_isReadWhereLinkSees(m_builtins.builtinCount())
    , m_isFoundByWhatCodeWrote(m_builtins.builtinCount())
    , m_isCalledByCodeInSight(m_builtins.builtinCount())
    , m_isSpelledInTexts(m_builtins.builtinCount())
    , m_modulesImportedByProgram(m_builtins.builtinCount())
    , m_writes(inputs.program, m_refused, inputs.linked.lookups)
    , m_namesOfProtocols(m_builtins.nameCount())
    , m_keysReadOnlyByRowsThatNameThem(m_builtins.nameCount())
    , m_keysNamedByRowsThatMayRun(m_builtins.nameCount())
    , m_hasComeIntoReach(m_builtins.builtinCount())
    , m_constructorIsListedForObjects(m_builtins.classCount())
    , m_modulesEvaluated(inputs.program.modules.size())
{
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (m_builtins.canonicalID(BuiltinID(index)).index() == index && m_builtins.conditionOnInstancesThatAnswerEveryName(BuiltinID(index)))
            m_makersOfObjectsThatListWhatTheyLike.append(BuiltinID(index));
    }
    m_uses.fill(OptionSet<BuiltinUse>(), m_builtins.builtinCount());
    m_stateWhenRowsWereRead.fill(0, m_builtins.builtinCount());
    m_witnesses.fill(noWitness, m_builtins.builtinCount());
    m_witnessesOfEscapes.fill(noWitness, m_builtins.builtinCount());
    for (Protocol protocol : allMembersOf<Protocol>) {
        for (BuiltinName name : m_builtins.keysReadBy(protocol))
            m_namesOfProtocols.set(name.index());
    }
    for (const BuiltinMarkedEntry& marked : m_builtins.marked(BuiltinMark::IsKeyReadOnlyByRowsThatNameIt)) {
        if (BuiltinName name = m_builtins.nameOfSymbol(marked.builtin))
            m_keysReadOnlyByRowsThatNameThem.set(name.index());
    }

    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        const Builtin& entry = m_builtins.at(BuiltinID(index));
        if (entry.holder && std::ranges::contains(m_builtins.namesOfHostMethodsCallingStartupLoads(), entry.name))
            m_holdersOfKeysThatPrinterLooksUnder.append(m_builtins.canonicalID(entry.holder));
    }
    m_classOfPrototype.fill(BuiltinClassID(), m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
        BuiltinClassID builtinClass(index);
        if (BuiltinID prototype = m_builtins.at(builtinClass).prototype)
            m_classOfPrototype[m_builtins.canonicalID(prototype).index()] = builtinClass;
        if (m_builtins.isSameOrSubclassOf(builtinClass, BuiltinClasses::Error))
            noteObjectsMayExist(builtinClass);
    }
    for (BuiltinClassID builtinClass : { BuiltinClasses::Object, BuiltinClasses::Function, BuiltinClasses::Array, BuiltinClasses::Arguments, BuiltinClasses::String, BuiltinClasses::Number, BuiltinClasses::Boolean, BuiltinClasses::Symbol, BuiltinClasses::BigInt })
        noteObjectsMayExist(builtinClass);
}

void BuilderOfProgramFacts::add(Vector<UnseenCodeRecord>& records, UnseenCodeCause cause, BuiltinID builtin, const Witness& witness) const
{
    bool eachPlaceHasRecord = hasFlag(m_inputs.build.options().reports, Wire::ReportSet::Uses);
    uint32_t number = builtin ? builtin.index() : Wire::none;
    for (UnseenCodeRecord& record : records) {
        if (record.cause != cause || record.builtin != number)
            continue;
        if (!eachPlaceHasRecord) {
            keepLowerWitnessInProgramFacts(record.witness, witness);
            return;
        }
        if (!hasFunction(witness) || (record.witness.function == witness.function && record.witness.value == witness.value))
            return;
        if (!hasFunction(record.witness)) {
            record.witness = witness;
            return;
        }
    }
    records.append(UnseenCodeRecord { cause, number, witness });
}

void BuilderOfProgramFacts::forEachCallInTextOfProgram(BuiltinID canonicalCallee, NOESCAPE const Invocable<void(const Witness&)> auto& functor) const
{
    for (const LinkedCallOfBuiltin& listed : m_inputs.linked.lookups.callsOfBuiltinsAtSites) {
        if (listed.row != canonicalCallee || !listed.caller || isLibraryCode(m_source, m_source.at(listed.caller)))
            continue;
        const BodyIndex* index = m_inputs.bodies.find(listed.caller);
        if (!index || !listed.site || listed.site.index() >= index->numberOfValues() || index->isParameter(listed.site))
            continue;
        functor(Witness { listed.caller.index(), listed.site.index(), m_source.at(listed.caller).locationOf(index->value(listed.site)) });
    }
    for (const Finding& call : m_callsOfEventsNamedInBodies) {
        if (call.subject == canonicalCallee.index() && standsInTextOfProgram(call.witness))
            functor(call.witness);
    }
}

void BuilderOfProgramFacts::addAtCallsInTextOfProgram(Vector<UnseenCodeRecord>& records, UnseenCodeCause cause, BuiltinID named, BuiltinID canonicalCallee) const
{
    bool isCalledInTextOfProgram = false;
    forEachCallInTextOfProgram(canonicalCallee, [&](const Witness& call) {
        isCalledInTextOfProgram = true;
        add(records, cause, named, call);
    });
    if (isCalledInTextOfProgram)
        return;
    const Witness& whereUsed = m_witnesses[canonicalCallee.index()];
    add(records, cause, named, standsInTextOfProgram(whereUsed) ? whereUsed : noWitness);
}

void BuilderOfProgramFacts::noteStartWithoutName(const Witness& witness)
{
    m_startsWithoutName.append(witness);
}

void BuilderOfProgramFacts::noteFindingsIn(std::span<const FunctionID> functions)
{
    static constexpr size_t distanceOfPlace = 16;
    static constexpr size_t distanceOfFindings = 8;
    for (size_t turn = 0; turn < functions.size(); ++turn) {
        if (turn + distanceOfPlace < functions.size())
            m_inputs.looks.prefetchPlaceOfFindingsOf(functions[turn + distanceOfPlace]);
        if (turn + distanceOfFindings < functions.size())
            m_inputs.looks.prefetchFindingsOf(functions[turn + distanceOfFindings]);
        noteFindingsIn(functions[turn]);
    }
}

void BuilderOfProgramFacts::noteFoundWhereFlowBroughtHolder(BuiltinID builtin, FunctionID reader)
{
    Finding finding;
    finding.kind = Finding::Kind::UseOfBuiltin;
    finding.detail = static_cast<uint8_t>(OptionSet<BuiltinUse>(BuiltinUse::Escaped).toRaw());
    finding.witness = { reader.index(), Wire::none, noSourceLocation };
    finding.isOfProgram = standsInTextOfProgram(finding.witness);
    finding.waitsForLink = true;
    finding.subject = m_builtins.canonicalID(builtin).index();
    take(std::span<const Finding>(&finding, 1));
}

void BuilderOfProgramFacts::noteStandsAboveClass(BuiltinID builtin, FunctionID constructor)
{
    BuiltinID canonical = m_builtins.canonicalID(builtin);
    Witness witness { constructor.index(), Wire::none, noSourceLocation };
    Vector<Finding, 6> findings;
    auto find = [&](Finding::Kind kind, uint8_t detail, uint32_t subject) {
        Finding finding;
        finding.kind = kind;
        finding.detail = detail;
        finding.isOfProgram = standsInTextOfProgram(witness);
        finding.subject = subject;
        finding.witness = witness;
        findings.append(finding);
    };
    find(Finding::Kind::UseOfBuiltin, static_cast<uint8_t>(OptionSet<BuiltinUse>(BuiltinUse::Constructed).toRaw()), canonical.index());
    findings.last().isCalledOutOfSightOfLink = true;
    findings.last().isCalledWithoutRow = true;
    find(Finding::Kind::StandsAboveClass, 0, canonical.index());
    if (m_rows.areEvents.quickGet(canonical.index()) || isLoaderOfMachineCodeInProgramFacts(m_builtins, canonical))
        find(Finding::Kind::CallOfEvent, 0, canonical.index());
    if (m_rows.startWorkers.quickGet(canonical.index()))
        find(Finding::Kind::WorkerStartWithoutName, 0, Wire::none);
    if (optionThatRunsCodeInProgramFacts(m_builtins, canonical))
        find(Finding::Kind::OptionThatRunsCode, 0, canonical.index());
    if (m_builtins.conditionOnInstancesThatAnswerEveryName(canonical))
        find(Finding::Kind::InstanceThatMayAnswerEveryName, 0, canonical.index());
    take(findings.span());
}

void BuilderOfProgramFacts::noteLetGoByLink(BuiltinID builtin)
{
    BuiltinID canonical = m_builtins.canonicalID(builtin);
    if (m_isLetGoByLink.set(canonical.index()))
        return;
    Vector<Finding, 1> released = m_findingsThatWaitForLink.take(canonical.index());
    take(released.span());
}

void BuilderOfProgramFacts::take(std::span<const Finding> findings)
{
    for (const Finding& found : findings) {
        Finding finding = found;
        if (finding.waitsForLink && !m_isLetGoByLink.get(m_builtins.canonicalID(BuiltinID(finding.subject)).index())) {
            m_findingsThatWaitForLink.add(m_builtins.canonicalID(BuiltinID(finding.subject)).index(), Vector<Finding, 1>()).iterator->value.append(finding);
            finding.detail = static_cast<uint8_t>(OptionSet<BuiltinUse>(BuiltinUse::Tested).toRaw());
        }
        m_writes.take(finding);
        switch (finding.kind) {
        case Finding::Kind::UseOfBuiltin:
        case Finding::Kind::ReadOfAnyMember: {
            auto uses = OptionSet<BuiltinUse>::fromRaw(finding.detail);
            BuiltinID canonical = m_builtins.canonicalID(BuiltinID(finding.subject));
            m_uses[finding.subject].add(uses);
            keepPlaceOf(canonical, m_witnesses[finding.subject], finding.witness);
            if (!(uses - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty()) {
                m_isNamedByOperation.quickSet(finding.subject);
                m_isNamedByOperation.quickSet(canonical.index());
            }
            if (finding.isCalledWhereLinkTakesAnotherCallee)
                m_isCalledWhereLinkHasNoCall.quickSet(canonical.index());
            if (finding.isCalledOutOfSightOfLink) {
                m_isCalledOutOfSightOfLink.quickSet(canonical.index());
                if (finding.isCalledWithoutRow)
                    m_isCalledWithoutRow.quickSet(canonical.index());
            }
            if (uses.containsAny(callingUsesInProgramFacts) && !finding.isCalledWithoutRow && m_builtins.at(canonical).overloadCount > 1)
                noteCallWhoseRowsLinkTells(wordOfCall(finding.witness.function, finding.witness.value), canonical);
            if (uses.contains(BuiltinUse::Escaped)) {
                if (m_rows.startWorkers.quickGet(canonical.index()) && isPreferredWitness(finding.witness, m_whereConstructorOfWorkersIsLetGo))
                    m_whereConstructorOfWorkersIsLetGo = finding.witness;
                m_isLetGoByOperation.quickSet(canonical.index());
                letGo(canonical, finding.witness);
            }
            break;
        }
        case Finding::Kind::WriteOnBuiltin: {
            BuiltinID holder = m_builtins.canonicalID(BuiltinID(finding.subject));
            BuiltinID member = finding.key == Wire::none ? BuiltinID() : findOwnMember(m_source, holder, PropertyKeyID(finding.key));
            if (member)
                m_uses[member.index()].add(BuiltinUse::Written);
            else if (finding.key == Wire::none && !(finding.detail & writeOnlyDeletesInProgramFacts))
                noteStoreUnderKeyNobodyKnowsOn(holder);
            if ((finding.detail & writeOnlyDeletesInProgramFacts) && (member || finding.key == Wire::none))
                noteDeletionOfMemberOf(holder);
            if (finding.isOfProgram) {
                m_writesOfProgramOnBuiltins.append(finding.witness);
                if (finding.detail & writeIsNamedByRowInProgramFacts)
                    m_namesThatRowsLayOnBuiltins.set(finding.key);
            }
            break;
        }
        case Finding::Kind::PrototypeChangeOfBuiltin:
            if (finding.isOfProgram)
                m_writesOfProgramOnBuiltins.append(finding.witness);
            break;
        case Finding::Kind::NameReadOffUnknownValue:
            if (BuiltinName name = m_source.builtinNameOf(PropertyKeyID(finding.subject))) {
                if (m_whereProgramReadsNames.isEmpty())
                    m_whereProgramReadsNames.fill(noWitness, m_builtins.nameCount());
                keepLowerWitnessInProgramFacts(m_whereProgramReadsNames[name.index()], finding.witness);
            }
            break;
        case Finding::Kind::PartOfClassIsUsed:
            m_partOfClassIsUsed.set(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            break;
        case Finding::Kind::ReadThatNoRowAnswers:
            if (finding.detail) {
                m_readsOffObjectsOfModulesThatNoMemberAnswers.append(finding);
                break;
            }
            m_objectsMayHaveMethodCalledWithStartupLoads = true;
            break;
        case Finding::Kind::NamespaceIsGivenByImportCall:
            noteInstancesLoseTheirConstructor();
            break;
        case Finding::Kind::KeysOfBuiltinListed:
            m_keysAreListedInSight.quickSet(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            break;
        case Finding::Kind::StandsAboveClass:
            if (m_builtins.has(BuiltinMark::InstancesShowWhatTheirMethodAnswersToFormatters, m_builtins.canonicalID(BuiltinID(finding.subject))))
                noteObjectHandedToFormatter(false);
            lookThrough(m_builtins.canonicalID(BuiltinID(finding.subject)));
            m_isOutOfSight.quickSet(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            m_mayBeHandedOperandsNobodyKnows.quickSet(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            break;
        case Finding::Kind::LightFormIsAskedForMore:
            m_lightFormsAskedForMore.set(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            break;
        case Finding::Kind::OptionThatRunsCode:
            m_mayBePassedOptionThatRunsCode.quickSet(finding.subject);
            break;
        case Finding::Kind::InstanceThatMayAnswerEveryName:
            if (finding.key == Wire::none)
                m_objectsMayHaveMethodCalledWithStartupLoads = true;
            else
                m_functionsThatAnswerEveryNameOnInstances.append(FunctionID(finding.key));
            break;
        case Finding::Kind::UnseenCode:
            if (m_leavesOutDirectEvals && static_cast<UnseenCodeCause>(finding.detail) == UnseenCodeCause::EvaluatesText)
                break;
            add(m_program.unseenCode, static_cast<UnseenCodeCause>(finding.detail), { }, finding.witness);
            if (static_cast<UnseenCodeCause>(finding.detail) == UnseenCodeCause::LoadsModuleByName)
                m_loadsOfProgramByName.append(finding.witness);
            break;
        case Finding::Kind::CallUnderComputedKey:
            m_callsUnderComputedKeys.append(finding);
            break;
        case Finding::Kind::BuiltinSpelledInText:
            m_isSpelledInTexts.quickSet(finding.subject);
            break;
        case Finding::Kind::CallOfEvent:
            m_callsOfEventsNamedInBodies.append(finding);
            break;
        case Finding::Kind::WorkerEntry:
            m_program.workerEntries.append(WorkerEntry { StringID(finding.subject), !!finding.detail, finding.witness });
            break;
        case Finding::Kind::WorkerStartWithoutName:
            noteStartWithoutName(finding.witness);
            break;
        case Finding::Kind::LoaderHandedToText:
            m_loaderIsHandedToText = true;
            break;
        case Finding::Kind::WorkerStartFromURL:
            m_program.workerStartsFromURLs.append(finding.witness);
            break;
        case Finding::Kind::NamedModule:
            if (finding.detail) {
                m_modulesSpelledInTexts.append(ModuleID(finding.subject));
                RuntimeModuleID spelled = m_source.at(ModuleID(finding.subject)).runtimeModule;
                if (BuiltinID object = spelled ? m_builtins.namespaceOf(spelled) : BuiltinID())
                    m_isSpelledInTexts.quickSet(m_builtins.canonicalID(object).index());
            } else
                evaluate(ModuleID(finding.subject), finding.witness);
            break;
        }
    }
}

void BuilderOfProgramFacts::noteTypeHandedToHandler(BuiltinTypeID type)
{
    SetForScope makesWhatLinkFollows(m_makesWhatLinkFollows, true);
    noteTypeIsMade(type, BuiltinOrigin::Host);
}

bool BuilderOfProgramFacts::isMarked(BuiltinClassID builtinClass, BuiltinMark mark) const
{
    BuiltinID constructor = m_builtins.at(builtinClass).constructor;
    return constructor && m_builtins.has(mark, m_builtins.canonicalID(constructor));
}

void BuilderOfProgramFacts::noteHalfOfWhatShowsAnswersOfMethods(bool& half)
{
    half = true;
    if (m_programMayLayMethodOnWhatNobodyFollows && m_objectsThatShowWhatTheirMethodAnswersMayExist)
        noteObjectHandedToFormatter(false);
}

void BuilderOfProgramFacts::noteWriteThroughUnseenByProgram(PropertyKeyID key)
{
    if (!key || std::ranges::contains(m_builtins.keysReadBy(Protocol::ToJSON), m_source.builtinNameOf(key)))
        noteHalfOfWhatShowsAnswersOfMethods(m_programMayLayMethodOnWhatNobodyFollows);
}

void BuilderOfProgramFacts::noteInstanceHasEscaped(BuiltinClassID builtinClass)
{
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (isMarked(current, BuiltinMark::InstancesShowWhatTheirMethodAnswersToFormatters))
            noteObjectHandedToFormatter(false);
    }
}

void BuilderOfProgramFacts::noteInstancesMayReachFormatters(BuiltinClassID builtinClass)
{
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (m_classesWhoseInstancesMayReachFormatters.set(current.index()))
            return;
        BuiltinID prototype = m_builtins.at(current).prototype;
        if (prototype && m_formattersMayCallMethodsOfInstances && m_isLookedThrough.quickGet(m_builtins.canonicalID(prototype).index()))
            m_holdersToLookThrough.append(m_builtins.canonicalID(prototype));
    }
}

void BuilderOfProgramFacts::noteInstancesOfAnyClassMayReachFormatters()
{
    if (std::exchange(m_instancesOfAnyClassMayReachFormatters, true) || !m_formattersMayCallMethodsOfInstances)
        return;
    for (size_t index : m_isLookedThrough)
        m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
}

void BuilderOfProgramFacts::noteObjectsMayExist(BuiltinClassID builtinClass)
{
    if (!m_makesWhatLinkFollows)
        noteInstancesMayReachFormatters(builtinClass);
    BuiltinID constructor = m_builtins.at(builtinClass).constructor;
    if (!m_notesWhatConstructionsInSightMake && constructor && m_builtins.conditionOnInstancesThatAnswerEveryName(m_builtins.canonicalID(constructor)))
        m_objectsMayHaveMethodCalledWithStartupLoads = true;
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (m_classesWithObjects.at(current.index()))
            return;
        m_classesWithObjects.set(current.index(), true);
        if (isMarked(current, BuiltinMark::InstancesShowWhatTheirMethodAnswersToFormatters))
            noteHalfOfWhatShowsAnswersOfMethods(m_objectsThatShowWhatTheirMethodAnswersMayExist);
        m_classesWithObjectsHaveGrown = true;
    }
}

bool BuilderOfProgramFacts::hostMakesInstancesOf(BuiltinClassID builtinClass) const
{
    if (builtinClass == BuiltinClasses::IntlSegments || builtinClass == BuiltinClasses::IntlSegmentIterator)
        return false;
    BuiltinID constructor = m_builtins.at(builtinClass).constructor;
    BuiltinID holder = constructor ? m_builtins.at(constructor).holder : BuiltinID();
    return !holder || (m_builtins.canonicalID(holder) != m_builtins.canonicalID(Builtins::Intl) && m_builtins.canonicalID(holder) != m_builtins.canonicalID(Builtins::Temporal));
}

bool BuilderOfProgramFacts::objectsMayExist(BuiltinClassID builtinClass) const
{
    if (m_classesWithObjects.at(builtinClass.index()))
        return true;
    return m_objectsOfAnyClassMayExist && (!m_defersRuntimeNames || hostMakesInstancesOf(builtinClass));
}
void BuilderOfProgramFacts::noteTypeIsMade(BuiltinTypeID type, BuiltinOrigin origin)
{
    BitVector& looked = m_notesWhatConstructionsInSightMake ? m_typesMadeByConstructionsInSight : origin == BuiltinOrigin::Host ? m_typesMadeByHost : m_typesMade;
    if (!type)
        return;
    bool isKnown = looked.set(type.index());
    bool isKnownOutOfSight = m_makesWhatLinkFollows || m_typesMadeOutOfSight.set(type.index());
    if (isKnown && isKnownOutOfSight)
        return;
    const BuiltinType& made = m_builtins.at(type);
    if (origin == BuiltinOrigin::Host && made.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject }) && !made.flags.contains(BuiltinTypeFlag::IsMadeOfListedClasses) && !std::exchange(m_objectsOfAnyClassMayExist, true))
        m_classesWithObjectsHaveGrown = true;
    for (const BuiltinTypePart& part : made.parts) {
        if (part.builtinClass)
            noteObjectsMayExist(part.builtinClass);
        if (part.builtinClass && origin == BuiltinOrigin::Host)
            noteRowOfHostNames(part.builtinClass);
        if (part.kind == BuiltinTypePartKind::Layout) {
            for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
                noteTypeIsMade(field.type, origin);
        }
        for (const BuiltinSlotContents& contents : part.contents) {
            if (!m_builtins.at(contents.slot).holdsWhatIsThrown)
                noteTypeIsMade(contents.type, origin);
        }
    }
}

void BuilderOfProgramFacts::noteRowOfHostNames(BuiltinClassID builtinClass)
{
    if (m_classesThatRowsOfHostName.set(builtinClass.index()))
        return;
    BuiltinID prototype = m_builtins.at(builtinClass).prototype;
    if (prototype && m_isLookedThrough.quickGet(m_builtins.canonicalID(prototype).index()))
        m_holdersToLookThrough.append(m_builtins.canonicalID(prototype));
}

void BuilderOfProgramFacts::noteClassesBelowWhoseFeaturesMakerNames(BuiltinID maker, BuiltinTypeID type)
{
    std::span<const BuiltinFeatureUse> named = m_builtins.featureUsesOf(maker);
    if (!type || named.empty())
        return;
    for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
        if (!part.builtinClass || part.isOfClassItself || !m_builtins.hasClassBelow(part.builtinClass))
            continue;
        for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
            BuiltinClassID below(index);
            const BuiltinClass& record = m_builtins.at(below);
            if (record.origin != BuiltinOrigin::Host || !record.prototype || below == part.builtinClass || !m_builtins.isSameOrSubclassOf(below, part.builtinClass))
                continue;
            std::span<const BuiltinFeatureUse> needed = m_builtins.featureUsesOf(m_builtins.canonicalID(record.prototype));
            bool makerNamesThem = !needed.empty() && std::ranges::all_of(needed, [&](const BuiltinFeatureUse& line) {
                return std::ranges::contains(named, line.feature, &BuiltinFeatureUse::feature);
            });
            if (makerNamesThem) {
                noteObjectsMayExist(below);
                noteRowOfHostNames(below);
            }
        }
    }
}

bool BuilderOfProgramFacts::mayRun(BuiltinID canonical) const
{
    switch (m_builtins.at(canonical).kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return anyRowMayApply(canonical) || m_uses[canonical.index()].containsAny(callingUsesInProgramFacts);
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool BuilderOfProgramFacts::mayRunOrHasObjects(BuiltinID canonical) const
{
    BuiltinClassID made = m_builtins.at(canonical).instanceClass;
    return mayRun(canonical) || (made && (m_objectsOfAnyClassMayExist || m_classesWithObjects.at(made.index())));
}

bool BuilderOfProgramFacts::liesInModuleThatProgramHolds(BuiltinID canonical) const
{
    return !!compiledModuleWithObject(m_source, m_builtins.canonicalID(m_builtins.rootOf(canonical)));
}

uint16_t BuilderOfProgramFacts::stateOfRowsOf(BuiltinID canonical) const
{
    static constexpr uint16_t isRead = 1 << 15;
    static constexpr uint16_t anyRow = 1 << 14;
    static constexpr uint16_t everyHandlerCounts = 1 << 13;
    static constexpr uint16_t anyOperand = 1 << 12;
    static constexpr uint16_t refusalsMayPrint = 1 << 11;
    static_assert(numberOfEnumerators<BuiltinUse> <= 11);
    return static_cast<uint16_t>(isRead | (anyRowMayApply(canonical) ? anyRow : 0) | (linkTellsWhatHandlersAreHanded(canonical) ? 0 : everyHandlerCounts) | (m_mayBeHandedOperandsNobodyKnows.quickGet(canonical.index()) ? anyOperand : 0) | (m_instancesLoseTheirConstructor ? refusalsMayPrint : 0) | m_uses[canonical.index()].toRaw());
}

void BuilderOfProgramFacts::noteKeysThatRowNames(const BuiltinOverload& overload)
{
    for (size_t index : m_keysReadOnlyByRowsThatNameThem) {
        if (m_keysNamedByRowsThatMayRun.get(index))
            continue;
        BuiltinName name(static_cast<uint32_t>(index));
        auto asksFor = [&](const BuiltinParameter& parameter) {
            auto lists = [&](Protocol protocol, const BuiltinNameStretch& names) {
                return parameter.protocols.contains(protocol) && (names.empty() || std::ranges::contains(names, name));
            };
            return parameter.uses == OperandUses::all() || lists(Protocol::Named, parameter.namedMembers) || lists(Protocol::CallsNamed, parameter.calledMembers);
        };
        bool isNamed = overload.isUndeclared() || (overload.receiver && asksFor(*overload.receiver)) || std::ranges::any_of(overload.parameters, asksFor)
            || std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) { return std::ranges::contains(action.key.names, name); });
        if (!isNamed)
            continue;
        m_keysNamedByRowsThatMayRun.set(index);
        m_keysNamedByRowsThatMayRunHaveGrown = true;
    }
}

bool BuilderOfProgramFacts::noteEveryRowMayApplyTo(BuiltinID canonical)
{
    if (m_hasCallThatLinkLeftToUnseen.set(canonical.index()))
        return false;
    m_stateWhenRowsWereRead[canonical.index()] = 0;
    return true;
}

bool BuilderOfProgramFacts::noteCallLeftToUnseen(uint64_t call)
{
    auto called = m_builtinsWithSeveralRowsCalledAt.find(call);
    return called != m_builtinsWithSeveralRowsCalledAt.end() && noteEveryRowMayApplyTo(BuiltinID(called->value));
}

void BuilderOfProgramFacts::noteCallWhoseRowsLinkTells(uint64_t call, BuiltinID canonical)
{
    if (!m_linkIsAtWork)
        return;

    auto added = m_builtinsWithSeveralRowsCalledAt.add(call, canonical.index());
    if (added.isNewEntry)
        m_callsListedSinceLinkTookThemOver.append(call);
    else if (added.iterator->value != canonical.index()) {
        noteEveryRowMayApplyTo(canonical);
        noteEveryRowMayApplyTo(BuiltinID(added.iterator->value));
    }
}

static bool handsOutPrototypeOfOperandInProgramFacts(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    if (overload.result.type && std::ranges::contains(builtins.at(overload.result.type).parts, BuiltinTypePartKind::Prototype, &BuiltinTypePart::kind))
        return true;
    return std::ranges::any_of(overload.flow.transfers, [](const BuiltinTransfer& transfer) {
        return transfer.from.kind == BuiltinPlaceKind::Prototype;
    });
}

void BuilderOfProgramFacts::noteWhatRowsMake(BuiltinID canonical)
{
    const Builtin& entry = m_builtins.at(canonical);
    ModuleID compiledWithThisObject = entry.holder ? ModuleID() : compiledModuleWithObject(m_source, canonical);
    bool isObjectThatBodyOfCompiledModuleReturns = compiledWithThisObject && objectOfLibraryFileIsResultOfBody(m_source, compiledWithThisObject);
    bool isExportOfCompiledModule = isObjectThatBodyOfCompiledModuleReturns || (entry.holder && liesInModuleThatProgramHolds(canonical));
    BuiltinOrigin origin = isExportOfCompiledModule || entry.flags.contains(BuiltinFlag::IsCompilerHelper) ? BuiltinOrigin::Engine : entry.origin;
    OptionSet<BuiltinUse> uses = m_uses[canonical.index()];
    bool anyRow = anyRowMayApply(canonical) || callingUsesOf(entry.kind).isEmpty();
    SetForScope notesWhatConstructionsInSightMake(m_notesWhatConstructionsInSightMake, !anyRow && m_builtins.conditionOnInstancesThatAnswerEveryName(canonical));
    SetForScope makesWhatLinkFollows(m_makesWhatLinkFollows, linkTellsWhatHandlersAreHanded(canonical));
    auto notePlace = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue && !place.holdsWhatIsThrown)
            noteTypeIsMade(place.newValueType, origin);
    };
    if (entry.instanceClass && (anyRow || uses.containsAny(callingUsesInProgramFacts)))
        noteObjectsMayExist(entry.instanceClass);
    BuiltinOrigin originOfBuiltin = origin;
    for (BuiltinOverloadID row : m_builtins.overloadIDsOf(canonical)) {
        const BuiltinOverload& overload = m_builtins.at(row);
        bool isTaken = anyRow || (overload.mode == BuiltinCallMode::Call ? uses.contains(BuiltinUse::Called) : overload.mode != BuiltinCallMode::Construct || uses.contains(BuiltinUse::Constructed));
        if (!isTaken)
            continue;
        if (noCallAppliesRow(canonical, row))
            continue;
        bool mayBeHandedWhatNobodyKnows = m_mayBeHandedOperandsNobodyKnows.quickGet(canonical.index()) || m_isCalledWithoutRow.quickGet(canonical.index()) || m_uses[canonical.index()].contains(BuiltinUse::Escaped);
        bool mayPrintWhatNobodyKnows = mayBeHandedWhatNobodyKnows && std::ranges::any_of(overload.parameters, [&](const BuiltinParameter& parameter) {
            return m_builtins.isPrintedByFormatter(canonical, parameter);
        });
        bool mayDescribeWhatNobodyKnows = mayBeHandedWhatNobodyKnows && !m_builtins.isUnder(canonical, Builtins::NativeFunctions) && (m_instancesLoseTheirConstructor || unseenCodeCanRun()) && std::ranges::any_of(overload.parameters, [](const BuiltinParameter& parameter) {
            return parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused);
        });
        if (mayPrintWhatNobodyKnows || mayDescribeWhatNobodyKnows)
            noteObjectHandedToFormatter(true);
        if (mayBeHandedWhatNobodyKnows && (overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Get) && handsOutPrototypeOfOperandInProgramFacts(m_builtins, overload))
            m_climberMayBeHandedWhatNobodyKnows = true;
        const RuntimeFunctionEntry* code = findRuntimeFunction(m_source, overload.code.implementation.function);
        bool programHoldsCode = code && code->function;
        if (overload.isUndeclared() && (programHoldsCode || isExportOfCompiledModule))
            continue;
        bool codeOfRowIsInSight = programHoldsCode && !entersNativeCode(overload.code.implementation);
        if (!codeOfRowIsInSight)
            noteKeysThatRowNames(overload);
        origin = codeOfRowIsInSight ? BuiltinOrigin::Engine : originOfBuiltin;
        bool constructsOperand = std::ranges::any_of(overload.flow.callbacks, &BuiltinCallback::constructs);
        if (overload.isUndeclared() && !constructsOperand && !std::exchange(m_objectsOfAnyClassMayExist, true))
            m_classesWithObjectsHaveGrown = true;
        auto isOfCellThatTableDescribes = [&](const BuiltinPlace& place) {
            if (place.kind != BuiltinPlaceKind::NativeCell)
                return false;
            if (place.holdsOnlyWhatIsStored || std::ranges::contains(m_builtins.nativeCellsFilledByCalledFunctions(), place.nativeCell))
                return true;
            return std::ranges::any_of(m_builtins.hostObjects(), [&](const BuiltinHostObject& object) {
                return object.nativeCell == place.nativeCell && object.platforms.contains(targetOfThisBuild().platform);
            });
        };
        auto goesIntoResult = [](const BuiltinTransfer& transfer) { return transfer.to.kind == BuiltinPlaceKind::Result; };
        auto answers = overload.flow.transfers | std::views::filter(goesIntoResult);
        bool answersWhatTableDescribes = !answers.empty() && std::ranges::all_of(answers, isOfCellThatTableDescribes, &BuiltinTransfer::from);
        noteTypeIsMade(overload.result.type, answersWhatTableDescribes ? BuiltinOrigin::Engine : origin);
        noteClassesBelowWhoseFeaturesMakerNames(canonical, overload.result.type);
        for (const BuiltinResultProperty& property : overload.result.properties)
            noteTypeIsMade(property.type, origin);
        for (const BuiltinTransfer& transfer : overload.flow.transfers)
            notePlace(transfer.from);
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            notePlace(callback.newTarget);
            notePlace(callback.thisValue);
            notePlace(callback.spreadArguments);
            std::ranges::for_each(callback.arguments, notePlace);
        }
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
            notePlace(action.value);
        for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
            if (linkTellsWhatHandlersAreHanded(canonical))
                break;
            if (m_builtins.has(BuiltinMark::PrintsWhatHandlersAnswer, canonical))
                noteObjectHandedToFormatter(true);
            notePlace(call.thisValue);
            std::ranges::for_each(call.arguments, notePlace);
        }
    }
}

void BuilderOfProgramFacts::noteWhatBodyMakes(FunctionID id)
{
    auto noteFunctionObject = [&](const Function& function) {
        bool isAsync = function.has(FunctionFlags::IsAsync);
        if (function.has(FunctionFlags::IsGenerator))
            noteObjectsMayExist(isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction);
        else if (isAsync)
            noteObjectsMayExist(BuiltinClasses::AsyncFunction);
    };
    const Function& function = m_source.at(id);
    noteFunctionObject(function);
    const ObjectTable& objects = m_source.types.objects();
    if (function.isCoroutine())
        noteObjectsMayExist(objects.at(objects.coroutineResultOf(id)).builtinClass);
    for (const SummaryNode& node : m_inputs.summaries.at(id).nodes) {
        if (node.kind == SummaryNode::Kind::ClassInstance) {
            SetForScope makesWhatLinkFollows(m_makesWhatLinkFollows, true);
            noteObjectsMayExist(BuiltinClassID(node.first));
        } else if (node.kind == SummaryNode::Kind::Closure)
            noteFunctionObject(m_source.at(FunctionID(node.first)));
    }
}

void BuilderOfProgramFacts::lookThroughPrototypesOfClassesWithObjects()
{
    do {
        for (uint32_t index = 0; index < m_uses.size(); ++index) {
            if (m_builtins.canonicalID(BuiltinID(index)).index() != index || (m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
                continue;
            if (m_uses[index].contains(BuiltinUse::Constructed)) {
                if (BuiltinID prototype = m_builtins.findOwnMember(BuiltinID(index), BuiltinNames::prototype))
                    lookThrough(m_builtins.canonicalID(prototype));
            }
            if (uint16_t state = stateOfRowsOf(BuiltinID(index)); m_stateWhenRowsWereRead[index] != state) {
                m_stateWhenRowsWereRead[index] = state;
                noteWhatRowsMake(BuiltinID(index));
            }
        }
        if (std::exchange(m_classesWithObjectsHaveGrown, false)) {
            for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
                BuiltinID builtin(index);
                if (m_builtins.canonicalID(builtin) != builtin || m_builtins.at(builtin).kind != BuiltinKind::Prototype)
                    continue;
                BuiltinClassID owner = m_classOfPrototype[index];
                if (!owner || objectsMayExist(owner))
                    lookThrough(builtin);
            }
        }
        bool proxiesHaveComeToBe = !m_loopsMayGiveAnyName && std::ranges::any_of(m_makersOfObjectsThatListWhatTheyLike, [&](BuiltinID maker) {
            BuiltinClassID made = m_builtins.at(maker).instanceClass;
            return mayRun(maker) || (made && m_classesWithObjects.at(made.index()));
        });
        m_loopsMayGiveAnyName |= proxiesHaveComeToBe;
        if (std::exchange(m_keysNamedByRowsThatMayRunHaveGrown, false) || proxiesHaveComeToBe) {
            for (size_t index : m_isLookedThrough)
                m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
        }
        followWhatIsLetGo();
        uniteUsesOfSameObject();
    } while (m_classesWithObjectsHaveGrown || std::ranges::any_of(std::views::iota(uint32_t(0), static_cast<uint32_t>(m_uses.size())), [&](uint32_t index) {
        return m_builtins.canonicalID(BuiltinID(index)).index() == index && m_stateWhenRowsWereRead[index] != stateOfRowsOf(BuiltinID(index)) && !(m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty();
    }));
}

void BuilderOfProgramFacts::takeNamesOf(FunctionID id)
{
}

void BuilderOfProgramFacts::take(const LookupsPastProgramObjects& lookups)
{
    m_namesRead.merge(lookups.namesRead);
    m_namesCalled.merge(lookups.namesCalled);
    m_namesCalledWithOperands.merge(lookups.namesCalledWithOperands);
    m_namesCalledByNativeCodeOffUnseenObjects.merge(lookups.namesCalledByNativeCodeOffUnseenObjects);
    m_namesSpelledByProgram.merge(lookups.namesSpelledByProgram);
    for (size_t index : lookups.builtinsRead)
        noteReadOfMember(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsCalled)
        noteCall(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsConstructed)
        noteConstruction(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.holdersReadUnderKeysNobodyKnows)
        noteReadUnderKeyNobodyKnowsOff(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsCalledWithoutRow)
        noteCallWithoutRow(BuiltinID(static_cast<uint32_t>(index)));
    m_rowsThatLinkApplies.merge(lookups.rowsApplied);
    m_hasCallThatLinkLeftToUnseen.merge(lookups.builtinsWithCallsLeftToUnseen);
    if (lookups.formattersMayCallMethodsOfInstances)
        noteObjectHandedToFormatter(lookups.formattersAreHandedObjects);
    if (lookups.instancesLoseTheirConstructor)
        noteInstancesLoseTheirConstructor();
    for (size_t index : lookups.typesHandedToHandlers)
        noteTypeHandedToHandler(BuiltinTypeID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.classesWhoseInstancesHaveEscaped)
        noteInstanceHasEscaped(BuiltinClassID(static_cast<uint16_t>(index)));
    for (size_t index : lookups.namesWrittenThroughUnseenByProgram)
        noteWriteThroughUnseenByProgram(PropertyKeyID(static_cast<uint32_t>(index)));
    if (lookups.writesUnderAnyNameThroughUnseenByProgram)
        noteWriteThroughUnseenByProgram({ });
    for (size_t index : lookups.classesWithInstancesAtUnseen)
        noteInstancesMayReachFormatters(BuiltinClassID(static_cast<uint16_t>(index)));
    for (size_t index : lookups.classesHandedToFormatters)
        noteInstancesMayReachFormatters(BuiltinClassID(static_cast<uint16_t>(index)));
    if (lookups.formattersAreHandedInstancesOfSeveralClasses)
        noteInstancesOfAnyClassMayReachFormatters();
    Vector<uint64_t> constructorsBelowBuiltins = copyToVector(lookups.constructorsBelowBuiltins);
    std::ranges::sort(constructorsBelowBuiltins);
    for (uint64_t pair : constructorsBelowBuiltins)
        noteStandsAboveClass(BuiltinID(static_cast<uint32_t>(pair >> 32)), FunctionID(static_cast<uint32_t>(pair)));
    Vector<uint64_t> builtinsFoundWhereFlowBroughtHolders = copyToVector(lookups.builtinsFoundWhereFlowBroughtHolders);
    std::ranges::sort(builtinsFoundWhereFlowBroughtHolders);
    for (uint64_t pair : builtinsFoundWhereFlowBroughtHolders)
        noteFoundWhereFlowBroughtHolder(BuiltinID(static_cast<uint32_t>(pair >> 32)), FunctionID(static_cast<uint32_t>(pair)));
    for (size_t index : lookups.builtinsUsedViaFlow)
        noteLetGoByLink(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsLetGo) {
        noteLetGoByLink(BuiltinID(static_cast<uint32_t>(index)));
        noteLossOf(BuiltinID(static_cast<uint32_t>(index)));
    }
    for (size_t index : lookups.holdersStoredOnUnderAnyName)
        noteStoreUnderKeyNobodyKnowsOn(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.holdersWhoseMembersAreDeleted)
        noteDeletionOfMemberOf(BuiltinID(static_cast<uint32_t>(index)));
    m_readsUnderComputedKeys |= lookups.readsUnderKeysNobodyKnows;
    m_readsUnderNamesThatLoopsGave |= lookups.readsUnderNamesThatLoopsGave;
    m_nativeCodeMayHoldObjects |= lookups.nativeCodeMayHoldObjects;
    m_registryMayGiveKeyOfPrinter |= std::ranges::any_of(m_builtins.namesOfHostMethodsCallingStartupLoads(), [&](BuiltinName key) {
        return lookups.conditionsThatOperandsMayMeet.contains(wordOfConditionInProgramFacts(m_builtins.canonicalID(Builtins::SymbolFor), key));
    });
    for (size_t index : lookups.builtinsCalledByCode)
        m_isCalledByCodeInSight.quickSet(m_builtins.canonicalID(BuiltinID(static_cast<uint32_t>(index))).index());
}

static bool isLibraryModuleWithoutCodeInProgramFacts(const Program& program, const Module& module)
{
    if (module.body && program.at(module.body).hasBody())
        return false;
    switch (module.kind) {
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
        return module.runtimeModule && program.builtins().at(module.runtimeModule).flags.contains(RuntimeModuleFlag::HasSource);
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
        return module.flags.contains(ModuleFlags::IsLibrary);
    case ModuleKind::Data:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BuilderOfProgramFacts::evaluate(ModuleID id, const Witness& witness)
{
    const Module& module = m_source.at(id);
    if (isCodeOutsideImageInProgramFacts(module) || module.flags.contains(ModuleFlags::IsChangedOnDiskByRun))
        add(m_program.unseenCode, UnseenCodeCause::ImportsUncompiledModule, { }, witness);
    m_hostIsAskedForValueOfModule |= module.kind == ModuleKind::External && module.externalKind == ExternalKind::HostValue;
    BuiltinID object = module.runtimeModule ? m_builtins.namespaceOf(module.runtimeModule) : BuiltinID();
    bool fillsVariables = std::ranges::any_of(module.syntheticExports, [](const SyntheticExport& entry) {
        return !!entry.variable;
    });
    if (object && fillsVariables) {
        if (standsInTextOfProgram(witness))
            m_modulesImportedByProgram.quickSet(m_builtins.canonicalID(object).index());
        auto memberUnder = [&](const SyntheticExport& entry) {
            std::optional<PropertyKeyID> key = m_source.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = entry.name });
            return key ? findOwnMember(m_source, m_builtins.canonicalID(object), *key) : BuiltinID();
        };
        bool onlyFillsVariables = module.kind == ModuleKind::Runtime || loaderFillsVariablesOffValueOf(module);
        bool everyNameIsMember = onlyFillsVariables && std::ranges::all_of(module.syntheticExports, [&](const SyntheticExport& entry) {
            return !entry.variable || memberUnder(entry);
        });
        if (everyNameIsMember) {
            m_uses[m_builtins.canonicalID(object).index()].add(BuiltinUse::Read);
            keepPlaceOf(m_builtins.canonicalID(object), m_witnesses[m_builtins.canonicalID(object).index()], witness);
            for (const SyntheticExport& entry : module.syntheticExports) {
                if (entry.variable)
                    noteLossOf(memberUnder(entry), witness);
            }
        } else
            letGo(m_builtins.canonicalID(object), witness);
    }
    if (!m_modulesEvaluated.quickSet(id.index()))
        m_modulesToFollow.append(id);
}

void BuilderOfProgramFacts::followModules()
{
    for (const Entry& entry : m_source.entries)
        evaluate(entry.module, noWitness);
    for (uint32_t index = 0; index < m_source.modules.size(); ++index) {
        FunctionID body = m_source.modules[index].body;
        if (body && isReached(body))
            evaluate(ModuleID(index), noWitness);
    }

    followRequestsOfEvaluatedModules();
    AOT::sort(m_program.missingLibraryModules);
}

void BuilderOfProgramFacts::followRequestsOfEvaluatedModules()
{
    while (!m_modulesToFollow.isEmpty()) {
        ModuleID id = m_modulesToFollow.takeLast();
        const Module& module = m_source.at(id);
        if (isLibraryModuleWithoutCodeInProgramFacts(m_source, module))
            m_program.missingLibraryModules.append(id);
        Witness witness = module.body ? Witness { module.body.index(), Wire::none, m_source.at(module.body).declarationLocation() } : noWitness;
        for (const ModuleRequest& request : module.requests)
            evaluate(request.target, witness);
    }
}

void BuilderOfProgramFacts::noteEvaluated(ModuleID id)
{
    evaluate(id, noWitness);
    followRequestsOfEvaluatedModules();
}

void BuilderOfProgramFacts::letGo(BuiltinID canonical, const Witness& witness)
{
    uint32_t index = canonical.index();
    bool isLower = isPreferredWitness(witness, m_witnessesOfEscapes[index]);
    if (m_isLetGo.quickSet(index) && !isLower)
        return;
    if (isLower)
        m_witnessesOfEscapes[index] = witness;
    m_uses[index].add(BuiltinUse::Escaped);
    keepPlaceOf(canonical, m_witnesses[index], witness);
    m_holdersToSpreadBelow.append(canonical);
    lookThrough(canonical);
}

void BuilderOfProgramFacts::lookThrough(BuiltinID canonicalHolder)
{
    if (!m_isLookedThrough.quickSet(canonicalHolder.index()))
        m_holdersToLookThrough.append(canonicalHolder);
}

void BuilderOfProgramFacts::spreadBelow(BuiltinID canonicalHolder)
{
    Witness witness = m_witnessesOfEscapes[canonicalHolder.index()];
    const Builtin& holder = m_builtins.at(canonicalHolder);
    if (m_followsClimbsFromWhatIsLetGo && holder.prototypeLink == BuiltinPrototypeLink::Builtin && holder.inheritsFrom)
        letGo(m_builtins.canonicalID(holder.inheritsFrom), witness);
    if (defersMembersOf(canonicalHolder)) {
        m_hasDeferredMembers = true;
        return;
    }
    bool handsOutEvents = !m_eventsAreWithheld || standsInTextOfProgram(witness);
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        if (!handsOutEvents && m_rows.areEvents.quickGet(m_builtins.canonicalID(member).index()))
            continue;
        m_uses[member.index()].add(BuiltinUse::Escaped);
        keepLowerWitnessInProgramFacts(m_witnesses[member.index()], witness);
        letGo(m_builtins.canonicalID(member), witness);
    }
}

void BuilderOfProgramFacts::noteObjectHandedToFormatter(bool isWalked)
{
    bool isNews = !std::exchange(m_formattersMayCallMethodsOfInstances, true);
    if (isWalked)
        isNews |= !std::exchange(m_formattersAreHandedObjects, true);
    if (!isNews)
        return;
    for (size_t index : m_isLookedThrough)
        m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
}

ALWAYS_INLINE bool BuilderOfProgramFacts::formatterWalksPrototype(BuiltinID canonicalHolder) const
{
    BuiltinClassID builtinClass = m_classOfPrototype[canonicalHolder.index()];
    return m_formattersAreHandedObjects && builtinClass && m_builtins.formatterWalksInstancesOf(builtinClass) && formatterMayComeByWhatStandsBelow(canonicalHolder);
}

bool BuilderOfProgramFacts::formatterMayComeByWhatStandsBelow(BuiltinID canonicalHolder) const
{
    BuiltinClassID builtinClass = m_classOfPrototype[canonicalHolder.index()];
    return !builtinClass || m_objectsOfAnyClassMayExist || m_instancesOfAnyClassMayReachFormatters || m_classesWhoseInstancesMayReachFormatters.get(builtinClass.index());
}

void BuilderOfProgramFacts::noteGettersThatFormatterRuns(BuiltinID canonicalHolder)
{
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        BuiltinID canonical = m_builtins.canonicalID(member);
        if (m_builtins.at(canonical).kind != BuiltinKind::Accessor || m_builtins.at(canonical).origin != BuiltinOrigin::Host)
            continue;
        for (const BuiltinOverload& row : m_builtins.overloadsOf(canonical)) {
            if (row.mode != BuiltinCallMode::Get || row.code.implementation.function || !row.result.type || !m_builtins.at(row.result.type).mayBeObject())
                continue;
            m_uses[member.index()].add(BuiltinUse::Read);
            for (const BuiltinTypePart& part : m_builtins.at(row.result.type).parts) {
                if (part.kind == BuiltinTypePartKind::Builtin)
                    noteLossOf(part.builtin);
                if (part.builtinClass)
                    noteInstancesMayReachFormatters(part.builtinClass);
            }
        }
    }
}

void BuilderOfProgramFacts::noteWhereNameOfRunIsCalled(const Finding& call)
{
    BuiltinID holder;
    for (BuiltinClassID current = call.subject == Wire::none ? BuiltinClassID() : BuiltinClassID(call.subject); current && !holder; current = m_builtins.at(current).parent)
        holder = m_builtins.at(current).prototype;
    if (!holder) {
        keepLowerWitnessInProgramFacts(m_whereNameOfRunIsCalled, call.witness);
        return;
    }
    uint32_t stepsLeft = m_builtins.builtinCount();
    for (holder = m_builtins.canonicalID(holder); holder && stepsLeft--; holder = m_builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom) : BuiltinID()) {
        auto known = std::ranges::find(m_whereNameOfRunIsCalledOffWrittenPrimitives, holder, &std::pair<BuiltinID, Witness>::first);
        if (known == m_whereNameOfRunIsCalledOffWrittenPrimitives.end())
            m_whereNameOfRunIsCalledOffWrittenPrimitives.append({ holder, call.witness });
        else
            keepLowerWitnessInProgramFacts(known->second, call.witness);
    }
}

void BuilderOfProgramFacts::findMembersOffUnknownValues(BuiltinID canonicalHolder)
{
    Witness whereNameOfRunIsCalled = m_whereNameOfRunIsCalled;
    for (const auto& [holder, witness] : m_whereNameOfRunIsCalledOffWrittenPrimitives) {
        if (holder == canonicalHolder)
            keepLowerWitnessInProgramFacts(whereNameOfRunIsCalled, witness);
    }
    if (formatterWalksPrototype(canonicalHolder))
        noteGettersThatFormatterRuns(canonicalHolder);
    if (mayHoldObjectInProgramFacts(m_builtins, canonicalHolder) && !m_builtins.at(objectAnsweredByBindingInProgramFacts(m_builtins, canonicalHolder)).flags.containsAny({ BuiltinFlag::ListsAllMembers, BuiltinFlag::IsGlobalObject }) && !liesInModuleThatProgramHolds(canonicalHolder) && (m_readsUnderComputedKeys || m_isReadUnderComputedKeys.quickGet(canonicalHolder.index()) || m_readsUnderNamesThatLoopsGave || !m_namesRead.isEmpty()))
        m_objectsMayHaveMethodCalledWithStartupLoads = true;
    BuiltinClassID classOfHolder = m_classOfPrototype[canonicalHolder.index()];
    bool defersMembers = defersMembersOf(canonicalHolder);
    if (defersMembers && (m_readsUnderComputedKeys || m_readsUnderNamesThatLoopsGave || m_isReadUnderComputedKeys.quickGet(canonicalHolder.index())))
        m_hasDeferredMembers = true;
    bool isOfClassThatRowsOfHostName = classOfHolder && m_classesThatRowsOfHostName.get(classOfHolder.index());
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        BuiltinName name = m_builtins.at(member).name;
        if (!name)
            continue;
        PropertyKeyID key = m_source.keyOf(name);
        bool isRead = key && m_namesRead.get(key.index());
        bool isNamedByCode = isRead || (key && m_namesCalled.get(key.index()));
        bool isKeyThatFormattersAloneLookUnder = std::ranges::contains(m_builtins.namesOfHostMethodsCallingStartupLoads(), name);
        bool isSymbolThatNativeCodeMayLookUnder = m_builtins.isSymbolName(name) && (isKeyThatFormattersAloneLookUnder ? m_formattersMayCallMethodsOfInstances && formatterMayComeByWhatStandsBelow(canonicalHolder) : !m_keysReadOnlyByRowsThatNameThem.get(name.index()) || m_keysNamedByRowsThatMayRun.get(name.index()));
        bool isCalledOffWhatRowsOfHostName = key && isOfClassThatRowsOfHostName && m_namesCalledByNativeCodeOffUnseenObjects.get(key.index());
        bool isNamed = isNamedByCode || isCalledOffWhatRowsOfHostName || (m_nativeCodeMayHoldObjects && (isSymbolThatNativeCodeMayLookUnder || m_namesOfProtocols.get(name.index())));
        bool canBeListed = m_builtins.at(member).canBeListed && !m_builtins.isSymbolName(name);
        bool isReadUnderComputedKeys = !defersMembers && (m_readsUnderComputedKeys || m_isReadUnderComputedKeys.quickGet(canonicalHolder.index()));
        bool isValueReadProgramWide = isRead || (!defersMembers && (m_readsUnderComputedKeys || (m_readsUnderNamesThatLoopsGave && (canBeListed || m_loopsMayGiveAnyName))));
        bool isValueRead = isValueReadProgramWide || isReadUnderComputedKeys;
        if (!isNamed && !isValueRead)
            continue;
        BuiltinID canonical = m_builtins.canonicalID(member);
        if (m_rows.areEvents.quickGet(canonical.index()) && !(m_eventsAreWithheld ? key && m_namesSpelledByProgram.get(key.index()) : isNamedByCode))
            continue;
        m_uses[member.index()].add(BuiltinUse::Read);
        if (!isNamedByCode)
            keepLowerWitnessInProgramFacts(m_witnesses[member.index()], whereNameOfRunIsCalled);
        if (isNamedByCode) {
            m_isFoundByName.quickSet(member.index());
            m_isFoundOffValuesNobodyKnows.quickSet(member.index());
        }
        std::span<const BuiltinName> keysOfSpecies = m_builtins.keysReadBy(Protocol::Species);
        if (!isNamedByCode && !isValueRead && name == keysOfSpecies[0] && m_builtins.findMember(canonical, keysOfSpecies[1]).outcome == BuiltinMemberLookup::Outcome::Absent)
            continue;
        m_isOutOfSight.quickSet(canonical.index());
        bool nativeCodeMayNameIt = isCalledOffWhatRowsOfHostName || (m_nativeCodeMayHoldObjects && (isSymbolThatNativeCodeMayLookUnder || m_namesOfProtocols.get(name.index())));
        if (isValueRead || nativeCodeMayNameIt || !key || unseenCodeCanRun() || m_namesCalledWithOperands.get(key.index()))
            m_mayBeHandedOperandsNobodyKnows.quickSet(canonical.index());
        if ((isNamedByCode || isReadUnderComputedKeys) && m_builtins.at(canonical).origin == BuiltinOrigin::Engine)
            m_isFoundByWhatCodeWrote.quickSet(canonical.index());
        if (isNamed)
            m_uses[canonical.index()].add(callingUsesOf(m_builtins.at(canonical).kind));
        if (isValueRead && standsForObjectInProgramFacts(m_builtins, canonical)) {
            BuiltinKind kind = m_builtins.at(canonical).kind;
            if (!isValueReadProgramWide && kind != BuiltinKind::Function && kind != BuiltinKind::Method)
                lookThrough(canonical);
            else
                letGo(canonical, noWitness);
        }
        for (const BuiltinOverload& row : m_builtins.overloadsOf(canonical)) {
            if (!isValueRead || row.mode != BuiltinCallMode::Get || !row.result.type)
                continue;
            for (const BuiltinTypePart& part : m_builtins.at(row.result.type).parts) {
                if (part.kind == BuiltinTypePartKind::Builtin)
                    noteLossOf(part.builtin);
            }
        }
    }
    const Builtin& object = m_builtins.at(canonicalHolder);
    if (object.prototypeLink == BuiltinPrototypeLink::Builtin)
        lookThrough(m_builtins.canonicalID(object.inheritsFrom));
}

void BuilderOfProgramFacts::noteStoreUnderKeyNobodyKnowsOn(BuiltinID canonicalHolder)
{
    uint32_t stepsLeft = m_builtins.builtinCount();
    for (BuiltinID holder = canonicalHolder; holder && stepsLeft--; holder = m_builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom) : BuiltinID()) {
        for (BuiltinID member : m_builtins.membersOf(holder)) {
            if (std::ranges::contains(m_builtins.overloadsOf(m_builtins.canonicalID(member)), BuiltinCallMode::Set, &BuiltinOverload::mode))
                m_uses[member.index()].add(BuiltinUse::Written);
        }
    }
}

void BuilderOfProgramFacts::noteDeletionOfMemberOf(BuiltinID canonicalHolder)
{
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        if (m_builtins.at(member).flags.contains(BuiltinFlag::IsLazilyCreated))
            noteReadOfMember(member);
    }
}

void BuilderOfProgramFacts::noteReadOfMember(BuiltinID member)
{
    m_uses[member.index()].add(BuiltinUse::Read);
    m_isFoundByName.quickSet(member.index());
    m_isReadWhereLinkSees.quickSet(member.index());
}

void BuilderOfProgramFacts::noteLossOf(BuiltinID builtin, const Witness& witness)
{
    BuiltinID canonical = m_builtins.canonicalID(builtin);
    m_uses[canonical.index()].add(BuiltinUse::Read);
    keepPlaceOf(canonical, m_witnesses[canonical.index()], witness);
    m_isOutOfSight.quickSet(canonical.index());
    m_mayBeHandedOperandsNobodyKnows.quickSet(canonical.index());
    m_uses[canonical.index()].add(callingUsesOf(m_builtins.at(canonical).kind));
    if (standsForObjectInProgramFacts(m_builtins, canonical))
        letGo(canonical, witness);
}

void BuilderOfProgramFacts::followWhatIsLetGo()
{
    while (!m_holdersToSpreadBelow.isEmpty() || !m_holdersToLookThrough.isEmpty()) {
        if (!m_holdersToSpreadBelow.isEmpty())
            spreadBelow(m_holdersToSpreadBelow.takeLast());
        else
            findMembersOffUnknownValues(m_holdersToLookThrough.takeLast());
    }
}

void BuilderOfProgramFacts::uniteUsesOfSameObject()
{
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        uint32_t canonical = m_builtins.canonicalID(BuiltinID(index)).index();
        if (canonical == index)
            continue;
        m_uses[canonical].add(m_uses[index]);
        keepPlaceOf(BuiltinID(canonical), m_witnesses[canonical], m_witnesses[index]);
    }
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        uint32_t canonical = m_builtins.canonicalID(BuiltinID(index)).index();
        if (canonical == index || !m_uses[canonical].containsAny(usesOfObjectItselfInProgramFacts))
            continue;
        m_uses[index].add(m_uses[canonical] & usesOfObjectItselfInProgramFacts);
        keepLowerWitnessInProgramFacts(m_witnesses[index], m_witnesses[canonical]);
    }
}

std::optional<UnseenCodeCause> BuilderOfProgramFacts::causeOfLettingCodeIn(BuiltinID canonical) const
{
    if (std::optional<UnseenCodeCause> event = eventOfCalling(m_builtins, canonical, m_refused))
        return event;
    if (optionThatRunsCodeInProgramFacts(m_builtins, canonical) && !m_mayBePassedOptionThatRunsCode.quickGet(canonical.index()) && !m_inputs.linked.lookups.builtinsPassedOptionThatRunsCode.get(canonical.index()) && !m_isLetGo.quickGet(canonical.index()) && !m_isOutOfSight.quickGet(canonical.index()))
        return std::nullopt;
    if (eventOfCalling(m_builtins, canonical, RefusedCode { }))
        return std::nullopt;
    if (m_builtins.has(BuiltinMark::ReadLoadsCodeByName, canonical))
        return m_refused.codeOutsideImage ? std::nullopt : std::optional(UnseenCodeCause::LoadsCodeByName);
    if (isLoaderOfMachineCodeInProgramFacts(m_builtins, canonical))
        return std::nullopt;
    if (m_builtins.has(BuiltinMark::LoadsCodeWithoutJSAccess, canonical))
        return std::nullopt;
    return UnseenCodeCause::RunsUnseenCode;
}

void BuilderOfProgramFacts::listBuiltinsThatLetCodeIn()
{
    bool someAreNamedAtRunTime = m_inputs.linked.lookups.wereRuntimeNamesDeferred;
    BitVector usedByCodeInSight = someAreNamedAtRunTime ? builtinsThatCodeInSightUses() : BitVector();
    for (size_t index : m_rows.areEvents) {
        if (someAreNamedAtRunTime && !usedByCodeInSight.quickGet(index))
            continue;
        BuiltinID builtin(static_cast<uint32_t>(index));
        OptionSet<BuiltinUse> uses = m_uses[index];
        bool readIsEvent = m_builtins.has(BuiltinMark::ReadLoadsCodeByName, builtin) || m_builtins.at(builtin).kind == BuiltinKind::Accessor;
        if (!uses.containsAny(usesOfObjectItselfInProgramFacts) && !(readIsEvent && uses.contains(BuiltinUse::Read)))
            continue;
        if (std::optional<UnseenCodeCause> cause = causeOfLettingCodeIn(builtin))
            addAtCallsInTextOfProgram(m_program.unseenCode, *cause, builtin, builtin);
    }
}

void BuilderOfProgramFacts::listLoadsOfUnseenCode()
{
    if (m_defersRuntimeNames)
        return;
    auto admitting = std::ranges::find_if(m_program.unseenCode, admitsUnknownJavaScriptInProgramFacts, &UnseenCodeRecord::cause);
    if (admitting == m_program.unseenCode.end())
        return;
    Witness witness = admitting->witness;
    add(m_program.unseenCode, m_refused.codeOutsideImage ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName, { }, witness);
}

bool BuilderOfProgramFacts::machineCodeMayBeLoaded() const
{
    return std::ranges::any_of(m_builtins.loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
        return std::ranges::any_of(std::array { loader.callee, loader.named }, [&](BuiltinID builtin) {
            return builtin && m_uses[m_builtins.canonicalID(builtin).index()].containsAny(usesOfObjectItselfInProgramFacts);
        });
    });
}

void BuilderOfProgramFacts::listLoadsOfMachineCode()
{
    for (const BuiltinLoaderOfMachineCode& loader : m_builtins.loadersOfMachineCode()) {
        for (BuiltinID builtin : { loader.callee, loader.named }) {
            if (!builtin)
                continue;
            uint32_t index = m_builtins.canonicalID(builtin).index();
            if (m_uses[index].containsAny(usesOfObjectItselfInProgramFacts))
                addAtCallsInTextOfProgram(m_program.loadsOfMachineCode, UnseenCodeCause::RunsMachineCode, loader.named, BuiltinID(index));
        }
    }
}

BitVector BuilderOfProgramFacts::allowRuntimeNames()
{
    m_defersRuntimeNames = false;
    m_classesWithObjectsHaveGrown |= m_objectsOfAnyClassMayExist;
    if (m_hasDeferredMembers) {
        if (m_isLetGo.quickGet(m_globalObject.index()))
            m_holdersToSpreadBelow.append(m_globalObject);
        m_holdersToLookThrough.append(m_globalObject);
    }
    BitVector used(m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        if (m_uses[index].isEmpty())
            continue;
        used.quickSet(index);
        used.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
    }
    return used;
}

bool BuilderOfProgramFacts::isReachableByName(BuiltinID member) const
{
    const Builtin& entry = m_builtins.at(member);
    if (!entry.holder)
        return true;
    BuiltinID holder = m_builtins.canonicalID(entry.holder);
    BuiltinClassID classOfPrototype = m_classOfPrototype[holder.index()];
    BuiltinClassID builtinClass = classOfPrototype ? classOfPrototype : m_builtins.at(holder).instanceClass;
    if (!builtinClass || hostMakesInstancesOf(builtinClass) || m_inputs.linked.lookups.staticallyInstantiatedClasses.get(builtinClass.index()))
        return true;
    return !classOfPrototype && entry.kind != BuiltinKind::Constant && entry.kind != BuiltinKind::Prototype;
}
BitVector BuilderOfProgramFacts::builtinsThatCodeInSightUses() const
{
    const LookupsPastProgramObjects& lookups = m_inputs.linked.lookups;
    BitVector used = lookups.staticallyUsedBuiltins;
    used.merge(m_isSpelledInTexts);
    for (size_t index : m_isFoundByName) {
        BuiltinID member(static_cast<uint32_t>(index));
        BuiltinName name = m_builtins.at(member).name;
        PropertyKeyID key = name ? m_source.keyOf(name) : PropertyKeyID();
        if (!isReachableByName(member))
            continue;
        if (!key || !lookups.namesSpelledByProgram.get(key.index()))
            continue;
        used.quickSet(index);
        used.quickSet(m_builtins.canonicalID(member).index());
    }
    return used;
}

void BuilderOfProgramFacts::listBuiltinUses()
{
    bool someAreNamedAtRunTime = m_inputs.linked.lookups.wereRuntimeNamesDeferred;
    BitVector usedByCodeInSight = someAreNamedAtRunTime ? builtinsThatCodeInSightUses() : BitVector();
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        OptionSet<BuiltinUse> uses = m_uses[index];
        if (someAreNamedAtRunTime && !usedByCodeInSight.quickGet(index) && !usedByCodeInSight.quickGet(m_builtins.canonicalID(BuiltinID(index)).index()))
            uses.add(BuiltinUse::NamedAtRunTime);
        if (!m_uses[index].isEmpty())
            m_tables.builtinUses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(uses.toRaw()), m_witnesses[index] });
    }
}

Vector<BuiltinUseRecord> BuilderOfProgramFacts::usesWithWhatNobodyNamesAsEscaped() const
{
    static constexpr OptionSet<BuiltinUse> usesThatNameNothing { BuiltinUse::Tested, BuiltinUse::Escaped };
    BitVector isNamed = m_isSpelledInTexts;
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        bool isNamedInBody = m_isNamedByOperation.quickGet(index) || m_uses[index].containsAny({ BuiltinUse::Written, BuiltinUse::StoredByName });
        if (!isNamedInBody && !m_isFoundByName.quickGet(index) && !m_isCalledByCodeInSight.quickGet(index))
            continue;
        const Builtin& entry = m_builtins.at(BuiltinID(index));
        bool isClassFoundOffItsPrototype = entry.kind == BuiltinKind::Constructor && entry.holder && m_builtins.at(entry.holder).kind == BuiltinKind::Prototype;
        if (isNamedInBody || !isClassFoundOffItsPrototype)
            isNamed.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
    }
    Witness whereNamesFromRunTimeComeIn = noWitness;
    for (const UnseenCodeRecord& record : m_program.unseenCode) {
        if (standsInTextOfProgram(record.witness))
            keepLowerWitnessInProgramFacts(whereNamesFromRunTimeComeIn, record.witness);
    }
    Vector<BuiltinUseRecord> records = m_tables.builtinUses;
    for (BuiltinUseRecord& record : records) {
        auto uses = OptionSet<BuiltinUse>::fromRaw(static_cast<OptionSet<BuiltinUse>::StorageType>(record.uses));
        if (uses.contains(BuiltinUse::NamedAtRunTime)) {
            if (!standsInTextOfProgram(record.witness))
                record.witness = whereNamesFromRunTimeComeIn;
            continue;
        }
        if ((uses - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        uint32_t canonical = m_builtins.canonicalID(BuiltinID(record.builtin)).index();
        if (isObjectOfClassInProgramFacts(m_builtins, BuiltinID(canonical)) && !m_isSpelledInTexts.quickGet(canonical) && !m_partOfClassIsUsed.get(canonical) && (uses - OptionSet<BuiltinUse> { BuiltinUse::Tested, BuiltinUse::Read }).isEmpty()) {
            record.uses = BuiltinUse::Tested;
            continue;
        }
        if (m_isSpelledInTexts.quickGet(canonical) || m_modulesImportedByProgram.quickGet(canonical))
            uses.add(BuiltinUse::Read);
        else if (!isNamed.quickGet(canonical)) {
            uses = (uses & usesThatNameNothing) | OptionSet<BuiltinUse> { BuiltinUse::Escaped };
            if (!standsInTextOfProgram(record.witness))
                record.witness = noWitness;
        }
        record.uses = static_cast<BuiltinUse>(uses.toRaw());
    }
    return records;
}

BitVector BuilderOfProgramFacts::builtinsFoundByNameAlone() const
{
    BitVector found(m_builtins.builtinCount());
    for (size_t index : m_isFoundOffValuesNobodyKnows)
        found.quickSet(m_builtins.canonicalID(BuiltinID(static_cast<uint32_t>(index))).index());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        bool isSeenUsed = m_isSpelledInTexts.quickGet(index) || m_isNamedByOperation.quickGet(index) || m_isReadWhereLinkSees.quickGet(index) || m_isCalledByCodeInSight.quickGet(index) || m_inputs.linked.lookups.builtinsCalledWithOperandsNobodyRead.get(index) || m_uses[index].containsAny({ BuiltinUse::Written, BuiltinUse::StoredByName });
        if (isSeenUsed)
            found.quickClear(m_builtins.canonicalID(BuiltinID(index)).index());
    }
    return found;
}

BitVector BuilderOfProgramFacts::builtinsThatCodeInSightHandsOn() const
{
    BitVector handedOn = m_isLetGoByOperation;
    handedOn.merge(m_inputs.linked.lookups.builtinsAtUnseen);
    handedOn.merge(m_isCalledWhereLinkHasNoCall);
    handedOn.merge(m_inputs.linked.lookups.builtinsCalledWithOperandsNobodyRead);
    return handedOn;
}

void BuilderOfProgramFacts::layPlacesWhereProgramReadsNames()
{
    if (m_whereProgramReadsNames.isEmpty())
        return;
    for (FeatureUseRecord& record : m_tables.featureUses) {
        if ((record.kind != FeatureUseKind::Named && record.kind != FeatureUseKind::FoundByName) || record.builtin == Wire::none || hasFunction(record.witness))
            continue;
        if (BuiltinName name = m_builtins.at(BuiltinID(record.builtin)).name)
            record.witness = m_whereProgramReadsNames[name.index()];
    }
}

void BuilderOfProgramFacts::listHoldersWhoseKeysAreNeverListed()
{
    if (m_program.unseenCodeCanRun || m_source.admitsInspector)
        return;
    BitVector mayBeHeld(m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (m_isFoundByName.quickGet(index) || m_uses[index].contains(BuiltinUse::Escaped))
            mayBeHeld.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
    }
    for (uint32_t waiting : m_findingsThatWaitForLink.keys())
        mayBeHeld.quickSet(waiting);
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID holder(index);
        BuiltinKind kind = m_builtins.at(holder).kind;
        if (m_builtins.canonicalID(holder) != holder || holder == m_globalObject || (kind != BuiltinKind::Namespace && kind != BuiltinKind::Constructor && kind != BuiltinKind::Function))
            continue;
        if (mayBeHeld.quickGet(index) || m_isLetGo.quickGet(index) || m_isOutOfSight.quickGet(index) || m_isLookedThrough.quickGet(index))
            continue;
        bool isGoneThrough = false;
        for (BuiltinID current = holder; current && !isGoneThrough;) {
            isGoneThrough = m_keysAreListedInSight.quickGet(current.index());
            BuiltinID above = m_builtins.at(current).holder;
            current = above ? m_builtins.canonicalID(above) : BuiltinID();
        }
        if (isGoneThrough)
            continue;
        m_tables.builtinHoldersWhoseKeysAreNeverListed.set(index);
    }
}

void BuilderOfProgramFacts::lockMembersThatNobodyWrites()
{
    const BitVector& takenByLink = m_inputs.linked.lookups.membersTakenForConstants;
    m_writes.settle();
    bool unseenCodeUsesObjectsOfLanguage = programLetsUnseenCodeRun();
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        bool isTakenByLink = takenByLink.get(index);
        bool isUsed = isTakenByLink || m_isNamedByOperation.quickGet(index) || m_isFoundByName.quickGet(index) || m_builtins.at(member).kind == BuiltinKind::Constant;
        isUsed |= unseenCodeUsesObjectsOfLanguage && m_builtins.at(member).origin == BuiltinOrigin::Engine;
        if (!isUsed || !m_writes.canLockAtStartUp(member)) {
            if (ReasonBuiltinStaysOpen reason = m_source.isLockLeftToProgram(member) ? m_writes.reasonToLeaveOpen(member) : ReasonBuiltinStaysOpen::None; reason != ReasonBuiltinStaysOpen::None)
                m_tables.builtinMembersLeftOpen.append(BuiltinLeftOpen { member, reason });
            RELEASE_ASSERT(!isTakenByLink);
            continue;
        }
        if (ReasonBuiltinStaysOpen reason = m_writes.reasonToLeaveOpen(member); reason != ReasonBuiltinStaysOpen::None) {
            RELEASE_ASSERT(!isTakenByLink);
            m_tables.builtinMembersLeftOpen.append(BuiltinLeftOpen { member, reason });
            continue;
        }
        m_tables.builtinMembersLockedAtStartUp.set(index);
        if (!m_builtins.has(BuiltinMark::MayBeAbsent, member))
            m_tables.constantBuiltinMembers.set(index);
    }
}

bool BuilderOfProgramFacts::isMainModulePathReplaceable() const
{
    return std::ranges::any_of(m_source.replaceableBuiltins, [&](const ReplaceableBuiltin& replaceable) {
        return !replaceable.staysLocked && m_builtins.contains(replaceable.member) && m_builtins.has(BuiltinMark::HoldsPathOfMainModule, m_builtins.canonicalID(replaceable.member));
    });
}

static constexpr OpcodeSet valuesMadeInBodyInProgramFacts = primitiveConstants | OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::FinishObject, Opcode::NewArray, Opcode::NewClass, Opcode::FinishClass, Opcode::Data };

bool BuilderOfProgramFacts::isConstantLocation(BuiltinID member) const
{
    if (m_builtins.has(BuiltinMark::MayBeAbsent, member))
        return false;
    return m_builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || m_builtins.isLocationOfLockedObject(member) || m_tables.constantBuiltinMembers.get(member.index());
}

BuiltinID BuilderOfProgramFacts::builtinReadByPath(const BodyIndex& index, ValueID value) const
{
    value = value ? index.stripCasts(value) : ValueID();
    if (!value || index.isParameter(value))
        return { };
    BodyValue read = index.value(value);
    auto objectOf = [&](RuntimeModuleID module) {
        return module ? m_builtins.namespaceOf(module) : BuiltinID();
    };
    switch (read.opcode()) {
    case Opcode::Builtin:
        return read.as<Opcode::Builtin>().builtin();
    case Opcode::ModuleBuiltin:
        return read.as<Opcode::ModuleBuiltin>().builtin();
    case Opcode::RequireBuiltinModule:
        return objectOf(read.as<Opcode::RequireBuiltinModule>().runtimeModule());
    case Opcode::RequireModule:
        return objectOf(m_source.at(read.as<Opcode::RequireModule>().module()).runtimeModule);
    case Opcode::PropGet: {
        BuiltinID holder = builtinReadByPath(index, read.operand(0));
        BuiltinID member = holder ? findOwnMember(m_source, m_builtins.canonicalID(holder), read.as<Opcode::PropGet>().key()) : BuiltinID();
        return member && isConstantLocation(member) ? member : BuiltinID();
    }
    default:
        return { };
    }
}

BuiltinID BuilderOfProgramFacts::builtinSpelledByPath(const BodyIndex& index, ValueID value) const
{
    if (BuiltinID read = builtinReadByPath(index, value))
        return read;
    value = value ? index.stripCasts(value) : ValueID();
    if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet)
        return { };
    BodyValue read = index.value(value);
    BuiltinID holder = builtinSpelledByPath(index, read.operand(0));
    return holder ? findOwnMember(m_source, m_builtins.canonicalID(holder), read.as<Opcode::PropGet>().key()) : BuiltinID();
}

bool BuilderOfProgramFacts::isSurelyNot(const BodyIndex& index, ValueID value, BuiltinID member) const
{
    if (BuiltinID builtin = builtinReadByPath(index, value))
        return !member || m_builtins.canonicalID(builtin) != m_builtins.canonicalID(member);
    value = value ? index.stripCasts(value) : ValueID();
    return value && !index.isParameter(value) && valuesMadeInBodyInProgramFacts.contains(index.opcodeOf(value));
}

bool BuilderOfProgramFacts::isGuardedByFailingTest(const BodyIndex& index, ValueID site, BuiltinID member) const
{
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(site));
    if (!edge || !index.terminator(edge->from).isConditionalBranch())
        return false;
    ValueID condition = index.terminator(edge->from).as<TerminatorKind::Branch>().conditionOperand();
    if (!condition || index.isParameter(condition))
        return false;
    BodyValue test = index.value(condition);
    bool isTakenEdge = !edge->targetIndex;
    bool runsIfNullish = isTakenEdge && test.opcode() == Opcode::TypeTest && isNullishType(test.as<Opcode::TypeTest>().testedType());
    bool testsTruthiness = test.opcode() == Opcode::ToBoolean;
    if (!runsIfNullish && !testsTruthiness)
        return false;
    member = m_builtins.canonicalID(member);
    BuiltinID tested = builtinReadByPath(index, test.operand(0));
    if (!tested || m_builtins.canonicalID(tested) != member)
        return false;
    std::optional<bool> isTruthy;
    if (isObjectKind(m_builtins.at(member).kind) || m_builtins.nameOfSymbol(member))
        isTruthy = true;
    else if (std::optional<double> number = m_builtins.numberOf(member))
        isTruthy = *number && !std::isnan(*number);
    else if (std::optional<StringView> string = m_builtins.stringOf(member))
        isTruthy = !string->isEmpty();
    if (!isTruthy)
        return false;
    return runsIfNullish || *isTruthy != isTakenEdge;
}

bool BuilderOfProgramFacts::isHandedOverOnce(const BodyIndex& index, ValueID made, Vector<PropertyKeyID, 2>& namesTakenAway) const
{
    uint32_t mentionsCounted = 0;
    uint32_t mentionsFound = 0;
    uint32_t takers = 0;
    auto count = [&](ValueID name) {
        if (index.useCount(name) == BodyIndex::maxCountedUses)
            takers = 2;
        mentionsCounted += index.useCount(name);
    };
    count(made);
    for (uint32_t number = made.index() + 1; number < index.numberOfValues(); ++number) {
        ValueID id(number);
        if (index.isParameter(id))
            continue;
        BodyValue operation = index.value(id);
        uint32_t mentions = 0;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand && index.stripCasts(operand) == made;
        });
        mentionsFound += mentions;
        if (!mentions)
            continue;
        if (index.stripCasts(id) == made)
            count(id);
        else if (operation.opcode() == Opcode::PropDelete)
            namesTakenAway.append(operation.as<Opcode::PropDelete>().key());
        else
            takers += mentions;
    }
    return takers == 1 && mentionsFound == mentionsCounted;
}

bool BuilderOfProgramFacts::changesMember(const BodyIndex& index, ValueID descriptor, BuiltinID member, bool& isCertain) const
{
    isCertain = false;
    descriptor = descriptor ? index.stripCasts(descriptor) : ValueID();
    Vector<PropertyKeyID, 2> namesTakenAway;
    bool isAsBodyShows = descriptor && !index.isParameter(descriptor) && isHandedOverOnce(index, descriptor, namesTakenAway);
    if (isAsBodyShows && index.opcodeOf(descriptor) == Opcode::Data) {
        const auto* constant = m_source.at(index.value(descriptor).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>();
        return !constant || std::ranges::any_of(constant->members, [&](const DataMember& part) {
            return isPartThatBringsValueOrAccessor(part.key) && !namesTakenAway.contains(part.key);
        });
    }
    if (descriptor && !index.isParameter(descriptor) && index.opcodeOf(descriptor) == Opcode::Call) {
        BodyValue call = index.value(descriptor);
        std::span<const ValueID> arguments = call.variadicOperands();
        BuiltinID callee = builtinReadByPath(index, call.operand(0));
        ValueID name = arguments.size() == 2 ? index.stripCasts(arguments[1]) : ValueID();
        if (callee && m_builtins.canonicalID(callee) == m_builtins.canonicalID(Builtins::ObjectGetOwnPropertyDescriptor) && name && !index.isParameter(name) && index.opcodeOf(name) == Opcode::String) {
            BuiltinID described = builtinReadByPath(index, arguments[0]);
            std::optional<PropertyKeyID> key = m_source.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index.value(name).as<Opcode::String>().string() });
            if (described && key && findOwnMember(m_source, m_builtins.canonicalID(described), *key) == member)
                return false;
        }
    }
    if (!descriptor || index.isParameter(descriptor) || index.opcodeOf(descriptor) != Opcode::FinishObject)
        return true;
    ValueID literal = index.stripCasts(index.value(descriptor).operand(0));
    if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewObject || index.blockOf(literal) != index.blockOf(descriptor))
        return true;

    bool isAccessor = m_builtins.at(member).kind == BuiltinKind::Accessor;
    bool changes = false;
    isCertain = true;
    Vector<std::pair<PropertyKeyID, ValueID>, 3> partsWrittenLast;
    bool isAsWritten = isAsBodyShows && forEachPropertyOfWrittenLiteral(index, descriptor, { }, [&](PropertyKeyID key, ValueID brought) {
        if (!isPartThatBringsValueOrAccessor(key) || namesTakenAway.contains(key))
            return;
        partsWrittenLast.removeAllMatching([&](const auto& earlier) {
            return earlier.first == key;
        });
        partsWrittenLast.append({ key, brought });
    });
    if (!isAsWritten) {
        isCertain = false;
        return true;
    }
    for (auto [key, brought] : partsWrittenLast) {
        bool isValue = key == m_source.keyOf(BuiltinNames::value);
        if (isValue && !isAccessor && !isSurelyNot(index, brought, member)) {
            if (builtinReadByPath(index, brought))
                continue;
            isCertain = false;
        }
        if (!isValue && isAccessor && !isSurelyNot(index, brought, { }))
            isCertain = false;
        changes = true;
    }
    return changes;
}

bool BuilderOfProgramFacts::holdsNoCodeOfProgram(const BodyIndex& index, ValueID value) const
{
    if (builtinReadByPath(index, value))
        return true;
    value = index.stripCasts(value);
    return value && !index.isParameter(value) && (primitiveConstants.contains(index.opcodeOf(value)) || widestPrimitiveResultOf(index.opcodeOf(value)));
}

bool BuilderOfProgramFacts::namesBuiltinThatEndsProcess(const BodyIndex& index, const BodyValue& value) const
{
    auto endsProcess = [&](BuiltinID named) {
        return named && m_builtins.has(BuiltinMark::MayTerminateProcess, m_builtins.canonicalID(named));
    };
    switch (value.opcode()) {
    case Opcode::Builtin:
        return endsProcess(value.as<Opcode::Builtin>().builtin());
    case Opcode::ModuleBuiltin:
        return endsProcess(value.as<Opcode::ModuleBuiltin>().builtin());
    case Opcode::PropGet: {
        PropertyKeyID key = value.as<Opcode::PropGet>().key();
        if (BuiltinID holder = builtinReadByPath(index, value.operand(0)))
            return endsProcess(findOwnMember(m_source, m_builtins.canonicalID(holder), key));
        return std::ranges::any_of(m_builtins.marked(BuiltinMark::MayTerminateProcess), [&](const BuiltinMarkedEntry& entry) {
            BuiltinName name = m_builtins.at(entry.builtin).name;
            return name && m_source.keyOf(name) == key;
        });
    }
    default:
        return false;
    }
}

bool BuilderOfProgramFacts::codeThatNobodyNamesMayLeave()
{
    if (m_codeThatNobodyNamesMayLeave)
        return *m_codeThatNobodyNamesMayLeave;
    m_codeThatNobodyNamesMayLeave = false;
    m_codeThatNobodyNamesMayLeave = [&] {
        auto standsInProgram = [&](const Witness& witness) {
            return witness.function != Wire::none && !isLibraryCode(m_source, m_source.at(FunctionID(witness.function)));
        };
        if (programLetsUnseenCodeRun() || std::ranges::any_of(m_program.loadsOfMachineCode, standsInProgram, &UnseenCodeRecord::witness))
            return true;
        for (const BuiltinMarkedEntry& entry : m_builtins.marked(BuiltinMark::MayTerminateProcess)) {
            uint32_t builtin = m_builtins.canonicalID(entry.builtin).index();
            if (m_isLetGo.quickGet(builtin) && standsInProgram(m_witnessesOfEscapes[builtin]))
                return true;
        }
        BitVector areEnteredInOrder(m_source.functions.size());
        for (ModuleID module : evaluationOrder()) {
            if (FunctionID body = m_source.at(module).body)
                areEnteredInOrder.quickSet(body.index());
        }
        for (uint32_t index = 0; index < m_source.functions.size(); ++index) {
            FunctionID id(index);
            if (!isReached(id) || areEnteredInOrder.quickGet(index) || !m_inputs.bodies.find(id) || isLibraryCode(m_source, m_source.at(id)))
                continue;
            if (bodyMayLeave(id))
                return true;
        }
        return false;
    }();
    if (*m_codeThatNobodyNamesMayLeave) {
        for (BitVector& bodies : m_bodiesThatStay)
            bodies.clearAll();
    }
    return *m_codeThatNobodyNamesMayLeave;
}

static bool operandMayRunCodeInProgramFacts(Opcode opcode, uint32_t operandIndex)
{
    operandIndex = std::min(operandIndex, numberOfNamedOperandsOf(opcode));
    return !protocolsOf(opcode, operandIndex).isEmpty() || operandUsesOf(opcode, operandIndex).containsAny({ OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly });
}

bool BuilderOfProgramFacts::mayLeave(FunctionID id, const BodyIndex& index, const BodyValue& value, ThrowsOfOperations throwsOfOperations)
{
    Opcode opcode = value.opcode();
    if (namesBuiltinThatEndsProcess(index, value))
        return true;
    auto isHandedCode = [&](uint32_t firstOperand) {
        bool isHanded = false;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            isHanded |= operandIndex >= firstOperand && operand && !holdsNoCodeOfProgram(index, operand);
        });
        return isHanded;
    };
    if (effectRuleOf(opcode) == EffectRule::Operator)
        return isHandedCode(0) && codeThatNobodyNamesMayLeave();
    if ((effectRuleOf(opcode) == EffectRule::Declared && declaredBoundOf(opcode).alwaysCompletes()) || opcode == Opcode::RequireBuiltinModule) {
        bool isHandedCodeThatItRuns = false;
        if (operandsMayRunCode(opcode)) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                isHandedCodeThatItRuns |= operand && operandMayRunCodeInProgramFacts(opcode, operandIndex) && !holdsNoCodeOfProgram(index, operand);
            });
        }
        return isHandedCodeThatItRuns && codeThatNobodyNamesMayLeave();
    }
    if (opcode == Opcode::RequireDynamic)
        return true;
    bool waits = opcode == Opcode::Await || opcode == Opcode::Yield || opcode == Opcode::InitialYield
        || (opcode == Opcode::IteratorResume && value.as<Opcode::IteratorResume>().iteratorKind() != IteratorKind::Sync)
        || (opcode == Opcode::IteratorClose && value.as<Opcode::IteratorClose>().iteratorKind() != IteratorKind::Sync);
    if (waits && m_source.at(id).isModuleBody())
        return true;
    if (opcode == Opcode::RequireModule || opcode == Opcode::EvaluateModule) {
        const Module& target = m_source.at(opcode == Opcode::RequireModule ? value.as<Opcode::RequireModule>().module() : value.as<Opcode::EvaluateModule>().module());
        if (target.flags.contains(ModuleFlags::ThrowsAtLoad))
            return true;
        return target.body && !isLibraryCode(m_source, m_source.at(target.body)) && bodyMayLeave(target.body, throwsOfOperations);
    }
    if (accessClassOf(opcode) == AccessClass::ByName) {
        std::optional<uint8_t> receiver = receiverOf(opcode);
        ValueID object = receiver ? index.stripCasts(value.operand(*receiver)) : ValueID();
        if (!object)
            return true;
        if (accessOf(opcode) == Access::Define && !index.isParameter(object) && valuesMadeInBodyInProgramFacts.contains(index.opcodeOf(object)))
            return false;
        BuiltinID holder = builtinReadByPath(index, object);
        if (!holder)
            return codeThatNobodyNamesMayLeave();
        if (opcode == Opcode::PropSet && !m_source.admitsReplacedBuiltins) {
            PropertyKeyID key = value.as<Opcode::PropSet>().key();
            if (m_source.nameClassOf(key) != NameClass::Proto && !isLockedLocation(m_source, m_builtins.canonicalID(holder), key))
                return codeThatNobodyNamesMayLeave();
        }
        if (opcode != Opcode::PropGet)
            return true;
        auto read = value.as<Opcode::PropGet>();
        if (!read.isFreeName() || read.isForTypeOf())
            return false;
        BuiltinID member = findOwnMember(m_source, m_builtins.canonicalID(holder), read.key());
        return !member || !isConstantLocation(member);
    }
    if (!callsByValueInProgramFacts.contains(opcode))
        return codeThatNobodyNamesMayLeave();

    BuiltinID callee = builtinReadByPath(index, value.operand(0));
    if (!callee)
        return codeThatNobodyNamesMayLeave();
    callee = m_builtins.canonicalID(callee);
    if (m_builtins.has(BuiltinMark::MayTerminateProcess, callee) || m_rows.areEvents.quickGet(callee.index()))
        return true;
    return isHandedCode(1) && codeThatNobodyNamesMayLeave();
}

bool BuilderOfProgramFacts::mayThrowByItself(const BodyIndex& index, const BodyValue& value) const
{
    Opcode opcode = value.opcode();
    auto madeBy = [&](ValueID operand) -> std::optional<Opcode> {
        operand = operand ? index.stripCasts(operand) : ValueID();
        if (!operand || index.isParameter(operand))
            return std::nullopt;
        return index.opcodeOf(operand);
    };
    auto isAnswerOfBuiltinThatIsNeverMissing = [&](ValueID answer) {
        BuiltinID callee = builtinReadByPath(index, index.value(index.stripCasts(answer)).operand(0));
        std::span<const BuiltinOverload> rows = callee ? m_builtins.overloadsOf(m_builtins.canonicalID(callee)) : std::span<const BuiltinOverload>();
        return !rows.empty() && std::ranges::all_of(rows, [&](const BuiltinOverload& row) {
            if (row.isUndeclared() || !row.result.type)
                return false;
            const BuiltinType& type = m_builtins.at(row.result.type);
            return !type.isAnything() && !type.primitives.containsAny({ BuiltinPrimitive::Null, BuiltinPrimitive::Undefined, BuiltinPrimitive::NotPassed });
        });
    };
    auto isMadeHere = [&](ValueID operand) {
        std::optional<Opcode> made = madeBy(operand);
        if (made && callsByValueInProgramFacts.contains(*made))
            return isAnswerOfBuiltinThatIsNeverMissing(operand);
        return made && made != Opcode::Null && made != Opcode::Undefined && (valuesMadeInBodyInProgramFacts.contains(*made) || made == Opcode::RequireBuiltinModule);
    };
    if (opcode == Opcode::RequireBuiltinModule || opcode == Opcode::RequireModule || opcode == Opcode::EvaluateModule || opcode == Opcode::RecordRequire)
        return false;
    std::optional<uint8_t> receiver = receiverOf(opcode);
    if (receiver && accessOf(opcode) == Access::Define && isMadeHere(value.operand(*receiver)))
        return false;
    if (opcode == Opcode::NewClass)
        return false;
    if ((opcode == Opcode::DefineGetter || opcode == Opcode::DefineSetter || (opcode == Opcode::DefineMethod && !value.operand(OperandIndex::DefineMethod::computedKey))) && isMadeHere(value.operand(0)))
        return false;
    switch (effectRuleOf(opcode)) {
    case EffectRule::Declared: {
        bool mayRunCodeThatThrows = declaredBoundOf(opcode).mayThrowByRule();
        if (!mayRunCodeThatThrows && operandsMayRunCode(opcode)) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                mayRunCodeThatThrows |= operand && operandMayRunCodeInProgramFacts(opcode, operandIndex) && !holdsNoCodeOfProgram(index, operand);
            });
        }
        return mayRunCodeThatThrows;
    }
    case EffectRule::Operator: {
        bool mayRefuseOperand = false;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            mayRefuseOperand |= operand && (madeBy(operand) == Opcode::BigInt || !holdsNoCodeOfProgram(index, operand));
        });
        return mayRefuseOperand;
    }
    case EffectRule::Access: {
        if (!receiver)
            return false;
        BuiltinID holder = builtinReadByPath(index, value.operand(*receiver));
        if (holder && opcode == Opcode::PropSet && value.as<Opcode::PropSet>().isFreeName())
            return !findOwnMember(m_source, m_builtins.canonicalID(holder), value.as<Opcode::PropSet>().key());
        if ((opcode == Opcode::PropHas || opcode == Opcode::ElemHas) && madeBy(value.operand(*receiver)).transform([](Opcode made) { return primitiveConstants.contains(made); }).value_or(false))
            return true;
        return !holder && !isMadeHere(value.operand(*receiver));
    }
    case EffectRule::Call: {
        if (!callsByValueInProgramFacts.contains(opcode))
            return false;
        if (builtinReadByPath(index, value.operand(0)))
            return false;
        std::optional<Opcode> callee = madeBy(value.operand(0));
        if (callee == Opcode::PropGet) {
            ValueID holder = index.value(index.stripCasts(value.operand(0))).operand(0);
            std::optional<Opcode> made = madeBy(holder);
            return made == Opcode::NewObject || made == Opcode::FinishObject || (made == Opcode::Data && m_source.at(index.value(index.stripCasts(holder)).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>());
        }
        bool constructs = opcode != Opcode::Call && opcode != Opcode::CallList;
        if (callee == Opcode::NewClass || callee == Opcode::FinishClass)
            return !constructs;
        FunctionID called;
        if (callee == Opcode::NewClosure)
            called = index.value(index.stripCasts(value.operand(0))).as<Opcode::NewClosure>().function();
        else if (callee == Opcode::GetModuleVariable) {
            const Variable& variable = m_source.at(index.value(index.stripCasts(value.operand(0))).variable());
            if (variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable))
                called = variable.function;
        }
        if (!called)
            return callee && (valuesMadeInBodyInProgramFacts.contains(*callee) || callee == Opcode::BigInt);
        const Function& function = m_source.at(called);
        return constructs && (function.kind != FunctionKind::Plain || function.flags.containsAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }));
    }
    default:
        return false;
    }
}

bool BuilderOfProgramFacts::bodyMayLeave(FunctionID id, ThrowsOfOperations throwsOfOperations)
{
    const BodyIndex* index = m_inputs.bodies.find(id);
    BitVector& bodiesThatStay = m_bodiesThatStay[throwsOfOperations == ThrowsOfOperations::Count];
    if (bodiesThatStay.get(id.index()))
        return false;
    if (!index || m_bodiesBeingAsked.set(id.index()))
        return true;
    bool leaves = false;
    for (BlockID number : index->blocksInDominatorOrder()) {
        BodyBlock block = index->block(number);
        leaves |= block.terminator().kind() == TerminatorKind::Throw;
        for (BodyValue value : block.values())
            leaves = leaves || mayLeave(id, *index, value, throwsOfOperations) || (throwsOfOperations == ThrowsOfOperations::Count && mayThrowByItself(*index, value));
        if (leaves)
            break;
    }
    m_bodiesBeingAsked.clear(id.index());
    if (!leaves)
        bodiesThatStay.set(id.index());
    return leaves;
}

Vector<ModuleID> BuilderOfProgramFacts::evaluationOrder() const
{
    struct Frame {
        ModuleID module;
        uint32_t nextRequest { 0 };
    };
    Vector<ModuleID> order;
    BitVector isVisited(m_source.modules.size());
    Vector<Frame, 16> stack;
    if (!m_source.entries.isEmpty()) {
        stack.append({ m_source.entries[0].module });
        isVisited.quickSet(m_source.entries[0].module.index());
    }
    while (!stack.isEmpty()) {
        const Module& module = m_source.at(stack.last().module);
        if (stack.last().nextRequest == module.requests.size()) {
            order.append(stack.takeLast().module);
            continue;
        }
        uint32_t number = stack.last().nextRequest++;
        const ModuleRequest& request = module.requests[number];
        bool isSurelyEvaluated = request.kind == ModuleRequestKind::Import ? request.phase == ImportPhase::Evaluation : !number && isCommonJSFacade(m_source, module);
        if (isSurelyEvaluated && !isVisited.quickSet(request.target.index()))
            stack.append({ request.target });
    }
    return order;
}

bool BuilderOfProgramFacts::throwInFrontMayBeSurvived(const Witness& site)
{
    const BodyIndex& indexOfSite = *m_inputs.bodies.find(FunctionID(site.function));
    BlockID blockOfSite = indexOfSite.blockOf(ValueID(site.value));
    auto cannotRunInFrontOfSite = [&](FunctionID function, ValueID value) {
        if (function.index() != site.function)
            return false;
        BlockID block = indexOfSite.blockOf(value);
        if (block != blockOfSite)
            return indexOfSite.strictlyDominates(blockOfSite, block);
        bool isBehindSite = false;
        for (BodyValue other : indexOfSite.block(block).values()) {
            isBehindSite |= other.id().index() == site.value;
            if (other.id() == value)
                return isBehindSite;
        }
        return false;
    };
    auto isMarked = [&](BuiltinID named) {
        return named && m_builtins.has(BuiltinMark::MayLayListenerForUncaughtExceptions, m_builtins.canonicalID(named));
    };
    auto laysListener = [&](const BodyIndex& index, const BodyValue& value) {
        switch (value.opcode()) {
        case Opcode::Builtin:
            return isMarked(value.as<Opcode::Builtin>().builtin());
        case Opcode::ModuleBuiltin:
            return isMarked(value.as<Opcode::ModuleBuiltin>().builtin());
        case Opcode::PropGet: {
            BuiltinID holder = value.as<Opcode::PropGet>().isFreeName() ? BuiltinID() : builtinReadByPath(index, value.as<Opcode::PropGet>().objectOperand());
            return holder && isMarked(memberFoundOnInProgramFacts(m_source, holder, value.as<Opcode::PropGet>().key()));
        }
        case Opcode::ElemGet: {
            BuiltinID holder = builtinReadByPath(index, value.as<Opcode::ElemGet>().objectOperand());
            if (!holder)
                return false;
            holder = objectAnsweredByBindingInProgramFacts(m_builtins, m_builtins.canonicalID(holder));
            return std::ranges::any_of(m_builtins.marked(BuiltinMark::MayLayListenerForUncaughtExceptions), [&](const BuiltinMarkedEntry& entry) {
                BuiltinName name = m_builtins.at(entry.builtin).name;
                return name && m_builtins.findMember(holder, name).outcome == BuiltinMemberLookup::Outcome::Found;
            });
        }
        default:
            return false;
        }
    };
    bool unseenCodeMayRunInFront = programLetsUnseenCodeRunWhere([&](const Witness& witness) {
        return !cannotRunInFrontOfSite(FunctionID(witness.function), ValueID(witness.value));
    });
    if (unseenCodeMayRunInFront)
        return true;
    for (uint32_t number = 0; number < m_source.functions.size(); ++number) {
        FunctionID id(number);
        const BodyIndex* index = isReached(id) && !isLibraryCode(m_source, m_source.at(id)) ? m_inputs.bodies.find(id) : nullptr;
        if (!index)
            continue;
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            for (BodyValue value : index->block(BlockID(block)).values()) {
                if (laysListener(*index, value) && !cannotRunInFrontOfSite(id, value.id()))
                    return true;
            }
        }
    }
    return false;
}

bool BuilderOfProgramFacts::everyRunComesTo(const Witness& site)
{
    FunctionID id(site.function);
    ValueID place(site.value);
    const Function& function = m_source.at(id);
    if (!function.module || m_source.at(function.module).body != id)
        return false;
    const BodyIndex& index = *m_inputs.bodies.find(id);
    BlockID blockOfSite = index.blockOf(place);
    if (!index.isReachable(blockOfSite) || index.block(blockOfSite).hasHandler())
        return false;
    Vector<ModuleID> order = evaluationOrder();
    auto position = std::ranges::find(order, function.module);
    if (position == order.end())
        return false;

    for (BlockID other : index.blocksInDominatorOrder()) {
        if (index.strictlyDominates(blockOfSite, other))
            continue;
        BodyBlock block = index.block(other);
        for (BodyValue value : block.values()) {
            if (value.id() == place)
                break;
            if (mayLeave(id, index, value, ThrowsOfOperations::Count) || mayThrowByItself(index, value))
                return false;
        }
        TerminatorKind kind = block.terminator().kind();
        if (other != blockOfSite && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall || kind == TerminatorKind::Throw))
            return false;
    }
    for (auto other = order.begin(); other != order.end(); ++other) {
        const Module& module = m_source.at(*other);
        if (other == position || (other > position && module.kind != ModuleKind::CommonJs))
            continue;
        if (module.flags.contains(ModuleFlags::ThrowsAtLoad))
            return false;
        if (module.body && !isLibraryCode(m_source, m_source.at(module.body)) && bodyMayLeave(module.body, ThrowsOfOperations::Count))
            return false;
    }
    return !throwInFrontMayBeSurvived(site);
}

template<typename Functor>
bool BuilderOfProgramFacts::forEachPropertyOfWrittenLiteral(const BodyIndex& index, ValueID finished, ValueID handedOverAt, const Functor& functor) const
{
    finished = finished ? index.stripCasts(finished) : ValueID();
    if (!finished || index.isParameter(finished))
        return false;
    ValueID literal;
    if (index.opcodeOf(finished) == Opcode::NewDictionary)
        literal = std::exchange(finished, handedOverAt);
    else if (index.opcodeOf(finished) == Opcode::FinishObject) {
        literal = index.stripCasts(index.value(finished).operand(0));
        if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewObject)
            return false;
    }
    if (!literal || !finished || index.blockOf(literal) != index.blockOf(finished))
        return false;
    if (BodyValue made = index.value(literal); made.opcode() == Opcode::NewObject && made.as<Opcode::NewObject>().layout()) {
        std::span<const uint32_t> fields = made.as<Opcode::NewObject>().fields();
        std::span<const ValueID> values = made.variadicOperands();
        const Layout& layout = m_source.at(made.as<Opcode::NewObject>().layout());
        for (size_t position = 0; position < fields.size() && position < values.size(); ++position)
            functor(layout.fields[fields[position]].key, values[position]);
    }
    for (uint32_t number = literal.index() + 1; number < finished.index(); ++number) {
        BodyValue part = index.value(ValueID(number));
        bool touchesLiteral = false;
        part.forEachOperand([&](ValueID operand, uint32_t) {
            touchesLiteral |= operand && index.stripCasts(operand) == literal;
        });
        if (!touchesLiteral)
            continue;
        if (part.opcode() != Opcode::PropDefine || index.stripCasts(part.operand(0)) != literal)
            return false;
        functor(part.as<Opcode::PropDefine>().key(), part.as<Opcode::PropDefine>().valueOperand());
    }
    return true;
}

const DataNode::Object* BuilderOfProgramFacts::objectOfConstantsBehind(const BodyIndex& index, ValueID value) const
{
    value = value ? index.stripCasts(value) : ValueID();
    if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::Data || !index.onlyUsingValue(value))
        return nullptr;
    return m_source.at(index.value(value).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>();
}

bool BuilderOfProgramFacts::isPartThatBringsValueOrAccessor(PropertyKeyID key) const
{
    return key == m_source.keyOf(BuiltinNames::value) || key == m_source.keyOf(BuiltinNames::get) || key == m_source.keyOf(BuiltinNames::set);
}

bool BuilderOfProgramFacts::bringsValueOrAccessor(const DataNode::Object& descriptor) const
{
    return std::ranges::any_of(descriptor.members, [&](const DataMember& member) {
        return isPartThatBringsValueOrAccessor(member.key);
    });
}

std::optional<UserDiagnosticKind> BuilderOfProgramFacts::refusalOfCopy(const BodyIndex& index, const Witness& site, BuiltinID holder, BuiltinPropertyActionKind action, ValueID source, bool throwsWhereRefused)
{
    BuiltinPropertyActionKind actionUnderName = action == BuiltinPropertyActionKind::AssignsFrom ? BuiltinPropertyActionKind::StoresByKey : BuiltinPropertyActionKind::TakesDescriptor;
    std::optional<UserDiagnosticKind> worst;
    auto note = [&](PropertyKeyID key, ValueID brought) {
        std::optional<UserDiagnosticKind> kind = refusalOf(index, site, holder, key, actionUnderName, brought, throwsWhereRefused);
        if (kind == UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns)
            kind = UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (kind && (!worst || *kind < *worst))
            worst = kind;
    };
    if (forEachPropertyOfWrittenLiteral(index, source, ValueID(site.value), note))
        return worst;
    worst = std::nullopt;
    if (const DataNode::Object* constant = objectOfConstantsBehind(index, source)) {
        for (const DataMember& member : constant->members) {
            const DataNode::Object* descriptor = member.value.tryAs<DataNode::Object>();
            if (actionUnderName == BuiltinPropertyActionKind::TakesDescriptor && descriptor && !bringsValueOrAccessor(*descriptor))
                continue;
            note(member.key, { });
        }
        return worst;
    }

    const Vector<WriteFollowedToBuiltin>& followed = m_inputs.linked.lookups.writesFollowedToBuiltins;
    auto found = std::ranges::find_if(followed, [&](const WriteFollowedToBuiltin& write) {
        return write.function.index() == site.function && write.site.index() == site.value && m_builtins.canonicalID(write.holder) == holder;
    });
    if (found != followed.end() && found->knowsNamesBrought) {
        for (PropertyKeyID key : found->namesBrought)
            note(key, { });
        return worst;
    }
    note({ }, { });
    return worst;
}

std::optional<UserDiagnosticKind> BuilderOfProgramFacts::refusalOf(const BodyIndex& index, const Witness& site, BuiltinID holder, PropertyKeyID key, BuiltinPropertyActionKind action, ValueID brought, bool throwsWhereRefused)
{
    const Builtin& object = m_builtins.at(holder);
    if (action == BuiltinPropertyActionKind::AssignsFrom || action == BuiltinPropertyActionKind::TakesDescriptorMap || action == BuiltinPropertyActionKind::DefinesFrom)
        return refusalOfCopy(index, site, holder, action, brought, throwsWhereRefused);
    if (!key) {
        if (m_source.admitsReplacedBuiltins && !isFileOfProductInTestRun(m_source, FunctionID(site.function)))
            return std::nullopt;
        if (object.flags.contains(BuiltinFlag::IsLockedObject))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        return object.flags.contains(BuiltinFlag::HasLockedMembers) ? std::optional(UserDiagnosticKind::LockedBuiltinWriteWithUnknownKey) : std::nullopt;
    }
    BuiltinID member = findOwnMember(m_source, holder, key);
    bool isLockedAsInExecutable = member && writeOpensNothingInTestRun(m_source, FunctionID(site.function), member);
    if ((!isLockedLocation(m_source, holder, key) && !isLockedAsInExecutable) || m_source.nameClassOf(key) == NameClass::Proto)
        return std::nullopt;

    bool isCertain = false;
    switch (action) {
    case BuiltinPropertyActionKind::StoresByKey:
        if (member && m_builtins.at(member).kind == BuiltinKind::Accessor)
            return std::nullopt;
        if (BuiltinID stored = member ? builtinReadByPath(index, brought) : BuiltinID(); stored && m_builtins.canonicalID(stored) == m_builtins.canonicalID(member))
            return std::nullopt;
        if (member && isGuardedByFailingTest(index, ValueID(site.value), member))
            return std::nullopt;
        isCertain = isSurelyNot(index, brought, member);
        break;
    case BuiltinPropertyActionKind::DeletesByKey:
        if (!member)
            return std::nullopt;
        isCertain = true;
        break;
    case BuiltinPropertyActionKind::TakesDescriptor:
        isCertain = true;
        if (member && !changesMember(index, brought, member, isCertain))
            return std::nullopt;
        break;
    default:
        break;
    }
    if (!throwsWhereRefused) {
        bool isSilent = action != BuiltinPropertyActionKind::StoresByKey && action != BuiltinPropertyActionKind::DeletesByKey;
        isSilent |= member && (m_builtins.at(member).kind == BuiltinKind::Prototype || !isObjectKind(m_builtins.at(member).kind));
        return isSilent ? UserDiagnosticKind::LockedBuiltinWriteIsIgnored : UserDiagnosticKind::LockedBuiltinWriteMayRun;
    }
    return isCertain && !isLockedAsInExecutable && everyRunComesTo(site) ? UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns : UserDiagnosticKind::LockedBuiltinWriteMayRun;
}

std::optional<UserDiagnosticKind> BuilderOfProgramFacts::refusalOfWriteAt(const Witness& site, BuiltinID followed)
{
    const BodyIndex& index = *m_inputs.bodies.find(FunctionID(site.function));
    BodyValue write = index.value(ValueID(site.value));
    Opcode opcode = write.opcode();
    auto holderAt = [&](ValueID value) {
        BuiltinID holder = m_source.admitsReplacedBuiltins ? builtinSpelledByPath(index, value) : builtinReadByPath(index, value);
        return holder ? m_builtins.canonicalID(holder) : value ? followed : BuiltinID();
    };

    if (!callsByValueInProgramFacts.contains(opcode)) {
        BuiltinID holder = holderAt(write.operand(0));
        if (!holder)
            return std::nullopt;
        if (opcode == Opcode::PropSet) {
            auto store = write.as<Opcode::PropSet>();
            return refusalOf(index, site, holder, store.key(), BuiltinPropertyActionKind::StoresByKey, store.valueOperand(), store.mode() == ECMAMode::Strict);
        }
        if (opcode == Opcode::PropDelete)
            return refusalOf(index, site, holder, write.as<Opcode::PropDelete>().key(), BuiltinPropertyActionKind::DeletesByKey, { }, write.as<Opcode::PropDelete>().mode() == ECMAMode::Strict);
        if (opcode == Opcode::PropDefine)
            return refusalOf(index, site, holder, write.as<Opcode::PropDefine>().key(), BuiltinPropertyActionKind::DefinesName, { }, true);
        return refusalOf(index, site, holder, { }, BuiltinPropertyActionKind::StoresByKey, { }, true);
    }

    BuiltinID callee = m_source.admitsReplacedBuiltins ? builtinSpelledByPath(index, write.operand(0)) : builtinReadByPath(index, write.operand(0));
    if (!callee || !callsThatSpellOutArgumentsInProgramFacts.contains(opcode))
        return std::nullopt;
    callee = m_builtins.canonicalID(callee);
    BuiltinCallMode mode = constructionsInProgramFacts.contains(opcode) ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    std::span<const ValueID> arguments = write.variadicOperands();
    auto passedAs = [&](std::optional<BuiltinOperand> operand) -> ValueID {
        if (!operand)
            return { };
        if (operand->isReceiver())
            return mode == BuiltinCallMode::Call ? write.operand(1) : ValueID();
        if (operand->kind() == BuiltinOperand::Kind::ArgumentsFrom)
            return operand->argumentIndex() + 1 == arguments.size() ? arguments[operand->argumentIndex()] : ValueID();
        return operand->kind() == BuiltinOperand::Kind::Argument && operand->argumentIndex() < arguments.size() ? arguments[operand->argumentIndex()] : ValueID();
    };
    bool throwsWhereRefused = m_builtins.at(callee).holder != Builtins::Reflect || m_inputs.build.options().admitsReplacedBuiltins();

    std::optional<UserDiagnosticKind> worst;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(callee)) {
        if (overload.mode != mode)
            continue;
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (BuiltinID changed = action.kind == BuiltinPropertyActionKind::ChangesPrototype ? holderAt(passedAs(action.object)) : BuiltinID(); changed && m_builtins.hasLockedPrototypeLink(changed)) {
                UserDiagnosticKind kind = throwsWhereRefused ? UserDiagnosticKind::LockedBuiltinWriteMayRun : UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
                if (!worst || kind < *worst)
                    worst = kind;
            }
            BuiltinID holder = writesUnderKeyOfCallInProgramFacts(action) ? holderAt(passedAs(action.object)) : BuiltinID();
            if (!holder)
                continue;
            bool isCopy = action.kind == BuiltinPropertyActionKind::AssignsFrom || action.kind == BuiltinPropertyActionKind::TakesDescriptorMap || action.kind == BuiltinPropertyActionKind::DefinesFrom;
            if (action.key.kind != BuiltinKeyKind::Operand && !isCopy)
                continue;
            PropertyKeyID key;
            ValueID passedKey = passedAs(action.key.operand);
            passedKey = passedKey ? index.stripCasts(passedKey) : ValueID();
            if (passedKey && !index.isParameter(passedKey) && index.opcodeOf(passedKey) == Opcode::String) {
                std::optional<PropertyKeyID> spelled = m_source.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index.value(passedKey).as<Opcode::String>().string() });
                if (!spelled)
                    continue;
                key = *spelled;
            }
            std::optional<UserDiagnosticKind> kind = refusalOf(index, site, holder, key, action.kind, passedAs(action.value.operand), throwsWhereRefused);
            if (!throwsWhereRefused && kind == UserDiagnosticKind::LockedBuiltinWriteMayRun)
                kind = UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
            if (kind && (!worst || *kind < *worst))
                worst = kind;
        }
    }
    return worst;
}

void BuilderOfProgramFacts::reportWritesThatLocksRefuse()
{
    static_assert(UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns < UserDiagnosticKind::LockedBuiltinWriteMayRun && UserDiagnosticKind::LockedBuiltinWriteMayRun < UserDiagnosticKind::LockedBuiltinWriteIsIgnored);
    Vector<UserDiagnostic> diagnostics;
    auto report = [&](const Witness& site, BuiltinID followed) {
        if (std::optional<UserDiagnosticKind> kind = refusalOfWriteAt(site, followed)) {
            diagnostics.append(UserDiagnostic {
                .kind = *kind,
                .function = FunctionID(site.function),
                .block = m_inputs.bodies.find(FunctionID(site.function))->blockOf(ValueID(site.value)),
                .value = ValueID(site.value),
                .location = site.location,
            });
        }
    };
    for (const Witness& site : m_writesOfProgramOnBuiltins)
        report(site, { });
    for (const WriteFollowedToBuiltin& write : m_inputs.linked.lookups.writesFollowedToBuiltins) {
        const BodyIndex* index = m_inputs.bodies.find(write.function);
        if (!index)
            continue;
        report(Witness { write.function.index(), write.site.index(), m_source.at(write.function).locationOf(index->value(write.site)) }, m_builtins.canonicalID(write.holder));
    }
    AOT::sort(diagnostics, { }, [](const UserDiagnostic& diagnostic) {
        return std::tuple(diagnostic.location.module, diagnostic.location.position, diagnostic.function, diagnostic.kind, diagnostic.value);
    });
    diagnostics.shrink(diagnostics.size() - std::ranges::unique(diagnostics, [](const UserDiagnostic& first, const UserDiagnostic& second) {
        return first.function == second.function && first.location.module == second.location.module && first.location.position == second.location.position;
    }).size());
    m_inputs.build.diagnostics().append(WTF::move(diagnostics));
}

bool BuilderOfProgramFacts::isNameOfModuleWrittenOut(const BodyIndex& index, const BodyValue& call, BuiltinID loader) const
{
    if (!callsThatSpellOutArgumentsInProgramFacts.contains(call.opcode()))
        return false;
    std::span<const ValueID> arguments = call.variadicOperands();
    return std::ranges::all_of(m_builtins.overloadsOf(loader), [&](const BuiltinOverload& overload) {
        return std::ranges::all_of(overload.parameters, [&](const BuiltinParameter& parameter) {
            if (!parameter.roles.contains(BuiltinOperandRole::ModuleSpecifier))
                return true;
            if (parameter.operand.kind() != BuiltinOperand::Kind::Argument || parameter.operand.argumentIndex() >= arguments.size())
                return false;
            ValueID name = index.stripCasts(arguments[parameter.operand.argumentIndex()]);
            return !index.isParameter(name) && index.opcodeOf(name) == Opcode::String;
        });
    });
}

void BuilderOfProgramFacts::reportLoadsUnderNamesThatNobodyHasRead()
{
    if (!m_refused.codeOutsideImage || m_source.admitsInspector)
        return;
    Vector<UserDiagnostic> diagnostics;
    if (m_loaderIsHandedToText)
        diagnostics.append(UserDiagnostic { .kind = UserDiagnosticKind::RequirePassedToUnseenCode });
    for (const Witness& site : m_loadsOfProgramByName) {
        const BodyIndex* index = m_inputs.bodies.find(FunctionID(site.function));
        if (!index || site.value == Wire::none)
            continue;
        BodyValue load = index->value(ValueID(site.value));
        if (!loadsByNameInProgramFacts.contains(load.opcode()))
            continue;
        if (load.opcode() == Opcode::ImportDynamic ? load.as<Opcode::ImportDynamic>().allNamesAreKnown() : load.as<Opcode::RequireDynamic>().allNamesAreKnown())
            continue;
        ValueID name = index->stripCasts(load.operand(0));
        if (!index->isParameter(name) && index->opcodeOf(name) == Opcode::String)
            continue;
        diagnostics.append(UserDiagnostic {
            .kind = UserDiagnosticKind::NonConstantImportPath,
            .function = FunctionID(site.function),
            .block = index->blockOf(ValueID(site.value)),
            .value = ValueID(site.value),
            .location = site.location,
        });
    }
    for (size_t row : m_rows.areEvents) {
        BuiltinID loader(static_cast<uint32_t>(row));
        if (eventOfCalling(m_builtins, loader, m_refused) != UnseenCodeCause::LoadsModuleByName)
            continue;
        forEachCallInTextOfProgram(loader, [&](const Witness& site) {
            const BodyIndex* index = m_inputs.bodies.find(FunctionID(site.function));
            if (!index || site.value == Wire::none || isNameOfModuleWrittenOut(*index, index->value(ValueID(site.value)), loader))
                return;
            diagnostics.append(UserDiagnostic {
                .kind = UserDiagnosticKind::NonConstantModuleName,
                .function = FunctionID(site.function),
                .block = index->blockOf(ValueID(site.value)),
                .value = ValueID(site.value),
                .location = site.location,
            });
        });
    }
    AOT::sort(diagnostics, { }, [](const UserDiagnostic& diagnostic) {
        return std::tuple(diagnostic.location.module, diagnostic.location.position, diagnostic.function, diagnostic.value);
    });
    diagnostics.shrink(diagnostics.size() - std::ranges::unique(diagnostics, [](const UserDiagnostic& first, const UserDiagnostic& second) {
        return first.function == second.function && first.location.module == second.location.module && first.location.position == second.location.position;
    }).size());
    m_inputs.build.diagnostics().append(WTF::move(diagnostics));
}

void BuilderOfProgramFacts::listLibraryCodeLacked()
{
    const LookupsPastProgramObjects& lookups = m_inputs.linked.lookups;
    BitVector isReachedAsValue = m_isFoundByWhatCodeWrote;
    for (const BitVector* told : { &lookups.builtinsCalled, &lookups.builtinsConstructed, &lookups.builtinsCalledOutOfSight, &lookups.builtinsLetGo }) {
        for (size_t index : *told)
            isReachedAsValue.quickSet(m_builtins.canonicalID(BuiltinID(static_cast<uint32_t>(index))).index());
    }
    auto listUnlessHeld = [&](RuntimeFunctionID function) {
        if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_source, function); entry && entry->function)
            return;
        if (m_builtins.contains(function) && !m_builtins.at(function).platforms.contains(targetOfThisBuild().platform))
            return;
        m_program.missingLibraryFunctions.append(function);
    };

    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        BuiltinID builtin(index);
        if (m_builtins.canonicalID(builtin) != builtin)
            continue;
        bool isNamed = m_isNamedByOperation.quickGet(index) && !(m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty();
        bool isReached = isReachedAsValue.quickGet(index);
        if (!isNamed && !isReached)
            continue;
        for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(builtin)) {
            if (!called.condition && called.function)
                listUnlessHeld(called.function);
        }
        bool mayRun = isReached || m_uses[index].containsAny(usesOfObjectItselfInProgramFacts) || m_isOutOfSight.quickGet(index);
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(builtin)) {
            if (mayRun && overload.code.implementation.function)
                listUnlessHeld(overload.code.implementation.function);
        }
    }
    AOT::sort(m_program.missingLibraryFunctions);
    removeRepeatedElements(m_program.missingLibraryFunctions);
}

void BuilderOfProgramFacts::listLocaleUses()
{
    const UnseenCodeRecord* admitting = nullptr;
    for (const UnseenCodeRecord& record : m_program.unseenCode) {
        if (admitsUnknownJavaScriptInProgramFacts(record.cause) && (!admitting || isPreferredWitness(record.witness, admitting->witness)))
            admitting = &record;
    }
    if (admitting) {
        m_tables.localeUses.append(LocaleUse { LocaleUseKind::Unknown, { }, admitting->builtin == Wire::none ? BuiltinID() : BuiltinID(admitting->builtin), admitting->witness });
        return;
    }

    const LookupsPastProgramObjects& lookups = m_inputs.linked.lookups;
    RowsThatTakeLocales takers(m_builtins);
    LocalesPassedAtCalls passed = localesPassedAtCalls(m_source, m_inputs.bodies, takers, lookups.callsOfBuiltinsAtSites.span());
    Vector<LocaleUse> uses = WTF::move(passed.uses);
    bool someCallOfProgramPassesWhatNobodyKnows = std::ranges::any_of(uses, [&](const LocaleUse& use) {
        return use.kind == LocaleUseKind::Unknown && standsInTextOfProgram(use.witness);
    });
    BitVector rows = WTF::move(takers.rows);
    rows.merge(m_rows.readLocalesWithoutDescription);
    for (size_t index : rows) {
        if (someCallOfProgramPassesWhatNobodyKnows)
            break;
        bool isCalled = m_uses[index].containsAny(callingUsesInProgramFacts) || lookups.builtinsCalled.get(index);
        bool callsAreSeen = !m_uses[index].contains(BuiltinUse::Escaped) && !m_isOutOfSight.quickGet(index) && !passed.rowsCalledWhereNobodySeesOperands.get(index) && (!isCalled || passed.rowsWithCallsLookedAt.get(index));
        bool isCalledWithoutDescription = m_rows.readLocalesWithoutDescription.quickGet(index) && m_uses[index].containsAny(callingUsesInProgramFacts);
        if (!callsAreSeen || isCalledWithoutDescription)
            uses.append(LocaleUse { LocaleUseKind::Unknown, { }, BuiltinID(static_cast<uint32_t>(index)), m_witnesses[index] });
    }

    auto subjectOf = [](const LocaleUse& use) {
        return std::tuple(use.kind, use.tag);
    };
    AOT::sort(uses, [&](const LocaleUse& a, const LocaleUse& b) {
        if (subjectOf(a) != subjectOf(b))
            return subjectOf(a) < subjectOf(b);
        if (isPreferredWitness(a.witness, b.witness) || isPreferredWitness(b.witness, a.witness))
            return isPreferredWitness(a.witness, b.witness);
        return a.builtin < b.builtin;
    });
    removeRepeatedElements(uses, [&](const LocaleUse& a, const LocaleUse& b) {
        return subjectOf(a) == subjectOf(b);
    });
    m_tables.localeUses = WTF::move(uses);
}

bool BuilderOfProgramFacts::mayReadRecordOfLastMatch() const
{
    BuiltinID constructor = m_builtins.at(BuiltinClasses::RegExp).constructor;
    if (!constructor || m_program.unseenCodeCanRun)
        return true;
    BuiltinID canonical = m_builtins.canonicalID(constructor);
    bool unknownValuesMayBeIt = m_hasDeferredMembers || isOutOfSight(canonical) || m_isLookedThrough.quickGet(canonical.index());
    return std::ranges::any_of(m_builtins.membersOf(canonical), [&](BuiltinID member) {
        const Builtin& entry = m_builtins.at(member);
        if (entry.mutability != BuiltinMutability::ChangesOnItsOwn)
            return false;
        PropertyKeyID key = entry.name ? m_source.keyOf(entry.name) : PropertyKeyID();
        return !m_uses[member.index()].isEmpty() || !m_uses[m_builtins.canonicalID(member).index()].isEmpty() || (unknownValuesMayBeIt && key && m_namesRead.get(key.index()));
    });
}

void BuilderOfProgramFacts::listWorkers()
{
    bool isNamedByOperation = false;
    bool isOutOfSight = false;
    Witness lowestEscape = noWitness;
    for (size_t index : m_rows.startWorkers) {
        isNamedByOperation |= m_isNamedByOperation.quickGet(index);
        if (!m_uses[index].contains(BuiltinUse::Escaped) && !m_isOutOfSight.quickGet(index))
            continue;
        isOutOfSight = true;
        keepLowerWitnessInProgramFacts(lowestEscape, m_witnessesOfEscapes[index]);
    }
    if (isOutOfSight && (hasFunction(lowestEscape) || isNamedByOperation))
        noteStartWithoutName(m_whereConstructorOfWorkersIsLetGo);
    AOT::sort(m_startsWithoutName, isLowerWitness);
    removeRepeatedElements(m_startsWithoutName, [](const Witness& a, const Witness& b) {
        return !isLowerWitness(a, b) && !isLowerWitness(b, a);
    });
    m_program.workerStartsWithoutName = WTF::move(m_startsWithoutName);

    Vector<WorkerEntry>& entries = m_program.workerEntries;
    AOT::sort(entries, [&](const WorkerEntry& a, const WorkerEntry& b) {
        if (a.path != b.path)
            return compareProgramStrings(m_source.string(a.path), m_source.string(b.path)) < 0;
        return isLowerWitness(a.witness, b.witness);
    });
    size_t kept = 0;
    for (const WorkerEntry& entry : entries) {
        if (kept && entries[kept - 1].path == entry.path)
            entries[kept - 1].hasOptions |= entry.hasOptions;
        else
            entries[kept++] = entry;
    }
    entries.shrink(kept);

    Vector<Witness>& startsFromURLs = m_program.workerStartsFromURLs;
    auto placeOf = [](const Witness& witness) {
        return std::tuple(witness.function, witness.value);
    };
    AOT::sort(startsFromURLs, { }, placeOf);
    startsFromURLs.shrink(std::ranges::unique(startsFromURLs, { }, placeOf).begin() - startsFromURLs.begin());
}

bool BuilderOfProgramFacts::needsOneInstanceAtMost() const
{
    if (m_inputs.build.options().runsInSeveralRealms())
        return false;
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (!m_builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        if (m_program.unseenCodeCanRun || !m_uses[index].isEmpty() || !m_uses[m_builtins.canonicalID(BuiltinID(index)).index()].isEmpty())
            return false;
    }
    return true;
}

void BuilderOfProgramFacts::noteReached(FunctionID id)
{
    take(m_inputs.looks.of(id));
    takeNamesOf(id);
    noteWhatBodyMakes(id);
}

bool BuilderOfProgramFacts::knowsWhatBodyMakes(FunctionID id) const
{
    auto knowsFunctionObject = [&](const Function& function) -> bool {
        bool isAsync = function.has(FunctionFlags::IsAsync);
        if (function.has(FunctionFlags::IsGenerator))
            return m_classesWithObjects.at((isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction).index());
        return !isAsync || m_classesWithObjects.at(BuiltinClasses::AsyncFunction.index());
    };
    const Function& function = m_source.at(id);
    if (!knowsFunctionObject(function))
        return false;
    const ObjectTable& objects = m_source.types.objects();
    if (function.isCoroutine() && !m_classesWithObjects.at(objects.at(objects.coroutineResultOf(id)).builtinClass.index()))
        return false;
    for (const SummaryNode& node : m_inputs.summaries.at(id).nodes) {
        if (node.kind == SummaryNode::Kind::ClassInstance ? !m_classesWithObjects.at(node.first) : node.kind == SummaryNode::Kind::Closure && !knowsFunctionObject(m_source.at(FunctionID(node.first))))
            return false;
    }
    return true;
}

bool BuilderOfProgramFacts::mayExport(const Module& module, StringID name) const
{
    if (module.kind != ModuleKind::SourceText || module.exportRule != SyntheticExportRule::None || module.exportsVariable)
        return true;
    return std::ranges::any_of(module.requests, &ModuleRequest::isDynamicStarExport) || std::ranges::contains(module.exports, name, &ModuleExport::name);
}

void BuilderOfProgramFacts::useBuiltinsThatHostHandsExportsTo()
{
    for (uint32_t index = 0; index < m_source.entries.size(); ++index) {
        const Entry& entry = m_source.entries[index];
        const Module& module = m_source.at(entry.module);
        bool findsSomething = !m_inputs.linked.lookups.entriesThatHandHostNothing.get(index) && std::ranges::any_of(entry.hostReads, [&](const HostRead& read) {
            return mayExport(module, read.exportName);
        });
        for (const HostRead& read : entry.hostReads) {
            if (!findsSomething || !read.builtin)
                continue;
            BuiltinID canonical = m_builtins.canonicalID(read.builtin);
            m_uses[canonical.index()].add(BuiltinUse::Called);
            if (!m_inputs.linked.lookups.entriesThatHostStartsByCallInSight.get(index))
                m_isCalledByHost.quickSet(canonical.index());
        }
    }
}

void BuilderOfProgramFacts::settle()
{
    for (const Finding& call : m_callsUnderComputedKeys) {
        if (m_inputs.linked.lookups.readsOfProgramUnderKeysNobodyKnows.contains((static_cast<uint64_t>(call.witness.function) + 1) << 32 | call.witness.value))
            noteWhereNameOfRunIsCalled(call);
    }
    useBuiltinsThatHostHandsExportsTo();
    for (const BuiltinUsedAtStart& used : m_source.builtinsUsedAtStart)
        letGo(m_builtins.canonicalID(used.builtin), noWitness);

    if (m_source.admitsInspector) {
        add(m_program.unseenCode, UnseenCodeCause::OptionAdmitsCode, { }, noWitness);
        m_uses.fill(everyBuiltinUseInProgramFacts, m_uses.size());
    } else {
        for (BuiltinMark mark : { BuiltinMark::IsResolverThatLoaderCalls, BuiltinMark::IsRequireThatLoaderCalls }) {
            for (const BuiltinMarkedEntry& marked : m_builtins.marked(mark)) {
                BuiltinID member = marked.builtin;
                const LookupsPastProgramObjects& lookups = m_inputs.linked.lookups;
                PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
                bool isWritten = lookups.membersWritten.get(member.index()) || lookups.writesUnderAnyNameThroughUnseenByProgram || (key && lookups.namesWrittenThroughUnseenByProgram.get(key.index()));
                BuiltinID holder = m_builtins.canonicalID(m_builtins.at(member).holder);
                if (isWritten && holder && !m_uses[holder.index()].isEmpty())
                    letGo(holder, noWitness);
            }
        }
        for (const Finding& read : m_readsOffObjectsOfModulesThatNoMemberAnswers) {
            if (m_namesThatRowsLayOnBuiltins.get(read.key))
                letGo(m_builtins.canonicalID(BuiltinID(read.subject)), read.witness);
        }
        if (m_inputs.linked.lookups.climbsFromLostBuiltins && !std::exchange(m_followsClimbsFromWhatIsLetGo, true)) {
            for (size_t index : m_isLetGo)
                m_holdersToSpreadBelow.append(BuiltinID(static_cast<uint32_t>(index)));
        }
        for (size_t index : m_isLookedThrough)
            m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
        lookThroughPrototypesOfClassesWithObjects();
    }
    listBuiltinsThatLetCodeIn();
    listLoadsOfUnseenCode();
    listLoadsOfMachineCode();

    if (m_eventsAreWithheld && !m_source.admitsInspector && !m_program.unseenCode.isEmpty()) {
        bool unseenCodeRunsAlready = hasRecordThat(admitsUnknownJavaScriptInProgramFacts);
        m_eventsAreWithheld = false;
        for (size_t index : m_isLetGo)
            m_holdersToSpreadBelow.append(BuiltinID(static_cast<uint32_t>(index)));
        for (size_t index : m_isLookedThrough)
            m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
        lookThroughPrototypesOfClassesWithObjects();
        if (!unseenCodeRunsAlready) {
            listBuiltinsThatLetCodeIn();
            listLoadsOfUnseenCode();
            listLoadsOfMachineCode();
        }
    }

    if (hasRecordThat(admitsUnknownJavaScriptInProgramFacts) && !m_source.admitsInspector && !m_defersRuntimeNames) {
        letGo(m_globalObject, noWitness);
        if (!std::exchange(m_objectsOfAnyClassMayExist, true))
            m_classesWithObjectsHaveGrown = true;
        lookThroughPrototypesOfClassesWithObjects();
    }
}

void BuilderOfProgramFacts::takeBuiltinsThatCameIntoReach(Vector<BuiltinID>& list)
{
    bool holderOfKeyThatPrinterLooksUnderIsAtHand = std::ranges::any_of(m_holdersOfKeysThatPrinterLooksUnder, [&](BuiltinID holder) {
        return m_isLookedThrough.quickGet(holder.index()) || !(m_uses[holder.index()] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty();
    });
    for (size_t index = 0; index < m_classesWithObjects.size(); ++index) {
        BuiltinClassID builtinClass(static_cast<uint32_t>(index));
        if (!m_classesWithObjects.at(index) || m_constructorIsListedForObjects.quickSet(index) || m_builtins.isSameOrSubclassOf(builtinClass, BuiltinClasses::Function))
            continue;
        if (BuiltinID constructor = m_builtins.at(builtinClass).constructor)
            list.append(m_builtins.canonicalID(constructor));
    }
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        BuiltinID builtin(index);
        if (m_builtins.canonicalID(builtin) != builtin || (m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        if (!m_hasComeIntoReach.quickSet(index))
            list.append(builtin);
        if (m_objectsMayHaveMethodCalledWithStartupLoads || !mayRunOrHasObjects(builtin))
            continue;
        const Builtin& entry = m_builtins.at(builtin);
        bool givesOnlySymbolsRegisteredUnderOtherTexts = builtin == m_builtins.canonicalID(Builtins::SymbolFor) && !m_registryMayGiveKeyOfPrinter && !anyRowMayApply(builtin);
        bool givesSymbolInUse = mayRun(builtin) && !givesOnlySymbolsRegisteredUnderOtherTexts && !entry.flags.contains(BuiltinFlag::IsCompilerHelper) && !liesInModuleThatProgramHolds(builtin) && std::ranges::any_of(m_builtins.overloadIDsOf(builtin), [&](BuiltinOverloadID row) {
            if (noCallAppliesRow(builtin, row))
                return false;
            const BuiltinOverload& overload = m_builtins.at(row);
            if (overload.isUndeclared() || !overload.result.type) {
                const RuntimeFunctionEntry* code = findRuntimeFunction(m_source, overload.code.implementation.function);
                return overload.isUndeclared() && !(code && code->function && !entersNativeCode(overload.code.implementation));
            }
            const BuiltinType& given = m_builtins.at(overload.result.type);
            if (given.isAnything())
                return entry.origin == BuiltinOrigin::Host && !handsBackWhatProgramHandedInInProgramFacts(overload);
            bool listsSymbols = std::ranges::any_of(given.parts, [&](const BuiltinTypePart& part) {
                return std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
                    return m_builtins.at(contents.type).primitives.contains(BuiltinPrimitive::Symbol);
                });
            });
            if (listsSymbols)
                return holderOfKeyThatPrinterLooksUnderIsAtHand;
            if (!given.primitives.contains(BuiltinPrimitive::Symbol) || given.freshness == BuiltinFreshness::Yes)
                return false;
            auto mayBringSymbol = [&](const BuiltinParameter& parameter) {
                const BuiltinType& accepted = m_builtins.at(parameter.accepted);
                return accepted.mayBeObject() || accepted.primitives.contains(BuiltinPrimitive::Symbol);
            };
            if (overload.mode == BuiltinCallMode::Get) {
                BuiltinName onlySymbolGiven = m_builtins.nameOfSymbol(builtin);
                return !onlySymbolGiven || std::ranges::contains(m_builtins.namesOfHostMethodsCallingStartupLoads(), onlySymbolGiven);
            }
            return !(overload.receiver && mayBringSymbol(*overload.receiver)) && std::ranges::none_of(overload.parameters, mayBringSymbol);
        });
        BuiltinID prototypeOfInstances = entry.instanceClass ? m_builtins.at(entry.instanceClass).prototype : BuiltinID();
        bool formatterMayComeByInstance = !prototypeOfInstances || formatterMayComeByWhatStandsBelow(m_builtins.canonicalID(prototypeOfInstances));
        m_objectsMayHaveMethodCalledWithStartupLoads |= givesSymbolInUse || (m_formattersMayCallMethodsOfInstances && m_builtins.has(BuiltinMark::InstancesHaveMethodCalledWithStartupLoads, builtin) && formatterMayComeByInstance)
            || std::ranges::contains(m_builtins.namesOfHostMethodsCallingStartupLoads(), entry.name);
    }
}

void BuilderOfProgramFacts::takeBuiltinsThatCameOutOfSight(Vector<BuiltinID>& list)
{
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        if (m_builtins.canonicalID(BuiltinID(index)).index() != index)
            continue;
        if (!m_uses[index].contains(BuiltinUse::Escaped) && !m_isOutOfSight.quickGet(index) && !m_isCalledOutOfSightOfLink.quickGet(index))
            continue;
        if (!m_hasComeOutOfSight.quickSet(index))
            list.append(BuiltinID(index));
    }
}

void BuilderOfProgramFacts::run()
{
    m_linkIsAtWork = false;
    take(m_inputs.linked.lookups);
    for (uint32_t index = 0; index < m_source.functions.size(); ++index) {
        if (isReached(FunctionID(index)))
            noteReached(FunctionID(index));
    }
    followModules();
    settle();

    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    AOT::sort(m_program.unseenCode, { }, orderOf);
    AOT::sort(m_program.loadsOfMachineCode, { }, orderOf);
    m_program.unseenCodeCanRun = hasRecordThat(admitsUnknownJavaScriptInProgramFacts);
    m_program.unseenScriptsCanRun = hasRecordThat(admitsScriptsInProgramFacts);
    m_program.setsMainModulePath = isMainModulePathReplaceable();
    m_program.hasOneInstance = needsOneInstanceAtMost();

    listBuiltinUses();
    listHoldersWhoseKeysAreNeverListed();
    lockMembersThatNobodyWrites();
    reportWritesThatLocksRefuse();
    reportLoadsUnderNamesThatNobodyHasRead();
    BitVector builtinsWhoseOperandsNobodyKnows(m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (anyRowMayApply(BuiltinID(index)) || m_isCalledByHost.quickGet(index))
            builtinsWhoseOperandsNobodyKnows.quickSet(index);
    }
    listWorkers();
    bool unseenCodeMayRun = !m_program.unseenCode.isEmpty() || !m_program.loadsOfMachineCode.isEmpty() || !m_program.workerStartsWithoutName.isEmpty();
    BitVector foundByNameAlone = builtinsFoundByNameAlone();
    BitVector handedOnByCodeInSight = builtinsThatCodeInSightHandsOn();
    m_tables.featureUses = FeaturesOfProgramFacts(m_inputs, m_refused, builtinsWhoseOperandsNobodyKnows, foundByNameAlone, handedOnByCodeInSight, unseenCodeMayRun).run(usesWithWhatNobodyNamesAsEscaped().span());
    layPlacesWhereProgramReadsNames();
    m_program.mayReadLegacyRegExpStatics = mayReadRecordOfLastMatch();
    m_program.matchesRegExps = m_program.unseenCodeCanRun || std::ranges::contains(m_tables.featureUses, static_cast<uint32_t>(RemovableFeature::RegExp), &FeatureUseRecord::feature);
    listLibraryCodeLacked();
    listLocaleUses();
}

void computeFactsOfProgram(const InputsOfProgramFacts& inputs, FactTables& tables)
{
    BuilderOfProgramFacts(inputs, tables).run();
}

BuiltinsInReach::BuiltinsInReach(const Program& program, const LooksAtBodies& looks, const ThinSummaries& summaries, BuildContext& build)
    : m_tables(makeUniqueWithoutFastMallocCheck<FactTables>())
    , m_linked(makeUniqueWithoutFastMallocCheck<LinkTables>())
    , m_inputs(makeUniqueWithoutFastMallocCheck<InputsOfProgramFacts>(InputsOfProgramFacts { program, looks.bodies, looks, summaries, *m_linked, build }))
    , m_builder(makeUniqueWithoutFastMallocCheck<BuilderOfProgramFacts>(*m_inputs, *m_tables))
{
    const BuiltinTable& builtins = program.builtins();
    if (build.options().excludesFeaturesNamedAtRunTime())
        m_builder->deferRuntimeNames();
    m_builtinsWorthFollowingToWrites.ensureSize(builtins.builtinCount());
    if (program.admitsReplacedBuiltins) {
        for (BuiltinID member : program.builtinsWhoseLockIsLeftToProgram())
            m_builtinsWorthFollowingToWrites.quickSet(builtins.canonicalID(builtins.at(member).holder).index());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.canonicalID(BuiltinID(index)).index() == index && builtins.canTakeNameInTestRuns(BuiltinID(index), { }))
                m_builtinsWorthFollowingToWrites.quickSet(index);
        }
    }
    bool hasGrown = true;
    while (std::exchange(hasGrown, false)) {
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID member(index);
            const Builtin& entry = builtins.at(member);
            if (!entry.holder || (!m_builder->writes().canLockAtStartUp(member) && !m_builtinsWorthFollowingToWrites.quickGet(builtins.canonicalID(member).index())))
                continue;
            hasGrown |= !m_builtinsWorthFollowingToWrites.quickSet(builtins.canonicalID(entry.holder).index());
        }
    }
}

BuiltinsInReach::~BuiltinsInReach() = default;

void BuiltinsInReach::leaveOutDirectEvals()
{
    m_builder->leaveOutDirectEvals();
}

bool BuiltinsInReach::hasNoFindingsIn(FunctionID id) const
{
    return m_builder->hasNoFindingsIn(id);
}

bool BuiltinsInReach::knowsWhatBodyMakes(FunctionID id) const
{
    return m_builder->knowsWhatBodyMakes(id);
}

void BuiltinsInReach::noteFindingsIn(FunctionID id)
{
    m_wasToldSomethingNew = true;
    m_builder->noteFindingsIn(id);
}

void BuiltinsInReach::noteFindingsIn(std::span<const FunctionID> functions)
{
    if (functions.empty())
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteFindingsIn(functions);
}

void BuiltinsInReach::noteWhatBodyMakes(FunctionID id)
{
    m_wasToldSomethingNew = true;
    m_builder->noteObjectsThatBodyMakes(id);
}

void BuiltinsInReach::noteEvaluated(ModuleID id)
{
    m_wasToldSomethingNew = true;
    m_builder->noteEvaluated(id);
}

void BuiltinsInReach::noteNativeCodeMayHoldObjects()
{
    if (std::exchange(m_linked->lookups.nativeCodeMayHoldObjects, true))
        return;
    m_wasToldSomethingNew = true;
    m_builder->setNativeCodeMayHoldObjects(true);
}

void BuiltinsInReach::noteNameRead(PropertyKeyID key)
{
    if (m_linked->lookups.namesRead.set(key.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameRead(key);
}

void BuiltinsInReach::noteNameCalled(PropertyKeyID key, bool mayPassOperands)
{
    bool isNews = !m_linked->lookups.namesCalled.set(key.index());
    if (mayPassOperands)
        isNews |= !m_linked->lookups.namesCalledWithOperands.set(key.index());
    if (!isNews)
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameCalled(key, mayPassOperands);
}

void BuiltinsInReach::noteNameCalledByNativeCodeOffUnseenObjects(PropertyKeyID key)
{
    if (m_linked->lookups.namesCalledByNativeCodeOffUnseenObjects.set(key.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameCalledByNativeCodeOffUnseenObjects(key);
}

void BuiltinsInReach::noteNameSpelledByProgram(PropertyKeyID key)
{
    if (m_linked->lookups.namesSpelledByProgram.set(key.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameSpelledByProgram(key);
}

void BuiltinsInReach::noteReadOfProgramUnderKeyNobodyKnows(FunctionID reader, ValueID site)
{
    m_linked->lookups.readsOfProgramUnderKeysNobodyKnows.add((static_cast<uint64_t>(reader.index()) + 1) << 32 | site.index());
}

void BuiltinsInReach::noteReadUnderNameThatLoopGave()
{
    if (std::exchange(m_linked->lookups.readsUnderNamesThatLoopsGave, true))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteReadUnderNameThatLoopGave();
}

void BuiltinsInReach::noteReadUnderKeyNobodyKnowsOff(BuiltinID canonicalHolder)
{
    if (m_linked->lookups.holdersReadUnderKeysNobodyKnows.set(canonicalHolder.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteReadUnderKeyNobodyKnowsOff(canonicalHolder);
}

void BuiltinsInReach::noteReadUnderKeyNobodyKnows()
{
    if (std::exchange(m_linked->lookups.readsUnderKeysNobodyKnows, true))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteReadUnderKeyNobodyKnows();
}

std::span<const FunctionID> BuiltinsInReach::functionsThatAnswerEveryNameOnInstances() const
{
    return m_builder->functionsThatAnswerEveryNameOnInstances();
}

void BuiltinsInReach::noteInstancesAnswerEveryNameWithObjects()
{
    if (m_builder->objectsMayHaveMethodCalledWithStartupLoads())
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteInstancesAnswerEveryNameWithObjects();
}

void BuiltinsInReach::noteRead(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsRead.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteReadOfMember(builtin);
}

void BuiltinsInReach::noteCalled(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsCalled.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteCall(builtin);
}

void BuiltinsInReach::noteConstructed(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsConstructed.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteConstruction(builtin);
}

void BuiltinsInReach::noteLetGo(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsLetGo.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteLetGoByLink(builtin);
    m_builder->noteLossOf(builtin);
}

void BuiltinsInReach::noteCalledAtSite(FunctionID caller, ValueID site, bool isConstruction, BuiltinID row)
{
    m_linked->lookups.callsOfBuiltinsAtSites.append(LinkedCallOfBuiltin { caller, site, row, isConstruction });
}

void BuiltinsInReach::noteCalledByCode(BuiltinID builtin)
{
    m_linked->lookups.builtinsCalledByCode.set(builtin.index());
}

void BuiltinsInReach::noteCalledOutOfSight(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsCalledOutOfSight.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteCallOutOfSight(builtin);
}

void BuiltinsInReach::noteRowApplied(BuiltinID canonical, BuiltinOverloadID row)
{
    if (m_linked->lookups.rowsApplied.set(row.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteRowApplied(canonical, row);
}

Vector<uint64_t> BuiltinsInReach::takeCallsWhoseRowsLinkTells()
{
    return m_builder->takeCallsWhoseRowsLinkTells();
}

void BuiltinsInReach::noteCallLeftToUnseen(FunctionID caller, ValueID site)
{
    m_wasToldSomethingNew |= m_builder->noteCallLeftToUnseen(BuilderOfProgramFacts::wordOfCall(caller.index(), site.index()));
}

void BuiltinsInReach::noteCalledWithoutRow(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsCalledWithoutRow.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteCallWithoutRow(builtin);
}

void BuiltinsInReach::noteCalledWithOperandsNobodyRead(BuiltinID builtin)
{
    m_linked->lookups.builtinsCalledWithOperandsNobodyRead.set(builtin.index());
}

void BuiltinsInReach::noteStandsAboveClass(BuiltinID builtin, FunctionID constructor)
{
    uint64_t pair = static_cast<uint64_t>(builtin.index()) << 32 | constructor.index();
    if (!m_linked->lookups.constructorsBelowBuiltins.add(pair).isNewEntry)
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteStandsAboveClass(builtin, constructor);
}

void BuiltinsInReach::noteFoundWhereFlowBroughtHolder(BuiltinID builtin, FunctionID reader)
{
    if (!m_linked->lookups.builtinsFoundWhereFlowBroughtHolders.add(static_cast<uint64_t>(builtin.index()) << 32 | reader.index()).isNewEntry)
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteFoundWhereFlowBroughtHolder(builtin, reader);
}

void BuiltinsInReach::recordUsedViaFlow(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsUsedViaFlow.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteLetGoByLink(builtin);
}

void BuiltinsInReach::noteTypeHandedToHandler(BuiltinTypeID type)
{
    if (m_linked->lookups.typesHandedToHandlers.set(type.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteTypeHandedToHandler(type);
}

void BuiltinsInReach::noteInstancesLoseTheirConstructor()
{
    if (std::exchange(m_linked->lookups.instancesLoseTheirConstructor, true))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteInstancesLoseTheirConstructor();
}

void BuiltinsInReach::noteObjectHandedToFormatter(bool isWalked)
{
    bool isNews = !std::exchange(m_linked->lookups.formattersMayCallMethodsOfInstances, true);
    if (isWalked)
        isNews |= !std::exchange(m_linked->lookups.formattersAreHandedObjects, true);
    if (!isNews)
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteObjectHandedToFormatter(isWalked);
}

void BuiltinsInReach::noteCallMayRunCode(BuiltinID builtin)
{
    m_linked->lookups.builtinsWithCallsThatMayRunCode.set(builtin.index());
}

static uint64_t wordOfConditionInProgramFacts(BuiltinID canonical, BuiltinName toldUnder)
{
    return (static_cast<uint64_t>(canonical.index()) + 1) << 32 | toldUnder.index();
}

void BuiltinsInReach::noteOperandMayMeetCondition(BuiltinID builtin, BuiltinName toldUnder)
{
    if (!m_linked->lookups.conditionsThatOperandsMayMeet.add(wordOfConditionInProgramFacts(builtin, toldUnder)).isNewEntry || builtin != m_inputs->program.builtins().canonicalID(Builtins::SymbolFor))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteRegistryMayGiveKeyOfPrinter();
}

bool BuiltinsInReach::hasCallWhoseOperandMayMeet(const BuiltinCondition& condition, BuiltinID canonical) const
{
    if (m_linked->lookups.conditionsThatOperandsMayMeet.contains(wordOfConditionInProgramFacts(canonical, condition.nameToldUnder())))
        return true;
    return m_builder->anyRowMayApply(canonical) && (!condition.calledOn || m_builder->anyRowMayApply(m_inputs->program.builtins().canonicalID(condition.calledOn)));
}

bool BuiltinsInReach::hasCallThatMayRunCode(BuiltinID canonical) const
{
    return m_linked->lookups.builtinsWithCallsThatMayRunCode.get(canonical.index()) || m_builder->anyRowMayApply(canonical);
}

bool BuiltinsInReach::mayRun(BuiltinID canonical) const
{
    return m_builder->mayRun(canonical);
}

bool BuiltinsInReach::mayRunOrHasObjects(BuiltinID canonical) const
{
    return m_builder->mayRunOrHasObjects(canonical);
}

bool BuiltinsInReach::mayBeHandedWhatIsNoTextNumberOrBoolean(BuiltinID canonical) const
{
    const LookupsPastProgramObjects& lookups = m_linked->lookups;
    return lookups.holdersHandedWhatIsNoTextNumberOrBoolean.get(canonical.index()) || lookups.objectsNobodyKnowsAreHandedWhatIsNoTextNumberOrBoolean;
}

void BuiltinsInReach::noteWrite(BuiltinID holder, PropertyKeyID key, bool onlyDeletes)
{
    const Program& program = m_inputs->program;
    holder = program.builtins().canonicalID(holder);
    if (onlyDeletes && (!key || findOwnMember(program, holder, key)) && !m_linked->lookups.holdersWhoseMembersAreDeleted.set(holder.index())) {
        m_wasToldSomethingNew = true;
        m_builder->noteDeletionOfMemberOf(holder);
    }
    if (!key) {
        m_wasToldSomethingNew |= !m_linked->lookups.holdersWrittenUnderAnyName.set(holder.index());
        if (!onlyDeletes && !m_linked->lookups.holdersStoredOnUnderAnyName.set(holder.index())) {
            m_wasToldSomethingNew = true;
            m_builder->noteStoreUnderKeyNobodyKnowsOn(holder);
        }
        return;
    }
    if (BuiltinID member = findOwnMember(program, holder, key))
        m_wasToldSomethingNew |= !m_linked->lookups.membersWritten.set(member.index());
}

void BuiltinsInReach::noteHandedWhatIsNoTextNumberOrBoolean(BuiltinID holder)
{
    if (holder)
        m_wasToldSomethingNew |= !m_linked->lookups.holdersHandedWhatIsNoTextNumberOrBoolean.set(m_inputs->program.builtins().canonicalID(holder).index());
    else
        m_wasToldSomethingNew |= !std::exchange(m_linked->lookups.objectsNobodyKnowsAreHandedWhatIsNoTextNumberOrBoolean, true);
}

void BuiltinsInReach::noteWriteThroughLostBuiltins(PropertyKeyID key, bool isByFileOfProduct)
{
    LookupsPastProgramObjects& lookups = m_linked->lookups;
    m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughLostBuiltins.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughLostBuiltins, true);
    if (!isByFileOfProduct && m_inputs->program.admitsReplacedBuiltins)
        m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughLostBuiltinsBesideProduct.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughLostBuiltinsBesideProduct, true);
}

void BuiltinsInReach::noteNameWrittenThroughUnseenByLibrary(PropertyKeyID key)
{
    m_wasToldSomethingNew |= !m_linked->lookups.namesWrittenThroughUnseenByLibrary.set(key.index());
}

void BuiltinsInReach::noteWriteThroughUnseenByProgram(PropertyKeyID key, bool isByFileOfProduct)
{
    LookupsPastProgramObjects& lookups = m_linked->lookups;
    m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughUnseenByProgram.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughUnseenByProgram, true);
    m_builder->noteWriteThroughUnseenByProgram(key);
    if (!isByFileOfProduct && m_inputs->program.admitsReplacedBuiltins)
        m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughUnseenBesideProduct.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughUnseenBesideProduct, true);
}

void BuiltinsInReach::noteReadThroughLostBuiltins(PropertyKeyID key)
{
    m_wasToldSomethingNew |= !m_linked->lookups.namesReadThroughLostBuiltins.set(key.index());
}

void BuiltinsInReach::noteEntryHandsHostNothing(uint32_t entry)
{
    m_linked->lookups.entriesThatHandHostNothing.set(entry);
}

void BuiltinsInReach::noteHostStartsByCallInSight(uint32_t entry)
{
    m_linked->lookups.entriesThatHostStartsByCallInSight.set(entry);
}

void BuiltinsInReach::noteHostMayFindSomethingAt(uint32_t entry)
{
    m_wasToldSomethingNew |= m_linked->lookups.entriesThatHandHostNothing.clear(entry);
}

void BuiltinsInReach::noteClimbFromLostBuiltins()
{
    m_wasToldSomethingNew |= !std::exchange(m_linked->lookups.climbsFromLostBuiltins, true);
}

void BuiltinsInReach::noteClimbFromUnseenObjects()
{
    m_wasToldSomethingNew |= !std::exchange(m_linked->lookups.climbsFromUnseenObjects, true);
}

void BuiltinsInReach::noteLost(BuiltinID builtin)
{
    if (builtin && isWorthFollowingToWrites(builtin))
        m_wasToldSomethingNew |= !m_linked->lookups.lostBuiltins.set(builtin.index());
}

void BuiltinsInReach::noteAtUnseen(BuiltinID builtin)
{
    m_wasToldSomethingNew |= !m_linked->lookups.builtinsAtUnseen.set(builtin.index());
}

void BuiltinsInReach::noteOptionThatRunsCodePassed(BuiltinID builtin)
{
    m_wasToldSomethingNew |= !m_linked->lookups.builtinsPassedOptionThatRunsCode.set(builtin.index());
}

void BuiltinsInReach::noteLostBuiltinsReachUnseen()
{
    m_wasToldSomethingNew |= !std::exchange(m_linked->lookups.lostBuiltinsReachUnseen, true);
}

void BuiltinsInReach::noteInstanceHasEscaped(BuiltinClassID builtinClass)
{
    if (m_linked->lookups.classesWhoseInstancesHaveEscaped.set(builtinClass.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteInstanceHasEscaped(builtinClass);
}

void BuiltinsInReach::noteInstanceAtUnseen(BuiltinClassID builtinClass)
{
    if (m_linked->lookups.classesWithInstancesAtUnseen.set(builtinClass.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteInstancesMayReachFormatters(builtinClass);
}

void BuiltinsInReach::noteInstanceHandedToFormatter(BuiltinClassID builtinClass)
{
    if (!builtinClass) {
        if (std::exchange(m_linked->lookups.formattersAreHandedInstancesOfSeveralClasses, true))
            return;
        m_wasToldSomethingNew = true;
        m_builder->noteInstancesOfAnyClassMayReachFormatters();
        return;
    }
    if (m_linked->lookups.classesHandedToFormatters.set(builtinClass.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteInstancesMayReachFormatters(builtinClass);
}

static bool mayStillBeLockedInProgramFacts(const WritesOnBuiltinsInProgramFacts& writes, BuiltinID member)
{
    return writes.canLockAtStartUp(member) && !writes.isWrittenAsFarAsLinkGoes(member);
}

void BuiltinsInReach::settle(Vector<BuiltinID>& cameIntoReach, Vector<BuiltinID>& cameOutOfSight)
{
    if (!std::exchange(m_wasToldSomethingNew, false))
        return;
    m_builder->settle();
    if (m_builder->climberMayBeHandedWhatNobodyKnows()) {
        if (!std::exchange(m_linked->lookups.climbsFromUnseenObjects, true))
            m_builder->settle();
    }
    m_builder->takeBuiltinsThatCameIntoReach(cameIntoReach);
    m_builder->takeBuiltinsThatCameOutOfSight(cameOutOfSight);

    m_builder->writes().settle();
    BitVector& taken = m_linked->lookups.membersTakenForConstants;
    for (size_t index : taken) {
        BuiltinID member(static_cast<uint32_t>(index));
        if (!mayStillBeLockedInProgramFacts(m_builder->writes(), member))
            m_membersWrittenAfterAll.append(member);
    }
    for (BuiltinID member : m_membersWrittenAfterAll)
        taken.clear(member.index());
    m_namesTakenForUnlaid.removeAllMatching([&](const NameOnHolder& taken) {
        if (!mayHaveNameLaid(taken.holder, taken.key))
            return false;
        m_namesLaidAfterAll.append(taken);
        return true;
    });
}

bool BuiltinsInReach::mayHaveNameLaid(BuiltinID canonicalHolder, PropertyKeyID key) const
{
    return m_builder->machineCodeMayBeLoaded() || m_builder->writes().mayHaveNameLaidAsFarAsLinkGoes(canonicalHolder, key);
}

void BuiltinsInReach::takeForUnlaid(BuiltinID canonicalHolder, PropertyKeyID key)
{
    if (!m_namesEverTakenForUnlaid.add(static_cast<uint64_t>(canonicalHolder.index()) << 32 | key.bits()).isNewEntry)
        return;
    m_namesTakenForUnlaid.append(NameOnHolder { canonicalHolder, key });
    m_wasToldSomethingNew = true;
}

bool BuiltinsInReach::mayStillBeTakenForConstant(BuiltinID member) const
{
    return mayStillBeLockedInProgramFacts(m_builder->writes(), member);
}

bool BuiltinsInReach::takeForConstant(BuiltinID member)
{
    BitVector& taken = m_linked->lookups.membersTakenForConstants;
    if (taken.get(member.index()))
        return true;
    if (!mayStillBeLockedInProgramFacts(m_builder->writes(), member))
        return false;
    taken.set(member.index());
    return true;
}

LookupsPastProgramObjects BuiltinsInReach::takeLookups()
{
    m_linked->lookups.builtinsWithCallsLeftToUnseen = m_builder->builtinsWithCallsLeftToUnseen();
    return std::exchange(m_linked->lookups, { });
}

void BuiltinsInReach::putBackLookups(const LookupsPastProgramObjects& lookups)
{
    m_linked->lookups = lookups;
}

bool BuiltinsInReach::loaderOfHostMayRun() const
{
    return m_builder->loaderOfHostMayRun();
}

bool BuiltinsInReach::hostIsAskedForValueOfModule() const
{
    return m_builder->hostIsAskedForValueOfModule();
}

bool BuiltinsInReach::objectsMayHaveMethodCalledWithStartupLoads() const
{
    return m_builder->objectsMayHaveMethodCalledWithStartupLoads();
}

bool BuiltinsInReach::isOutOfSight(BuiltinID canonical) const
{
    return m_builder->isOutOfSight(canonical);
}

bool BuiltinsInReach::isAskedForOtherNames(BuiltinID canonical) const
{
    return m_builder->isAskedForOtherNames(canonical);
}

const BuiltinCondition* BuiltinsInReach::lightFormOf(BuiltinID canonical) const
{
    return m_builder->lightFormOf(canonical);
}

bool BuiltinsInReach::defersRuntimeNames() const
{
    return m_builder->defersRuntimeNames();
}

Vector<ModuleID> BuiltinsInReach::takeModulesSpelledInTexts()
{
    return m_builder->takeModulesSpelledInTexts();
}

bool BuiltinsInReach::allowRuntimeNames()
{
    if (!m_builder->defersRuntimeNames() || (!m_builder->hasRecordOfUnseenCode() && !m_builder->hasDeferredMembers()))
        return false;
    LookupsPastProgramObjects& lookups = m_linked->lookups;
    lookups.wereRuntimeNamesDeferred = true;
    lookups.staticallyInstantiatedClasses = m_builder->classesWithObjects();
    lookups.staticallyUsedBuiltins = m_builder->allowRuntimeNames();
    m_wasToldSomethingNew = true;
    return true;
}

bool BuiltinsInReach::loaderOfMachineCodeIsInUse() const
{
    return m_builder->loaderOfMachineCodeIsInUse();
}

bool BuiltinsInReach::unseenCodeCanRun() const
{
    return m_builder->unseenCodeCanRun();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
