/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTAccessPlan.h"
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

OperatorForm operatorFormFor(Walker&, const Draft&);
std::optional<uint32_t> orderWordWithFieldListedNext(const Walker&, LayoutID, PropertyKeyID, uint32_t orderWord);

static_assert(!static_cast<uint32_t>(StepArrayIteratorResult::Done) && static_cast<uint32_t>(StepArrayIteratorResult::Value) == 1);
static_assert(static_cast<uint32_t>(SetNextResult::HasEntry) == static_cast<uint32_t>(MapNextResult::HasEntry) && static_cast<uint32_t>(SetNextResult::Entry) == static_cast<uint32_t>(MapNextResult::Entry) && static_cast<uint32_t>(SetNextResult::Element) == static_cast<uint32_t>(MapNextResult::Key));
static_assert(static_cast<uint32_t>(JSMapIterator::Field::Entry) == static_cast<uint32_t>(JSSetIterator::Field::Entry) && static_cast<uint32_t>(JSMapIterator::Field::IteratedObject) == static_cast<uint32_t>(JSSetIterator::Field::IteratedObject) && static_cast<uint32_t>(JSMapIterator::Field::Kind) == static_cast<uint32_t>(JSSetIterator::Field::Kind));

static constexpr OpcodeSet opcodesOfIterationReplacements { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread };
static constexpr OpcodeSet opcodesOfCoroutineReplacements { Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope };
static constexpr OpcodeSet opcodesOfAllReplacements = opcodesOfIterationReplacements | opcodesOfCoroutineReplacements | opcodesThatStateTypes | OpcodeSet { Opcode::HasJSType, Opcode::IsHolderWithoutAddedNames, Opcode::ArraySpeciesCreate } | OpcodeSet {
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
    bool hasStopped() const { return m_hasStopped; }

    bool goesOnBehindArmThatLeft()
    {
        if (!m_hasStopped || m_walker.hasFailed() || !m_walker.isUnreachable())
            return false;
        m_hasStopped = false;
        return true;
    }

    void stopWhereNothingArrives() { m_hasStopped |= m_walker.isUnreachable(); }

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
        if (m_hasStopped)
            return { };
        Draft operation = draft(opcode);
        m_walker.chooseConservativeFields(operation);
        operation.setLeftOperand(left);
        operation.setRightOperand(right);
        operation.setForm(operatorFormFor(m_walker, operation));
        return buildAsItStands(WTF::move(operation));
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

    Reference narrowedByLess(Reference value, Reference test, bool isLess)
    {
        if (m_hasStopped || !test.isValue() || !value.isRecord())
            return value;
        Draft narrowed = draft(Opcode::Narrowed);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::Less::left);
        narrowed.setIsOnTakenEdge(isLess);
        return build(WTF::move(narrowed));
    }

    Reference boxed(Reference value) { return converted(value, Representation::JSValue); }

    Reference converted(Reference value, Representation representation)
    {
        if (m_hasStopped)
            return { };
        Representation held = m_walker.representationOf(value);
        if (isSameWord(held, representation))
            return value;
        std::optional<ConversionPath> path = findConversion(m_walker.types(), m_walker.typeOf(value), held, representation);
        if (!path) {
            m_walker.fail("what an arm of a replacement gives has no such form"_s);
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

class ArmsOfReplacements {
    WTF_MAKE_NONCOPYABLE(ArmsOfReplacements);
public:
    ArmsOfReplacements(BuilderOfReplacements& builder, Reference condition, uint32_t numberOfResults, bool recordsOutcome = false)
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
        walker.branch(condition, firstArm, m_secondArm, recordsOutcome);
        walker.continueIn(firstArm);
    }

    void endFirstArm(std::initializer_list<Reference> values)
    {
        jumpToJoin(values);
        if (!m_builder.hasStopped())
            m_builder.walker().continueIn(m_secondArm);
    }

    void endSecondArm(std::initializer_list<Reference> values)
    {
        jumpToJoin(values);
        m_builder.stopWhereNothingArrives();
    }

    Reference result(uint32_t index = 0) const { return m_builder.hasStopped() ? Reference() : m_results[index]; }

private:
    void jumpToJoin(std::initializer_list<Reference> values)
    {
        if (m_join && m_builder.goesOnBehindArmThatLeft()) {
            m_builder.walker().jumpTo(m_join, { });
            return;
        }
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

static TypeID typeClaimedByConvertForReplacement(Walker& walker, const DeclaredType& declared)
{
    if (!declared.has(PositionFlags::IsClaim) || declared.isExpected() || !declared.isEstablished())
        return { };
    return targetOfConvert({ walker.program(), walker.types(), PositionTypes::knowingNothing() }, declared);
}

static ALWAYS_INLINE void bindOperandInPlaceOfCastForReplacement(Walker& walker, const BodyValue& cast)
{
    Reference operand = walker.referenceOf(cast.operand(cast.opcode() == Opcode::NarrowByTest ? OperandIndex::NarrowByTest::left : 0));
    if (cast.opcode() == Opcode::Convert) {
        const DeclaredType declared = cast.as<Opcode::Convert>().declared();
        if (declared.has(PositionFlags::IsInt32Promise))
            checkInt32Promise(walker, declared, cast.operand(0), walker.originOf(cast));
        if (declared.has(PositionFlags::IsClaim) && !walker.currentBodyIndex().isUnused(cast.id())) {
            if (TypeID claimed = typeClaimedByConvertForReplacement(walker, declared)) {
                BuilderOfReplacements builder(walker, cast);
                operand = builder.proveOrTrap(operand, claimed);
                if (builder.hasStopped())
                    return;
            }
        }
    }
    walker.bind(cast.id(), operand);
}

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
    walker.loopsOverGenerators().hasStepsWrittenByHand |= !!walker.depthOfCopies();
}

static Reference buildKindTestForReplacement(BuilderOfReplacements& builder, Reference kind, JSC::IterationKind tested)
{
    return builder.applyOperator(Opcode::StrictEq, kind, builder.integer(static_cast<int64_t>(tested)));
}

static Reference buildPairForReplacement(BuilderOfReplacements& builder, Reference key, Reference value)
{
    Draft pair = builder.draft(Opcode::NewArray);
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
    builder.walker().state().invalidate(declaredBoundOf(step.opcode()));
    builder.walker().bind(step.id(), built);
}

static Reference buildTestOfPositionInIteratedObjectForReplacement(BuilderOfReplacements& builder, Reference iterated, Reference position)
{
    Draft test = builder.draft(Opcode::HasJSType);
    test.setValueOperand(iterated);
    test.setFirst(CheckedJSType::Int8ArrayType);
    test.setLast(CheckedJSType::BigUint64ArrayType);
    Reference isView = builder.build(WTF::move(test));

    ArmsOfReplacements arms(builder, isView, 1);
    {
        Draft length = builder.draft(Opcode::GetTypedArrayLength);
        length.setViewOperand(builder.proveOrTrap(iterated, TypeTable::object));
        length.setTypedArrayType(NotTypedArray);
        ArmsOfReplacements byLengthInLine(builder, builder.applyOperator(Opcode::Less, position, builder.build(WTF::move(length))), 1);
        byLengthInLine.endFirstArm({ builder.boolean(true) });
        Draft function = builder.draft(Opcode::Builtin);
        function.setBuiltin(Builtins::LinkTimeConstantTypedArrayLength);
        function.setIsNotReadFromHolder(true);
        Draft call = builder.draft(Opcode::Call);
        call.setCalleeOperand(builder.build(WTF::move(function)));
        call.setArgumentsOperands(std::array { iterated });
        byLengthInLine.endSecondArm({ builder.applyOperator(Opcode::Less, position, builder.build(WTF::move(call))) });
        arms.endFirstArm({ byLengthInLine.result() });
    }
    {
        Draft read = builder.draft(Opcode::PropGet);
        read.setObjectOperand(iterated);
        read.setKey(builder.walker().program().keyOf(BuiltinNames::length));
        Draft length = builder.draft(Opcode::ToLength);
        length.setValueOperand(builder.build(WTF::move(read)));
        arms.endSecondArm({ builder.applyOperator(Opcode::Less, position, builder.build(WTF::move(length))) });
    }
    return arms.result();
}

static const Operation* findMakerOfIteratorForReplacement(const Code& code, Reference iterator, InlineBuiltin maker)
{
    while (iterator && iterator.isValue()) {
        const Operation& operation = code.at(iterator.id());
        if (operation.opcode == Opcode::CallInline)
            return operation.inlineBuiltin() == maker && operation.receiverOperand() ? &operation : nullptr;
        if (!cellPreservingOpcodes.contains(operation.opcode))
            return nullptr;
        iterator = Reference::value(operation.operand(0));
    }
    return nullptr;
}

static Reference buildValueAtPositionOfArrayIteratorForReplacement(BuilderOfReplacements& builder, Reference iterator, Reference iterated, Reference position, bool walksList = false, const std::optional<PassedTest>& insideLength = std::nullopt)
{
    auto readElement = [&] {
        Draft read = builder.draft(walksList ? Opcode::GetArrayElement : Opcode::ElemGet);
        if (walksList) {
            read.setArrayOperand(iterated);
            read.setPositionOperand(position);
            read.setArrayStorage(ArrayStorage::Other);
            read.setLevel(KnowledgeLevel::Expected);
            read.setKeyIsIntegral(true);
            if (insideLength)
                read.setIsInBounds(*insideLength);
        } else {
            read.setObjectOperand(iterated);
            read.setPropertyOperand(position);
        }
        return builder.build(WTF::move(read));
    };
    if (const Operation* maker = findMakerOfIteratorForReplacement(builder.walker().code(), iterator, InlineBuiltin::ArrayIterator)) {
        switch (static_cast<JSC::IterationKind>(builder.walker().program().builtins().at(maker->overload()).code.implementation.inlineParameters[0])) {
        case JSC::IterationKind::Keys:
            return position;
        case JSC::IterationKind::Values:
            return readElement();
        case JSC::IterationKind::Entries:
            return buildPairForReplacement(builder, position, readElement());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    Reference kind = builder.proveOrTrap(builder.getInternalField(iterator, JSArrayIterator::Field::Kind), TypeTable::int32);
    ArmsOfReplacements byKeys(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Keys), 1);
    byKeys.endFirstArm({ position });

    Reference element = readElement();
    ArmsOfReplacements byEntries(builder, buildKindTestForReplacement(builder, kind, JSC::IterationKind::Entries), 1);
    byEntries.endFirstArm({ buildPairForReplacement(builder, position, element) });
    byEntries.endSecondArm({ element });

    byKeys.endSecondArm({ byEntries.result() });
    return byKeys.result();
}

static bool branchesOnDone(Walker& walker, const BodyValue& step)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyTerminator end = index.terminator(index.blockOf(step.id()));
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
    while (condition && !index.isParameter(condition) && index.opcodeOf(condition) == Opcode::ToBoolean)
        condition = index.value(condition).operand(0);
    if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
        return false;
    auto asked = index.value(condition).as<Opcode::ResultAt>();
    return asked.resultsOperand() == step.id() && asked.index() == static_cast<uint32_t>(StepArrayIteratorResult::Done);
}

static void buildThreadedArrayStep(BuilderOfReplacements& builder, const BodyValue& step, Reference iterator, Reference list, Reference index, Reference isFinished)
{
    Walker& walker = builder.walker();
    BlockID finished = walker.addBlock();
    BlockID unfinished = walker.addBlock();
    BlockID inside = walker.addBlock();
    BlockID outside = walker.addBlock();
    auto endWith = [&](Reference done, Reference value) {
        if (builder.goesOnBehindArmThatLeft() || builder.hasStopped())
            return;
        bindStepForReplacement(builder, step, done, value);
        if (!builder.hasStopped() && !walker.threadBranchAfter(step))
            walker.fail("a way of a step did not walk the end of its block"_s);
    };
    walker.branchOnTest(isFinished, finished, unfinished);
    walker.continueIn(finished);
    endWith(builder.boolean(true), builder.undefined());

    walker.continueIn(unfinished);
    Reference position = builder.narrowedByLess(index, isFinished, false);
    Draft read = builder.draft(Opcode::GetArrayLength);
    read.setArrayOperand(list);
    Reference length = builder.build(WTF::move(read));
    Reference isInside = builder.applyOperator(Opcode::Less, position, length);
    if (builder.hasStopped())
        return;
    walker.branchOnTest(isInside, inside, outside);
    walker.continueIn(outside);
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.integer(JSArrayIterator::doneIndex));
    endWith(builder.boolean(true), builder.undefined());

    walker.continueIn(inside);
    Reference narrowed = builder.narrowedByLess(position, isInside, true);
    std::optional<PassedTest> insideLength = builder.hasStopped() ? std::nullopt : walker.boundsProofAfterComparison(list, narrowed, length, isInside);
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.applyOperator(Opcode::Add, narrowed, builder.integer(1)));
    Reference value = buildValueAtPositionOfArrayIteratorForReplacement(builder, iterator, list, narrowed, true, insideLength);
    endWith(builder.boolean(false), value);
}

static void buildStepOfArrayIteratorAsReplacement(Walker& walker, const BodyValue& step)
{
    BuilderOfReplacements builder(walker, step);
    Reference iterator = builder.proveOrTrap(walker.referenceOf(step.as<Opcode::StepArrayIterator>().iteratorOperand()), TypeTable::object);
    Reference index = builder.converted(builder.proveOrTrap(builder.getInternalField(iterator, JSArrayIterator::Field::Index), TypeTable::integer), Representation::Int64);

    static_assert(JSArrayIterator::doneIndex == -1);
    Reference isFinished = builder.applyOperator(Opcode::Less, index, builder.integer(0));
    TypeID lists = walker.types().builtinClass(BuiltinClasses::Array);
    const Operation* maker = findMakerOfIteratorForReplacement(walker.code(), iterator, InlineBuiltin::ArrayIterator);
    Reference walked = maker ? Reference::value(maker->receiverOperand()) : Reference();
    bool walksKnownList = walked && walker.typeOf(walked) && walker.types().isSubset(walker.typeOf(walked), lists);
    if (!builder.hasStopped() && walksKnownList && walker.has(Mechanism::Folding) && isFinished.isValue() && branchesOnDone(walker, step) && walker.canThreadBranchAfter(step)) {
        buildThreadedArrayStep(builder, step, iterator, walked, index, isFinished);
        return;
    }
    ArmsOfReplacements byFinished(builder, isFinished, 2);
    byFinished.endFirstArm({ builder.boolean(true), builder.undefined() });
    index = builder.narrowedByLess(index, isFinished, false);

    auto stepOverList = [&](Reference list) -> std::pair<Reference, Reference> {
        Draft read = builder.draft(Opcode::GetArrayLength);
        read.setArrayOperand(list);
        Reference length = builder.build(WTF::move(read));
        Reference isInside = builder.applyOperator(Opcode::Less, index, length);
        ArmsOfReplacements byRange(builder, isInside, 2, true);
        Reference inside = builder.narrowedByLess(index, isInside, true);
        std::optional<PassedTest> insideLength = builder.hasStopped() ? std::nullopt : walker.boundsProofAfterComparison(list, inside, length, isInside);
        builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.applyOperator(Opcode::Add, inside, builder.integer(1)));
        byRange.endFirstArm({ builder.boolean(false), buildValueAtPositionOfArrayIteratorForReplacement(builder, iterator, list, inside, true, insideLength) });
        builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.integer(JSArrayIterator::doneIndex));
        byRange.endSecondArm({ builder.boolean(true), builder.undefined() });
        return { byRange.result(0), byRange.result(1) };
    };
    if (walksKnownList) {
        auto [done, value] = stepOverList(walked);
        byFinished.endSecondArm({ done, value });
        bindStepForReplacement(builder, step, byFinished.result(0), byFinished.result(1));
        return;
    }
    Reference iterated = builder.getInternalField(iterator, JSArrayIterator::Field::IteratedObject);
    Draft test = builder.draft(Opcode::TypeTest);
    test.setValueOperand(iterated);
    test.setTestedType(lists);
    Reference isList = builder.build(WTF::move(test));
    ArmsOfReplacements byList(builder, isList, 2);
    {
        Draft narrowed = builder.draft(Opcode::Narrowed);
        narrowed.setValueOperand(iterated);
        narrowed.setTestOperand(isList);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        auto [done, value] = stepOverList(builder.build(WTF::move(narrowed)));
        byList.endFirstArm({ done, value });
    }
    ArmsOfReplacements byRange(builder, buildTestOfPositionInIteratedObjectForReplacement(builder, iterated, index), 2);
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.applyOperator(Opcode::Add, index, builder.integer(1)));
    byRange.endFirstArm({ builder.boolean(false), buildValueAtPositionOfArrayIteratorForReplacement(builder, iterator, iterated, index) });
    builder.putInternalField(iterator, JSArrayIterator::Field::Index, builder.integer(JSArrayIterator::doneIndex));
    byRange.endSecondArm({ builder.boolean(true), builder.undefined() });
    byList.endSecondArm({ byRange.result(0), byRange.result(1) });

    byFinished.endSecondArm({ byList.result(0), byList.result(1) });
    bindStepForReplacement(builder, step, byFinished.result(0), byFinished.result(1));
}

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

static Reference buildValueOfFoundEntryForReplacement(BuilderOfReplacements& builder, CollectionSteppedByReplacement kindOfCollection, Reference iterator, Reference table, Reference found, std::optional<JSC::IterationKind> kind)
{
    Reference entry = builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Entry));
    Reference key = builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Key));
    Reference value = kindOfCollection == CollectionSteppedByReplacement::Map ? builder.resultAt(found, static_cast<uint32_t>(MapNextResult::Value)) : key;
    builder.putInternalField(iterator, JSSetIterator::Field::Entry, builder.applyOperator(Opcode::Add, entry, builder.integer(1)));

    putTableOfCollectionIteratorForReplacement(builder, iterator, table);

    if (kind) {
        switch (*kind) {
        case JSC::IterationKind::Keys:
            return key;
        case JSC::IterationKind::Values:
            return value;
        case JSC::IterationKind::Entries:
            return buildPairForReplacement(builder, key, value);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return buildValueOfCollectionIteratorKindForReplacement(builder, builder.getInternalField(iterator, JSSetIterator::Field::Kind), key, value);
}

static void buildStepOfCollectionIteratorAsReplacement(Walker& walker, const BodyValue& step, CollectionSteppedByReplacement kindOfCollection)
{
    BuilderOfReplacements builder(walker, step);
    Reference iterator = builder.proveOrTrap(walker.referenceOf(step.operand(0)), TypeTable::object);
    if (kindOfCollection == CollectionSteppedByReplacement::Set && !findMakerOfIteratorForReplacement(walker.code(), iterator, InlineBuiltin::SetIterator)) {
        Draft next = builder.draft(Opcode::NextSetElement);
        next.setIteratorOperand(iterator);
        Reference element = builder.build(WTF::move(next));
        Draft isMark = builder.draft(Opcode::IsCollectionIteratorClosedMark);
        isMark.setTableOperand(element);
        ArmsOfReplacements byMark(builder, builder.build(WTF::move(isMark)), 2);
        byMark.endFirstArm({ builder.boolean(true), builder.undefined() });
        byMark.endSecondArm({ builder.boolean(false), buildValueOfCollectionIteratorKindForReplacement(builder, builder.getInternalField(iterator, JSSetIterator::Field::Kind), element, element) });
        bindStepForReplacement(builder, step, byMark.result(0), byMark.result(1));
        return;
    }

    Draft read = builder.draft(Opcode::GetCollectionIteratorTable);
    read.setObjectOperand(iterator);
    Reference table = builder.build(WTF::move(read));
    Draft test = builder.draft(Opcode::IsCollectionIteratorClosedMark);
    test.setTableOperand(table);

    ArmsOfReplacements byFinished(builder, builder.build(WTF::move(test)), 2);
    byFinished.endFirstArm({ builder.boolean(true), builder.undefined() });

    TypeID collections = walker.types().builtinClass(classOfCollectionSteppedByReplacement(kindOfCollection));
    Reference collection;
    std::optional<JSC::IterationKind> kind;
    if (const Operation* maker = findMakerOfIteratorForReplacement(walker.code(), iterator, kindOfCollection == CollectionSteppedByReplacement::Map ? InlineBuiltin::MapIterator : InlineBuiltin::SetIterator)) {
        kind = static_cast<JSC::IterationKind>(walker.program().builtins().at(maker->overload()).code.implementation.inlineParameters[0]);
        ValueID iterated = maker->receiverOperand();
        collection = walker.code().isConstant(iterated) ? Reference::constant(iterated) : Reference::value(iterated);
    }
    if (!collection || !walker.typeOf(collection) || !walker.types().isSubset(walker.typeOf(collection), collections))
        collection = builder.proveOrTrap(builder.getInternalField(iterator, JSSetIterator::Field::IteratedObject), collections);
    Reference entry = builder.proveOrTrap(builder.getInternalField(iterator, JSSetIterator::Field::Entry), TypeTable::integer);
    Reference found = buildNextEntryForReplacement(builder, kindOfCollection, collection, table, entry);
    Draft current = builder.draft(Opcode::CollectionTable);
    current.setCollectionOperand(collection);
    Reference tableBehindStep = builder.build(WTF::move(current));
    ArmsOfReplacements byEntry(builder, builder.resultAt(found, static_cast<uint32_t>(MapNextResult::HasEntry)), 2);
    byEntry.endFirstArm({ builder.boolean(false), buildValueOfFoundEntryForReplacement(builder, kindOfCollection, iterator, tableBehindStep, found, kind) });
    putTableOfCollectionIteratorForReplacement(builder, iterator, builder.build(builder.draft(Opcode::CollectionIteratorClosedMark)));
    byEntry.endSecondArm({ builder.boolean(true), builder.undefined() });

    byFinished.endSecondArm({ byEntry.result(0), byEntry.result(1) });
    bindStepForReplacement(builder, step, byFinished.result(0), byFinished.result(1));
}

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
        RELEASE_ASSERT(step.moduleThatHasRun);
        buildStatusOperationOfModuleAsReplacement(builder, Opcode::BeginModuleEvaluation, step.moduleThatHasRun);
        buildStatusOperationOfModuleAsReplacement(builder, Opcode::FinishModuleEvaluation, step.moduleThatHasRun);
    }
}

static void buildEvaluationOfModuleAsReplacement(Walker& walker, const BodyValue& evaluation)
{
    auto written = evaluation.as<Opcode::EvaluateModule>();
    const Module& target = walker.program().at(written.module());

    BuilderOfReplacements builder(walker, evaluation);
    ModuleSitePlan site = walker.plan().moduleEvaluation().siteOf(walker.currentFunction(), evaluation.id(), written.module());
    if (!site.asksLoader) {
        buildModuleStepsAsReplacement(builder, site.steps, written.isCallOfRequire() ? hostRequireFunctionFor(target) : BuiltinID());
        return;
    }
    Draft request = builder.draft(Opcode::EvaluateRequiredModule);
    request.setModule(written.module());
    request.setReferrer(written.referrer());
    request.setSpecifier(target.path);
    if (written.isCallOfRequire())
        request.setCalledBuiltin(hostRequireFunctionFor(target));
    builder.build(WTF::move(request));
}

PropertyKeyID keyOfSymbolMadeOnce(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::Call)
        return { };
    const BodyIndex& body = walker.currentBodyIndex();
    ValueID callee = value.calleeOperand();
    while (callee && !body.isParameter(callee) && opcodesThatStateTypes.contains(body.opcodeOf(callee)))
        callee = body.value(callee).operand(0);
    if (!callee || body.isParameter(callee) || body.opcodeOf(callee) != Opcode::Builtin)
        return { };
    BuiltinID called = walker.program().builtins().canonicalID(body.value(callee).as<Opcode::Builtin>().builtin());
    if (called != Builtins::Symbol && called != Builtins::SymbolFor)
        return { };
    std::span<const SummaryKeyAtSite> symbols = walker.facts().summaries().at(walker.currentFunction()).symbolsMadeOnce.span();
    auto made = std::ranges::lower_bound(symbols, value.id(), { }, &SummaryKeyAtSite::site);
    return made != symbols.end() && made->site == value.id() ? made->key : PropertyKeyID();
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

static constexpr uint32_t valuesInRowOfAccessors = 32;
static_assert(valuesInRowOfAccessors <= 8 * sizeof(uint32_t));

static bool createAccessorsOfLiteralInSlots(Walker& walker, Reference object, std::span<const PropertyKeyID> keys, std::span<const Reference> functions, uint32_t getterMask, Origin origin)
{
    struct Accessor {
        PropertyKeyID key;
        Reference name;
        Reference getter;
        Reference setter;
        OptionSet<PropertyDescriptorFlags> flags;
    };
    Vector<Accessor, valuesInRowOfAccessors> accessors;
    for (uint32_t index = 0; index < keys.size(); ++index) {
        size_t place = accessors.findIf([&](const Accessor& accessor) {
            return accessor.key == keys[index];
        });
        if (place == notFound) {
            place = accessors.size();
            accessors.append({ keys[index], { }, { }, { }, { PropertyDescriptorFlags::HasEnumerable, PropertyDescriptorFlags::IsEnumerable, PropertyDescriptorFlags::HasConfigurable, PropertyDescriptorFlags::IsConfigurable } });
        }
        bool isGetter = getterMask >> index & 1;
        Reference& half = isGetter ? accessors[place].getter : accessors[place].setter;
        if (half)
            return false;
        half = functions[index];
        accessors[place].flags.add(isGetter ? PropertyDescriptorFlags::HasGet : PropertyDescriptorFlags::HasSet);
    }

    AccessContext context { walker.program(), WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() };
    for (Accessor& accessor : accessors) {
        const PropertyKey& key = walker.program().at(accessor.key);
        if (key.kind != PropertyKeyKind::Text)
            return false;
        Draft name(Opcode::String, origin);
        name.setString(key.text);
        accessor.name = walker.constant(WTF::move(name));
        auto typeOfHalf = [&](Reference half) {
            return half ? walker.typeOf(half) : TypeID();
        };
        if (!fieldCreatedInSlotBy(context, { walker.typeOf(object), walker.typeOf(accessor.name), TypeID(), typeOfHalf(accessor.getter), typeOfHalf(accessor.setter), accessor.flags }))
            return false;
    }
    LayoutID layout = singleLayoutOf(walker.types(), walker.types().at(walker.typeOf(object)));
    std::optional<uint32_t> orderWord = walker.state().isFresh(object) ? walker.state().orderWordOf(object) : std::nullopt;
    for (const Accessor& accessor : accessors) {
        if (orderWord)
            orderWord = orderWordWithFieldListedNext(walker, layout, accessor.key, *orderWord);
        Draft definition(Opcode::DefineOwnProperty, origin);
        walker.chooseConservativeFields(definition);
        definition.setObjectOperand(object);
        definition.setComputedKeyOperand(accessor.name);
        if (accessor.getter)
            definition.setGetterOperand(accessor.getter);
        if (accessor.setter)
            definition.setSetterOperand(accessor.setter);
        definition.setDescriptorFlags(accessor.flags);
        if (orderWord)
            definition.setOrderWordAfter(OrderWord(*orderWord));
        walker.build(WTF::move(definition));
        walker.state().noteOrderWord(object, orderWord);
    }
    return true;
}

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
    auto neighbor = [&](ValueID from, int32_t step) -> ValueID {
        for (uint32_t index = from.index() + step; index >= block.firstValue().index() && index < block.endOfValues().index() && index / valuesInRowOfAccessors == row; index += step) {
            BodyValue other = body.value(ValueID(index));
            if (laysAccessorThere(other))
                return other.id();
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
    if (createAccessorsOfLiteralInSlots(walker, walker.referenceOf(object), keys.span(), functions.span(), getterMask, walker.originOf(value)))
        return true;
    if (group.size() == 1)
        return false;
    Draft accessors(Opcode::DefineAccessors, walker.originOf(value));
    accessors.setObjectOperand(walker.referenceOf(object));
    accessors.setKeys(keys.span());
    accessors.setGetterMask(getterMask);
    accessors.setEnumerableMask(group.size() == 32 ? UINT32_MAX : (1u << group.size()) - 1);
    accessors.setFunctionsOperands(functions.span());
    walker.build(WTF::move(accessors));
    return true;
}

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

static bool isSymbolInReplacements(const Program& program, PropertyKeyID key)
{
    PropertyKeyKind kind = program.at(key).kind;
    return kind == PropertyKeyKind::WellKnownSymbol || kind == PropertyKeyKind::RegisteredSymbol || kind == PropertyKeyKind::CreatedSymbol;
}

static bool walkHasDerivedKeyOfAccess(const Walker& walker, const BodyValue& access, PropertyKeyID expected)
{
    if (!isSymbolInReplacements(walker.program(), expected))
        return true;
    TypeArena& types = walker.types();
    TypeID symbol = types.withPrimitives(types.symbolLiteral(expected), Primitive::Absent);
    ValueID operand = access.propertyOperand();
    if (Reference built = walker.referenceOf(operand))
        return types.isSubset(walker.typeOf(built), symbol);

    const BodyIndex& index = walker.currentBodyIndex();
    while (!index.isParameter(operand) && index.opcodeOf(operand) == Opcode::CheckInitialized)
        operand = index.value(operand).operand(0);
    if (index.isParameter(operand))
        return false;
    BodyValue definition = index.value(operand);
    TypeID read;
    if (definition.opcode() == Opcode::GetModuleVariable)
        read = walker.positions().variable(TypeLevel::Proven, definition.as<Opcode::GetModuleVariable>().variable());
    else if (definition.opcode() == Opcode::GetEnvironmentField)
        read = walker.positions().environmentField(TypeLevel::Proven, definition.as<Opcode::GetEnvironmentField>().layout(), definition.as<Opcode::GetEnvironmentField>().field());
    return read && types.isSubset(read, symbol);
}

PropertyKeyID keyThatAccessGoesBy(const Walker& walker, const BodyValue& access, PropertyKeyID* chosenAndNotDerived)
{
    if (!accessesUnderComputedKeys.contains(access.opcode()))
        return { };
    if (access.opcode() == Opcode::ElemDefine && access.as<Opcode::ElemDefine>().calledBuiltin())
        return { };
    if (walker.has(Mechanism::TypedOperations)) {
        if (PropertyKeyID chosen = walker.facts().linked().keyAt(walker.currentFunction(), access.id())) {
            if (walkHasDerivedKeyOfAccess(walker, access, chosen))
                return chosen;
            if (chosenAndNotDerived)
                *chosenAndNotDerived = chosen;
            return { };
        }
        return keyOfKnownTextThatAccessGoesBy(walker, access);
    }
    if (walker.has(Mechanism::DirectCalls)) {
        PropertyKeyID chosen = walker.facts().linked().keyAt(walker.currentFunction(), access.id());
        return chosen && isSymbolInReplacements(walker.program(), chosen) && walkHasDerivedKeyOfAccess(walker, access, chosen) ? chosen : PropertyKeyID();
    }

    Reference built = walker.referenceBoundTo(access.propertyOperand());
    if (!built || !built.isConstant() || walker.code().at(built.id()).opcode != Opcode::KeyConstant)
        return { };
    const Program& program = walker.program();
    PropertyKeyID key = walker.code().at(built.id()).key();
    bool isSymbol = program.at(key).kind == PropertyKeyKind::CreatedSymbol;
    return isSymbol && program.types.hasStableNumber(key) && !isSymbolOfLibrary(program, key) ? key : PropertyKeyID();
}

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

BodyValue Walker::accessAsWalked(const BodyValue& written, PropertyKeyID* chosenAndNotDerived)
{
    PropertyKeyID key = keyThatAccessGoesBy(*this, written, chosenAndNotDerived);
    return key ? accessByName(written, key) : written;
}

BodyValue Walker::accessByName(const BodyValue& written, PropertyKeyID key)
{
    uint64_t asked = static_cast<uint64_t>(written.id().index() + 1) << 32 | key.bits();
    return m_frames.last().accessesUnderProvenKeys.ensure(asked, [&] {
        return makeUnique<AccessUnderProvenKey>(written, key);
    }).iterator->value->value();
}

Walker::KeyOfAccess Walker::compareKeyOfAccessWithSymbol(const BodyValue& written, PropertyKeyID chosen)
{
    bool comesAgain = m_accessWhoseKeyIsTested == written.id() && m_depthOfCopiesAroundThatAccess == depthOfCopies();
    if (comesAgain && !m_opensGuesses)
        return KeyOfAccess::IsAnyValue;

    Reference key = referenceOf(written.propertyOperand());
    if (!key)
        return KeyOfAccess::IsAnyValue;
    TypeID symbol = m_types.symbolLiteral(chosen);
    if (!comesAgain && m_opensGuesses && has(Mechanism::Guesses) && key.isValue() && m_types.overlap(typeOf(key), symbol)) {
        const BodyIndex& index = currentBodyIndex();
        BlockID inputBlock = index.blockOf(written.id());
        uint32_t place = written.id().index() - index.block(inputBlock).firstValue().index();
        ValueID accessBefore = std::exchange(m_accessWhoseKeyIsTested, written.id());
        size_t depthBefore = std::exchange(m_depthOfCopiesAroundThatAccess, depthOfCopies());
        openGuess(*this, key, symbol, inputBlock, place, place + 1);
        m_accessWhoseKeyIsTested = accessBefore;
        m_depthOfCopiesAroundThatAccess = depthBefore;
        return KeyOfAccess::ArmsHaveWalkedAccess;
    }
    stopUnlessValueIsOf(key, symbol, ProofSource::OtherOperation);
    return KeyOfAccess::IsSymbol;
}

bool Walker::armHoldsKeyOf(ValueID access) const
{
    if (!m_accessWhoseKeyArmHolds || m_depthOfCopiesAroundKeyHeldByArm != depthOfCopies())
        return false;
    if (m_accessWhoseKeyArmHolds == access)
        return true;
    const BodyIndex& index = currentBodyIndex();
    BlockID block = index.blockOf(m_accessWhoseKeyArmHolds);
    if (!index.isInBlock(access, block) || access.index() < m_accessWhoseKeyArmHolds.index() || access.index() - index.block(block).firstValue().index() >= m_endOfKeyHeldByArm)
        return false;
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet };
    return accesses.contains(index.opcodeOf(access)) && index.stripCasts(index.value(access).propertyOperand()) == index.stripCasts(index.value(m_accessWhoseKeyArmHolds).propertyOperand());
}

Walker::KeyOfAccess Walker::tellKeyOfAccessApartByItsTexts(const BodyValue& written)
{
    if (!has(Mechanism::TypedOperations) || written.opcode() == Opcode::ElemDefine || isUnreachable() || armHoldsKeyOf(written.id()))
        return KeyOfAccess::IsAnyValue;
    Reference key = referenceOf(written.propertyOperand());
    TypeID proven = key ? typeOf(key) : TypeID();
    if (!proven)
        return KeyOfAccess::IsAnyValue;
    auto holdsWrittenTextsAlone = [&](const Type& asked) {
        return asked.isJavaScriptValue() && !asked.hasObjects() && asked.primitives == Primitive::LiteralString && asked.stringLiterals;
    };
    Type type = m_types.at(proven);
    if (!(type.primitives & Primitive::AnyString))
        return KeyOfAccess::IsAnyValue;
    if (!m_versionedLoops.isEmpty() && canAccessAtKeyedAddress(*this, written))
        return KeyOfAccess::IsAnyValue;
    bool looksUpCallee = canRestartIteration() && canBuildIterationGuard() && canLookUpCallee(*this, written);
    if (looksUpCallee && !holdsWrittenTextsAlone(type) && !isStaticMethodCallee(*this, written) && isInvariantInSpecializedVersion(key, IterationPurity::KnownEffectsOnly) && canHoistKeyedLookup(Opcode::GetValueUnderKeyOrAbsent))
        return KeyOfAccess::IsAnyValue;
    auto isHolderOfLayouts = [&] {
        auto saysLayouts = [&](TypeID said) {
            TypeID objects = said ? m_types.withoutNullish(said) : TypeID();
            return objects && !isNeverType(objects) && m_types.at(objects).hasObjects() && !m_types.at(objects).hasUnboundedUnknown();
        };
        Reference holder = referenceOf(written.objectOperand());
        return holder && (saysLayouts(typeOf(holder)) || saysLayouts(findExpectation(*this, written.objectOperand()).type));
    };
    bool textsAreOnlyExpected = false;
    bool holderIsOfLayouts = false;
    if (!holdsWrittenTextsAlone(type) && (written.opcode() == Opcode::ElemGet || written.opcode() == Opcode::ElemSet) && isInRepeatedCode() && m_opensGuesses && has(Mechanism::Guesses)) {
        TypeID expected = findExpectation(*this, written.propertyOperand()).type;
        TypeID withoutNothing = expected && canRestartIteration() ? m_types.withoutPrimitives(expected, Primitive::UndefinedOrNull | Primitive::Absent) : expected ? m_types.withoutNullish(expected) : TypeID();
        if (!expected || withoutNothing == TypeID(TypeTable::string) || (looksUpCallee && !holdsWrittenTextsAlone(m_types.at(m_types.withoutNullish(expected)))))
            expected = textsThatNamesOfHolderSayOfKey(*this, written, looksUpCallee);
        TypeID present = expected ? m_types.withoutNullish(expected) : TypeID();
        holderIsOfLayouts = isHolderOfLayouts();
        const BodyIndex& body = currentBodyIndex();
        ValueID definition = body.stripCasts(written.propertyOperand());
        bool isKeyOfForIn = definition && !body.isParameter(definition) && body.opcodeOf(definition) == Opcode::ResultAt && !body.isParameter(body.value(definition).operand(0)) && body.opcodeOf(body.value(definition).operand(0)) == Opcode::ForInStep;
        if ((holderIsOfLayouts || looksUpCallee) && !isKeyOfForIn && present && !isNeverType(present) && holdsWrittenTextsAlone(m_types.at(present)) && m_types.isSubset(present, proven)) {
            type = m_types.at(present);
            textsAreOnlyExpected = true;
        }
    }
    if (!holdsWrittenTextsAlone(type))
        return KeyOfAccess::IsAnyValue;
    const BodyIndex& index = currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(written.id()));
    uint32_t begin = written.id().index() - block.firstValue().index();
    uint32_t limit = limitOfStretchOfArms(*this, block);
    ValueID user = written.opcode() == Opcode::ElemGet ? index.onlyUsingValue(written.id()) : ValueID();
    uint32_t placeOfUser = user && block.holds(user) ? user.index() - block.firstValue().index() : limit;
    bool isCalled = placeOfUser < limit && index.opcodeOf(user) == Opcode::Call && index.value(user).as<Opcode::Call>().calleeOperand() == written.id();
    for (uint32_t place = begin + 1; isCalled && place < placeOfUser; ++place)
        isCalled = !opcodesThatCallDirectlyTakes.contains(index.opcodeOf(ValueID(block.firstValue().index() + place)));
    size_t mostTexts = isCalled ? maxDistinguishedCallees : textsAreOnlyExpected && isKeyPassedIntoLoop(*this, written) ? mostTextsOfPassedKey : mostTextsToldApart;
    if (uint32_t numberOfTexts = m_types.at(type.stringLiterals).size(); numberOfTexts < (textsAreOnlyExpected ? 1 : 2) || numberOfTexts > mostTexts)
        return KeyOfAccess::IsAnyValue;
    Vector<StringID, mostTextsToldApart> texts;
    bool everyTextIsName = true;
    m_types.at(type.stringLiterals).forEach([&](StringID text) {
        everyTextIsName = everyTextIsName && texts.size() < mostTexts && nameOfTextThatAccessMayGoBy(*this, written, text);
        if (everyTextIsName)
            texts.append(text);
    });
    if (!everyTextIsName || texts.isEmpty())
        return KeyOfAccess::IsAnyValue;
    if (texts.size() == 1 && !textsAreOnlyExpected)
        return KeyOfAccess::IsAnyValue;

    uint32_t end = isCalled ? placeOfUser + 1 : begin + 1;
    if (textsAreOnlyExpected) {
        ValueID rootOfKey = index.stripCasts(written.propertyOperand());
        bool hasLaterUse = false;
        for (ValueID name = written.propertyOperand(); !hasLaterUse; name = index.value(name).operand(0)) {
            hasLaterUse = !index.hasOneUse(name);
            if (name == rootOfKey)
                break;
        }
        for (uint32_t place = end; hasLaterUse && place < limit; ++place) {
            BodyValue later = index.value(ValueID(block.firstValue().index() + place));
            if ((later.opcode() == Opcode::ElemGet || later.opcode() == Opcode::ElemSet) && index.stripCasts(later.propertyOperand()) == rootOfKey)
                end = place + 1;
        }
    }
    if (!key.isValue() || !has(Mechanism::Guesses) || !m_opensGuesses || !isInRepeatedCode() || end > endOfStretchBeingWalked())
        return KeyOfAccess::IsAnyValue;

    auto typeOfInput = [&](ValueID input) {
        return typeOf(referenceOf(input));
    };
    for (StringID text : texts) {
        if (!textsAreOnlyExpected && typedFormOf(*this, accessByName(written, nameOfTextThatAccessMayGoBy(*this, written, text)), TypesOfInputs(typeOfInput)) == TypedForm::None && !isHolderOfLayouts())
            return KeyOfAccess::IsAnyValue;
    }

    Origin origin = originOf(written);
    if (textsAreOnlyExpected && !holderIsOfLayouts) {
        guardExpectedType(*this, written.objectOperand(), expectedReceiverType(*this, written), origin);
        if (isUnreachable() || hasFailed())
            return KeyOfAccess::ArmsHaveWalkedAccess;
    }
    StretchInArms stretch = stretchInArms(block.id(), begin, end);
    ArmsThatMeet built;
    ValueID nameOfKey = written.propertyOperand();
    Reference boundBefore = referenceBoundTo(nameOfKey);
    ValueID definitionOfKey = index.stripCasts(nameOfKey);
    Reference definitionBoundBefore = referenceBoundTo(definitionOfKey);
    auto walkArmWith = [&](Reference narrowed) {
        SetForScope accessWhoseKeyArmHolds(m_accessWhoseKeyArmHolds, written.id());
        SetForScope endOfKeyHeldByArm(m_endOfKeyHeldByArm, textsAreOnlyExpected ? end : 0u);
        SetForScope depthOfThatArm(m_depthOfCopiesAroundKeyHeldByArm, depthOfCopies());
        rememberedExpectationOf(written.id()) = { };
        bind(definitionOfKey, narrowed);
        bind(nameOfKey, narrowed);
        bind(written.id(), Reference());
        walkStretch(block.id(), begin, end);
        listArm(stretch, built);
        bind(definitionOfKey, definitionBoundBefore);
        bind(nameOfKey, boundBefore);
    };
    auto narrowedBy = [&](Reference value, Reference test, bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::StringEqualsLiteral::string);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        return build(WTF::move(narrowed));
    };

    BlockID meeting = addBlock();
    recordSpeculation();
    count(StatisticID::AccessesDispatchedByKeyString);
    Reference left = key;
    static_assert(OperandIndex::StringEqualsLiteral::string == OperandIndex::ValueEqualsLiteral::value);
    size_t numberOfTests = textsAreOnlyExpected ? texts.size() : texts.size() - 1;
    bool comparesInFront = isInvariantInSpecializedVersion(key) && tryHoistEntryTest();
    if (textsAreOnlyExpected && !comparesInFront && texts.size() >= 2) {
        Vector<BlockID, mostTextsToldApart> underNames;
        for (size_t place = 0; place < texts.size(); ++place)
            underNames.append(addBlock());
        BlockID underNoName = addBlock(BlockFrequency::Rare);
        TerminatorDraft overNames(TerminatorKind::SwitchString, origin);
        overNames.setSubjectOperand(key);
        overNames.setStrings(texts.span());
        endWithSwitchOverOwnBlocks(overNames, underNames.span(), underNoName);
        for (size_t place = 0; place < texts.size() && !hasFailed(); ++place) {
            continueIn(underNames[place]);
            if (!isUnreachable())
                walkArmWith(constant(PrimitiveKnownToFolding::text(texts[place])));
        }
        if (hasFailed())
            return KeyOfAccess::ArmsHaveWalkedAccess;
        continueIn(underNoName);
        numberOfTests = 0;
    }
    for (size_t place = 0; place < numberOfTests && !isUnreachable() && !hasFailed(); ++place) {
        Draft test(textsAreOnlyExpected ? Opcode::ValueEqualsLiteral : Opcode::StringEqualsLiteral, origin);
        chooseConservativeFields(test);
        test.setOperand(0, comparesInFront ? key : left);
        test.setLiteral(texts[place]);
        Reference passes;
        {
            SpecializedPreheaderScope preheaderScope(*this, comparesInFront);
            passes = build(WTF::move(test));
        }
        if (!passes)
            return KeyOfAccess::ArmsHaveWalkedAccess;
        BlockID passed = addBlock();
        BlockID failed = addBlock(textsAreOnlyExpected && place + 1 == numberOfTests ? BlockFrequency::Rare : BlockFrequency::Normal);
        branch(passes, passed, failed);
        continueIn(passed);
        if (Reference narrowed = narrowedBy(left, passes, true))
            walkArmWith(narrowed);
        if (hasFailed())
            return KeyOfAccess::ArmsHaveWalkedAccess;
        continueIn(failed);
        if (!comparesInFront || !textsAreOnlyExpected)
            left = narrowedBy(left, passes, false);
        if (!left)
            return KeyOfAccess::ArmsHaveWalkedAccess;
    }
    if (textsAreOnlyExpected && !isUnreachable() && !hasFailed()) {
        ClosedArm closedArm(*this, true);
        if (!isUnreachable())
            walkArmWith(left);
    } else if (!isUnreachable() && !hasFailed())
        walkArmWith(left);
    if (!hasFailed())
        joinArms(stretch, built, meeting);
    return KeyOfAccess::ArmsHaveWalkedAccess;
}

void Walker::stopUnlessValueIsOf(Reference value, TypeID tested, ProofSource source)
{
    Origin origin = m_origin;
    origin.isTailCall = false;
    Representation form = representationOf(value);
    if (form != Representation::JSValue && form != Representation::Cell && form != Representation::Cell32)
        value = convert(value, Representation::JSValue, origin);
    if (value && !isUnreachable())
        buildCheckOfProof(value, tested, TypeTable::anything, source, origin);
}

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
    case PropertyKeyKind::EnginePrivateName:
        return access.opcode() == Opcode::PropGet && access.isFreeName();
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

LocationMasks readsOfOwnKeyLookup(const Program& program)
{
    LocationMasks reads = declaredBoundInTable(Opcode::GetOwnValueUnderKeyOrAbsent).reads;
    reads.add(program, MemoryLocation::objectField(program.keyOf(BuiltinNames::__proto__)));
    return reads;
}

void narrowGlobalAccessEffects(const Program& program, Opcode opcode, PropertyKeyID key, Effects& effects)
{
    MemoryLocation property = MemoryLocation::objectField(key);
    if (opcode == Opcode::GetGlobalProperty) {
        effects.reads.add(program, property);
        return;
    }
    RELEASE_ASSERT(opcode == Opcode::PutGlobalBinding);
    effects.writes.remove(AbstractHeapSet { AbstractHeap::NamedProperties });
    effects.writes.add(program, property);
}

static bool hasGlobalBinding(const Walker& walker, const BodyValue& access, TypeID typeOfObject)
{
    if (!typeOfObject || holdsOnlyPrimitives(typeOfObject))
        return false;
    const Type& type = walker.types().recordOf(typeOfObject);
    BuiltinID named = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    if (!named)
        return false;
    const Program& program = walker.program();
    if (program.builtins().canonicalID(named) != program.builtins().canonicalID(Builtins::GlobalThis) || !mayHaveGlobalBindingForReplacement(program, access))
        return false;
    if (access.opcode() == Opcode::PropGet)
        return true;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID holder = index.stripCasts(access.objectOperand());
    if (!index.isParameter(holder) && index.opcodeOf(holder) == Opcode::Builtin)
        return walker.plan().globalName(access.key()).isStoredThroughBinding;
    BuiltinName name = program.builtinNameOf(access.key());
    return !name || !program.builtins().findGlobal(name);
}

bool mayUseGlobalBindingInIterations(const Walker& walker, const BodyValue& access, TypeID typeOfObject)
{
    if (access.opcode() == Opcode::PropGet && access.isForTypeOf())
        return false;
    if (BuiltinName name = access.isFreeName() ? walker.program().builtinNameOf(access.key()) : BuiltinName(); name && walker.program().builtins().findGlobal(name))
        return false;
    return hasGlobalBinding(walker, access, typeOfObject);
}

GlobalAddress buildGlobalAddress(Walker& walker, const BodyValue& access, Origin origin)
{
    Reference address;
    Reference test;
    bool isInPreheader = walker.canHoistGlobalAddress();
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInPreheader, Walker::IterationPurity::KnownEffectsOnly);
        Draft lookup(Opcode::GetGlobalPropertyAddress, origin);
        lookup.setKey(access.key());
        lookup.setPrecedesStore(access.opcode() == Opcode::PropSet);
        lookup.setSeesScriptBindings(access.isFreeName());
        address = walker.build(WTF::move(lookup));
        if (!address || walker.hasFailed())
            return { };
        Draft question(Opcode::IsPropertyFiled, origin);
        question.setAddressOperand(address);
        test = walker.build(WTF::move(question));
        if (!test || walker.hasFailed())
            return { };
    }
    walker.recordSpeculation();
    if (isInPreheader && walker.tryBranchInPreheader(test, true, Walker::IterationPurity::KnownEffectsOnly))
        return { address, true };
    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID goesOn = walker.addBlock();
    walker.branch(test, goesOn, leaves);
    walker.continueIn(leaves);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(goesOn);
    return { walker.isUnreachable() || walker.hasFailed() ? Reference() : address };
}

bool readFiledGlobal(Walker& walker, const BodyValue& read)
{
    Reference object = walker.referenceOf(read.objectOperand());
    if (read.isForTypeOf() || !object || !hasGlobalBinding(walker, read, walker.typeOf(object)))
        return false;
    const Type& typeOfObject = walker.types().at(walker.typeOf(object));
    BuiltinID holder = typeOfObject.isObject() ? singleBuiltinOf(walker.types(), typeOfObject) : BuiltinID();
    if (holder && findConstantMemberFrom(walker.program(), walker.facts(), holder, read.key()))
        return false;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Draft lookup(Opcode::GetGlobalPropertyAddress, origin);
    lookup.setKey(read.key());
    lookup.setSeesScriptBindings(read.isFreeName());
    Reference address = walker.build(WTF::move(lookup));
    if (!address)
        return walker.isUnreachable() || walker.hasFailed();
    Draft question(Opcode::IsPropertyFiled, origin);
    question.setAddressOperand(address);
    Reference isFiled = walker.build(WTF::move(question));
    if (!isFiled)
        return walker.isUnreachable() || walker.hasFailed();
    walker.recordSpeculation();
    BlockID passed = walker.addBlock();
    walker.branch(isFiled, passed, walker.blockWhereCopyLeavesForCall());
    walker.continueIn(passed);
    if (walker.isUnreachable())
        return true;
    Draft property(Opcode::GetGlobalProperty, origin);
    property.setAddressOperand(address);
    property.setKey(read.key());
    walker.bind(read.id(), walker.build(WTF::move(property)));
    return true;
}

static bool storeThroughBindingInIteration(Walker& walker, const BodyValue& access)
{
    if (BuiltinName name = walker.program().builtinNameOf(access.key()); name && walker.program().builtins().findGlobal(name))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations) || !walker.canBuildIterationGuardAfterStores() || !walker.canRecordUndoableStore() || walker.hasUninlinedCallInIterations())
        return false;
    Reference stored = walker.referenceOf(access.valueOperand());
    if (!stored)
        return false;
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    Reference address = buildGlobalAddress(walker, access, origin).address;
    if (!address)
        return true;
    Reference before;
    if (walker.canRecordUndoableStore()) {
        Draft read(Opcode::GetGlobalProperty, origin);
        read.setAddressOperand(address);
        read.setKey(access.key());
        before = walker.build(WTF::move(read));
        if (!before || walker.hasFailed())
            return true;
    }
    Draft store(Opcode::PutGlobalBinding, origin);
    walker.chooseConservativeFields(store);
    store.setAddressOperand(address);
    store.setKey(access.key());
    store.setValueOperand(stored);
    store.inputValueGiven = access.id();
    if (before)
        walker.recordUndoableGlobalStore(access.key(), address, before);
    walker.build(WTF::move(store));
    walker.takePendingUndoableStore();
    return true;
}

static bool replaceAccessToGlobalName(Walker& walker, const BodyValue& access)
{
    Reference object = walker.referenceOf(access.objectOperand());
    if (!object || !hasGlobalBinding(walker, access, walker.typeOf(object)))
        return false;

    PropertyKeyID key = access.key();
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

    if (storeThroughBindingInIteration(walker, access))
        return true;
    ValueID holder = walker.currentBodyIndex().stripCasts(access.objectOperand());
    bool namesBuiltin = !walker.currentBodyIndex().isParameter(holder) && walker.currentBodyIndex().opcodeOf(holder) == Opcode::Builtin;
    if (!namesBuiltin && !walker.isInLoop() && walker.program().at(walker.unit().function).isModuleBody())
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

static bool answerWhetherNamesWereAddedToHolder(Walker& walker, const BodyValue& asked)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID holder = table.canonicalID(asked.builtin());
    if (program.admitsReplacedBuiltins) {
        walker.bind(asked.id(), buildTestOfHolderWithoutAddedNames(walker, holder, walker.originOf(asked)));
        return true;
    }
    Draft answer(Opcode::Boolean, walker.originOf(asked));
    answer.setBoolean(table.at(holder).flags.contains(BuiltinFlag::IsLockedObject) && !program.isSomeNameOpenedOn(holder));
    walker.bind(asked.id(), walker.constant(WTF::move(answer)));
    return true;
}

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

static bool replaceSpeciesOfListByNewList(Walker& walker, const BodyValue& created)
{
    auto operation = created.as<Opcode::ArraySpeciesCreate>();
    Reference length = walker.referenceOf(operation.lengthOperand());
    Reference source = walker.referenceOf(operation.arrayOperand());
    bool runOverBoxedValuesTakesListFirst = walker.runOverBoxedValuesTakesNewListOfCopyFirst();
    if (!walker.takeWordThatCopyMakesNewList(source) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    if (!length || !walker.types().at(walker.typeOf(length)).isInteger())
        return false;
    Draft list(Opcode::NewArrayLike, walker.originOf(created));
    walker.chooseConservativeFields(list);
    list.setLengthOperand(length);
    list.setSourceOperand(operation.likeOperand() ? walker.referenceOf(operation.likeOperand()) : source);
    if (Reference room = operation.capacityOperand() ? walker.referenceOf(operation.capacityOperand()) : Reference(); room && walker.types().at(walker.typeOf(room)).isInteger())
        list.setCapacityOperand(room);
    list.setArrayStorage(ArrayStorage::Other);
    list.inputValueGiven = created.id();
    Reference made = walker.build(WTF::move(list));
    if (runOverBoxedValuesTakesListFirst && made && made.isValue())
        walker.noteListThatRunOverBoxedValuesTakesFirst(made.id());
    noteLengthThatListIsBornWith(walker, made);
    walker.bind(created.id(), made);
    walker.count(StatisticID::ArraySpeciesLookupsElided);
    return true;
}

constinit const OpcodeSet opcodesThatReplaceOperationTakes = opcodesOfAllReplacements;
static_assert(opcodesThatReplaceOperationTakesInChains == opcodesOfAllReplacements);

bool replaceOperationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    ASSERT(opcodesOfAllReplacements.contains(opcode));
    if (opcode == Opcode::PropGet || opcode == Opcode::PropSet)
        return replaceAccessToGlobalName(walker, value);
    if (opcode == Opcode::HasJSType)
        return replaceTestOfCellTypeByTestOfClass(walker, value);
    if (opcode == Opcode::IsHolderWithoutAddedNames)
        return answerWhetherNamesWereAddedToHolder(walker, value);
    if (opcode == Opcode::ArraySpeciesCreate)
        return replaceSpeciesOfListByNewList(walker, value);

    if (opcode == Opcode::DefineMethod)
        return leaveMemberToClassRecord(walker, value);
    if (opcode == Opcode::DefineGetter || opcode == Opcode::DefineSetter)
        return layAccessorsOfLiteralTogether(walker, value) || defineAccessorUnderNameThatKeyIs(walker, value);

    if (opcode == Opcode::Call && walker.hasUninformativeType(walker.referenceOf(value.calleeOperand())))
        return false;
    if (opcode == Opcode::Call && (replaceCallDoneInPlace(walker, value) || replaceCallThatMakesSymbolOnce(walker, value)))
        return true;
    if (callsByValue.contains(opcode) || constructionsByValue.contains(opcode))
        return replaceCallOfCompiledBuiltin(walker, value);

    if (opcodesThatStateTypes.contains(opcode)) {
        bindOperandInPlaceOfCastForReplacement(walker, value);
        return true;
    }
    if (opcode == Opcode::RecordRequire)
        return walker.plan().moduleEvaluation().canSkipRecordRequire(value.as<Opcode::RecordRequire>().module());

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

    if (!isReplaced)
        walker.fail("an operation that has no form in Code was not replaced"_s);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
