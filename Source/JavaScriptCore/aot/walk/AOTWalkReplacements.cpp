/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// At a suspension: nothing. What suspends among the replaced operations goes to AOTWalkIteration.cpp and AOTWalkCoroutines.cpp, and nothing that is built here suspends.
// At a throw: what is built here throws where its own operations throw, under the handler of the block being walked.
// It is never withheld.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTWalkKnown.h"
#include "AOTRepresentationRules.h"
#include "AOTThinSummary.h"
#include "AOTWalkStringFolds.h"
#include "JSArrayIterator.h"
#include "JSMapIterator.h"
#include "JSSetIterator.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!static_cast<uint32_t>(StepArrayIteratorResult::Done) && static_cast<uint32_t>(StepArrayIteratorResult::Value) == 1);
static_assert(static_cast<uint32_t>(SetNextResult::HasEntry) == static_cast<uint32_t>(MapNextResult::HasEntry) && static_cast<uint32_t>(SetNextResult::Entry) == static_cast<uint32_t>(MapNextResult::Entry) && static_cast<uint32_t>(SetNextResult::Element) == static_cast<uint32_t>(MapNextResult::Key));
static_assert(static_cast<uint32_t>(JSMapIterator::Field::Entry) == static_cast<uint32_t>(JSSetIterator::Field::Entry) && static_cast<uint32_t>(JSMapIterator::Field::IteratedObject) == static_cast<uint32_t>(JSSetIterator::Field::IteratedObject) && static_cast<uint32_t>(JSMapIterator::Field::Kind) == static_cast<uint32_t>(JSSetIterator::Field::Kind));

static constexpr OpcodeSet opcodesOfIterationReplacements { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread };
static constexpr OpcodeSet opcodesOfCoroutineReplacements { Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope };
// Code::append() turns away every operation that states a type, so Upcast and NarrowByField go the way of Convert and Check, whatever range their rows have.
static constexpr OpcodeSet opcodesOfAllReplacements = opcodesOfIterationReplacements | opcodesOfCoroutineReplacements | opcodesThatStateTypes | OpcodeSet { Opcode::HasJSType, Opcode::ArraySpeciesCreate } | OpcodeSet {
    Opcode::CreateIteratorResult, Opcode::StepArrayIterator, Opcode::StepMapIterator, Opcode::StepSetIterator, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::Call, Opcode::DefineMethod, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::PropGet, Opcode::PropSet
} | callsByValue | constructionsByValue;

namespace {

enum class CollectionSteppedByReplacement : uint8_t { Map, Set };

class BuilderOfReplacements {
    WTF_MAKE_NONCOPYABLE(BuilderOfReplacements);
public:
    BuilderOfReplacements(Walker& walker, const BodyValue& replaced)
        : m_walker(walker)
        , m_origin(walker.draftOf(replaced).origin())
    {
        m_origin.isTailCall = false;
    }

    Walker& walker() { return m_walker; }
    // A build gave nothing, and the walk has noted why. Nothing more is built.
    bool hasStopped() const { return m_hasStopped; }

    Draft draft(Opcode opcode) const { return Draft(opcode, m_origin); }

    Reference build(Draft&& operation)
    {
        if (m_hasStopped)
            return { };
        m_walker.chooseConservativeFields(operation);
        return buildAsItStands(WTF::move(operation));
    }

    Reference undefined() { return constant(draft(Opcode::Undefined)); }

    Reference boolean(bool value)
    {
        Draft written = draft(Opcode::Boolean);
        written.setBoolean(value);
        return constant(WTF::move(written));
    }

    Reference integer(int64_t value)
    {
        Draft written = draft(Opcode::Integer);
        written.setInteger(value);
        return constant(WTF::move(written));
    }

    Reference applyOperator(Opcode opcode, Reference left, Reference right)
    {
        Draft operation = draft(opcode);
        operation.setLeftOperand(left);
        operation.setRightOperand(right);
        return build(WTF::move(operation));
    }

    Reference resultAt(Reference results, uint32_t index)
    {
        Draft read = draft(Opcode::ResultAt);
        read.setResultsOperand(results);
        read.setIndex(index);
        return build(WTF::move(read));
    }

    template<typename InternalField>
    Reference getInternalField(Reference object, InternalField field)
    {
        Draft read = draft(Opcode::GetInternalField);
        read.setObjectOperand(object);
        read.setIndex(static_cast<uint32_t>(field));
        return build(WTF::move(read));
    }

    template<typename InternalField>
    void putInternalField(Reference object, InternalField field, Reference value)
    {
        Draft store = draft(Opcode::PutInternalField);
        store.setObjectOperand(object);
        store.setIndex(static_cast<uint32_t>(field));
        store.setValueOperand(value);
        build(WTF::move(store));
    }

    // For what the compiler itself has laid down: a claim of the library, a field that the engine and compiled code alone write. It is no guess and has no closed arm.
    // A row that takes one kind of operand has no form for any other, and Code has no way to state a type. So the type is tested for, and the failing side traps.
    Reference proveOrTrap(Reference value, TypeID promised)
    {
        if (m_hasStopped)
            return { };
        TypeArena& types = m_walker.types();
        if (types.isSubset(m_walker.typeOf(value), promised))
            return value;

        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(value);
        test.setTestedType(types.testablePartOf(promised));
        Reference passes = build(WTF::move(test));
        if (m_hasStopped)
            return { };

        BlockID passing = m_walker.addBlock();
        BlockID failing = m_walker.addBlock(BlockFrequency::Rare);
        m_walker.branch(passes, passing, failing);
        m_walker.continueIn(failing);
        m_walker.code().end(failing, TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
        m_walker.continueIn(passing);

        Draft narrowed = draft(Opcode::Narrowed);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        return buildAsItStands(WTF::move(narrowed));
    }

    // For a parameter of a join, which is boxed here. A cell is the same word.
    Reference boxed(Reference value)
    {
        if (m_hasStopped)
            return { };
        Representation held = m_walker.representationOf(value);
        if (isSameWord(held, Representation::JSValue))
            return value;
        std::optional<ConversionPath> path = findConversion(m_walker.types(), m_walker.typeOf(value), held, Representation::JSValue);
        if (!path) {
            m_walker.fail("what an arm of a replacement gives has no boxed form"_s);
            m_hasStopped = true;
            return { };
        }
        for (ConversionStep step : *path) {
            Draft conversion = draft(step.opcode);
            conversion.setValueOperand(value);
            m_walker.chooseConservativeFields(conversion);
            conversion.representation = step.to;
            value = buildAsItStands(WTF::move(conversion));
            if (m_hasStopped)
                return { };
        }
        return value;
    }

private:
    Reference buildAsItStands(Draft&& operation)
    {
        bool givesResult = resultRuleOf(operation.opcode) != ResultRule::None;
        Reference built = m_walker.build(WTF::move(operation));
        if (givesResult && !built)
            m_hasStopped = true;
        return built;
    }

    // Left unchosen it would get the form of its type. Every use here is a parameter of a join, an operand of a closed operator or an engine field, which hold boxed values.
    Reference constant(Draft&& written)
    {
        if (m_hasStopped)
            return { };
        written.representation = Representation::JSValue;
        Reference built = m_walker.constant(WTF::move(written));
        if (!built)
            m_hasStopped = true;
        return built;
    }

    Walker& m_walker;
    Origin m_origin;
    bool m_hasStopped { false };
};

// A branch, two arms, and the block in which they meet. Once made, the walk stands in the arm that is taken where the condition holds.
class ArmsOfReplacements {
    WTF_MAKE_NONCOPYABLE(ArmsOfReplacements);
public:
    ArmsOfReplacements(BuilderOfReplacements& builder, Reference condition, uint32_t numberOfResults)
        : m_builder(builder)
    {
        if (builder.hasStopped())
            return;
        Walker& walker = builder.walker();
        BlockID firstArm = walker.addBlock();
        m_secondArm = walker.addBlock();
        m_join = walker.addBlock();
        for (uint32_t index = 0; index < numberOfResults; ++index)
            m_results.append(walker.addParameter(m_join, { }, Representation::JSValue));
        walker.branch(condition, firstArm, m_secondArm);
        walker.continueIn(firstArm);
    }

    void endFirstArm(std::initializer_list<Reference> values)
    {
        jumpToJoin(values);
        if (!m_builder.hasStopped())
            m_builder.walker().continueIn(m_secondArm);
    }

    // The walk goes on behind the arms. Walker::jumpTo() has typed each result by the join of what arrived.
    void endSecondArm(std::initializer_list<Reference> values) { jumpToJoin(values); }

    Reference result(uint32_t index = 0) const { return m_builder.hasStopped() ? Reference() : m_results[index]; }

private:
    void jumpToJoin(std::initializer_list<Reference> values)
    {
        if (m_builder.hasStopped())
            return;
        RELEASE_ASSERT(values.size() == m_results.size());
        Vector<Reference, 2> arguments;
        for (Reference value : values)
            arguments.append(m_builder.boxed(value));
        if (m_builder.hasStopped())
            return;
        m_builder.walker().jumpTo(m_join, arguments.span());
    }

    BuilderOfReplacements& m_builder;
    BlockID m_secondArm;
    BlockID m_join;
    Vector<Reference, 2> m_results;
};

} // anonymous namespace

// ---- Operations that state a type

// Null: the Convert claims nothing. Its type is what the source declares, which Guesses reads off the body.
static TypeID typeClaimedByConvertForReplacement(Walker& walker, const DeclaredType& declared, Reference operand)
{
    if (!declared.has(PositionFlags::IsClaim))
        return { };
    TypingContext context { walker.program(), walker.types(), PositionTypes::knowingNothing() };
    ConvertResolution resolution = resolveConvert(context, walker.typeOf(operand), declared);
    switch (resolution.step) {
    case ConvertStep::Nothing:
    case ConvertStep::Test:
        return { };
    case ConvertStep::TakeAsFact:
        return resolution.target;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void bindOperandInPlaceOfCastForReplacement(Walker& walker, const BodyValue& cast)
{
    // What the tests have left of the value is the value, under the type that they prove.
    Reference operand = walker.referenceOf(cast.operand(cast.opcode() == Opcode::NarrowByTest ? OperandIndex::NarrowByTest::left : 0));
    if (cast.opcode() == Opcode::Convert)
        checkInt32Promise(walker, cast.as<Opcode::Convert>().declared(), cast.operand(0), walker.originOf(cast));
    if (cast.opcode() == Opcode::Convert && !walker.currentBodyIndex().isUnused(cast.id())) {
        if (TypeID claimed = typeClaimedByConvertForReplacement(walker, cast.as<Opcode::Convert>().declared(), operand)) {
            BuilderOfReplacements builder(walker, cast);
            operand = builder.proveOrTrap(operand, claimed);
            if (builder.hasStopped())
                return;
        }
    }
    walker.bind(cast.id(), operand);
}

// ---- CreateIteratorResult

static void buildIteratorResultAsReplacement(Walker& walker, const BodyValue& creation)
{
    const Program& program = walker.program();
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);

    auto written = creation.as<Opcode::CreateIteratorResult>();
    BuilderOfReplacements builder(walker, creation);
    Reference done = builder.proveOrTrap(walker.referenceOf(written.doneOperand()), TypeTable::boolean);

    Draft object = builder.draft(Opcode::NewObject);
    object.setLayout(layoutID);
    object.setFields(std::array { *valueField, *doneField });
    object.setValuesOperands(std::array { walker.referenceOf(written.valueOperand()), done });
    Reference result = builder.build(WTF::move(object));
    if (!builder.hasStopped())
        walker.bind(creation.id(), result);
}

// ---- What the steps of the engine's iterators share

static Reference buildKindTestForReplacement(BuilderOfReplacements& builder, Reference kind, JSC::IterationKind tested)
{
    return builder.applyOperator(Opcode::StrictEq, kind, builder.integer(static_cast<int64_t>(tested)));
}

static Reference buildPairForReplacement(BuilderOfReplacements& builder, Reference key, Reference value)
{
    Draft pair = builder.draft(Opcode::NewArray);
    // The program may store anything into the pair.
    pair.setContents(TypeTable::anything);
    pair.setElementsOperands(std::array { key, value });
    return builder.build(WTF::move(pair));
}

static void bindStepForReplacement(BuilderOfReplacements& builder, const BodyValue& step, Reference done, Reference value)
{
    Draft results = builder.draft(Opcode::Results);
    results.setValuesOperands(std::array { done, value });
    Reference built = builder.build(WTF::move(results));
    if (builder.hasStopped())
        return;
    // Whichever arm was walked last, the step as a whole has done what its row declares.
    builder.walker().state().invalidate(declaredBoundOf(step.opcode()));
    builder.walker().bind(step.id(), built);
}

// ---- StepArrayIterator

static Reference buildLengthOfIteratedObjectForReplacement(BuilderOfReplacements& builder, Reference iterated)
{
    Draft test = builder.draft(Opcode::HasJSType);
    test.setValueOperand(iterated);
    test.setFirst(CheckedJSType::Int8ArrayType);
    test.setLast(CheckedJSType::BigUint64ArrayType);
    Reference isView = builder.build(WTF::move(test));

    ArmsOfReplacements arms(builder, isView, 1);
    {
        Draft function = builder.draft(Opcode::Builtin);
        function.setBuiltin(Builtins::LinkTimeConstantTypedArrayLength);
        function.setIsNotReadFromHolder(true);
        Draft call = builder.draft(Opcode::Call);
        call.setCalleeOperand(builder.build(WTF::move(function)));
        call.setArgumentsOperands(std::array { iterated });
        arms.endFirstArm({ builder.build(WTF::move(call)) });
    }
    {
        Draft read = builder.draft(Opcode::PropGet);
        read.setObjectOperand(iterated);
        read.setKey(builder.walker().program().keyOf(BuiltinNames::length));
        Draft length = builder.draft(Opcode::ToLength);
        length.setValueOperand(builder.build(WTF::move(read)));
        arms.endSecondArm({ builder.build(WTF::move(length)) });
    }
    return arms.result();
}

static Reference buildValueAtPositionOfArrayIteratorForReplacement(BuilderOfReplacements& builder, Reference iterator, Reference iterated, Reference position)
{
    Reference kind = builder.getInternalField(iterator, JSArrayIterator::Field::Kind);
    ArmsOfReplacements byKeys(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Keys), 1);
    byKeys.endFirstArm({ position });

    Draft read = builder.draft(Opcode::ElemGet);
    read.setObjectOperand(iterated);
    read.setPropertyOperand(position);
    Reference element = builder.build(WTF::move(read));
    ArmsOfReplacements byEntries(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Entries), 1);
    byEntries.endFirstArm({ buildPairForReplacement(builder, position, element) });
    byEntries.endSecondArm({ element });

    byKeys.endSecondArm({ byEntries.result() });
    return byKeys.result();
}

static void buildStepOfArrayIteratorAsReplacement(Walker& walker, const BodyValue& step)
{
    BuilderOfReplacements builder(walker, step);
    Reference iterator = builder.proveOrTrap(walker.referenceOf(step.as<Opcode::StepArrayIterator>().iteratorOperand()), TypeTable::object);
    Reference index = builder.getInternalField(iterator, JSArrayIterator::Field::Index);

    ArmsOfReplacements byFinished(builder, builder.applyOperator(Opcode::StrictEq, index, builder.integer(JSArrayIterator::doneIndex)), 2);
    byFinished.endFirstArm({ builder.boolean(true), builder.undefined() });

    Reference iterated = builder.getInternalField(iterator, JSArrayIterator::Field::IteratedObject);
    Reference length = buildLengthOfIteratedObjectForReplacement(builder, iterated);
    ArmsOfReplacements byRange(builder, builder.applyOperator(Opcode::Less, index, length), 2);
    // The iterator moves on in front of the read. A getter or a trap that the read runs may step this iterator again, and a read that throws has used up its position.
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.applyOperator(Opcode::Add, index, builder.integer(1)));
    byRange.endFirstArm({ builder.boolean(false), buildValueAtPositionOfArrayIteratorForReplacement(builder, iterator, iterated, index) });
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.integer(JSArrayIterator::doneIndex));
    byRange.endSecondArm({ builder.boolean(true), builder.undefined() });

    byFinished.endSecondArm({ byRange.result(0), byRange.result(1) });
    bindStepForReplacement(builder, step, byFinished.result(0), byFinished.result(1));
}

// ---- StepMapIterator, StepSetIterator

static BuiltinClassID classOfCollectionSteppedByReplacement(CollectionSteppedByReplacement collection)
{
    switch (collection) {
    case CollectionSteppedByReplacement::Map:
        return BuiltinClasses::Map;
    case CollectionSteppedByReplacement::Set:
        return BuiltinClasses::Set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Reference buildNextEntryForReplacement(BuilderOfReplacements& builder, CollectionSteppedByReplacement kindOfCollection, Reference collection, Reference table, Reference entry)
{
    switch (kindOfCollection) {
    case CollectionSteppedByReplacement::Map: {
        Draft next = builder.draft(Opcode::MapNext);
        next.setMapOperand(collection);
        next.setTableOperand(table);
        next.setEntryOperand(entry);
        return builder.build(WTF::move(next));
    }
    case CollectionSteppedByReplacement::Set: {
        Draft next = builder.draft(Opcode::SetNext);
        next.setSetOperand(collection);
        next.setTableOperand(table);
        next.setEntryOperand(entry);
        return builder.build(WTF::move(next));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void putTableOfCollectionIteratorForReplacement(BuilderOfReplacements& builder, Reference iterator, Reference table)
{
    Draft store = builder.draft(Opcode::PutCollectionIteratorTable);
    store.setObjectOperand(iterator);
    store.setValueOperand(table);
    builder.build(WTF::move(store));
}

// `key` and `value` are one reference for a set, whose keys are its values.
static Reference buildValueOfCollectionIteratorKindForReplacement(BuilderOfReplacements& builder, Reference kind, Reference key, Reference value)
{
    ArmsOfReplacements byEntries(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Entries), 1);
    byEntries.endFirstArm({ buildPairForReplacement(builder, key, value) });
    if (key == value) {
        byEntries.endSecondArm({ value });
        return byEntries.result();
    }

    ArmsOfReplacements byKeys(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Keys), 1);
    byKeys.endFirstArm({ key });
    byKeys.endSecondArm({ value });
    byEntries.endSecondArm({ byKeys.result() });
    return byEntries.result();
}

static Reference buildValueOfFoundEntryForReplacement(BuilderOfReplacements& builder, CollectionSteppedByReplacement kindOfCollection, Reference iterator, Reference collection, Reference found)
{
    Reference entry = builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Entry));
    Reference key = builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Key));
    Reference value = kindOfCollection == CollectionSteppedByReplacement::Map ? builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Value)) : key;
    builder.putInternalField(iterator, JSSetIterator::Field::Entry, builder.applyOperator(Opcode::Add, entry, builder.integer(1)));

    // The entry counts in the table that the collection has now: the step follows a table that was replaced to its successor.
    Draft current = builder.draft(Opcode::CollectionTable);
    current.setCollectionOperand(collection);
    putTableOfCollectionIteratorForReplacement(builder, iterator, builder.build(WTF::move(current)));

    return buildValueOfCollectionIteratorKindForReplacement(builder, builder.getInternalField(iterator, JSSetIterator::Field::Kind), key, value);
}

static void buildStepOfCollectionIteratorAsReplacement(Walker& walker, const BodyValue& step, CollectionSteppedByReplacement kindOfCollection)
{
    BuilderOfReplacements builder(walker, step);
    Reference iterator = builder.proveOrTrap(walker.referenceOf(step.operand(0)), TypeTable::object);

    Draft read = builder.draft(Opcode::GetCollectionIteratorTable);
    read.setObjectOperand(iterator);
    Reference table = builder.build(WTF::move(read));
    Draft test = builder.draft(Opcode::IsCollectionIteratorClosedMark);
    test.setTableOperand(table);

    ArmsOfReplacements byFinished(builder, builder.build(WTF::move(test)), 2);
    byFinished.endFirstArm({ builder.boolean(true), builder.undefined() });

    Reference collection = builder.proveOrTrap(builder.getInternalField(iterator, JSSetIterator::Field::IteratedObject), walker.types().builtinClass(classOfCollectionSteppedByReplacement(kindOfCollection)));
    Reference entry = builder.proveOrTrap(builder.getInternalField(iterator, JSSetIterator::Field::Entry), TypeTable::integer);
    Reference found = buildNextEntryForReplacement(builder, kindOfCollection, collection, table, entry);
    ArmsOfReplacements byEntry(builder, builder.resultAt(found, static_cast<uint32_t>(MapNextResult::HasEntry)), 2);
    byEntry.endFirstArm({ builder.boolean(false), buildValueOfFoundEntryForReplacement(builder, kindOfCollection, iterator, collection, found) });
    putTableOfCollectionIteratorForReplacement(builder, iterator, builder.build(builder.draft(Opcode::CollectionIteratorClosedMark)));
    byEntry.endSecondArm({ builder.boolean(true), builder.undefined() });

    byFinished.endSecondArm({ byEntry.result(0), byEntry.result(1) });
    bindStepForReplacement(builder, step, byFinished.result(0), byFinished.result(1));
}

// ---- EvaluateModule

// A start asks the status of its module itself. The body of a module without a state is called where the one walk that comes to it stands. Either takes nothing but `this`, which is undefined.
static void buildCallOfModuleStepAsReplacement(BuilderOfReplacements& builder, const ModuleStep& step, BuiltinID calledBuiltin)
{
    Walker& walker = builder.walker();
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit entered = step.calleeIsStart ? CodeUnit { step.callee, CodeRole::ModuleStart } : conventions.directEntryOf(step.callee);
    if (!conventions.exists(entered)) {
        walker.fail("a module is evaluated by a call of a unit that does not exist"_s);
        return;
    }
    Vector<Reference, 1> operands;
    for (unsigned index = walker.program().at(step.callee).numberOfEntryParameters(); index--;)
        operands.append(builder.undefined());

    Draft call = builder.draft(Opcode::CallDirect);
    call.setFunction(step.callee);
    call.setArgumentsOperands(operands.span());
    call.setRole(entered.role);
    if (calledBuiltin)
        call.setCalledBuiltin(calledBuiltin);
    call.setIsNeverInlined(true);
    builder.build(WTF::move(call));
}

static void buildStatusOperationOfModuleAsReplacement(BuilderOfReplacements& builder, Opcode opcode, ModuleID module)
{
    Draft operation = builder.draft(opcode);
    operation.setModule(module);
    builder.build(WTF::move(operation));
}

static void buildModuleStepsAsReplacement(BuilderOfReplacements& builder, std::span<const ModuleStep> steps, BuiltinID calledBuiltin)
{
    const ModuleEvaluationPlan& evaluation = builder.walker().plan().moduleEvaluation();
    for (const ModuleStep& step : steps) {
        if (builder.hasStopped())
            return;
        if (step.callee) {
            if (step.calleeIsStart || !evaluation.leavesOutCallOf(step.callee))
                buildCallOfModuleStepAsReplacement(builder, step, calledBuiltin);
            continue;
        }
        if (step.moduleThatBegins) {
            buildStatusOperationOfModuleAsReplacement(builder, Opcode::BeginModuleEvaluation, step.moduleThatBegins);
            continue;
        }
        // The loader may hold the status in a record, and the run time takes it from there.
        RELEASE_ASSERT(step.moduleThatHasRun);
        buildStatusOperationOfModuleAsReplacement(builder, Opcode::BeginModuleEvaluation, step.moduleThatHasRun);
        buildStatusOperationOfModuleAsReplacement(builder, Opcode::FinishModuleEvaluation, step.moduleThatHasRun);
    }
}

// The request to the loader names the path that the module has now, which Image leaves alone while ModuleEvaluationPlan::isPathNamedBySites() says so.
static void buildEvaluationOfModuleAsReplacement(Walker& walker, const BodyValue& evaluation)
{
    auto written = evaluation.as<Opcode::EvaluateModule>();
    const Module& target = walker.program().at(written.module());

    BuilderOfReplacements builder(walker, evaluation);
    ModuleSitePlan site = walker.plan().moduleEvaluation().siteOf(walker.currentFunction(), evaluation.id(), written.module());
    if (!site.asksLoader) {
        buildModuleStepsAsReplacement(builder, site.steps, written.isCallOfRequire() ? functionOfHostThatRequireCallsFor(target) : BuiltinID());
        return;
    }
    Draft request = builder.draft(Opcode::EvaluateRequiredModule);
    request.setModule(written.module());
    request.setReferrer(written.referrer());
    request.setSpecifier(target.path);
    if (written.isCallOfRequire())
        request.setCalledBuiltin(functionOfHostThatRequireCallsFor(target));
    builder.build(WTF::move(request));
}

// ---- A symbol that is made once

PropertyKeyID keyOfSymbolMadeOnce(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::Call)
        return { };
    const BodyIndex& body = walker.currentBodyIndex();
    PropertyKeyID key = keyOfSymbolThatCallMakes(walker.program(), walker.currentFunction(), value, scopedLambda<BodyValue(ValueID)>([&](ValueID id) {
        return body.value(id);
    }));
    if (!key)
        return { };
    // Link has typed what the call answers by the scan's record of it. Whoever counted the loops again would count the edges that are written: `do { .. } while (false)`.
    const Vector<SummaryCall>& calls = walker.facts().summaries().at(walker.currentFunction()).calls;
    auto record = std::ranges::find_if(calls, [&](const SummaryCall& call) {
        return call.site == value.id() && call.callee == SummaryCall::Callee::Builtin;
    });
    return record != calls.end() ? keyOfSymbolMadeOnceBy(walker.program(), *record) : PropertyKeyID();
}

static bool replaceCallThatMakesSymbolOnce(Walker& walker, const BodyValue& call)
{
    PropertyKeyID key = keyOfSymbolMadeOnce(walker, call);
    if (!key)
        return false;
    Draft symbol(Opcode::KeyConstant, walker.originOf(call));
    symbol.setKey(key);
    walker.bind(call.id(), walker.constant(WTF::move(symbol)));
    return true;
}

// ---- A member that the record of its class creates

// The run time makes such a member with the class, or when it is first looked for: a closure of the function over the one environment that the class object captures (ClassTable::createMethodClosure()).
// So the definition goes only where the closure as written is that closure and nobody else has seen it. Nothing names the closure then, and the sweep drops it.
// A definition that stays defines the member once more, in its place among the keys. The plan leaves no member to a record where TypedOperations is withheld from the build.
static bool leaveMemberToClassRecord(Walker& walker, const BodyValue& value)
{
    auto definition = value.as<Opcode::DefineMethod>();
    if (!definition.key() || definition.computedKeyOperand())
        return false;
    const BodyIndex& body = walker.currentBodyIndex();
    auto isMadeBy = [&](ValueID id, Opcode opcode) {
        return id && !body.isParameter(id) && body.opcodeOf(id) == opcode;
    };
    auto inFrontOfCasts = [&](ValueID id) {
        while (id && !body.isParameter(id) && opcodesThatStateTypes.contains(body.opcodeOf(id)))
            id = body.value(id).operand(0);
        return id;
    };
    ValueID closure = definition.functionOperand();
    if (!isMadeBy(closure, Opcode::NewClosure) || !body.hasOneUse(closure))
        return false;
    auto member = body.value(closure).as<Opcode::NewClosure>();

    // The members of a class are numbered as they are written, so the number of one is its place in the list.
    LayoutID classLayout = definition.layout();
    std::span<const ClassMember> members = walker.facts().membersOf(classLayout);
    const Vector<MemberCreation>& creations = walker.plan().slots().classCreation(classLayout).members;
    uint32_t place = definition.index();
    if (place >= members.size() || place >= creations.size() || creations[place] != MemberCreation::ByClassRecord)
        return false;
    const ClassMember& listed = members[place];
    if (listed.function != member.function() || listed.key != definition.key() || listed.side != definition.side() || listed.kind != definition.methodKind())
        return false;

    std::span<const ValueID> capturesOfMember = member.capturesOperands();
    if (capturesOfMember.empty())
        return true;
    ValueID classObject = inFrontOfCasts(definition.classObjectOperand());
    if (capturesOfMember.size() != 1 || !isMadeBy(classObject, Opcode::NewClass))
        return false;
    auto newClass = body.value(classObject).as<Opcode::NewClass>();
    ValueID constructor = inFrontOfCasts(newClass.constructorOperand());
    if (newClass.layout() != classLayout || !isMadeBy(constructor, Opcode::NewClosure))
        return false;
    std::span<const ValueID> capturesOfClassObject = body.value(constructor).as<Opcode::NewClosure>().capturesOperands();
    return capturesOfClassObject.size() == 1 && inFrontOfCasts(capturesOfClassObject[0]) == inFrontOfCasts(capturesOfMember[0]);
}

// ---- The accessors of a literal

// `{ get a() { .. }, get b() { .. }, .. }`: a closure, a definition, a closure, a definition. One operation lays them all: the run time is entered once, and finds the layout once.
// Nothing but closures is made between the first and the last, so nobody can tell that the first is laid where the last is written.
// WHO GOES WITH WHOM IS READ OFF THE BODY AS IT IS WRITTEN, so every copy of it and both arms of a guess say the same: the definitions on one object, under written names, that stand side by side but for
// the closures, within one row of numbers of values. Such a row holds no more of them than the masks of the operation have bits.
static constexpr uint32_t valuesInRowOfAccessors = 32;

static bool layAccessorsOfLiteralTogether(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& body = walker.currentBodyIndex();
    BodyBlock block = body.block(body.blockOf(value.id()));
    ValueID object = value.objectOperand();
    auto laysAccessorThere = [&](const BodyValue& other) {
        if ((other.opcode() != Opcode::DefineGetter && other.opcode() != Opcode::DefineSetter) || other.objectOperand() != object)
            return false;
        return other.computedKeyOperand() ? !!nameWrittenAsKey(walker, other.computedKeyOperand()) : !!other.key();
    };
    if (!laysAccessorThere(value))
        return false;

    uint32_t row = value.id().index() / valuesInRowOfAccessors;
    // The nearest definition of the group on that side. Null: there is none.
    auto neighbor = [&](ValueID from, int32_t step) -> ValueID {
        for (uint32_t index = from.index() + step; index >= block.firstValue().index() && index < block.endOfValues().index() && index / valuesInRowOfAccessors == row; index += step) {
            BodyValue other = body.value(ValueID(index));
            if (laysAccessorThere(other))
                return other.id();
            // What spells a key that is written does nothing that anybody sees.
            bool spellsWrittenKey = other.opcode() == Opcode::String || (other.opcode() == Opcode::ToPropertyKey && nameWrittenAsKey(walker, other.id()));
            if (other.opcode() != Opcode::NewClosure && !spellsWrittenKey)
                break;
        }
        return { };
    };
    if (neighbor(value.id(), 1))
        return true;

    Vector<ValueID, valuesInRowOfAccessors> group { value.id() };
    while (ValueID earlier = neighbor(group.last(), -1))
        group.append(earlier);
    if (group.size() == 1)
        return false;
    group.reverse();

    Vector<PropertyKeyID, valuesInRowOfAccessors> keys;
    Vector<Reference, valuesInRowOfAccessors> functions;
    uint32_t getterMask = 0;
    for (uint32_t index = 0; index < group.size(); ++index) {
        BodyValue definition = body.value(group[index]);
        keys.append(definition.computedKeyOperand() ? nameWrittenAsKey(walker, definition.computedKeyOperand()) : definition.key());
        functions.append(walker.referenceOf(definition.functionOperand()));
        if (definition.opcode() == Opcode::DefineGetter)
            getterMask |= 1u << index;
    }
    Draft accessors(Opcode::DefineAccessors, walker.originOf(value));
    accessors.setObjectOperand(walker.referenceOf(object));
    accessors.setKeys(keys.span());
    accessors.setGetterMask(getterMask);
    accessors.setEnumerableMask(group.size() == 32 ? UINT32_MAX : (1u << group.size()) - 1);
    accessors.setFunctionsOperands(functions.span());
    walker.build(WTF::move(accessors));
    return true;
}

// ---- An access under a key that is proven

static Opcode accessByNameInPlaceOf(Opcode written)
{
    switch (written) {
    case Opcode::ElemGet:
        return Opcode::PropGet;
    case Opcode::ElemSet:
        return Opcode::PropSet;
    case Opcode::ElemHas:
        return Opcode::PropHas;
    case Opcode::ElemDelete:
        return Opcode::PropDelete;
    case Opcode::ElemDefine:
        return Opcode::PropDefine;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PropertyKeyID keyThatAccessGoesBy(const Walker& walker, const BodyValue& access)
{
    if (!accessesUnderComputedKeys.contains(access.opcode()))
        return { };
    // It stands for a call of the built-in, which a stack trace shows.
    if (access.opcode() == Opcode::ElemDefine && access.as<Opcode::ElemDefine>().calledBuiltin())
        return { };
    if (walker.has(Mechanism::TypedOperations)) {
        if (PropertyKeyID proven = walker.facts().linked().keyAt(walker.currentFunction(), access.id()))
            return proven;
        return keyOfKnownTextThatAccessGoesBy(walker, access);
    }
    if (walker.has(Mechanism::DirectCalls))
        return { };

    Reference built = walker.referenceBoundTo(access.propertyOperand());
    if (!built || !built.isConstant() || walker.code().at(built.id()).opcode != Opcode::KeyConstant)
        return { };
    const Program& program = walker.program();
    PropertyKeyID key = walker.code().at(built.id()).key();
    bool isSymbol = program.at(key).kind == PropertyKeyKind::CreatedSymbol;
    // Whether the accesses under a symbol of the library are accesses by name Link alone says: LinkFlow::keyOf().
    return isSymbol && program.types.hasStableNumber(key) && !isSymbolOfLibrary(program, key) ? key : PropertyKeyID();
}

// The fields of the two operations stand in one order, with the key in the place of the operand.
AccessUnderProvenKey::AccessUnderProvenKey(const BodyValue& written, PropertyKeyID key)
    : m_entryOfKey(key.bits() + 1)
    , m_unit(written.unit())
    , m_id(written.id())
{
    static constexpr uint32_t placeOfKey = 1;
    Opcode opcode = accessByNameInPlaceOf(written.opcode());
    m_unit.tables[static_cast<size_t>(RemappedIDKind::PropertyKey)] = { &m_entryOfKey, 1 };

    const uint32_t* words = written.words();
    uint32_t place = 0;
    m_words[place++] = (words[0] & ~BodyValue::opcodeMask) | static_cast<uint32_t>(opcode);
    for (uint32_t word = 1; word < BodyValue::numberOfHeadWords; ++word)
        m_words[place++] = words[word];
    RELEASE_ASSERT(!hasFlagsWordOnWire(written.opcode()));
    if (hasFlagsWordOnWire(opcode))
        m_words[place++] = 0;

    uint32_t firstField = place;
    uint32_t numberOfFields = numberOfFieldWords(opcode, m_words.data() + firstField);
    RELEASE_ASSERT(numberOfFields <= numberOfFieldWords(written.opcode(), written.fieldWords().words()) && firstField + numberOfFields < m_words.size());
    for (uint32_t field = 0; field < numberOfFields; ++field)
        m_words[place++] = field == placeOfKey ? 0 : written.fieldWords().word(field);
    if (words[0] & Wire::WrittenPositionFollows)
        m_words[place++] = words[written.sizeInWords() - 1];

    BodyValue made = value();
    RELEASE_ASSERT(made.sizeInWords() == place && made.key() == key && made.objectOperand() == written.objectOperand() && made.position() == written.position());
}

BodyValue Walker::accessAsWalked(const BodyValue& written)
{
    PropertyKeyID key = keyThatAccessGoesBy(*this, written);
    if (!key)
        return written;
    auto& known = m_frames.last().accessesUnderProvenKeys;
    for (const std::unique_ptr<AccessUnderProvenKey>& access : known) {
        if (access->id() == written.id() && access->key() == key)
            return access->value();
    }
    known.append(makeUnique<AccessUnderProvenKey>(written, key));
    return known.last()->value();
}

// ---- A name of the global object

// Image numbers the name of each binding and Plan keeps its sets by the key, so the key is a name of the program's table. An index and a number in print may be elements, and `__proto__` is an accessor of Object.prototype.
static bool mayHaveGlobalBindingForReplacement(const Program& program, const BodyValue& access)
{
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || !program.types.hasStableNumber(key))
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::CreatedSymbol:
        break;
    // The library's `$name`. Written as a free name it keeps for good what it has found, and so does the record: isFixed() of AOTGlobalBindingOperations.cpp. Read off the object it is asked for each time.
    case PropertyKeyKind::EnginePrivateName:
        return access.opcode() == Opcode::PropGet && access.isFreeName();
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

// A free name, and a name that is read off or stored on the global object. The first access files in a record of the instance what it has found: the value under a watch, or the place where it lies.
// Whoever stores, defines or deletes the name, or declares it in a script, empties the record, whether the build has seen that code or not. So nothing has to be known of the name, and no lock is leaned on.
static bool replaceAccessToGlobalName(Walker& walker, const BodyValue& access)
{
    const Program& program = walker.program();
    PropertyKeyID key = access.key();
    // The record of the receiver is at hand: nearly none is the global object.
    Reference object = walker.referenceOf(access.objectOperand());
    TypeID typeOfObject = object ? walker.typeOf(object) : TypeID();
    if (!typeOfObject)
        return false;
    Type type = walker.types().at(typeOfObject);
    BuiltinID named = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    if (!named || program.builtins().canonicalID(named) != program.builtins().canonicalID(Builtins::GlobalThis))
        return false;
    if (!mayHaveGlobalBindingForReplacement(program, access))
        return false;

    Draft written = walker.draftOf(access);
    if (access.opcode() == Opcode::PropGet) {
        Draft read(Opcode::GetGlobalBinding, written.origin());
        walker.chooseConservativeFields(read);
        read.setKey(key);
        read.setLevel(KnowledgeLevel::Expected);
        read.setSeesScriptBindings(access.isFreeName());
        read.setIsForTypeOf(access.isForTypeOf());
        read.expected = written.expected;
        read.inputValueGiven = access.id();
        walker.bind(access.id(), walker.build(WTF::move(read)));
        return true;
    }

    // A store through a binding replaces the value without a report, so the reads of such a name look at no constant. Plan lists the names that a body stores on the global object as it is written, and no other store is bound.
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID holder = index.valueBehindCasts(access.objectOperand());
    if (index.isParameter(holder) || index.opcodeOf(holder) != Opcode::Builtin || !walker.plan().globalName(key).isStoredThroughBinding)
        return false;
    Draft store(Opcode::SetGlobalBinding, written.origin());
    walker.chooseConservativeFields(store);
    store.setKey(key);
    store.setMode(access.mode());
    store.setValueOperand(walker.referenceOf(access.valueOperand()));
    store.setLevel(KnowledgeLevel::Expected);
    store.setSeesScriptBindings(access.isFreeName());
    store.inputValueGiven = access.id();
    walker.build(WTF::move(store));
    return true;
}

// ---- HasJSType

// `$isJSArray(value)`. Where the engine's type of a cell is how an instance of one built-in class is told (BuiltinInstanceTest::JSTypeEquals), the question is the test of that class.
// It is built as that test: the code is the same, and behind it the value is of the class, so nothing that takes such an instance tests again.
static bool replaceTestOfCellTypeByTestOfClass(Walker& walker, const BodyValue& test)
{
    auto operation = test.as<Opcode::HasJSType>();
    if (operation.first() != operation.last())
        return false;
    const JSType asked = toJSType(operation.first());
    const BuiltinTable& table = walker.program().builtins();
    std::optional<BuiltinClassID> told;
    for (unsigned index = 0; index < table.classCount(); ++index) {
        const BuiltinClass& builtinClass = table.at(BuiltinClassID(index));
        if (builtinClass.instanceTest != BuiltinInstanceTest::JSTypeEquals || builtinClass.type != asked)
            continue;
        if (told)
            return false;
        told = builtinClass.id;
    }
    Reference value = walker.referenceOf(operation.valueOperand());
    if (!told || !value)
        return false;
    TypeArena& types = walker.types();
    TypeID tested = types.testablePartOf(types.builtinClass(*told));
    const ClassPart* part = tested && types.at(tested).isObject() ? singleBuiltinClassOf(types, types.at(tested)) : nullptr;
    if (!part || part->builtinClass != *told)
        return false;

    Draft written(Opcode::TypeTest, walker.originOf(test));
    written.setValueOperand(value);
    written.setTestedType(tested);
    walker.bind(test.id(), walker.build(WTF::move(written)));
    return true;
}

// ---- ArraySpeciesCreate

// In a copy of `filter` or `map` whose call reaches the row of lists alone: what the species of such a list makes is `new Array(length)`. It is built as that, so the result is a list that this code alone has.
static bool replaceSpeciesOfListByNewList(Walker& walker, const BodyValue& created)
{
    auto operation = created.as<Opcode::ArraySpeciesCreate>();
    Reference length = walker.referenceOf(operation.lengthOperand());
    if (!walker.takeWordThatCopyMakesNewList(walker.referenceOf(operation.arrayOperand())) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    if (!length || !walker.types().at(walker.typeOf(length)).isInteger())
        return false;
    Draft list(Opcode::NewArrayLike, walker.originOf(created));
    walker.chooseConservativeFields(list);
    list.setLengthOperand(length);
    list.setArrayStorage(ArrayStorage::Other);
    list.inputValueGiven = created.id();
    walker.bind(created.id(), walker.build(WTF::move(list)));
    walker.count(StatisticID::NewListsInPlaceOfSpecies);
    return true;
}

// ---- The link of the chain

constinit const OpcodeSet opcodesThatReplaceOperationTakes = opcodesOfAllReplacements;

bool replaceOperationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfAllReplacements.contains(opcode))
        return false;
    if (opcode == Opcode::PropGet || opcode == Opcode::PropSet)
        return replaceAccessToGlobalName(walker, value);
    if (opcode == Opcode::HasJSType)
        return replaceTestOfCellTypeByTestOfClass(walker, value);
    if (opcode == Opcode::ArraySpeciesCreate)
        return replaceSpeciesOfListByNewList(walker, value);

    // Any other definition is copied.
    if (opcode == Opcode::DefineMethod)
        return leaveMemberToClassRecord(walker, value);
    if (opcode == Opcode::DefineGetter || opcode == Opcode::DefineSetter)
        return layAccessorsOfLiteralTogether(walker, value) || defineAccessorUnderNameThatKeyIs(walker, value);

    // Each of the three goes by the built-in that is called, which the type of the callee names.
    if (opcode == Opcode::Call && walker.nothingIsProvenOf(walker.referenceOf(value.calleeOperand())))
        return false;
    // Every other call has its form in Code.
    if (opcode == Opcode::Call && (replaceCallDoneInPlace(walker, value) || replaceCallThatMakesSymbolOnce(walker, value)))
        return true;
    if (callsByValue.contains(opcode) || constructionsByValue.contains(opcode))
        return replaceCallOfCompiledBuiltin(walker, value);

    if (opcodesThatStateTypes.contains(opcode)) {
        bindOperandInPlaceOfCastForReplacement(walker, value);
        return true;
    }
    // ModuleEvaluationPlan::isPathNamedBySites() goes by the same question.
    if (opcode == Opcode::RecordRequire)
        return walker.plan().moduleEvaluation().recordsNothingThatIsSeen(value.as<Opcode::RecordRequire>().module());

    bool isReplaced = true;
    if (opcodesOfIterationReplacements.contains(opcode))
        isReplaced = replaceIteration(walker, value);
    else if (opcodesOfCoroutineReplacements.contains(opcode))
        isReplaced = replaceCoroutineOperation(walker, value);
    else if (opcode == Opcode::CreateIteratorResult)
        buildIteratorResultAsReplacement(walker, value);
    else if (opcode == Opcode::StepArrayIterator)
        buildStepOfArrayIteratorAsReplacement(walker, value);
    else if (opcode == Opcode::StepMapIterator)
        buildStepOfCollectionIteratorAsReplacement(walker, value, CollectionSteppedByReplacement::Map);
    else if (opcode == Opcode::StepSetIterator)
        buildStepOfCollectionIteratorAsReplacement(walker, value, CollectionSteppedByReplacement::Set);
    else {
        RELEASE_ASSERT(opcode == Opcode::EvaluateModule);
        buildEvaluationOfModuleAsReplacement(walker, value);
    }

    // Walker::copy() has no form for any of these either.
    if (!isReplaced)
        walker.fail("an operation that has no form in Code was not replaced"_s);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
