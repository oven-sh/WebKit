/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTFlowResults.h"
#include "AOTOperationData.h"
#include "AOTReuseEnvironments.h"
#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The variables lie in this order. The words that the collector visits lie together, so the image lists them as a few ranges.
enum class VariableGroup : uint8_t {
    UndefinedValue,
    UndefinedInteger,
    UndefinedDouble,
    AbsentInteger,
    AbsentDouble,
    UndefinedReference4,
    UndefinedInteger4,
    AbsentInteger4,
    UndefinedByte,
    ZeroValue,
    ZeroFunction,
    ZeroWord8,
    ZeroReference4,
    ZeroWord4,
    ZeroWord2,
    ZeroWord1,
};

} // anonymous namespace

static constexpr unsigned numberOfFilledGroups = 9;
static constexpr unsigned staticsWordSize = sizeof(uint64_t);

static_assert(static_cast<unsigned>(VariableGroup::ZeroValue) == numberOfFilledGroups);
static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::LiteralString).absentBits);
static_assert(!traitsOf(SlotRepresentation::OptionalCell32).absentBits && !traitsOf(SlotRepresentation::LiteralString32).absentBits && !traitsOf(SlotRepresentation::OptionalBoolean).absentBits);
static_assert(widthOf(SlotRepresentation::JSValue) == staticsWordSize && widthOf(SlotRepresentation::LiteralString) == staticsWordSize && widthOf(SlotRepresentation::OptionalInt64) == staticsWordSize && widthOf(SlotRepresentation::OptionalDouble) == staticsWordSize);
static_assert(defaultLimitValue(LimitID::MaxStaticsSize, Architecture::ARM64) <= UINT32_MAX && defaultLimitValue(LimitID::MaxStaticsSize, Architecture::X86_64) <= UINT32_MAX);
static_assert(sizeof(EncodedJSValue) == staticsWordSize && sizeof(JSCell*) == staticsWordSize);
static_assert(MemberGuardData::bitsPerWord == 8 * staticsWordSize);
static_assert(sizeof(ArrayAllocationProfile) == staticsWordSize && ArrayAllocationProfile::sizeOfLastArrayWord == staticsWordSize);

uint64_t initialWordOf(const Program& program, VariableID id, const PlannedVariable& planned)
{
    const Variable& variable = program.at(id);
    if (planned.storage != VariableStorage::Statics || planned.encoding != VariableEncoding::Plain)
        return 0;
    // The hooks of a class object ask the variable whether the static field is defined, at times no proof about compiled reads covers.
    if (variable.flow.initialization != InitializationState::Unknown)
        return variable.owner ? traitsOf(planned.representation).absentBits : 0;
    if (variable.initialization == VariableInitialization::Undefined)
        return traitsOf(planned.representation).undefinedBits;
    RELEASE_ASSERT(variable.initialization == VariableInitialization::Uninitialized);
    return traitsOf(planned.representation).absentBits;
}

static VariableGroup zeroGroupOf(const PlacedVariable& placed)
{
    switch (widthOf(placed.representation)) {
    case 8:
        if (!traitsOf(placed.representation).holdsCell)
            return VariableGroup::ZeroWord8;
        return placed.encoding == VariableEncoding::FunctionAsZero ? VariableGroup::ZeroFunction : VariableGroup::ZeroValue;
    case 4:
        return traitsOf(placed.representation).isNarrowReference ? VariableGroup::ZeroReference4 : VariableGroup::ZeroWord4;
    case 2:
        return VariableGroup::ZeroWord2;
    case 1:
        return VariableGroup::ZeroWord1;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static VariableGroup groupOf(VariableInitialization initialization, const PlacedVariable& placed)
{
    if (!placed.initialWord)
        return zeroGroupOf(placed);
    bool startsUndefined = initialization == VariableInitialization::Undefined;
    switch (placed.representation) {
    case SlotRepresentation::JSValue:
    case SlotRepresentation::LiteralString:
        return VariableGroup::UndefinedValue;
    case SlotRepresentation::OptionalInt64:
        return startsUndefined ? VariableGroup::UndefinedInteger : VariableGroup::AbsentInteger;
    case SlotRepresentation::OptionalDouble:
        return startsUndefined ? VariableGroup::UndefinedDouble : VariableGroup::AbsentDouble;
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::LiteralString32:
        return VariableGroup::UndefinedReference4;
    case SlotRepresentation::OptionalInt32:
        return startsUndefined ? VariableGroup::UndefinedInteger4 : VariableGroup::AbsentInteger4;
    case SlotRepresentation::OptionalBoolean:
        return VariableGroup::UndefinedByte;
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::DeferredClosure:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whoever starts the program evaluates these on the spot, so their words are filled in one go.
static BitVector findFilesBroughtByFirstEntryPoint(const Program& program)
{
    BitVector brought(program.modules.size());
    if (program.entries.isEmpty())
        return brought;
    Vector<uint32_t, 64> pending { program.entries.first().module.index() };
    brought.quickSet(pending.first());
    while (!pending.isEmpty()) {
        for (const ModuleRequest& request : program.modules[pending.takeLast()].requests) {
            bool comesAlong = request.kind == ModuleRequestKind::Import || request.completes;
            if (comesAlong && !brought.quickSet(request.target.index()))
                pending.append(request.target.index());
        }
    }
    return brought;
}

static bool hasCodeOfProgram(const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
        return true;
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nobody calls a body that only returns. The entry that the host calls holds nothing but the call of the function that it stands in front of.
static bool entersFile(const Program& program, const Function& function)
{
    if (!function.isModuleBody() || !function.hasBody() || !function.module)
        return false;
    const Block& first = function.blocks.first();
    bool onlyCallsBodyOfFile = function.blocks.size() == 1 && !first.values.isEmpty() && std::ranges::all_of(first.values, [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::CallDirect && program.at(value.function()).isModuleBody() && program.at(value.function()).module == function.module;
    });
    if (onlyCallsBodyOfFile)
        return false;
    bool onlyReturns = function.blocks.size() == 1 && first.terminator.kind == TerminatorKind::Return && std::ranges::all_of(first.values, [&](ValueID id) {
        return function.at(id).opcode == Opcode::Undefined;
    });
    return !onlyReturns;
}

// What a file has written runs, and its names are handed out, when the file has been entered or a file that imports it: by the function that evaluates what it imports,
// by its code, or by the run time where somebody asks for it. So it is enough that whoever enters a file fills the first words of that file and of all that it imports, at whatever depth.
// To fill more is harmless as long as nothing of what is filled has been entered. So the files go in groups, each filled as a whole when the first of its files is entered:
// a group is what the imports lead to from a file that nobody imports, less what they led to from an earlier one.
static void chooseFilesFilledWhenEntered(const PlacementContext& context, StaticsPlan& plan)
{
    constexpr uint32_t none = UINT32_MAX;
    const Program& program = context.program;
    uint32_t numberOfModules = program.modules.size();
    plan.groupOfFile = Vector<std::optional<uint32_t>>(numberOfModules);
    if (program.entries.isEmpty())
        return;
    BitVector hasWords(numberOfModules);
    for (uint32_t index = 0; index < plan.variables.size(); ++index) {
        ModuleID module = program.variables[index].module;
        if (plan.variables[index].hasWordInStatics() && plan.variables[index].initialWord && module && hasCodeOfProgram(program.at(module)))
            hasWords.quickSet(module.index());
    }
    BitVector isBroughtByFirstEntryPoint = findFilesBroughtByFirstEntryPoint(program);
    hasWords.exclude(isBroughtByFirstEntryPoint);
    if (hasWords.isEmpty())
        return;
    BitVector isEntered(numberOfModules);
    for (const Function& function : program.functions) {
        if (entersFile(program, function))
            isEntered.quickSet(function.module.index());
    }
    hasWords.filter(isEntered);
    if (hasWords.isEmpty())
        return;

    auto isGrouped = [&](uint32_t module) {
        return hasCodeOfProgram(program.modules[module]) && !isBroughtByFirstEntryPoint.quickGet(module);
    };
    auto forEachImportedFile = [&](uint32_t module, const Invocable<void(uint32_t)> auto& functor) {
        for (const ModuleRequest& request : program.modules[module].requests) {
            if (request.kind == ModuleRequestKind::Import && isGrouped(request.target.index()))
                functor(request.target.index());
        }
    };
    BitVector isImported(numberOfModules);
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (!isGrouped(module))
            continue;
        forEachImportedFile(module, [&](uint32_t imported) {
            if (imported != module)
                isImported.quickSet(imported);
        });
    }
    Vector<uint32_t> groupOfModule(FillWith { }, numberOfModules, none);
    Vector<uint32_t> firstFileOf;
    Vector<uint32_t, 64> pending;
    auto group = [&](uint32_t first) {
        uint32_t number = firstFileOf.size();
        firstFileOf.append(first);
        groupOfModule[first] = number;
        pending.append(first);
        while (!pending.isEmpty()) {
            forEachImportedFile(pending.takeLast(), [&](uint32_t imported) {
                if (groupOfModule[imported] != none)
                    return;
                groupOfModule[imported] = number;
                pending.append(imported);
            });
        }
    };
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (isGrouped(module) && !isImported.quickGet(module))
            group(module);
    }
    // What is left imports itself in a circle.
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (isGrouped(module) && groupOfModule[module] == none)
            group(module);
    }

    Vector<std::pair<uint32_t, uint32_t>> importedAndImporter;
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (!isGrouped(module))
            continue;
        forEachImportedFile(module, [&](uint32_t imported) {
            if (groupOfModule[imported] != groupOfModule[module])
                importedAndImporter.append({ groupOfModule[imported], groupOfModule[module] });
        });
    }
    AOT::sort(importedAndImporter);
    importedAndImporter.shrink(std::ranges::unique(importedAndImporter).begin() - importedAndImporter.begin());
    BitVector findsWords(firstFileOf.size());
    for (size_t module : hasWords) {
        if (!findsWords.quickSet(groupOfModule[module]))
            pending.append(groupOfModule[module]);
    }
    while (!pending.isEmpty()) {
        for (auto [imported, importer] : std::ranges::equal_range(importedAndImporter, pending.takeLast(), { }, &std::pair<uint32_t, uint32_t>::first)) {
            if (!findsWords.quickSet(importer))
                pending.append(importer);
        }
    }

    // Groups that hold the same kinds of first words lie side by side, so that one run of equal words goes through many of them. Those without words come last.
    Vector<uint32_t> kindsOfWords(FillWith { }, firstFileOf.size(), 0u);
    for (uint32_t index = 0; index < plan.variables.size(); ++index) {
        const PlacedVariable& placed = plan.variables[index];
        ModuleID module = program.variables[index].module;
        if (placed.hasWordInStatics() && placed.initialWord && module && hasWords.quickGet(module.index()))
            kindsOfWords[groupOfModule[module.index()]] |= 1u << static_cast<unsigned>(groupOf(program.variables[index].initialization, placed));
    }
    Vector<uint32_t> found;
    for (size_t group : findsWords)
        found.append(static_cast<uint32_t>(group));
    AOT::sort(found, [&](uint32_t left, uint32_t right) {
        return std::tuple(!kindsOfWords[left], kindsOfWords[left], left) < std::tuple(!kindsOfWords[right], kindsOfWords[right], right);
    });
    Vector<uint32_t> numberOf(FillWith { }, firstFileOf.size(), none);
    for (uint32_t group : found) {
        numberOf[group] = plan.groupsOfFilesFilledWhenEntered.size();
        plan.groupsOfFilesFilledWhenEntered.append({ ModuleID(firstFileOf[group]), 0, 0, { } });
    }
    for (auto [imported, importer] : importedAndImporter) {
        if (findsWords.quickGet(imported))
            plan.groupsOfFilesFilledWhenEntered[numberOf[importer]].importedGroups.append(numberOf[imported]);
    }
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (groupOfModule[module] != none && findsWords.quickGet(groupOfModule[module]))
            plan.groupOfFile[module] = numberOf[groupOfModule[module]];
    }
    for (uint32_t index = 0; index < plan.variables.size(); ++index) {
        PlacedVariable& placed = plan.variables[index];
        ModuleID module = program.variables[index].module;
        if (placed.hasWordInStatics() && placed.initialWord && module && hasWords.quickGet(module.index()))
            placed.region = StaticsRegion::FilledWithFile;
    }

    Vector<std::optional<BitVector>> groupsFilledWith(plan.groupsOfFilesFilledWhenEntered.size());
    auto isFilledWith = [&](uint32_t filled, uint32_t entered) {
        if (filled == entered)
            return true;
        std::optional<BitVector>& known = groupsFilledWith[entered];
        if (!known) {
            known = BitVector(plan.groupsOfFilesFilledWhenEntered.size());
            pending.append(entered);
            while (!pending.isEmpty()) {
                for (uint32_t imported : plan.groupsOfFilesFilledWhenEntered[pending.takeLast()].importedGroups) {
                    if (!known->quickSet(imported))
                        pending.append(imported);
                }
            }
        }
        return known->quickGet(filled);
    };
    // Where nobody comes to a function but functions that enter files whose group brings this one's along, the words are filled by then.
    const CallGraph& callGraph = context.inputs.callGraph;
    auto isEnteredBehindFill = [&](FunctionID id, uint32_t filled) {
        if (callGraph.entrySet(id) != EntrySet::DirectCallers || callGraph.callerSet(id) != CallerSet::Complete)
            return false;
        return std::ranges::all_of(callGraph.callersOf(id), [&](FunctionID caller) {
            const Function& function = program.at(caller);
            std::optional<uint32_t> entered = entersFile(program, function) ? plan.groupOfFile[function.module.index()] : std::nullopt;
            return entered && isFilledWith(filled, *entered);
        });
    };
    plan.functionsThatEnterFiles = BitVector(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!entersFile(program, function))
            continue;
        std::optional<uint32_t> entered = plan.groupOfFile[function.module.index()];
        if (entered && !isEnteredBehindFill(FunctionID(index), *entered))
            plan.functionsThatEnterFiles.quickSet(index);
    }
    context.build.statistics().count(StatisticID::GroupsOfFilesFilledWhenEntered, plan.groupsOfFilesFilledWhenEntered.size());
    context.build.statistics().count(StatisticID::FunctionsThatFillVariablesOfFiles, plan.functionsThatEnterFiles.bitCount());
}

static Vector<PlacedVariable> createPlacedVariables(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    BitVector described = findVariablesDescribedToRunTime(program, context.inputs.flow);
    return Vector<PlacedVariable>(program.variables.size(), [&](size_t index) {
        VariableID id(index);
        PlannedVariable planned = context.slots().variable(id);
        PlacedVariable placed;
        placed.storage = planned.storage;
        placed.encoding = planned.encoding;
        placed.representation = planned.representation;
        if (planned.encoding == VariableEncoding::FunctionAsZero)
            placed.function = program.at(id).function;
        if (planned.storage == VariableStorage::Statics && !sites.variablesNamedByCode.get(index) && !described.quickGet(index))
            placed.storage = VariableStorage::None;
        if (!placed.hasWordInStatics())
            return placed;
        placed.initialWord = initialWordOf(program, id, planned);
        placed.region = !placed.initialWord || planned.isUsedBeforeProgramEntry ? StaticsRegion::FilledAtInstanceCreation : StaticsRegion::FilledAtProgramEntry;
        return placed;
    });
}

static bool hasValueWord(const PlacementContext& context, ModuleID id)
{
    switch (context.program.at(id).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return context.inputs.flow.module(id).namespaceUse != NamespaceUse::NeverCreated;
    case ModuleKind::CommonJs:
        return false;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::pair<StaticsPlan, LiteralSites> placeStatics(const PlacementContext& context, const PlacedSites& sites, GuardBitTable& guardBits)
{
    const Program& program = context.program;
    StaticsPlan plan;
    LiteralSites literals;
    uint64_t end = 0;
    uint32_t firstClearedOffset = 0;
    auto allocate = [&](uint64_t size, unsigned alignment) {
        end = WTF::roundUpToMultipleOf(alignment, end);
        return static_cast<uint32_t>(std::exchange(end, end + size));
    };
    auto allocateWord = [&] {
        return allocate(staticsWordSize, staticsWordSize);
    };
    auto allocateRoot = [&] {
        plan.roots.append(allocateWord());
        return plan.roots.last();
    };
    auto allocateEach = [&](size_t count, const auto& allocateOne) {
        return Vector<uint32_t>(count, [&](size_t) {
            return allocateOne();
        });
    };
    auto placeRegion = [&](const auto& place) {
        place();
        if (end <= context.build.limit(LimitID::MaxStaticsSize))
            return true;
        context.build.reportLimitExceeded(LimitID::MaxStaticsSize, end);
        return false;
    };

    auto placeVariables = [&] {
        plan.variables = createPlacedVariables(context, sites);
        chooseFilesFilledWhenEntered(context, plan);
        // Filled at program entry, filled with the instance, filled with their files, zero.
        constexpr unsigned firstBucketFilledWithFiles = 2 * numberOfFilledGroups;
        unsigned firstBucketOfZeroes = firstBucketFilledWithFiles + numberOfFilledGroups * plan.groupsOfFilesFilledWhenEntered.size();
        Vector<std::pair<unsigned, uint32_t>> order;
        for (uint32_t index = 0; index < plan.variables.size(); ++index) {
            const PlacedVariable& placed = plan.variables[index];
            if (!placed.hasWordInStatics())
                continue;
            unsigned group = static_cast<unsigned>(groupOf(program.variables[index].initialization, placed));
            switch (placed.region) {
            case StaticsRegion::FilledAtProgramEntry:
                order.append({ group, index });
                break;
            case StaticsRegion::FilledAtInstanceCreation:
                order.append({ group < numberOfFilledGroups ? numberOfFilledGroups + group : firstBucketOfZeroes + group, index });
                break;
            case StaticsRegion::FilledWithFile:
                order.append({ firstBucketFilledWithFiles + numberOfFilledGroups * plan.groupFilledWhenEntered(program.variables[index].module).value() + group, index });
                break;
            }
        }
        AOT::sortInLine(order);
        bool isFilledAtProgramEntry = true;
        auto finishVariablesFilledAtProgramEntry = [&] {
            if (std::exchange(isFilledAtProgramEntry, false))
                plan.sizeFilledAtProgramEntry = allocate(0, staticsWordSize);
        };
        PlacedStaticsOfFiles* file = nullptr;
        const PlacedVariable* lastVariableOfFile = nullptr;
        auto finishFile = [&] {
            PlacedStaticsOfFiles* finished = std::exchange(file, nullptr);
            if (!finished)
                return;
            unsigned width = widthOf(lastVariableOfFile->representation);
            while (end % staticsWordSize)
                plan.paddingOfFilesFilledWhenEntered.append({ allocate(width, width), lastVariableOfFile->representation, lastVariableOfFile->initialWord });
            finished->numberOfWords = allocate(0, staticsWordSize) / staticsWordSize - finished->firstWord;
        };
        bool isInFrontOfFiles = !plan.groupsOfFilesFilledWhenEntered.isEmpty();
        auto beginFiles = [&] {
            if (std::exchange(isInFrontOfFiles, false))
                plan.firstWordFilledWithFiles = allocate(0, staticsWordSize) / staticsWordSize;
        };
        bool isInFrontOfZeroes = isInFrontOfFiles;
        auto finishFiles = [&] {
            beginFiles();
            finishFile();
            if (std::exchange(isInFrontOfZeroes, false)) {
                plan.endWordFilledWithFiles = allocate(0, staticsWordSize) / staticsWordSize;
                plan.offsetOfFilledFiles = allocate(plan.groupsOfFilesFilledWhenEntered.size(), 1);
            }
        };
        for (auto [bucket, index] : order) {
            if (bucket >= numberOfFilledGroups)
                finishVariablesFilledAtProgramEntry();
            if (bucket >= firstBucketOfZeroes)
                finishFiles();
            else if (bucket >= firstBucketFilledWithFiles) {
                beginFiles();
                PlacedStaticsOfFiles& next = plan.groupsOfFilesFilledWhenEntered[(bucket - firstBucketFilledWithFiles) / numberOfFilledGroups];
                if (file != &next) {
                    finishFile();
                    file = &next;
                    next.firstWord = allocate(0, staticsWordSize) / staticsWordSize;
                }
            }
            PlacedVariable& variable = plan.variables[index];
            unsigned width = widthOf(variable.representation);
            variable.offset = allocate(width, width);
            lastVariableOfFile = &variable;
            // A root is visited as a word of eight bytes. The image lists the narrow references in a table of their own.
            SlotTraits traits = traitsOf(variable.representation);
            if (!traits.holdsCell || traits.isNarrowReference)
                continue;
            RELEASE_ASSERT(width == staticsWordSize && !(variable.offset % staticsWordSize));
            plan.roots.append(variable.offset);
        }
        finishVariablesFilledAtProgramEntry();
        finishFiles();
        uint32_t firstWordOfNext = plan.endWordFilledWithFiles;
        for (PlacedStaticsOfFiles& placed : plan.groupsOfFilesFilledWhenEntered | std::views::reverse) {
            if (!placed.numberOfWords)
                placed.firstWord = firstWordOfNext;
            firstWordOfNext = placed.firstWord;
        }
    };
    auto placeModuleValues = [&] {
        plan.moduleValueOffsets = Vector<std::optional<uint32_t>>(program.modules.size(), [&](size_t index) {
            return hasValueWord(context, ModuleID(index)) ? std::optional(allocateRoot()) : std::nullopt;
        });
    };
    auto placeGuardWords = [&] {
        Vector<PlacedGuardBit>& bits = guardBits.bits;
        if (bits.isEmpty())
            return;
        // ClassData::guardWordsOffset says "no guard words" with 0.
        if (!end)
            allocateWord();
        for (size_t first = 0; first < bits.size();) {
            uint32_t wordOffset = allocateWord();
            size_t last = first;
            while (last < bits.size() && bits[last].classLayout == bits[first].classLayout && bits[last].side == bits[first].side && (last == first || bits[last].bit % MemberGuardData::bitsPerWord))
                bits[last++].wordOffset = wordOffset;
            RELEASE_ASSERT(!bits[first].classLayout || !bits[first].bit || wordOffset == bits[first - 1].wordOffset + staticsWordSize);
            first = last;
        }
    };
    auto placeModuleBuiltinCells = [&] {
        for (const ModuleBuiltinUse& use : sites.moduleBuiltins)
            plan.moduleBuiltinCells.append({ use.builtin, use.module, allocateRoot() });
    };
    auto placeModuleNamespaces = [&] {
        plan.moduleNamespaceOffsets.fill(std::nullopt, program.modules.size());
        for (ModuleID module : sites.moduleNamespaces) {
            ModuleKind kind = program.at(module).kind;
            if (kind == ModuleKind::Runtime || kind == ModuleKind::RuntimeSource)
                plan.moduleNamespaceOffsets[module.index()] = allocateRoot();
        }
    };
    // The table of constant data begins with these nodes, in this order.
    auto placeStorageInImage = [&] {
        uint64_t next = DataNodeHeader::firstNode;
        literals.nodesOfStorageInImage = sites.arraysWithStorageInImage.map([&](DataID data) {
            return safeCast<uint32_t>(std::exchange(next, next + DataNodeHeader::numberOfWords(DataNodeHeader::Kind::Array, program.at(data).root.as<DataNode::Array>().elements.size())));
        });
    };
    auto placeLastArrays = [&] {
        uint64_t size = static_cast<uint64_t>(sites.profiledArrayLiterals.size()) * staticsWordSize;
        plan.distanceFromArrayProfileToLastArray = static_cast<uint32_t>(size);
        firstClearedOffset = allocate(size, staticsWordSize);
    };
    auto placeFreeEnvironmentCells = [&] {
        plan.freeCellOffsets = Vector<std::optional<uint32_t>>(program.layouts.size(), [&](size_t index) {
            return context.inputs.environmentReuse.isReused(LayoutID(index)) ? std::optional(allocateWord()) : std::nullopt;
        });
        if (uint64_t clearedSize = end - firstClearedOffset)
            plan.clearedEachCollection.append({ firstClearedOffset, static_cast<uint32_t>(clearedSize) });
    };

    bool fits = placeRegion(placeVariables)
        && placeRegion(placeModuleValues)
        && placeRegion(placeGuardWords)
        && placeRegion(placeModuleBuiltinCells)
        && placeRegion(placeModuleNamespaces)
        && placeRegion([&] { literals.sharedStorageOffsets = allocateEach(sites.copyOnWriteArrays.size(), allocateRoot); })
        && placeRegion(placeStorageInImage)
        && placeRegion([&] { literals.cellOffsetsOfDataMadeOnce = allocateEach(sites.dataMadeOnce.size(), allocateRoot); })
        && placeRegion([&] { literals.regExpCellOffsets = allocateEach(sites.regExpLiterals.size(), allocateRoot); })
        && placeRegion([&] { literals.templateObjectOffsets = allocateEach(sites.templateObjects.size(), allocateRoot); })
        && placeRegion([&] { literals.profiledLiteralOffsets = allocateEach(sites.profiledArrayLiterals.size(), allocateWord); })
        && placeRegion(placeLastArrays)
        && placeRegion(placeFreeEnvironmentCells);
    if (fits) {
        AOT::sort(plan.roots);
        plan.size = static_cast<uint32_t>(WTF::roundUpToMultipleOf<staticsWordSize>(end));
    }
    return { WTF::move(plan), WTF::move(literals) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
