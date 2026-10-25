/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsPlaces.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTCFG.h"
#include "AOTFlowSiteData.h"
#include "AOTFlowUnseenCode.h"
#include "AOTKnownClosures.h"
#include "AOTRemovableFeature.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTValueIdentity.h"
#include "IntlObject.h"
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static Witness witnessAt(const Function& function, FunctionID id, ValueID value)
{
    return { id.index(), value.index(), function.locationOf(value) };
}

static void keepEarlierPlace(Witness& current, const Witness& candidate)
{
    auto orderOf = [](const Witness& place) {
        return std::tuple(place.location.module, place.location.position, place.function, place.value);
    };
    if (hasFunction(candidate) && (!hasFunction(current) || orderOf(candidate) < orderOf(current)))
        current = candidate;
}

static bool isOfLibrary(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::Model)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::Data;
}

ValueFlowPlacesInProgram::ValueFlowPlacesInProgram(const ValueFlowResultInputs& inputs, const BitVector& builtinsAskedAbout)
    : m_program(inputs.program)
    , m_namedInProgram(FillWith { }, inputs.program.builtins().builtinCount(), noWitness)
    , m_namedInLibrary(m_namedInProgram)
    , m_hasEscaped(inputs.program.builtins().builtinCount())
    , m_wayInto(FillWith { }, inputs.program.functions.size(), noWitness)
{
    enum class Kind : uint8_t { Name, Entry, ReadUnderKeyNobodyKnows };
    struct Found {
        Kind kind;
        uint32_t builtinOrFunctionEntered;
        Witness witness;
    };

    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    const ValueSets& sets = inputs.sets();

    ValueSetMakers makers(inputs);
    Vector<ValueFlowObjectID, 32> objects;
    for (size_t index : builtinsAskedAbout)
        universe.appendObjectsOf(BuiltinID(static_cast<uint32_t>(index)), objects);
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

    Vector<Found> allFound = collectWithMakers<Found>(inputs, makers, program.functions.size(), [&](unsigned index, ValueSetMaker& maker, Vector<Found>& found) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        bool isInLibrary = isOfLibrary(program, function);
        if (!solver.isReached(id) || (!isInLibrary && !isWrittenInProgram(function)))
            return;

        auto noteBuiltinsIn = [&](ValueSetID held, ValueID place) {
            if (ValueSets::isComplement(held) || !sets.meet(held, objectsAskedAbout))
                return;
            sets.forEachMember(maker.intersect(held, objectsAskedAbout), [&](ValueFlowObjectID object) {
                found.append({ Kind::Name, builtins.canonicalID(universe.builtinOf(object)).index(), witnessAt(function, id, place) });
            });
        };
        auto noteEntryInto = [&](FunctionID entered, const Witness& witness) {
            if (entered && entered != id && isOfLibrary(program, program.at(entered)))
                found.append({ Kind::Entry, entered.index(), witness });
        };
        auto noteRequestFor = [&](ModuleID module, const Witness& witness) {
            if (!module)
                return;
            noteEntryInto(program.at(module).body, witness);
            BuiltinID namespaceObject = program.at(module).runtimeModule ? builtins.at(program.at(module).runtimeModule).namespaceObject : BuiltinID();
            if (namespaceObject && builtinsAskedAbout.get(builtins.canonicalID(namespaceObject).index()))
                found.append({ Kind::Name, builtins.canonicalID(namespaceObject).index(), witness });
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
            case Opcode::ElemGet:
                if (!isInLibrary && sets.contains(inputs.valueOf(maker, operands[*OperandPosition::object(statement.opcode)]), ValueFlowUniverse::unknownObject())
                    && sets.contains(inputs.valueOf(maker, operands[*OperandPosition::property(statement.opcode)]), ValueFlowUniverse::primitive(PrimitiveSet::String)))
                    found.append({ Kind::ReadUnderKeyNobodyKnows, 0, witness });
                break;
            case Opcode::CallDirect:
                noteEntryInto(operation.function(), witness);
                break;
            case Opcode::ModuleNamespace:
            case Opcode::ImportModule:
            case Opcode::RequireModule:
                noteRequestFor(operation.module(), witness);
                break;
            case Opcode::Call:
            case Opcode::CallList:
            case Opcode::Construct:
            case Opcode::ConstructList:
            case Opcode::SuperConstruct:
            case Opcode::SuperConstructList: {
                ValueSetID callees = ValueFlowRuleContext::valueShownToRules(operands[0], solver, maker);
                if (ValueSets::isComplement(callees) || !sets.meet(callees, closuresOfLibrary))
                    break;
                sets.forEachMember(maker.intersect(callees, closuresOfLibrary), [&](ValueFlowObjectID callee) {
                    noteEntryInto(universe.closureFunction(callee), witness);
                });
                break;
            }
            default:
                break;
            }
        }
    });

    Vector<Found> entriesFromLibrary;
    Vector<FunctionID> entered;
    auto enter = [&](FunctionID function, const Witness& witness) {
        if (!hasFunction(m_wayInto[function.index()]))
            entered.append(function);
        keepEarlierPlace(m_wayInto[function.index()], witness);
    };
    for (const Found& found : allFound) {
        bool isInLibrary = isOfLibrary(program, program.functions[found.witness.function]);
        if (found.kind == Kind::Name)
            keepEarlierPlace((isInLibrary ? m_namedInLibrary : m_namedInProgram)[found.builtinOrFunctionEntered], found.witness);
        else if (found.kind == Kind::ReadUnderKeyNobodyKnows)
            keepEarlierPlace(m_readUnderKeyNobodyKnows, found.witness);
        else if (isInLibrary)
            entriesFromLibrary.append(found);
        else
            enter(FunctionID(found.builtinOrFunctionEntered), found.witness);
    }

    auto functionOf = [](const Found& entry) {
        return entry.witness.function;
    };
    AOT::sort(entriesFromLibrary, { }, functionOf);
    while (!entered.isEmpty()) {
        Vector<std::pair<FunctionID, FunctionID>> entriesOfRound;
        for (FunctionID from : std::exchange(entered, { })) {
            for (const Found& entry : std::ranges::equal_range(entriesFromLibrary, from.index(), { }, functionOf)) {
                if (!hasFunction(m_wayInto[entry.builtinOrFunctionEntered]))
                    entriesOfRound.append({ FunctionID(entry.builtinOrFunctionEntered), from });
            }
        }
        for (auto [function, from] : entriesOfRound)
            enter(function, m_wayInto[from.index()]);
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

struct SpelledBuiltin {
    std::span<const uint8_t> text;
    BuiltinID builtin;
    bool isMethod { false };
};

struct NamesFoundByWorker {
    using CandidateList = Vector<BuiltinID, 2>;
    using ListBySets = HashMap<uint64_t, CandidateList, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    BitVector named;
    BitVector literals;
    BitVector stringsInData;
    ListBySets candidatesUnderKeys;
};

} // anonymous namespace

static bool isWordCharacter(uint8_t character)
{
    return isASCIIAlphanumeric(character) || character == '_' || character == '$';
}

static void forEachWord(std::span<const uint8_t> text, NOESCAPE const Invocable<void(std::span<const uint8_t>)> auto& functor)
{
    for (size_t start = 0; start < text.size();) {
        if (!isWordCharacter(text[start])) {
            ++start;
            continue;
        }
        size_t end = start + 1;
        while (end < text.size() && isWordCharacter(text[end]))
            ++end;
        functor(text.subspan(start, end - start));
        start = end;
    }
}

static BuiltinID builtinBoundTo(const Program& program, const Function& function, ValueID value)
{
    const BuiltinTable& builtins = program.builtins();
    const Value& definition = function.at(definitionThroughSameValues(function, value));
    if (definition.opcode == Opcode::Builtin)
        return builtins.canonicalID(definition.builtin());
    if (definition.opcode != Opcode::PropGet || !definition.key())
        return { };
    const Value& holder = function.at(definitionThroughSameValues(function, definition.objectOperand()));
    if (holder.opcode != Opcode::Builtin)
        return { };
    BuiltinID member = findOwnMember(program, builtins.canonicalID(holder.builtin()), definition.key());
    return member ? builtins.canonicalID(member) : BuiltinID();
}

static unsigned wordFilterIndex(std::span<const uint8_t> word)
{
    return (word.size() * 131 + word.front() * 31 + word.back()) & 4095;
}

ValueFlowNamedBuiltins::ValueFlowNamedBuiltins(const ValueFlowResultInputs& inputs, const BitVector& candidates, std::span<const Member> members)
    : m_named(inputs.program.builtins().builtinCount())
{
    if (candidates.isEmpty())
        return;

    using CandidateList = NamesFoundByWorker::CandidateList;
    using ListByIndex = HashMap<uint32_t, CandidateList, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    const ValueSets& sets = inputs.sets();

    ListByIndex candidatesByKey;
    ListByIndex candidatesByNamespace;
    Vector<SpelledBuiltin> spelledBuiltins;
    BuiltinID globalObject = builtins.canonicalID(Builtins::GlobalThis);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        BuiltinID candidate = builtins.canonicalID(BuiltinID(index));
        if (!candidates.quickGet(candidate.index()) || !entry.name || entry.kind == BuiltinKind::Prototype)
            continue;
        if (entry.kind == BuiltinKind::Constructor && entry.holder && builtins.at(entry.holder).kind == BuiltinKind::Prototype)
            continue;
        if (PropertyKeyID key = program.keyOf(entry.name))
            candidatesByKey.add(key.index(), CandidateList()).iterator->value.appendIfNotContains(candidate);
        if (!builtins.isSymbolName(entry.name))
            spelledBuiltins.append({ builtins.textOf(entry.name), candidate, entry.holder && builtins.at(entry.holder).kind == BuiltinKind::Prototype });
        BuiltinID holder = entry.holder ? builtins.canonicalID(entry.holder) : BuiltinID();
        if (holder && holder != globalObject && builtins.at(holder).kind == BuiltinKind::Namespace)
            candidatesByNamespace.add(holder.index(), CandidateList()).iterator->value.appendIfNotContains(candidate);
    }

    auto lists = [&](ValueSetID set, ValueFlowObjectID object) {
        return !ValueSets::isComplement(set) && sets.contains(set, object);
    };
    auto holds = [&](const ValueFlowOperand& operand, ValueFlowObjectID object) {
        return (operand.constant && lists(operand.constant, object)) || (operand.node && lists(solver.valueOf(operand.node), object));
    };
    auto mayBeAnyObject = [&](ValueSetID set) {
        return sets.contains(set, ValueFlowUniverse::unknownObject()) || sets.contains(set, ValueFlowUniverse::untrackedObject());
    };
    auto mayBeAnyKey = [&](ValueSetID set) {
        return mayBeAnyObject(set) || sets.contains(set, ValueFlowUniverse::primitive(PrimitiveSet::String));
    };
    auto isKeyKnown = [&](const ValueFlowOperand& operand) {
        return !(operand.constant && mayBeAnyKey(operand.constant)) && !(operand.node && mayBeAnyKey(solver.valueOf(operand.node)));
    };
    auto setsOf = [&](const ValueFlowOperand& operand) {
        uint64_t constant = operand.constant ? operand.constant.index() : ValueSets::nothing.index();
        uint64_t arriving = operand.node ? solver.valueOf(operand.node).index() : ValueSets::nothing.index();
        return constant << 32 | arriving;
    };
    auto mayAnswer = [&](ValueSetID answers, BuiltinID candidate) {
        if (!isObjectKind(builtins.at(candidate).kind))
            return true;
        return sets.contains(answers, universe.objectOf(candidate)) || mayBeAnyObject(answers);
    };

    ListByIndex candidatesBehindFunction;
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!entry.function || !builtins.contains(entry.id))
            continue;
        for (BuiltinID implemented : builtins.at(entry.id).implements) {
            for (const Member& member : members) {
                if (member.builtin == builtins.canonicalID(implemented))
                    candidatesBehindFunction.add(entry.function.index(), CandidateList()).iterator->value.appendIfNotContains(member.candidate);
            }
        }
    }

    auto findNamesIn = [&](uint32_t index, NamesFoundByWorker& found) {
        found.named.ensureSize(builtins.builtinCount());
        auto noteReadUnder = [&](std::span<const BuiltinID> list, ValueFlowNodeID result) {
            ValueSetID answers = solver.valueOf(result);
            for (BuiltinID candidate : list) {
                if (mayAnswer(answers, candidate))
                    found.named.quickSet(candidate.index());
            }
        };
        if (!solver.isReached(FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        bool isOfProgram = isWrittenInProgram(function) && !isOfLibrary(program, function);
        for (const Value& value : function.values) {
            if (value.opcode == Opcode::String && isOfProgram)
                found.literals.set(value.string().index());
            if (value.opcode == Opcode::Data && isOfProgram) {
                forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                    if (const auto* text = node.tryAs<DataNode::String>())
                        found.stringsInData.set(text->value.index());
                });
            }
            if (value.opcode != Opcode::Builtin)
                continue;
            for (BuiltinID bound = value.builtin(); bound; bound = builtins.at(bound).holder) {
                bound = builtins.canonicalID(bound);
                if (candidates.quickGet(bound.index()))
                    found.named.quickSet(bound.index());
            }
        }

        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || !statement.site || !statement.result)
                continue;
            switch (statement.opcode) {
            case Opcode::PropGet:
            case Opcode::SuperGet: {
                PropertyKeyID key = function.at(statement.site).key();
                auto list = key ? candidatesByKey.find(key.index()) : candidatesByKey.end();
                if (list != candidatesByKey.end())
                    noteReadUnder(list->value.span(), statement.result);
                break;
            }
            case Opcode::ElemGet: {
                std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
                const ValueFlowOperand& property = operands[*OperandPosition::property(statement.opcode)];
                auto underKeys = found.candidatesUnderKeys.add(setsOf(property), CandidateList());
                if (underKeys.isNewEntry && isKeyKnown(property)) {
                    for (auto& [key, list] : candidatesByKey) {
                        if (holds(property, universe.keyConstant(PropertyKeyID(key))))
                            underKeys.iterator->value.appendVector(list);
                    }
                }
                noteReadUnder(underKeys.iterator->value.span(), statement.result);

                BuiltinID holder = builtinBoundTo(program, function, function.at(statement.site).objectOperand());
                auto inNamespace = holder ? candidatesByNamespace.find(holder.index()) : candidatesByNamespace.end();
                if (inNamespace == candidatesByNamespace.end())
                    break;
                for (BuiltinID candidate : inNamespace->value)
                    found.named.quickSet(candidate.index());
                break;
            }
            default:
                break;
            }
        }
    };

    constexpr uint32_t functionsPerTask = 64;
    uint32_t numberOfFunctions = program.functions.size();
    Vector<NamesFoundByWorker> foundByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfFunctions + functionsPerTask - 1) / functionsPerTask, [&](unsigned task, unsigned worker) {
        for (uint32_t index = task * functionsPerTask; index < std::min(numberOfFunctions, (task + 1) * functionsPerTask); ++index) {
            if (!candidatesBehindFunction.contains(index))
                findNamesIn(index, foundByWorker[worker]);
        }
    });

    BitVector literals;
    BitVector stringsInData;
    for (NamesFoundByWorker& found : foundByWorker) {
        m_named.merge(found.named);
        literals.merge(found.literals);
        stringsInData.merge(found.stringsInData);
        found = { };
    }

    for (bool hasEntered = true; std::exchange(hasEntered, false);) {
        Vector<uint32_t, 8> entered;
        for (auto& [function, behind] : candidatesBehindFunction) {
            if (std::ranges::any_of(behind, [&](BuiltinID candidate) { return m_named.quickGet(candidate.index()); }))
                entered.append(function);
        }
        for (uint32_t function : entered) {
            candidatesBehindFunction.remove(function);
            findNamesIn(function, foundByWorker[0]);
            hasEntered = true;
        }
        m_named.merge(foundByWorker[0].named);
    }

    spelledBuiltins.removeAllMatching([&](const SpelledBuiltin& spelled) {
        return spelled.text.empty() || m_named.quickGet(spelled.builtin.index());
    });
    if (spelledBuiltins.isEmpty())
        return;
    BitVector wordFilter(4096);
    for (const SpelledBuiltin& spelled : spelledBuiltins)
        wordFilter.quickSet(wordFilterIndex(spelled.text));
    stringsInData.exclude(literals);
    Vector<uint32_t> literalList;
    for (size_t literal : literals)
        literalList.append(static_cast<uint32_t>(literal));
    size_t numberOfWholeLiterals = literalList.size();
    for (size_t string : stringsInData)
        literalList.append(static_cast<uint32_t>(string));

    constexpr uint32_t literalsPerTask = 1024;
    Vector<BitVector> spelledByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (literalList.size() + literalsPerTask - 1) / literalsPerTask, [&](unsigned task, unsigned worker) {
        BitVector& spelledHere = spelledByWorker[worker];
        spelledHere.ensureSize(builtins.builtinCount());
        for (size_t position = task * literalsPerTask; position < std::min<size_t>(literalList.size(), (task + 1) * literalsPerTask); ++position) {
            forEachWord(program.string(StringID(literalList[position])), [&](std::span<const uint8_t> word) {
                if (!wordFilter.quickGet(wordFilterIndex(word)))
                    return;
                for (const SpelledBuiltin& spelled : spelledBuiltins) {
                    if ((position < numberOfWholeLiterals || spelled.isMethod) && std::ranges::equal(spelled.text, word))
                        spelledHere.quickSet(spelled.builtin.index());
                }
            });
        }
    });
    for (const BitVector& spelledHere : spelledByWorker)
        m_named.merge(spelledHere);
}

static bool isHandedToCallAlone(const Function& function, ValueID list, ValueID call)
{
    CFG cfg(function);
    UseLists useLists(function, cfg);
    for (ValueID current = list;;) {
        std::optional<Use> use = useLists.singleUse(current);
        if (!use || use->kind != Use::Kind::Operand)
            return false;
        if (use->user == call)
            return true;
        if (!isCast(function.at(use->user).opcode))
            return false;
        current = use->user;
    }
}

static void noteLocalesPassedTo(const Program& program, BuiltinID builtin, FunctionID caller, ValueID call, ValueID passed, Vector<FlowLocaleUse>& uses)
{
    AOT_THRESHOLD(unsigned, maxValuesFollowed, 256);
    const Function& callingFunction = program.at(caller);
    if (!passed) {
        uses.append({ LocaleUseKind::HostDefault, { }, builtin, witnessAt(callingFunction, caller, call) });
        return;
    }
    Witness witness = witnessAt(callingFunction, caller, hasModule(callingFunction.locationOf(passed)) ? passed : call);
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visited;
    Vector<std::pair<FunctionID, ValueID>, 16> worklist;
    auto note = [&](LocaleUseKind kind, StringID tag = { }) {
        uses.append({ kind, tag, builtin, witness });
    };
    auto noteText = [&](StringID text) {
        Vector<char16_t> buffer;
        if (isStructurallyValidLanguageTag(program.strings.viewOf(text, buffer)))
            note(LocaleUseKind::Constant, text);
        else
            note(LocaleUseKind::Unknown);
    };
    auto visit = [&](FunctionID function, ValueID value) {
        if (!value || visited.size() >= maxValuesFollowed)
            note(LocaleUseKind::Unknown);
        else if (visited.add(static_cast<uint64_t>(function.index()) << 32 | value.index()).isNewEntry)
            worklist.append({ function, value });
    };

    visit(caller, passed);
    while (!worklist.isEmpty()) {
        auto [id, value] = worklist.takeLast();
        const Function& function = program.at(id);
        const Value& definition = function.at(value);
        switch (definition.opcode) {
        case Opcode::String:
            noteText(definition.string());
            break;
        case Opcode::Undefined:
            note(LocaleUseKind::HostDefault);
            break;
        case Opcode::Data: {
            const DataNode::Array* list = program.at(definition.data()).root.tryAs<DataNode::Array>();
            bool holdsOnlyTexts = list && std::ranges::all_of(list->elements, [](const DataNode& element) {
                return element.is<DataNode::String>();
            });
            if (!holdsOnlyTexts || id != caller || !isHandedToCallAlone(function, value, call)) {
                note(LocaleUseKind::Unknown);
                break;
            }
            if (list->elements.isEmpty())
                note(LocaleUseKind::HostDefault);
            for (const DataNode& element : list->elements)
                noteText(element.as<DataNode::String>().value);
            break;
        }
        case Opcode::Parameter: {
            uint32_t block = 0;
            while (block < function.blocks.size() && !function.blocks[block].parameters.contains(value))
                ++block;
            if (block == function.blocks.size() || BlockID(block) == entryBlock) {
                note(LocaleUseKind::Unknown);
                break;
            }
            CFG cfg(function);
            for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(function.blocks[block].parameters.find(value)));
                visit(id, incoming.kind == IncomingValue::Kind::Argument ? incoming.argument : ValueID());
            }
            break;
        }
        case Opcode::GetModuleVariable: {
            const Variable& variable = program.at(definition.variable());
            FunctionID body = variable.flags.contains(VariableFlags::IsImmutable) && variable.module ? program.at(variable.module).body : FunctionID();
            ValueID stored;
            unsigned numberOfStores = 0;
            if (body && program.at(body).hasBody()) {
                for (const Value& store : program.at(body).values) {
                    if (store.opcode != Opcode::PutModuleVariable || store.variable() != definition.variable())
                        continue;
                    ++numberOfStores;
                    stored = store.valueOperand();
                }
            }
            visit(body, numberOfStores == 1 ? stored : ValueID());
            break;
        }
        default:
            visit(id, definition.opcode == Opcode::CheckInitialized || isCast(definition.opcode) ? definition.operand(0) : ValueID());
            break;
        }
    }
}

static bool readsDataOfLocales(RemovableFeature feature)
{
    using enum RemovableFeature;
    return feature == IntlCollation || feature == IntlNumberFormat || feature == IntlDateTimeFormat || feature == IntlUnits || feature == IntlSegmenter || feature == IntlDisplayNames || feature == IntlOther;
}

Vector<FlowLocaleUse> collectLocaleUses(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& builtinsWhoseCodeRuns, std::span<const UnseenCodeRecord> unseenCode)
{
    for (const UnseenCodeRecord& record : unseenCode) {
        if (admitsUnknownJavaScript(record.cause))
            return { FlowLocaleUse { LocaleUseKind::Unknown, { }, record.builtin == Wire::none ? BuiltinID() : BuiltinID(record.builtin), record.witness } };
    }

    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    uint32_t numberOfBuiltins = builtins.builtinCount();
    Vector<FlowLocaleUse> uses;
    auto addUnknown = [&](BuiltinID builtin, const Witness& witness = noWitness) {
        uses.append({ LocaleUseKind::Unknown, { }, builtin, witness });
    };

    Vector<Vector<BuiltinOperand, 1>> localeOperands(numberOfBuiltins);
    BitVector takesLocale(numberOfBuiltins);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        BuiltinID builtin = builtins.canonicalID(BuiltinID(index));
        if (!builtinsWhoseCodeRuns.quickGet(builtin.index()))
            continue;
        for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index))) {
            if (overload.isUndeclared() && std::ranges::any_of(builtins.featureUsesOf(builtin), readsDataOfLocales, &BuiltinFeatureUse::feature)) {
                takesLocale.quickSet(builtin.index());
                addUnknown(builtin);
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
        ValueFlowObjectID object = inputs.universe().objectOf(BuiltinID(index));
        if (inputs.solver().usesOf(BuiltinID(index)).contains(BuiltinUse::Escaped) || (object && (inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object))))
            addUnknown(builtin);
    }

    BitVector isApplied(numberOfBuiltins);
    forEachBuiltinApplication(inputs, heap, entriesThatTakeLocale, [&](const ValueFlowBuiltinApplication& application) {
        BuiltinID builtin = builtins.canonicalID(application.builtin);
        isApplied.quickSet(builtin.index());
        if (!application.site) {
            addUnknown(builtin);
            return;
        }
        const Function& function = program.at(application.function);
        const Value& call = function.at(application.site);
        bool spellsOutArguments = call.opcode == Opcode::Call || call.opcode == Opcode::Construct;
        for (BuiltinOperand operand : localeOperands[builtin.index()]) {
            if (!spellsOutArguments || operand.kind() != BuiltinOperand::Kind::Argument) {
                addUnknown(builtin, witnessAt(function, application.function, application.site));
                continue;
            }
            std::span<const ValueID> arguments = call.variadicOperands();
            noteLocalesPassedTo(program, builtin, application.function, application.site, operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : ValueID(), uses);
        }
    });
    for (size_t index : takesLocale) {
        if (!isApplied.quickGet(index))
            addUnknown(BuiltinID(static_cast<uint32_t>(index)));
    }

    AOT::sort(uses, { }, [](const FlowLocaleUse& use) {
        return std::tuple(use.kind, use.tag, use.builtin, !hasFunction(use.witness), use.witness.function, use.witness.value);
    });
    uses.shrink(std::ranges::unique(uses, [](const FlowLocaleUse& a, const FlowLocaleUse& b) {
        return a.kind == b.kind && a.tag == b.tag && a.builtin == b.builtin;
    }).begin() - uses.begin());
    return uses;
}

Vector<FlowWorkerEntry> collectWorkerEntries(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    auto isEntryPath = [](const BuiltinParameter& parameter) {
        return parameter.roles.contains(BuiltinOperandRole::WorkerEntryPath);
    };
    BitVector constructors = builtinsWithOverload(builtins, [&](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.parameters, isEntryPath);
    });
    if (constructors.isEmpty() || universe.embeddedFilePaths() == ValueSets::nothing)
        return { };
    ValueSetMaker& maker = heap.maker();
    ValueSetID followed = ValueSets::nothing;
    ValueSetID followedWithOptions = ValueSets::nothing;
    bool getsUnknownValue = false;
    bool getsUnknownValueWithOptions = false;
    struct Start {
        Witness witness { noWitness };
        ValueSetID paths { ValueSets::nothing };
        bool getsUnknownValue { false };
    };
    Vector<Start> starts;
    forEachBuiltinApplication(inputs, heap, constructors, [&](const ValueFlowBuiltinApplication& application) {
        Start start;
        if (application.site)
            start.witness = witnessAt(inputs.program.at(application.function), application.function, application.site);
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            bool hasOptions = inputs.sets().meet(application.otherArguments, universe.objects()) || std::ranges::any_of(overload.parameters, [&](const BuiltinParameter& parameter) {
                return !isEntryPath(parameter) && inputs.sets().meet(application.valueAt(parameter.operand, maker), universe.objects());
            });
            for (const BuiltinParameter& parameter : overload.parameters) {
                if (!isEntryPath(parameter))
                    continue;
                ValueSetID passed = application.valueAt(parameter.operand, maker);
                ValueSetID paths = maker.intersect(passed, universe.embeddedFilePaths());
                followed = maker.unite(followed, paths);
                start.paths = maker.unite(start.paths, paths);
                if (hasOptions)
                    followedWithOptions = maker.unite(followedWithOptions, paths);
                if (inputs.sets().contains(passed, ValueFlowUniverse::unknownObject())) {
                    getsUnknownValue = true;
                    start.getsUnknownValue = true;
                    getsUnknownValueWithOptions |= hasOptions;
                }
            }
        }
        starts.append(start);
    });
    AOT::sort(starts, { }, [](const Start& start) {
        return std::tuple(!hasFunction(start.witness), start.witness.function, start.witness.value);
    });
    // A file whose path another native function looks at is more likely a page that the program reads or sends than a Worker:
    // one function that both kinds of path pass through sends all of them to the constructor.
    ValueSetID usedElsewhere = ValueSets::nothing;
    if (followed != ValueSets::nothing) {
        BitVector otherBuiltins(builtins.builtinCount());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (!constructors.quickGet(index))
                otherBuiltins.quickSet(index);
        }
        forEachBuiltinApplication(inputs, heap, otherBuiltins, followed, [&](const ValueFlowBuiltinApplication& application) {
            auto noteUseOf = [&](BuiltinOperand operand) {
                usedElsewhere = maker.unite(usedElsewhere, maker.intersect(application.valueAt(operand, maker), followed));
            };
            for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
                if (overload.isUndeclared())
                    noteUseOf(BuiltinOperand::argumentsFrom(0));
                for (const BuiltinParameter& parameter : overload.parameters) {
                    if (parameter.uses.contains(OperandUse::Inspects))
                        noteUseOf(parameter.operand);
                }
            }
        });
    }
    // An unknown value may be any path that escaped, for example the result of a method called on an unknown receiver.
    ValueSetID escapedPaths = ValueSets::nothing;
    if (getsUnknownValue)
        escapedPaths = maker.intersect(inputs.solver().valueOf(ValueFlowProgramNodes::escaped()), universe.embeddedFilePaths());
    Vector<FlowWorkerEntry> entries;
    inputs.sets().forEachMember(maker.unite(followed, escapedPaths), [&](ValueFlowObjectID key) {
        bool isFollowed = inputs.sets().contains(followed, key);
        bool isOnlyStarted = isFollowed && !inputs.sets().contains(usedElsewhere, key);
        auto start = std::ranges::find_if(starts, [&](const Start& start) {
            return isFollowed ? inputs.sets().contains(start.paths, key) : start.getsUnknownValue;
        });
        entries.append({ inputs.program.at(universe.keyOf(key)).text, !isOnlyStarted, isFollowed ? inputs.sets().contains(followedWithOptions, key) : getsUnknownValueWithOptions, start == starts.end() ? noWitness : start->witness });
    });
    AOT::sort(entries, { }, &FlowWorkerEntry::path);
    return entries;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
