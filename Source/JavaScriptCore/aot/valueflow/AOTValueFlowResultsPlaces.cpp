/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsPlaces.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTFlowUnseenCode.h"
#include "AOTRemovableFeature.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "IntlObject.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static Witness witnessAt(const Function& function, FunctionID id, ValueID value)
{
    return { id.index(), value.index(), function.locationOf(value) };
}

// Whoever reads a report looks for the first place in the text.
static void keepEarlierPlace(Witness& current, const Witness& candidate)
{
    auto orderOf = [](const Witness& place) {
        return std::tuple(place.location.module, place.location.position, place.function, place.value);
    };
    if (hasFunction(candidate) && (!hasFunction(current) || orderOf(candidate) < orderOf(current)))
        current = candidate;
}

// Nobody who reads a report has written it: a function of the library, and the body that the build makes for a file that holds no code, as for a native addon.
static bool isOfLibrary(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::Model)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::Data;
}

static bool callsValue(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    default:
        return false;
    }
}

namespace {

struct EntryIntoLibrary {
    FunctionID entered;
    FunctionID from;
    Witness witness;
};

struct FoundInFunctions {
    Vector<std::pair<BuiltinID, Witness>> namedInProgram;
    Vector<std::pair<BuiltinID, Witness>> namedInLibrary;
    Vector<EntryIntoLibrary> entries;
    Witness readUnderKeyNobodyKnows { noWitness };
};

} // anonymous namespace

ValueFlowPlacesInProgram::ValueFlowPlacesInProgram(const ValueFlowResultInputs& inputs, const BitVector& builtinsAskedAbout)
    : m_program(inputs.program)
    , m_namedInProgram(FillWith { }, inputs.program.builtins().builtinCount(), noWitness)
    , m_namedInLibrary(FillWith { }, inputs.program.builtins().builtinCount(), noWitness)
    , m_hasEscaped(inputs.program.builtins().builtinCount())
    , m_wayInto(FillWith { }, inputs.program.functions.size(), noWitness)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    const ValueSets& sets = inputs.sets();

    ValueSetMakers makers(inputs);
    Vector<ValueFlowObjectID> objects;
    for (size_t index : builtinsAskedAbout) {
        Vector<ValueFlowObjectID, 32> objectsOfBuiltin;
        universe.appendObjectsOf(BuiltinID(static_cast<uint32_t>(index)), objectsOfBuiltin);
        objects.appendVector(objectsOfBuiltin);
    }
    ValueSetID objectsAskedAbout = makers.of(0).of(objects.span());
    objects.shrink(0);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (isOfLibrary(program, program.functions[index]) && solver.isReached(FunctionID(index)))
            objects.append(universe.closureOf(FunctionID(index)));
    }
    ValueSetID closuresOfLibrary = makers.of(0).of(objects.span());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (solver.usesOf(BuiltinID(index)).contains(BuiltinUse::Escaped))
            m_hasEscaped.quickSet(builtins.canonicalID(BuiltinID(index)).index());
    }

    constexpr uint32_t functionsPerTask = 64;
    uint32_t numberOfFunctions = program.functions.size();
    Vector<FoundInFunctions> foundInTask((numberOfFunctions + functionsPerTask - 1) / functionsPerTask);
    forEachIndexInParallel(inputs.loops(), foundInTask.size(), [&](unsigned task, unsigned worker) {
        FoundInFunctions& found = foundInTask[task];
        ValueSetMaker& maker = makers.of(worker);
        for (uint32_t index = task * functionsPerTask; index < std::min(numberOfFunctions, (task + 1) * functionsPerTask); ++index) {
            FunctionID id(index);
            if (!solver.isReached(id))
                continue;
            const Function& function = program.functions[index];
            bool isOfProgram = isWrittenInProgram(function) && !isOfLibrary(program, function);
            if (!isOfProgram && !isOfLibrary(program, function))
                continue;

            auto noteBuiltinsIn = [&](ValueSetID held, ValueID place) {
                if (held == ValueSets::nothing || ValueSets::isComplement(held) || !sets.meet(held, objectsAskedAbout))
                    return;
                sets.forEachMember(maker.intersect(held, objectsAskedAbout), [&](ValueFlowObjectID object) {
                    (isOfProgram ? found.namedInProgram : found.namedInLibrary).append({ builtins.canonicalID(universe.builtinOf(object)), witnessAt(function, id, place) });
                });
            };
            auto noteEntryInto = [&](FunctionID entered, const Witness& witness) {
                if (entered && entered != id && isOfLibrary(program, program.at(entered)))
                    found.entries.append({ entered, id, witness });
            };
            // Whoever asks for a module runs its body, and names the namespace object of a module of the run time.
            auto noteRequestFor = [&](ModuleID module, const Witness& witness) {
                if (!module)
                    return;
                noteEntryInto(program.at(module).body, witness);
                BuiltinID namespaceObject = program.at(module).runtimeModule ? builtins.at(program.at(module).runtimeModule).namespaceObject : BuiltinID();
                if (namespaceObject && builtinsAskedAbout.get(builtins.canonicalID(namespaceObject).index()))
                    (isOfProgram ? found.namedInProgram : found.namedInLibrary).append({ builtins.canonicalID(namespaceObject), witness });
            };

            for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
                if (statement.kind == ValueFlowStatementKind::ModuleEvaluation) {
                    for (const ModuleRequest& request : program.at(universe.moduleWithBody(id)).requests)
                        noteRequestFor(request.target, { index, Wire::none, function.declarationLocation() });
                    continue;
                }
                if (statement.kind != ValueFlowStatementKind::Operation || !statement.site)
                    continue;

                const Value& operation = function.at(statement.site);
                std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
                if (statement.result)
                    noteBuiltinsIn(solver.valueOf(statement.result), statement.site);
                for (uint32_t position = 0; position < operands.size(); ++position) {
                    if (!operands[position].constant)
                        continue;
                    ValueID operand = position < operation.numberOfOperands() ? operation.operand(position) : ValueID();
                    noteBuiltinsIn(operands[position].constant, operand ? operand : statement.site);
                }

                Witness witness = witnessAt(function, id, statement.site);
                switch (statement.opcode) {
                case Opcode::ElemGet: {
                    if (!isOfProgram)
                        break;
                    ValueSetID receivers = inputs.valueOf(maker, operands[*OperandPosition::object(statement.opcode)]);
                    ValueSetID keys = inputs.valueOf(maker, operands[*OperandPosition::property(statement.opcode)]);
                    if (sets.contains(receivers, ValueFlowUniverse::unknownObject()) && sets.contains(keys, ValueFlowUniverse::primitive(PrimitiveSet::String)))
                        keepEarlierPlace(found.readUnderKeyNobodyKnows, witness);
                    break;
                }
                case Opcode::CallDirect:
                    noteEntryInto(operation.function(), witness);
                    break;
                case Opcode::ModuleNamespace:
                case Opcode::ImportModule:
                case Opcode::RequireModule:
                    noteRequestFor(operation.module(), witness);
                    break;
                default:
                    break;
                }
                if (!callsValue(statement.opcode))
                    continue;
                ValueSetID callees = ValueFlowRuleContext::valueShownToRules(operands[0], solver, maker);
                if (ValueSets::isComplement(callees) || !sets.meet(callees, closuresOfLibrary))
                    continue;
                sets.forEachMember(maker.intersect(callees, closuresOfLibrary), [&](ValueFlowObjectID callee) {
                    noteEntryInto(universe.closureFunction(callee), witness);
                });
            }
        }
    });

    Vector<EntryIntoLibrary> entriesFromLibrary;
    Vector<FunctionID> entered;
    auto enter = [&](FunctionID function, const Witness& witness) {
        if (!hasFunction(m_wayInto[function.index()]))
            entered.append(function);
        keepEarlierPlace(m_wayInto[function.index()], witness);
    };
    for (FoundInFunctions& found : foundInTask) {
        for (auto& [builtin, witness] : found.namedInProgram)
            keepEarlierPlace(m_namedInProgram[builtin.index()], witness);
        for (auto& [builtin, witness] : found.namedInLibrary)
            keepEarlierPlace(m_namedInLibrary[builtin.index()], witness);
        keepEarlierPlace(m_readUnderKeyNobodyKnows, found.readUnderKeyNobodyKnows);
        for (const EntryIntoLibrary& entry : found.entries) {
            if (isOfLibrary(program, program.at(entry.from)))
                entriesFromLibrary.append(entry);
            else
                enter(entry.entered, entry.witness);
        }
        found = { };
    }

    // Breadth first, so that the nearest statement of the program speaks for a function. All that one round enters is entered before the next round begins: among equals the first place in the text stays.
    AOT::sort(entriesFromLibrary, { }, [](const EntryIntoLibrary& entry) {
        return std::tuple(entry.from, entry.entered);
    });
    while (!entered.isEmpty()) {
        Vector<FunctionID> enteredBefore = std::exchange(entered, { });
        BitVector wasEntered(numberOfFunctions);
        for (uint32_t index = 0; index < numberOfFunctions; ++index) {
            if (hasFunction(m_wayInto[index]))
                wasEntered.quickSet(index);
        }
        for (FunctionID from : enteredBefore) {
            for (const EntryIntoLibrary& entry : std::ranges::equal_range(entriesFromLibrary, from, { }, &EntryIntoLibrary::from)) {
                if (!wasEntered.quickGet(entry.entered.index()))
                    enter(entry.entered, m_wayInto[from.index()]);
            }
        }
    }
}

Witness ValueFlowPlacesInProgram::wayTo(const Witness& place) const
{
    if (!hasFunction(place) || !isOfLibrary(m_program, m_program.at(FunctionID(place.function))))
        return place;
    return m_wayInto[place.function];
}

Witness ValueFlowPlacesInProgram::whereNamed(BuiltinID builtin) const
{
    if (hasFunction(m_namedInProgram[builtin.index()]))
        return m_namedInProgram[builtin.index()];
    if (Witness way = wayTo(m_namedInLibrary[builtin.index()]); hasFunction(way))
        return way;
    return m_hasEscaped.quickGet(builtin.index()) ? m_readUnderKeyNobodyKnows : noWitness;
}

namespace {

// A set knows a string by its kind alone unless the text is a key.
class LocalesThatCallsName {
    WTF_MAKE_NONCOPYABLE(LocalesThatCallsName);
public:
    explicit LocalesThatCallsName(const Program& program)
        : m_program(program)
    {
    }

    // `passed` is none for an argument that the call does not pass.
    void add(BuiltinID builtin, FunctionID caller, ValueID call, ValueID passed)
    {
        const Function& function = m_program.at(caller);
        if (!passed) {
            uses.append({ LocaleUseKind::HostDefault, { }, builtin, witnessAt(function, caller, call) });
            return;
        }
        m_builtin = builtin;
        // A parameter is written nowhere.
        m_witness = witnessAt(function, caller, hasModule(function.locationOf(passed)) ? passed : call);
        m_caller = caller;
        m_call = call;
        m_visited.clear();
        visit(caller, passed);
        while (!m_worklist.isEmpty()) {
            auto [currentFunction, currentValue] = m_worklist.takeLast();
            follow(currentFunction, currentValue);
        }
    }

    void addUnknown(BuiltinID builtin, const Witness& witness)
    {
        uses.append({ LocaleUseKind::Unknown, { }, builtin, witness });
    }

    Vector<FlowLocaleUse> uses;

private:
    static constexpr unsigned maxValuesFollowed = 256;

    void note(LocaleUseKind kind, StringID tag = { })
    {
        uses.append({ kind, tag, m_builtin, m_witness });
    }

    // A text that is no tag names no language. What the built-in makes of it nobody has said: Intl.supportedValuesOf("collation") opens the data of every language.
    void noteText(StringID text)
    {
        Vector<char16_t> buffer;
        if (isStructurallyValidLanguageTag(m_program.strings.viewOf(text, buffer)))
            note(LocaleUseKind::Constant, text);
        else
            note(LocaleUseKind::Unknown);
    }

    void visit(FunctionID function, ValueID value)
    {
        if (!value || m_visited.size() >= maxValuesFollowed) {
            note(LocaleUseKind::Unknown);
            return;
        }
        if (m_visited.add(static_cast<uint64_t>(function.index()) << 32 | value.index()).isNewEntry)
            m_worklist.append({ function, value });
    }

    void follow(FunctionID id, ValueID value)
    {
        const Function& function = m_program.at(id);
        const Value& definition = function.at(value);
        switch (definition.opcode) {
        case Opcode::String:
            noteText(definition.string());
            return;
        case Opcode::Undefined:
            note(LocaleUseKind::HostDefault);
            return;
        case Opcode::Data:
            followList(id, function, value);
            return;
        case Opcode::Parameter:
            followParameter(id, function, value);
            return;
        case Opcode::GetModuleVariable:
            followConstantOfModule(definition.variable());
            return;
        case Opcode::CheckInitialized:
            visit(id, definition.operand(0));
            return;
        default:
            if (isCast(definition.opcode))
                visit(id, definition.operand(0));
            else
                note(LocaleUseKind::Unknown);
            return;
        }
    }

    // The operation makes a new array each time, and the call is the only one that ever has it: nothing is put into it behind the texts that are written out.
    bool isHandedToCallAlone(FunctionID id, const Function& function, ValueID list) const
    {
        if (id != m_caller)
            return false;
        CFG cfg(function);
        UseLists useLists(function, cfg);
        for (ValueID current = list;;) {
            std::optional<Use> use = useLists.singleUse(current);
            if (!use || use->kind != Use::Kind::Operand)
                return false;
            if (use->user == m_call)
                return true;
            if (!isCast(function.at(use->user).opcode))
                return false;
            current = use->user;
        }
    }

    void followList(FunctionID id, const Function& function, ValueID value)
    {
        const DataNode::Array* list = m_program.at(function.at(value).data()).root.tryAs<DataNode::Array>();
        bool holdsOnlyTexts = list && std::ranges::all_of(list->elements, [](const DataNode& element) {
            return element.is<DataNode::String>();
        });
        if (!holdsOnlyTexts || !isHandedToCallAlone(id, function, value)) {
            note(LocaleUseKind::Unknown);
            return;
        }
        // An empty list asks for the default locale.
        if (list->elements.isEmpty())
            note(LocaleUseKind::HostDefault);
        for (const DataNode& element : list->elements)
            noteText(element.as<DataNode::String>().value);
    }

    void followParameter(FunctionID id, const Function& function, ValueID parameter)
    {
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            size_t index = function.blocks[block].parameters.find(parameter);
            if (index == notFound)
                continue;
            if (BlockID(block) == entryBlock)
                break;
            CFG cfg(function);
            for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(index));
                if (incoming.kind == IncomingValue::Kind::Argument)
                    visit(id, incoming.argument);
                else
                    note(LocaleUseKind::Unknown);
            }
            return;
        }
        note(LocaleUseKind::Unknown);
    }

    // `const locale = "de"` at the top of a module has one store, which stands in the body of the module.
    void followConstantOfModule(VariableID id)
    {
        const Variable& variable = m_program.at(id);
        FunctionID body = variable.flags.contains(VariableFlags::IsImmutable) && variable.module ? m_program.at(variable.module).body : FunctionID();
        if (!body || !m_program.at(body).hasBody()) {
            note(LocaleUseKind::Unknown);
            return;
        }
        ValueID stored;
        unsigned numberOfStores = 0;
        for (const Value& store : m_program.at(body).values) {
            if (store.opcode != Opcode::PutModuleVariable || store.variable() != id)
                continue;
            ++numberOfStores;
            stored = store.valueOperand();
        }
        if (numberOfStores == 1)
            visit(body, stored);
        else
            note(LocaleUseKind::Unknown);
    }

    const Program& m_program;
    BuiltinID m_builtin;
    Witness m_witness { noWitness };
    FunctionID m_caller;
    ValueID m_call;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_visited;
    Vector<std::pair<FunctionID, ValueID>, 16> m_worklist;
};

} // anonymous namespace

// The data that ICU keeps by language belongs to these: src/trim/features/icu-data.def in Bun.
static bool readsDataOfLocales(RemovableFeature feature)
{
    switch (feature) {
    case RemovableFeature::IntlCollation:
    case RemovableFeature::IntlNumberFormat:
    case RemovableFeature::IntlDateTimeFormat:
    case RemovableFeature::IntlUnits:
    case RemovableFeature::IntlSegmenter:
    case RemovableFeature::IntlDisplayNames:
    case RemovableFeature::IntlOther:
        return true;
    default:
        return false;
    }
}

Vector<FlowLocaleUse> collectLocaleUses(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& builtinsWhoseCodeRuns, std::span<const UnseenCodeRecord> unseenCode)
{
    // Code that the build has not seen names any locale.
    for (const UnseenCodeRecord& record : unseenCode) {
        if (admitsUnknownJavaScript(record.cause))
            return { FlowLocaleUse { LocaleUseKind::Unknown, { }, record.builtin == Wire::none ? BuiltinID() : BuiltinID(record.builtin), record.witness } };
    }

    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    uint32_t numberOfBuiltins = builtins.builtinCount();
    LocalesThatCallsName locales(program);

    // By canonical entry: where a call passes a locale. Which overload a call selects depends on its operands, so every overload counts.
    Vector<Vector<BuiltinOperand, 1>> localeOperands(numberOfBuiltins);
    BitVector takesLocale(numberOfBuiltins);
    BitVector hasOverloadWithoutRow(numberOfBuiltins);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        BuiltinID builtin = builtins.canonicalID(BuiltinID(index));
        if (!builtinsWhoseCodeRuns.quickGet(builtin.index()))
            continue;
        for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index))) {
            // Nobody has said what native code without a row does with its operands. It gets at the data of a language only through a feature that its entry names.
            if (overload.isUndeclared() && std::ranges::any_of(builtins.featureUsesOf(builtin), readsDataOfLocales, &BuiltinFeatureUse::feature) && !hasOverloadWithoutRow.quickSet(builtin.index())) {
                takesLocale.quickSet(builtin.index());
                locales.addUnknown(builtin, noWitness);
            }
            for (const BuiltinParameter& parameter : overload.parameters) {
                if (!parameter.roles.contains(BuiltinOperandRole::Locale))
                    continue;
                takesLocale.quickSet(builtin.index());
                localeOperands[builtin.index()].appendIfNotContains(parameter.operand);
            }
        }
    }
    if (takesLocale.isEmpty())
        return { };

    BitVector entriesThatTakeLocale(numberOfBuiltins);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        BuiltinID builtin = builtins.canonicalID(BuiltinID(index));
        if (!takesLocale.quickGet(builtin.index()))
            continue;
        entriesThatTakeLocale.quickSet(index);
        // Whoever was handed the function calls it with operands that nobody sees.
        ValueFlowObjectID object = inputs.universe().objectOf(BuiltinID(index));
        if (inputs.solver().usesOf(BuiltinID(index)).contains(BuiltinUse::Escaped) || (object && (inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object))))
            locales.addUnknown(builtin, noWitness);
    }

    BitVector isApplied(numberOfBuiltins);
    forEachBuiltinApplication(inputs, heap, entriesThatTakeLocale, [&](const ValueFlowBuiltinApplication& application) {
        BuiltinID builtin = builtins.canonicalID(application.builtin);
        isApplied.quickSet(builtin.index());
        // What all calls of one set of callees share, and a call by a protocol, has no place in the program and no operand that anybody can read.
        if (!application.site) {
            locales.addUnknown(builtin, noWitness);
            return;
        }
        const Function& function = program.at(application.function);
        const Value& call = function.at(application.site);
        bool spellsOutArguments = call.opcode == Opcode::Call || call.opcode == Opcode::Construct;
        for (BuiltinOperand operand : localeOperands[builtin.index()]) {
            if (!spellsOutArguments || operand.kind() != BuiltinOperand::Kind::Argument) {
                locales.addUnknown(builtin, witnessAt(function, application.function, application.site));
                continue;
            }
            std::span<const ValueID> arguments = call.variadicOperands();
            locales.add(builtin, application.function, application.site, operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : ValueID());
        }
    });
    // Its code runs, and nobody has seen the call.
    for (size_t index : takesLocale) {
        if (!isApplied.quickGet(index))
            locales.addUnknown(BuiltinID(static_cast<uint32_t>(index)), noWitness);
    }

    // One record for each built-in: whoever writes the report picks the one that has a place.
    auto orderOf = [](const FlowLocaleUse& use) {
        return std::tuple(use.kind, use.tag, use.builtin, !hasFunction(use.witness), use.witness.function, use.witness.value);
    };
    AOT::sort(locales.uses, { }, orderOf);
    locales.uses.shrink(std::ranges::unique(locales.uses, [](const FlowLocaleUse& a, const FlowLocaleUse& b) {
        return a.kind == b.kind && a.tag == b.tag && a.builtin == b.builtin;
    }).begin() - locales.uses.begin());
    return WTF::move(locales.uses);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
