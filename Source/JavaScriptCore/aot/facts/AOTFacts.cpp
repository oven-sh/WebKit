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
#include "AOTEnforcedFields.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkFlow.h"
#include "AOTLinkStrings.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/threads/BinarySemaphore.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(FactKind kind)
{
    switch (kind) {
    case FactKind::NarrowedTypes:
        return "NarrowedTypes"_s;
    case FactKind::ProvenContents:
        return "ProvenContents"_s;
    case FactKind::OperandSummaries:
        return "OperandSummaries"_s;
    case FactKind::HeapWords:
        return "HeapWords"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Facts::everyObjectHas(TypeReader types, TypeID id, ObjectFact fact) const
{
    if (!id || holdsOnlyPrimitives(id))
        return false;
    Type type = types.at(id);
    if (!type.hasOnlyNamedObjects())
        return false;
    return types.at(type.objects).allOf([&](ObjectID member) {
        return object(member).facts.contains(fact);
    });
}

bool Facts::someObjectHas(TypeReader types, TypeID id, ObjectFact fact) const
{
    if (!id || holdsOnlyPrimitives(id))
        return false;
    Type type = types.at(id);
    if (!type.hasNamedObjects())
        return false;
    return types.at(type.objects).anyOf([&](ObjectID member) {
        return object(member).facts.contains(fact);
    });
}

// ---- The table of built-ins

RefusedCode refusedCodeOf(const Program& program)
{
    return {
        .evaluatedText = hasFlag(program.excludedCode, ExcludedCode::EvaluatedText),
        .hostEvaluatedText = hasFlag(program.excludedCode, ExcludedCode::HostEvaluatedText),
        // An image that admits the inspector loads files from disk whatever else it was built to refuse: Modules::refusesCodeOutsideImage().
        .codeOutsideImage = !program.admitsInspector && (runsOnlyCodeOfImage || hasFlag(program.excludedCode, ExcludedCode::CodeOutsideImage)),
    };
}

bool refusesTextAsUnseenCode(const BuiltinTable& builtins, BuiltinID canonical, RefusedCode refused)
{
    const Builtin& entry = builtins.at(canonical);
    if (entry.origin == BuiltinOrigin::Engine)
        return refused.evaluatedText && entry.flags.contains(BuiltinFlag::EvaluatesText);
    return refused.hostEvaluatedText && (entry.flags.contains(BuiltinFlag::EvaluatesText) || builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, canonical));
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    builtin = builtins.canonicalID(builtin);
    OptionSet<BuiltinFlag> flags = builtins.at(builtin).flags;
    // `module._compile(text)` hands the text a require, so it loads what the text names: nothing where the text is refused.
    if (refusesTextAsUnseenCode(builtins, builtin, refused))
        return std::nullopt;
    bool loadsFromImageAlone = refused.codeOutsideImage || builtin == Builtins::ProcessGetBuiltinModule;
    // A load of a module of the image runs no code that nobody has read. The text does.
    if (flags.contains(BuiltinFlag::EvaluatesText) && (loadsFromImageAlone || !flags.contains(BuiltinFlag::LoadsModuleByName)))
        return UnseenCodeCause::EvaluatesText;
    if (flags.contains(BuiltinFlag::LoadsModuleByName))
        return loadsFromImageAlone ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName;

    const BuiltinUnseenCode* row = builtins.unseenCodeOf(builtin);
    // The call runs unseen code only where an operand has a property of some name.
    if (row && row->condition && row->condition->hasProperty)
        return std::nullopt;
    if (row ? !row->condition || !row->condition->buildAdmitsRuntimeCode : flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

// Here yes is the conservative answer: ObjectTable::mergedFormOf() keeps such a function out of what `unknown` covers. So ask as for a build that refuses no code.
bool raisesEventWhenCalled(const BuiltinTable& builtins, BuiltinID builtin)
{
    builtin = builtins.canonicalID(builtin);
    if (eventOfCalling(builtins, builtin, { }))
        return true;
    return std::ranges::any_of(builtins.loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
        return builtins.canonicalID(loader.callee) == builtin;
    });
}

bool isMadeWithCodeOfLibrary(const BuiltinTable& builtins, BuiltinID member)
{
    member = builtins.canonicalID(member);
    const Builtin& entry = builtins.at(member);
    if (entry.origin != BuiltinOrigin::Host)
        return false;
    if (!builtins.calledFunctionsOf(member).empty() || !builtins.moduleLoadsOf(member).empty())
        return true;
    return !isObjectKind(entry.kind) && std::ranges::any_of(builtins.overloadsOf(member), [](const BuiltinOverload& overload) {
        return !!overload.code.implementation.function;
    });
}

// ---- The stage

// The stable numbers end at the keys of the wire's list and those that the reader has added. Behind them every text gets its key, by StringID, so the program alone decides the numbers.
static void endStableNumbersOfKeysForFacts(Program& program, const ParallelLoops& loops)
{
    program.computeLocationBits(loops);
    program.types.endStableNumbersOfPropertyKeys(loops);
    program.types.addKeysOfTexts(static_cast<uint32_t>(program.strings.size()), loops);
}

static std::unique_ptr<BodyIndexTable> indexBodiesForFacts(const Program& program, const ParallelLoops& loops, std::unique_ptr<BodyIndexTable>&& indexesOfVerifier)
{
    auto numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    std::unique_ptr<BodyIndexTable> bodies = WTF::move(indexesOfVerifier);
    if (!bodies)
        bodies = makeUniqueWithoutFastMallocCheck<BodyIndexTable>(numberOfFunctions, loops.width());
    else
        bodies->addFunctionsWithoutBodies(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (function.hasBody() && !bodies->has(id))
            bodies->build(id, function, worker);
    });
    return bodies;
}

static BuiltinID builtinWrittenAsHeritageForFacts(TypeReader types, BodyValue heritage)
{
    Opcode opcode = heritage.opcode();
    if (opcode == Opcode::Builtin)
        return heritage.as<Opcode::Builtin>().builtin();
    if (opcode == Opcode::ModuleBuiltin)
        return heritage.as<Opcode::ModuleBuiltin>().builtin();
    if (opcode != Opcode::Convert)
        return { };
    TypeID written = heritage.as<Opcode::Convert>().declared().written();
    return written ? singleBuiltinOf(types, types.at(written)) : BuiltinID();
}

static Vector<ClassBelowBuiltinClass> findClassesBelowBuiltinClassesForFacts(const Program& program, const BodyIndexTable& bodies, const ParallelLoops& loops)
{
    static constexpr OpcodeSet evaluationsOfClasses { Opcode::NewClass };
    TypeReader types(program);
    Vector<ClassBelowBuiltinClass> classes = collectInParallel<ClassBelowBuiltinClass>(loops, static_cast<unsigned>(program.functions.size()), [&](unsigned number, Vector<ClassBelowBuiltinClass>& found) {
        const BodyIndex* index = bodies.find(FunctionID(number));
        if (!index || !index->holdsAnyOf(evaluationsOfClasses))
            return;
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            for (BodyValue value : index->block(BlockID(block)).values()) {
                if (value.opcode() != Opcode::NewClass)
                    continue;
                BodyOperation<Opcode::NewClass> newClass = value.as<Opcode::NewClass>();
                ValueID heritage = newClass.heritageOperand();
                if (!heritage || writtenBaseOf(program.at(newClass.layout())))
                    continue;
                BuiltinID builtin = builtinWrittenAsHeritageForFacts(types, index->value(heritage));
                if (!builtin)
                    continue;
                if (BuiltinClassID builtinClass = classMadeByHeritage(program.builtins(), builtin))
                    found.append(ClassBelowBuiltinClass { newClass.layout(), builtinClass });
            }
        }
    });
    AOT::sort(classes);
    removeRepeatedElements(classes);
    return classes;
}

// No TypeArena is alive here: what one remembers of the classes that are noted is stale from here on.
static void updateProgramBehindIndexesForFacts(Program& program, BodyIndexTable& bodies, const ParallelLoops& loops)
{
    Vector<uint32_t> numberOfSitesOfFunctions(program.functions.size(), [](size_t) -> uint32_t {
        return 0;
    });
    program.types.objects().reserveSites(numberOfSitesOfFunctions.span(), loops);
    program.setConstructionStarts(bodies.takeConstructionStarts());
    Vector<ClassBelowBuiltinClass> classes = findClassesBelowBuiltinClassesForFacts(program, bodies, loops);
    noteSubclassesOfBuiltinClasses(program, classes.span());
}

// A call never suspends its caller, whatever the callee does with its own frame.
static Effects effectsSeenByCallerForFacts(Effects effects)
{
    effects.maySuspend = false;
    return effects;
}

static void fillFunctionTablesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    size_t numberOfFunctions = program.functions.size();
    RELEASE_ASSERT(linked.functions.size() == numberOfFunctions && summaries.size() == numberOfFunctions);

    tables.summaries.reserveInitialCapacity(numberOfFunctions);
    tables.effects.reserveInitialCapacity(numberOfFunctions);
    tables.functionsNeverReached.ensureSize(numberOfFunctions);
    tables.entryParameters.firstRowOfOwner.reserveInitialCapacity(numberOfFunctions + 1);

    for (size_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(static_cast<uint32_t>(index));
        const Function& function = program.at(id);
        const LinkedFunction& link = linked.function(id);
        const ThinSummary& summary = summaries.at(id);

        tables.summaries.append(function.hasBody() ? Summary { summary.numberOfOperations, !summary.has(SummaryTrait::HasLoops) } : Summary { });
        tables.effects.append(FunctionEffects { effectsSeenByCallerForFacts(link.effectsOnReturn), effectsSeenByCallerForFacts(link.effectsOnThrow) });
        if (!link.isReached)
            tables.functionsNeverReached.quickSet(index);

        Vector<PositionFacts>& rows = tables.entryParameters.rows;
        tables.entryParameters.firstRowOfOwner.append(static_cast<uint32_t>(rows.size()));
        std::span<const LinkedParameter> parameters = linked.parametersOf(id);
        RELEASE_ASSERT(parameters.size() == function.numberOfEntryParameters());
        if (!link.everyCallerIsKnown)
            continue;
        for (const LinkedParameter& parameter : parameters)
            rows.append(PositionFacts { parameter.proven });
    }
    tables.entryParameters.firstRowOfOwner.append(static_cast<uint32_t>(tables.entryParameters.rows.size()));
}

// A LAYOUT THAT REACHES NO UNSEEN CODE IS READ WITHOUT A LOOK, on the word of linked(). So whatever breaks that word at run time is a false proof, and an emptied slot under such a read would be worse than a stop:
// the record of the layout has what is ruled out, and validateMutation() ends the process there. Of a layout that reaches unseen code nothing is ruled out: what is held of it is looked at.
// `function onConnection(socket) { socket.parser = p }`, which unseen code may call: in linked() the parameter holds anything, so the store names no layout there. held() expects a socket and appends the name.
// The objects get a slot for it all the same. At most maxAppendedFieldsOutOfLine names a layout in all: what is left over is an extra property, as ever.
static void layKeysThatHeldAloneAppendsOver(LinkTables& linked, const LinkTables& held, size_t numberOfLayouts)
{
    if (held.appendedKeys.isEmpty())
        return;
    linked.firstKeyAppendedByHeldAloneOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        linked.firstKeyAppendedByHeldAloneOfLayout.append(static_cast<uint32_t>(linked.keysAppendedByHeldAlone.size()));
        LayoutID id(static_cast<uint32_t>(index));
        std::span<const PropertyKeyID> proven = linked.appendedKeysOf(id);
        size_t room = maxAppendedFieldsOutOfLine - std::min<size_t>(maxAppendedFieldsOutOfLine, proven.size());
        std::span<const PropertyKeyID> keysOfHeld = held.appendedKeysOf(id);
        for (size_t place = 0; place < keysOfHeld.size(); ++place) {
            PropertyKeyID key = keysOfHeld[place];
            if (!room)
                break;
            if (std::ranges::find(proven, key) != proven.end())
                continue;
            if (held.appendedFieldsThatTypeOfThisRequires.get(held.firstAppendedFieldOfLayout[index] + place))
                linked.keysAppendedByHeldAloneThatLieInCell.set(linked.keysAppendedByHeldAlone.size());
            linked.keysAppendedByHeldAlone.append(key);
            --room;
        }
    }
    linked.firstKeyAppendedByHeldAloneOfLayout.append(static_cast<uint32_t>(linked.keysAppendedByHeldAlone.size()));
}

static void fillFieldTablesForFacts(const Program& program, const LinkTables& linked, Mechanisms mechanisms, FactTables& tables)
{
    size_t numberOfLayouts = program.layouts.size();
    RELEASE_ASSERT(linked.layouts.size() == numberOfLayouts);

    bool holdsAccessorsInSlots = withoutMechanismsMissingDependencies(mechanisms).contains(Mechanism::TypedSlots);
    tables.firstFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        tables.firstFieldOfLayout.append(static_cast<uint32_t>(tables.fields.size()));
        LayoutID id(static_cast<uint32_t>(index));
        const LinkedLayout& layout = linked.layouts[index];
        uint32_t numberDeclared = static_cast<uint32_t>(program.layouts[index].fields.size());
        uint32_t numberOfFields = numberDeclared + static_cast<uint32_t>(linked.appendedKeysOf(id).size());
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FieldFacts facts;
            // `Object.defineProperty(this, "id", { value: id })`: as with the accessors of a literal below: it chooses how the objects are laid out and claims nothing of what they hold.
            if (const LinkedCell* cell = field >= numberDeclared && holdsAccessorsInSlots ? linked.findCellOfField({ id, field }) : nullptr) {
                facts.formOfValue = cell->formThatDefinitionsCreate;
                if (facts.formOfValue.contains(FieldForm::IsAccessor))
                    facts.facts.add(FieldFact::IsAccessor);
            }
            // `{ get x() { .. } }`: the slot holds the pair of functions, so that laying them fills it. As a plain field it would leave its slot with each object that is made.
            // It chooses how the objects are laid out and claims nothing of what they hold: the record of the layout says the form, and a property that is created in another form lies beside its slot.
            if (const LinkedCell* cell = holdsAccessorsInSlots ? linked.findCellOfField({ id, field }) : nullptr; cell && cell->isFilledByAccessorsAlone)
                facts.facts.add(FieldFact::IsAccessor);
            if (!layout.possibleChanges.contains(LayoutChange::LosesProperty))
                facts.stability.add(FieldStability::IsNeverDeleted);
            if (!layout.reachesUnseenCode) {
                // A store by name creates a field that is declared with other attributes beside its slot, with no descriptor in sight.
                bool isDeclaredPlain = field >= numberDeclared ? facts.formOfValue.isEmpty() : fieldAt(program, declarationOf(program, { id, field })).form.isEmpty();
                if (isDeclaredPlain && !layout.possibleChanges.contains(LayoutChange::GetsAccessor))
                    facts.stability.add(FieldStability::IsNeverRedefined);
                // Of the proven type. Whether the slot takes all of that type is for whoever knows the slot.
                const LinkedCell* cell = linked.findCellOfField({ id, field });
                if (cell && cell->everyWriterIsKnown && cell->proven)
                    facts.stability.add(FieldStability::IsNeverStoredWithOtherType);
            }
            tables.fields.append(facts);
        }
    }
    tables.firstFieldOfLayout.append(static_cast<uint32_t>(tables.fields.size()));
}

// `const counts = {}; counts[word] = 1`. A record that lists no name and is stored under none has no slot: as a compiled object it would hold extra properties alone, and each new key would make it a structure of its own.
// So its objects are ordinary objects. The answer chooses how the objects of a layout are laid out and claims nothing of what they hold: a wrong one costs speed alone.
// A layout shares its slots with the layouts above and below it, which need one answer, so only a layout that stands alone is asked.
static bool mayHoldDictionariesForFacts(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.name || layout.constructor || !layout.fields.isEmpty() || !layout.staticFields.isEmpty() || layout.computedKeyCount)
        return false;
    if (layout.base || layout.writtenBuiltinBase || !layout.supertypes.isEmpty() || !program.layoutsExpectedBelow(id).empty())
        return false;
    return layout.flags.isEmpty() || layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsWrittenUnderDictionaryType };
}

static void fillLayoutTablesForFacts(const Program& program, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    BitVector holdsDictionaries(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        if (!linked.layouts[index].isFilledUnderComputedKeys && !held.layouts[index].isFilledUnderComputedKeys)
            continue;
        if (mayHoldDictionariesForFacts(program, id) && linked.appendedKeysOf(id).empty() && held.appendedKeysOf(id).empty())
            holdsDictionaries.quickSet(index);
    }
    if (holdsDictionaries.isEmpty())
        return;

    for (const Layout& layout : program.layouts) {
        for (LayoutID supertype : layout.supertypes)
            holdsDictionaries.quickClear(supertype.index());
    }
    // Native code makes the objects of a typed answer by their layouts.
    for (const ExpectedResult& expected : program.expectedResults) {
        for (LayoutID layout : expected.layouts)
            holdsDictionaries.quickClear(layout.index());
    }
    // An object without slots is made empty. `{ 1: "one" }` lists no name either, and what it lists is an element. The loader makes the objects of a module of data by their layouts.
    BitVector isDataOfModule(program.data.size());
    for (const Module& module : program.modules) {
        if (module.data)
            isDataOfModule.quickSet(module.data.index());
    }
    Vector<const DataNode*, 32> nodesToVisit;
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        nodesToVisit.append(&program.data[index].root);
        while (!nodesToVisit.isEmpty()) {
            const DataNode& node = *nodesToVisit.takeLast();
            if (const auto* array = node.tryAs<DataNode::Array>()) {
                for (const DataNode& element : array->elements)
                    nodesToVisit.append(&element);
            } else if (const auto* object = node.tryAs<DataNode::Object>()) {
                if (object->layout && (!object->members.isEmpty() || isDataOfModule.quickGet(index)))
                    holdsDictionaries.quickClear(object->layout.index());
                for (const DataMember& member : object->members)
                    nodesToVisit.append(&member.value);
            }
        }
    }
    // Link ran while the shape told the objects of every layout from all others: where a written type names a layout, its filters let the objects of the layout through and turned all others away.
    // An ordinary object has no shape. The guard that the walk builds for such a type would let any object through, and what Link says behind the guard would no longer hold.
    // Every type that a position, a Convert, a Check or a TypeTest goes by is one of the wire's list.
    const TypeTable& types = program.types;
    for (TypeID type : types.typesOfWireIndexes()) {
        if (!type || holdsOnlyPrimitives(type))
            continue;
        types.at(types.at(type).unnamedLayouts).forEach([&](LayoutID layout) {
            holdsDictionaries.quickClear(layout.index());
        });
    }

    tables.layouts.fill(LayoutFacts { }, numberOfLayouts);
    for (size_t index : holdsDictionaries)
        tables.layouts[index].facts.add(LayoutFact::ObjectsAreDictionaries);
}

static void fillVariableTablesForFacts(const LinkTables& linked, FactTables& tables)
{
    tables.variables.fill(VariableFacts { }, linked.variables.size());
    for (size_t index = 0; index < linked.variables.size(); ++index) {
        if (linked.variables[index].isFilledAtEveryRead)
            tables.variables[index].location.initialization = InitializationState::StoredBeforeEveryRead;
    }
}

// `let hook = () => undefined;  function setHook(f) { hook = f }  ..  setHook(realHook)`. By the records of the scan and Link's list of calls alone: an edge into the node of a variable says what is stored there.
// What comes out of anything else (a call, a field, a join) is not followed and does not count: the table names what a call of the variable tests for, and what else arrives is called as written.
static void fillFunctionsGivenToVariablesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    if (summaries.size() != program.functions.size() || linked.functions.size() != program.functions.size())
        return;
    struct Given {
        FunctionID function;
        bool areSeveral { false };
    };
    using GivenByVariable = HashMap<uint32_t, Given, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    GivenByVariable givenTo;
    auto noteIn = [](GivenByVariable& table, uint32_t variable, FunctionID function) {
        Given& given = table.add(variable, Given { }).iterator->value;
        if (!given.function)
            given.function = function;
        else if (given.function != function)
            given.areSeveral = true;
    };
    auto note = [&](uint32_t variable, FunctionID function) {
        noteIn(givenTo, variable, function);
    };
    // The one function whose closures a node of a summary stands for. Null: it is no such node.
    auto functionAt = [&](const SummaryNode& node) -> FunctionID {
        if (node.kind == SummaryNode::Kind::Closure)
            return FunctionID(node.first);
        if (node.kind != SummaryNode::Kind::Variable || node.first >= linked.variables.size())
            return { };
        TypeID proven = linked.variables[node.first].proven;
        if (!proven || holdsOnlyPrimitives(proven))
            return { };
        Type type = program.types.at(proven);
        return type.isObject() ? singleFunctionOf(program.types, type) : FunctionID();
    };
    struct ParameterStored {
        FunctionID function;
        uint32_t entryParameter { 0 };
        uint32_t variable { 0 };
    };
    Vector<ParameterStored> parametersStored;
    BitVector functionsThatStoreParameter;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        // What the body of a module stores is what the variable starts with: whoever replaces it means the other to be called.
        if (!linked.function(function).isReached || program.at(function).isModuleBody())
            continue;
        const ThinSummary& summary = summaries.at(function);
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& to = summary.nodes[edge.to];
            if (to.kind != SummaryNode::Kind::Variable)
                continue;
            const SummaryNode& from = summary.nodes[edge.from];
            if (FunctionID stored = functionAt(from))
                note(to.first, stored);
            else if (from.kind == SummaryNode::Kind::Parameter) {
                parametersStored.append({ function, from.first, to.first });
                functionsThatStoreParameter.set(index);
            }
        }
    }
    if (givenTo.isEmpty() && parametersStored.isEmpty())
        return;
    for (const LinkedCall& listed : linked.calls) {
        if (!functionsThatStoreParameter.get(listed.callee.index()))
            continue;
        const ThinSummary& summary = summaries.at(listed.caller);
        // The calls of a summary need not stand in the order of their sites, and few functions store a parameter into a variable.
        auto call = std::ranges::find_if(summary.calls, [&](const SummaryCall& candidate) {
            return candidate.site == listed.site && !candidate.isMadeByProtocol;
        });
        if (call == summary.calls.end() || call->passesList)
            continue;
        uint32_t firstPositional = program.at(listed.callee).firstParameterIndex();
        for (const ParameterStored& stored : parametersStored) {
            if (stored.function != listed.callee)
                continue;
            // By its binding a call lists the entry parameters, else the arguments as they are written.
            uint32_t place = stored.entryParameter;
            if (call->callee != SummaryCall::Callee::Binding) {
                if (place < firstPositional)
                    continue;
                place -= firstPositional;
            }
            if (place >= call->numberOfArguments)
                continue;
            if (FunctionID handed = functionAt(summary.nodes[summary.arguments[call->firstArgument + place]]))
                note(stored.variable, handed);
        }
    }
    // What the few variables start with: the bodies of their modules say it.
    GivenByVariable atStart;
    BitVector modulesOfVariables;
    for (const auto& [variable, given] : givenTo) {
        if (!given.areSeveral && variable < program.variables.size())
            modulesOfVariables.set(program.variables[variable].module.index());
    }
    for (size_t module : modulesOfVariables) {
        FunctionID body = program.modules[module].body;
        if (!body || !linked.function(body).isReached)
            continue;
        const ThinSummary& summary = summaries.at(body);
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& to = summary.nodes[edge.to];
            if (to.kind != SummaryNode::Kind::Variable || !givenTo.contains(to.first))
                continue;
            if (FunctionID stored = functionAt(summary.nodes[edge.from]))
                noteIn(atStart, to.first, stored);
        }
    }
    for (const auto& [variable, given] : givenTo) {
        if (given.areSeveral)
            continue;
        auto start = atStart.find(variable);
        FunctionID first = start != atStart.end() && !start->value.areSeveral && start->value.function != given.function ? start->value.function : FunctionID();
        tables.functionsGivenToVariables.append({ variable, given.function, first });
    }
    std::ranges::sort(tables.functionsGivenToVariables, { }, &FactTables::FunctionGivenToVariable::variable);
}

// A class is evaluated at most once in a realm where the program has one NewClass of it, outside of every loop, in the body of a module that the loader enters once,
// or in a function that Link proves to run at most once: `var Parser = (function () { function Parser() { } Parser.prototype.peek = ..; return Parser; })()`, what a transpiler of classes writes.
// The class of a constructor function is evaluated where the function object is made: by a NewClosure, or by the loader for a variable that is born as the function.
// It goes by the records and the bodies alone, reached or not. The run time has no answer to a second evaluation of such a class.
static void fillClassTablesForFacts(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    size_t numberOfLayouts = program.layouts.size();
    BitVector bodiesEnteredOnce = bodiesThatLoaderEntersAtMostOnce(program);
    auto loaderEntersOnce = [&](FunctionID id) {
        return bodiesEnteredOnce.get(id.index());
    };

    Vector<uint32_t> numberOfEvaluations;
    numberOfEvaluations.fill(0, numberOfLayouts);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (LayoutID evaluated : summaries.at(FunctionID(index)).classesEvaluated)
            ++numberOfEvaluations[evaluated.index()];
    }
    auto classEvaluatedForVariable = [&](const Variable& variable) {
        return variable.initialization == VariableInitialization::Function ? classOfConstructorFunction(program, variable.function) : LayoutID();
    };
    for (const Variable& variable : program.variables) {
        if (LayoutID evaluated = classEvaluatedForVariable(variable))
            ++numberOfEvaluations[evaluated.index()];
    }

    tables.classes.fill(ClassFacts { }, numberOfLayouts);
    for (const Variable& variable : program.variables) {
        LayoutID evaluated = classEvaluatedForVariable(variable);
        if (evaluated && numberOfEvaluations[evaluated.index()] == 1 && variable.module && program.at(variable.module).body && loaderEntersOnce(program.at(variable.module).body))
            tables.classes[evaluated.index()].evaluations = ExecutionCount::AtMostOnce;
    }

    // A summary lists a class once, however often its body evaluates it.
    Vector<uint32_t> numberOfEvaluationsInBody;
    numberOfEvaluationsInBody.fill(0, numberOfLayouts);
    Vector<LayoutID, 16> classesEvaluatedInBody;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        FunctionID id(number);
        bool runsAtMostOnce = loaderEntersOnce(id) || (number < linked.functions.size() && linked.functions[number].runsAtMostOnce);
        if (summaries.at(id).classesEvaluated.isEmpty() || !runsAtMostOnce)
            continue;
        const BodyIndex* index = bodies.find(id);
        if (!index || index->hasIrreducibleFlow())
            continue;
        classesEvaluatedInBody.shrink(0);
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            bool comesAgain = !!index->loopOf(BlockID(block));
            for (BodyValue value : index->block(BlockID(block)).values()) {
                LayoutID evaluated;
                if (value.opcode() == Opcode::NewClass)
                    evaluated = value.as<Opcode::NewClass>().layout();
                else if (value.opcode() == Opcode::NewClosure)
                    evaluated = classOfConstructorFunction(program, value.as<Opcode::NewClosure>().function());
                if (!evaluated)
                    continue;
                if (!numberOfEvaluationsInBody[evaluated.index()])
                    classesEvaluatedInBody.append(evaluated);
                numberOfEvaluationsInBody[evaluated.index()] += comesAgain ? 2 : 1;
            }
        }
        for (LayoutID evaluated : classesEvaluatedInBody) {
            if (std::exchange(numberOfEvaluationsInBody[evaluated.index()], 0) == 1 && numberOfEvaluations[evaluated.index()] == 1)
                tables.classes[evaluated.index()].evaluations = ExecutionCount::AtMostOnce;
        }
    }

    // `class Socket extends NetSocket { }`, where nobody has written what NetSocket is: the record names what is seen to arrive there.
    // The run time goes up and down the rows of the records: a row that comes back to itself, which no program can build, names nothing.
    auto heritageSeenToArrive = [&](LayoutID id) {
        bool isAsked = id.index() < held.layouts.size() && !program.at(id).base && program.at(id).has(LayoutFlags::HasHeritage);
        return isAsked ? held.layouts[id.index()].heritage : LayoutID();
    };
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID heritage = heritageSeenToArrive(LayoutID(static_cast<uint32_t>(index)));
        size_t stepsLeft = numberOfLayouts;
        LayoutID current = heritage;
        while (current && stepsLeft--)
            current = program.at(current).base ? program.at(current).base : heritageSeenToArrive(current);
        if (!heritage || current)
            continue;
        tables.classes[index].baseState = BaseState::None;
        tables.classes[index].prototypeBase = heritage;
    }
}

// What the DefineMethods of the program give each class, in the order of the definitions. ONE TABLE FOR THE RECORD OF THE CLASS AND FOR THE WALK: a guard of a class holds while its holders are as the record declares,
// so whoever asks what stands under a name above an instance asks the list that the record is written from. It goes by the bodies alone, reached or not.
static void fillMembersOfClassesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    struct DefinedMember {
        LayoutID classLayout;
        uint32_t index { 0 };
        ClassMember member;
    };
    Vector<DefinedMember> defined;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        const ThinSummary& summary = summaries.at(FunctionID(number));
        for (const SummaryMember& written : summary.members) {
            const SummaryNode& value = summary.nodes[written.value];
            ClassMember member;
            member.key = written.key;
            member.side = written.side;
            member.kind = written.kind;
            if (value.kind == SummaryNode::Kind::Closure)
                member.function = FunctionID(value.first);
            defined.append({ written.classLayout, written.index, member });
        }
    }
    AOT::stableSort(defined, { }, [](const DefinedMember& row) {
        return std::pair(row.classLayout, row.index);
    });

    size_t numberOfLayouts = program.layouts.size();
    FactRows<ClassMember>& rows = tables.membersOfClass;
    rows.firstRowOfOwner.reserveInitialCapacity(numberOfLayouts + 1);
    size_t next = 0;
    for (size_t layout = 0; layout < numberOfLayouts; ++layout) {
        rows.firstRowOfOwner.append(static_cast<uint32_t>(rows.rows.size()));
        for (size_t first = next; next < defined.size() && defined[next].classLayout.index() == layout; ++next) {
            const DefinedMember& row = defined[next];
            if (next == first || row.index != defined[next - 1].index) {
                rows.rows.append(row.member);
                continue;
            }
            // Two bodies evaluate the class. Where they do not say the same of a member, it counts as one under a key that nobody knows, which no record creates.
            ClassMember& listed = rows.rows.last();
            if (listed.key != row.member.key || listed.side != row.member.side || listed.kind != row.member.kind || listed.function != row.member.function) {
                listed.key = { };
                listed.function = { };
            }
        }
        appendMembersAssignedToPrototype(program, linked, LayoutID(static_cast<uint32_t>(layout)), rows.rows);
    }
    rows.firstRowOfOwner.append(static_cast<uint32_t>(rows.rows.size()));
}

std::unique_ptr<Facts> computeFacts(Program& program, BuildContext& build, const FactsRequest& request)
{
    return computeFacts(program, build, request, nullptr);
}

void printBreachesOfClosureOfLink(PrintStream& out, Program& program, BuildContext& build, Mechanisms mechanisms)
{
    // As computeFacts() comes to Link.
    const ParallelLoops& loops = build.parallelLoops();
    endStableNumbersOfKeysForFacts(program, loops);
    std::unique_ptr<BodyIndexTable> bodies = indexBodiesForFacts(program, loops, { });
    if (build.diagnostics().hasErrors())
        return;
    updateProgramBehindIndexesForFacts(program, *bodies, loops);
    mechanisms = withoutMechanismsMissingDependencies(mechanisms);
    ThinSummaries summaries = scanBodies(program, build.diagnostics(), loops, mechanisms);
    if (build.diagnostics().hasErrors())
        return;
    Families families(program, loops);
    OwnedLooksAtBodies looks = lookAtBodies(program, *bodies, summaries, build);
    printBreachesOfClosureOfLink(out, program, summaries, families, *looks, mechanisms, build);
}

// The lanes of computeFacts() behind Link.
static void orderFunctionsByReferrersForFacts([[maybe_unused]] Program& program, const ParallelLoops& loops, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    layReferrersOfHeldReadingOver(parts.linked, parts.held, loops);
    steps.didFinish("lay referrers over"_s);
    rankReferrersFirst(parts.linked);
    steps.didFinish("rank referrers"_s);
}

static void fillTablesOfFunctionsForFacts(const Program& program, const FactsRequest&, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    fillFunctionTablesForFacts(program, *parts.summaries, parts.linked, parts.tables);
    steps.didFinish("tables of functions"_s);
}

static void fillTablesOfFieldsLayoutsAndClassesForFacts(const Program& program, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        layKeysThatHeldAloneAppendsOver(parts.linked, parts.held, program.layouts.size());
    build.statistics().count(StatisticID::FlowAppendedFields, parts.linked.appendedKeys.size() + parts.linked.keysAppendedByHeldAlone.size());
    fillFieldTablesForFacts(program, parts.linked, parts.mechanisms, parts.tables);
    fillLayoutTablesForFacts(program, parts.linked, parts.held, parts.tables);
    fillTablesOfStringsForFacts(program, *parts.summaries, parts.linked, parts.held, parts.tables);
    fillVariableTablesForFacts(parts.linked, parts.tables);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        fillFunctionsGivenToVariablesForFacts(program, *parts.summaries, parts.linked, parts.tables);
    if (parts.mechanisms.contains(Mechanism::TypedOperations))
        fillClassTablesForFacts(program, *parts.bodies, *parts.summaries, parts.linked, parts.held, parts.tables);
    // What a record expects on a prototype object is no proof, and what is held sees further: a map of descriptors that native code has had is lost to the other reading.
    fillMembersOfClassesForFacts(program, *parts.summaries, parts.held, parts.tables);
    steps.didFinish("tables of fields, layouts, strings, variables, classes"_s);
}

static void fillTablesOfProgramForFacts(const Program& program, const OwnedLooksAtBodies& looks, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    computeFactsOfProgram({ program, *parts.bodies, *looks, *parts.summaries, parts.linked, build }, parts.tables);
    parts.linked.exportsSpiedOnThatWireLeavesShut.appendVector(parts.held.exportsSpiedOnThatWireLeavesShut);
    finishFactsOfTestRun(program, build, parts.tables, WTF::move(parts.linked.exportsSpiedOnThatWireLeavesShut));
    steps.didFinish("facts of the program"_s);
}

std::unique_ptr<Facts> computeFacts(Program& program, BuildContext& build, const FactsRequest& request, std::unique_ptr<BodyIndexTable>&& indexesOfVerifier)
{
    const ParallelLoops& loops = build.parallelLoops();
    auto hasFailed = [&] {
        return build.diagnostics().hasErrors();
    };

    StepTimes steps(build, "facts"_s);
    endStableNumbersOfKeysForFacts(program, loops);
    steps.didFinish("keys of texts"_s);

    Facts::Parts parts;
    parts.mechanisms = withoutMechanismsMissingDependencies(request.mechanisms);
    parts.tables.mocks = findMocksOfProgram(program);
    parts.bodies = indexBodiesForFacts(program, loops, WTF::move(indexesOfVerifier));
    if (hasFailed())
        return nullptr;
    updateProgramBehindIndexesForFacts(program, *parts.bodies, loops);
    steps.didFinish("mocks, indexes of bodies"_s);

    PhaseTimes times(build, "scan and Link"_s);
    parts.summaries = makeUniqueWithoutFastMallocCheck<ThinSummaries>(scanBodies(program, build.diagnostics(), loops, parts.mechanisms));
    times.didFinish("scan"_s);
    if (hasFailed())
        return nullptr;
    parts.families = makeUniqueWithoutFastMallocCheck<Families>(program, loops);
    times.didFinish("families"_s);
    steps.didFinish("scan, families"_s);
    OwnedLooksAtBodies looks = lookAtBodies(program, *parts.bodies, *parts.summaries, build);
    steps.didFinish("look at bodies"_s);
    // ONE FLOW, READ TWICE: link() answers linked() and leaves held() in `turns`.
    TurnsOfReadings turns;
    runLanes(loops, {
        [&] {
            parts.linked = link(program, *parts.summaries, *parts.families, *looks, parts.mechanisms, build, request.orderOfWorklist, &turns);
            if (parts.mechanisms.contains(Mechanism::Guesses))
                parts.layoutsByNames = LayoutsByNames(program, parts.linked);
        },
        // It reads the records of the program, which never move, and writes two lists that nobody else looks at.
        [&] {
            if (parts.mechanisms.contains(Mechanism::BuiltinCalls))
                workOutRegExpTexts(program, *parts.bodies, *parts.summaries, parts.tables.program);
        },
    });
    // The lane says nothing itself: Link reports beside it.
    for (const RegExpCallWithoutText& call : parts.tables.program.regExpCallsWithoutText) {
        const BodyIndex& index = *parts.bodies->find(call.function);
        build.diagnostics().add(UserDiagnostic {
            .kind = UserDiagnosticKind::RegExpTextKnownAtRunTime,
            .function = call.function,
            .block = index.blockOf(call.site),
            .value = call.site,
            .count = 1,
            .location = program.at(call.function).locationOf(index.value(call.site)),
        });
    }
    const SizeOfLinkFlow& sizeOfFlow = parts.linked.sizeOfFlow;
    build.statistics().count(StatisticID::FactNodes, sizeOfFlow.nodes);
    build.statistics().count(StatisticID::FactEdges, sizeOfFlow.edges);
    build.statistics().count(StatisticID::FactsCarriedOverEdges, sizeOfFlow.nodesTakenOffWorklist);
    build.statistics().count(StatisticID::FactRuleCalls, sizeOfFlow.observersTold);
    build.statistics().count(StatisticID::FactRounds, sizeOfFlow.rounds);
    parts.held = WTF::move(turns.held);
    finishReadingsOfLink(program, parts.mechanisms, turns, parts.linked, parts.held);
    // The flow is gone, and it was half as large as all that the build holds beside it.
    build.backgroundJobs().add([](const ParallelLoops&) {
        WTF::releaseFastMallocFreeMemory();
    });
    steps.didFinish("the two readings"_s);
    openNamesFoundByLink(program, build, parts.linked, parts.held);
    steps.didFinish("open names"_s);
    times.didFinish("Link"_s);
    if (hasFailed())
        return nullptr;

    // Each lane fills tables of its own and reads none that another lane fills. Of what is linked the first writes the referrers, the groups, the cycles, the ranks and what of a function goes by them,
    // the second the keys that held() alone appends and the third the exports that are spied on: no other lane asks for any of it.
    runLanes(loops, {
        [&] { orderFunctionsByReferrersForFacts(program, build.parallelLoops(), build, parts); },
        [&] { fillTablesOfFieldsLayoutsAndClassesForFacts(program, build, parts); },
        [&] { fillTablesOfProgramForFacts(program, looks, build, parts); },
        [&] { fillTablesOfFunctionsForFacts(program, request, build, parts); },
    });
    // Each closure of a function has a prototype object of its own.
    for (size_t layout = 0; layout < parts.linked.layouts.size() && layout < parts.tables.classes.size(); ++layout) {
        if (parts.tables.classes[layout].evaluations != ExecutionCount::AtMostOnce)
            parts.linked.layouts[layout].prototypeObjectIsSettledAheadOfEveryObject = false;
    }
    // It interns types, so it runs beside nobody. No lane reads what it writes.
    parts.linked.fieldsReadAsIntegers = fieldsReadAsIntegersIn(*looks);
    parts.linked.fieldsReadForArithmetic = fieldsReadForArithmeticIn(*looks);
    parts.linked.fieldsStoredWhatIsNoInt32 = fieldsStoredWhatIsNoInt32In(*looks);
    parts.linked.fieldsStoredCounts = fieldsStoredCountsIn(*looks, program, parts.linked);
    parts.linked.keysStoredWhatIsNoInt32 = keysStoredWhatIsNoInt32In(*looks);
    fillEnforcedFieldsForFacts(program, parts.linked, parts.held, *parts.families, parts.mechanisms, parts.tables);
    steps.didFinish("referrers beside tables"_s);
    times.didFinish("referrers, tables"_s);
    if (hasFailed())
        return nullptr;
    return makeUnique<Facts>(program, WTF::move(parts));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
