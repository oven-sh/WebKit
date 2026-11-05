/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleLists.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/HashMap.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct RequestTargetsOfModuleLists {
    const Vector<Typed::ModuleRequest>& requests;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].target; }
};

// What a module requires runs inside its body, so a require() closes no cycle of imports: it leads nowhere.
struct ImportTargetsOfModuleLists {
    const Vector<Typed::ModuleRequest>& requests;
    ModuleID importer;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].kind == Typed::ModuleRequestKind::Require ? importer : requests[index].target; }
};

using DirectoryNumbersOfModuleLists = UncheckedKeyHashMap<uint64_t, uint32_t>;
using PiecesOfModulePath = Vector<StringID, 16>;

// The lists of modules that stand side by side. An entry that names a place in one of the lists counts from the start of these.
struct ModuleListsOfChunk : ModuleLists {
    Vector<uint32_t> filesShownByFaces;
};

// A place in each list that holds the entries of the modules one behind the other.
struct PlacesInModuleLists {
    uint32_t displayPaths { 0 };
    uint32_t requests { 0 };
    uint32_t exports { 0 };
    uint32_t modulesThatDeclareExports { 0 };
    uint32_t indirectExports { 0 };
    uint32_t syntheticExports { 0 };
    uint32_t requiredNames { 0 };
    uint32_t exportStatements { 0 };
};

} // anonymous namespace

void forEachIndexOfLoopsInParallel(const ParallelLoops& loops, std::initializer_list<LoopBesideOthers> loopsBesideEachOther)
{
    unsigned count = 0;
    for (const LoopBesideOthers& loop : loopsBesideEachOther)
        count += loop.count;
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        for (const LoopBesideOthers& loop : loopsBesideEachOther) {
            if (index < loop.count) {
                loop.body(index);
                return;
            }
            index -= loop.count;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool unitHasCodeForModuleLists(const TableInputs& inputs, CodeUnit unit)
{
    return inputs.functions.positionOfUnit(unit, inputs.plan.conventions().indexOf(unit)).has_value();
}

static bool moduleHasCompiledBody(const TableInputs& inputs, ModuleID id)
{
    FunctionID body = inputs.program.at(id).body;
    if (!body)
        return false;
    return unitHasCodeForModuleLists(inputs, { body, Typed::CodeRole::Body })
        || unitHasCodeForModuleLists(inputs, { body, Typed::CodeRole::ModuleStart })
        || unitHasCodeForModuleLists(inputs, { body, Typed::CodeRole::GenericCallEntry });
}

// IsNeverEvaluated: a body that only returns has no code either, and neither has one that does nothing, whose call the walk drops. Their modules are evaluated like any other.
// IsBodyEnteredByRunTime: not where compiled code alone comes to the module: it may store no status, so a body that the loader found in the table would run again. Else its start, else its body.
// The roots of Compile and the units behind a mention of the module go by the same three questions in the same order: unitThatLoaderEnters() of plan/AOTUnitsNamedOutsideCode.cpp, UnitsBehindMention::addModule() of AOTMentions.cpp.
static OptionSet<AnswerAboutModule> askAboutBodyOfModule(const TableInputs& inputs, ModuleID id)
{
    const Typed::ModuleEvaluationPlan& evaluation = inputs.plan.moduleEvaluation();
    FunctionID body = inputs.program.at(id).body;
    bool bodyIsEntered = inputs.plan.moduleBodyIsEntered(id);
    if (!moduleHasCompiledBody(inputs, id)) {
        if (body && bodyIsEntered && !inputs.facts.mayBeReached(body))
            return AnswerAboutModule::IsNeverEvaluated;
        return { };
    }
    OptionSet<AnswerAboutModule> answers { AnswerAboutModule::HasCompiledBody };
    if (!evaluation.isEvaluatedByCodeAlone(id) && (evaluation.findStart(id) || bodyIsEntered))
        answers.add(AnswerAboutModule::IsBodyEnteredByRunTime);
    return answers;
}

// The host enters the function by the entry of its closures, which the plan names for it: addFunctionsThatHostEnters() of plan/AOTUnitsNamedOutsideCode.cpp.
// An entry that holds the body leaves the body without a unit of its own. A function that is left out here is run by the interpreter, and nothing says so.
// A body with code whose entry has none is listed, so that whoever numbers the entry refuses it.
static bool hostEntersCodeOfFunction(const TableInputs& inputs, FunctionID function)
{
    return unitHasCodeForModuleLists(inputs, inputs.plan.conventions().closureEntryOf(function)) || unitHasCodeForModuleLists(inputs, { function, Typed::CodeRole::Body });
}

// The entry that the table names goes on to the start of the module, which evaluates what the module imports and keeps its status.
static bool entryOfModuleBodyLeadsToStart(const TableInputs& inputs, FunctionID body)
{
    const Typed::CallingConventions& conventions = inputs.plan.conventions();
    return conventions.genericEntry(conventions.closureEntryOf(body)).target.role == Typed::CodeRole::ModuleStart;
}

static FunctionID functionOfVariableInClosureTable(const TableInputs& inputs, VariableID id)
{
    FunctionID planned = inputs.plan.variable(id).function;
    return planned ? planned : inputs.program.at(id).function;
}

static SlotData slotAtOffsetForModuleLists(const TableEncoder& encoder, uint32_t offset, SlotRepresentation representation)
{
    if (offset > SlotData::maxOffset) {
        encoder.build.reportLimitExceeded(LimitID::MaxFieldOffset, offset);
        return SlotData::create(0, representation);
    }
    return SlotData::create(offset, representation);
}

namespace {

// The walk over the modules asks one of these two for every number. The first answers with the numbers of the image. The second notes what was asked for, so that Image numbers it.
struct NumbersOfImageForModuleLists {
    uint32_t shownString(StringID id) const { return encoder.stringNumber(id); }
    uint32_t nameOfText(StringID text, ASCIILiteral use, ModuleID owner) const { return encoder.identifierNumberOfText(text, use, owner); }

    uint32_t entryOfBody(FunctionID body, ModuleID owner) const
    {
        // The loader would run the body and nothing of what the module imports.
        const Typed::ModuleStart* start = encoder.plan.moduleEvaluation().findStart(owner);
        if (start && !start->steps.empty() && !entryOfModuleBodyLeadsToStart(encoder.inputs, body))
            encoder.fail("EntryOfModuleBodyPassesByStepsOfItsStart"_s, owner);
        return encoder.entrypointOf(body, "the body of the module"_s, owner);
    }

    SlotData slotOfVariable(VariableID id) const
    {
        const Vector<PlacedVariable>& variables = encoder.numbers.statics().variables;
        if (id.index() >= variables.size()) {
            encoder.fail("VariableReadByTableIsNotPlaced"_s, id);
            return { };
        }
        const PlacedVariable& placed = variables[id.index()];
        // The run time reads the word raw.
        if (placed.plan.encoding == Typed::VariableEncoding::UndefinedAsZero) {
            encoder.fail("VariableReadByTableHoldsUndefinedAsZero"_s, id);
            return { };
        }
        switch (placed.plan.storage) {
        case Typed::VariableStorage::Statics:
            if (!placed.hasWord) {
                encoder.fail("VariableReadByTableHasNoWord"_s, id);
                return { };
            }
            return slotAtOffsetForModuleLists(encoder, placed.offset, placed.plan.representation);
        case Typed::VariableStorage::ClosureTable: {
            uint32_t entrypoint = encoder.entrypointOf(functionOfVariableInClosureTable(encoder.inputs, id), "what a table reads from the variable"_s, id);
            return entrypoint == UINT32_MAX ? SlotData { } : slotAtOffsetForModuleLists(encoder, entrypoint, SlotRepresentation::DeferredClosure);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    SlotData slotOfModuleValue(ModuleID id) const
    {
        const Vector<uint32_t>& offsets = encoder.numbers.statics().moduleValueOffsets;
        uint32_t offset = id.index() < offsets.size() ? offsets[id.index()] : PlacedStatics::noOffset;
        if (offset == PlacedStatics::noOffset) {
            encoder.fail("ModuleValueHasNoWord"_s, id);
            return { };
        }
        return slotAtOffsetForModuleLists(encoder, offset, SlotRepresentation::JSValue);
    }

    bool hasRequireEdge(ModuleID referrer, ModuleID target) const
    {
        std::span<const RequireEdge> edges = encoder.numbers.requireEdges();
        auto found = std::ranges::lower_bound(edges, std::pair(referrer, target), { }, [](const RequireEdge& edge) {
            return std::pair(edge.referrer, edge.target);
        });
        return found != edges.end() && found->referrer == referrer && found->target == target;
    }

    const TableEncoder& encoder;
};

// A set of NamesOfTables has a bit for each string of the program, so the modules of a chunk list what they name, with repeats.
struct IDsNotedForModuleLists {
    void addTo(NamesOfTables& names) const
    {
        for (uint32_t id : strings)
            names.strings.set(id);
        for (uint32_t id : textsNamedAsKeys)
            names.textsNamedAsKeys.set(id);
        for (uint32_t id : variables)
            names.variables.set(id);
        for (uint32_t id : functionsWithClosureCell)
            names.functionsWithClosureCell.set(id);
    }

    Vector<uint32_t> strings;
    Vector<uint32_t> textsNamedAsKeys;
    Vector<uint32_t> variables;
    Vector<uint32_t> functionsWithClosureCell;
};

struct NamesNotedForModuleLists {
    uint32_t shownString(StringID id) const
    {
        noted.strings.append((id ? id : inputs.program.strings.emptyString()).index());
        return 0;
    }

    uint32_t nameOfText(StringID text, ASCIILiteral, ModuleID) const
    {
        noted.textsNamedAsKeys.append(text.index());
        return 0;
    }

    uint32_t entryOfBody(FunctionID, ModuleID) const { return 0; }

    SlotData slotOfVariable(VariableID id) const
    {
        noted.variables.append(id.index());
        switch (inputs.plan.variable(id).storage) {
        case Typed::VariableStorage::ClosureTable:
            if (FunctionID function = functionOfVariableInClosureTable(inputs, id))
                noted.functionsWithClosureCell.append(function.index());
            break;
        case Typed::VariableStorage::Statics:
            break;
        }
        return { };
    }

    SlotData slotOfModuleValue(ModuleID) const { return { }; }
    // Requests name nothing that Image numbers.
    bool hasRequireEdge(ModuleID, ModuleID) const { return false; }

    const TableInputs& inputs;
    IDsNotedForModuleLists& noted;
};

} // anonymous namespace

static bool mayHavePathInPieces(const Typed::Module& module)
{
    return module.kind == ModuleKind::SourceText || module.kind == ModuleKind::Data;
}

// A piece ends with a slash or with the path.
static void forEachPieceOfModulePath(std::span<const uint8_t> path, NOESCAPE const Invocable<void(std::span<const uint8_t>)> auto& functor)
{
    if (path.empty() || path.back() == '/' || !std::ranges::contains(path, '/'))
        return;
    while (!path.empty()) {
        auto slash = std::ranges::find(path, '/');
        size_t length = slash == path.end() ? path.size() : static_cast<size_t>(slash - path.begin()) + 1;
        functor(path.first(length));
        path = path.subspan(length);
    }
}

static bool unseenCodeMayNameFilesOfProgram(const Typed::Facts& facts)
{
    for (const auto& record : facts.unseenCode()) {
        using Cause = decltype(record.cause);
        switch (record.cause) {
        // A text that the program evaluates finds no file: the run time refuses it where nothing is found by name.
        case Cause::EvaluatesText:
        case Cause::LoadsModuleByName:
        case Cause::RunsMachineCode:
            continue;
        // Code that comes from outside the image names what it likes.
        case Cause::Unknown:
        case Cause::ImportsUncompiledModule:
        case Cause::RunsUnseenCode:
        case Cause::OptionAdmitsCode:
        case Cause::FlowResultDisabled:
        case Cause::LoadsCodeByName:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return false;
}

void hidePathsOfModules(Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, const BuildContext& build)
{
    using Typed::ModuleFlags;
    if (!build.options().obfuscates())
        return;
    // A name that is made at run time may be that of any file, and so may one that a walk over the folders of the executable finds.
    if (facts.findsFilesOfProgramByName() || unseenCodeMayNameFilesOfProgram(facts))
        return;

    BitVector isRead = Typed::modulesThatHostAsksFor(program, true, true);
    auto read = [&](ModuleID module) {
        if (module)
            isRead.quickSet(module.index());
    };
    for (const Typed::Module& module : program.modules) {
        // The host resolves a name against the file that asks. For a module that is not of the image the table has that file.
        read(module.referrer);
        // "Export named 'a' not found in module <path>"
        for (const Typed::RequiredName& required : module.requiredNames)
            read(required.module);
    }
    for (const Typed::ModuleFailure& failure : program.moduleFailures) {
        read(failure.importer);
        read(failure.exporter);
        read(failure.endsLoadOf);
    }
    constexpr OptionSet<ModuleFlags> isKnownByPath {
        ModuleFlags::IsLibrary, ModuleFlags::IsBundlerHelper, ModuleFlags::NamesWayToModuleObjects, ModuleFlags::HasInstalledPositions, ModuleFlags::IsFoundByName,
        ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::IsListedInCache, ModuleFlags::RunsAgain, ModuleFlags::IsChangedOnDiskByRun,
        ModuleFlags::IsShownByEntryOfCache, ModuleFlags::IsHandedOutByTestRunner, ModuleFlags::IsReachedWithoutExtension, ModuleFlags::IsReachedUnderAnotherExtension, ModuleFlags::IsReachedAsFolder,
    };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        Typed::Module& module = program.modules[index];
        if (isRead.quickGet(index) || module.kind != ModuleKind::SourceText || module.flags.containsAny(isKnownByPath))
            continue;
        // `import.meta`, `__filename`, `require.resolve("./a")`, the message of a require() that fails.
        if (facts.isPathReadByOperation(ModuleID(index)))
            continue;
        // A require() that code leaves to the loader asks for the module under the path that it had when the code was made. `module.children` lists who was required.
        if (plan.moduleEvaluation().isPathNamedBySites(ModuleID(index)))
            continue;
        // As the face of a CommonJS file is named.
        module.path = program.strings.intern(makeString('#', index));
    }
}

void internPiecesOfModulePaths(Typed::Program& program)
{
    for (const Typed::Module& module : program.modules) {
        if (!mayHavePathInPieces(module))
            continue;
        forEachPieceOfModulePath(program.string(module.path), [&](std::span<const uint8_t> piece) {
            program.strings.intern(piece);
        });
    }
}

// Such a module is simple whatever requests the tables keep, unless a cycle of imports leads through it: they keep no request that the program lacks.
static bool pathLiesInPiecesOutsideCyclesOfImports(const Typed::Program& program, const Typed::Module& module)
{
    return program.strings.size() <= Number24::max && mayHavePathInPieces(module) && !module.flags.contains(Typed::ModuleFlags::HasTopLevelAwait);
}

static BitVector findModulesOutsideCyclesOfImports(const Typed::Program& program)
{
    const Vector<Typed::Module>& modules = program.modules;
    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(static_cast<unsigned>(modules.size()), [&](unsigned index) {
        return ImportTargetsOfModuleLists { modules[index].requests, ModuleID(index) };
    });
    BitVector result(modules.size());
    for (size_t index = 0; index < modules.size(); ++index) {
        if (components.membersOf(components.componentOf[index]).size() == 1)
            result.quickSet(index);
    }
    return result;
}

// None where a piece is no string of the program.
static PiecesOfModulePath findPiecesOfModulePath(const Typed::Program& program, const Typed::Module& module)
{
    PiecesOfModulePath pieces;
    bool everyPieceIsString = true;
    forEachPieceOfModulePath(program.string(module.path), [&](std::span<const uint8_t> piece) {
        pieces.append(program.strings.find(piece));
        everyPieceIsString &= !!pieces.last();
    });
    if (!everyPieceIsString)
        pieces.clear();
    return pieces;
}

// A number depends on every path in front, so this goes in order. Files that stand side by side lie in the same directories for the most part: only where a path leaves that of the file in front of it is a directory looked up.
static void numberDirectoriesOfModulePaths(const BitVector& modulesOutsideCyclesOfImports, std::span<const PiecesOfModulePath> piecesOfPaths, AnswersAboutModules& answers)
{
    DirectoryNumbersOfModuleLists numbersOfDirectories;
    Vector<std::pair<StringID, uint32_t>, 16> directoriesOfLastPath;
    answers.directoryOf.fill(Number24::max, piecesOfPaths.size());
    for (size_t index = 0; index < piecesOfPaths.size(); ++index) {
        const PiecesOfModulePath& pieces = piecesOfPaths[index];
        if (pieces.isEmpty() || !modulesOutsideCyclesOfImports.quickGet(index))
            continue;
        std::span<const StringID> names = pieces.span().first(pieces.size() - 1);
        size_t numberInCommon = 0;
        while (numberInCommon < names.size() && numberInCommon < directoriesOfLastPath.size() && directoriesOfLastPath[numberInCommon].first == names[numberInCommon])
            ++numberInCommon;
        directoriesOfLastPath.shrink(numberInCommon);
        uint32_t directory = numberInCommon ? directoriesOfLastPath.last().second : Number24::max;
        for (StringID name : names.subspan(numberInCommon)) {
            directory = numbersOfDirectories.ensure((static_cast<uint64_t>(directory) << 32 | name.index()) + 1, [&] {
                answers.directories.append({ directory, name });
                return static_cast<uint32_t>(answers.directories.size() - 1);
            }).iterator->value;
            directoriesOfLastPath.append({ name, directory });
        }
        RELEASE_ASSERT(directory < Number24::max);
        answers.directoryOf[index] = directory;
        answers.pathOrNameOf[index] = pieces.last();
    }
}

static bool moduleRunsNothingItself(const Typed::Program& program, const AnswersAboutModules& answers, ModuleID id)
{
    switch (program.at(id).kind) {
    case ModuleKind::SourceText:
        return !answers.has(id, AnswerAboutModule::IsBodyEnteredByRunTime) && (program.at(id).requiredNames.isEmpty() || answers.has(id, AnswerAboutModule::IsNeverEvaluated));
    case ModuleKind::CommonJs:
        return answers.has(id, AnswerAboutModule::IsNeverEvaluated);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void markModulesNotWorthVisiting(const Typed::Program& program, const Typed::StronglyConnectedComponents& componentsOfRequests, AnswersAboutModules& answers)
{
    const Vector<Typed::Module>& modules = program.modules;
    const Vector<uint32_t>& componentOf = componentsOfRequests.componentOf;

    // A component has a higher number than every component it requests.
    BitVector componentsWorthVisiting(componentsOfRequests.numberOfComponents);
    for (uint32_t index : componentsOfRequests.members) {
        bool isWorthVisiting = !moduleRunsNothingItself(program, answers, ModuleID(index)) || std::ranges::any_of(modules[index].requests, [&](const Typed::ModuleRequest& request) {
            return componentsWorthVisiting.quickGet(componentOf[request.target.index()]);
        });
        if (isWorthVisiting)
            componentsWorthVisiting.quickSet(componentOf[index]);
    }
    for (size_t index = 0; index < modules.size(); ++index) {
        if (!componentsWorthVisiting.quickGet(componentOf[index]))
            answers.ofModules[index].add(AnswerAboutModule::IsNotWorthVisiting);
    }
}

// Whether a unit has code is a search through the code of every unit, and a piece of a path is looked up by its text. What goes through a graph or through every function has one thread, and runs beside those.
AnswersAboutModules askAboutModules(const TableInputs& inputs, const ParallelLoops& loops)
{
    const Typed::Program& program = inputs.program;
    const Vector<Typed::Module>& modules = program.modules;
    unsigned numberOfModules = static_cast<unsigned>(modules.size());
    unsigned numberOfRuntimeFunctions = static_cast<unsigned>(program.runtimeFunctions.size());
    size_t numberOfVariables = program.variables.size();

    AnswersAboutModules answers;
    answers.ofModules = Vector<OptionSet<AnswerAboutModule>>(numberOfModules);
    answers.pathOrNameOf = Vector<StringID>(numberOfModules);
    Typed::StronglyConnectedComponents componentsOfRequests;
    BitVector modulesOutsideCyclesOfImports;
    BitVector functionsCalledByHost;
    Vector<PiecesOfModulePath> piecesOfPaths(numberOfModules);
    Vector<uint8_t> hostEntersCodeOfRuntimeFunction(FillWith { }, numberOfRuntimeFunctions, static_cast<uint8_t>(false));
    Vector<Vector<uint32_t>> variablesThatHoldMocksInRanges(numberOfRangesOfIndices(numberOfVariables));
    forEachIndexOfLoopsInParallel(loops, {
        { 1, [&](unsigned) {
            componentsOfRequests = Typed::computeStronglyConnectedComponents(numberOfModules, [&](unsigned index) {
                return RequestTargetsOfModuleLists { modules[index].requests };
            });
        } },
        { 1, [&](unsigned) { modulesOutsideCyclesOfImports = findModulesOutsideCyclesOfImports(program); } },
        { 1, [&](unsigned) { functionsCalledByHost = Typed::functionsCalledByHost(program); } },
        { numberOfModules, [&](unsigned index) {
            const Typed::Module& module = modules[index];
            answers.ofModules[index] = askAboutBodyOfModule(inputs, ModuleID(index));
            if (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode)
                answers.ofModules[index].add(AnswerAboutModule::IsUncompiledCode);
            answers.pathOrNameOf[index] = module.path;
            if (pathLiesInPiecesOutsideCyclesOfImports(program, module))
                piecesOfPaths[index] = findPiecesOfModulePath(program, module);
        } },
        { numberOfRuntimeFunctions, [&](unsigned place) {
            FunctionID function = program.runtimeFunctions[place].function;
            hostEntersCodeOfRuntimeFunction[place] = function && hostEntersCodeOfFunction(inputs, function);
        } },
        { static_cast<unsigned>(variablesThatHoldMocksInRanges.size()), [&](unsigned range) {
            auto [first, end] = boundsOfRangeOfIndices(range, numberOfVariables);
            for (size_t index = first; index < end; ++index) {
                if (program.variables[index].flags.contains(Typed::VariableFlags::HoldsMock))
                    variablesThatHoldMocksInRanges[range].append(static_cast<uint32_t>(index));
            }
        } },
    });

    markModulesNotWorthVisiting(program, componentsOfRequests, answers);
    numberDirectoriesOfModulePaths(modulesOutsideCyclesOfImports, piecesOfPaths.span(), answers);
    answers.mayMocksStandInPlaceOfModules = program.admitsReplacedBuiltins || std::ranges::any_of(program.modules, [](const Typed::Module& module) {
        return module.flags.contains(Typed::ModuleFlags::MayBeMocked);
    });
    answers.variablesThatHoldMocks = Vector<VariableID>(numberOfModules);
    for (const Vector<uint32_t>& variables : variablesThatHoldMocksInRanges) {
        for (uint32_t index : variables) {
            VariableID& first = answers.variablesThatHoldMocks[program.variables[index].module.index()];
            if (!first)
                first = VariableID(index);
        }
    }
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (answers.ofModules[index].contains(AnswerAboutModule::IsUncompiledCode))
            answers.modulesOfUncompiledCode.append(index);
    }
    for (uint32_t place = 0; place < numberOfRuntimeFunctions; ++place) {
        if (hostEntersCodeOfRuntimeFunction[place] && functionsCalledByHost.get(program.runtimeFunctions[place].function.index()))
            answers.compiledRuntimeFunctions.append(place);
    }
    return answers;
}

template<typename Numbers>
static void setDisplayPathInModuleLists(const Typed::Program& program, ModuleID id, bool obfuscates, const Numbers& numbers, ModuleLists& lists, ModuleData& entry)
{
    const Typed::Module& module = program.at(id);
    std::span<const uint8_t> displayPath = module.displayPath ? program.string(module.displayPath) : std::span<const uint8_t>();
    entry.charactersInFrontOfDisplayPath = ModuleData::hasNoDisplayPath;
    if ((obfuscates && !module.runtimeModule) || displayPath.empty())
        return;
    std::span<const uint8_t> publicPath = program.string(module.path);
    if (publicPath.size() >= displayPath.size()) {
        std::span<const uint8_t> inFront = publicPath.first(publicPath.size() - displayPath.size());
        // The run time counts characters.
        bool isCounted = inFront.size() < ModuleData::hasNoDisplayPath && std::ranges::all_of(inFront, isASCII<uint8_t>);
        if (isCounted && std::ranges::equal(publicPath.subspan(inFront.size()), displayPath)) {
            entry.charactersInFrontOfDisplayPath = static_cast<uint8_t>(inFront.size());
            return;
        }
    }
    entry.charactersInFrontOfDisplayPath = ModuleData::displayPathIsListed;
    lists.displayPaths.append({ Number24(id.index()), Number24(std::min(numbers.shownString(module.displayPath), Number24::max)) });
}

template<typename Numbers>
static ModuleData createRecordForModuleLists(const TableInputs& inputs, const AnswersAboutModules& answers, ModuleID id, const Numbers& numbers)
{
    using Flag = ModuleData::Flag;
    using Typed::ModuleFlags;
    const Typed::Program& program = inputs.program;
    const Typed::Module& module = program.at(id);
    bool isModuleOfHost = module.kind == ModuleKind::RuntimeSource || module.kind == ModuleKind::Runtime;
    bool isBundlerHelper = Typed::isBundlerHelperModule(program, module);
    bool isOfLibrary = isModuleOfHost || module.flags.contains(ModuleFlags::IsLibrary) || isBundlerHelper;
    bool hasValue = module.exportsVariable || inputs.plan.hasModuleValue(id);
    bool isEntered = answers.has(id, AnswerAboutModule::IsBodyEnteredByRunTime);

    ModuleData entry { };
    entry.hasInstalledPositions = module.flags.contains(ModuleFlags::HasInstalledPositions);
    entry.loadedByLoader = module.flags.contains(ModuleFlags::IsLoadedByLoader);
    entry.importedWhileItRunsOrAsNamespace = module.flags.contains(module.kind == ModuleKind::CommonJs ? ModuleFlags::MayBeImportedWhileItRuns : module.kind == ModuleKind::Data ? ModuleFlags::AnswersLoaderWithNamespace : ModuleFlags::IsImportedAsNamespace);
    if (isModuleOfHost)
        entry.importedWhileItRunsOrAsNamespace = inputs.plan.hasRequireCacheValue(id);
    entry.evictableOrHeldUnderNameWithoutPrefix = module.flags.contains(isOfLibrary ? ModuleFlags::IsHeldUnderNameWithoutPrefix : ModuleFlags::MayBeEvicted);
    entry.kind = module.kind;
    entry.externalKind = module.kind != ModuleKind::External && module.flags.contains(ModuleFlags::MayBeMocked) ? ModuleData::mayBeMockedBit : module.externalKind;
    entry.exportRule = module.exportRule;
    entry.requireResult = module.requireResult;
    entry.body = optionalNumber24(isEntered ? numbers.entryOfBody(module.body, id) : UINT32_MAX);
    RELEASE_ASSERT(!module.referrer || !module.runtimeModule);
    uint32_t referrerOrRuntimeModule = module.referrer ? module.referrer.index() : module.runtimeModule ? module.runtimeModule.index() : UINT32_MAX;
    entry.referrerOrRuntimeModule = optionalNumber24(referrerOrRuntimeModule);
    if (module.exportsVariable)
        entry.value = numbers.slotOfVariable(module.exportsVariable);
    else if (hasValue)
        entry.value = numbers.slotOfModuleValue(id);
    const std::pair<bool, Flag> flags[] = {
        { module.kind == ModuleKind::CommonJs && module.syntheticExports.isEmpty(), Flag::CapturesOnDemand },
        { module.kind != ModuleKind::CommonJs && module.flags.contains(ModuleFlags::HasTopLevelAwait), Flag::HasTopLevelAwait },
        { module.kind == ModuleKind::CommonJs && module.flags.contains(ModuleFlags::RunsAgain), Flag::RunsAgainAfterThrow },
        { module.kind == ModuleKind::CommonJs && module.flags.contains(ModuleFlags::IsModuleToImports), Flag::IsModuleToImports },
        { module.kind != ModuleKind::CommonJs || module.flags.contains(ModuleFlags::KeepsError), Flag::KeepsError },
        { isEntered && entry.bodyFunction() && entryOfModuleBodyLeadsToStart(inputs, module.body), Flag::EvaluatesItself },
        { hasValue, Flag::HasValue },
        { isBundlerHelper, Flag::IsLibraryCodeWithFilePath },
        { isOfLibrary, Flag::IsOfLibrary },
    };
    for (auto [holds, flag] : flags) {
        if (holds)
            entry.add(flag);
    }
    return entry;
}

// No edge: the code of the requester evaluates the target where the require() stands, and asks the run time nothing.
// A site that finds its target by a name at run time has no edge either, and asks the run time: code calls only what the plan lets it call.
template<typename Numbers>
static bool isAnsweredByCode(const TableInputs& inputs, const Numbers& numbers, ModuleID requester, const Typed::ModuleRequest& request)
{
    switch (inputs.program.at(request.target).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
        return inputs.plan.moduleEvaluation().isCalledWhereRequired(request.target) && !numbers.hasRequireEdge(requester, request.target);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The plan gave a variable that the run time reads a word in a form that the run time can read. It has decided, by what Facts says of the namespace, which exports those are.
static bool isExportDescribedInModuleLists(const TableInputs& inputs, const Typed::ModuleExport& moduleExport)
{
    switch (moduleExport.kind) {
    case ModuleExportKind::Variable:
        return inputs.plan.variable(moduleExport.variable).isDescribedToRunTime;
    case ModuleExportKind::Namespace:
    case ModuleExportKind::Indirect:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Numbers>
static void appendExportsToModuleLists(const TableInputs& inputs, ModuleID id, VariableID variableThatHoldsMock, bool mayMocksStandInPlaceOfModules, const Numbers& numbers, ModuleLists& lists)
{
    const Typed::Program& program = inputs.program;
    for (const Typed::ModuleExport& moduleExport : program.at(id).exports) {
        if (!isExportDescribedInModuleLists(inputs, moduleExport))
            continue;
        ModuleExportData entry { };
        entry.name = numbers.nameOfText(moduleExport.name, "an export of"_s, id);
        entry.kindBits = static_cast<uint32_t>(moduleExport.kind);
        entry.isDeferred = moduleExport.phase == ImportPhase::Defer;
        entry.isFromStarExport = moduleExport.isFromStarExport;
        entry.isDroppedWhileEmpty = moduleExport.isDroppedWhileEmpty;
        ModuleID declaring = id;
        switch (moduleExport.kind) {
        case ModuleExportKind::Variable:
            entry.variable = numbers.slotOfVariable(moduleExport.variable);
            entry.isReplaceable = program.at(moduleExport.variable).flags.contains(Typed::VariableFlags::IsReplaceable);
            declaring = program.at(moduleExport.variable).module;
            break;
        case ModuleExportKind::Namespace:
            entry.module = moduleExport.module.index();
            break;
        case ModuleExportKind::Indirect:
            entry.indirectExport = static_cast<uint32_t>(lists.indirectExports.size());
            lists.indirectExports.append({ moduleExport.module.index(), numbers.nameOfText(moduleExport.importName, "what another module exports under a name of"_s, id) });
            break;
        }
        lists.exports.append(entry);
        if (mayMocksStandInPlaceOfModules)
            lists.modulesThatDeclareExports.append(declaring.index());
    }
    // ModuleTable::wordOfMock(). No program without such a module has such an entry.
    if (variableThatHoldsMock) {
        ModuleExportData entry { };
        entry.kindBits = ModuleExportData::kindBitsOfWordOfMock;
        entry.variable = numbers.slotOfVariable(variableThatHoldsMock);
        if (mayMocksStandInPlaceOfModules)
            lists.modulesThatDeclareExports.append(id.index());
        lists.exports.append(entry);
    }
}

// Reads the records of other modules and none of their entries in the lists: the modules are walked in any order, in lists of any length.
template<typename Numbers>
static void appendModuleToModuleLists(const TableInputs& inputs, const AnswersAboutModules& answers, ModuleID id, const Numbers& numbers, ModuleLists& lists)
{
    const Typed::Program& program = inputs.program;
    const Typed::Module& module = program.at(id);
    ModuleData record = createRecordForModuleLists(inputs, answers, id, numbers);
    setDisplayPathInModuleLists(program, id, inputs.options.obfuscates(), numbers, lists, record);
    record.publicPathOrName = numbers.shownString(answers.pathOrNameOf[id.index()]);
    record.directory = Number24(answers.directoryOf[id.index()]);
    lists.setListStarts(record);
    lists.modules.append(record);

    // The run time finds the file behind a face in the first request of the face, whether the body of the face calls the file or asks the loader for it.
    const Typed::ModuleRequest* requestOfFace = Typed::isFaceOfCommonJSFile(program, module) ? &module.requests[0] : nullptr;
    for (const Typed::ModuleRequest& request : module.requests) {
        if (answers.ofModules[request.target.index()].containsAll({ AnswerAboutModule::IsNotWorthVisiting, AnswerAboutModule::IsNeverEvaluated }))
            continue;
        bool isRequire = request.kind == Typed::ModuleRequestKind::Require;
        if (isRequire && &request != requestOfFace && !isAnsweredByCode(inputs, numbers, id, request))
            continue;
        lists.requests.append(ModuleRequestData::create(request.target.index(), request.phase == ImportPhase::Defer, request.isDynamicStarExport, isRequire, request.isHoisted));
    }
    appendExportsToModuleLists(inputs, id, answers.variablesThatHoldMocks[id.index()], answers.mayMocksStandInPlaceOfModules, numbers, lists);
    for (const Typed::SyntheticExport& syntheticExport : module.syntheticExports) {
        SyntheticExportData entry { };
        entry.name = numbers.nameOfText(syntheticExport.name, "an export that the run time fills for"_s, id);
        entry.absentIsUndefined = syntheticExport.isKnownMember;
        entry.hasVariable = !!syntheticExport.variable;
        entry.isReplaceable = syntheticExport.isReplaceable;
        entry.variable = syntheticExport.variable ? numbers.slotOfVariable(syntheticExport.variable) : SlotData { };
        lists.syntheticExports.append(entry);
    }
    if (answers.has(id, AnswerAboutModule::IsNeverEvaluated))
        return;
    for (const Typed::RequiredName& requiredName : module.requiredNames) {
        if (requiredName.specifier)
            lists.exportStatements.append({ static_cast<uint32_t>(lists.requiredNames.size()), numbers.shownString(requiredName.exportName), requiredName.dropsExportIfAbsent, numbers.shownString(requiredName.specifier) });
        lists.requiredNames.append({ requiredName.module.index(), requiredName.mayBeAbsent, numbers.nameOfText(requiredName.name, "what another module must export for"_s, id) });
    }
}

template<typename Numbers>
static void appendWhatNoModuleOwnsToModuleLists(const TableInputs& inputs, const AnswersAboutModules& answers, const Numbers& numbers, ModuleLists& lists)
{
    const Typed::Program& program = inputs.program;
    lists.directories.reserveInitialCapacity(answers.directories.size());
    for (const AnswersAboutModules::Directory& directory : answers.directories)
        lists.directories.append({ Number24(directory.parent), Number24(numbers.shownString(directory.name)) });
    for (const Typed::AmbiguousExport& ambiguous : program.ambiguousExports)
        lists.ambiguousExports.append({ ambiguous.module.index(), numbers.shownString(ambiguous.name) });
    AOT::stableSort(lists.ambiguousExports, { }, &AmbiguousExportData::module);
    for (const Typed::SourceStamp& stamp : program.sourceStamps)
        lists.sourceStamps.append({ stamp.module.index(), stamp.low, stamp.high });
    AOT::stableSort(lists.sourceStamps, { }, &SourceStampData::module);
}

// ModuleData::evaluatesItself() says that what the run time enters stores the status of the module. Where it says so wrongly the body runs twice or never.
static void settleBodyInModuleLists(const TableEncoder& encoder, ModuleID id, ModuleData& record)
{
    const Typed::Module& module = encoder.program.at(id);
    const Typed::ModuleEvaluationPlan& evaluation = encoder.plan.moduleEvaluation();
    // It stores no status, so the loader would take it for unevaluated and run a body that it finds here once more.
    if (evaluation.isEvaluatedByCodeAlone(id)) {
        record.body = Number24(Number24::max);
        record.flags &= ~static_cast<uint16_t>(ModuleData::Flag::EvaluatesItself);
        return;
    }
    // Nothing but the steps calls the modules that have no body here.
    const Typed::ModuleStart* start = evaluation.findStart(id);
    bool isEnteredAtStart = record.bodyFunction() && record.evaluatesItself();
    if (start && !start->steps.empty() && !isEnteredAtStart) {
        // A module that nothing reaches has neither code nor a body here.
        if (record.bodyFunction() || encoder.hasCode({ module.body, Typed::CodeRole::ModuleStart }))
            encoder.fail("ModuleStartWithStepsIsNotEntered"_s, id);
        return;
    }
    if (!record.bodyFunction() || record.evaluatesItself())
        return;
    // The entry that the record names leads to the body.
    bool isFileWithCode = module.kind == ModuleKind::SourceText || module.kind == ModuleKind::CommonJs;
    if (isFileWithCode && encoder.inputs.bodyFacts.functionsThatOnlyReturn.get(module.body.index())) {
        record.body = Number24(Number24::max);
        return;
    }
    // Such a body opens with BeginModuleEvaluation, which refuses a module that the loader has begun.
    if (evaluation.moduleThatBeginsIn(module.body) == id)
        record.add(ModuleData::Flag::EvaluatesItself);
}

// The run time finds the file behind a face in the first request that the tables keep. The module is the last of the lists.
static void markFaceOfFileInModuleLists(const Typed::Program& program, ModuleID id, ModuleListsOfChunk& lists)
{
    const Typed::Module& module = program.at(id);
    if (!Typed::isFaceOfCommonJSFile(program, module))
        return;
    ModuleData& record = lists.modules.last();
    std::span<const ModuleRequestData> requests = lists.requests.span().subspan(record.firstRequest);
    uint32_t file = module.requests[0].target.index();
    if (requests.empty() || !requests.front().isRequire || requests.front().target() != file)
        return;
    record.faceOfFile = true;
    lists.filesShownByFaces.append(file);
}

static void forEachListOfEntriesOfModules(const auto& functor)
{
    functor(&ModuleLists::displayPaths, &PlacesInModuleLists::displayPaths);
    functor(&ModuleLists::requests, &PlacesInModuleLists::requests);
    functor(&ModuleLists::exports, &PlacesInModuleLists::exports);
    functor(&ModuleLists::modulesThatDeclareExports, &PlacesInModuleLists::modulesThatDeclareExports);
    functor(&ModuleLists::indirectExports, &PlacesInModuleLists::indirectExports);
    functor(&ModuleLists::syntheticExports, &PlacesInModuleLists::syntheticExports);
    functor(&ModuleLists::requiredNames, &PlacesInModuleLists::requiredNames);
    functor(&ModuleLists::exportStatements, &PlacesInModuleLists::exportStatements);
}

// The lists of a chunk are as long as they are, so where each begins among all is a sum, and every chunk is then written there beside the others. Leaves room for the last record, and frees the lists of the chunks.
static ModuleLists joinModuleListsOfChunks(const ParallelLoops& loops, const ChunksOfIndices& chunks, Vector<ModuleListsOfChunk>& listsOfChunks)
{
    // [chunk], and one more for the end.
    Vector<PlacesInModuleLists> starts(chunks.size() + 1);
    for (unsigned chunk = 0; chunk < chunks.size(); ++chunk) {
        forEachListOfEntriesOfModules([&](auto list, auto place) {
            starts[chunk + 1].*place = starts[chunk].*place + static_cast<uint32_t>((listsOfChunks[chunk].*list).size());
        });
    }

    ModuleLists lists;
    lists.modules.grow(chunks.firstIndexOf.last() + 1);
    forEachListOfEntriesOfModules([&](auto list, auto place) {
        (lists.*list).grow(starts.last().*place);
    });
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        ModuleLists& part = listsOfChunks[chunk];
        const PlacesInModuleLists& start = starts[chunk];
        for (ModuleData& record : part.modules) {
            record.firstRequest = Number24(start.requests + record.firstRequest);
            record.firstExport = Number24(start.exports + record.firstExport);
            record.firstSyntheticExport = Number24(start.syntheticExports + record.firstSyntheticExport);
            record.firstRequiredName = Number24(start.requiredNames + record.firstRequiredName);
        }
        for (ModuleExportData& entry : part.exports) {
            if (entry.kindBits == static_cast<uint32_t>(ModuleExportKind::Indirect))
                entry.indirectExport += start.indirectExports;
        }
        for (ExportStatementData& statement : part.exportStatements)
            statement.requiredName += start.requiredNames;
        memcpySpan(lists.modules.mutableSpan().subspan(chunks.firstIndexOf[chunk]), part.modules.span());
        forEachListOfEntriesOfModules([&](auto list, auto place) {
            memcpySpan((lists.*list).mutableSpan().subspan(start.*place), (part.*list).span());
        });
        part = { };
    });
    return lists;
}

static void markExportsBoundByRequiredNames(ModuleLists& lists)
{
    for (const RequiredNameData& requiredName : lists.requiredNames) {
        const ModuleData& exporter = lists.modules[requiredName.exporter];
        if (exporter.exportRule != SyntheticExportRule::RuntimeModule || requiredName.mayBeAbsent)
            continue;
        uint32_t end = lists.modules[requiredName.exporter + 1].firstSyntheticExport;
        for (uint32_t index = exporter.firstSyntheticExport; index < end; ++index) {
            if (lists.syntheticExports[index].name == requiredName.name)
                lists.syntheticExports[index].isBoundByRequiredName = true;
        }
    }
}

ModuleLists collectModuleLists(TableEncoder& encoder, const AnswersAboutModules& answers)
{
    const TableInputs& inputs = encoder.inputs;
    const Typed::Program& program = inputs.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    NumbersOfImageForModuleLists numbers { encoder };

    ChunksOfIndices chunks = divideIntoChunks(static_cast<unsigned>(program.modules.size()));
    Vector<ModuleListsOfChunk> listsOfChunks(chunks.size());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        ModuleListsOfChunk& lists = listsOfChunks[chunk];
        for (unsigned index = chunks.firstIndexOf[chunk]; index < chunks.firstIndexOf[chunk + 1]; ++index) {
            appendModuleToModuleLists(inputs, answers, ModuleID(index), numbers, lists);
            // computeModuleFlags() goes by which modules have a body.
            settleBodyInModuleLists(encoder, ModuleID(index), lists.modules.last());
            markFaceOfFileInModuleLists(program, ModuleID(index), lists);
        }
    });

    ModuleLists lists = joinModuleListsOfChunks(loops, chunks, listsOfChunks);
    ModuleData endRecord { };
    lists.setListStarts(endRecord);
    lists.modules.last() = endRecord;
    appendWhatNoModuleOwnsToModuleLists(inputs, answers, numbers, lists);
    for (const ModuleListsOfChunk& listsOfChunk : listsOfChunks) {
        for (uint32_t file : listsOfChunk.filesShownByFaces)
            lists.modules[file].shownByFace = true;
    }
    for (size_t index : Typed::modulesThatHostAsksFor(program, inputs.facts.findsFilesOfProgramByName()))
        lists.modules[index].add(ModuleData::Flag::IsEntryPoint);
    markExportsBoundByRequiredNames(lists);
    return lists;
}

void nameWhatModuleListsName(const TableInputs& inputs, const AnswersAboutModules& answers, const ParallelLoops& loops, NamesOfTables& names)
{
    ChunksOfIndices chunks = divideIntoChunks(static_cast<unsigned>(inputs.program.modules.size()));
    // [chunk], and one more for what no module owns.
    Vector<IDsNotedForModuleLists> notedByChunks(chunks.size() + 1);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        NamesNotedForModuleLists numbers { inputs, notedByChunks[chunk] };
        ModuleLists lists;
        for (unsigned index = chunks.firstIndexOf[chunk]; index < chunks.firstIndexOf[chunk + 1]; ++index)
            appendModuleToModuleLists(inputs, answers, ModuleID(index), numbers, lists);
    });
    ModuleLists lists;
    appendWhatNoModuleOwnsToModuleLists(inputs, answers, NamesNotedForModuleLists { inputs, notedByChunks.last() }, lists);
    for (const IDsNotedForModuleLists& noted : notedByChunks)
        noted.addTo(names);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
