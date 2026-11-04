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
#include "AOTIRProgram.h"
#include "AOTObjects.h"
#include "AOTOpcodeSets.h"
#include "AOTRemovableFeature.h"
#include "AOTResultRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "IntlObject.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet namersOfBuiltinsInProgramFacts { Opcode::Builtin, Opcode::ModuleBuiltin, Opcode::NativeBinding, Opcode::RequireBuiltinModule };
// `require("bun:jsc")`: the object of a module of native code.
static constexpr OpcodeSet giversOfBuiltinsInProgramFacts = namersOfBuiltinsInProgramFacts | OpcodeSet { Opcode::RequireModule };
static constexpr OpcodeSet loadsByNameInProgramFacts { Opcode::ImportDynamic, Opcode::RequireDynamic };
static constexpr OpcodeSet loadsAndEvaluationsInProgramFacts { Opcode::ModuleNamespace, Opcode::ImportModule, Opcode::RequireModule, Opcode::EvaluateModule, Opcode::ImportDynamic, Opcode::RequireDynamic, Opcode::ResolveDynamic, Opcode::DirectEval };
static constexpr OpcodeSet accessesUnderConstantKeysInProgramFacts { Opcode::PropGet, Opcode::PropHas, Opcode::PropSet, Opcode::PropDefine, Opcode::PropDelete };
static constexpr OpcodeSet callsByValueInProgramFacts { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet constructionsInProgramFacts { Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet callsThatSpellOutArgumentsInProgramFacts { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct };

static constexpr OptionSet<BuiltinUse> everyBuiltinUseInProgramFacts { BuiltinUse::Tested, BuiltinUse::Read, BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Written, BuiltinUse::Escaped, BuiltinUse::StoredByName };
static constexpr OptionSet<BuiltinUse> callingUsesInProgramFacts { BuiltinUse::Called, BuiltinUse::Constructed };
// Every other use describes one location that holds the object, not the object.
static constexpr OptionSet<BuiltinUse> usesOfObjectItselfInProgramFacts { BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };

// `const { Script } = $cpp("NodeVM.cpp", "Bun::createNodeVMBinding")`: the one object that the rows of the binding answer. The binding itself where they do not name one.
static BuiltinID objectAnsweredByBindingInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
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

static void keepLowerWitnessInProgramFacts(Witness& lowest, const Witness& candidate)
{
    if (isLowerWitness(candidate, lowest))
        lowest = candidate;
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

// Evaluated text declares nothing in the scope of scripts.
static constexpr bool admitsScriptsInProgramFacts(UnseenCodeCause cause)
{
    return admitsUnknownJavaScriptInProgramFacts(cause) && cause != UnseenCodeCause::EvaluatesText;
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

// `Bun.serve({ app })`: the call runs unseen code only where an operand has a property of some name.
static const BuiltinCondition* optionThatRunsCodeInProgramFacts(const BuiltinTable& builtins, BuiltinID canonical)
{
    const BuiltinUnseenCode* row = builtins.unseenCodeOf(canonical);
    return row && row->condition && row->condition->hasProperty ? row->condition : nullptr;
}

// ---- What the table says of rows, asked once

struct LocaleOperandInProgramFacts {
    BuiltinID row;
    BuiltinCallMode mode;
    BuiltinOperand operand;

    friend bool operator==(const LocaleOperandInProgramFacts&, const LocaleOperandInProgramFacts&) = default;
};

// All by canonical built-in.
struct RowsOfProgramFacts {
    explicit RowsOfProgramFacts(const BuiltinTable&);

    static constexpr uint32_t bitOfArgument(size_t argument) { return 1u << std::min<size_t>(argument, 31); }

    BitVector takeLocales;
    // Nothing says which operand is the locale, and a feature of the row reads the data of locales.
    BitVector readLocalesWithoutDescription;
    // Ascending by row.
    Vector<LocaleOperandInProgramFacts> localeOperands;
    BitVector startWorkers;
    // Bit i: argument i may be the file. Bit 31 stands for every argument from 31 on.
    uint32_t fileArguments { 0 };
    BuiltinID constructorOfURLs;
    // callOrReadOfBuiltinIsEvent().
    BitVector areEvents;
    // A row writes on an operand under a key that the call brings: `Reflect.set(target, key, value)`, `Object.assign(target, source)`.
    BitVector writeUnderKeysOfCalls;
};

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

RowsOfProgramFacts::RowsOfProgramFacts(const BuiltinTable& builtins)
    : takeLocales(builtins.builtinCount())
    , readLocalesWithoutDescription(builtins.builtinCount())
    , startWorkers(builtins.builtinCount())
    , areEvents(builtins.builtinCount())
    , writeUnderKeysOfCalls(builtins.builtinCount())
{
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID entry(index);
        BuiltinID row = builtins.canonicalID(entry);
        if (row == entry && callOrReadOfBuiltinIsEvent(builtins, row))
            areEvents.quickSet(index);

        auto noteParameter = [&](BuiltinCallMode mode, const BuiltinParameter& parameter) {
            if (parameter.roles.contains(BuiltinOperandRole::Locale)) {
                takeLocales.quickSet(row.index());
                localeOperands.append(LocaleOperandInProgramFacts { row, mode, parameter.operand });
            }
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
            if (std::ranges::any_of(overload.flow.propertyActions, writesUnderKeyOfCallInProgramFacts))
                writeUnderKeysOfCalls.quickSet(row.index());
            if (const BuiltinParameter* receiver = overload.receiver.get())
                noteParameter(overload.mode, *receiver);
            for (const BuiltinParameter& parameter : overload.parameters)
                noteParameter(overload.mode, parameter);
        }
    }
    AOT::sort(localeOperands, { }, [](const LocaleOperandInProgramFacts& localeOperand) {
        BuiltinOperand operand = localeOperand.operand;
        return std::tuple(localeOperand.row, localeOperand.mode, operand.kind(), operand.isReceiver() ? 0u : operand.argumentIndex());
    });
    removeRepeatedElements(localeOperands);

    BuiltinClassID classOfURLs = BuiltinClasses::URL;
    if (BuiltinID constructor = classOfURLs ? builtins.at(classOfURLs).constructor : BuiltinID())
        constructorOfURLs = builtins.canonicalID(constructor);
}

// ---- The look at one body

struct FindingOfProgramFacts {
    // ReadOfAnyMember: a UseOfBuiltin that lets go of what lies on the object, and not of the object.
    // OptionThatRunsCode: a call in sight may pass the property with which the row runs unseen code.
    // NameReadOffUnknownValue: code of the program spells a name of the table in a read off a value that the look does not follow.
    // ReadThatNoRowAnswers: off a built-in that the look follows, under a name that the table neither lists there nor rules out.
    // KeysOfBuiltinListed: an operation, or a row that a call in sight may enter, goes through the own keys of a built-in that the look follows, or may run code of the program with it for `this`.
    enum class Kind : uint8_t { UseOfBuiltin, ReadOfAnyMember, WriteOnBuiltin, NameStoredByRow, NameReadOffUnknownValue, ReadThatNoRowAnswers, KeysOfBuiltinListed, OptionThatRunsCode, UnseenCode, LocaleUse, WorkerEntry, WorkerStartWithoutName, URLThatStartsWorker, URLThatLeavesBody, NamedModule };

    Kind kind { Kind::UseOfBuiltin };
    // UseOfBuiltin, ReadOfAnyMember: OptionSet<BuiltinUse>. UnseenCode: UnseenCodeCause. LocaleUse: LocaleUseKind. WorkerEntry: whether the start may have options.
    uint8_t detail { 0 };
    // UseOfBuiltin, ReadOfAnyMember, WriteOnBuiltin, OptionThatRunsCode, LocaleUse: BuiltinID. NameStoredByRow, NameReadOffUnknownValue: PropertyKeyID. WorkerEntry: StringID. NamedModule: ModuleID.
    uint32_t subject { Wire::none };
    // WriteOnBuiltin: PropertyKeyID, or none where nobody can read the key.
    uint32_t key { Wire::none };
    // LocaleUse of LocaleUseKind::Constant.
    StringID tag;
    Witness witness { noWitness };
};

// What the text of the program's own code shows of the numbers in a field. A guess at the form of a slot goes by it, and no proof. The looks run side by side and each only adds: in which order tells nothing.
// A field is named by the layout that declares it and its key, as the written type of the receiver leads to it. Off a receiver of which nothing is written a use says nothing, and a fraction speaks against the name.
struct VariableStoredInFieldInProgramFacts {
    LayoutID declarer;
    PropertyKeyID key;
    // One of the two: a variable of a module, a variable that closures share.
    VariableID ofModule;
    FieldReference ofEnvironment;
};

class NamesOfNumbersInProgramFacts {
    WTF_MAKE_NONCOPYABLE(NamesOfNumbersInProgramFacts);
public:
    explicit NamesOfNumbersInProgramFacts(size_t numberOfKeys)
        : m_numberOfKeys(numberOfKeys)
        , m_storedWhatIsNoInt32(std::make_unique<std::atomic<uint32_t>[]>(numberOfKeys / 32 + 1))
    {
    }

    void noteReadAsInteger(LayoutID declarer, PropertyKeyID key) { add(m_fieldsReadAsIntegers, declarer, key); }
    void noteReadForArithmetic(LayoutID declarer, PropertyKeyID key) { add(m_fieldsReadForArithmetic, declarer, key); }
    void noteStoredWhatIsNoInt32(LayoutID declarer, PropertyKeyID key) { add(m_fieldsStoredWhatIsNoInt32, declarer, key); }
    void noteStoredWhatIsNoInt32(PropertyKeyID key) { set(m_storedWhatIsNoInt32, key); }
    void noteStoredVariable(VariableStoredInFieldInProgramFacts store)
    {
        Locker locker { m_lock };
        m_variablesStoredInFields.append(store);
    }
    Vector<VariableStoredInFieldInProgramFacts> takeVariablesStoredInFields() { return WTF::move(m_variablesStoredInFields); }
    FieldsOfNumbers takeFieldsReadAsIntegers() { return WTF::move(m_fieldsReadAsIntegers); }
    FieldsOfNumbers takeFieldsReadForArithmetic() { return WTF::move(m_fieldsReadForArithmetic); }
    FieldsOfNumbers takeFieldsStoredWhatIsNoInt32() { return WTF::move(m_fieldsStoredWhatIsNoInt32); }
    BitVector keysStoredWhatIsNoInt32() const { return listed(m_storedWhatIsNoInt32); }

private:
    using Words = std::unique_ptr<std::atomic<uint32_t>[]>;

    // Few places in a program tell anything, so they can wait for one another.
    void add(FieldsOfNumbers& fields, LayoutID declarer, PropertyKeyID key)
    {
        Locker locker { m_lock };
        fields.add(declarer, key);
    }

    static void set(Words& words, PropertyKeyID key)
    {
        std::atomic<uint32_t>& word = words[key.index() / 32];
        uint32_t bit = 1u << (key.index() % 32);
        if (!(word.load(std::memory_order_relaxed) & bit))
            word.fetch_or(bit, std::memory_order_relaxed);
    }

    BitVector listed(const Words& words) const
    {
        BitVector result;
        result.ensureSize(m_numberOfKeys);
        for (size_t index = 0; index < m_numberOfKeys; ++index) {
            if (words[index / 32].load(std::memory_order_relaxed) >> (index % 32) & 1)
                result.quickSet(index);
        }
        return result;
    }

    size_t m_numberOfKeys;
    Words m_storedWhatIsNoInt32;
    Lock m_lock;
    FieldsOfNumbers m_fieldsReadAsIntegers;
    FieldsOfNumbers m_fieldsReadForArithmetic;
    FieldsOfNumbers m_fieldsStoredWhatIsNoInt32;
    Vector<VariableStoredInFieldInProgramFacts> m_variablesStoredInFields;
};

static constexpr OpcodeSet storesUnderConstantKeysInProgramFacts { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesUnderComputedKeysInProgramFacts { Opcode::ElemGet, Opcode::ElemSet };
static constexpr OpcodeSet readsOfVariablesInProgramFacts { Opcode::CheckInitialized, Opcode::GetModuleVariable, Opcode::GetEnvironmentField };
// `+` is not among them: it joins texts, and its other operand is mostly of no known type.
static constexpr OpcodeSet operatorsOnNumbersAloneInProgramFacts = operatorsOfKinds({ OperatorKind::Multiplicative }) | allRelations | OpcodeSet { Opcode::Sub, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet operationsThatTellOfNumbersInProgramFacts = allBitwiseOperators | operatorsOnNumbersAloneInProgramFacts | storesUnderConstantKeysInProgramFacts | accessesUnderComputedKeysInProgramFacts | OpcodeSet { Opcode::Call, Opcode::NewObject };
// A value is followed from the operation that names a built-in, through whatever hands it on, reads under constant keys and tests, as far as its own body. Anywhere else it is let go.
class BodyLookOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(BodyLookOfProgramFacts);
public:
    BodyLookOfProgramFacts(const Program& program, const RowsOfProgramFacts& rows, RefusedCode refused, FunctionID id, const BodyIndex& index, std::span<const BlockID> blocksNeverEntered, Vector<FindingOfProgramFacts>& found, NamesOfNumbersInProgramFacts& namesOfNumbers)
        : m_namesOfNumbers(namesOfNumbers)
        , m_program(program)
        , m_builtins(program.builtins())
        , m_rows(rows)
        , m_refused(refused)
        , m_id(id)
        , m_function(program.at(id))
        , m_isOfProgram(!isLibraryCode(program, m_function))
        , m_index(index)
        , m_blocksNeverEntered(blocksNeverEntered)
        , m_found(found)
    {
    }

    void run();

private:
    using Finding = FindingOfProgramFacts;
    enum class Fate : uint8_t { IsInspected, IsCalled, IsConstructed, IsAskedForAnyMember, IsLetGo };

    Witness witnessAt(const BodyValue& value) const { return { m_id.index(), value.id().index(), m_function.locationOf(value) }; }
    Witness witnessAt(const BodyTerminator& terminator) const { return { m_id.index(), Wire::none, m_function.locationOf(terminator) }; }

    BuiltinID builtinAt(ValueID value) const { return value ? m_builtinOfValue[value.index()] : BuiltinID(); }
    // Null too where the value may be something else.
    BuiltinID builtinSurelyAt(ValueID value) const { return value && !m_mayBeSomethingElse.get(value.index()) ? m_builtinOfValue[value.index()] : BuiltinID(); }
    ValueID originOfURL(ValueID value) const { return value && !m_originOfURLOfValue.isEmpty() ? m_originOfURLOfValue[value.index()] : ValueID(); }
    bool isFollowed(ValueID value) const { return builtinAt(value) || originOfURL(value); }
    BodyValue valueBehindCasts(ValueID value) const { return m_index.value(m_index.valueBehindCasts(value)); }

    void note(Finding::Kind kind, uint8_t detail, uint32_t subject, const Witness& witness, StringID tag = { }) { m_found.append(Finding { kind, detail, subject, Wire::none, tag, witness }); }
    void noteWrite(BuiltinID holder, PropertyKeyID key, const BodyValue& place) { m_found.append(Finding { Finding::Kind::WriteOnBuiltin, 0, holder.index(), key ? key.index() : Wire::none, { }, witnessAt(place) }); }
    void noteUse(BuiltinID builtin, BuiltinUse use, const auto& place) { note(Finding::Kind::UseOfBuiltin, static_cast<uint8_t>(OptionSet<BuiltinUse>(use).toRaw()), builtin.index(), witnessAt(place)); }
    void noteFate(ValueID, Fate, const auto& place);
    void handOn(ValueID from, ValueID to);
    bool passOn(ValueID argument, ValueID parameter, const BodyBlock& from);

    void look(const BodyValue&);
    TypeID writtenTypeOf(ValueID) const;
    void forEachDeclarerOfFieldOf(ValueID receiver, PropertyKeyID, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    void noteReadUsedAsInteger(ValueID);
    void noteReadUsedForArithmetic(ValueID);
    void noteWhatIsStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID, ValueID stored);
    TypeID writtenTypeWhereValueIsHandedOn(ValueID) const;
    void noteVariableStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID, ValueID stored);
    void noteWhatIsToldOfNumbers(const BodyValue&);
    void nameBuiltin(const BodyValue&, BuiltinID);
    void lookAtLoad(const BodyValue&);
    void noteObjectOfModule(RuntimeModuleID, const BodyValue& load);
    void accessMember(const BodyValue&, BuiltinID holder);
    void lookAtCall(const BodyValue&);
    void noteLocalesPassed(const BodyValue& call, BuiltinID row, BuiltinCallMode, bool spellsOutArguments);
    void noteStartOfWorker(const BodyValue& call, bool spellsOutArguments);
    bool listKeysOfLiteral(ValueID, Vector<PropertyKeyID, 4>&) const;
    bool mayPass(const BodyValue& call, const BuiltinCondition& option, bool spellsOutArguments) const;
    void noteWritesOfRow(const BodyValue& call, BuiltinID row, BuiltinCallMode, bool spellsOutArguments);
    void lookAtEnd(const BodyBlock&);

    BuiltinID memberFoundOn(BuiltinID holder, PropertyKeyID) const;
    bool tableRulesOutMemberFoundFrom(BuiltinID holder, PropertyKeyID) const;
    bool mayGoThroughOwnKeysOf(const BodyValue& user, uint32_t operandIndex) const;
    bool rowOnlyInspects(BuiltinID callee, BuiltinCallMode, BuiltinOperand) const;
    Fate fateOf(const BodyValue& user, uint32_t operandIndex) const;

    NamesOfNumbersInProgramFacts& m_namesOfNumbers;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const RowsOfProgramFacts& m_rows;
    const RefusedCode m_refused;
    const FunctionID m_id;
    const Function& m_function;
    const bool m_isOfProgram;
    const BodyIndex& m_index;
    // ThinSummary::blocksNeverEntered.
    std::span<const BlockID> m_blocksNeverEntered;
    Vector<Finding>& m_found;
    // By value, as the operation names it. Empty in a body that names none.
    Vector<BuiltinID> m_builtinOfValue;
    // By value: an edge brings a parameter of a block the built-in, and other edges bring it other values.
    BitVector m_mayBeSomethingElse;
    // Each is noted once a body.
    Vector<PropertyKeyID, 8> m_namesReadOffUnknownValues;
    // By value: the construction of the URL that it holds. Empty in a body that constructs none.
    Vector<ValueID> m_originOfURLOfValue;
};

void BodyLookOfProgramFacts::noteFate(ValueID value, Fate fate, const auto& place)
{
    if (BuiltinID builtin = builtinAt(value)) {
        switch (fate) {
        case Fate::IsInspected:
            noteUse(builtin, BuiltinUse::Tested, place);
            break;
        case Fate::IsCalled:
            noteUse(builtin, BuiltinUse::Called, place);
            break;
        case Fate::IsConstructed:
            noteUse(builtin, BuiltinUse::Constructed, place);
            break;
        case Fate::IsAskedForAnyMember:
            note(Finding::Kind::ReadOfAnyMember, static_cast<uint8_t>(OptionSet<BuiltinUse>(BuiltinUse::Escaped).toRaw()), builtin.index(), witnessAt(place));
            break;
        case Fate::IsLetGo:
            noteUse(builtin, BuiltinUse::Escaped, place);
            break;
        }
    }
    if (fate != Fate::IsLetGo && fate != Fate::IsAskedForAnyMember)
        return;
    if (ValueID origin = originOfURL(value))
        note(Finding::Kind::URLThatLeavesBody, 0, Wire::none, witnessAt(m_index.value(origin)));
}

void BodyLookOfProgramFacts::handOn(ValueID from, ValueID to)
{
    if (BuiltinID builtin = builtinAt(from)) {
        m_builtinOfValue[to.index()] = builtin;
        if (m_mayBeSomethingElse.get(from.index()))
            m_mayBeSomethingElse.set(to.index());
    }
    if (ValueID origin = originOfURL(from))
        m_originOfURLOfValue[to.index()] = origin;
}

// `let holder = globalThis; for (const name of names) holder = holder[name];`: a parameter of a block further on in the stream may hold the built-in that an edge brings it.
bool BodyLookOfProgramFacts::passOn(ValueID argument, ValueID parameter, const BodyBlock& from)
{
    BuiltinID builtin = builtinAt(argument);
    if (!builtin || originOfURL(argument) || parameter.index() < from.endOfValues().index())
        return false;
    BuiltinID held = builtinAt(parameter);
    if (held && held != builtin)
        return false;
    m_builtinOfValue[parameter.index()] = builtin;
    m_mayBeSomethingElse.set(parameter.index());
    return true;
}

// `import("bun:jsc")`: the namespace object of a module of native code, or a promise of it, is a value that nobody follows.
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
    // `require.resolve(name)`: the front end writes the operation in the place of the call, and the host carries it out with the code behind the function.
    if (opcode == Opcode::ResolveDynamic) {
        // The two other forms are native code.
        if (auto resolve = value.as<Opcode::ResolveDynamic>(); !resolve.isURL() && !resolve.isForRequire())
            noteUse(Builtins::ModuleRequireResolve, BuiltinUse::Called, value);
        return;
    }
    if (loadsByNameInProgramFacts.contains(opcode)) {
        // What the library loads under a name that it makes is a module of the library.
        if (m_function.role == FunctionRole::Source)
            note(Finding::Kind::UnseenCode, static_cast<uint8_t>(m_refused.codeOutsideImage ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName), Wire::none, witnessAt(value));
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
    if (opcode == Opcode::RequireModule)
        nameBuiltin(value, m_program.at(module).runtimeModule ? m_builtins.namespaceOf(m_program.at(module).runtimeModule) : BuiltinID());
    else if (opcode != Opcode::EvaluateModule)
        noteObjectOfModule(m_program.at(module).runtimeModule, value);
}

BuiltinID BodyLookOfProgramFacts::memberFoundOn(BuiltinID holder, PropertyKeyID key) const
{
    BuiltinID canonical = objectAnsweredByBindingInProgramFacts(m_builtins, m_builtins.canonicalID(holder));
    if (BuiltinID own = findOwnMember(m_program, canonical, key))
        return own;
    BuiltinMemberName name = builtinMemberNameOf(m_program, key);
    if (!name)
        return { };
    BuiltinMemberLookup lookup = m_builtins.findMember(canonical, name.name);
    return lookup.outcome == BuiltinMemberLookup::Outcome::Found ? lookup.member : BuiltinID();
}

// Each object that the lookup passes by lists all that it has.
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

void BodyLookOfProgramFacts::accessMember(const BodyValue& value, BuiltinID holder)
{
    Opcode opcode = value.opcode();
    PropertyKeyID key;
    BuiltinUse use = BuiltinUse::Written;
    if (opcode == Opcode::PropGet) {
        key = value.as<Opcode::PropGet>().key();
        use = BuiltinUse::Read;
    } else if (opcode == Opcode::PropHas) {
        key = value.as<Opcode::PropHas>().key();
        use = BuiltinUse::Tested;
    } else if (opcode == Opcode::PropSet)
        key = value.as<Opcode::PropSet>().key();
    else if (opcode == Opcode::PropDelete)
        key = value.as<Opcode::PropDelete>().key();
    else
        key = value.as<Opcode::PropDefine>().key();

    if (use == BuiltinUse::Written)
        noteWrite(holder, key, value);
    BuiltinID member = memberFoundOn(holder, key);
    // An accessor that the program has laid above the holder is called with the holder for `this`.
    bool mayRunAccessorAbove = use == BuiltinUse::Written || (use == BuiltinUse::Read && (!member || (m_builtins.at(m_builtins.canonicalID(member)).kind == BuiltinKind::Accessor && m_builtins.canonicalID(m_builtins.at(member).holder) != m_builtins.canonicalID(holder))));
    if (mayRunAccessorAbove)
        note(Finding::Kind::KeysOfBuiltinListed, 0, holder.index(), witnessAt(value));
    if (!member) {
        // Each module has its own `require` and its own module object, and the table speaks of neither. `detail`: the builder asks whether the program lays the name on a built-in.
        if (opcode == Opcode::PropGet && key && m_builtins.at(m_builtins.rootOf(m_builtins.canonicalID(holder))).scope != BuiltinScope::Realm)
            m_found.append(Finding { Finding::Kind::ReadThatNoRowAnswers, 1, holder.index(), key.index(), { }, witnessAt(value) });
        if (opcode == Opcode::PropGet && !tableRulesOutMemberFoundFrom(holder, key))
            note(Finding::Kind::ReadThatNoRowAnswers, 0, holder.index(), witnessAt(value));
        return;
    }
    noteUse(member, use, value);
    if (opcode != Opcode::PropGet || !standsForObjectInProgramFacts(m_builtins, m_builtins.canonicalID(member)))
        return;
    m_builtinOfValue[value.id().index()] = member;
    if (m_mayBeSomethingElse.get(value.operand(0).index()))
        m_mayBeSomethingElse.set(value.id().index());
}

// Native code looks at the operand and hands it to nobody, in every overload that the call may enter.
bool BodyLookOfProgramFacts::rowOnlyInspects(BuiltinID callee, BuiltinCallMode mode, BuiltinOperand position) const
{
    bool hasOverload = false;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(callee))) {
        if (overload.mode != mode)
            continue;
        hasOverload = true;
        if (overload.isUndeclared())
            return false;
        const BuiltinParameter* parameter = position.isReceiver() ? overload.receiver.get() : m_builtins.parameterFor(overload, position);
        // A row without a receiver does not look at `this`. Of an argument that no parameter covers nothing is said.
        if (!parameter) {
            if (position.isReceiver())
                continue;
            return false;
        }
        if (!(parameter->uses - OperandUses { OperandUse::Inspects }).isEmpty())
            return false;
    }
    return hasOverload;
}

// Of an operand that fateOf() finds inspected.
bool BodyLookOfProgramFacts::mayGoThroughOwnKeysOf(const BodyValue& user, uint32_t operandIndex) const
{
    // Every other protocol goes through the keys, or calls what it finds on the operand or above it with the operand for `this`: a function of the program that was laid there has it then.
    static constexpr Protocols protocolsThatShowNothing { Protocol::HasInstance, Protocol::PrototypeChain };
    Opcode opcode = user.opcode();
    if (!callsByValueInProgramFacts.contains(opcode))
        return !(operandInfoOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).protocols - protocolsThatShowNothing).isEmpty();
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
        return parameter && !(parameter->protocols - protocolsThatShowNothing).isEmpty();
    });
}

BodyLookOfProgramFacts::Fate BodyLookOfProgramFacts::fateOf(const BodyValue& user, uint32_t operandIndex) const
{
    Opcode opcode = user.opcode();
    if (!callsByValueInProgramFacts.contains(opcode)) {
        bool isOnlyInspected = (user.operandUses(operandIndex) - OperandUses { OperandUse::Inspects }).isEmpty();
        // An access under a key that is computed finds any member.
        if (accessClassOf(opcode) == AccessClass::ByName)
            return isOnlyInspected && receiverOf(opcode) == operandIndex ? Fate::IsAskedForAnyMember : Fate::IsLetGo;
        return isOnlyInspected ? Fate::IsInspected : Fate::IsLetGo;
    }

    bool isConstruction = constructionsInProgramFacts.contains(opcode);
    if (!operandIndex)
        return isConstruction ? Fate::IsConstructed : Fate::IsCalled;
    if (isConstruction && operandIndex == 1 && user.operand(1) == user.operand(0))
        return Fate::IsInspected;
    BuiltinID callee = builtinSurelyAt(user.operand(0));
    if (!callee)
        return Fate::IsLetGo;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    BuiltinCallMode mode = isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    if (operandIndex == 1 && !isConstruction)
        return rowOnlyInspects(callee, mode, BuiltinOperand::receiver()) ? Fate::IsInspected : Fate::IsLetGo;
    if (operandIndex < numberOfNamedOperands || !callsThatSpellOutArgumentsInProgramFacts.contains(opcode))
        return Fate::IsLetGo;
    uint32_t argument = operandIndex - numberOfNamedOperands;
    if (argument > BuiltinOperand::maxArgumentIndex)
        return Fate::IsLetGo;
    return rowOnlyInspects(callee, mode, BuiltinOperand::argument(argument)) ? Fate::IsInspected : Fate::IsLetGo;
}

void BodyLookOfProgramFacts::noteLocalesPassed(const BodyValue& call, BuiltinID row, BuiltinCallMode mode, bool spellsOutArguments)
{
    auto noteLocale = [&](LocaleUseKind kind, StringID tag = { }) {
        note(Finding::Kind::LocaleUse, static_cast<uint8_t>(kind), row.index(), witnessAt(call), tag);
    };
    std::span<const ValueID> arguments = call.variadicOperands();
    for (const LocaleOperandInProgramFacts& localeOperand : std::ranges::equal_range(m_rows.localeOperands, row, { }, &LocaleOperandInProgramFacts::row)) {
        if (localeOperand.mode != mode)
            continue;
        if (!spellsOutArguments || localeOperand.operand.kind() != BuiltinOperand::Kind::Argument) {
            noteLocale(LocaleUseKind::Unknown);
            continue;
        }
        unsigned argument = localeOperand.operand.argumentIndex();
        if (argument >= arguments.size()) {
            noteLocale(LocaleUseKind::HostDefault);
            continue;
        }
        BodyValue passed = valueBehindCasts(arguments[argument]);
        if (passed.opcode() == Opcode::Undefined) {
            noteLocale(LocaleUseKind::HostDefault);
            continue;
        }
        if (passed.opcode() != Opcode::String) {
            noteLocale(LocaleUseKind::Unknown);
            continue;
        }
        StringID text = passed.as<Opcode::String>().string();
        Vector<char16_t> buffer;
        if (isStructurallyValidLanguageTag(m_program.strings.viewOf(text, buffer)))
            noteLocale(LocaleUseKind::Constant, text);
        else
            noteLocale(LocaleUseKind::Unknown);
    }
}

void BodyLookOfProgramFacts::noteStartOfWorker(const BodyValue& call, bool spellsOutArguments)
{
    if (!spellsOutArguments)
        return note(Finding::Kind::WorkerStartWithoutName, 0, Wire::none, witnessAt(call));

    // Where the table marks no argument, any of them may be the file, and any may be the options.
    bool mayHaveOptions = !m_rows.fileArguments;
    bool namesFile = false;
    bool mayGetUnknownFile = false;
    Vector<StringID, 1> paths;
    std::span<const ValueID> arguments = call.variadicOperands();
    for (size_t argument = 0; argument < arguments.size(); ++argument) {
        BodyValue passed = valueBehindCasts(arguments[argument]);
        Opcode opcode = passed.opcode();
        bool isNothing = opcode == Opcode::Undefined || opcode == Opcode::Null;
        if (m_rows.fileArguments && !(m_rows.fileArguments & RowsOfProgramFacts::bitOfArgument(argument))) {
            mayHaveOptions |= !isNothing;
            continue;
        }
        if (opcode == Opcode::String)
            paths.append(passed.as<Opcode::String>().string());
        else if (ValueID origin = originOfURL(arguments[argument])) {
            namesFile = true;
            note(Finding::Kind::URLThatStartsWorker, 0, Wire::none, witnessAt(m_index.value(origin)));
        } else
            mayGetUnknownFile |= !isNothing;
    }
    for (StringID path : paths)
        note(Finding::Kind::WorkerEntry, mayHaveOptions, path.index(), witnessAt(call));
    if (mayGetUnknownFile || (paths.isEmpty() && !namesFile))
        note(Finding::Kind::WorkerStartWithoutName, 0, Wire::none, witnessAt(call));
}

// False: the value is no literal of this block, or something besides definitions under constant keys has touched it.
bool BodyLookOfProgramFacts::listKeysOfLiteral(ValueID value, Vector<PropertyKeyID, 4>& keys) const
{
    ValueID finished = m_index.valueBehindCasts(value);
    if (!finished || m_index.isParameter(finished) || m_index.opcodeOf(finished) != Opcode::FinishObject)
        return false;
    ValueID literal = m_index.valueBehindCasts(m_index.value(finished).operand(0));
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
            touchesLiteral |= operand && m_index.valueBehindCasts(operand) == literal;
        });
        if (!touchesLiteral)
            continue;
        if (part.opcode() == Opcode::PropDefine && m_index.valueBehindCasts(part.operand(0)) == literal)
            keys.append(part.as<Opcode::PropDefine>().key());
        else
            listsEveryKey = false;
    }
    return listsEveryKey;
}

// `Object.defineProperty(exports, "name", d)` spells the name that it writes. `Reflect.set(Date, key, f)` writes on Date under a key that nobody can read.
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
            if (!writesUnderKeyOfCallInProgramFacts(action))
                continue;
            BuiltinID target = builtinAt(passedAs(action.object));
            // `Object.assign(value.constructor, { parse })` spells the names that its literal lists.
            if (action.key.kind == BuiltinKeyKind::None && action.value.operand && !action.value.operand->isReceiver() && spellsOutArguments) {
                Vector<PropertyKeyID, 4> keys;
                bool listsEveryKey = true;
                size_t end = action.value.operand->kind() == BuiltinOperand::Kind::Argument ? action.value.operand->argumentIndex() + 1 : arguments.size();
                for (size_t argument = action.value.operand->argumentIndex(); argument < std::min(end, arguments.size()); ++argument)
                    listsEveryKey &= listKeysOfLiteral(arguments[argument], keys);
                for (PropertyKeyID listed : keys) {
                    if (target)
                        noteWrite(target, listed, call);
                    else
                        note(Finding::Kind::NameStoredByRow, 0, listed.index(), witnessAt(call));
                }
                if (listsEveryKey)
                    continue;
            }
            PropertyKeyID key;
            ValueID passedKey = action.key.kind == BuiltinKeyKind::Operand ? passedAs(action.key.operand) : ValueID();
            if (passedKey && valueBehindCasts(passedKey).opcode() == Opcode::String) {
                // The reader gives every name of the table its key, so no member lies under a text without one.
                std::optional<PropertyKeyID> spelled = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = valueBehindCasts(passedKey).as<Opcode::String>().string() });
                if (!spelled)
                    continue;
                key = *spelled;
            }
            if (target)
                m_found.append(Finding { Finding::Kind::WriteOnBuiltin, static_cast<uint8_t>(key ? 1 : 0), target.index(), key ? key.index() : Wire::none, { }, witnessAt(call) });
            else if (key)
                note(Finding::Kind::NameStoredByRow, 0, key.index(), witnessAt(call));
            else
                note(Finding::Kind::NameStoredByRow, 0, Wire::none, witnessAt(call));
        }
    }
}

// A literal of this block that lists the option is passed. What else arrives there Link follows: CallInLinkCalls::noteOptionThatRunsCodeAmong().
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
    // `{ ...base, app }`, `options.app = app`
    ValueID passed = m_index.valueBehindCasts(arguments[option.operand.argumentIndex()]);
    if (!passed || m_index.isParameter(passed))
        return false;
    for (uint32_t number = passed.index() + 1; number < call.id().index(); ++number) {
        if (m_index.isParameter(ValueID(number)))
            continue;
        BodyValue part = m_index.value(ValueID(number));
        bool storesOption = (part.opcode() == Opcode::PropDefine && part.as<Opcode::PropDefine>().key() == key) || (part.opcode() == Opcode::PropSet && part.as<Opcode::PropSet>().key() == key);
        if (storesOption && m_index.valueBehindCasts(part.operand(0)) == passed)
            return true;
    }
    return false;
}

void BodyLookOfProgramFacts::lookAtCall(const BodyValue& call)
{
    BuiltinID callee = builtinAt(call.operand(0));
    if (!callee)
        return;
    BuiltinID row = m_builtins.canonicalID(callee);
    Opcode opcode = call.opcode();
    bool isConstruction = constructionsInProgramFacts.contains(opcode);
    bool spellsOutArguments = callsThatSpellOutArgumentsInProgramFacts.contains(opcode);

    if (m_rows.takeLocales.quickGet(row.index()))
        noteLocalesPassed(call, row, isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, spellsOutArguments);
    if (m_rows.startWorkers.quickGet(row.index()))
        noteStartOfWorker(call, spellsOutArguments);
    if (m_rows.writeUnderKeysOfCalls.quickGet(row.index()))
        noteWritesOfRow(call, row, isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, spellsOutArguments);
    if (const BuiltinCondition* option = optionThatRunsCodeInProgramFacts(m_builtins, row)) {
        if (mayPass(call, *option, spellsOutArguments))
            note(Finding::Kind::OptionThatRunsCode, 0, row.index(), witnessAt(call));
    }
    if (isConstruction && row == m_rows.constructorOfURLs) {
        if (m_originOfURLOfValue.isEmpty())
            m_originOfURLOfValue.fill(ValueID(), m_index.numberOfValues());
        m_originOfURLOfValue[call.id().index()] = call.id();
    }
}

// What checked code writes of the value: of a parameter of the function, or where an operation states it on the way.
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

void BodyLookOfProgramFacts::forEachDeclarerOfFieldOf(ValueID receiver, PropertyKeyID key, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    TypeID written = writtenTypeOf(receiver);
    if (!written)
        return;
    for (LayoutID layout : membersOf(TypeReader(m_program), m_program.types.at(written))) {
        if (std::optional<FieldReference> field = findField(m_program, layout, key))
            functor(declarationOf(m_program, *field).layout);
    }
}

// `text.substring(node.pos, node.end)`, `links[symbol.id]`, `list[o.at + 1]`, `(o.low + o.high) >> 1`: behind casts and ONE sum.
void BodyLookOfProgramFacts::noteReadUsedAsInteger(ValueID used)
{
    if (!used)
        return;
    auto noteRead = [&](const BodyValue& definition) {
        if (definition.opcode() != Opcode::PropGet)
            return false;
        if (PropertyKeyID key = definition.as<Opcode::PropGet>().key()) {
            forEachDeclarerOfFieldOf(definition.as<Opcode::PropGet>().objectOperand(), key, [&](LayoutID declarer) {
                m_namesOfNumbers.noteReadAsInteger(declarer, key);
            });
        }
        return true;
    };
    ValueID behindCasts = m_index.valueBehindCasts(used);
    if (!behindCasts || m_index.isParameter(behindCasts))
        return;
    BodyValue definition = m_index.value(behindCasts);
    if (noteRead(definition) || !allSums.contains(definition.opcode()))
        return;
    definition.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID part = operand ? m_index.valueBehindCasts(operand) : ValueID();
        if (part && !m_index.isParameter(part))
            noteRead(m_index.value(part));
    });
}

// `const result = new Type(checker, flags); result.id = typeCount; return result;`: nothing is written of what `new` gives, and the result of the function is written.
TypeID BodyLookOfProgramFacts::writtenTypeWhereValueIsHandedOn(ValueID value) const
{
    value = m_index.valueBehindCasts(value);
    if (!value)
        return { };
    for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
        for (BodyValue user : m_index.block(BlockID(number)).values()) {
            if (user.opcode() == Opcode::Convert && user.as<Opcode::Convert>().declared().isGuarded() && m_index.valueBehindCasts(user.operand(0)) == value)
                return user.as<Opcode::Convert>().declared().written();
        }
    }
    return { };
}

// `symbol.id = nextSymbolId; nextSymbolId++`: whoever reads the field may hand it on before anybody uses it as an integer: `links[getSymbolId(symbol)]`. What Link proves of the variable says what the field gets.
void BodyLookOfProgramFacts::noteVariableStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID key, ValueID stored)
{
    while (stored && !m_index.isParameter(stored) && m_index.opcodeOf(stored) == Opcode::CheckInitialized)
        stored = m_index.valueBehindCasts(m_index.value(stored).operand(0));
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
    // Most variables that are stored into fields hold functions and objects, or nothing is written of them. `absent`: a `let` in front of its declaration.
    if (!declared || !declared->isGuarded() || !declared->written())
        return;
    Type writtenOfVariable = m_program.types.at(declared->written());
    if (!(writtenOfVariable.primitives & Primitive::Number) || !writtenOfVariable.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
        return;
    // A name that stores append is listed by no layout: there the layout stands for itself.
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

// `this.r * this.r`, `a.x - b.x`, `p.y < limit`, `Math.sqrt(v.x)`: the read itself, behind casts.
void BodyLookOfProgramFacts::noteReadUsedForArithmetic(ValueID used)
{
    ValueID behindCasts = used ? m_index.valueBehindCasts(used) : ValueID();
    if (!behindCasts || m_index.isParameter(behindCasts) || m_index.opcodeOf(behindCasts) != Opcode::PropGet)
        return;
    auto read = m_index.value(behindCasts).as<Opcode::PropGet>();
    if (PropertyKeyID key = read.key()) {
        forEachDeclarerOfFieldOf(read.objectOperand(), key, [&](LayoutID declarer) {
            m_namesOfNumbers.noteReadForArithmetic(declarer, key);
        });
    }
}

// A written fraction, a written number that 32 bits do not hold, a quotient, a power.
void BodyLookOfProgramFacts::noteWhatIsStoredUnder(ValueID receiver, LayoutID allocated, PropertyKeyID key, ValueID stored)
{
    ValueID behindCasts = key && stored ? m_index.valueBehindCasts(stored) : ValueID();
    if (!behindCasts || m_index.isParameter(behindCasts))
        return;
    BodyValue definition = m_index.value(behindCasts);
    Opcode opcode = definition.opcode();
    if (readsOfVariablesInProgramFacts.contains(opcode)) {
        noteVariableStoredUnder(receiver, allocated, key, behindCasts);
        return;
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
        value.forEachOperand([&](ValueID operand, uint32_t) {
            noteReadUsedAsInteger(operand);
        });
        return;
    }
    if (operatorsOnNumbersAloneInProgramFacts.contains(opcode)) {
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

    // `Math.sqrt(v.x)`: the front end names the member itself.
    if (ValueID callee = m_index.valueBehindCasts(value.as<Opcode::Call>().calleeOperand()); callee && !m_index.isParameter(callee) && m_index.opcodeOf(callee) == Opcode::Builtin) {
        BuiltinID member = m_builtins.canonicalID(m_index.value(callee).as<Opcode::Builtin>().builtin());
        if (BuiltinID holder = m_builtins.at(member).holder; holder && m_builtins.canonicalID(holder) == m_builtins.canonicalID(Builtins::Math)) {
            for (ValueID argument : value.variadicOperands())
                noteReadUsedForArithmetic(argument);
        }
        return;
    }

    // A row of a text or a list that converts its argument to an integer: the receiver is anybody's guess, and this is one.
    PropertyKeyID calleeKey = value.as<Opcode::Call>().calleeKey();
    std::span<const ValueID> arguments = value.variadicOperands();
    if (!calleeKey || arguments.empty() || !m_program.builtinNameOf(calleeKey))
        return;
    bool passesRead = std::ranges::any_of(arguments, [&](ValueID argument) {
        ValueID behindCasts = argument ? m_index.valueBehindCasts(argument) : ValueID();
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
void BodyLookOfProgramFacts::look(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (m_isOfProgram && operationsThatTellOfNumbersInProgramFacts.contains(opcode))
        noteWhatIsToldOfNumbers(value);
    if (loadsAndEvaluationsInProgramFacts.contains(opcode))
        lookAtLoad(value);
    // `value.prototype`
    if (opcode == Opcode::PropGet && m_isOfProgram && (m_builtinOfValue.isEmpty() || !builtinAt(value.operand(0)))) {
        PropertyKeyID key = value.as<Opcode::PropGet>().key();
        if (key && m_program.builtinNameOf(key) && !m_namesReadOffUnknownValues.contains(key)) {
            m_namesReadOffUnknownValues.append(key);
            note(Finding::Kind::NameReadOffUnknownValue, 0, key.index(), witnessAt(value));
        }
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
    if (inputOperationsThatHandOn.contains(opcode)) {
        handOn(value.operand(0), value.id());
        operandAccountedFor = 0;
    } else if (accessesUnderConstantKeysInProgramFacts.contains(opcode)) {
        if (BuiltinID holder = builtinAt(value.operand(0)))
            accessMember(value, holder);
        operandAccountedFor = 0;
    } else if (callsByValueInProgramFacts.contains(opcode))
        lookAtCall(value);

    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex == operandAccountedFor || !isFollowed(operand))
            return;
        Fate fate = fateOf(value, operandIndex);
        noteFate(operand, fate, value);
        if (fate == Fate::IsInspected && builtinAt(operand) && mayGoThroughOwnKeysOf(value, operandIndex))
            note(Finding::Kind::KeysOfBuiltinListed, 0, builtinAt(operand).index(), witnessAt(value));
        // `Date[name] = f`
        if (fate == Fate::IsLetGo && builtinAt(operand) && accessClassOf(opcode) == AccessClass::ByName && receiverOf(opcode) == operandIndex)
            noteWrite(builtinAt(operand), { }, value);
    });
}

void BodyLookOfProgramFacts::lookAtEnd(const BodyBlock& block)
{
    BodyTerminator terminator = block.terminator();
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!isFollowed(operand))
            return;
        bool isInspected = (terminator.operandUses(operandIndex) - OperandUses { OperandUse::Inspects }).isEmpty();
        noteFate(operand, isInspected ? Fate::IsInspected : Fate::IsLetGo, terminator);
    });
    block.forEachOutgoingEdge([&](BodyTarget target, bool isHandlerEdge) {
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t position = 0; position < arguments.size(); ++position) {
            if (isFollowed(arguments[position]) && (isHandlerEdge || !passOn(arguments[position], m_index.block(target.block()).parameter(position), block)))
                noteFate(arguments[position], Fate::IsLetGo, terminator);
        }
    });
    // `if (typeof Worker === "function") new Worker(path)`: what a test hands to a block that it alone enters, further on in the stream, is the value that was tested.
    m_index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
        if (!isFollowed(result.subject))
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
    // `function below(value) { return value.prototype; }`
    bool mayReadNameOffUnknownValue = m_isOfProgram && m_index.holdsAnyOf(OpcodeSet { Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::NewObject });
    if (!namesBuiltins && !mayReadNameOffUnknownValue && !m_index.holdsAnyOf(loadsAndEvaluationsInProgramFacts))
        return;
    if (namesBuiltins)
        m_builtinOfValue.fill(BuiltinID(), m_index.numberOfValues());
    for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
        if (!m_blocksNeverEntered.empty() && m_blocksNeverEntered.front().index() == number) {
            m_blocksNeverEntered = m_blocksNeverEntered.subspan(1);
            continue;
        }
        BodyBlock block = m_index.block(BlockID(number));
        for (BodyValue value : block.values())
            look(value);
        if (namesBuiltins)
            lookAtEnd(block);
    }
}

// ---- The looks at all bodies

class LooksAtBodies {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LooksAtBodies);
public:
    explicit LooksAtBodies(const BodyIndexTable& bodies)
        : bodies(bodies)
    {
    }

    std::span<const FindingOfProgramFacts> of(FunctionID id) const LIFETIME_BOUND { return byFunction.of(id.index()); }

    const BodyIndexTable& bodies;
    FactRows<FindingOfProgramFacts> byFunction;
    FieldsOfNumbers fieldsReadAsIntegers;
    FieldsOfNumbers fieldsReadForArithmetic;
    FieldsOfNumbers fieldsStoredWhatIsNoInt32;
    Vector<VariableStoredInFieldInProgramFacts> variablesStoredInFields;
    BitVector keysStoredWhatIsNoInt32;
};

const FieldsOfNumbers& fieldsReadAsIntegersIn(const LooksAtBodies& looks)
{
    return looks.fieldsReadAsIntegers;
}

const FieldsOfNumbers& fieldsReadForArithmeticIn(const LooksAtBodies& looks)
{
    return looks.fieldsReadForArithmetic;
}

const FieldsOfNumbers& fieldsStoredWhatIsNoInt32In(const LooksAtBodies& looks)
{
    return looks.fieldsStoredWhatIsNoInt32;
}

FieldsOfNumbers fieldsStoredCountsIn(const LooksAtBodies& looks, const Program& program, const LinkTables& linked)
{
    FieldsOfNumbers counts;
    for (const VariableStoredInFieldInProgramFacts& store : looks.variablesStoredInFields) {
        const LinkedCell* cell = store.ofModule ? &linked.variables[store.ofModule.index()] : linked.findCellOfField(store.ofEnvironment);
        if (!cell || !cell->proven)
            continue;
        // `absent`: a `let` in front of its declaration.
        Type proven = program.types.at(cell->proven);
        if (proven.primitives & Primitive::NumberWithoutFraction && proven.hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::UndefinedOrNull | Primitive::Absent))
            counts.add(store.declarer, store.key);
    }
    return counts;
}

const BitVector& keysStoredWhatIsNoInt32In(const LooksAtBodies& looks)
{
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

// Code is refused if either the program or the options refuse it, so the run time admits no more than is counted on here.
static RefusedCode codeRefusedInProgramFacts(const Program& program, BuildContext& build)
{
    const AOT::CompileOptions& options = build.options();
    RefusedCode refused = refusedCodeOf(program);
    refused.evaluatedText |= options.excludesEvaluatedText();
    refused.hostEvaluatedText |= options.excludesHostEvaluatedText();
    refused.codeOutsideImage |= !options.admitsUnseenCode() && options.excludesCodeOutsideImage();
    return refused;
}

OwnedLooksAtBodies lookAtBodies(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, BuildContext& build)
{
    RowsOfProgramFacts rows(program.builtins());
    RefusedCode refused = codeRefusedInProgramFacts(program, build);
    auto numberOfFunctions = static_cast<unsigned>(program.functions.size());

    OwnedLooksAtBodies looks(new LooksAtBodies(bodies));
    FactRows<FindingOfProgramFacts>& byFunction = looks->byFunction;
    NamesOfNumbersInProgramFacts namesOfNumbers(program.types.numberOfPropertyKeys());
    byFunction.rows = collectInParallel<FindingOfProgramFacts>(build.parallelLoops(), numberOfFunctions, [&](unsigned number, Vector<FindingOfProgramFacts>& found) {
        FunctionID id(number);
        if (const BodyIndex* index = bodies.find(id))
            BodyLookOfProgramFacts(program, rows, refused, id, *index, summaries.at(id).blocksNeverEntered.span(), found, namesOfNumbers).run();
    });
    looks->fieldsReadAsIntegers = namesOfNumbers.takeFieldsReadAsIntegers();
    looks->fieldsReadForArithmetic = namesOfNumbers.takeFieldsReadForArithmetic();
    looks->fieldsStoredWhatIsNoInt32 = namesOfNumbers.takeFieldsStoredWhatIsNoInt32();
    looks->keysStoredWhatIsNoInt32 = namesOfNumbers.keysStoredWhatIsNoInt32();
    looks->variablesStoredInFields = namesOfNumbers.takeVariablesStoredInFields();
    // Each finding has its place in the body that was looked at.
    byFunction.firstRowOfOwner.fill(0, numberOfFunctions + 1);
    for (const FindingOfProgramFacts& finding : byFunction.rows)
        ++byFunction.firstRowOfOwner[finding.witness.function + 1];
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        byFunction.firstRowOfOwner[index + 1] += byFunction.firstRowOfOwner[index];
    return looks;
}

// ---- Features

// A member of the host without a guard of its own has that of the nearest holder.
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

// From the rows of builtinUses() to the features of whatever may run behind them: the class of a member of a prototype object, and the classes of what a row makes.
class FeaturesOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(FeaturesOfProgramFacts);
public:
    FeaturesOfProgramFacts(const InputsOfProgramFacts&, RefusedCode);

    Vector<FeatureUseRecord> run(std::span<const BuiltinUseRecord>);

private:
    // A test runs no code of the built-in. An escape says who holds it, not how they came by it.
    static constexpr OptionSet<BuiltinUse> usesThatNameNothing { BuiltinUse::Tested, BuiltinUse::Escaped };

    void collectUses(std::span<const BuiltinUseRecord>);
    void noteNamesOfHolders(BuiltinID);
    void noteCodeMayRun(BuiltinID, const Witness&);
    void noteInstancesAreMade(BuiltinClassID, const Witness&);
    void noteTypeIsMade(BuiltinTypeID, const Witness&);
    void noteTypesThatOverloadMakes(const BuiltinOverload&, const Witness&);
    void noteClassesThatFunctionsMake();
    void visitRow(BuiltinID);
    void appendRecordsOf(BuiltinID);
    void sortRecordsAndMarkUnnamed();
    BuiltinID entryWithFeaturesOf(BuiltinClassID) const;
    BuiltinID outermostHolderWithFeature(BuiltinID, uint32_t feature) const;

    const Program& m_program;
    const LinkTables& m_linked;
    const BuiltinTable& m_builtins;
    const RefusedCode m_refused;
    // All by canonical built-in.
    Vector<BuiltinClassID> m_classOfPrototype;
    Vector<Witness> m_witnesses;
    BitVector m_isNamed;
    BitVector m_codeMayRun;
    Vector<BuiltinID> m_rowsToVisit;
    BitVector m_madeClasses;
    BitVector m_madeTypes;
    Vector<FeatureUseRecord> m_records;
};

FeaturesOfProgramFacts::FeaturesOfProgramFacts(const InputsOfProgramFacts& inputs, RefusedCode refused)
    : m_program(inputs.program)
    , m_linked(inputs.linked)
    , m_builtins(inputs.program.builtins())
    , m_refused(refused)
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

void FeaturesOfProgramFacts::collectUses(std::span<const BuiltinUseRecord> records)
{
    for (const BuiltinUseRecord& record : records) {
        BuiltinID builtin = m_builtins.canonicalID(BuiltinID(record.builtin));
        auto uses = OptionSet<BuiltinUse>::fromRaw(static_cast<OptionSet<BuiltinUse>::StorageType>(record.uses));
        keepLowerWitnessInProgramFacts(m_witnesses[builtin.index()], record.witness);
        if ((uses - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        noteCodeMayRun(builtin, record.witness);
        if ((uses - usesThatNameNothing).isEmpty())
            continue;
        m_isNamed.quickSet(builtin.index());
        // `Intl.DateTimeFormat` may be one operation, which reads the member and says nothing of `Intl`. A read off a value that nobody knows has no place and names no holder: `x.prototype` would name every class.
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

// What an operation makes by calling the run time is listed with the entries that the image uses: writeFeatureSection() of driver/AOTResultSections.cpp.
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

void FeaturesOfProgramFacts::visitRow(BuiltinID builtin)
{
    const Builtin& entry = m_builtins.at(builtin);
    Witness witness = m_witnesses[builtin.index()];
    // The table lays the features of a class on its prototype object or its constructor, and a member of the prototype object need not repeat them.
    if ((entry.kind == BuiltinKind::Method || entry.kind == BuiltinKind::Accessor) && entry.holder) {
        if (BuiltinClassID owner = m_classOfPrototype[m_builtins.canonicalID(entry.holder).index()])
            noteCodeMayRun(entryWithFeaturesOf(owner), witness);
    }
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(builtin))
        noteTypesThatOverloadMakes(overload, witness);
}

void FeaturesOfProgramFacts::appendRecordsOf(BuiltinID builtin)
{
    // Its call throws before any code of its features runs.
    if (refusesTextAsUnseenCode(m_builtins, builtin, m_refused))
        return;
    for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(builtin)) {
        // Nobody has looked at the operands, so a condition on one may hold.
        if (line.condition && line.condition->buildAdmitsRuntimeCode)
            continue;
        m_records.append(FeatureUseRecord { static_cast<uint32_t>(line.feature), builtin.index(), m_witnesses[builtin.index()], FeatureUseKind::Named });
    }
}

BuiltinID FeaturesOfProgramFacts::outermostHolderWithFeature(BuiltinID builtin, uint32_t feature) const
{
    BuiltinID outermost = builtin;
    for (BuiltinID holder = m_builtins.at(builtin).holder; holder; holder = m_builtins.at(holder).holder) {
        holder = m_builtins.canonicalID(holder);
        bool hasFeature = std::ranges::any_of(m_builtins.featureUsesOf(holder), [&](const BuiltinFeatureUse& line) {
            return static_cast<uint32_t>(line.feature) == feature;
        });
        if (hasFeature)
            outermost = holder;
    }
    return outermost;
}

void FeaturesOfProgramFacts::sortRecordsAndMarkUnnamed()
{
    auto orderOf = [](const FeatureUseRecord& record) {
        return std::tuple(record.feature, record.builtin);
    };
    AOT::sort(m_records, { }, orderOf);
    m_records.shrink(std::ranges::unique(m_records, { }, orderOf).begin() - m_records.begin());
    // An unnamed use does not keep a feature that stays only when named, so whatever is in doubt is named. Those features are all the engine's.
    for (FeatureUseRecord& record : m_records) {
        if (record.builtin == Wire::none)
            continue;
        BuiltinID builtin(record.builtin);
        if (m_builtins.at(builtin).origin == BuiltinOrigin::Host)
            continue;
        if (!m_isNamed.quickGet(outermostHolderWithFeature(builtin, record.feature).index()))
            record.kind = FeatureUseKind::Unnamed;
    }
}

Vector<FeatureUseRecord> FeaturesOfProgramFacts::run(std::span<const BuiltinUseRecord> uses)
{
    // Text that a debugger evaluates names whatever the table holds and hands it whatever it likes. So every line holds, whatever its condition, and every feature is named.
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

    collectUses(uses);
    noteClassesThatFunctionsMake();
    while (!m_rowsToVisit.isEmpty())
        visitRow(m_rowsToVisit.takeLast());
    for (size_t index : m_codeMayRun)
        appendRecordsOf(BuiltinID(static_cast<uint32_t>(index)));
    sortRecordsAndMarkUnnamed();
    return WTF::move(m_records);
}

// ---- Writes on the members that a program may replace

// A MEMBER THAT THE PROGRAM USES IS LOCKED UNLESS A WRITE WAS FOLLOWED TO IT. The locks bind whoever writes, so none has to be withheld for a claim to hold: a lock too many refuses a write of the program's own.
// What nobody has followed to the object is refused when it runs: the writes of text that is evaluated, of code from disk and of native code, and a write through a value that nobody knows under a key that nobody can read.
// It goes by what each reached body shows of itself (take()) and by where Link's flow has brought the built-ins (LookupsPastProgramObjects). Both only grow, and the more there is the more stays open.
// One is asked while Link still goes (BuiltinsInReach::takeForConstant()), and one behind Link: they end alike.
class WritesOnBuiltinsInProgramFacts {
    WTF_MAKE_NONCOPYABLE(WritesOnBuiltinsInProgramFacts);
public:
    WritesOnBuiltinsInProgramFacts(const Program&, RefusedCode, const LookupsPastProgramObjects&);

    void take(const FindingOfProgramFacts&);
    // In front of the questions, behind whatever the lookups have come to hold.
    void settle();

    // False: the program calls, constructs or lets go a built-in of which runsCodeThatNoLockIsLaidBeside() holds.
    bool locksAreLaid() const { return m_locksAreLaid; }
    bool canLockAtStartUp(BuiltinID member) const;
    // None: a lock on the member turns away nothing that the build has seen coming.
    ReasonBuiltinStaysOpen reasonToLeaveOpen(BuiltinID member) const { return reasonToLeaveOpen(member, m_isLostOutOfPlaceThatHolds); }
    // For Link, whose answers may only go from yes to no: what this finds unwritten reasonToLeaveOpen() finds unwritten.
    bool isWrittenAsFarAsLinkGoes(BuiltinID member) const { return reasonToLeaveOpen(member, m_isLost) != ReasonBuiltinStaysOpen::None; }

private:
    ReasonBuiltinStaysOpen reasonToLeaveOpen(BuiltinID member, const BitVector& lost) const;
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

    // By built-in, as the table lists it, and at the entry that stands for the object that lies there: an operation names the object and the member that it writes.
    BitVector m_isWritten;
    // By canonical built-in.
    // An operation of the program's own code lets the object go, or reads it off its holder under a key that nobody knows. Or it lies on such an object under a name that the program reads.
    BitVector m_isCarriedOutByProgram;
    // The same without what the files of the product let go in a test run. It is asked of a member that an executable locks whatever the program does.
    BitVector m_isCarriedOutBesideProduct;
    // An operation has the object in sight and writes on it under a key that nobody can read.
    BitVector m_isWrittenUnderAnyName;
    // By property key.
    BitVector m_namesReadByProgram;
    // By canonical built-in again. As of settle(). LookupsPastProgramObjects::lostBuiltins, and what the reads through such values find on them.
    BitVector m_isLost;
    // The same without the built-ins whose own place has come open. Such a one is lost wherever it is read from then on, `globalThis[name] = 1; new TextEncoder()`, which says nothing of where it goes.
    BitVector m_isLostOutOfPlaceThatHolds;
    // What has reached unseen code, and all that lies on it: whoever holds an object reads its members.
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
    // Whoever has the object reads what lies on it, under the names that the program spells: `for (const constructor of [TextDecoder, URL]) constructor.prototype[name] = f`, `globalThis[holder].prototype[name] = f`.
    // A name that stands in a text alone is no such name: `"TextEncoder.prototype.encode".split(".")` leads a loop to the object under keys that nobody knows, one step from the global object and no further.
    auto isReadByProgram = [&](BuiltinID member) {
        PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
        return key && m_namesReadByProgram.get(key.index());
    };
    closeOverMembers(m_isCarriedOutByProgram, isReadByProgram);
    if (m_source.admitsReplacedBuiltins)
        closeOverMembers(m_isCarriedOutBesideProduct, isReadByProgram);

    // The same of what the flow has lost: `pick(unknown, new WeakRef(o)).constructor.prototype[name] = f`.
    // What a read under a key that nobody knows answers is followed no further.
    mergeInto(m_isLost, m_lookups.lostBuiltins);
    closeOverNamesReadThroughLostBuiltins(m_isLost);

    mergeInto(m_isWithinReachOfUnseen, m_lookups.builtinsAtUnseen);
    // Whoever holds an object that the host has made comes to what stands above it: `hidden.constructor.canParse = f`. The classes of the language are left alone.
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
    // The global object is handed to whatever may answer a name that the table does not list. Holding it gives nothing that a free name does not give, and whoever writes what he finds by a free name is followed from there.
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

// The locks bind whoever writes, so none has to be withheld for the claims to hold. Beside these the compiler has never laid a lock of the program's: what they run is taken for code of the program, which may replace what a program may replace.
// `Bun.plugin()`, `module._compile()`, `getProtectedObjects()`. Not text that is evaluated in the scope of the global object, a load by a name, or machine code. Nor `Bun.build()`: the macros that it runs have a realm of their own.
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
    // What the names find is the affair of the loader. A text that is handed a loader runs whatever the names are.
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
    bool isOfProgram = !isLibraryCode(m_source, m_source.at(FunctionID(finding.witness.function)));

    switch (finding.kind) {
    case Kind::UseOfBuiltin:
    case Kind::ReadOfAnyMember: {
        auto uses = OptionSet<BuiltinUse>::fromRaw(finding.detail);
        BuiltinID canonical = m_builtins.canonicalID(BuiltinID(finding.subject));
        if (uses.containsAny(writingUses))
            noteWritten(BuiltinID(finding.subject));
        // Whoever is handed the function may pass it anything.
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
            // It may name whatever an executable leaves to the program, and nothing else: noteWritten() passes the others by.
            for (BuiltinID member : m_builtins.membersOf(holder))
                noteWritten(member);
        } else if (finding.key == Wire::none)
            m_isWrittenUnderAnyName.quickSet(holder.index());
        else if (BuiltinID member = findOwnMember(m_source, holder, PropertyKeyID(finding.key)))
            noteWritten(member);
        break;
    }
    case Kind::NameStoredByRow:
        break;
    case Kind::NameReadOffUnknownValue:
        m_namesReadByProgram.set(finding.subject);
        break;
    case Kind::ReadThatNoRowAnswers:
    case Kind::KeysOfBuiltinListed:
    case Kind::UnseenCode:
    case Kind::LocaleUse:
    case Kind::WorkerEntry:
    case Kind::WorkerStartWithoutName:
    case Kind::URLThatStartsWorker:
    case Kind::URLThatLeavesBody:
    case Kind::NamedModule:
        break;
    }
}

bool WritesOnBuiltinsInProgramFacts::canLockAtStartUp(BuiltinID member) const
{
    const Builtin& entry = m_builtins.at(member);
    if (!entry.holder || !entry.name || entry.mutability != BuiltinMutability::ConstantUnlessWritten || m_builtins.isLocationOfLockedObject(member))
        return false;
    if (entry.scope != BuiltinScope::Realm || !m_builtins.isUnder(member, Builtins::GlobalThis))
        return false;
    // A default export is no property of its holder: the loader hands it out.
    return m_builtins.findOwnMember(entry.holder, entry.name) == member;
}

bool WritesOnBuiltinsInProgramFacts::isReadOffUnseen(BuiltinName name) const
{
    PropertyKeyID key = m_source.keyOf(name);
    return m_lookups.readsUnderKeysNobodyKnows || (key && m_lookups.namesRead.get(key.index()));
}

// A value that nobody knows may be the object.
bool WritesOnBuiltinsInProgramFacts::isReachedThroughUnseen(BuiltinID canonicalHolder) const
{
    // `this` of a sloppy function, `Function("return this")()`: nobody has to name the global object to come by it.
    if (m_isWithinReachOfUnseen.quickGet(canonicalHolder.index()) || canonicalHolder == m_builtins.canonicalID(Builtins::GlobalThis))
        return true;
    const Builtin& entry = m_builtins.at(canonicalHolder);
    switch (entry.kind) {
    // `unknown.toString.tag = 1`: the name finds the function on a prototype object or a constructor, and the function has not escaped with that.
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return entry.name && isReadOffUnseen(entry.name);
    // `Object.getPrototypeOf(unknown).deref = f`, `unknown.constructor.prototype.deref = f`
    case BuiltinKind::Prototype:
        return m_lookups.climbsFromUnseenObjects || isReadOffUnseen(BuiltinNames::__proto__) || (isReadOffUnseen(BuiltinNames::constructor) && isReadOffUnseen(BuiltinNames::prototype));
    case BuiltinKind::Constructor:
    case BuiltinKind::Namespace:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `Date.now = f` names the object and the member. `Date[name] = f` names the object. `put(Date, name, f)`: the flow brings the object to the store.
// `pick(Math, Date).now = f`, `for (const holder of [Date, performance]) holder[name] = f`: the flow brings several, and the write counts on each: LinkType::holdsLostBuiltins.
// `hidden.now = f`: code of the program spells the name on a value that nobody knows, which may be the object.
// `emitter._events = Math; emitter.on(name, listener)`: an operation of the program lets the object go, and whoever has it may write on it: `events[type] = listener` of node:events.
// What the library alone lets go stays locked: it keeps built-in objects in objects of its own, `w.emitter = process`, and replaces no member so. It lays methods on the prototype object of a class that native code hands it:
// `Object.defineProperty(MessagePort.prototype, "close", d)` of node:worker_threads.
ReasonBuiltinStaysOpen WritesOnBuiltinsInProgramFacts::reasonToLeaveOpen(BuiltinID member, const BitVector& lost) const
{
    if (!m_locksAreLaid)
        return ReasonBuiltinStaysOpen::NoLockIsLaid;
    for (size_t passed : m_lookups.builtinsPassedOptionThatRunsCode) {
        if (runsCodeThatNoLockIsLaidBeside(BuiltinID(static_cast<uint32_t>(passed)), true))
            return ReasonBuiltinStaysOpen::NoLockIsLaid;
    }
    BuiltinID canonical = m_builtins.canonicalID(member);
    // Text that a debugger evaluates does with the realm what it likes, and is turned away nowhere.
    if (m_source.admitsInspector)
        return ReasonBuiltinStaysOpen::IsOpenToDebugger;
    // The run time leaves such a member open for the tests.
    if (m_source.isUnlocked(member) || m_source.isUnlocked(canonical))
        return ReasonBuiltinStaysOpen::IsLeftOpenForTests;
    if (m_isWritten.quickGet(member.index()) || m_isWritten.quickGet(canonical.index()) || m_lookups.membersWritten.get(member.index()))
        return ReasonBuiltinStaysOpen::IsWrittenUnderItsName;
    BuiltinID holder = m_builtins.canonicalID(m_builtins.at(member).holder);
    if (m_isWrittenUnderAnyName.quickGet(holder.index()) || m_lookups.holdersWrittenUnderAnyName.get(holder.index()))
        return ReasonBuiltinStaysOpen::HolderIsWrittenUnderAnyName;
    PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
    if (lost.quickGet(holder.index())) {
        if (m_lookups.writesUnderAnyNameThroughLostBuiltins || (key && m_lookups.namesWrittenThroughLostBuiltins.get(key.index())))
            return ReasonBuiltinStaysOpen::IsWrittenThroughLostBuiltins;
        // `const find = Object.entries(roads)[0][1]; find()[name] = f`: the value has gone out of sight and come back.
        if (m_lookups.lostBuiltinsReachUnseen && (m_lookups.writesUnderAnyNameThroughUnseenByProgram || (key && m_lookups.namesWrittenThroughUnseenByProgram.get(key.index()))))
            return ReasonBuiltinStaysOpen::IsWrittenThroughLostBuiltins;
    }
    if (key && m_lookups.namesWrittenThroughUnseenByProgram.get(key.index()) && isReachedThroughUnseen(holder))
        return ReasonBuiltinStaysOpen::IsSpelledThroughUnseen;
    if (key && m_lookups.namesWrittenThroughUnseenByLibrary.get(key.index()) && m_builtins.at(holder).origin == BuiltinOrigin::Host && m_builtins.at(holder).kind == BuiltinKind::Prototype)
        return ReasonBuiltinStaysOpen::IsSpelledThroughUnseen;
    if ((m_source.isLockLeftToProgram(member) ? m_isCarriedOutBesideProduct : m_isCarriedOutByProgram).quickGet(holder.index()))
        return ReasonBuiltinStaysOpen::HolderIsLetGoByProgram;
    return ReasonBuiltinStaysOpen::None;
}

// ---- The whole program

class BuilderOfProgramFacts {
    WTF_MAKE_NONCOPYABLE(BuilderOfProgramFacts);
public:
    BuilderOfProgramFacts(const InputsOfProgramFacts&, FactTables&);

    void run();

    // For BuiltinsInReach: the first half of run(), piece by piece.
    void noteReached(FunctionID);
    void noteFindingsIn(FunctionID id) { take(m_inputs.looks.of(id)); }
    void noteObjectsThatBodyMakes(FunctionID id) { noteWhatBodyMakes(id); }
    bool hasNoFindingsIn(FunctionID id) const { return m_inputs.looks.of(id).empty(); }
    bool knowsWhatBodyMakes(FunctionID) const;
    WritesOnBuiltinsInProgramFacts& writes() LIFETIME_BOUND { return m_writes; }
    void noteEvaluated(ModuleID id) { evaluate(id, noWitness); }
    void setNativeCodeMayHoldObjects(bool mayHold) { m_nativeCodeMayHoldObjects = mayHold; }
    void take(const LookupsPastProgramObjects&);
    void noteNameRead(PropertyKeyID key) { m_namesRead.set(key.index()); }
    bool isWrittenInSight(BuiltinID canonical) const { return m_holdersWrittenInSight.get(canonical.index()); }
    void noteNameCalled(PropertyKeyID key) { m_namesCalled.set(key.index()); }
    void noteNameSpelledByProgram(PropertyKeyID key) { m_namesSpelledByProgram.set(key.index()); }
    void noteReadUnderKeyNobodyKnows() { m_readsUnderComputedKeys = true; }
    void noteUse(BuiltinID builtin, OptionSet<BuiltinUse> uses) { m_uses[builtin.index()].add(uses); }
    void noteCall(BuiltinID builtin) { noteUse(m_builtins.canonicalID(builtin), callingUsesOf(m_builtins.at(m_builtins.canonicalID(builtin)).kind) | OptionSet<BuiltinUse> { BuiltinUse::Read }); }
    void noteReadOfMember(BuiltinID);
    void noteLossOf(BuiltinID);
    void noteCallOutOfSight(BuiltinID builtin) { m_isCalledOutOfSightOfLink.quickSet(m_builtins.canonicalID(builtin).index()); }
    void takeBuiltinsThatCameOutOfSight(Vector<BuiltinID>&);
    bool objectsMayHaveMethodCalledWithStartupLoads() const { return m_objectsMayHaveMethodCalledWithStartupLoads; }
    void settle();
    void takeBuiltinsThatCameIntoReach(Vector<BuiltinID>&);
    bool hasRecordOfUnseenCode() const { return !m_program.unseenCode.isEmpty(); }
    bool unseenCodeCanRun() const { return hasRecordThat(admitsUnknownJavaScriptInProgramFacts); }
    // Code of the program evaluates a text or loads a file that was not compiled, or the options of the build admit one. What the library alone could let in nobody has asked for.
    bool programLetsUnseenCodeRun() const
    {
        return std::ranges::any_of(m_program.unseenCode, [&](const UnseenCodeRecord& record) {
            if (!admitsUnknownJavaScriptInProgramFacts(record.cause))
                return false;
            if (record.witness.function == Wire::none)
                return record.cause == UnseenCodeCause::OptionAdmitsCode;
            return !isLibraryCode(m_source, m_source.at(FunctionID(record.witness.function)));
        });
    }

private:
    using Finding = FindingOfProgramFacts;

    static OptionSet<BuiltinUse> callingUsesOf(BuiltinKind);

    bool hasRecordThat(bool (*holdsOf)(UnseenCodeCause)) const { return std::ranges::any_of(m_program.unseenCode, holdsOf, &UnseenCodeRecord::cause); }
    bool isReached(FunctionID id) const { return m_inputs.linked.function(id).isReached; }

    void take(std::span<const Finding>);
    void takeNamesOf(FunctionID);
    bool linkSeesCallAt(const Witness&, BuiltinID canonical);
    void noteObjectsMayExist(BuiltinClassID);
    void noteTypeIsMade(BuiltinTypeID, BuiltinOrigin);
    void noteWhatRowsMake(BuiltinID canonical);
    void noteWhatBodyMakes(FunctionID);
    void lookThroughPrototypesOfClassesWithObjects();
    bool mayExport(const Module&, StringID name) const;
    void useBuiltinsThatHostHandsExportsTo();
    void followModules();
    void evaluate(ModuleID, const Witness&);

    void letGo(BuiltinID canonical, const Witness&);
    void lookThrough(BuiltinID canonicalHolder);
    void spreadBelow(BuiltinID canonicalHolder);
    void findMembersOffUnknownValues(BuiltinID canonicalHolder);
    void followWhatIsLetGo();
    bool standsInTextOfProgram(const Witness& witness) const { return hasFunction(witness) && !isLibraryCode(m_source, m_source.at(FunctionID(witness.function))); }
    // Text of the program first: a place of the program's own is not hidden by one of the library.
    bool standsInFront(const Witness& witness, const Witness& other) const
    {
        bool isOfProgram = standsInTextOfProgram(witness);
        return isOfProgram != standsInTextOfProgram(other) ? isOfProgram : isLowerWitness(witness, other);
    }
    void keepPlaceOf(BuiltinID canonical, Witness& kept, const Witness& candidate) const
    {
        if (!m_rows.areEvents.quickGet(canonical.index()))
            keepLowerWitnessInProgramFacts(kept, candidate);
        else if (standsInFront(candidate, kept))
            kept = candidate;
    }
    void uniteUsesOfSameObject();

    static void add(Vector<UnseenCodeRecord>&, UnseenCodeCause, BuiltinID, const Witness&);
    std::optional<UnseenCodeCause> causeOfLettingCodeIn(BuiltinID canonical) const;
    void listBuiltinsThatLetCodeIn();
    void listLoadsOfUnseenCode();
    void listLoadsOfMachineCode();
    void listBuiltinUses();
    void listHoldersWhoseKeysAreNeverListed();
    void lockMembersThatNobodyWrites();

    bool isConstantLocation(BuiltinID member) const;
    BuiltinID builtinReadByPath(const BodyIndex&, ValueID) const;
    bool isSurelyNot(const BodyIndex&, ValueID, BuiltinID member) const;
    bool changesMember(const BodyIndex&, ValueID descriptor, BuiltinID member, bool& isCertain) const;
    bool holdsNoCodeOfProgram(const BodyIndex&, ValueID) const;
    bool namesBuiltinThatEndsProcess(const BodyIndex&, const BodyValue&) const;
    bool objectsFindNothingUnder(LayoutID, PropertyKeyID) const;
    bool findsNoThen(FunctionID, const BodyIndex&, ValueID awaited) const;
    bool codeThatNobodyNamesMayLeave();
    bool mayLeave(FunctionID, const BodyIndex&, const BodyValue&);
    bool bodyMayLeave(FunctionID);
    Vector<ModuleID> evaluationOrder() const;
    bool everyRunComesTo(const Witness&);
    std::optional<UserDiagnosticKind> refusalOf(const BodyIndex&, const Witness&, BuiltinID holder, PropertyKeyID, BuiltinPropertyActionKind, ValueID brought, bool throwsWhereRefused);
    // `followed`: what the flow has brought to the write, for an operand of which the body does not say what it is.
    std::optional<UserDiagnosticKind> refusalOfWriteAt(const Witness&, BuiltinID followed = { });
    std::optional<UserDiagnosticKind> refusalOfCopy(const BodyIndex&, const Witness&, BuiltinID holder, BuiltinPropertyActionKind, ValueID source, bool throwsWhereRefused);
    template<typename Functor> bool forEachPropertyOfWrittenLiteral(const BodyIndex&, ValueID, ValueID handedOverAt, const Functor&) const;
    const DataNode::Object* objectOfConstantsBehind(const BodyIndex&, ValueID) const;
    bool isHandedOverOnce(const BodyIndex&, ValueID made, Vector<PropertyKeyID, 2>& namesTakenAway) const;
    bool isPartThatBringsValueOrAccessor(PropertyKeyID) const;
    bool bringsValueOrAccessor(const DataNode::Object& descriptor) const;
    void reportWritesThatLocksRefuse();
    void reportLoadsUnderNamesThatNobodyHasRead();
    bool frontEndLeavesMainModuleOpen() const;
    bool needsOneInstanceAtMost() const;
    bool isHeldByEveryProgram(RuntimeFunctionID) const;
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

    // By built-in, as the table lists it.
    Vector<OptionSet<BuiltinUse>> m_uses;
    Vector<Witness> m_witnesses;
    // An operation names it, reads it off its holder, calls it or lets it go.
    BitVector m_isNamedByOperation;
    // By canonical built-in.
    Vector<Witness> m_witnessesOfEscapes;
    BitVector m_isLetGo;
    // Found under a name, or under a key that nobody knows, on a value that nobody knows: its calls are not seen.
    BitVector m_isOutOfSight;
    // By canonical ID. A call of it is written where the scan has not bound it, or goes by no row.
    BitVector m_isCalledOutOfSightOfLink;
    // linkSeesCallAt(). Of the function that was asked about last, once it was asked about often: the site and the built-in of each call that the scan has bound to one, in order.
    Vector<uint64_t> m_callsBoundToBuiltins;
    uint32_t m_functionOfCallsBoundToBuiltins { UINT32_MAX };
    uint32_t m_numberOfQuestionsAboutCallsOfFunction { 0 };
    BitVector m_hasComeOutOfSight;
    // A VALUE THAT NOBODY KNOWS IS AN OBJECT THAT MAY EXIST. By class: an operation of reached code makes an object of it, a row of a built-in that is used does, or the run time does by itself.
    // Nobody has the prototype object of a class without objects but by naming the class, which lets it go like any holder.
    BitVector m_classesWithObjects;
    BitVector m_typesMade;
    // By canonical ID.
    BitVector m_rowsAreReadForWhatTheyMake;
    Vector<BuiltinClassID> m_classOfPrototype;
    // Native code of the host hands out an object of which its row says nothing.
    bool m_objectsOfAnyClassMayExist { false };
    bool m_classesWithObjectsHaveGrown { true };
    bool m_objectsMayHaveMethodCalledWithStartupLoads { false };
    BitVector m_isLookedThrough;
    // A call in sight may pass the property with which the row runs unseen code.
    BitVector m_mayBePassedOptionThatRunsCode;
    Vector<BuiltinID> m_holdersToSpreadBelow;
    Vector<BuiltinID> m_holdersToLookThrough;
    // By built-in, as the table lists it: reached code reads or calls its name on a value that nobody knows.
    BitVector m_isFoundByName;
    // By canonical ID.
    // Until the program has a cause of its own: settle().
    bool m_eventsAreWithheld { true };
    BitVector m_keysAreListedInSight;
    // By key: a call in text of the program has a row write under the name, which the call spells, on a built-in that the look follows: `Object.defineProperty(Function.prototype, "me", descriptor)`.
    BitVector m_namesThatRowsLayOnBuiltins;
    Vector<Finding> m_readsOffObjectsOfModulesThatNoMemberAnswers;

    // By property key, over reached code.
    // By the entry that stands for the object: the look has followed a write to it.
    BitVector m_holdersWrittenInSight;
    BitVector m_namesRead;
    BitVector m_namesCalled;
    BitVector m_namesSpelledByProgram;
    bool m_readsUnderComputedKeys { false };
    // Over reached code.
    WritesOnBuiltinsInProgramFacts m_writes;
    // By name of the table: what an operation or native code looks up by a protocol, without a word in any summary.
    BitVector m_namesOfProtocols;
    // False: it holds primitives and the built-ins that it was handed in sight, so it looks nothing up.
    bool m_nativeCodeMayHoldObjects { false };
    // By canonical built-in: takeBuiltinsThatCameIntoReach() has listed it.
    BitVector m_hasComeIntoReach;

    BitVector m_modulesEvaluated;
    Vector<ModuleID> m_modulesToFollow;

    Vector<Witness> m_writesOfProgramOnBuiltins;
    Vector<Witness> m_loadsOfProgramByName;
    BitVector m_bodiesBeingAsked;
    std::optional<bool> m_codeThatNobodyNamesMayLeave;
    Vector<LocaleUse> m_localeUsesAtCalls;
    Vector<Witness> m_urlsThatLeaveBodies;
    bool m_isStartedWithoutName { false };
    Witness m_startWithoutName { noWitness };
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
    , m_rows(m_builtins)
    , m_refused(codeRefusedInProgramFacts(inputs.program, inputs.build))
    , m_globalObject(m_builtins.canonicalID(Builtins::GlobalThis))
    , m_tables(tables)
    , m_program(tables.program)
    , m_isNamedByOperation(m_builtins.builtinCount())
    , m_isLetGo(m_builtins.builtinCount())
    , m_isOutOfSight(m_builtins.builtinCount())
    , m_isCalledOutOfSightOfLink(m_builtins.builtinCount())
    , m_hasComeOutOfSight(m_builtins.builtinCount())
    , m_classesWithObjects(m_builtins.classCount())
    , m_rowsAreReadForWhatTheyMake(m_builtins.builtinCount())
    , m_keysAreListedInSight(m_builtins.builtinCount())
    , m_isLookedThrough(m_builtins.builtinCount())
    , m_mayBePassedOptionThatRunsCode(m_builtins.builtinCount())
    , m_isFoundByName(m_builtins.builtinCount())
    , m_writes(inputs.program, m_refused, inputs.linked.lookups)
    , m_namesOfProtocols(m_builtins.nameCount())
    , m_hasComeIntoReach(m_builtins.builtinCount())
    , m_modulesEvaluated(inputs.program.modules.size())
{
    m_uses.fill(OptionSet<BuiltinUse>(), m_builtins.builtinCount());
    m_witnesses.fill(noWitness, m_builtins.builtinCount());
    m_witnessesOfEscapes.fill(noWitness, m_builtins.builtinCount());
    for (Protocol protocol : allMembersOf<Protocol>) {
        for (BuiltinName name : m_builtins.keysReadBy(protocol))
            m_namesOfProtocols.set(name.index());
    }

    m_classOfPrototype.fill(BuiltinClassID(), m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
        BuiltinClassID builtinClass(index);
        if (BuiltinID prototype = m_builtins.at(builtinClass).prototype)
            m_classOfPrototype[m_builtins.canonicalID(prototype).index()] = builtinClass;
        // Any operation may throw one, and no node says which.
        if (m_builtins.isSameOrSubclassOf(builtinClass, BuiltinClasses::Error))
            noteObjectsMayExist(builtinClass);
    }
    // Every program has them, or a primitive that finds what they find.
    for (BuiltinClassID builtinClass : { BuiltinClasses::Object, BuiltinClasses::Function, BuiltinClasses::Array, BuiltinClasses::Arguments, BuiltinClasses::String, BuiltinClasses::Number, BuiltinClasses::Boolean, BuiltinClasses::Symbol, BuiltinClasses::BigInt })
        noteObjectsMayExist(builtinClass);
}

void BuilderOfProgramFacts::add(Vector<UnseenCodeRecord>& records, UnseenCodeCause cause, BuiltinID builtin, const Witness& witness)
{
    uint32_t number = builtin ? builtin.index() : Wire::none;
    auto found = std::ranges::find_if(records, [&](const UnseenCodeRecord& record) {
        return record.cause == cause && record.builtin == number;
    });
    if (found == records.end())
        records.append(UnseenCodeRecord { cause, number, witness });
    else
        keepLowerWitnessInProgramFacts(found->witness, witness);
}

void BuilderOfProgramFacts::noteStartWithoutName(const Witness& witness)
{
    m_isStartedWithoutName = true;
    keepLowerWitnessInProgramFacts(m_startWithoutName, witness);
}

void BuilderOfProgramFacts::take(std::span<const Finding> findings)
{
    for (const Finding& finding : findings) {
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
            if (uses.containsAny(callingUsesInProgramFacts) && !linkSeesCallAt(finding.witness, canonical))
                m_isCalledOutOfSightOfLink.quickSet(canonical.index());
            if (uses.contains(BuiltinUse::Escaped))
                letGo(canonical, finding.witness);
            break;
        }
        case Finding::Kind::WriteOnBuiltin: {
            BuiltinID holder = m_builtins.canonicalID(BuiltinID(finding.subject));
            m_holdersWrittenInSight.set(holder.index());
            BuiltinID member = finding.key == Wire::none ? BuiltinID() : findOwnMember(m_source, holder, PropertyKeyID(finding.key));
            if (member)
                m_uses[member.index()].add(BuiltinUse::Written);
            if (!isLibraryCode(m_source, m_source.at(FunctionID(finding.witness.function)))) {
                m_writesOfProgramOnBuiltins.append(finding.witness);
                if (finding.detail)
                    m_namesThatRowsLayOnBuiltins.set(finding.key);
            }
            break;
        }
        case Finding::Kind::NameStoredByRow:
            // Under a key that nobody knows: the symbol under which the host looks for an inspector may be the key.
            if (finding.subject == Wire::none)
                m_objectsMayHaveMethodCalledWithStartupLoads = true;
            break;
        case Finding::Kind::NameReadOffUnknownValue:
            break;
        case Finding::Kind::ReadThatNoRowAnswers:
            if (finding.detail) {
                m_readsOffObjectsOfModulesThatNoMemberAnswers.append(finding);
                break;
            }
            m_objectsMayHaveMethodCalledWithStartupLoads = true;
            break;
        case Finding::Kind::KeysOfBuiltinListed:
            m_keysAreListedInSight.quickSet(m_builtins.canonicalID(BuiltinID(finding.subject)).index());
            break;
        case Finding::Kind::OptionThatRunsCode:
            m_mayBePassedOptionThatRunsCode.quickSet(finding.subject);
            break;
        case Finding::Kind::UnseenCode:
            add(m_program.unseenCode, static_cast<UnseenCodeCause>(finding.detail), { }, finding.witness);
            if (static_cast<UnseenCodeCause>(finding.detail) == UnseenCodeCause::LoadsModuleByName)
                m_loadsOfProgramByName.append(finding.witness);
            break;
        case Finding::Kind::LocaleUse:
            m_localeUsesAtCalls.append(LocaleUse { static_cast<LocaleUseKind>(finding.detail), finding.tag, BuiltinID(finding.subject), finding.witness });
            break;
        case Finding::Kind::WorkerEntry:
            m_program.workerEntries.append(WorkerEntry { StringID(finding.subject), false, !!finding.detail, finding.witness });
            break;
        case Finding::Kind::WorkerStartWithoutName:
            noteStartWithoutName(finding.witness);
            break;
        case Finding::Kind::URLThatStartsWorker:
            m_program.urlsThatStartWorkers.append(finding.witness);
            break;
        case Finding::Kind::URLThatLeavesBody:
            m_urlsThatLeaveBodies.append(finding.witness);
            break;
        case Finding::Kind::NamedModule:
            evaluate(ModuleID(finding.subject), finding.witness);
            break;
        }
    }
}

// The scan has bound the call to the built-in, so Link goes by its rows and enters the code behind them with the operands of the call: CallInLinkCalls::callBuiltin().
bool BuilderOfProgramFacts::linkSeesCallAt(const Witness& witness, BuiltinID canonical)
{
    static constexpr uint32_t mostQuestionsAnsweredByGoingThroughCalls = 8;
    const auto& calls = m_inputs.summaries.at(FunctionID(witness.function)).calls;
    if (m_functionOfCallsBoundToBuiltins != witness.function) {
        m_functionOfCallsBoundToBuiltins = witness.function;
        m_numberOfQuestionsAboutCallsOfFunction = 0;
    }
    if (m_numberOfQuestionsAboutCallsOfFunction < mostQuestionsAnsweredByGoingThroughCalls) {
        ++m_numberOfQuestionsAboutCallsOfFunction;
        return std::ranges::any_of(calls, [&](const SummaryCall& call) {
            return call.site.index() == witness.value && call.callee == SummaryCall::Callee::Builtin && m_builtins.canonicalID(call.builtin) == canonical;
        });
    }
    if (m_numberOfQuestionsAboutCallsOfFunction == mostQuestionsAnsweredByGoingThroughCalls) {
        ++m_numberOfQuestionsAboutCallsOfFunction;
        m_callsBoundToBuiltins.shrink(0);
        for (const SummaryCall& call : calls) {
            if (call.callee == SummaryCall::Callee::Builtin)
                m_callsBoundToBuiltins.append(static_cast<uint64_t>(call.site.index()) << 32 | m_builtins.canonicalID(call.builtin).index());
        }
        std::ranges::sort(m_callsBoundToBuiltins);
    }
    return std::ranges::binary_search(m_callsBoundToBuiltins, static_cast<uint64_t>(witness.value) << 32 | canonical.index());
}

void BuilderOfProgramFacts::noteObjectsMayExist(BuiltinClassID builtinClass)
{
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (m_classesWithObjects.quickSet(current.index()))
            return;
        m_classesWithObjectsHaveGrown = true;
        // A trap answers under any key, the symbol that the host looks up too: on the proxy, and on whatever stands below it.
        if (m_builtins.at(current).type == ProxyObjectType)
            m_objectsMayHaveMethodCalledWithStartupLoads = true;
    }
}

// As FeaturesOfProgramFacts::noteTypeIsMade() reads a type.
void BuilderOfProgramFacts::noteTypeIsMade(BuiltinTypeID type, BuiltinOrigin origin)
{
    if (!type || m_typesMade.set(type.index()))
        return;
    const BuiltinType& made = m_builtins.at(type);
    // What the engine's own code hands out without a word on it was handed in, or lies in what was.
    if (origin == BuiltinOrigin::Host && made.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject }) && !std::exchange(m_objectsOfAnyClassMayExist, true))
        m_classesWithObjectsHaveGrown = true;
    for (const BuiltinTypePart& part : made.parts) {
        if (part.builtinClass)
            noteObjectsMayExist(part.builtinClass);
        if (part.kind == BuiltinTypePartKind::Layout) {
            for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
                noteTypeIsMade(field.type, origin);
        }
        for (const BuiltinSlotContents& contents : part.contents)
            noteTypeIsMade(contents.type, origin);
    }
}

// As FeaturesOfProgramFacts::noteTypesThatOverloadMakes() reads a row.
void BuilderOfProgramFacts::noteWhatRowsMake(BuiltinID canonical)
{
    BuiltinOrigin origin = m_builtins.at(canonical).origin;
    auto notePlace = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue)
            noteTypeIsMade(place.newValueType, origin);
    };
    if (BuiltinClassID made = m_builtins.at(canonical).instanceClass)
        noteObjectsMayExist(made);
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(canonical)) {
        if (overload.isUndeclared() && !std::exchange(m_objectsOfAnyClassMayExist, true))
            m_classesWithObjectsHaveGrown = true;
        noteTypeIsMade(overload.result.type, origin);
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
        if (node.kind == SummaryNode::Kind::ClassInstance)
            noteObjectsMayExist(BuiltinClassID(node.first));
        else if (node.kind == SummaryNode::Kind::Closure)
            noteFunctionObject(m_source.at(FunctionID(node.first)));
    }
}

void BuilderOfProgramFacts::lookThroughPrototypesOfClassesWithObjects()
{
    do {
        for (uint32_t index = 0; index < m_uses.size(); ++index) {
            if (m_builtins.canonicalID(BuiltinID(index)).index() != index || (m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
                continue;
            // What `new F()` makes stands below F.prototype, whatever the rows say of its class.
            if (m_uses[index].contains(BuiltinUse::Constructed)) {
                if (BuiltinID prototype = m_builtins.findOwnMember(BuiltinID(index), BuiltinNames::prototype))
                    lookThrough(m_builtins.canonicalID(prototype));
            }
            if (!m_rowsAreReadForWhatTheyMake.quickSet(index))
                noteWhatRowsMake(BuiltinID(index));
        }
        if (std::exchange(m_classesWithObjectsHaveGrown, false)) {
            for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
                BuiltinID builtin(index);
                if (m_builtins.canonicalID(builtin) != builtin || m_builtins.at(builtin).kind != BuiltinKind::Prototype)
                    continue;
                BuiltinClassID owner = m_classOfPrototype[index];
                if (m_objectsOfAnyClassMayExist || !owner || m_classesWithObjects.quickGet(owner.index()))
                    lookThrough(builtin);
            }
        }
        followWhatIsLetGo();
        uniteUsesOfSameObject();
        // What was found is used, and its rows make objects in turn.
    } while (m_classesWithObjectsHaveGrown || std::ranges::any_of(std::views::iota(uint32_t(0), static_cast<uint32_t>(m_uses.size())), [&](uint32_t index) {
        return m_builtins.canonicalID(BuiltinID(index)).index() == index && !m_rowsAreReadForWhatTheyMake.quickGet(index) && !(m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty();
    }));
}

void BuilderOfProgramFacts::takeNamesOf(FunctionID id)
{
}

void BuilderOfProgramFacts::take(const LookupsPastProgramObjects& lookups)
{
    m_namesRead.merge(lookups.namesRead);
    m_namesCalled.merge(lookups.namesCalled);
    m_namesSpelledByProgram.merge(lookups.namesSpelledByProgram);
    for (size_t index : lookups.builtinsRead)
        noteReadOfMember(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsCalled)
        noteCall(BuiltinID(static_cast<uint32_t>(index)));
    for (size_t index : lookups.builtinsLetGo)
        noteLossOf(BuiltinID(static_cast<uint32_t>(index)));
    m_readsUnderComputedKeys |= lookups.readsUnderKeysNobodyKnows;
    m_nativeCodeMayHoldObjects |= lookups.nativeCodeMayHoldObjects;
}

// ---- Modules

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
    if (isCodeOutsideImageInProgramFacts(module))
        add(m_program.unseenCode, UnseenCodeCause::ImportsUncompiledModule, { }, witness);
    // `import { heapStats } from "bun:jsc"`: where the loader fills a variable for a name out of the object of a module of native code, nobody follows the variable.
    BuiltinID object = module.runtimeModule ? m_builtins.namespaceOf(module.runtimeModule) : BuiltinID();
    bool fillsVariables = std::ranges::any_of(module.syntheticExports, [](const SyntheticExport& entry) {
        return !!entry.variable;
    });
    if (object && fillsVariables)
        letGo(m_builtins.canonicalID(object), witness);
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

    while (!m_modulesToFollow.isEmpty()) {
        ModuleID id = m_modulesToFollow.takeLast();
        const Module& module = m_source.at(id);
        if (isLibraryModuleWithoutCodeInProgramFacts(m_source, module))
            m_program.libraryModulesLacked.append(id);
        Witness witness = module.body ? Witness { module.body.index(), Wire::none, m_source.at(module.body).declarationLocation() } : noWitness;
        for (const ModuleRequest& request : module.requests)
            evaluate(request.target, witness);
    }
    AOT::sort(m_program.libraryModulesLacked);
}

// ---- What is let go, and what values that nobody knows find

void BuilderOfProgramFacts::letGo(BuiltinID canonical, const Witness& witness)
{
    uint32_t index = canonical.index();
    bool isLower = standsInFront(witness, m_witnessesOfEscapes[index]);
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

// Whoever holds the object comes by all that hangs below it.
void BuilderOfProgramFacts::spreadBelow(BuiltinID canonicalHolder)
{
    Witness witness = m_witnessesOfEscapes[canonicalHolder.index()];
    // Where the text of the library or a lookup has let the holder go, whoever holds it comes by a function whose call or read is an event as anybody does, by a lookup: findMembersOffUnknownValues().
    bool handsOutEvents = !m_eventsAreWithheld || standsInTextOfProgram(witness);
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        if (!handsOutEvents && m_rows.areEvents.quickGet(m_builtins.canonicalID(member).index()))
            continue;
        m_uses[member.index()].add(BuiltinUse::Escaped);
        keepLowerWitnessInProgramFacts(m_witnesses[member.index()], witness);
        letGo(m_builtins.canonicalID(member), witness);
    }
}

// `list.map(f)`, `text[name]`: a value that nobody knows may be what was let go, an instance of any class, or what stands above either.
// `take(f).constructor(text)`: a name that THE TEXT OF THE PROGRAM reads or calls finds a function whose call or read is an event as it finds any other member.
// A name that the text of the library alone spells finds none: `Promise.resolve(rows).then(..)` beside any async function: the library reads `constructor` off what it is handed. What the library does with what it names is the library's matter,
// and the run time refuses the text of whoever comes by an evaluator on that road. A key that nobody knows and a protocol find none either: ObjectTable::mergedFormOf().
void BuilderOfProgramFacts::findMembersOffUnknownValues(BuiltinID canonicalHolder)
{
    // The object may have more than the table lists, and a read that nobody follows finds that too.
    if (!m_builtins.at(canonicalHolder).flags.containsAny({ BuiltinFlag::ListsAllMembers, BuiltinFlag::IsGlobalObject }) && (m_readsUnderComputedKeys || !m_namesRead.isEmpty()))
        m_objectsMayHaveMethodCalledWithStartupLoads = true;
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        BuiltinName name = m_builtins.at(member).name;
        if (!name)
            continue;
        PropertyKeyID key = m_source.keyOf(name);
        bool isRead = key && m_namesRead.get(key.index());
        bool isNamedByCode = isRead || (key && m_namesCalled.get(key.index()));
        bool isNamed = isNamedByCode || (m_nativeCodeMayHoldObjects && (m_builtins.isSymbolName(name) || m_namesOfProtocols.get(name.index())));
        bool isHandedOn = isRead || m_readsUnderComputedKeys;
        if (!isNamed && !isHandedOn)
            continue;
        BuiltinID canonical = m_builtins.canonicalID(member);
        if (m_rows.areEvents.quickGet(canonical.index()) && !(m_eventsAreWithheld ? key && m_namesSpelledByProgram.get(key.index()) : isNamedByCode))
            continue;
        m_uses[member.index()].add(BuiltinUse::Read);
        if (isNamedByCode)
            m_isFoundByName.quickSet(member.index());
        m_isOutOfSight.quickSet(canonical.index());
        // A key that nobody knows names nothing: BuiltinUse::Escaped says all that follows from it.
        if (isNamed)
            m_uses[canonical.index()].add(callingUsesOf(m_builtins.at(canonical).kind));
        if (isHandedOn && standsForObjectInProgramFacts(m_builtins, canonical))
            letGo(canonical, noWitness);
    }
    // The lookup goes on to what stands above the object, which is not let go by that.
    const Builtin& object = m_builtins.at(canonicalHolder);
    if (object.prototypeLink == BuiltinPrototypeLink::Builtin)
        lookThrough(m_builtins.canonicalID(object.inheritsFrom));
}

// Link follows what the read gives: noteLossOf().
void BuilderOfProgramFacts::noteReadOfMember(BuiltinID member)
{
    m_uses[member.index()].add(BuiltinUse::Read);
    m_isFoundByName.quickSet(member.index());
}

// As findMembersOffUnknownValues() does with what a name that is read finds.
void BuilderOfProgramFacts::noteLossOf(BuiltinID builtin)
{
    BuiltinID canonical = m_builtins.canonicalID(builtin);
    m_uses[canonical.index()].add(BuiltinUse::Read);
    m_isOutOfSight.quickSet(canonical.index());
    m_uses[canonical.index()].add(callingUsesOf(m_builtins.at(canonical).kind));
    if (standsForObjectInProgramFacts(m_builtins, canonical))
        letGo(canonical, noWitness);
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

// ---- The lists

std::optional<UnseenCodeCause> BuilderOfProgramFacts::causeOfLettingCodeIn(BuiltinID canonical) const
{
    if (std::optional<UnseenCodeCause> event = eventOfCalling(m_builtins, canonical, m_refused))
        return event;
    // Each call in sight has been asked for the option. Whoever else has the function may pass it anything.
    if (optionThatRunsCodeInProgramFacts(m_builtins, canonical) && !m_mayBePassedOptionThatRunsCode.quickGet(canonical.index()) && !m_inputs.linked.lookups.builtinsPassedOptionThatRunsCode.get(canonical.index()) && !m_isLetGo.quickGet(canonical.index()) && !m_isOutOfSight.quickGet(canonical.index()))
        return std::nullopt;
    // The call would let code in if nothing were refused. In this build it throws.
    if (eventOfCalling(m_builtins, canonical, RefusedCode { }))
        return std::nullopt;
    if (m_builtins.has(BuiltinMark::ReadLoadsCodeByName, canonical))
        return m_refused.codeOutsideImage ? std::nullopt : std::optional(UnseenCodeCause::LoadsCodeByName);
    // A load of machine code has its one record in loadsOfMachineCode.
    if (isLoaderOfMachineCodeInProgramFacts(m_builtins, canonical))
        return std::nullopt;
    if (m_builtins.has(BuiltinMark::LoadsCodeWithoutJSAccess, canonical))
        return std::nullopt;
    return UnseenCodeCause::RunsUnseenCode;
}

void BuilderOfProgramFacts::listBuiltinsThatLetCodeIn()
{
    for (size_t index : m_rows.areEvents) {
        BuiltinID builtin(static_cast<uint32_t>(index));
        OptionSet<BuiltinUse> uses = m_uses[index];
        bool readIsEvent = m_builtins.has(BuiltinMark::ReadLoadsCodeByName, builtin) || m_builtins.at(builtin).kind == BuiltinKind::Accessor;
        if (!uses.containsAny(usesOfObjectItselfInProgramFacts) && !(readIsEvent && uses.contains(BuiltinUse::Read)))
            continue;
        if (std::optional<UnseenCodeCause> cause = causeOfLettingCodeIn(builtin))
            add(m_program.unseenCode, *cause, builtin, standsInTextOfProgram(m_witnesses[index]) ? m_witnesses[index] : noWitness);
    }
}

// Code that nobody has read has import() without being handed a loader, and the run time turns away every name that no record lets in.
void BuilderOfProgramFacts::listLoadsOfUnseenCode()
{
    auto admitting = std::ranges::find_if(m_program.unseenCode, admitsUnknownJavaScriptInProgramFacts, &UnseenCodeRecord::cause);
    if (admitting == m_program.unseenCode.end())
        return;
    Witness witness = admitting->witness;
    add(m_program.unseenCode, m_refused.codeOutsideImage ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName, { }, witness);
}

void BuilderOfProgramFacts::listLoadsOfMachineCode()
{
    for (const BuiltinLoaderOfMachineCode& loader : m_builtins.loadersOfMachineCode()) {
        // The program may hold `named` alone, with `callee` behind code that this look does not follow.
        for (BuiltinID builtin : { loader.callee, loader.named }) {
            if (!builtin)
                continue;
            uint32_t index = m_builtins.canonicalID(builtin).index();
            if (m_uses[index].containsAny(usesOfObjectItselfInProgramFacts))
                add(m_program.loadsOfMachineCode, UnseenCodeCause::RunsMachineCode, loader.named, standsInTextOfProgram(m_witnesses[index]) ? m_witnesses[index] : noWitness);
        }
    }
}

void BuilderOfProgramFacts::listBuiltinUses()
{
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        if (!m_uses[index].isEmpty())
            m_tables.builtinUses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(m_uses[index].toRaw()), m_witnesses[index] });
    }
}

// The look at a body follows a built-in from the operation that names it as far as that body, and lets it go anywhere else. What Link has found by a read it follows further, and says nothing of what is done with it.
// A prototype object is handed out by whoever is asked what stands above an instance.
void BuilderOfProgramFacts::listHoldersWhoseKeysAreNeverListed()
{
    if (m_program.unseenCodeCanRun || m_source.admitsInspector)
        return;
    BitVector mayBeHeld(m_builtins.builtinCount());
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (m_isFoundByName.quickGet(index) || m_uses[index].contains(BuiltinUse::Escaped))
            mayBeHeld.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
    }
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID holder(index);
        BuiltinKind kind = m_builtins.at(holder).kind;
        if (m_builtins.canonicalID(holder) != holder || holder == m_globalObject || (kind != BuiltinKind::Namespace && kind != BuiltinKind::Constructor && kind != BuiltinKind::Function))
            continue;
        if (mayBeHeld.quickGet(index) || m_isLetGo.quickGet(index) || m_isOutOfSight.quickGet(index) || m_isLookedThrough.quickGet(index))
            continue;
        // Whoever goes through an object has what hangs on it.
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

// ---- Locks

// First the choice, then the claim that rests on it: a member that a program may replace, that this one uses and that it is not seen to write is locked when it starts. From then on it is a constant.
// Used: reached code names it, or Link has gone by it. What only text can come by is left to the text: `new Function("Bun.file = 1")()` in a program that never says Bun.file.
void BuilderOfProgramFacts::lockMembersThatNobodyWrites()
{
    // What Link has gone by to the end no write was followed to: BuiltinsInReach::takeForConstant().
    const BitVector& takenByLink = m_inputs.linked.lookups.membersTakenForConstants;
    m_writes.settle();
    // Whatever the program lets in has the objects of the language in its scope and goes by them as compiled code does: `eval("Date.n" + "ow = f")` is refused whether or not compiled code asks for the time.
    bool unseenCodeUsesObjectsOfLanguage = programLetsUnseenCodeRun();
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        bool isTakenByLink = takenByLink.get(index);
        // `process.platform`: the build has put the value of a constant in the place of its reads, so no operation shows them.
        bool isUsed = isTakenByLink || m_isNamedByOperation.quickGet(index) || m_isFoundByName.quickGet(index) || m_builtins.at(member).kind == BuiltinKind::Constant;
        isUsed |= unseenCodeUsesObjectsOfLanguage && m_builtins.at(member).origin == BuiltinOrigin::Engine;
        if (!isUsed || !m_writes.canLockAtStartUp(member)) {
            // The run time's own table locks such a member whether or not this program uses it, so the image has to let through a write that was followed to it.
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

// A member that the list lacks is a constant for the front end: it has decided `import.meta.main` and `require.main === module` in the code.
// The run time has to refuse the store then, or the code would answer for one file and `require.main` for another.
bool BuilderOfProgramFacts::frontEndLeavesMainModuleOpen() const
{
    return std::ranges::any_of(m_source.replaceableBuiltins, [&](const ReplaceableBuiltin& replaceable) {
        return m_builtins.contains(replaceable.member) && m_builtins.has(BuiltinMark::HoldsPathOfMainModule, m_builtins.canonicalID(replaceable.member));
    });
}

// ---- The writes that a lock turns away, as far as one body shows them

static constexpr OpcodeSet valuesMadeInBodyInProgramFacts = primitiveConstants | OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::FinishObject, Opcode::NewArray, Opcode::NewClass, Opcode::FinishClass, Opcode::Data };

bool BuilderOfProgramFacts::isConstantLocation(BuiltinID member) const
{
    if (m_builtins.has(BuiltinMark::MayBeAbsent, member))
        return false;
    return m_builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || m_builtins.isLocationOfLockedObject(member) || m_tables.constantBuiltinMembers.get(member.index());
}

// `Date.prototype`: each name of the path finds a member that never changes, so the read answers that member in every run.
BuiltinID BuilderOfProgramFacts::builtinReadByPath(const BodyIndex& index, ValueID value) const
{
    value = value ? index.valueBehindCasts(value) : ValueID();
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

bool BuilderOfProgramFacts::isSurelyNot(const BodyIndex& index, ValueID value, BuiltinID member) const
{
    if (BuiltinID builtin = builtinReadByPath(index, value))
        return !member || m_builtins.canonicalID(builtin) != m_builtins.canonicalID(member);
    value = value ? index.valueBehindCasts(value) : ValueID();
    return value && !index.isParameter(value) && valuesMadeInBodyInProgramFacts.contains(index.opcodeOf(value));
}

// Whether one operation alone takes the object that the body has made, beside those that delete a property of it under a name.
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
            mentions += operand && index.valueBehindCasts(operand) == made;
        });
        mentionsFound += mentions;
        if (!mentions)
            continue;
        if (index.valueBehindCasts(id) == made)
            count(id);
        else if (operation.opcode() == Opcode::PropDelete)
            namesTakenAway.append(operation.as<Opcode::PropDelete>().key());
        else
            takers += mentions;
    }
    // An edge may hand it to a parameter, under which nobody here looks for it.
    return takers == 1 && mentionsFound == mentionsCounted;
}

// A definition that asks for what is already there succeeds, locked or not: `{ writable: false }`. Only what the literal itself lists is surely there when the definition runs.
bool BuilderOfProgramFacts::changesMember(const BodyIndex& index, ValueID descriptor, BuiltinID member, bool& isCertain) const
{
    isCertain = false;
    descriptor = descriptor ? index.valueBehindCasts(descriptor) : ValueID();
    Vector<PropertyKeyID, 2> namesTakenAway;
    bool isAsBodyShows = descriptor && !index.isParameter(descriptor) && isHandedOverOnce(index, descriptor, namesTakenAway);
    if (isAsBodyShows && index.opcodeOf(descriptor) == Opcode::Data) {
        const auto* constant = m_source.at(index.value(descriptor).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>();
        return !constant || std::ranges::any_of(constant->members, [&](const DataMember& part) {
            return isPartThatBringsValueOrAccessor(part.key) && !namesTakenAway.contains(part.key);
        });
    }
    // `Object.defineProperty(Date.prototype, "getTime", Object.getOwnPropertyDescriptor(Date.prototype, "getTime"))` asks for what is there.
    if (descriptor && !index.isParameter(descriptor) && index.opcodeOf(descriptor) == Opcode::Call) {
        BodyValue call = index.value(descriptor);
        std::span<const ValueID> arguments = call.variadicOperands();
        BuiltinID callee = builtinReadByPath(index, call.operand(0));
        ValueID name = arguments.size() == 2 ? index.valueBehindCasts(arguments[1]) : ValueID();
        if (callee && m_builtins.canonicalID(callee) == m_builtins.canonicalID(Builtins::ObjectGetOwnPropertyDescriptor) && name && !index.isParameter(name) && index.opcodeOf(name) == Opcode::String) {
            BuiltinID described = builtinReadByPath(index, arguments[0]);
            std::optional<PropertyKeyID> key = m_source.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index.value(name).as<Opcode::String>().string() });
            if (described && key && findOwnMember(m_source, m_builtins.canonicalID(described), *key) == member)
                return false;
        }
    }
    if (!descriptor || index.isParameter(descriptor) || index.opcodeOf(descriptor) != Opcode::FinishObject)
        return true;
    ValueID literal = index.valueBehindCasts(index.value(descriptor).operand(0));
    if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewObject || index.blockOf(literal) != index.blockOf(descriptor))
        return true;

    bool isAccessor = m_builtins.at(member).kind == BuiltinKind::Accessor;
    bool changes = false;
    isCertain = true;
    bool isAsWritten = isAsBodyShows && forEachPropertyOfWrittenLiteral(index, descriptor, { }, [&](PropertyKeyID key, ValueID brought) {
        bool isValue = key == m_source.keyOf(BuiltinNames::value);
        if (!isPartThatBringsValueOrAccessor(key) || namesTakenAway.contains(key))
            return;
        // `{ value: Math.max }` on Math.max. The halves of a built-in accessor have no entries of their own.
        if (isValue && !isAccessor && !isSurelyNot(index, brought, member)) {
            if (builtinReadByPath(index, brought))
                return;
            isCertain = false;
        }
        if (!isValue && isAccessor && !isSurelyNot(index, brought, { }))
            isCertain = false;
        changes = true;
    });
    if (!isAsWritten) {
        isCertain = false;
        return true;
    }
    return changes;
}

bool BuilderOfProgramFacts::holdsNoCodeOfProgram(const BodyIndex& index, ValueID value) const
{
    if (builtinReadByPath(index, value))
        return true;
    value = index.valueBehindCasts(value);
    return value && !index.isParameter(value) && (primitiveConstants.contains(index.opcodeOf(value)) || widestPrimitiveResultOf(index.opcodeOf(value)));
}

// `process.exit`, whether it is called there or handed on. `this.exit` in a listener: the host hands out the holder, so its name on a value that nobody knows counts too.
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

// On an object of the layout, and on whatever the program has laid above it. Above that stands an object of the engine that the program cannot extend.
bool BuilderOfProgramFacts::objectsFindNothingUnder(LayoutID layout, PropertyKeyID key) const
{
    const LinkTables& linked = m_inputs.linked;
    const LinkedLayout& claims = linked.layout(layout);
    if (claims.reachesUnseenCode || !claims.listsEveryMemberAbove || claims.possibleChanges.containsAny({ LayoutChange::GainsProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor }))
        return false;
    if (findField(m_source, layout, key) || std::ranges::contains(linked.appendedKeysOf(layout), key))
        return false;
    return linked.findMember(m_source, layout, ClassSide::Prototype, key).kind == MemberFound::Kind::Nothing;
}

// `await 2`, `await new Point()`: what finds no `then` is handed back in a later turn, whatever else happens.
bool BuilderOfProgramFacts::findsNoThen(FunctionID id, const BodyIndex& index, ValueID awaited) const
{
    awaited = awaited ? index.valueBehindCasts(awaited) : ValueID();
    if (!awaited || index.isParameter(awaited))
        return false;
    Opcode opcode = index.opcodeOf(awaited);
    if (primitiveConstants.contains(opcode) || widestPrimitiveResultOf(opcode))
        return true;
    FunctionID callee = callsByValueInProgramFacts.contains(opcode) ? m_inputs.linked.calleeAt(id, awaited) : FunctionID();
    TypeID result = callee ? m_inputs.linked.function(callee).result : TypeID();
    if (!result)
        return false;
    Type type = m_source.at(result);
    if (!type.hasObjects())
        return type.isJavaScriptValue();
    PropertyKeyID then = m_source.keyOf(BuiltinNames::then);
    return type.hasOnlyLayouts() && std::ranges::all_of(membersOf(m_source, type), [&](LayoutID layout) {
        return objectsFindNothingUnder(layout, then);
    });
}

// What nobody names is text, native code, a built-in that somebody has in hand or a function of the program.
// Each function is asked while such code counts for nothing: where none leaves by what is in sight, none leaves.
bool BuilderOfProgramFacts::codeThatNobodyNamesMayLeave()
{
    if (m_codeThatNobodyNamesMayLeave)
        return *m_codeThatNobodyNamesMayLeave;
    m_codeThatNobodyNamesMayLeave = false;
    m_codeThatNobodyNamesMayLeave = [&] {
        // What the library alone could let in, or let go of, nobody has asked for.
        auto standsInProgram = [&](const Witness& witness) {
            return witness.function != Wire::none && !isLibraryCode(m_source, m_source.at(FunctionID(witness.function)));
        };
        if (programLetsUnseenCodeRun() || std::ranges::any_of(m_program.loadsOfMachineCode, standsInProgram, &UnseenCodeRecord::witness))
            return true;
        // Then only an operation that names it comes by it, and each of those leaves where it stands.
        for (const BuiltinMarkedEntry& entry : m_builtins.marked(BuiltinMark::MayTerminateProcess)) {
            uint32_t builtin = m_builtins.canonicalID(entry.builtin).index();
            if (m_isLetGo.quickGet(builtin) && standsInProgram(m_witnessesOfEscapes[builtin]))
                return true;
        }
        // The loader enters those in their order: everyRunComesTo().
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
    return *m_codeThatNobodyNamesMayLeave;
}

// Code leaves where what stands behind it may never run: the process ends, code of the program throws, a name that nobody has declared throws, the body of a module waits.
// What an operation or a built-in throws of itself does not count: nearly every one may, and nearly none does. Nor does a loop that never ends.
bool BuilderOfProgramFacts::mayLeave(FunctionID id, const BodyIndex& index, const BodyValue& value)
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
    // `+value` asks an object for a primitive.
    if (effectRuleOf(opcode) == EffectRule::Operator)
        return isHandedCode(0) && codeThatNobodyNamesMayLeave();
    if ((effectRuleOf(opcode) == EffectRule::Declared && declaredBoundOf(opcode).alwaysCompletes()) || opcode == Opcode::RequireBuiltinModule)
        return false;
    // No file may lie under a name that is made at run time.
    if (opcode == Opcode::RequireDynamic)
        return true;
    bool waits = opcode == Opcode::Await || opcode == Opcode::Yield || opcode == Opcode::InitialYield
        || (opcode == Opcode::IteratorResume && value.as<Opcode::IteratorResume>().iteratorKind() != IteratorKind::Sync)
        || (opcode == Opcode::IteratorClose && value.as<Opcode::IteratorClose>().iteratorKind() != IteratorKind::Sync);
    // A function that waits has returned to its caller.
    if (waits && m_source.at(id).isModuleBody() && (opcode != Opcode::Await || !findsNoThen(id, index, value.operand(0))))
        return true;
    if (opcode == Opcode::RequireModule || opcode == Opcode::EvaluateModule) {
        const Module& target = m_source.at(opcode == Opcode::RequireModule ? value.as<Opcode::RequireModule>().module() : value.as<Opcode::EvaluateModule>().module());
        if (target.flags.contains(ModuleFlags::ThrowsAtLoad))
            return true;
        return target.body && !isLibraryCode(m_source, m_source.at(target.body)) && bodyMayLeave(target.body);
    }
    if (accessClassOf(opcode) == AccessClass::ByName) {
        std::optional<uint8_t> receiver = receiverOf(opcode);
        ValueID object = receiver ? index.valueBehindCasts(value.operand(*receiver)) : ValueID();
        if (!object)
            return true;
        // A literal that is being filled: a definition runs no code. A read of it may run a getter that the literal lays: `({ get ready() { process.exit(0); } }).ready`.
        if (accessOf(opcode) == Access::Define && !index.isParameter(object) && valuesMadeInBodyInProgramFacts.contains(index.opcodeOf(object)))
            return false;
        BuiltinID holder = builtinReadByPath(index, object);
        // An accessor, a trap of a proxy.
        if (!holder)
            return codeThatNobodyNamesMayLeave();
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
    // A row may call what it is handed, or what it finds on it.
    return isHandedCode(1) && codeThatNobodyNamesMayLeave();
}

bool BuilderOfProgramFacts::bodyMayLeave(FunctionID id)
{
    const BodyIndex* index = m_inputs.bodies.find(id);
    if (!index || m_bodiesBeingAsked.set(id.index()))
        return true;
    bool leaves = false;
    for (BlockID number : index->blocksInDominatorOrder()) {
        BodyBlock block = index->block(number);
        leaves |= block.terminator().kind() == TerminatorKind::Throw;
        for (BodyValue value : block.values())
            leaves = leaves || mayLeave(id, *index, value);
        if (leaves)
            break;
    }
    m_bodiesBeingAsked.clear(id.index());
    return leaves;
}

// The modules that the first entry imports, each behind what it imports itself, and the CommonJS files among them. Whatever else is loaded is loaded by code, which may not run.
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
        bool isSurelyEvaluated = request.kind == ModuleRequestKind::Import ? request.phase == ImportPhase::Evaluation : !number && isFaceOfCommonJSFile(m_source, module);
        if (isSurelyEvaluated && !isVisited.quickSet(request.target.index()))
            stack.append({ request.target });
    }
    return order;
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

    bool waits = false;
    for (BlockID other : index.blocksInDominatorOrder()) {
        if (index.strictlyDominates(blockOfSite, other))
            continue;
        BodyBlock block = index.block(other);
        for (BodyValue value : block.values()) {
            if (value.id() == place)
                break;
            if (mayLeave(id, index, value))
                return false;
            waits |= value.opcode() == Opcode::Await;
        }
        TerminatorKind kind = block.terminator().kind();
        if (other != blockOfSite && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall || kind == TerminatorKind::Throw))
            return false;
    }
    // The loader runs a CommonJS file that an ES module imports while it loads the graph: ahead of every ES module, wherever the import stands.
    for (auto other = order.begin(); other != order.end(); ++other) {
        const Module& module = m_source.at(*other);
        // While a module waits the loader goes on with those that do not wait for it.
        if (other == position || (other > position && module.kind != ModuleKind::CommonJs && !waits))
            continue;
        if (module.flags.contains(ModuleFlags::ThrowsAtLoad))
            return false;
        if (module.body && !isLibraryCode(m_source, m_source.at(module.body)) && bodyMayLeave(module.body))
            return false;
    }
    return true;
}

// The names and values of a literal that the body writes and hands over as written. False: it is none, or something else has touched it meanwhile.
template<typename Functor>
bool BuilderOfProgramFacts::forEachPropertyOfWrittenLiteral(const BodyIndex& index, ValueID finished, ValueID handedOverAt, const Functor& functor) const
{
    finished = finished ? index.valueBehindCasts(finished) : ValueID();
    if (!finished || index.isParameter(finished))
        return false;
    ValueID literal;
    // A literal that is written where a map of names is taken has no layout and no end of its own.
    if (index.opcodeOf(finished) == Opcode::NewDictionary)
        literal = std::exchange(finished, handedOverAt);
    else if (index.opcodeOf(finished) == Opcode::FinishObject) {
        literal = index.valueBehindCasts(index.value(finished).operand(0));
        if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewObject)
            return false;
    }
    if (!literal || !finished || index.blockOf(literal) != index.blockOf(finished))
        return false;
    // What the allocation itself fills.
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
            touchesLiteral |= operand && index.valueBehindCasts(operand) == literal;
        });
        if (!touchesLiteral)
            continue;
        if (part.opcode() != Opcode::PropDefine || index.valueBehindCasts(part.operand(0)) != literal)
            return false;
        functor(part.as<Opcode::PropDefine>().key(), part.as<Opcode::PropDefine>().valueOperand());
    }
    return true;
}

const DataNode::Object* BuilderOfProgramFacts::objectOfConstantsBehind(const BodyIndex& index, ValueID value) const
{
    value = value ? index.valueBehindCasts(value) : ValueID();
    // Handed over as written: whoever else has it may have stored into it since.
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

// `Object.assign(Date.prototype, { getTime: f })`, `Object.defineProperties(Date.prototype, { getTime: { value: f } })`: a write under each name that the source brings.
// The literal says them where the body writes it, with what lies under each. Else the flow says which objects come: `Object.assign(Date.prototype, make())`. Else nobody can read the names.
// A copy stops no build: whether a name is still there when the copy runs is not asked.
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
    // A literal of constants alone is one operation.
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

// `action`: how the write is made, in the words of the rows. `brought`: the value, or the descriptor.
std::optional<UserDiagnosticKind> BuilderOfProgramFacts::refusalOf(const BodyIndex& index, const Witness& site, BuiltinID holder, PropertyKeyID key, BuiltinPropertyActionKind action, ValueID brought, bool throwsWhereRefused)
{
    const Builtin& object = m_builtins.at(holder);
    if (action == BuiltinPropertyActionKind::AssignsFrom || action == BuiltinPropertyActionKind::TakesDescriptorMap || action == BuiltinPropertyActionKind::DefinesFrom)
        return refusalOfCopy(index, site, holder, action, brought, throwsWhereRefused);
    if (!key) {
        // The names that the files of the tests spell are open, and the key may be one of them. The run says what it has refused.
        if (m_source.admitsReplacedBuiltins && !isFileOfProductInTestRun(m_source, FunctionID(site.function)))
            return std::nullopt;
        // An object that is locked by its names takes those that are open on it.
        if (object.flags.contains(BuiltinFlag::IsLockedObject))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        return object.flags.contains(BuiltinFlag::HasLockedMembers) ? std::optional(UserDiagnosticKind::LockedBuiltinWriteUnderUnreadName) : std::nullopt;
    }
    // A store under `__proto__` goes to the setter of Object.prototype.
    BuiltinID member = findOwnMember(m_source, holder, key);
    bool isLockedAsInExecutable = member && writeOpensNothingInTestRun(m_source, FunctionID(site.function), member);
    if ((!isLockedLocation(m_source, holder, key) && !isLockedAsInExecutable) || m_source.nameClassOf(key) == NameClass::Proto)
        return std::nullopt;

    bool isCertain = false;
    switch (action) {
    case BuiltinPropertyActionKind::StoresByKey:
        // The setter takes the store.
        if (member && m_builtins.at(member).kind == BuiltinKind::Accessor)
            return std::nullopt;
        if (BuiltinID stored = member ? builtinReadByPath(index, brought) : BuiltinID(); stored && m_builtins.canonicalID(stored) == m_builtins.canonicalID(member))
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
    // A test run goes on: the test files that load the statement fail at it, each by itself.
    return isCertain && !isLockedAsInExecutable && everyRunComesTo(site) ? UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns : UserDiagnosticKind::LockedBuiltinWriteMayRun;
}

std::optional<UserDiagnosticKind> BuilderOfProgramFacts::refusalOfWriteAt(const Witness& site, BuiltinID followed)
{
    const BodyIndex& index = *m_inputs.bodies.find(FunctionID(site.function));
    BodyValue write = index.value(ValueID(site.value));
    Opcode opcode = write.opcode();
    auto holderAt = [&](ValueID value) {
        BuiltinID holder = builtinReadByPath(index, value);
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

    BuiltinID callee = builtinReadByPath(index, write.operand(0));
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
        // `Object.assign(target, source)`: the one that is passed of as many as the row takes.
        if (operand->kind() == BuiltinOperand::Kind::ArgumentsFrom)
            return operand->argumentIndex() + 1 == arguments.size() ? arguments[operand->argumentIndex()] : ValueID();
        return operand->kind() == BuiltinOperand::Kind::Argument && operand->argumentIndex() < arguments.size() ? arguments[operand->argumentIndex()] : ValueID();
    };
    // Reflect answers false where it is refused. In a test run every refusal throws.
    bool throwsWhereRefused = m_builtins.at(callee).holder != Builtins::Reflect || m_inputs.build.options().admitsReplacedBuiltins();

    std::optional<UserDiagnosticKind> worst;
    for (const BuiltinOverload& overload : m_builtins.overloadsOf(callee)) {
        if (overload.mode != mode)
            continue;
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            BuiltinID holder = writesUnderKeyOfCallInProgramFacts(action) ? holderAt(passedAs(action.object)) : BuiltinID();
            if (!holder)
                continue;
            bool isCopy = action.kind == BuiltinPropertyActionKind::AssignsFrom || action.kind == BuiltinPropertyActionKind::TakesDescriptorMap || action.kind == BuiltinPropertyActionKind::DefinesFrom;
            if (action.key.kind != BuiltinKeyKind::Operand && !isCopy)
                continue;
            PropertyKeyID key;
            ValueID passedKey = passedAs(action.key.operand);
            passedKey = passedKey ? index.valueBehindCasts(passedKey) : ValueID();
            if (passedKey && !index.isParameter(passedKey) && index.opcodeOf(passedKey) == Opcode::String) {
                std::optional<PropertyKeyID> spelled = m_source.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index.value(passedKey).as<Opcode::String>().string() });
                if (!spelled)
                    continue;
                key = *spelled;
            }
            std::optional<UserDiagnosticKind> kind = refusalOf(index, site, holder, key, action.kind, passedAs(action.value.operand), throwsWhereRefused);
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
    // `const put = (object, key, value) => Object.defineProperty(object, key, { value }); put(Date.prototype, "getTime", f)`
    for (const WriteFollowedToBuiltin& write : m_inputs.linked.lookups.writesFollowedToBuiltins) {
        const BodyIndex* index = m_inputs.bodies.find(write.function);
        if (!index)
            continue;
        report(Witness { write.function.index(), write.site.index(), m_source.at(write.function).locationOf(index->value(write.site)) }, m_builtins.canonicalID(write.holder));
    }
    // The front end may emit one place in the source as several operations, as it does for the body of a `finally`.
    AOT::sort(diagnostics, { }, [](const UserDiagnostic& diagnostic) {
        return std::tuple(diagnostic.location.module, diagnostic.location.position, diagnostic.function, diagnostic.kind, diagnostic.value);
    });
    diagnostics.shrink(diagnostics.size() - std::ranges::unique(diagnostics, [](const UserDiagnostic& first, const UserDiagnostic& second) {
        return first.function == second.function && first.location.module == second.location.module && first.location.position == second.location.position;
    }).size());
    m_inputs.build.diagnostics().append(WTF::move(diagnostics));
}

// `require(process.env.NAME ?? "./a.js")`: the executable runs only what was compiled into it, so the load throws where the name is that of other code. The build says so where it can still be mended.
void BuilderOfProgramFacts::reportLoadsUnderNamesThatNobodyHasRead()
{
    // An image that admits the inspector refuses no code.
    if (!m_refused.codeOutsideImage || m_source.admitsInspector)
        return;
    Vector<UserDiagnostic> diagnostics;
    for (const Witness& site : m_loadsOfProgramByName) {
        const BodyIndex* index = m_inputs.bodies.find(FunctionID(site.function));
        if (!index || site.value == Wire::none)
            continue;
        BodyValue load = index->value(ValueID(site.value));
        if (!loadsByNameInProgramFacts.contains(load.opcode()))
            continue;
        // `import(new URL("./a.js", import.meta.url))`: the front end has read every name and answers each ahead of the load. The load remains only for a path that is spelled another way.
        if (load.opcode() == Opcode::ImportDynamic ? load.as<Opcode::ImportDynamic>().allNamesAreKnown() : load.as<Opcode::RequireDynamic>().allNamesAreKnown())
            continue;
        // The front end leaves a constant path to the run time where no such file exists at build time.
        ValueID name = index->valueBehindCasts(load.operand(0));
        if (!index->isParameter(name) && index->opcodeOf(name) == Opcode::String)
            continue;
        diagnostics.append(UserDiagnostic {
            .kind = UserDiagnosticKind::CodeOutsideImageIsRefused,
            .function = FunctionID(site.function),
            .block = index->blockOf(ValueID(site.value)),
            .value = ValueID(site.value),
            .location = site.location,
        });
    }
    // The front end may emit one place in the source as several operations, as it does for the body of a `finally`.
    AOT::sort(diagnostics, { }, [](const UserDiagnostic& diagnostic) {
        return std::tuple(diagnostic.location.module, diagnostic.location.position, diagnostic.function, diagnostic.value);
    });
    diagnostics.shrink(diagnostics.size() - std::ranges::unique(diagnostics, [](const UserDiagnostic& first, const UserDiagnostic& second) {
        return first.function == second.function && first.location.module == second.location.module && first.location.position == second.location.position;
    }).size());
    m_inputs.build.diagnostics().append(WTF::move(diagnostics));
}

// RuntimeUses::of_every_program() of src/aot/builtins/runtime_uses.rs in Bun.
bool BuilderOfProgramFacts::isHeldByEveryProgram(RuntimeFunctionID function) const
{
    if (!m_builtins.contains(function))
        return true;
    switch (m_builtins.at(function).role) {
    case RuntimeFunctionRole::EngineBuiltin:
    case RuntimeFunctionRole::CompilerFunction:
    case RuntimeFunctionRole::Model:
        return true;
    case RuntimeFunctionRole::HostBuiltin:
        return std::ranges::contains(m_builtins.functionsCalledWithStartupLoads(), function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A record stops the build, so none is written on a guess. Of the host a program holds what its files mention: section 2.5 of src/aot/CONTRACT-front.md in Bun.
// So a built-in that no operation names answers only for what every program holds, and a line that hangs on a condition, which nobody here has looked at, answers for nothing.
void BuilderOfProgramFacts::listLibraryCodeLacked()
{
    BitVector runtimeModulesWithRecord(m_builtins.moduleCount());
    for (const Module& module : m_source.modules) {
        if (module.runtimeModule)
            runtimeModulesWithRecord.set(module.runtimeModule.index());
    }

    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        BuiltinID builtin(index);
        if (m_builtins.canonicalID(builtin) != builtin || (m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        bool isNamed = m_isNamedByOperation.quickGet(index);
        for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(builtin)) {
            if (called.condition || !called.function)
                continue;
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_source, called.function); entry && entry->function)
                continue;
            if (m_builtins.contains(called.function) && !m_builtins.at(called.function).platforms.contains(targetOfThisBuild().platform))
                continue;
            if (isNamed || isHeldByEveryProgram(called.function))
                m_program.libraryFunctionsLacked.append(called.function);
        }
        if (!isNamed)
            continue;
        for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(builtin)) {
            if (load.condition || !load.module || runtimeModulesWithRecord.get(load.module.index()))
                continue;
            if (m_builtins.at(load.module).flags.contains(RuntimeModuleFlag::HasSource))
                m_program.libraryModulesWithoutRecord.append(load.module);
        }
    }
    AOT::sort(m_program.libraryFunctionsLacked);
    removeRepeatedElements(m_program.libraryFunctionsLacked);
    AOT::sort(m_program.libraryModulesWithoutRecord);
    removeRepeatedElements(m_program.libraryModulesWithoutRecord);
}

void BuilderOfProgramFacts::listLocaleUses()
{
    // Code that nobody has read names whatever locale it likes.
    for (const UnseenCodeRecord& record : m_program.unseenCode) {
        if (!admitsUnknownJavaScriptInProgramFacts(record.cause))
            continue;
        m_tables.localeUses.append(LocaleUse { LocaleUseKind::Unknown, { }, record.builtin == Wire::none ? BuiltinID() : BuiltinID(record.builtin), record.witness });
        return;
    }

    Vector<LocaleUse> uses = WTF::move(m_localeUsesAtCalls);
    BitVector rows = m_rows.takeLocales;
    rows.merge(m_rows.readLocalesWithoutDescription);
    for (size_t index : rows) {
        // Nobody looks at the operands of a call that Link finds: `(1234.5).toLocaleString(process.argv[2])`.
        bool callsAreSeen = !m_uses[index].contains(BuiltinUse::Escaped) && !m_isOutOfSight.quickGet(index) && !m_inputs.linked.lookups.builtinsCalled.get(index);
        bool isCalledWithoutDescription = m_rows.readLocalesWithoutDescription.quickGet(index) && m_uses[index].containsAny(callingUsesInProgramFacts);
        if (!callsAreSeen || isCalledWithoutDescription)
            uses.append(LocaleUse { LocaleUseKind::Unknown, { }, BuiltinID(static_cast<uint32_t>(index)), m_witnesses[index] });
    }

    auto subjectOf = [](const LocaleUse& use) {
        return std::tuple(use.kind, use.tag, use.builtin);
    };
    AOT::sort(uses, [&](const LocaleUse& a, const LocaleUse& b) {
        if (subjectOf(a) != subjectOf(b))
            return subjectOf(a) < subjectOf(b);
        return isLowerWitness(a.witness, b.witness);
    });
    removeRepeatedElements(uses, [&](const LocaleUse& a, const LocaleUse& b) {
        return subjectOf(a) == subjectOf(b);
    });
    m_tables.localeUses = WTF::move(uses);
}

// A match leaves a record of itself for `RegExp.$1` and its like: the members of the constructor that change on their own. builtinUses() lists whatever anybody may come by.
bool BuilderOfProgramFacts::mayReadRecordOfLastMatch() const
{
    BuiltinID constructor = m_builtins.at(BuiltinClasses::RegExp).constructor;
    if (!constructor || m_program.unseenCodeCanRun)
        return true;
    return std::ranges::any_of(m_builtins.membersOf(m_builtins.canonicalID(constructor)), [&](BuiltinID member) {
        const Builtin& entry = m_builtins.at(member);
        if (entry.mutability != BuiltinMutability::ChangesOnItsOwn)
            return false;
        PropertyKeyID key = entry.name ? m_source.keyOf(entry.name) : PropertyKeyID();
        return !m_uses[member.index()].isEmpty() || !m_uses[m_builtins.canonicalID(member).index()].isEmpty() || (key && m_namesRead.get(key.index()));
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
    // `value.constructor` is the constructor of Workers only once a Worker is made, and nobody makes the first without naming the constructor or a holder above it.
    // Code that nobody has read may name it. That is said by the records of unseen code, which whoever asks about Workers reads beside this one: here it would be a warning without a place in every test run.
    if (isOutOfSight && (hasFunction(lowestEscape) || isNamedByOperation))
        noteStartWithoutName(lowestEscape);
    if (m_isStartedWithoutName)
        m_program.workerStartWithoutName = m_startWithoutName;

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

    // Where no operation names the constructor, no URL of the program reaches it but through code that nobody has read, and workerStartWithoutName says that.
    Vector<Witness>& urls = m_program.urlsThatStartWorkers;
    if (isNamedByOperation)
        urls.appendVector(m_urlsThatLeaveBodies);
    auto placeOf = [](const Witness& witness) {
        return std::tuple(witness.function, witness.value);
    };
    AOT::sort(urls, { }, placeOf);
    urls.shrink(std::ranges::unique(urls, { }, placeOf).begin() - urls.begin());
}

// An instance is a realm in which compiled code of the image runs, with statics, tables and classes of its own. A yes is made true: reserveInstance() then refuses a second one in a VM.
// So the question is whether the program needs a second: the option says that it runs in several realms, or it may come to a built-in whose row says that it starts an instance.
// It says nothing of another VM. A Worker has a heap, a global object and a loader of its own, and no cell of it comes here.
bool BuilderOfProgramFacts::needsOneInstanceAtMost() const
{
    if (m_inputs.build.options().runsInSeveralRealms())
        return false;
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (!m_builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        // Code that nobody has seen has the global object, import() and process.getBuiltinModule(), so it finds every entry of the table.
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

// As noteWhatBodyMakes().
bool BuilderOfProgramFacts::knowsWhatBodyMakes(FunctionID id) const
{
    auto knowsFunctionObject = [&](const Function& function) {
        bool isAsync = function.has(FunctionFlags::IsAsync);
        if (function.has(FunctionFlags::IsGenerator))
            return m_classesWithObjects.quickGet((isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction).index());
        return !isAsync || m_classesWithObjects.quickGet(BuiltinClasses::AsyncFunction.index());
    };
    const Function& function = m_source.at(id);
    if (!knowsFunctionObject(function))
        return false;
    const ObjectTable& objects = m_source.types.objects();
    if (function.isCoroutine() && !m_classesWithObjects.quickGet(objects.at(objects.coroutineResultOf(id)).builtinClass.index()))
        return false;
    for (const SummaryNode& node : m_inputs.summaries.at(id).nodes) {
        if (node.kind == SummaryNode::Kind::ClassInstance ? !m_classesWithObjects.quickGet(node.first) : node.kind == SummaryNode::Kind::Closure && !knowsFunctionObject(m_source.at(FunctionID(node.first))))
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

// `export default { fetch }`: the host reads an export of an entry point and hands it to a built-in. No operation names that one.
// What an entry point exports may make the loader read the other names off another object: it awaits a module that exports `then`. So one name that is there stands for all.
void BuilderOfProgramFacts::useBuiltinsThatHostHandsExportsTo()
{
    for (const Entry& entry : m_source.entries) {
        const Module& module = m_source.at(entry.module);
        bool findsSomething = std::ranges::any_of(entry.hostReads, [&](const HostRead& read) {
            return mayExport(module, read.exportName);
        });
        for (const HostRead& read : entry.hostReads) {
            if (findsSomething && read.builtin)
                m_uses[m_builtins.canonicalID(read.builtin).index()].add(BuiltinUse::Called);
        }
    }
}

// Everything in it only adds, so it may be called again when more is reached.
void BuilderOfProgramFacts::settle()
{
    useBuiltinsThatHostHandsExportsTo();

    // Text that a debugger evaluates names whatever the realm holds, the loaders, the evaluators and the host's own among it, and does with it what it likes.
    if (m_source.admitsInspector) {
        add(m_program.unseenCode, UnseenCodeCause::OptionAdmitsCode, { }, noWitness);
        m_uses.fill(everyBuiltinUseInProgramFacts, m_uses.size());
    } else {
        // The loader calls what the program has laid in the place of its resolver or of its `require`, on the object that holds the place.
        for (BuiltinMark mark : { BuiltinMark::IsResolverThatLoaderCalls, BuiltinMark::IsRequireThatLoaderCalls }) {
            for (const BuiltinMarkedEntry& marked : m_builtins.marked(mark)) {
                BuiltinID member = marked.builtin;
                const LookupsPastProgramObjects& lookups = m_inputs.linked.lookups;
                PropertyKeyID key = m_source.keyOf(m_builtins.at(member).name);
                bool isWritten = lookups.membersWritten.get(member.index()) || lookups.writesUnderAnyNameThroughUnseenByProgram || (key && lookups.namesWrittenThroughUnseenByProgram.get(key.index()));
                // A value that nobody knows is the holder only where something has come by the holder.
                BuiltinID holder = m_builtins.canonicalID(m_builtins.at(member).holder);
                if (isWritten && holder && !m_uses[holder.index()].isEmpty())
                    letGo(holder, noWitness);
            }
        }
        // Under a name that the table lists nowhere on the way up lies nothing, or what the program has laid above: an accessor, whose getter has the object for its `this`.
        for (const Finding& read : m_readsOffObjectsOfModulesThatNoMemberAnswers) {
            if (m_namesThatRowsLayOnBuiltins.get(read.key))
                letGo(m_builtins.canonicalID(BuiltinID(read.subject)), read.witness);
        }
        // The names have grown since these were looked through.
        for (size_t index : m_isLookedThrough)
            m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
        lookThroughPrototypesOfClassesWithObjects();
    }
    listBuiltinsThatLetCodeIn();
    listLoadsOfUnseenCode();

    // What is withheld is withheld from a program in which nothing else is open. Behind the first cause every name that reached code spells finds an event, and an event goes out of sight with its holder.
    if (m_eventsAreWithheld && !m_source.admitsInspector && !m_program.unseenCode.isEmpty()) {
        m_eventsAreWithheld = false;
        for (size_t index : m_isLetGo)
            m_holdersToSpreadBelow.append(BuiltinID(static_cast<uint32_t>(index)));
        for (size_t index : m_isLookedThrough)
            m_holdersToLookThrough.append(BuiltinID(static_cast<uint32_t>(index)));
        lookThroughPrototypesOfClassesWithObjects();
        listBuiltinsThatLetCodeIn();
        listLoadsOfUnseenCode();
    }

    // Such code has the global object. What that lets in besides only adds records that admit code.
    if (hasRecordThat(admitsUnknownJavaScriptInProgramFacts) && !m_source.admitsInspector) {
        letGo(m_globalObject, noWitness);
        if (!std::exchange(m_objectsOfAnyClassMayExist, true))
            m_classesWithObjectsHaveGrown = true;
        lookThroughPrototypesOfClassesWithObjects();
        listBuiltinsThatLetCodeIn();
    }
}

void BuilderOfProgramFacts::takeBuiltinsThatCameIntoReach(Vector<BuiltinID>& list)
{
    for (uint32_t index = 0; index < m_uses.size(); ++index) {
        BuiltinID builtin(index);
        if (m_builtins.canonicalID(builtin) != builtin || (m_uses[index] - OptionSet<BuiltinUse> { BuiltinUse::Tested }).isEmpty())
            continue;
        if (m_hasComeIntoReach.quickSet(index))
            continue;
        list.append(builtin);
        const Builtin& entry = m_builtins.at(builtin);
        bool givesSymbolInUse = std::ranges::any_of(m_builtins.overloadsOf(builtin), [&](const BuiltinOverload& overload) {
            if (overload.isUndeclared() || !overload.result.type)
                return overload.isUndeclared();
            const BuiltinType& given = m_builtins.at(overload.result.type);
            if (given.isAnything())
                return entry.origin == BuiltinOrigin::Host;
            if (!given.primitives.contains(BuiltinPrimitive::Symbol) || given.freshness == BuiltinFreshness::Yes)
                return false;
            // A symbol that was handed in is in hand already. What a read finds on an object of the host was handed in by nobody.
            auto mayBringSymbol = [&](const BuiltinParameter& parameter) {
                const BuiltinType& accepted = m_builtins.at(parameter.accepted);
                return accepted.mayBeObject() || accepted.primitives.contains(BuiltinPrimitive::Symbol);
            };
            if (overload.mode == BuiltinCallMode::Get)
                return true;
            return !(overload.receiver && mayBringSymbol(*overload.receiver)) && std::ranges::none_of(overload.parameters, mayBringSymbol);
        });
        m_objectsMayHaveMethodCalledWithStartupLoads |= givesSymbolInUse || m_builtins.has(BuiltinMark::InstancesHaveMethodCalledWithStartupLoads, builtin)
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
    take(m_inputs.linked.lookups);
    for (uint32_t index = 0; index < m_source.functions.size(); ++index) {
        if (isReached(FunctionID(index)))
            noteReached(FunctionID(index));
    }
    followModules();
    settle();
    listLoadsOfMachineCode();

    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin);
    };
    AOT::sort(m_program.unseenCode, { }, orderOf);
    AOT::sort(m_program.loadsOfMachineCode, { }, orderOf);
    m_program.unseenCodeCanRun = hasRecordThat(admitsUnknownJavaScriptInProgramFacts);
    m_program.unseenScriptsCanRun = hasRecordThat(admitsScriptsInProgramFacts);
    m_program.storesPathOfMainModule = frontEndLeavesMainModuleOpen();
    m_program.hasOneInstance = needsOneInstanceAtMost();

    listBuiltinUses();
    listHoldersWhoseKeysAreNeverListed();
    lockMembersThatNobodyWrites();
    reportWritesThatLocksRefuse();
    reportLoadsUnderNamesThatNobodyHasRead();
    m_tables.featureUses = FeaturesOfProgramFacts(m_inputs, m_refused).run(m_tables.builtinUses.span());
    m_program.mayReadLegacyRegExpStatics = mayReadRecordOfLastMatch();
    m_program.matchesRegExps = m_program.unseenCodeCanRun || std::ranges::contains(m_tables.featureUses, static_cast<uint32_t>(RemovableFeature::RegExp), &FeatureUseRecord::feature);
    listLibraryCodeLacked();
    listLocaleUses();
    listWorkers();
}

void computeFactsOfProgram(const InputsOfProgramFacts& inputs, FactTables& tables)
{
    BuilderOfProgramFacts(inputs, tables).run();
}

// ---- While Link still finds what is reached

BuiltinsInReach::BuiltinsInReach(const Program& program, const LooksAtBodies& looks, const ThinSummaries& summaries, BuildContext& build)
    : m_tables(makeUniqueWithoutFastMallocCheck<FactTables>())
    , m_linked(makeUniqueWithoutFastMallocCheck<LinkTables>())
    , m_inputs(makeUniqueWithoutFastMallocCheck<InputsOfProgramFacts>(InputsOfProgramFacts { program, looks.bodies, looks, summaries, *m_linked, build }))
    , m_builder(makeUniqueWithoutFastMallocCheck<BuilderOfProgramFacts>(*m_inputs, *m_tables))
{
    // From the members up: an object leads to what lies on it.
    const BuiltinTable& builtins = program.builtins();
    m_builtinsWorthFollowingToWrites.ensureSize(builtins.builtinCount());
    // In a test run the image opens what a write was followed to, also where the program can lay no lock of its own: a member of %TypedArray%.prototype, a new name on %GeneratorFunction.prototype%.
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

void BuiltinsInReach::noteWhatBodyMakes(FunctionID id)
{
    m_wasToldSomethingNew = true;
    m_builder->noteObjectsThatBodyMakes(id);
}

void BuiltinsInReach::noteReached(FunctionID id)
{
    m_wasToldSomethingNew = true;
    m_builder->noteReached(id);
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

void BuiltinsInReach::noteNameCalled(PropertyKeyID key)
{
    if (m_linked->lookups.namesCalled.set(key.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameCalled(key);
}

void BuiltinsInReach::noteNameSpelledByProgram(PropertyKeyID key)
{
    if (m_linked->lookups.namesSpelledByProgram.set(key.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteNameSpelledByProgram(key);
}

void BuiltinsInReach::noteReadUnderKeyNobodyKnows()
{
    if (std::exchange(m_linked->lookups.readsUnderKeysNobodyKnows, true))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteReadUnderKeyNobodyKnows();
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

void BuiltinsInReach::noteLetGo(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsLetGo.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteLossOf(builtin);
}

void BuiltinsInReach::noteCalledOutOfSight(BuiltinID builtin)
{
    if (m_linked->lookups.builtinsCalledOutOfSight.set(builtin.index()))
        return;
    m_wasToldSomethingNew = true;
    m_builder->noteCallOutOfSight(builtin);
}

bool BuiltinsInReach::mayBeWritten(BuiltinID canonical) const
{
    const LookupsPastProgramObjects& lookups = m_linked->lookups;
    return lookups.holdersWritten.get(canonical.index()) || m_builder->isWrittenInSight(canonical) || lookups.writesUnderAnyNameThroughUnseenByProgram || lookups.writesUnderAnyNameThroughLostBuiltins
        || !lookups.namesWrittenThroughUnseenByProgram.isEmpty() || !lookups.namesWrittenThroughUnseenByLibrary.isEmpty() || !lookups.namesWrittenThroughLostBuiltins.isEmpty();
}

void BuiltinsInReach::noteWrite(BuiltinID holder, PropertyKeyID key)
{
    const Program& program = m_inputs->program;
    holder = program.builtins().canonicalID(holder);
    m_wasToldSomethingNew |= !m_linked->lookups.holdersWritten.set(holder.index());
    if (!key) {
        m_wasToldSomethingNew |= !m_linked->lookups.holdersWrittenUnderAnyName.set(holder.index());
        return;
    }
    if (BuiltinID member = findOwnMember(program, holder, key))
        m_wasToldSomethingNew |= !m_linked->lookups.membersWritten.set(member.index());
}

void BuiltinsInReach::noteWriteThroughLostBuiltins(PropertyKeyID key)
{
    LookupsPastProgramObjects& lookups = m_linked->lookups;
    m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughLostBuiltins.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughLostBuiltins, true);
}

void BuiltinsInReach::noteNameWrittenThroughUnseenByLibrary(PropertyKeyID key)
{
    m_wasToldSomethingNew |= !m_linked->lookups.namesWrittenThroughUnseenByLibrary.set(key.index());
}

void BuiltinsInReach::noteWriteThroughUnseenByProgram(PropertyKeyID key)
{
    LookupsPastProgramObjects& lookups = m_linked->lookups;
    m_wasToldSomethingNew |= key ? !lookups.namesWrittenThroughUnseenByProgram.set(key.index()) : !std::exchange(lookups.writesUnderAnyNameThroughUnseenByProgram, true);
}

void BuiltinsInReach::noteReadThroughLostBuiltins(PropertyKeyID key)
{
    m_wasToldSomethingNew |= !m_linked->lookups.namesReadThroughLostBuiltins.set(key.index());
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

void BuiltinsInReach::noteInstanceAtUnseen(BuiltinClassID builtinClass)
{
    m_wasToldSomethingNew |= !m_linked->lookups.classesWithInstancesAtUnseen.set(builtinClass.index());
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
}

// It goes by the sets as they stood at the last settle(). What was told since is looked at there.
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
    return std::exchange(m_linked->lookups, { });
}

bool BuiltinsInReach::loaderOfHostMayRun() const
{
    return m_builder->hasRecordOfUnseenCode();
}

bool BuiltinsInReach::objectsMayHaveMethodCalledWithStartupLoads() const
{
    return m_builder->objectsMayHaveMethodCalledWithStartupLoads();
}

bool BuiltinsInReach::unseenCodeCanRun() const
{
    return m_builder->unseenCodeCanRun();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
