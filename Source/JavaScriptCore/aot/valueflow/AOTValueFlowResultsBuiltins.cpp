/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowUnseenCode.h"
#include "AOTKnownClosures.h"
#include "AOTLedgerOfBuiltinObjects.h"
#include "AOTPhaseTimes.h"
#include "AOTPrimitiveKeyTexts.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSort.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowResultsPlaces.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRulesModules.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<BuiltinUse> callingBuiltinUses { BuiltinUse::Called, BuiltinUse::Constructed };
static constexpr OptionSet<BuiltinUse> accessingBuiltinUses { BuiltinUse::Read, BuiltinUse::Written };

static ValueSetID valueOfHostNode(const ValueFlowResultInputs& inputs, ValueFlowHostNode node)
{
    return inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(node));
}

static OptionSet<BuiltinUse> usesEnteringCode(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::NativeBinding:
        return callingBuiltinUses | accessingBuiltinUses;
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        return callingBuiltinUses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findBuiltinsWhoseCodeRuns(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector namespacesOfModules;
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        if (BuiltinID namespaceObject = builtins.at(RuntimeModuleID(index)).namespaceObject)
            namespacesOfModules.set(namespaceObject.index());
    }
    BitVector builtinsWhoseCodeRuns(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        OptionSet<BuiltinUse> enteringUses = usesEnteringCode(entry.kind);
        bool mayHoldFunctionsWithoutEntry = (entry.kind == BuiltinKind::Function || entry.kind == BuiltinKind::Method) && !entry.flags.contains(BuiltinFlag::ListsAllMembers);
        if (namespacesOfModules.get(index) || mayHoldFunctionsWithoutEntry)
            enteringUses.add(accessingBuiltinUses);
        if (inputs.solver().usesOf(BuiltinID(index)).containsAny(enteringUses))
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(BuiltinID(index)).index());
    }

    constexpr uint32_t objectsPerTask = 4096;
    uint32_t numberOfObjects = universe.numberOfObjectsMadeWithClassesByProgram();
    Vector<BitVector> allocatedClassesOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfObjects + objectsPerTask - 1) / objectsPerTask, [&](unsigned task, unsigned worker) {
        for (uint32_t position = task * objectsPerTask; position < std::min(numberOfObjects, (task + 1) * objectsPerTask); ++position) {
            ValueFlowObjectID object = universe.objectMadeWithClassByProgramAt(position);
            switch (universe.kindOf(object)) {
            case ValueFlowObjectKind::CoroutineResult:
            case ValueFlowObjectKind::RestArray:
            case ValueFlowObjectKind::Site:
                break;
            case ValueFlowObjectKind::Primitive:
            case ValueFlowObjectKind::UnknownObject:
            case ValueFlowObjectKind::Builtin:
            case ValueFlowObjectKind::ModuleBuiltin:
            case ValueFlowObjectKind::Namespace:
            case ValueFlowObjectKind::Closure:
            case ValueFlowObjectKind::FunctionPrototype:
            case ValueFlowObjectKind::ConstructedObject:
            case ValueFlowObjectKind::Instance:
            case ValueFlowObjectKind::Prototype:
            case ValueFlowObjectKind::ResultOfBuiltin:
            case ValueFlowObjectKind::Key:
                continue;
            }
            BuiltinClassID builtinClass = universe.classOf(object);
            if (builtinClass && inputs.isReached(universe.functionOf(object)))
                allocatedClassesOfWorker[worker].set(builtinClass.index());
        }
    });
    BitVector allocatedClasses;
    for (const BitVector& classes : allocatedClassesOfWorker)
        allocatedClasses.merge(classes);
    for (size_t index : allocatedClasses) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (BuiltinID entry = builtinClass.prototype ? builtinClass.prototype : builtinClass.constructor)
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(entry).index());
    }
    return builtinsWhoseCodeRuns;
}

static bool mayHold(const BuiltinTable& builtins, const LedgerOfBuiltinObjects& ledger, BuiltinID builtin, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    bool runsUnseenCodeForProperty = condition->hasProperty && unseenCode && unseenCode->condition && unseenCode->condition->hasProperty == condition->hasProperty;
    if (ledger.isCoveredByUnknownObject(builtin) && !condition->calledOn && !runsUnseenCodeForProperty)
        return true;
    if (condition->firstText() && isObjectKind(builtins.at(builtin).kind) && !ledger.operandMayHaveProperty(builtin, condition->firstText()))
        return false;
    return !condition->hasProperty || ledger.operandMayHaveProperty(builtin, condition->hasProperty);
}

static void forEachTypeOfValueThatOverloadMakes(const BuiltinOverload& overload, NOESCAPE const Invocable<void(BuiltinTypeID)> auto& functor)
{
    auto visit = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue && place.newValueType)
            functor(place.newValueType);
    };
    if (overload.result.type)
        functor(overload.result.type);
    for (const BuiltinTransfer& transfer : overload.flow.transfers)
        visit(transfer.from);
    for (const BuiltinCallback& callback : overload.flow.callbacks) {
        visit(callback.newTarget);
        visit(callback.thisValue);
        visit(callback.spreadArguments);
        std::ranges::for_each(callback.arguments, visit);
    }
    for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
        visit(action.value);
    for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
        visit(call.thisValue);
        std::ranges::for_each(call.arguments, visit);
    }
}

static BuiltinID outermostHolderWithFeature(const BuiltinTable& builtins, BuiltinID builtin, uint32_t feature)
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

static void markUnnamedFeatureUses(const ValueFlowResultInputs& inputs, Vector<FeatureUseRecord>& uses)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    auto candidateOf = [&](const FeatureUseRecord& use) {
        if (builtins.at(BuiltinID(use.builtin)).origin == BuiltinOrigin::Host)
            return BuiltinID();
        return outermostHolderWithFeature(builtins, BuiltinID(use.builtin), use.feature);
    };
    BitVector candidates(builtins.builtinCount());
    Vector<ValueFlowNamedBuiltins::Member> members;
    for (const FeatureUseRecord& use : uses) {
        if (BuiltinID candidate = candidateOf(use)) {
            candidates.quickSet(candidate.index());
            members.append({ BuiltinID(use.builtin), candidate });
        }
    }
    ValueFlowNamedBuiltins named(inputs, candidates, members.span());
    for (FeatureUseRecord& use : uses) {
        if (BuiltinID candidate = candidateOf(use); candidate && !named.contains(candidate))
            use.kind = FeatureUseKind::Unnamed;
    }
}

static Vector<FeatureUseRecord> collectFeatureUses(const ValueFlowResultInputs& inputs, const LedgerOfBuiltinObjects& ledger, const BitVector& builtinsWhoseCodeRuns)
{
    constexpr uint32_t noClass = UINT32_MAX;
    const BuiltinTable& builtins = inputs.program.builtins();
    ValueSetID namedByStatements = valueOfHostNode(inputs, ValueFlowHostNode::BuiltinsThatStatementsName);
    auto isNamedByStatement = [&](BuiltinID builtin) {
        return inputs.sets().contains(namedByStatements, inputs.universe().objectOf(builtin));
    };
    auto canonical = [&](BuiltinID builtin) {
        return builtins.canonicalID(builtin).index();
    };

    BitVector makersNameFeatures(builtins.classCount());
    Vector<uint32_t> classOfBuiltin;
    classOfBuiltin.fill(noClass, builtins.builtinCount());
    Vector<uint32_t> hostClassOfPrototype;
    hostClassOfPrototype.fill(noClass, builtins.builtinCount());
    BitVector hangsOutsidePrototypeObjects(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder = builtins.at(BuiltinID(index)).holder;
        if (holder && builtins.at(holder).kind != BuiltinKind::Prototype)
            hangsOutsidePrototypeObjects.quickSet(canonical(BuiltinID(index)));
    }
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (builtinClass.origin != BuiltinOrigin::Host || !builtinClass.prototype)
            continue;
        hostClassOfPrototype[canonical(builtinClass.prototype)] = index;
        if (builtinClass.instancesAreEntries)
            continue;
        bool canBeNamed = builtinClass.constructor && hangsOutsidePrototypeObjects.quickGet(canonical(builtinClass.constructor));
        if (canBeNamed && (isNamedByStatement(builtinClass.constructor) || isNamedByStatement(builtinClass.prototype)))
            continue;
        makersNameFeatures.quickSet(index);
        classOfBuiltin[canonical(builtinClass.prototype)] = index;
        if (builtinClass.constructor && !canBeNamed)
            classOfBuiltin[canonical(builtinClass.constructor)] = index;
    }
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder = builtins.at(BuiltinID(index)).holder;
        if (holder && classOfBuiltin[canonical(BuiltinID(index))] == noClass)
            classOfBuiltin[canonical(BuiltinID(index))] = classOfBuiltin[canonical(holder)];
    }
    auto needsWithoutCondition = [&](BuiltinID builtin, RemovableFeature feature) {
        return std::ranges::any_of(builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) { return line.feature == feature && !line.condition; });
    };
    auto prototypeOf = [&](uint32_t builtinClass) {
        return builtins.canonicalID(builtins.at(BuiltinClassID(builtinClass)).prototype);
    };
    auto checkMakerOf = [&](uint32_t made, BuiltinID maker) {
        uint32_t owner = classOfBuiltin[maker.index()];
        if (!makersNameFeatures.quickGet(made) || owner == made)
            return;
        bool namesFeatures = std::ranges::all_of(builtins.featureUsesOf(prototypeOf(made)), [&](const BuiltinFeatureUse& line) {
            return line.condition || needsWithoutCondition(maker, line.feature) || (owner != noClass && needsWithoutCondition(prototypeOf(owner), line.feature));
        });
        makersNameFeatures.quickSet(made, namesFeatures);
    };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID maker = builtins.canonicalID(BuiltinID(index));
        Vector<BuiltinTypeID, 8> types;
        for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index)))
            forEachTypeOfValueThatOverloadMakes(overload, [&](BuiltinTypeID type) { types.appendIfNotContains(type); });
        for (size_t position = 0; position < types.size(); ++position) {
            for (const BuiltinTypePart& part : builtins.at(types[position]).parts) {
                for (BuiltinClassID current = part.builtinClass; current; current = builtins.at(current).parent)
                    checkMakerOf(current.index(), maker);
                if (part.builtinClass && !part.isOfClassItself) {
                    for (BuiltinClassID below : inputs.universe().builtinTables().classesAtOrBelow(part.builtinClass)) {
                        if (below != part.builtinClass)
                            checkMakerOf(below.index(), maker);
                    }
                }
                for (const BuiltinSlotContents& contents : part.contents) {
                    if (contents.type)
                        types.appendIfNotContains(contents.type);
                }
            }
        }
    }

    auto entryWithFeaturesOfClassOf = [&](const Builtin& member) -> BuiltinID {
        if (!member.holder || (member.kind != BuiltinKind::Method && member.kind != BuiltinKind::Accessor))
            return { };
        uint32_t owner = hostClassOfPrototype[canonical(member.holder)];
        if (owner == noClass)
            return { };
        BuiltinID constructor = builtins.at(BuiltinClassID(owner)).constructor;
        if (!builtins.featureUsesOf(prototypeOf(owner)).empty() || !constructor)
            return prototypeOf(owner);
        return builtins.canonicalID(constructor);
    };

    Vector<FeatureUseRecord> uses;
    for (size_t index : builtinsWhoseCodeRuns) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (entry.flags.contains(BuiltinFlag::EvaluatesText) && refusesAllSourceText(builtins, BuiltinID(index), inputs.solver().input().refusedCode))
            continue;
        uint32_t owner = classOfBuiltin[index];
        auto addUnlessRootedByMakersOfClass = [&](RemovableFeature feature) {
            if (owner == noClass || !makersNameFeatures.quickGet(owner) || !needsWithoutCondition(prototypeOf(owner), feature))
                uses.append(FeatureUseRecord { static_cast<uint32_t>(feature), static_cast<uint32_t>(index), noWitness });
        };
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index))) {
            if (mayHold(builtins, ledger, BuiltinID(index), line.condition))
                addUnlessRootedByMakersOfClass(line.feature);
        }
        BuiltinID entryOfClass = entryWithFeaturesOfClassOf(entry);
        if (!entryOfClass || !isNamedByStatement(builtins.canonicalID(entry.holder)))
            continue;
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(entryOfClass)) {
            if (!line.condition)
                addUnlessRootedByMakersOfClass(line.feature);
        }
    }
    auto orderOf = [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.builtin);
    };
    AOT::sortInLine(uses, { }, orderOf);
    uses.shrink(std::ranges::unique(uses, { }, orderOf).begin() - uses.begin());
    markUnnamedFeatureUses(inputs, uses);
    return uses;
}

static void sortAndRemoveRepeatedRecords(Vector<UnseenCodeRecord>& records)
{
    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    AOT::sort(records, { }, orderOf);
    records.shrink(std::ranges::unique(records, { }, orderOf).begin() - records.begin());
}

static Vector<UnseenCodeRecord> collectUnseenCode(const ValueFlowResultInputs& inputs, Vector<UnseenCodeRecord>& loadsOfMachineCode)
{
    bool unseenCodeRuns = unseenCodeCanRun(inputs.solution);
    bool modulesAreNamedAtRunTime = modulesAreLoadedByName(inputs.solution);
    Vector<UnseenCodeRecord> records = unseenCodeRecordsOf(inputs.solution);
    records.removeAllMatching([&](const UnseenCodeRecord& record) {
        if (record.cause == UnseenCodeCause::RunsMachineCode) {
            loadsOfMachineCode.append(record);
            return true;
        }
        return admitsUnknownJavaScript(record.cause) ? !unseenCodeRuns : !modulesAreNamedAtRunTime;
    });
    if (unseenCodeRuns && !std::ranges::any_of(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::RunsUnseenCode });
    bool codeOutsideImageIsLoaded = inputs.solver().input().refusedCode.hostRunsFilesOutsideImage || (filesOfProgramAreLoadedByName(inputs.solution) && !inputs.solver().input().refusedCode.codeOutsideImage);
    if (codeOutsideImageIsLoaded && !std::ranges::any_of(records, loadsCodeOutsideImage, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsCodeByName });
    if (modulesAreNamedAtRunTime && !std::ranges::any_of(records, loadsModulesByName, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsModuleByName });
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });
    sortAndRemoveRepeatedRecords(records);
    return records;
}

static bool isCallOfProgramUnderComputedName(const Program& program, const Witness& witness)
{
    const Function& function = program.at(FunctionID(witness.function));
    const Value& call = function.at(ValueID(witness.value));
    constexpr unsigned positionOfName = 2;
    if (!isWrittenInProgram(function) || call.opcode != Opcode::Call || call.numberOfOperands() <= positionOfName || !call.operand(positionOfName))
        return false;
    return function.at(definitionThroughSameValues(function, call.operand(positionOfName))).opcode != Opcode::String;
}

// The front end leaves a constant path to the runtime if no such file exists at build time.
static bool isLoadUnderConstantName(const Program& program, const Witness& witness)
{
    const Function& function = program.at(FunctionID(witness.function));
    const Value& load = function.at(ValueID(witness.value));
    if (load.opcode != Opcode::ImportDynamic && load.opcode != Opcode::RequireDynamic)
        return false;
    return function.at(definitionThroughSameValues(function, load.specifierOperand())).opcode == Opcode::String;
}

// `for (const name of ["os", "./plugin.ts"]) await import(name)`: the build knows every name, and each finds a module of the image.
static bool findsModuleUnderEveryName(const ValueFlowResultInputs& inputs, const Witness& witness, bool isCallOfLoader)
{
    const Program& program = inputs.program;
    FunctionID function(witness.function);
    const Value& load = program.at(function).at(ValueID(witness.value));
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(function.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.site != ValueID(witness.value))
            continue;
        size_t indexOfName = isCallOfLoader ? 2 : 0;
        if (statement.operandCount <= indexOfName)
            return false;
        BorrowedValueSetMaker maker(inputs.makerPool());
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        auto valuesOf = [&](const ValueFlowOperand& operand) {
            return ValueFlowRuleContext::valueShownToRules(operand, inputs.solver(), maker.get());
        };
        auto findsEveryNameFrom = [&](ModuleID referrer, bool takesStringsAlone) {
            return modulesThatNamesFind(program, inputs.universe(), inputs.solver().input().refusedCode, { function, load.operand(indexOfName), referrer, takesStringsAlone }, [&](const ScopedLambda<bool(ValueFlowObjectID)>& functor) {
                inputs.sets().anyMember(valuesOf(operands[indexOfName]), functor);
            }).findsEveryName();
        };
        if (!isCallOfLoader)
            return findsEveryNameFrom(load.referrer(), load.opcode == Opcode::RequireDynamic);
        const BuiltinTable& builtins = program.builtins();
        return !inputs.sets().anyMember(valuesOf(operands[0]), [&](ValueFlowObjectID loader) {
            BuiltinID builtin = inputs.universe().builtinOf(loader);
            return !findsEveryNameFrom(inputs.universe().moduleOf(loader), builtin && builtins.canonicalID(builtin) == Builtins::ModuleRequire);
        });
    }
    return false;
}

static void warnAboutLoadsUnderComputedNames(const ValueFlowResultInputs& inputs, std::span<const UnseenCodeRecord> records)
{
    const Program& program = inputs.program;
    BuildContext& build = inputs.build;
    Vector<SourceLocation> reportedLocations;
    for (const UnseenCodeRecord& record : records) {
        if (record.cause != UnseenCodeCause::LoadsModuleByName || record.witness.value == Wire::none)
            continue;
        bool isCallOfLoader = record.builtin != Wire::none;
        if (isCallOfLoader ? !isCallOfProgramUnderComputedName(program, record.witness) : isLoadUnderConstantName(program, record.witness))
            continue;
        if (findsModuleUnderEveryName(inputs, record.witness, isCallOfLoader))
            continue;
        if (hasModule(record.witness.location)) {
            if (reportedLocations.contains(record.witness.location))
                continue;
            reportedLocations.append(record.witness.location);
        }
        build.diagnostics().add(UserDiagnostic {
            .kind = isCallOfLoader ? UserDiagnosticKind::ModuleNamedAtRunTimeIsRefused : UserDiagnosticKind::CodeOutsideImageIsRefused,
            .function = FunctionID(record.witness.function),
            .value = ValueID(record.witness.value),
            .location = record.witness.location,
        });
    }
}

namespace {

struct BuiltinLocationClaim {
    BuiltinLocationState state { BuiltinLocationState::Unknown };
    UnprovenCause cause { UnprovenCause::Unknown };
};

} // anonymous namespace

static constexpr BuiltinLocationClaim unknownLocation(UnprovenCause cause) { return { BuiltinLocationState::Unknown, cause }; }

static UnprovenCause causeOf(const Write& write)
{
    switch (write.form) {
    case WriteForm::Delete:
        return UnprovenCause::FieldDeleted;
    case WriteForm::Define:
    case WriteForm::DefineAccessor:
        return UnprovenCause::PropertyRedefined;
    case WriteForm::Link:
        return UnprovenCause::PrototypeChanged;
    case WriteForm::Freeze:
        return UnprovenCause::IntegrityChanged;
    case WriteForm::AtCreation:
    case WriteForm::Store:
        return UnprovenCause::StoreOfOtherType;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinLocationClaim stateOfLocation(const Writers& writers)
{
    auto all = writers.all();
    if (!all)
        return unknownLocation(causeOf(writers.open()));
    if (!all->value().empty())
        return unknownLocation(causeOf(all->value().front()));
    return { BuiltinLocationState::Constant, UnprovenCause::Unknown };
}

static bool isIntact(const Writers& ofProperties, const Writers& ofPrototypeLink)
{
    auto links = ofPrototypeLink.all();
    return ofProperties.all() && links && links->value().empty();
}

static Expected<bool> isNoWriteFollowedTo(const BuiltinTable& builtins, BuiltinID member, OptionSet<BuiltinUse> uses, const Writers& writers)
{
    // Unseen code hands the objects of the realm around, and declares globals. There a write through a value nobody knows counts for the members that the program names.
    if (writers.open().contains(Actor::UnseenCode))
        return !uses.contains(BuiltinUse::Written) && builtins.canonicalID(builtins.at(member).holder) != Builtins::GlobalThis;
    return writers.seen().predicted().empty();
}

// A member that the program uses, and that no write was followed to, is worth a lock. A write that comes all the same is refused.
static Expected<bool> isWorthLockingAtStartUp(const BuiltinTable& builtins, BuiltinID member, OptionSet<BuiltinUse> uses, const Writers& writers)
{
    if (uses.isEmpty() || !canLockAtStartUp(builtins, member))
        return false;
    return isNoWriteFollowedTo(builtins, member, uses, writers);
}

// Adds the locations that say where they are written, and keeps one record for each place.
static Vector<BuiltinLocationFlow> mergeWrites(Vector<BuiltinLocationFlow> writes, std::span<const BuiltinLocationFlow> locations)
{
    auto placeOfWrite = [](const BuiltinLocationFlow& write) {
        return std::tuple(write.holder, write.key, write.witness.function, write.witness.location.module, write.witness.location.position);
    };
    for (const BuiltinLocationFlow& location : locations) {
        if (hasFunction(location.witness))
            writes.append(location);
    }
    AOT::sort(writes, { }, [&](const BuiltinLocationFlow& write) {
        return std::tuple(placeOfWrite(write), write.witness.value);
    });
    writes.shrink(writes.size() - std::ranges::unique(writes, { }, placeOfWrite).size());
    return writes;
}

static void sortAndMergeLocations(Vector<BuiltinLocationFlow>& locations)
{
    auto orderOf = [](const BuiltinLocationFlow& location) {
        return std::tuple(location.holder, location.key);
    };
    AOT::stableSortInLine(locations, { }, orderOf);
    size_t size = 0;
    for (const BuiltinLocationFlow& location : locations) {
        if (!size || orderOf(locations[size - 1]) != orderOf(location)) {
            locations[size++] = location;
            continue;
        }
        if (location.state == BuiltinLocationState::Unknown)
            locations[size - 1].state = BuiltinLocationState::Unknown;
        keepLowerWitness(locations[size - 1].witness, location.witness);
    }
    locations.shrink(size);
}

static std::optional<unsigned> computedKeyOfWrite(const Value& operation)
{
    if (operation.opcode == Opcode::ElemSet || operation.opcode == Opcode::ElemDefine || operation.opcode == Opcode::ElemDelete)
        return OperandPosition::property(operation.opcode);
    if ((operation.opcode == Opcode::DefineGetter || operation.opcode == Opcode::DefineSetter) && !operation.key())
        return OperandPosition::computedKey(operation.opcode);
    return std::nullopt;
}

static bool writesUnderKey(BuiltinPropertyActionKind kind)
{
    switch (kind) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        return true;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
    case BuiltinPropertyActionKind::DefinesFrom:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinCallMode> modeOfCallOperation(Opcode opcode)
{
    if (opcode == Opcode::Call || opcode == Opcode::CallList)
        return BuiltinCallMode::Call;
    if (opcode == Opcode::Construct || opcode == Opcode::ConstructList)
        return BuiltinCallMode::Construct;
    return std::nullopt;
}

static ValueID valuePassedAt(const Value& call, BuiltinOperand position)
{
    if ((call.opcode != Opcode::Call && call.opcode != Opcode::Construct) || position.kind() == BuiltinOperand::Kind::ArgumentsFrom)
        return { };
    unsigned index = position.isReceiver() ? 1 : 2 + position.argumentIndex();
    return index < call.numberOfOperands() ? call.operand(index) : ValueID();
}

static bool mayWriteLockedLocationUnderKeyNobodyKnows(const BuiltinTable& builtins, const LedgerOfBuiltinObjects& ledger)
{
    bool mayWrite = false;
    ledger.forEachHolderWrittenUnderKeysNobodyKnows([&](BuiltinID holder) {
        mayWrite = mayWrite || builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames }) || !builtins.closedKeysOf(holder).empty()
            || std::ranges::any_of(builtins.membersOf(holder), [&](BuiltinID member) { return isLockedLocation(builtins, member); });
    });
    return mayWrite;
}

static Vector<ValueFlowWriteUnderNewName> collectWritesOfLockedLocations(const ValueFlowResultInputs& inputs, const LedgerOfBuiltinObjects& holders, ValueFlowHeapView& heap, bool writesLockedLocation, Vector<BuiltinLocationFlow>& locations)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    PropertyKeyID keyOfPrototypeLink = program.keyOf(BuiltinNames::__proto__);
    holders.forEachHolderOfWrittenPrototypeLink([&](BuiltinID holder) {
        writesLockedLocation |= isLockedLocation(program, holder, keyOfPrototypeLink) || builtins.isNativeFunction(holder);
    });
    if (!writesLockedLocation && !mayWriteLockedLocationUnderKeyNobodyKnows(builtins, holders))
        return { };

    struct Write {
        BuiltinID holder;
        PropertyKeyID key;
        StringID text;
        Witness witness;
    };
    struct Notes {
        ValueSetMaker& maker;
        Vector<Write>& writes;
        // A store under `__proto__` goes to the setter of Object.prototype. A definition makes an own property of that name.
        bool changesPrototypeUnderItsName { false };
    };
    auto listsItsObjects = [&](ValueSetID objects) {
        return !ValueSets::isComplement(objects) && !sets.meet(objects, universe.objectsThatMayBeAnything());
    };
    auto spelledOutTextsOf = [&](FunctionID function, ValueID keyValue) -> std::span<const StringID> {
        return keyValue ? program.typeOf(program.at(function), keyValue).stringLiterals.span() : std::span<const StringID>();
    };
    auto note = [&](const Notes& notes, ValueSetID receivers, PropertyKeyID key, const Witness& witness) {
        if (notes.changesPrototypeUnderItsName && key == keyOfPrototypeLink) {
            for (BuiltinID holder : holders.holdersIn(notes.maker, notes.maker.intersect(receivers, universe.builtinFunctions()))) {
                if (builtins.isNativeFunction(holder) && !isLockedLocation(program, holder, key))
                    notes.writes.append({ holder, key, { }, witness });
            }
        }
        const ValueFlowHeapTables& tables = universe.builtinTables().heap();
        ValueSetID holdersThatMayLockKey = tables.under(key).holdersWithLockedLocation;
        if (program.isIndexLike(key))
            holdersThatMayLockKey = notes.maker.unite(holdersThatMayLockKey, tables.holdersWithLockedIndexes());
        if (!sets.meet(receivers, holdersThatMayLockKey))
            return;
        for (BuiltinID holder : holders.holdersIn(notes.maker, notes.maker.intersect(receivers, holdersThatMayLockKey))) {
            if (isLockedLocation(program, holder, key))
                notes.writes.append({ holder, key, { }, witness });
        }
    };
    auto noteUnderText = [&](const Notes& notes, ValueSetID receivers, StringID text, const Witness& witness) {
        if (std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text })) {
            note(notes, receivers, *key, witness);
            return;
        }
        for (BuiltinID holder : holders.holdersIn(notes.maker, receivers)) {
            if (builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || builtins.closesIndexKeys(holder))
                notes.writes.append({ holder, { }, text, witness });
        }
    };
    auto noteUnderNamesAmong = [&](const Notes& notes, ValueSetID keys, FunctionID function, ValueID keyValue, ValueSetID receivers, const Witness& witness) {
        if (ValueSets::isComplement(keys))
            return;
        std::span<const StringID> spelledOutTexts = spelledOutTextsOf(function, keyValue);
        Vector<ValueFlowObjectID, 8> primitives;
        for (auto [primitive, text] : primitiveKeyTexts)
            primitives.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive)));
        if (!spelledOutTexts.empty() && !(program.typeOf(program.at(function), keyValue).primitives & Primitive::String))
            primitives.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        if (!sets.isSubset(keys, notes.maker.unite(universe.keys(), notes.maker.of(primitives.span()))))
            return;
        ValueSetID namesAmongKeys = notes.maker.intersect(keys, universe.keys());
        if (sets.size(namesAmongKeys) <= inputs.build.limit(LimitID::MaxValueFlowMembersToldApart)) {
            sets.forEachMember(namesAmongKeys, [&](ValueFlowObjectID key) {
                if (universe.kindOf(key) == ValueFlowObjectKind::Key)
                    note(notes, receivers, universe.keyOf(key), witness);
            });
        }
        for (auto [primitive, text] : primitiveKeyTexts) {
            StringID string = sets.contains(keys, ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive))) ? program.strings.find(byteCast<uint8_t>(text.span())) : StringID();
            if (string)
                noteUnderText(notes, receivers, string, witness);
        }
        if (!sets.contains(keys, ValueFlowUniverse::primitive(PrimitiveSet::String)))
            return;
        for (StringID text : spelledOutTexts)
            noteUnderText(notes, receivers, text, witness);
    };

    ValueSetMakers makers(inputs);
    Vector<Write> writes = collectWithMakers<Write>(inputs, makers, program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<Write>& found) {
        if (!inputs.isReached(FunctionID(function)))
            return;
        for (const ValueFlowStatement& statement : bodies.statementsOf(function)) {
            Notes notes { maker, found, statement.opcode == Opcode::PropSet || statement.opcode == Opcode::ElemSet };
            std::optional<unsigned> object = statement.kind == ValueFlowStatementKind::Operation && statement.site && !modeOfCallOperation(statement.opcode) ? OperandPosition::object(statement.opcode) : std::nullopt;
            std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
            if (!object || !listsItsObjects(heap.possibleValuesOf(maker, operands[*object])))
                continue;
            Witness witness = witnessOfStatement(program, statement);
            ValueFlowStatementID id(static_cast<uint32_t>(&statement - bodies.statements().data()));
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
                if (shapeOf(key.name().kind).reach == ValueFlowNameReach::OneKey)
                    note(notes, key.set, PropertyKeyID(key.payload), witness);
            });
            const Value& operation = program.at(statement.function).at(statement.site);
            if (operation.opcode == Opcode::PropSet && operation.key() == keyOfPrototypeLink)
                note(notes, heap.possibleValuesOf(maker, operands[*object]), keyOfPrototypeLink, witness);
            if (std::optional<unsigned> key = computedKeyOfWrite(operation))
                noteUnderNamesAmong(notes, heap.possibleValuesOf(maker, operands[*key]), statement.function, operation.operand(*key), heap.possibleValuesOf(maker, operands[*object]), witness);
        }
    });

    auto siteOf = [](FunctionID function, ValueID site) {
        return static_cast<uint64_t>(function.index()) << 32 | site.index();
    };
    std::span<const ValueFlowHeapIndex::CallOfValue> calls = heap.index().callsThatMayApplyBuiltins();
    ValueSetID callablesThatAreNoBuiltinFunctions = heap.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctions()));
    Vector<uint64_t> callsOfBuiltinFunctionsAlone = WTF::map(findIndicesInParallel(inputs.loops(), calls.size(), [&](unsigned index) {
        return modeOfCallOperation(bodies.statements()[calls[index].statement].opcode) && !ValueSets::isComplement(calls[index].callees) && !sets.meet(calls[index].callees, callablesThatAreNoBuiltinFunctions);
    }), [&](uint32_t index) {
        const ValueFlowStatement& call = bodies.statements()[calls[index].statement];
        return siteOf(call.function, call.site);
    });
    AOT::sort(callsOfBuiltinFunctionsAlone);
    BitVector builtinsThatWriteUnderKeys = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, [](BuiltinPropertyActionKind kind) {
            return writesUnderKey(kind) || kind == BuiltinPropertyActionKind::ChangesPrototype;
        }, &BuiltinPropertyAction::kind);
    });
    Notes notes { heap.maker(), writes };
    forEachBuiltinApplication(inputs, heap, builtinsThatWriteUnderKeys, [&](const ValueFlowBuiltinApplication& application) {
        if (!application.site || !builtinsThatWriteUnderKeys.quickGet(application.builtin.index()) || !std::ranges::binary_search(callsOfBuiltinFunctionsAlone, siteOf(application.function, application.site)))
            return;
        const Function& function = program.at(application.function);
        Witness witness { application.function.index(), application.site.index(), function.locationOf(application.site) };
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                bool changesPrototype = action.kind == BuiltinPropertyActionKind::ChangesPrototype;
                if (!writesUnderKey(action.kind) && !changesPrototype)
                    continue;
                ValueSetID receivers = application.valueAt(action.object, notes.maker);
                if (!listsItsObjects(receivers))
                    continue;
                if (changesPrototype) {
                    note(Notes { notes.maker, notes.writes, true }, receivers, keyOfPrototypeLink, witness);
                    continue;
                }
                switch (action.key.kind) {
                case BuiltinKeyKind::Names:
                    for (BuiltinName name : action.key.names)
                        note(notes, receivers, program.keyOf(name), witness);
                    break;
                case BuiltinKeyKind::Operand:
                    noteUnderNamesAmong(notes, application.valueAt(*action.key.operand, notes.maker), application.function, valuePassedAt(function.at(application.site), *action.key.operand), receivers, witness);
                    break;
                case BuiltinKeyKind::None:
                case BuiltinKeyKind::Any:
                case BuiltinKeyKind::Indexes:
                case BuiltinKeyKind::ElementsOf:
                    break;
                }
            }
        }
    });

    Vector<ValueFlowWriteUnderNewName> underNewNames;
    for (const Write& write : writes) {
        if (write.key)
            locations.append(BuiltinLocationFlow { write.holder, write.key, BuiltinLocationState::Constant, write.witness });
        else
            underNewNames.append({ write.holder, write.text, write.witness });
    }
    return underNewNames;
}

static Vector<ValueFlowWriteUnderNewName> computeBuiltinLocations(const ValueFlowResultInputs& inputs, const LedgerOfBuiltinObjects& holders, ValueFlowHeapView& heap, FlowBuiltinTables& tables)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    tables.constantMembers = BitVector(builtins.builtinCount());
    tables.intactHolders = BitVector(builtins.builtinCount());
    tables.membersLockedAtStartUp = BitVector(builtins.builtinCount());
    tables.membersWrittenInSharedRealm = BitVector(builtins.builtinCount());
    ValueSetID storedByName = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::MembersStoredByNameThroughPlaceholders));
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        bool isEnabled = inputs.build.isEnabled(FlowResultKind::ConstantBuiltinLocations, index);
        BuiltinID canonical = builtins.canonicalID(member);
        bool takesNameFromTests = std::ranges::contains(program.openedNames, canonical, &OpenedName::holder);
        if (isEnabled && !takesNameFromTests && isIntact(holders.writersOfEveryOwnProperty(canonical), holders.writersOfPrototypeLink(canonical)))
            tables.intactHolders.quickSet(index);
        const Builtin& entry = builtins.at(member);
        if (!entry.holder || !entry.name)
            continue;
        OptionSet<BuiltinUse> uses = inputs.solver().usesOf(member);
        // A store that names the member may reach its holder through a receiver that flow does not list. The ledger has no writer for it.
        bool isStoredThroughUnknownReceiver = canLockAtStartUp(builtins, member) && inputs.sets().contains(storedByName, inputs.universe().objectOf(canonical));
        // First the choice, which the image carries out. Then the claim, which rests on the record of the choice.
        Writers withoutLocks = holders.writersOfMemberWithoutLocks(member);
        // The run time leaves it open for the tests, whether or not a write was followed to it.
        bool isUnlocked = program.isUnlocked(member);
        if (isEnabled && !isStoredThroughUnknownReceiver && !isUnlocked && isWorthLockingAtStartUp(builtins, member, uses, withoutLocks).predicted())
            tables.membersLockedAtStartUp.quickSet(index);
        if (inputs.build.options().sharesRealms() && !isUnlocked && canLockAtStartUp(builtins, member) && (isStoredThroughUnknownReceiver || !isNoWriteFollowedTo(builtins, member, uses, withoutLocks).predicted()))
            tables.membersWrittenInSharedRealm.quickSet(index);
        BuiltinLocationClaim claim = isEnabled || Locks::byTable(builtins, member) ? stateOfLocation(holders.behindLocks(member, withoutLocks)) : unknownLocation(UnprovenCause::ResultDisabled);
        if (isStoredThroughUnknownReceiver || isUnlocked)
            claim = unknownLocation(UnprovenCause::UnknownWriter);
        tables.constantMembers.quickSet(index, claim.state == BuiltinLocationState::Constant && !builtins.has(BuiltinMark::MayBeAbsent, member));
        if (!uses.isEmpty())
            tables.locations.append(BuiltinLocationFlow { entry.holder, program.keyOf(entry.name), claim.state, noWitness });
    }
    bool writesLockedLocation = false;
    PropertyKeyID keyOfPrototypeLink = program.keyOf(BuiltinNames::__proto__);
    holders.forEachNameWritten([&](BuiltinID holder, PropertyKeyID key) {
        writesLockedLocation |= key == keyOfPrototypeLink && builtins.isNativeFunction(holder);
        if (isLockedLocation(program, holder, key)) {
            tables.locations.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Constant, noWitness });
            writesLockedLocation = true;
        } else if (!findOwnMember(program, holder, key))
            tables.locations.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Unknown, noWitness });
    });
    Vector<ValueFlowWriteUnderNewName> underNewNames = collectWritesOfLockedLocations(inputs, holders, heap, writesLockedLocation, tables.locations);
    tables.writesOfLocations = mergeWrites(WTF::move(tables.writesOfLocations), tables.locations.span());
    sortAndMergeLocations(tables.locations);
    return underNewNames;
}

void recordLockedWritesUnderNewNames(const Program& program, TypeTable& types, std::span<const ValueFlowWriteUnderNewName> writes, FlowResultTables& tables)
{
    if (writes.empty())
        return;
    for (const ValueFlowWriteUnderNewName& write : writes) {
        PropertyKeyID key = types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = write.text });
        if (isLockedLocation(program, write.holder, key))
            tables.builtins.locations.append(BuiltinLocationFlow { write.holder, key, BuiltinLocationState::Constant, write.witness });
    }
    tables.builtins.writesOfLocations = mergeWrites(WTF::move(tables.builtins.writesOfLocations), tables.builtins.locations.span());
    sortAndMergeLocations(tables.builtins.locations);
}

static ProgramInstances countProgramInstances(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions) || inputs.build.options().sharesRealms())
        return ProgramInstances::Unknown;
    bool unseenCodeFindsEveryBuiltin = unseenCodeHasLoader(inputs.solution);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (!builtins.at(builtin).flags.contains(BuiltinFlag::StartsInstance) && !builtins.at(builtins.canonicalID(builtin)).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        ValueFlowObjectID object = inputs.universe().objectOf(builtin);
        if (unseenCodeFindsEveryBuiltin || inputs.solver().usesOf(builtin).containsAny(usesEnteringCode(builtins.at(builtin).kind)) || (object && (inputs.hasEscaped(object) || inputs.isLost(object))))
            return ProgramInstances::Unknown;
    }
    return ProgramInstances::AtMostOne;
}

static LegacyRegExpStatics findReadsOfLegacyRegExpStatics(const ValueFlowResultInputs& inputs, const LedgerOfBuiltinObjects& ledger)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BuiltinID constructor = builtins.canonicalID(builtins.at(BuiltinClasses::RegExp).constructor);
    if (!inputs.build.isEnabled(FlowResultKind::ObjectUses, constructor.index()) || !ledger.reachersOf(constructor).all())
        return LegacyRegExpStatics::Unknown;
    bool isMentioned = std::ranges::any_of(builtins.membersOf(constructor), [&](BuiltinID member) {
        return builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn && (!inputs.solver().usesOf(member).isEmpty() || !inputs.solver().usesOf(builtins.canonicalID(member)).isEmpty());
    });
    return isMentioned ? LegacyRegExpStatics::Unknown : LegacyRegExpStatics::NeverRead;
}

void findHoldersWithListedKeys(const ValueFlowResultInputs& inputs, const ValueFlowUsesOfHeap& usesOfHeap, FlowBuiltinTables& tables)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BorrowedValueSetMaker maker(inputs.makerPool());
    auto forEachHolderIn = [&](ValueSetID objects, NOESCAPE const Invocable<void(BuiltinID)> auto& functor) {
        inputs.sets().forEachMemberInLine(maker.get().intersect(objects, inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
            functor(builtins.canonicalID(inputs.universe().builtinOf(object)));
        });
    };
    auto noteHolder = [&](BuiltinID holder) {
        tables.holdersWithListedKeys.set(holder.index());
    };
    forEachHolderIn(objectsWithAllKeysShown(usesOfHeap), noteHolder);
    forEachHolderIn(objectsWithEnumerableKeysShown(usesOfHeap), [&](BuiltinID holder) {
        bool showsOrder = std::ranges::any_of(builtins.membersOf(holder), [&](BuiltinID member) {
            return builtins.at(member).isAddedAsEnumerable;
        });
        if (showsOrder)
            noteHolder(holder);
    });
    // Code that nobody saw finds every object of the realm. Without such code an object that went out of sight is listed only where it comes back, and there it is an operand that nobody knows.
    if (unseenCodeCanRun(inputs.solution))
        forEachHolderIn(inputs.universe().builtinObjects(), noteHolder);
}

ValueFlowClassesOfBuiltinResults computeClassesOfBuiltinResults(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    using Noted = std::pair<ValueFlowObjectID, BuiltinClassID>;
    ValueSetMakers makers(inputs);
    const ValueFlowHeapIndex::NodesWrittenUnderName* written = heapIndex.findNodesWrittenUnder(allocatedAsName);
#if ASSERT_ENABLED
    BitVector hasNotedAllocation;
    for (const ValueFlowHeapIndex::WrittenNode& node : written ? written->nodes.span() : std::span<const ValueFlowHeapIndex::WrittenNode>()) {
        if (ValueSets::isComplement(node.objects) || node.value == ValueSets::nothing)
            continue;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
            hasNotedAllocation.set(object.index());
        });
    }
#endif
    Vector<Noted> noted = collectWithMakers<Noted>(inputs, makers, inputs.program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<Noted>& found) {
        if (!inputs.isReached(FunctionID(function)))
            return;
        for (const ValueFlowStatement& call : inputs.bodies().statementsOf(function)) {
            std::optional<BuiltinCallMode> mode = call.kind == ValueFlowStatementKind::Operation ? modeOfCallOperation(call.opcode) : std::nullopt;
            ValueFlowObjectID object = mode ? universe.siteObject(call) : ValueFlowObjectID();
            if (!object || universe.layoutOf(object))
                continue;
            ValueSetID callees = inputs.possibleValuesOf(maker, inputs.bodies().operandsOf(call)[0]);
            if (ValueSets::isComplement(callees) || inputs.sets().meet(callees, universe.otherCallables())) {
                found.append({ object, { } });
                continue;
            }
            ValueSetID builtinCallees = maker.intersect(callees, universe.builtinFunctions());
            // `m.call(o)`: a built-in m makes the object of this call.
            ValueFlowObjectID callOfFunctions = universe.objectOf(builtins.canonicalID(Builtins::FunctionPrototypeCall));
            if (inputs.sets().contains(builtinCallees, callOfFunctions) && inputs.bodies().operandsOf(call).size() > 2) {
                ValueSetID forwarded = inputs.possibleValuesOf(maker, inputs.bodies().operandsOf(call)[1]);
                if (!ValueSets::isComplement(forwarded)) {
                    for (BuiltinID made : makersOfIteratorsCalledInPlace) {
                        ValueFlowObjectID object = universe.objectOf(builtins.canonicalID(made));
                        if (inputs.sets().contains(forwarded, object))
                            builtinCallees = maker.unite(builtinCallees, maker.of(object));
                    }
                }
            }
            // Whatever rule answers a call with the object of the call has noted as what it made the object.
            ASSERT(!call.result || !inputs.sets().contains(inputs.solver().possibleValuesOf(call.result), object) || hasNotedAllocation.get(object.index()));
            inputs.sets().forEachMemberInLine(builtinCallees, [&](ValueFlowObjectID callee) {
                for (const BuiltinOverload& overload : builtins.overloadsOf(universe.builtinOf(callee))) {
                    if (overload.mode != *mode || !overload.result.type)
                        continue;
                    for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
                        switch (part.kind) {
                        case BuiltinTypePartKind::Instance:
                            found.append({ object, part.builtinClass });
                            break;
                        case BuiltinTypePartKind::InstanceOfReceiverClass:
                        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
                        case BuiltinTypePartKind::Layout:
                            found.append({ object, { } });
                            break;
                        case BuiltinTypePartKind::Builtin:
                        case BuiltinTypePartKind::Operand:
                        case BuiltinTypePartKind::Slot:
                        case BuiltinTypePartKind::IteratedValues:
                        case BuiltinTypePartKind::CallbackResult:
                        case BuiltinTypePartKind::Prototype:
                            break;
                        }
                    }
                }
            });
            if (builtinCallees != ValueSets::nothing && inputs.sets().isSubset(builtinCallees, maker.of(universe.objectOf(builtins.canonicalID(Builtins::ObjectEntries)))))
                found.append({ universe.siteObject(call, 1), BuiltinClasses::Array });
        }
    });
    // One object a built-in stands for all that it makes where native code calls it. The constructor of a typed array answers nothing else, and each allocation has noted its class there.
    if (written) {
        HashMap<uint32_t, BuiltinClassID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> classMadeAs;
        for (uint32_t index = 0; index < builtins.classCount(); ++index) {
            BuiltinClassID builtinClass(index);
            BuiltinID constructor = builtins.at(builtinClass).constructor;
            if (constructor && builtins.typedArrayClassOf(builtinClass) == builtinClass && universe.builtinTables().objectStandingFor(builtinClass))
                classMadeAs.add(universe.resultOf(constructor, 0).index(), builtinClass);
        }
        for (const ValueFlowHeapIndex::WrittenNode& node : written->nodes) {
            if (ValueSets::isComplement(node.objects))
                continue;
            inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
                BuiltinClassID made = classMadeAs.get(object.index());
                if (!made)
                    return;
                if (ValueSets::isComplement(node.value)) {
                    noted.append({ object, { } });
                    return;
                }
                inputs.sets().forEachMember(node.value, [&](ValueFlowObjectID named) {
                    noted.append({ object, named == universe.builtinTables().objectStandingFor(made) ? made : BuiltinClassID() });
                });
            });
        }
        // The new target gives the prototype, and it need not be the constructor that is called: Reflect.construct(Uint8Array, [2], Other).
        // Where no new target comes out of an operand each constructor is its own, also where one call may reach two of them.
        bool newTargetsComeFromOperands = !inputs.sets().isEmpty(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands)));
        if (const ValueFlowHeapIndex::NodesWrittenUnderName* links = newTargetsComeFromOperands ? heapIndex.findNodesWrittenUnder(prototypeLinkName) : nullptr) {
            for (const ValueFlowHeapIndex::WrittenNode& node : links->nodes) {
                if (ValueSets::isComplement(node.objects))
                    continue;
                inputs.sets().forEachMemberInLine(node.objects, [&](ValueFlowObjectID object) {
                    BuiltinClassID made = classMadeAs.get(object.index());
                    if (!made)
                        return;
                    if (ValueSets::isComplement(node.value)) {
                        noted.append({ object, { } });
                        return;
                    }
                    inputs.sets().forEachMember(node.value, [&](ValueFlowObjectID prototype) {
                        if (prototype != universe.objectOf(builtins.at(made).prototype))
                            noted.append({ object, { } });
                    });
                });
            }
        }
    }
    ValueFlowClassesOfBuiltinResults classes;
    for (auto [object, builtinClass] : noted) {
        auto result = classes.add(object.index(), builtinClass);
        if (result.iterator->value != builtinClass)
            result.iterator->value = BuiltinClassID();
    }
    // One object of a call stands for what every function that arrives there makes. It has the class that the rows name only where each allocation has noted that class.
    for (const ValueFlowHeapIndex::WrittenNode& node : written ? written->nodes.span() : std::span<const ValueFlowHeapIndex::WrittenNode>()) {
        if (ValueSets::isComplement(node.objects))
            continue;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
            auto found = classes.find(object.index());
            if (found == classes.end() || !found->value)
                return;
            ValueFlowObjectID standing = universe.builtinTables().objectStandingFor(found->value);
            bool isNotedOtherwise = ValueSets::isComplement(node.value);
            if (!isNotedOtherwise) {
                inputs.sets().forEachMember(node.value, [&](ValueFlowObjectID named) {
                    isNotedOtherwise |= named != (standing ? standing : object);
                });
            }
            if (isNotedOtherwise)
                found->value = BuiltinClassID();
        });
    }
    return classes;
}

static void recordPlacesOfUnseenCode(const BuiltinTable& builtins, const ValueFlowPlacesInProgram& places, Vector<UnseenCodeRecord>& records)
{
    auto kindOf = [](const UnseenCodeRecord& record) {
        return std::pair(record.cause, record.builtin);
    };
    auto isInProgram = [&](const UnseenCodeRecord& record) {
        return hasFunction(record.witness) && places.wayTo(record.witness) == record.witness;
    };
    Vector<std::pair<UnseenCodeCause, uint32_t>> kindsInProgram;
    for (const UnseenCodeRecord& record : records) {
        if (isInProgram(record))
            kindsInProgram.appendIfNotContains(kindOf(record));
    }
    records.removeAllMatching([&](const UnseenCodeRecord& record) {
        return !isInProgram(record) && kindsInProgram.contains(kindOf(record));
    });
    for (UnseenCodeRecord& record : records) {
        if (Witness way = places.wayTo(record.witness); hasFunction(way))
            record.witness = way;
        else if (!hasFunction(record.witness) && record.builtin != Wire::none)
            record.witness = places.whereNamed(builtins.canonicalID(BuiltinID(record.builtin)));
    }
    auto evaluator = std::ranges::find_if(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause);
    for (UnseenCodeRecord& record : records) {
        if (evaluator != records.end() && record.cause == UnseenCodeCause::LoadsModuleByName && record.builtin == Wire::none && !hasFunction(record.witness)) {
            record.builtin = evaluator->builtin;
            record.witness = evaluator->witness;
        }
    }
    sortAndRemoveRepeatedRecords(records);
}

static void recordPlaces(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BitVector builtinsAskedAbout(builtins.builtinCount());
    auto askAboutEach = [&](const auto& records) {
        for (const auto& record : records) {
            if (record.builtin != Wire::none)
                builtinsAskedAbout.quickSet(builtins.canonicalID(BuiltinID(record.builtin)).index());
        }
    };
    askAboutEach(tables.builtins.uses);
    askAboutEach(tables.wholeProgram.unseenCode);
    askAboutEach(tables.wholeProgram.loadsOfMachineCode);

    ValueFlowPlacesInProgram places(inputs, builtinsAskedAbout);
    auto whereNamed = [&](uint32_t builtin) {
        return builtin == Wire::none ? noWitness : places.whereNamed(builtins.canonicalID(BuiltinID(builtin)));
    };
    for (BuiltinUseRecord& use : tables.builtins.uses)
        use.witness = whereNamed(use.builtin);
    for (FeatureUseRecord& use : tables.builtins.featureUses)
        use.witness = whereNamed(use.builtin);
    AOT::sort(tables.builtins.featureUses, { }, [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.witness.location.module, use.witness.location.position, use.builtin);
    });
    for (FlowLocaleUse& use : tables.builtins.localeUses) {
        use.witness = places.wayTo(use.witness);
        if (!hasFunction(use.witness) && use.builtin)
            use.witness = whereNamed(use.builtin.index());
    }
    recordPlacesOfUnseenCode(builtins, places, tables.wholeProgram.unseenCode);
    recordPlacesOfUnseenCode(builtins, places, tables.wholeProgram.loadsOfMachineCode);
}

static constexpr OpcodeSet operationsWritingProperties { Opcode::PropSet, Opcode::PropDefine, Opcode::PropDelete, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemDelete, Opcode::DefineGetter, Opcode::DefineSetter };

static bool writesUnderKeyOfOperand(const BuiltinPropertyAction& action)
{
    return writesUnderKey(action.kind) && action.key.kind == BuiltinKeyKind::Operand;
}

namespace {

struct WriteByTests {
    BuiltinID member;
    // Or a new name on an object that takes none.
    BuiltinID holder;
    StringID name;

    friend auto operator<=>(const WriteByTests&, const WriteByTests&) = default;
};

} // anonymous namespace

// The locked members that a write may reach, and the names that it may add to an object which takes none, as far as a test run can do without those locks. By values, so through helpers and tables of names too.
// One too many costs a test where it is used, and a build that starts over. One that is missed refuses the write when it comes.
static void collectWhatTestsReplace(const ValueFlowResultInputs& inputs, ValueFlowUsesOfLibraryCode& result)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueSets& sets = inputs.sets();

    ValueFlowHeapView heap(inputs);
    Vector<ValueFlowObjectID, 32> objectsOfHolders;
    BitVector isHolder(builtins.builtinCount());
    auto addHolder = [&](BuiltinID holder) {
        if (!isHolder.quickSet(builtins.canonicalID(holder).index()))
            universe.appendObjectsOf(builtins.canonicalID(holder), objectsOfHolders);
    };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.canBeReplacedInTestRuns(BuiltinID(index)))
            addHolder(builtins.at(BuiltinID(index)).holder);
        if (builtins.canTakeNameInTestRuns(BuiltinID(index), { }))
            addHolder(BuiltinID(index));
    }
    ValueSetID holders = heap.maker().of(objectsOfHolders.span());

    auto note = [&](ValueSetMaker& maker, ValueSetID receivers, PropertyKeyID key, Vector<WriteByTests>& found) {
        sets.forEachMember(maker.intersect(receivers, holders), [&](ValueFlowObjectID object) {
            BuiltinID holder = builtins.canonicalID(universe.builtinOf(object));
            if (BuiltinID member = findOwnMember(program, holder, key)) {
                if (builtins.canBeReplacedInTestRuns(member))
                    found.append({ member, { }, { } });
                return;
            }
            const PropertyKey& name = program.at(key);
            if (name.kind == PropertyKeyKind::Text && !program.isIndexLike(key) && builtins.canTakeNameInTestRuns(holder, program.builtinNameOf(key)) && !program.isOpened(holder, name.text))
                found.append({ { }, holder, name.text });
        });
    };
    auto noteUnderKeys = [&](ValueSetMaker& maker, ValueSetID receivers, ValueSetID keys, Vector<WriteByTests>& found) {
        if (ValueSets::isComplement(keys))
            return;
        // Several objects under several keys: a helper, or a loop over a table, where every object meets every key. Nearly all of those pairs are never written.
        // The front end reads the pairs where they are spelled: resolve/replaced_members.rs in Bun.
        keys = maker.intersect(keys, universe.keys());
        if (sets.size(receivers) > 1 && sets.size(keys) > 1)
            return;
        sets.forEachMember(keys, [&](ValueFlowObjectID key) {
            if (universe.kindOf(key) == ValueFlowObjectKind::Key)
                note(maker, receivers, universe.keyOf(key), found);
        });
    };
    // A receiver that may be anything names no holder.
    auto mayBeHolder = [&](ValueSetID receivers) {
        return !ValueSets::isComplement(receivers) && sets.meet(receivers, holders);
    };

    ValueSetMakers makers(inputs);
    Vector<WriteByTests> writes = collectWithMakers<WriteByTests>(inputs, makers, program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<WriteByTests>& found) {
        // Bun's own code replaces nothing for a test.
        if (!inputs.isReached(FunctionID(function)) || isLibraryCode(program, program.functions[function]))
            return;
        for (const ValueFlowStatement& statement : bodies.statementsOf(function)) {
            if (statement.kind != ValueFlowStatementKind::Operation || !statement.site || !operationsWritingProperties.contains(statement.opcode))
                continue;
            std::optional<unsigned> object = OperandPosition::object(statement.opcode);
            std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
            ValueSetID receivers = object ? inputs.possibleValuesOf(maker, operands[*object]) : ValueSets::nothing;
            if (!mayBeHolder(receivers))
                continue;
            const Value& operation = program.at(statement.function).at(statement.site);
            std::optional<unsigned> computedKey = computedKeyOfWrite(operation);
            if (computedKey)
                noteUnderKeys(maker, receivers, inputs.possibleValuesOf(maker, operands[*computedKey]), found);
            else
                note(maker, receivers, operation.key(), found);
        }
    });

    BitVector writers = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, writesUnderKeyOfOperand);
    });
    forEachBuiltinApplication(inputs, heap, writers, holders, [&](const ValueFlowBuiltinApplication& application) {
        if (application.function && isLibraryCode(program, program.at(application.function)))
            return;
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                if (!writesUnderKeyOfOperand(action))
                    continue;
                ValueSetID receivers = application.valueAt(action.object, heap.maker());
                if (mayBeHolder(receivers))
                    noteUnderKeys(heap.maker(), receivers, application.valueAt(*action.key.operand, heap.maker()), writes);
            }
        }
    });

    AOT::sort(writes);
    removeRepeatedElements(writes);
    for (const WriteByTests& write : writes) {
        if (write.member)
            result.builtinsThatTestsReplace.append(write.member);
        else
            result.namesThatTestsAdd.append({ write.holder, Vector<uint8_t>(program.string(write.name)) });
    }
}

ValueFlowUsesOfLibraryCode findUsesOfLibraryCode(const ValueFlowResultInputs& inputs)
{
    ValueFlowUsesOfLibraryCode found;
    found.unseenCode = collectUnseenCode(inputs, found.loadsOfMachineCode);
    for (uint32_t index = 0; index < inputs.program.builtins().builtinCount(); ++index) {
        if (OptionSet<BuiltinUse> uses = inputs.solver().usesOf(BuiltinID(index)); !uses.isEmpty())
            found.builtinUses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(uses.toRaw()), noWitness });
    }
    for (size_t function : inputs.solver().libraryFunctionsLacked())
        found.libraryFunctionsLacked.append(RuntimeFunctionID(static_cast<uint32_t>(function)));
    if (!inputs.build.options().admitsReplacedBuiltins()) {
        found.exportsThatSpiesReplace = collectExportsThatSpiesReplace(inputs);
        return found;
    }
    // Both walk the calls of built-in functions.
    ValueFlowHeapIndex index(inputs);
    ValueFlowResultInputs inputsWithIndex = inputs;
    inputsWithIndex.heapIndex = &index;
    found.exportsThatSpiesReplace = collectExportsThatSpiesReplace(inputsWithIndex);
    collectWhatTestsReplace(inputsWithIndex, found);
    return found;
}

Vector<ValueFlowWriteUnderNewName> computeBuiltinResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfLibraryCode&& usesOfLibraryCode, FlowResultTables& tables, std::unique_ptr<LedgerPart>& ledgerOfBuiltinObjects)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueFlowHeapView heap(inputs);
    auto ownedHolders = makeUnique<LedgerOfBuiltinObjects>(inputs, heap, tables.builtins);
    const LedgerOfBuiltinObjects& holders = *ownedHolders;
    ledgerOfBuiltinObjects = WTF::move(ownedHolders);
    times.didFinish("built-ins: holders"_s);
    Vector<ValueFlowWriteUnderNewName> writesUnderNewNames = computeBuiltinLocations(inputs, holders, heap, tables.builtins);
    times.didFinish("built-ins: locations"_s);
    BitVector builtinsWhoseCodeRuns = findBuiltinsWhoseCodeRuns(inputs);
    tables.wholeProgram.unseenCode = WTF::move(usesOfLibraryCode.unseenCode);
    tables.wholeProgram.loadsOfMachineCode = WTF::move(usesOfLibraryCode.loadsOfMachineCode);
    tables.wholeProgram.findsFilesOfProgramByName = filesOfProgramAreLoadedByName(inputs.solution) || inputs.solver().input().refusedCode.hostRunsFilesOutsideImage;
    tables.wholeProgram.hostRunsFilesOutsideImage = inputs.solver().input().refusedCode.hostRunsFilesOutsideImage;
    warnAboutLoadsUnderComputedNames(inputs, tables.wholeProgram.unseenCode.span());
    tables.wholeProgram.instances = countProgramInstances(inputs);
    times.didFinish("built-ins: code that runs, unseen code, instances"_s);
    tables.builtins.uses = WTF::move(usesOfLibraryCode.builtinUses);
    tables.builtins.libraryFunctionsLacked = WTF::move(usesOfLibraryCode.libraryFunctionsLacked);
    inputs.build.statistics().count(StatisticID::FlowLibraryFunctionsLacked, tables.builtins.libraryFunctionsLacked.size());
    times.didFinish("built-ins: uses"_s);
    tables.builtins.legacyRegExpStatics = findReadsOfLegacyRegExpStatics(inputs, holders);
    tables.builtins.featureUses = collectFeatureUses(inputs, holders, builtinsWhoseCodeRuns);
    tables.builtins.builtinsThatMayBeTrimmed = findBuiltinsThatMayBeTrimmed(inputs.program.builtins(), tables.builtins.featureUses.span(), inputs.build.options());
    tables.builtins.localeUses = collectLocaleUses(inputs, heap, builtinsWhoseCodeRuns, tables.wholeProgram.unseenCode.span());
    tables.builtins.workerEntries = collectWorkerEntries(inputs, heap);
    tables.builtins.workerStartWithoutName = findWorkerStartWithoutName(inputs);
    tables.builtins.exportsThatSpiesReplace = WTF::move(usesOfLibraryCode.exportsThatSpiesReplace);
    tables.builtins.builtinsThatTestsReplace = WTF::move(usesOfLibraryCode.builtinsThatTestsReplace);
    tables.builtins.namesThatTestsAdd = WTF::move(usesOfLibraryCode.namesThatTestsAdd);
    times.didFinish("built-ins: statics, features, locales"_s);
    if (hasFlag(inputs.build.options().reports, ReportSet::Flow)) {
        recordPlaces(inputs, tables);
        times.didFinish("built-ins: places for the report"_s);
    }
    tables.builtins.areFilled = true;
    return writesUnderNewNames;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
