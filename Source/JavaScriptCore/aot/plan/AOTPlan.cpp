/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlan.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTFieldsStorage.h"
#include "AOTIRProgram.h"
#include "AOTMechanisms.h"
#include "AOTNameClass.h"
#include "AOTNumberMethodSelectors.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramVerifier.h"
#include "AOTRowsOfLayouts.h"
#include "AOTScalarizedResults.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTTailCallPlan.h"
#include "AOTUnionPlan.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!(AOT::Closure::allocationSize(1) % 16));
static_assert(AOT::Closure::maxNumberOfCaptures % 2);
static_assert(unorderedOrderWord == orderWordOf(0, 0));
static_assert(representsAbsentAsZero(SlotRepresentation::JSValue) && traitsOf(SlotRepresentation::JSValue).undefinedBits);

StorageOfLayouts chooseStorageOfLayouts(const Program&, const Facts&, const ParallelLoops&);
SlotPlanParts computeSlotPlanParts(Program&, const Facts&, BuildContext&, const StorageOfLayouts&, KeyOrders&, PromotedEnvironments&);
void holdDoorsToProofsInSlotPlan(Program&, const Facts&, const ParallelLoops&, SlotPlanParts&);
ShapeTable computeShapeTable(const Program&, const LinkTables&, const StorageOfLayouts&, LayoutID closureLayout, std::span<const uint32_t> orderOfLayouts);
Vector<ParameterGuardPlan> planParameterGuards(Program&, const Facts&, const Plan&, BuildContext&);
void finishMethodTableForUnions(Program&, const Facts&, Plan&);
ModulesThatWriteFields findModulesThatWriteFields(const Program&, const Facts&, const SlotPlan&, const ParallelLoops&);

static MethodTablePlan planMethodTable(const Program& program, const Facts& facts, const CallingConventions& conventions, const StorageOfLayouts& storage, const BitVector& layoutsWithClassRecord)
{
    MethodTablePlan table;
    if (!facts.mechanisms().contains(Mechanism::DirectCalls))
        return table;
    const LinkTables& linked = facts.linked();


    Vector<uint8_t> numbersOfArgumentsPassedUnderKey;
    numbersOfArgumentsPassedUnderKey.fill(0, program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!facts.mayBeReached(FunctionID(index)))
            continue;
        for (const SummaryCall& call : facts.summaries().at(FunctionID(index)).calls) {
            if (call.callee == SummaryCall::Callee::Name && call.key && !call.isMadeByProtocol && !call.isConstruction && !call.passesList && call.numberOfArguments <= maxArgumentsOfTableCall)
                numbersOfArgumentsPassedUnderKey[call.key.index()] |= 1 << call.numberOfArguments;
        }
    }

    auto isEnteredThroughTable = [&](FunctionID method) {
        const Function& function = program.at(method);
        FunctionConvention convention = conventions.of(method);
        if (linked.function(method).everyCallerIsKnown && linked.function(method).everyCallHasOneCallee)
            return false;
        return function.kind == FunctionKind::Method && function.captures.isEmpty() && !function.has(FunctionFlags::TakesCallee) && !function.has(FunctionFlags::HasNewTarget)
            && convention.has(CodeRole::Body) && !convention.takesArgumentsInBuffer && conventions.numberOfResults(conventions.directEntryOf(method)) == 1;
    };
    auto sideOf = [](const LinkedMember& member) {
        return std::pair { member.classLayout, member.side };
    };

    enum class AnswerBelowClass : uint8_t { NotAsked, IsMethodWritten, IsSomethingElse };
    struct MembersOfClass {
        uint32_t first { 0 };
        uint32_t end { 0 };
        bool isAsked { false };
        bool hasMemberWithoutKey { false };
        bool hasMemberUnderKeyThatIsCalled { false };
    };
    Vector<MembersOfClass> membersOfClasses(program.layouts.size());
    BitVector membersThatMayHaveRows(linked.members.size());
    Vector<AnswerBelowClass> answersBelowClassesOfMembers;
    answersBelowClassesOfMembers.fill(AnswerBelowClass::NotAsked, linked.members.size());
    auto membersOf = [&](LayoutID current) -> const MembersOfClass& {
        MembersOfClass& known = membersOfClasses[current.index()];
        if (std::exchange(known.isAsked, true))
            return known;
        auto members = std::ranges::equal_range(linked.members, std::pair { current, ClassSide::Prototype }, { }, sideOf);
        known.first = static_cast<uint32_t>(members.begin() - linked.members.begin());
        known.end = static_cast<uint32_t>(members.end() - linked.members.begin());
        for (uint32_t place = known.first; place < known.end; ++place) {
            const LinkedMember& member = linked.members[place];
            if (!member.key) {
                known.hasMemberWithoutKey = true;
                continue;
            }
            known.hasMemberUnderKeyThatIsCalled |= !!numbersOfArgumentsPassedUnderKey[member.key.index()];
            if (numbersOfArgumentsPassedUnderKey[member.key.index()] && member.kind == MethodKind::Method && member.written && isEnteredThroughTable(member.written)
                && linked.membersUnder(current, ClassSide::Prototype, member.key).size() == 1 && program.at(member.key).kind == PropertyKeyKind::Text)
                membersThatMayHaveRows.quickSet(place);
        }
        return known;
    };

    constexpr uint32_t isNotAsked = UINT32_MAX - 1;
    Vector<uint32_t> nearestClassesWithMembers;
    nearestClassesWithMembers.fill(isNotAsked, program.layouts.size());
    auto nearestClassWithMembers = [&](LayoutID from) {
        Vector<uint32_t, 16> classesPassed;
        LayoutID current = from;
        for (; current && nearestClassesWithMembers[current.index()] == isNotAsked && classesPassed.size() <= program.layouts.size(); current = program.at(current).base) {
            const MembersOfClass& members = membersOf(current);
            if (members.hasMemberWithoutKey || members.hasMemberUnderKeyThatIsCalled || program.at(current).has(LayoutFlags::IsConstructorFunction)) {
                nearestClassesWithMembers[current.index()] = current.index();
                break;
            }
            classesPassed.append(current.index());
        }
        LayoutID nearest = current && nearestClassesWithMembers[current.index()] != isNotAsked ? LayoutID::fromBits(nearestClassesWithMembers[current.index()]) : LayoutID();
        for (uint32_t passed : classesPassed)
            nearestClassesWithMembers[passed] = nearest.bits();
        return nearest;
    };

    BitVector keysOfFields(program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& record = program.layouts[index];
        size_t numberNotCopied = baseWhoseFieldListIsCopiedWhole(program, LayoutID(index)) ? record.ownFieldCount : record.fields.size();
        for (size_t field = 0; field < numberNotCopied; ++field) {
            if (record.fields[field].key)
                keysOfFields.quickSet(record.fields[field].key.index());
        }
    }

    Vector<PropertyKeyID> keysOfRows;
    Vector<MethodTableRow> rowsAsFound;
    Vector<uint32_t> numbersOfRowsUnderKeys;
    numbersOfRowsUnderKeys.fill(0, program.types.numberOfPropertyKeys() + 1);
    Vector<uint32_t> lastLayoutThatPassedKey;
    lastLayoutThatPassedKey.fill(UINT32_MAX, program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID layout(index);
        const Layout& record = program.layouts[index];
        if (record.kind != LayoutKind::Instance || record.has(LayoutFlags::IsConstructorFunction) || !isFoundByShapeTest(record) || storage.isNeverAllocated(layout) || !layoutsWithClassRecord.get(index))
            continue;
        bool keepsWhatIsFoundAbove = !linked.layout(layout).possibleChanges.containsAny({ LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        bool listsEveryMemberAbove = linked.layout(layout).listsEveryMemberAbove;
        unsigned numberOfClassesPassed = 0;
        for (LayoutID current = nearestClassWithMembers(layout); current; current = nearestClassWithMembers(program.at(current).base)) {
            if (++numberOfClassesPassed > program.layouts.size() || program.at(current).has(LayoutFlags::IsConstructorFunction))
                break;
            const MembersOfClass& members = membersOf(current);
            if (members.hasMemberWithoutKey)
                break;
            for (uint32_t place = members.first; place < members.end; ++place) {
                const LinkedMember& member = linked.members[place];
                if (std::exchange(lastLayoutThatPassedKey[member.key.index()], index) == index)
                    continue;
                if (!membersThatMayHaveRows.quickGet(place))
                    continue;
                if (keysOfFields.quickGet(member.key.index()) && findField(program, layout, member.key))
                    continue;
                if (std::ranges::contains(linked.appendedKeysOf(layout), member.key) || std::ranges::contains(linked.keysAppendedByHeldAloneOf(layout), member.key))
                    continue;
                auto findsMethodWritten = [&] {
                    return linked.findMember(program, layout, ClassSide::Prototype, member.key).method == member.written;
                };
                bool isProven = false;
                if (keepsWhatIsFoundAbove && listsEveryMemberAbove) {
                    if (current == layout)
                        isProven = findsMethodWritten();
                    else {
                        AnswerBelowClass& answer = answersBelowClassesOfMembers[place];
                        if (answer == AnswerBelowClass::NotAsked)
                            answer = findsMethodWritten() ? AnswerBelowClass::IsMethodWritten : AnswerBelowClass::IsSomethingElse;
                        isProven = answer == AnswerBelowClass::IsMethodWritten;
                    }
                }
                keysOfRows.append(member.key);
                rowsAsFound.append(MethodTableRow { layout, member.written, isProven ? LayoutID() : layout });
                ++numbersOfRowsUnderKeys[member.key.index() + 1];
            }
        }
    }
    for (size_t key = 1; key < numbersOfRowsUnderKeys.size(); ++key)
        numbersOfRowsUnderKeys[key] += numbersOfRowsUnderKeys[key - 1];
    struct RowUnderKey {
        PropertyKeyID key;
        MethodTableRow row;
    };
    Vector<RowUnderKey> rows(rowsAsFound.size());
    for (size_t place = 0; place < rowsAsFound.size(); ++place)
        rows[numbersOfRowsUnderKeys[keysOfRows[place].index()]++] = { keysOfRows[place], rowsAsFound[place] };

    for (size_t first = 0; first < rows.size();) {
        MethodFamily family;
        family.key = rows[first].key;
        size_t end = first;
        for (; end < rows.size() && rows[end].key == family.key; ++end) {
            family.rows.append(rows[end].row);
            if (family.methods.isEmpty() || family.methods.last() != rows[end].row.method)
                family.methods.append(rows[end].row.method);
            family.hasGuardedRow = true;
        }
        family.hasGuardedRow = !formsOfSelector(program, linked, conventions, family).everyRowIsProven;
        first = end;
        AOT::sort(family.methods);
        removeRepeatedElements(family.methods);
        uint32_t placeOfFamily = static_cast<uint32_t>(table.families.size());
        for (unsigned numberOfArguments = 0; numberOfArguments <= maxArgumentsOfTableCall; ++numberOfArguments) {
            if (!(numbersOfArgumentsPassedUnderKey[family.key.index()] & 1 << numberOfArguments))
                continue;
            table.selectors.append(MethodSelector { placeOfFamily, numberOfArguments, table.numberOfUnits });
            table.numberOfUnits += 1 + static_cast<uint32_t>(family.methods.size());
        }
        table.families.append(WTF::move(family));
    }
    return table;
}

static BitVector variablesDescribedToRunTimeByPlan(const Program& program)
{
    BitVector described(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            described.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            add(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            add(binding.variable);
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::HoldsInstance, VariableFlags::GuardsInstance, VariableFlags::AllocatesInstance, VariableFlags::HasSeveralInstances }) || program.variables[index].owner)
            described.quickSet(index);
    }
    return described;
}

static Vector<uint32_t> variablesNamedByFunctionsThatBodyCallsInPlan(const Program& program, const Facts& facts, FunctionID body)
{
    Vector<uint32_t> named;
    const ThinSummary& summary = facts.summaries().at(body);
    for (const SummaryCall& call : summary.calls) {
        FunctionID called;
        if (call.callee == SummaryCall::Callee::Binding)
            called = call.function;
        else if (call.callee == SummaryCall::Callee::Value && call.subject != noSummaryNode && summary.nodes[call.subject].kind == SummaryNode::Kind::Closure)
            called = FunctionID(summary.nodes[call.subject].first);
        if (!called || called == body || program.at(called).module != program.at(body).module)
            continue;
        for (const SummaryNode& node : facts.summaries().at(called).nodes) {
            if (node.kind == SummaryNode::Kind::Variable)
                named.append(node.first);
        }
    }
    std::ranges::sort(named);
    return named;
}

static DeferredDefinitions deferredDefinitionsOfPlan(const Program& program, const Facts& facts, const BitVector& variablesDescribedToRunTime, bool checksProofs)
{
    DeferredDefinitions result;
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::Deferral))
        return result;

    const LinkTables& linked = facts.linked();
    BitVector bodiesSeen(program.functions.size());
    BitVector bodiesOfSeveralModules(program.functions.size());
    for (const Module& module : program.modules) {
        if (module.body && bodiesSeen.quickSet(module.body.index()))
            bodiesOfSeveralModules.quickSet(module.body.index());
    }

    for (size_t number : bodiesSeen) {
        FunctionID body(static_cast<uint32_t>(number));
        if (bodiesOfSeveralModules.quickGet(number))
            continue;
        const BodyIndex* index = nullptr;
        std::optional<Vector<uint32_t>> namedByCalls;
        for (const SummaryDefinition& definition : facts.summaries().at(body).definitions) {
            const Variable& variable = program.at(definition.variable);
            if (definition.variable.index() >= linked.variables.size() || variablesDescribedToRunTime.quickGet(definition.variable.index()))
                continue;
            const LinkedCell& cell = linked.variable(definition.variable);
            if (!cell.everyWriterIsKnown || cell.isWrittenAfterConstruction || !cell.proven || variable.owner || variable.initialization == VariableInitialization::Function
                || variable.flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::IsReplaceable, VariableFlags::HoldsInstance }))
                continue;
            if (!index)
                index = facts.bodies().find(body);
            if (!index)
                break;
            BodyValue made = index->value(definition.made);
            if (made.opcode() == Opcode::Data) {
                if (!namedByCalls)
                    namedByCalls = variablesNamedByFunctionsThatBodyCallsInPlan(program, facts, body);
                if (std::ranges::binary_search(*namedByCalls, definition.variable.index()))
                    continue;
            }
            if (made.opcode() == Opcode::NewClosure)
                continue;

            bool isBornAbsent = variable.initialization == VariableInitialization::Uninitialized;
            bool isMarkedFromBirth = isBornAbsent && !checksProofs && (cell.isFilledAtEveryRead || !(program.at(cell.proven).primitives & Primitive::Absent));
            uint16_t mark = isMarkedFromBirth ? Primitive::Absent : isBornAbsent ? Primitive::Undefined : Primitive::Null;
            if (std::optional<ProvenInEveryVersion> established = variable.declared.established(); established && !(program.at(*established).primitives & mark))
                continue;
            result.variables.append(DeferredVariable { definition.variable, mark, !isMarkedFromBirth, body, definition.made, made.opcode() });
        }
    }

    std::ranges::sort(result.variables, { }, &DeferredVariable::variable);
    result.isDeferred.ensureSize(program.variables.size());
    result.bodiesThatDefine.ensureSize(program.functions.size());
    for (uint32_t place = 0; place < result.variables.size(); ++place) {
        RELEASE_ASSERT(!result.isDeferred.quickSet(result.variables[place].variable.index()));
        result.bodiesThatDefine.quickSet(result.variables[place].body.index());
        result.variablesByMaker.append(place);
    }
    std::ranges::sort(result.variablesByMaker, { }, [&](uint32_t place) {
        return std::pair { result.variables[place].body, result.variables[place].made };
    });
    return result;
}

static SlotRepresentation formOfNumbersProvenOfVariableInPlan(const Program& program, const LinkTables& linked, VariableID id)
{
    constexpr uint16_t markers = Primitive::UndefinedOrNull | Primitive::Absent;
    if (id.index() >= linked.variables.size())
        return SlotRepresentation::JSValue;
    const LinkedCell& cell = linked.variable(id);
    if (!cell.everyWriterIsKnown || !cell.proven || isNeverType(cell.proven))
        return SlotRepresentation::JSValue;
    Type proven = program.at(cell.proven);
    if (!(proven.primitives & Primitive::Number) || !proven.hasOnlyPrimitives(Primitive::Number | markers))
        return SlotRepresentation::JSValue;
    if (!(proven.primitives & Primitive::Double))
        return SlotRepresentation::OptionalInt64;
#if CPU(X86_64)
    constexpr bool boxesCutVariables = true;
#else
    constexpr bool boxesCutVariables = false;
#endif
    bool isCutToInt32 = boxesCutVariables && id.index() < linked.variablesCutToInt32.size() && linked.variablesCutToInt32.quickGet(id.index());
    return isCutToInt32 ? SlotRepresentation::JSValue : SlotRepresentation::OptionalDouble;
}

template<typename Functor>
static auto findInClassOfVariable(const Program& program, const Facts& facts, VariableID id, const Functor& functor) -> decltype(functor(std::declval<const ClassPart&>()))
{
    auto findIn = [&](TypeID held) -> decltype(functor(std::declval<const ClassPart&>())) {
        if (!held || isNeverType(held))
            return { };
        Type type = program.at(held);
        type.primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent);
        const ClassPart* onlyClass = singleBuiltinClassOf(TypeReader(program), type);
        return onlyClass ? functor(*onlyClass) : decltype(functor(*onlyClass)) { };
    };
    if (auto found = findIn(facts.beforeEvaluation().typeOf(id)))
        return found;
    return id.index() < facts.linked().variables.size() ? findIn(facts.linked().variable(id).proven) : decltype(functor(std::declval<const ClassPart&>())) { };
}

static std::optional<TypedArrayType> findViewTypeOfVariable(const Program& program, const Facts& facts, VariableID id)
{
    return findInClassOfVariable(program, facts, id, [&](const ClassPart& held) -> std::optional<TypedArrayType> {
        TypedArrayType kind = program.builtins().typedArrayTypeOf(held.builtinClass);
        return isTypedView(kind) && hasInlineElementAccess(kind) ? std::optional(kind) : std::nullopt;
    });
}

static bool isViewVariable(const Program& program, const Facts& facts, VariableID id)
{
    return findViewTypeOfVariable(program, facts, id).has_value();
}

static bool isBufferVariable(const Program& program, const Facts& facts, VariableID id)
{
    return findInClassOfVariable(program, facts, id, [](const ClassPart& held) {
        return held.builtinClass == BuiltinClasses::ArrayBuffer;
    });
}

static void findViewsOfOneBuffer(const Program& program, const Facts& facts, Vector<PlannedVariable>& variables)
{
    BitVector modulesWithViews(program.modules.size());
    BitVector modulesAsked(program.modules.size());
    Vector<uint32_t> views;
    for (uint32_t index = 0; index < variables.size(); ++index) {
        ModuleID module = program.variables[index].module;
        if (!variables[index].hasViewMirror || !module)
            continue;
        views.append(index);
        if (modulesWithViews.quickSet(module.index()))
            modulesAsked.quickSet(module.index());
    }
    if (modulesAsked.isEmpty())
        return;

    struct BufferOfVariable {
        VariableID variable;
        uint32_t buffer;
    };
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> buffersOfViews;
    BitVector givenOtherwise(variables.size());
    uint32_t numberOfBuffers = 0;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        ModuleID module = program.functions[number].module;
        const BodyIndex* index = module && modulesAsked.quickGet(module.index()) ? facts.bodies().find(FunctionID(number)) : nullptr;
        if (!index || !index->opcodes().contains(Opcode::PutModuleVariable))
            continue;
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            Vector<BufferOfVariable, 2> buffers;
            auto bufferOfView = [&](ValueID stored) -> std::optional<uint32_t> {
                if (index->isParameter(stored) || index->opcodeOf(stored) != Opcode::Construct)
                    return std::nullopt;
                std::span<const ValueID> arguments = index->value(stored).argumentsOperands();
                ValueID given = arguments.size() == 1 ? index->stripCasts(arguments[0]) : ValueID();
                if (!given || index->isParameter(given) || index->opcodeOf(given) != Opcode::GetModuleVariable || index->blockOf(given) != BlockID(block))
                    return std::nullopt;
                VariableID variable = index->value(given).variable();
                if (!isBufferVariable(program, facts, variable))
                    return std::nullopt;
                auto found = std::ranges::find(buffers, variable, &BufferOfVariable::variable);
                if (found == buffers.end()) {
                    buffers.append({ variable, numberOfBuffers++ });
                    return buffers.last().buffer;
                }
                return found->buffer;
            };
            for (BodyValue store : index->block(BlockID(block)).values()) {
                if (store.opcode() != Opcode::PutModuleVariable)
                    continue;
                VariableID variable = store.variable();
                buffers.removeAllMatching([&](const BufferOfVariable& held) {
                    return held.variable == variable;
                });
                if (!variables[variable.index()].hasViewMirror)
                    continue;
                ValueID stored = index->stripCasts(store.valueOperand());
                if (!index->isParameter(stored) && (index->opcodeOf(stored) == Opcode::Undefined || index->opcodeOf(stored) == Opcode::Null))
                    continue;
                if (std::optional<uint32_t> buffer = bufferOfView(stored))
                    buffersOfViews.add(variable.index(), Vector<uint32_t, 2>()).iterator->value.append(*buffer);
                else
                    givenOtherwise.quickSet(variable.index());
            }
        }
    }

    for (uint32_t place = 0; place < views.size(); ++place) {
        auto buffers = buffersOfViews.find(views[place]);
        if (buffers == buffersOfViews.end() || givenOtherwise.quickGet(views[place]) || variables[views[place]].firstViewOfBuffer)
            continue;
        Vector<uint32_t, 8> together;
        for (uint32_t other : views.span().subspan(place)) {
            auto buffersOfOther = buffersOfViews.find(other);
            if (buffersOfOther != buffersOfViews.end() && buffersOfOther->value == buffers->value && !givenOtherwise.quickGet(other))
                together.append(other);
        }
        if (together.size() < 2)
            continue;
        uint64_t locations = 0;
        for (uint32_t view : together)
            locations |= Typed::maskOf(program, MemoryLocation::moduleVariable(VariableID(view)));
        for (uint32_t view : together) {
            unsigned size = logElementSize(*findViewTypeOfVariable(program, facts, VariableID(view)));
            variables[view].firstViewOfBuffer = VariableID(together[0]);
            variables[view].firstViewOfElementSize = VariableID(*std::ranges::find_if(together, [&](uint32_t other) {
                return logElementSize(*findViewTypeOfVariable(program, facts, VariableID(other))) == size;
            }));
            variables[view].locationsOfViewsOfBuffer = locations;
        }
    }
}

static Vector<PlannedVariable> variablesOfPlan(const Program& program, const Facts& facts, const BitVector& described)
{
    bool leavesOutWordsNeverStored = facts.mechanisms().contains(Mechanism::TypedSlots);
    bool holdsNumbersRaw = withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots);
    BitVector filledByRunTime = holdsNumbersRaw ? variablesThatRunTimeMayFill(program) : BitVector();

    Vector<PlannedVariable> variables(program.variables.size());
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& declaration = program.variables[index];
        PlannedVariable& planned = variables[index];
        planned.isDescribedToRunTime = described.quickGet(index);
        if (declaration.owner)
            continue;
        planned.hasViewMirror = holdsNumbersRaw && declaration.initialization != VariableInitialization::Function
            && !declaration.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance })
            && isViewVariable(program, facts, VariableID(index));
        if (holdsNumbersRaw && !planned.isDescribedToRunTime && declaration.initialization != VariableInitialization::Function && !filledByRunTime.get(index)
            && !declaration.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance }))
            planned.representation = formOfNumbersProvenOfVariableInPlan(program, facts.linked(), VariableID(index));
        SlotTraits traits = traitsOf(planned.representation);
        switch (declaration.initialization) {
        case VariableInitialization::Uninitialized:
            planned.initialWord = traits.absentBits;
            break;
        case VariableInitialization::Undefined:
            planned.initialWord = traits.undefinedBits;
            break;
        case VariableInitialization::Function:
            RELEASE_ASSERT(declaration.function);
            planned.encoding = VariableEncoding::FunctionAsZero;
            planned.function = declaration.function;
            if (leavesOutWordsNeverStored && facts.linked().function(declaration.function).isReached && !facts.linked().variable(VariableID(index)).isStoredInto)
                planned.storage = VariableStorage::ClosureTable;
            break;
        }
    }
    if (holdsNumbersRaw)
        findViewsOfOneBuffer(program, facts, variables);
    return variables;
}

static BitVector modulesWhoseImportsAreNeverMockedInPlan(const Program& program)
{
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    BitVector neverMocked(numberOfModules);
    if (program.admitsReplacedBuiltins)
        return neverMocked;

    Vector<uint32_t> firstImporter;
    firstImporter.fill(0, numberOfModules + 1);
    auto forEachImport = [&](const Invocable<void(uint32_t imported, uint32_t importer)> auto& functor) {
        for (uint32_t index = 0; index < numberOfModules; ++index) {
            for (const ModuleRequest& request : program.modules[index].requests) {
                if (request.kind == ModuleRequestKind::Import && request.target)
                    functor(request.target.index(), index);
            }
        }
    };
    forEachImport([&](uint32_t imported, uint32_t) {
        ++firstImporter[imported + 1];
    });
    for (uint32_t index = 0; index < numberOfModules; ++index)
        firstImporter[index + 1] += firstImporter[index];
    Vector<uint32_t> importers;
    importers.fill(0, firstImporter.last());
    Vector<uint32_t> nextImporter = firstImporter;
    forEachImport([&](uint32_t imported, uint32_t importer) {
        importers[nextImporter[imported]++] = importer;
    });

    BitVector importsWhatMayBeMocked(numberOfModules);
    Vector<uint32_t, 16> pending;
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (program.modules[index].flags.contains(ModuleFlags::MayBeMocked))
            pending.append(index);
    }
    while (!pending.isEmpty()) {
        uint32_t imported = pending.takeLast();
        for (uint32_t importer : importers.subspan(firstImporter[imported], firstImporter[imported + 1] - firstImporter[imported])) {
            if (!importsWhatMayBeMocked.quickSet(importer))
                pending.append(importer);
        }
    }
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (!importsWhatMayBeMocked.quickGet(index))
            neverMocked.quickSet(index);
    }
    return neverMocked;
}

static void chooseModuleWordsOfPlan(const Program& program, PlanParts& parts)
{
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    parts.modulesWithValue.ensureSize(numberOfModules);
    parts.modulesWithRequireCacheValue.ensureSize(numberOfModules);
    parts.modulesWithNamespaceWord.ensureSize(numberOfModules);
    parts.modulesWhoseBodyIsPassedOver.ensureSize(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::CommonJs)
            parts.modulesWithValue.quickSet(index);
        bool isCommonJsWithOneNamespace = module.kind == ModuleKind::CommonJs && module.flags.contains(ModuleFlags::IsImportedAsNamespace) && !module.flags.containsAny({ ModuleFlags::MayBeReevaluated, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted });
        if (module.kind == ModuleKind::Runtime || module.kind == ModuleKind::RuntimeSource || isCommonJsWithOneNamespace)
            parts.modulesWithNamespaceWord.quickSet(index);
        if (!module.body || !program.at(module.body).hasBody() || bodyOfModuleOnlyReturns(program.at(module.body).body))
            parts.modulesWhoseBodyIsPassedOver.quickSet(index);
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || !request.target)
                continue;
            if (requireReadsModuleValue(program, program.at(request.target)) && requireAddsToRequireCache(program, ModuleID(index), request.target))
                parts.modulesWithRequireCacheValue.quickSet(request.target.index());
        }
    }
    for (const Module& module : program.modules) {
        if (isCommonJSFacade(program, module))
            parts.modulesWithNamespaceWord.quickClear(module.requests[0].target.index());
    }
    parts.modulesWithRequireCacheValue.filter(parts.modulesWithValue);
    parts.modulesWhoseImportsAreNeverMocked = modulesWhoseImportsAreNeverMockedInPlan(program);
}

static BitVector bodiesWithModuleStartInPlan(const Program& program, const ModuleEvaluationPlan& evaluation)
{
    BitVector bodies(program.functions.size());
    for (const ModuleStart& start : evaluation.starts())
        bodies.quickSet(start.body.index());
    return bodies;
}

static LayoutID closureLayoutOfPlan(const Program& program)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Closure)
            return LayoutID(index);
    }
    return { };
}

static BitVector layoutsMadeAsCompiledObjectsInPlan(const Program& program, const StorageOfLayouts& storage)
{
    BitVector layouts(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (storage.holdsCompiledObjectsInUse(LayoutID(index)))
            layouts.quickSet(index);
    }
    return layouts;
}

static BitVector layoutsWithClassRecordInPlan(const Program& program, const Facts& facts)
{
    BitVector layouts(program.layouts.size());
    Vector<LayoutID, 64> toFollow;
    auto add = [&](LayoutID id) {
        if (id && program.at(id).kind == LayoutKind::Instance && !layouts.quickSet(id.index()))
            toFollow.append(id);
    };
    const LinkTables& link = facts.linked();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!link.function(FunctionID(index)).isReached)
            continue;
        for (LayoutID evaluated : facts.summaries().at(FunctionID(index)).classesEvaluated)
            add(evaluated);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        bool isAllocated = !layout.has(LayoutFlags::IsNeverAllocated) && (index >= link.layouts.size() || link.layouts[index].isAllocated);
        if (isAllocated || holdsStateOfCollectionsAlone(layout))
            add(LayoutID(index));
    }
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && link.function(variable.function).isReached)
            add(classOfConstructorFunction(program, variable.function));
    }
    while (!toFollow.isEmpty()) {
        LayoutID id = toFollow.takeLast();
        ClassFacts classFacts = facts.classOf(id);
        add(program.at(id).base);
        add(classFacts.base);
        add(classFacts.prototypeBase);
    }
    return layouts;
}

static BitVector functionsWhoseResultsPassNoReturnInPlan(const Program& program, const Facts& facts, const BitVector& functionsThatEndTheirTailCalls)
{
    const LinkTables& linked = facts.linked();
    BitVector found(program.functions.size());
    if (linked.functions.size() != program.functions.size())
        return found;

    TypeReader types(program);
    Vector<FunctionID> toTell;
    Vector<std::pair<FunctionID, FunctionID>> callersByCallee;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        FunctionID function(number);
        TypeID held = linked.function(function).result;
        if (!held)
            continue;
        bool passesNoReturn = testTellsNoClassOfObjectsIn(types, types.at(held)) || facts.testMentionsWhatIsOutOfReach(types, held, scopedLambda<bool(FunctionID)>([&](FunctionID named) {
            return linked.leadsTo(CodeUnit { function, CodeRole::Body }, named);
        }));
        const BodyIndex* index = facts.bodies().find(function);
        bool endsItsTailCalls = functionsThatEndTheirTailCalls.get(number);
        for (const SummaryCall& call : facts.summaries().at(function).calls) {
            if (passesNoReturn || endsItsTailCalls)
                break;
            if (!call.isInTailPosition)
                continue;
            FunctionID entered = index ? facts.functionThatCallEntersByBinding(*index, index->value(call.site)) : FunctionID();
            TypeID heldByCallee = entered && !program.at(entered).isCoroutine() ? linked.function(entered).result : TypeID();
            passesNoReturn = !heldByCallee || !isSubset(types, types.at(heldByCallee), types.at(held));
            if (!passesNoReturn)
                callersByCallee.append({ entered, function });
        }
        if (passesNoReturn && !found.quickSet(number))
            toTell.append(function);
    }
    std::ranges::sort(callersByCallee);
    while (!toTell.isEmpty()) {
        FunctionID callee = toTell.takeLast();
        for (auto [told, caller] : std::ranges::equal_range(callersByCallee, callee, { }, &std::pair<FunctionID, FunctionID>::first)) {
            if (!found.quickSet(caller.index()))
                toTell.append(caller);
        }
    }
    return found;
}

static BitVector functionsWithClosureCellInPlan(const Program& program, const CallingConventions& conventions)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    BitVector constructors(numberOfFunctions);
    for (const Layout& layout : program.layouts) {
        if (layout.constructor)
            constructors.quickSet(layout.constructor.index());
    }

    BitVector functions(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (!conventions.of(FunctionID(index)).has(CodeRole::GenericCallEntry))
            continue;
        if (program.functions[index].captures.isEmpty() || constructors.quickGet(index))
            functions.quickSet(index);
    }
    return functions;
}

static void addClosureCellsOfFunctionsWithoutCapturesLeft(const Program& program, PlanParts& parts)
{
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        uint32_t numberRemoved = parts.promotedEnvironments.numberOfRemovedCapturesOf(id);
        if (numberRemoved && numberRemoved == program.functions[index].captures.size() && parts.conventions.of(id).has(CodeRole::GenericCallEntry))
            parts.functionsWithClosureCell.quickSet(index);
    }
}

ParentInClassRecord parentNamedByClassRecord(const Program& program, const Facts& facts, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.isDerived())
        return { writtenBaseOf(layout), true };
    ClassFacts classFacts = facts.classOf(id);
    switch (classFacts.baseState) {
    case BaseState::Layout:
        return { classFacts.base, true };
    case BaseState::Unknown:
        return { writtenBaseOf(layout), false };
    case BaseState::None:
        return { classFacts.prototypeBase, false };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isDefinedByClassSyntaxInPlan(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && !layout.has(LayoutFlags::IsConstructorFunction);
}

static bool capturesAtMostEnvironmentOfClassObjectInPlan(const Program& program, const Function& constructor, const Function& member)
{
    if (member.captures.isEmpty())
        return true;
    if (member.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = member.captures[0].written();
    return captured && captured == constructor.captures[0].written() && program.at(captured).environment;
}

static bool classRecordCanCreateMemberInPlan(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || !member.function || member.isAssignedByProgram || program.isIndexLike(member.key) || classHolderIsMadeWithName(program, member.side, member.key))
        return false;
    if (PropertyKeyKind kind = program.at(member.key).kind; kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::RegisteredSymbol)
        return false;
    const Function& function = program.at(member.function);
    return function.name && capturesAtMostEnvironmentOfClassObjectInPlan(program, constructor, function);
}

static bool classAboveIsEvaluatedOnceInPlan(const Program& program, const Facts& facts, LayoutID id)
{
    if (!facts.isEvaluatedAtMostOnce(id))
        return false;
    const Layout& layout = program.at(id);
    return !layout.has(LayoutFlags::IsConstructorFunction) || (layout.constructor && program.at(layout.constructor).captures.isEmpty());
}

static LayoutID provenBaseOfClassInPlan(const Facts& facts, LayoutID id)
{
    ClassFacts classFacts = facts.classOf(id);
    return classFacts.baseState == BaseState::Layout ? classFacts.base : LayoutID();
}

static Vector<ClassCreationPlan> classCreationOfPlan(const Program& program, const Facts& facts)
{
    if (!facts.mechanisms().contains(Mechanism::TypedOperations))
        return { };

    size_t numberOfLayouts = program.layouts.size();
    BitVector classesBelowClassesEvaluatedOnce = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        LayoutID base = provenBaseOfClassInPlan(facts, id);
        if (!base)
            return { true, { } };
        if (!classAboveIsEvaluatedOnceInPlan(program, facts, base))
            return { false, { } };
        return { std::nullopt, base };
    });
    BitVector classesWhoseRowEndsWithoutHeritage = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        if (LayoutID base = provenBaseOfClassInPlan(facts, id))
            return { std::nullopt, base };
        return { !program.at(id).has(LayoutFlags::HasHeritage) && !facts.classOf(id).prototypeBase, { } };
    });

    BitVector classesWhoseGuardsHoldWhileClear = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = program.at(id);
        bool recordNamesEveryMember = std::ranges::all_of(facts.membersOf(id), [](const ClassMember& member) {
            return member.key && !member.isAssignedByProgram;
        });
        if (!isDefinedByClassSyntaxInPlan(layout) || !recordNamesEveryMember)
            return { false, { } };
        ClassFacts classFacts = facts.classOf(id);
        LayoutID parent;
        switch (classFacts.baseState) {
        case BaseState::None:
            return { !layout.has(LayoutFlags::HasHeritage) && !classFacts.prototypeBase, { } };
        case BaseState::Layout:
            parent = classFacts.base;
            break;
        case BaseState::Unknown:
            parent = writtenBaseOf(layout);
            if (!parent)
                return { true, { } };
            break;
        }
        if (!parent)
            return { false, { } };
        return { std::nullopt, parent };
    });

    Vector<ClassCreationPlan> plans(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            continue;
        std::span<const ClassMember> members = facts.membersOf(id);
        ClassCreationPlan& plan = plans[index];
        plan.members.fill(MemberCreation::AtDefinition, members.size());
        if (!isDefinedByClassSyntaxInPlan(layout))
            continue;

        const Function& constructor = program.at(layout.constructor);
        std::array<bool, numberOfEnumerators<ClassSide>> recordCreatesSide;
        recordCreatesSide.fill(true);
        for (size_t member = 0; member < members.size(); ++member) {
            bool& recordCreatesNextMember = recordCreatesSide[static_cast<unsigned>(members[member].side)];
            recordCreatesNextMember = recordCreatesNextMember && classRecordCanCreateMemberInPlan(program, constructor, members[member]);
            if (recordCreatesNextMember)
                plan.members[member] = MemberCreation::ByClassRecord;
        }

        bool basesAreEvaluatedOnce = classesBelowClassesEvaluatedOnce.quickGet(index);
        plan.createsMembersLazily = facts.isEvaluatedAtMostOnce(id) && recordCreatesSide[static_cast<unsigned>(ClassSide::Prototype)];
        plan.createsPrototypeBeforeClassObject = plan.createsMembersLazily && basesAreEvaluatedOnce && classesWhoseRowEndsWithoutHeritage.quickGet(index);
        plan.createsClassObjectOnMutation = plan.createsMembersLazily && basesAreEvaluatedOnce && recordCreatesSide[static_cast<unsigned>(ClassSide::Static)];
        plan.guardsHoldWhileClear = classesWhoseGuardsHoldWhileClear.quickGet(index);
        plan.guardsOfPrototypeSideHoldWhileClear = plan.guardsHoldWhileClear;
    }
    return plans;
}

static StoredGlobalValue storedValueOfNode(const Program& program, const LinkTables& link, FunctionID function, const ThinSummary& summary, const SummaryNode& value)
{
    StoredGlobalValue stored;
    auto takeType = [&](TypeID type) {
        if (!type || holdsOnlyPrimitives(type) || !program.at(type).hasObjects() || program.at(type).hasUnboundedUnknown())
            return;
        if (FunctionID only = singleFunctionOf(TypeReader(program), program.at(type)))
            stored.function = only;
        else if (LayoutID only = singleLayoutOf(TypeReader(program), program.at(type)))
            stored.layout = only;
        else
            stored.type = type;
    };
    switch (value.kind) {
    case SummaryNode::Kind::Allocation:
        stored.layout = LayoutID(value.first);
        break;
    case SummaryNode::Kind::ClassInstance:
        stored.builtinClass = BuiltinClassID(value.first);
        break;
    case SummaryNode::Kind::Closure:
        stored.function = FunctionID(value.first);
        break;
    case SummaryNode::Kind::Constant:
        takeType(TypeID::fromBits(value.first));
        break;
    case SummaryNode::Kind::Variable:
        takeType(link.variable(VariableID(value.first)).proven);
        break;
    case SummaryNode::Kind::ResultOfCall: {
        const SummaryCall& call = summary.calls[value.first];
        FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : link.calleeAt(function, call.site);
        if (callee && call.isConstruction)
            stored.layout = program.at(callee).constructedClass;
        break;
    }
    default:
        break;
    }
    return stored;
}

static std::optional<StoredGlobalValue> findStoredValue(const Program& program, const LinkTables& link, FunctionID function, const ThinSummary& summary, const SummaryAccess& store, uint32_t globalObject)
{
    static constexpr unsigned mostArrivals = 4;
    std::optional<StoredGlobalValue> found;
    bool isUnknown = false;
    unsigned numberOfArrivals = 0;
    auto take = [&](uint32_t node) {
        const SummaryNode& value = summary.nodes[node];
        if (value.kind == SummaryNode::Kind::Constant && holdsOnlyPrimitives(TypeID::fromBits(value.first)))
            return;
        if (value.kind == SummaryNode::Kind::ResultOfAccess && summary.accesses[value.first].key == store.key && isGlobalObject(program, link, summary, summary.accesses[value.first].subject, globalObject))
            return;
        StoredGlobalValue stored = storedValueOfNode(program, link, function, summary, value);
        isUnknown |= stored.isUnknown() || (found && !found->isSameAs(stored));
        found = stored;
    };
    if (summary.nodes[store.value].kind != SummaryNode::Kind::Join)
        take(store.value);
    else {
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != store.value)
                continue;
            if (++numberOfArrivals > mostArrivals || summary.nodes[edge.from].kind == SummaryNode::Kind::Join)
                return StoredGlobalValue { };
            take(edge.from);
        }
    }
    if (isUnknown)
        return StoredGlobalValue { };
    return found;
}

static Vector<StoredGlobalValue> agreedGlobalValues(Vector<StoredGlobalValue>&& stored)
{
    std::ranges::stable_sort(stored, { }, [](const StoredGlobalValue& value) { return value.key.index(); });
    Vector<StoredGlobalValue> only;
    for (size_t place = 0; place < stored.size();) {
        size_t end = place + 1;
        bool agrees = !stored[place].isUnknown();
        for (; end < stored.size() && stored[end].key == stored[place].key; ++end)
            agrees &= stored[end].isSameAs(stored[place]);
        if (agrees)
            only.append(stored[place]);
        place = end;
    }
    return only;
}

static BitVector globalNamesNeverStoredInPlan(const Program& program, const Facts& facts, Vector<StoredGlobalValue>& stored)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    BitVector neverStored(numberOfKeys);
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (program.types.hasStableNumber(PropertyKeyID(index)))
            neverStored.quickSet(index);
    }
    uint32_t globalObject = program.builtins().canonicalID(Builtins::GlobalThis).index();
    const ThinSummaries& summaries = facts.summaries();
    const LinkTables& link = facts.linked();
    auto readStore = [&](FunctionID function, const ThinSummary& summary, const SummaryAccess& store) {
        if (store.key.index() >= numberOfKeys)
            return;
        const SummaryNode& subject = summary.nodes[store.subject];
        if (subject.kind == SummaryNode::Kind::Builtin)
            neverStored.quickClear(store.key.index());
        if (store.isToContents() || store.value == noSummaryNode || !program.types.hasStableNumber(store.key))
            return;
        if (std::optional<StoredGlobalValue> value = findStoredValue(program, link, function, summary, store, globalObject)) {
            value->key = store.key;
            stored.append(*value);
        }
    };
    if (link.listsGlobalStores) {
        for (const LinkedGlobalStore& store : link.globalStores) {
            const ThinSummary& summary = summaries.at(store.function);
            readStore(store.function, summary, summary.accesses[store.access]);
        }
        return neverStored;
    }
    for (uint32_t function = 0; function < summaries.size(); ++function) {
        const ThinSummary& summary = summaries.at(FunctionID(function));
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind == SummaryAccess::Kind::Store && access.key && isGlobalObject(program, link, summary, access.subject, globalObject))
                readStore(FunctionID(function), summary, access);
        }
    }
    return neverStored;
}

static BitVector namesOfFieldsWrittenAsOneFunctionInPlan(const Program& program)
{
    BitVector names(program.types.numberOfPropertyKeys());
    for (const Layout& layout : program.layouts) {
        for (const Field& field : layout.fields) {
            if (field.key && field.declared.isExpected() && field.declared.written() && singleFunctionOf(TypeReader(program), program.at(field.declared.written())))
                names.quickSet(field.key.index());
        }
    }
    return names;
}

static void planWordsOfModulesAndGlobalNames(const Program& program, const Facts& facts, BuildContext& build, PlanParts& parts, const BitVector& functionsThatEndTheirTailCalls)
{
    chooseModuleWordsOfPlan(program, parts);
    parts.namesOfFieldsWrittenAsOneFunction = namesOfFieldsWrittenAsOneFunctionInPlan(program);
    Vector<StoredGlobalValue> storedGlobalValues;
    parts.globalNamesNeverStoredByName = globalNamesNeverStoredInPlan(program, facts, storedGlobalValues);
    parts.storedGlobalValues = agreedGlobalValues(WTF::move(storedGlobalValues));
    parts.globalNamesNeverStoredThroughBinding = parts.globalNamesNeverStoredByName;
    parts.functionsWhoseResultsPassNoReturn = functionsWhoseResultsPassNoReturnInPlan(program, facts, functionsThatEndTheirTailCalls);
    parts.variablesThatRunTimeMayFill = variablesThatRunTimeMayFill(program);
}

static void planShapes(Program& program, const Facts& facts, const StorageOfLayouts& storage, PlanParts& parts)
{
    Vector<uint32_t> orderOfLayoutsForShapes = orderOfLayoutsForShapeNumbers(program, facts, storage);
    parts.shapes = computeShapeTable(program, facts.linked(), storage, parts.closureLayout, orderOfLayoutsForShapes.span());
}

static void planConventions(const Program& program, const Facts& facts, BuildContext& build, const StorageOfLayouts& storage, PlanParts& parts)
{
    bool everyFunctionCanBeCounted = true;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        size_t numberOfParameters = program.functions[index].numberOfEntryParameters();
        if (numberOfParameters <= std::numeric_limits<decltype(FunctionConvention::numberOfEntryParameters)>::max())
            continue;
        build.reportLimitExceeded(LimitID::MaxReservedStackArguments, numberOfParameters, Subject { .function = FunctionID(index) });
        everyFunctionCanBeCounted = false;
    }
    if (!everyFunctionCanBeCounted)
        return;

    parts.conventions = computeCallingConventions(program, facts, layoutsMadeAsCompiledObjectsInPlan(program, storage), build.parallelLoops());
    parts.functionsWithClosureCell = functionsWithClosureCellInPlan(program, parts.conventions);
    uint64_t numberOfAsyncFunctionsThatNeverSuspend = 0;
    uint64_t numberOfFunctionsThatReturnNumberOrUndefinedRaw = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const FunctionConvention& convention = parts.conventions.of(FunctionID(index));
        numberOfAsyncFunctionsThatNeverSuspend += convention.coroutineNeverSuspends;
        if (!convention.has(CodeRole::Body) || convention.numberOfResults != 1)
            continue;
        Representation result = parts.conventions.resultRepresentation({ FunctionID(index), CodeRole::Body });
        numberOfFunctionsThatReturnNumberOrUndefinedRaw += result == Representation::OptionalInt64 || result == Representation::OptionalDouble;
    }
    build.statistics().count(StatisticID::NonSuspendingAsyncFunctions, numberOfAsyncFunctionsThatNeverSuspend);
    build.statistics().count(StatisticID::NumbersOrUndefinedReturnedRaw, numberOfFunctionsThatReturnNumberOrUndefinedRaw);

}

static bool makesObjectsWithBirthsInPlan(const Program& program, const LinkTables& held, const SlotPlanParts& slots, uint32_t layout)
{
    LayoutKind kind = program.layouts[layout].kind;
    bool isRecordThatStaysAsBorn = kind == LayoutKind::Record && !held.layouts[layout].isDefinedOnByDescriptorsThatLayValuesAlone && !held.layouts[layout].possibleChanges.containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor });
    return (kind == LayoutKind::Instance || isRecordThatStaysAsBorn) && slots.layouts[layout].holdsCompiledObjects() && !slots.layoutsNeverAllocated.get(layout);
}

static bool isFilledByInitializersAloneInPlan(const Program& program, const LinkTables& held, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Instance || held.layout(id).possibleChanges.containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor }))
        return false;
    for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
        if (held.field({ id, field }).isOfBirth)
            return false;
    }
    return true;
}

static bool slotMayBeOfBirthInPlan(const PlannedLayout& planned, FieldIndex field)
{
    const PlacedSlot& slot = planned.slots[field];
    return slot.storage == SlotStorage::Inline && slot.indexInLayoutRecord && canLeaveSlot(slot.representation) && !slot.holdsAccessor && !slot.mayExistWithoutStorage
        && slot.representation != SlotRepresentation::DeferredClosure && !slot.deferredFunction && planned.environmentOfDeferredClosures != field;
}

static void markSlotsFilledAtBirthInPlan(Program& program, const Facts& facts, const ParallelLoops& loops, Statistics& statistics, SlotPlanParts& slots)
{
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots) || program.admitsInspector)
        return;
    const LinkTables& linked = facts.linked();
    const LinkTables& held = facts.held();
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    RELEASE_ASSERT(held.firstFieldOfLayout.size() == numberOfLayouts + 1 && linked.firstFieldOfLayout.size() == numberOfLayouts + 1 && slots.layouts.size() == numberOfLayouts);

    auto declarationOfField = [&](FieldReference reference) {
        FieldReference declaration = declarationOf(program, reference);
        return held.firstFieldOfLayout[declaration.layout.index()] + declaration.field;
    };
    static constexpr uint8_t isLeftOutBySomeLayout = 1 << 0;
    static constexpr uint8_t isLookedAtInSomeLayout = 1 << 1;
    static constexpr uint8_t mayHoldMarkInSomeLayout = 1 << 2;
    Vector<uint8_t> saidOfDeclarations;
    saidOfDeclarations.fill(0, held.fields.size());
    auto say = [&](uint32_t declaration, uint8_t what) {
        AtomicRef(saidOfDeclarations[declaration]).fetch_or(what, std::memory_order_relaxed);
    };
    auto hasSeveralParents = [&](LayoutID id) {
        size_t numberOfClassesLeft = numberOfLayouts;
        while (program.at(id).base && numberOfClassesLeft--)
            id = program.at(id).base;
        bool hasParents = false;
        forEachOfSeveralParents(program, id, [&](LayoutID) { hasParents = true; });
        return hasParents;
    };
    BitVector standsInRowOfClasses(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        forEachOfSeveralParents(program, LayoutID(index), [&](LayoutID parent) {
            standsInRowOfClasses.quickSet(index);
            standsInRowOfClasses.quickSet(parent.index());
        });
        if (!layout.base || layout.isDerived())
            continue;
        standsInRowOfClasses.quickSet(index);
        standsInRowOfClasses.quickSet(declaredLayoutOf(program, layout.base).index());
    }
    static constexpr uint32_t layoutsInRange = 256;
    forEachIndexInParallel(loops, (numberOfLayouts + layoutsInRange - 1) / layoutsInRange, [&](unsigned range) {
        uint32_t endOfRange = std::min(numberOfLayouts, (range + 1) * layoutsInRange);
        for (uint32_t index = range * layoutsInRange; index < endOfRange; ++index) {
            if (!makesObjectsWithBirthsInPlan(program, held, slots, index))
                continue;
            LayoutID id(index);
            bool isReadWithoutLook = !linked.layout(id).reachesUnseenCode;
            bool waitsForInitializer = isFilledByInitializersAloneInPlan(program, held, id);
            bool isFilledByParents = !hasSeveralParents(id);
            for (FieldIndex field = 0; field < program.layouts[index].fields.size(); ++field) {
                uint32_t declaration = declarationOfField({ id, field });
                const Field& declared = program.layouts[index].fields[field];
                bool isOfBirth = held.field({ id, field }).isOfBirth || (isFilledByParents && declared.flags.contains(FieldFlags::IsInherited) && held.fields[declaration].isOfBirth)
                    || (waitsForInitializer && declared.key && declared.form.isEmpty() && linked.fieldsFilledByEveryInitializer.names(program, id, declared.key));
                bool gainsNamesBySpread = (held.recordsThatSpreadsFill.get(index) || (program.layouts[index].kind == LayoutKind::Record && held.layout(id).isFilledUnderComputedKeys)) && held.layout(id).possibleChanges.contains(LayoutChange::GainsProperty);
                if (!isOfBirth || gainsNamesBySpread || !slotMayBeOfBirthInPlan(slots.layouts[index], field) || standsInRowOfClasses.quickGet(declaredLayoutOf(program, id).index()))
                    say(declaration, isLeftOutBySomeLayout);
                else if (!isReadWithoutLook || !linked.field({ id, field }).isFilled)
                    say(declaration, isLookedAtInSomeLayout);
                const PlacedSlot& slot = slots.layouts[index].slots[field];
                const Field& record = program.layouts[index].fields[field];
                const Field& origin = record.originLayout && record.originField ? fieldAt(program, { record.originLayout, *record.originField }) : record;
                TypeID written = origin.declared.written();
                LinkedCell cell = held.field({ id, field });
                FieldReference declaringField = declarationOf(program, { id, field });
                bool isPosition = declared.key && linked.fieldsOfPositions.contains(declaringField.layout, declared.key);
                bool isFlag = declared.key && linked.fieldsStoredBooleansAlone.contains(declaringField.layout, declared.key);
                bool holdsPositions = slot.type && ((program.types.at(slot.type).isInt32OrNullish() && (isPosition || held.holdsSteppedIntegers({ id, field }))) || isFlag);
                bool isWrittenWithoutNullish = written && !program.types.at(written).isAnything() && !program.types.at(written).mayBeUndefinedOrNull();
                bool isExpectedWithoutNullish = written && origin.declared.isExpected() && origin.declared.writtenFlags().contains(PositionFlags::IsFromCompiler) && program.layouts[index].kind == LayoutKind::Instance && program.layouts[index].constructor && !isLibraryCode(program, program.at(program.layouts[index].constructor))
                    && declared.key && !linked.fieldsStoredNullish.names(program, declaringField.layout, declared.key);
                if (!traitsOf(slot.representation).hasUndefined || !slot.type || (!holdsPositions && !isWrittenWithoutNullish && !isExpectedWithoutNullish)
                    || cell.isOnlyGivenUndefinedByMakers || cell.isLeftUndefinedByMakers || cell.isGivenByNoMaker || cell.isGivenUndefinedOrNullInSight || held.recordsThatSpreadsFill.get(index)
                    || (slot.provenType && program.types.at(slot.provenType).mayBeUndefinedOrNull()))
                    say(declaration, mayHoldMarkInSomeLayout);
            }
        }
    });

    Vector<Vector<FieldReference>> fieldsWithRawValuesInRanges;
    fieldsWithRawValuesInRanges.grow((numberOfLayouts + layoutsInRange - 1) / layoutsInRange);
    forEachIndexInParallel(loops, static_cast<unsigned>(fieldsWithRawValuesInRanges.size()), [&](unsigned range) {
        uint32_t endOfRange = std::min(numberOfLayouts, (range + 1) * layoutsInRange);
        for (uint32_t index = range * layoutsInRange; index < endOfRange; ++index) {
            if (!makesObjectsWithBirthsInPlan(program, held, slots, index))
                continue;
            for (FieldIndex field = 0; field < program.layouts[index].fields.size(); ++field) {
                uint32_t declaration = declarationOfField({ LayoutID(index), field });
                uint8_t said = saidOfDeclarations[declaration];
                slots.layouts[index].slots[field].isFilledAtBirth = (said & (isLookedAtInSomeLayout | isLeftOutBySomeLayout)) == isLookedAtInSomeLayout;
                slots.layouts[index].mayBearMark |= slots.layouts[index].slots[field].isFilledAtBirth;
                const Field& declared = program.layouts[index].fields[field];
                slots.layouts[index].isFilledByInitializer |= slots.layouts[index].slots[field].isFilledAtBirth && !declared.flags.contains(FieldFlags::IsInherited) && declared.key && linked.fieldsFilledByEveryInitializer.names(program, LayoutID(index), declared.key) && isFilledByInitializersAloneInPlan(program, held, LayoutID(index));
                slots.layouts[index].marksFrozenObjects = slots.layouts[index].mayBearMark && !held.layout(LayoutID(index)).possibleChanges.contains(LayoutChange::IsFrozen);
                slots.layouts[index].marksOtherPrototypes = slots.layouts[index].mayBearMark && !held.layout(LayoutID(index)).possibleChanges.contains(LayoutChange::ChangesPrototype);
                if (slots.layouts[index].slots[field].isFilledAtBirth && !(said & mayHoldMarkInSomeLayout)) {
                    slots.layouts[index].fieldsThatHoldRawValuesFromBirth.set(field);
                    fieldsWithRawValuesInRanges[range].append({ LayoutID(index), field });
                }
            }
        }
    });

    uint64_t numberOfFieldsInHierarchies = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (program.layouts[index].base && !program.layouts[index].isDerived() && slots.layouts[index].mayBearMark)
            numberOfFieldsInHierarchies += std::ranges::count_if(slots.layouts[index].slots, &PlacedSlot::isFilledAtBirth);
    }
    statistics.count(StatisticID::HierarchyFieldsFilledAtBirth, numberOfFieldsInHierarchies);

    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (const Vector<FieldReference>& fields : fieldsWithRawValuesInRanges) {
        for (FieldReference reference : fields) {
            PlacedSlot& slot = slots.layouts[reference.layout.index()].slots[reference.field];
            slot.typeOfRawValue = types.withoutNullish(slot.typeInLayoutRecord());
        }
    }
}

std::unique_ptr<Plan> planProgram(Program& program, const Facts& facts, BuildContext& build)
{
    StepTimes times(build, nameOf(Stage::PlanSlots));
    auto finish = [&](ASCIILiteral step) {
        times.didFinish(step);
        return !build.diagnostics().hasErrors();
    };
    auto runLane = [&](ASCIILiteral step, const auto& lane) {
        StepTimes timesOfLane(build, nameOf(Stage::PlanSlots));
        lane();
        timesOfLane.didFinish(step);
    };

    auto hasFailed = [&] {
        return build.diagnostics().hasErrors();
    };

    PlanParts parts;
    parts.usesWideReferences = build.recordedOptions().useWideReferences();
    parts.hasOneInstance = facts.hasOneInstance();
    parts.closureLayout = closureLayoutOfPlan(program);
    const StorageOfLayouts storage = chooseStorageOfLayouts(program, facts, build.parallelLoops());
    FunctionsThatEndTheirTailCalls functionsThatEndTheirTailCalls;
    StackTestPlan stackTests;
    auto planSlotsAndKeyOrders = [&] {
        SlotPlanParts slots;
        Vector<PlannedVariable> variables;
        Vector<ClassCreationPlan> classCreation;
        runLanes(build.parallelLoops(), {
            [&] { runLane("plan: lane of slots"_s, [&] { slots = computeSlotPlanParts(program, facts, build, storage, parts.keyOrders, parts.promotedEnvironments); }); },
            [&] { runLane("plan: lane of variables, class creation"_s, [&] {
                const BitVector variablesDescribedToRunTime = variablesDescribedToRunTimeByPlan(program);
                parts.deferredDefinitions = deferredDefinitionsOfPlan(program, facts, variablesDescribedToRunTime, AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime));
                parts.heldRegExps = HeldRegExps(program, facts);
                variables = variablesOfPlan(program, facts, variablesDescribedToRunTime);
                classCreation = classCreationOfPlan(program, facts);
            }); },
        });
        if (hasFailed())
            return;
        slots.variables = WTF::move(variables);
        slots.classCreation = WTF::move(classCreation);
        build.statistics().count(StatisticID::EnvironmentsPromotedToVariables, parts.promotedEnvironments.layouts.size());
        build.statistics().count(StatisticID::PermanentClassCapturesPromoted, parts.promotedEnvironments.numberOfCapturesOfClassesStoredForGood);
        holdDoorsToProofsInSlotPlan(program, facts, build.parallelLoops(), slots);
        appendPromotedVariables(parts.promotedEnvironments, slots);
        markSlotsFilledAtBirthInPlan(program, facts, build.parallelLoops(), build.statistics(), slots);
        markFieldsHeldFilledInSlotPlan(program, facts.linked(), build.parallelLoops(), slots);
        parts.slots = SlotPlan(WTF::move(slots));
    };
    auto planConventionsAndStackTests = [&] {
        bool plansStackTests = facts.mechanisms().contains(Mechanism::DirectCalls);
        MethodTablePlan methodTable;
        CallsCountedForStackTests callsCounted;
        runLanes(build.parallelLoops(), {
            [&] { runLane("plan: lane of module evaluation"_s, [&] { parts.moduleEvaluation = planModuleEvaluation(program, facts, build.parallelLoops(), build.statistics()); }); },
            [&] { runLane("plan: lane of conventions, method table, cycles of calls"_s, [&] {
                planConventions(program, facts, build, storage, parts);
                if (hasFailed())
                    return;
                runLanes(build.parallelLoops(), {
                    [&] {
                        parts.layoutsWithClassRecord = layoutsWithClassRecordInPlan(program, facts);
                        methodTable = planMethodTable(program, facts, parts.conventions, storage, parts.layoutsWithClassRecord);
                        static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) < 1ull << 24);
                        if (build.exceeds(LimitID::ImageFunctions, methodTable.numberOfUnits))
                            build.reportLimitExceeded(LimitID::ImageFunctions, methodTable.numberOfUnits);
                    },
                    [&] {
                        if (plansStackTests)
                            callsCounted = findCallsCountedForStackTests(program, facts, parts.conventions, build.parallelLoops());
                    },
                });
            }); },
        });
        if (hasFailed())
            return;
        parts.conventions.setMethodTable(WTF::move(methodTable));
        parts.conventions.setModuleStarts(program, bodiesWithModuleStartInPlan(program, parts.moduleEvaluation));
        if (plansStackTests)
            stackTests = planStackTests(program, facts, parts.conventions, parts.moduleEvaluation, build.parallelLoops(), WTF::move(callsCounted));
    };
    runLanes(build.parallelLoops(), {
        [&] { runLane("plan: lane of slots, key orders"_s, planSlotsAndKeyOrders); },
        [&] { runLane("plan: lane of modules, conventions, method table, stack tests"_s, planConventionsAndStackTests); },
        [&] { runLane("plan: lane of shapes"_s, [&] { planShapes(program, facts, storage, parts); }); },
        [&] { runLane("plan: lane of tail calls that end, words of modules, names"_s, [&] {
            if (facts.mechanisms().contains(Mechanism::DirectCalls))
                functionsThatEndTheirTailCalls = findFunctionsThatEndTheirTailCalls(program, facts);
            planWordsOfModulesAndGlobalNames(program, facts, build, parts, functionsThatEndTheirTailCalls.whateverArrives);
        }); },
    });
    if (!finish("plan: slots, key orders beside conventions, stack tests, shapes"_s))
        return nullptr;

    parts.conventions.setRemovedCaptures(program, parts.promotedEnvironments);
    addClosureCellsOfFunctionsWithoutCapturesLeft(program, parts);
    std::unique_ptr<Plan> plan = makeUnique<Plan>(WTF::move(parts));
    ModulesThatWriteFields modulesThatWriteFields;
    Vector<ParameterGuardPlan> parameterGuards;
    runLanes(build.parallelLoops(), {
        [&] { parameterGuards = planParameterGuards(program, facts, *plan, build); },
        [&] { runLane("plan: lane of modules that write fields"_s, [&] { modulesThatWriteFields = findModulesThatWriteFields(program, facts, plan->slots(), build.parallelLoops()); }); },
    });
    for (const Layout& layout : program.layouts) {
        for (const Field& field : layout.fields) {
            if (field.flags.contains(FieldFlags::MayBeDefinedAbove) && layout.module)
                modulesThatWriteFields.namesDefinedAbove.add(SlotPlan::nameInModule(layout.module, field.key));
        }
    }
    plan->setModulesThatWriteFields(WTF::move(modulesThatWriteFields));
    plan->setParameterGuards(parameterGuards.span());
    plan->setScalarizedResults(planScalarizedResults(program, facts, *plan));
    plan->setScalarizedParameters(program, planScalarizedParameters(program, facts, *plan));
    plan->setFunctionsThatEndTheirTailCalls(WTF::move(functionsThatEndTheirTailCalls));
    planTailCallsAndStackTests(program, facts, *plan, WTF::move(stackTests));
    finishMethodTableForUnions(program, facts, *plan);
    plan->fuseCallEntriesThatOnlyMove(program, facts, build.parallelLoops());
    plan->findCallEntriesThatWalkBodies(program, facts);
    if (!finish("plan: parameter guards"_s))
        return nullptr;
    return plan;
}

static bool allocationIsBornUnfinishedInPlan(const PlannedLayout& planned, std::span<const FieldIndex> initialFields)
{
    unsigned numberOfFields = planned.numberOfFields();
    BitVector isInitialField(numberOfFields);
    for (FieldIndex field : initialFields) {
        RELEASE_ASSERT(field < numberOfFields);
        isInitialField.quickSet(field);
    }
    for (FieldIndex field = 0; field < numberOfFields; ++field) {
        if (planned.slots[field].isFilledAtBirth && !isInitialField.quickGet(field))
            return true;
    }
    return false;
}

std::optional<PlacedAllocation> Plan::allocation(const Program& program, LayoutID id, std::span<const FieldIndex> initialFields) const
{
    const PlannedLayout& planned = layout(id);
    uint32_t shape = shapes().generalShapeOf(id);
    if (!planned.holdsCompiledObjects() || !shapes().isAssigned(shape))
        return std::nullopt;

    PlacedAllocation placed;
    placed.layout = id;
    placed.shape = shape;
    placed.size = planned.size;
    const Layout& declared = program.at(declaredLayoutOf(program, id));
    if (declared.kind == LayoutKind::Instance)
        placed.constructedBy = declared.constructor;
    placed.isBornUnfinished = allocationIsBornUnfinishedInPlan(planned, initialFields);

    Vector<FieldIndex, 16> fieldsWithStorage;
    for (FieldIndex field : initialFields) {
        if (planned.slots[field].hasStorage() && planned.environmentOfDeferredClosures != field)
            fieldsWithStorage.append(field);
    }
    std::optional<uint32_t> orderWord = keyOrders().findOrderWord(id, fieldsWithStorage.span());
    if (!orderWord)
        return std::nullopt;
    placed.orderWord = *orderWord;
    return placed;
}

PlacedAllocation Plan::environmentAllocation(LayoutID id) const
{
    PlacedAllocation placed;
    placed.layout = id;
    placed.shape = shapes().generalShapeOf(id);
    placed.size = layout(id).size;
    return placed;
}

PlacedAllocation Plan::closureAllocation(unsigned numberOfCaptures) const
{
    RELEASE_ASSERT(numberOfCaptures <= AOT::Closure::maxNumberOfCaptures);
    unsigned roomForCaptures = numberOfCaptures | 1;
    PlacedAllocation placed;
    placed.layout = m_parts.closureLayout;
    placed.shape = shapes().generalShapeOf(m_parts.closureLayout);
    placed.size = static_cast<uint32_t>(AOT::Closure::allocationSize(roomForCaptures));
    placed.numberOfCaptures = static_cast<uint16_t>(roomForCaptures);
    return placed;
}

namespace {

class CollectorOfUnitsNamedOutsideCode {
    WTF_MAKE_NONCOPYABLE(CollectorOfUnitsNamedOutsideCode);
public:
    CollectorOfUnitsNamedOutsideCode(const Program& program, const Facts& facts, const Plan& plan)
        : m_program(program)
        , m_facts(facts)
        , m_plan(plan)
        , m_conventions(plan.conventions())
        , m_functionsThatHostEnters(functionsCalledByHost(program))
        , m_visitedClasses(program.layouts.size())
    {
    }

    Vector<CodeUnit> collect();

private:
    void add(CodeUnit);
    void addClosureOf(FunctionID);
    void addFunctionsNamedByClassRecords(LayoutID classLayout);

    CodeUnit unitThatLoaderEnters(ModuleID) const;
    void addModulesThatLoaderEnters();
    void addBodiesThatStartsOfModulesCall();
    void addFunctionsThatHostEnters();
    void addInitialFunctionsOfVariables();
    void addFunctionsWithoutReferrers();
    void addFunctionsThatHoldInt32Promises();

    const Program& m_program;
    const Facts& m_facts;
    const Plan& m_plan;
    const CallingConventions& m_conventions;
    BitVector m_functionsThatHostEnters;
    BitVector m_visitedClasses;
    Vector<CodeUnit> m_units;
};

void CollectorOfUnitsNamedOutsideCode::add(CodeUnit unit)
{
    if (m_conventions.exists(unit))
        m_units.append(unit);
}

void CollectorOfUnitsNamedOutsideCode::addClosureOf(FunctionID function)
{
    if (!function)
        return;
    CodeUnit entry = m_conventions.closureEntryOf(function);
    if (!m_conventions.exists(entry))
        return;
    m_units.append(entry);
    if (CodeUnit constructEntry = m_conventions.genericEntry(entry).constructEntry)
        add(constructEntry);
    addFunctionsNamedByClassRecords(m_program.at(function).constructedClass);
}

void CollectorOfUnitsNamedOutsideCode::addFunctionsNamedByClassRecords(LayoutID classLayout)
{
    LayoutID current = classLayout;
    while (current && m_plan.hasClassRecord(current) && !m_visitedClasses.set(current.index())) {
        addClosureOf(m_program.at(current).constructor);
        std::span<const ClassMember> members = m_facts.membersOf(current);
        const Vector<MemberCreation>& creations = m_plan.slots().classCreation(current).members;
        for (size_t index = 0; index < members.size() && index < creations.size(); ++index) {
            switch (creations[index]) {
            case MemberCreation::AtDefinition:
                break;
            case MemberCreation::ByClassRecord:
                addClosureOf(members[index].function);
                break;
            }
        }
        ClassFacts classFacts = m_facts.classOf(current);
        current = classFacts.baseState == BaseState::Layout ? classFacts.base : LayoutID();
    }
}

CodeUnit CollectorOfUnitsNamedOutsideCode::unitThatLoaderEnters(ModuleID id) const
{
    const ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    if (evaluation.isEvaluatedByCodeAlone(id))
        return { };
    FunctionID body = m_program.at(id).body;
    if (evaluation.findStart(id))
        return { body, CodeRole::ModuleStart };
    if (m_plan.moduleBodyIsEntered(id))
        return { body, CodeRole::Body };
    return { };
}

void CollectorOfUnitsNamedOutsideCode::addModulesThatLoaderEnters()
{
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        FunctionID body = m_program.modules[index].body;
        if (!body)
            continue;
        CodeUnit entered = unitThatLoaderEnters(ModuleID(index));
        if (!entered || !m_facts.mayBeReached(body)) {
            m_functionsThatHostEnters.clear(body.index());
            continue;
        }
        if (entered.role != CodeRole::ModuleStart)
            add(entered);
    }
}

void CollectorOfUnitsNamedOutsideCode::addBodiesThatStartsOfModulesCall()
{
    const ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    for (const ModuleStart& start : evaluation.starts()) {
        if (!m_conventions.exists({ start.body, CodeRole::ModuleStart }))
            continue;
        for (const ModuleStartOperation& operation : evaluation.operationsOf(start)) {
            if (operation.kind == ModuleStartOperation::Kind::CallBody)
                add(m_conventions.directEntryOf(operation.callee));
        }
    }
}

void CollectorOfUnitsNamedOutsideCode::addFunctionsWithoutReferrers()
{
    const LargeList<LinkedFunction>& functions = m_facts.linked().functions;
    for (uint32_t index = 0; index < functions.size(); ++index) {
        if (!functions[index].isInQueueFromStart)
            continue;
        for (unsigned role = 0; role < numberOfCodeRolesOfOneFunction; ++role) {
            if (CodeUnit unit { FunctionID(index), static_cast<CodeRole>(role) }; m_conventions.isSeedOfFunctionInQueueFromStart(unit))
                add(unit);
        }
    }
}

void CollectorOfUnitsNamedOutsideCode::addFunctionsThatHoldInt32Promises()
{
    const LargeList<LinkedFunction>& functions = m_facts.linked().functions;
    for (uint32_t index = 0; index < functions.size(); ++index) {
        if (functions[index].isReached && functions[index].holdsInt32Promise)
            addClosureOf(FunctionID(index));
    }
}

void CollectorOfUnitsNamedOutsideCode::addFunctionsThatHostEnters()
{
    const LargeList<LinkedFunction>& functions = m_facts.linked().functions;
    FunctionID sorts = functionThatSorts(m_program);
    if (sorts && sorts.index() < functions.size() && !functions[sorts.index()].everyCallerIsKnown && m_facts.mayBeReached(sorts))
        addClosureOf(sorts);
    for (size_t index : m_functionsThatHostEnters) {
        FunctionID function(index);
        if (m_facts.mayBeReached(function))
            addClosureOf(function);
    }
}

void CollectorOfUnitsNamedOutsideCode::addInitialFunctionsOfVariables()
{
    unsigned numberOfVariables = m_plan.slots().numberOfVariables();
    for (unsigned index = 0; index < numberOfVariables; ++index) {
        PlannedVariable variable = m_plan.variable(VariableID(index));
        if (variable.isDescribedToRunTime)
            addClosureOf(variable.function);
    }
}

Vector<CodeUnit> CollectorOfUnitsNamedOutsideCode::collect()
{
    addModulesThatLoaderEnters();
    addBodiesThatStartsOfModulesCall();
    addFunctionsThatHostEnters();
    addInitialFunctionsOfVariables();
    addFunctionsWithoutReferrers();
    addFunctionsThatHoldInt32Promises();

    AOT::sort(m_units);
    removeRepeatedElements(m_units);
    return WTF::move(m_units);
}

} // anonymous namespace

Vector<CodeUnit> unitsNamedOutsideCode(const Program& program, const Facts& facts, const Plan& plan)
{
    return CollectorOfUnitsNamedOutsideCode(program, facts, plan).collect();
}

#define FOR_EACH_AOT_PLAN_RULE(v) \
    v(WideReferencesOfBuild, "the plan and the options of the build disagree on wide references") \
    v(NumberOfLayouts, "the plan does not have one layout for each layout of the program") \
    v(NumberOfVariables, "the plan does not have one variable for each variable of the program and each added one") \
    v(NumberOfSlots, "the layout does not have one slot for each declared field, each appended field and each hidden word") \
    v(CellSize, "the size of the cell is no multiple of 8") \
    v(SlotAlignment, "a slot is not aligned to its width") \
    v(SlotOutsideCell, "a slot lies in front of the first field or past the end of the cell") \
    v(SlotOutsideExtension, "a slot lies in front of the first granule or past the end of the Extension") \
    v(SlotsOverlap, "a slot shares bytes with another slot of the layout") \
    v(StorageMissing, "cells of the layout may be made, and a field has no storage") \
    v(ShapeMissing, "cells of the layout may be made, and the layout has no shape") \
    v(ShapeOfOtherLayout, "a shape of the layout names another layout, or a shape lies outside the shapes of its layout") \
    v(FirstEnvironmentShape, "the first shape of environments is no shape") \
    v(SideOfShape, "the shape of an object does not lie below the first shape of environments, or another shape does") \
    v(OrdersWithoutObjects, "the layout holds no compiled objects and has key orders") \
    v(NumberOfKeyOrders, "the layout has more key orders than an order word can name") \
    v(FieldOfKeyOrder, "a key order is empty, or lists a field twice or one without storage") \
    v(SortingOfKeyOrders, "the key orders of the layout are not sorted, or one begins another") \
    v(UnitsOfMethodTable, "the method table does not have the units of its selectors and no other") \
    v(ExtensionWord, "a slot lies out of line and the layout has no word for the Extension, or it has the word and no such slot")

namespace {

enum class RuleOfPlan : uint32_t {
    None,
#define AOT_PLAN_RULE(name, words) name,
    FOR_EACH_AOT_PLAN_RULE(AOT_PLAN_RULE)
#undef AOT_PLAN_RULE
};

struct ExtentOfPlannedSlot {
    uint64_t first { 0 };
    uint64_t end { 0 };
};

class CheckerOfPlan {
    WTF_MAKE_NONCOPYABLE(CheckerOfPlan);
public:
    CheckerOfPlan(const Program& program, const Plan& plan, BuildContext& build)
        : m_program(program)
        , m_plan(plan)
        , m_build(build)
    {
    }

    void check();

private:
    static ASCIILiteral wordsOf(RuleOfPlan);
    bool expect(bool holds, RuleOfPlan, LayoutID = { });

    void checkArea(LayoutID, const PlannedLayout&, SlotStorage, uint64_t start, uint64_t limit, RuleOfPlan ruleOfBounds);
    void checkLayout(LayoutID);
    void checkShapes();
    void checkKeyOrdersOf(LayoutID);
    void checkMethodTable();

    const Program& m_program;
    const Plan& m_plan;
    BuildContext& m_build;
    Vector<ExtentOfPlannedSlot, 32> m_extents;
    BitVector m_isListed;
};

ASCIILiteral CheckerOfPlan::wordsOf(RuleOfPlan rule)
{
    switch (rule) {
    case RuleOfPlan::None:
        break;
#define AOT_PLAN_RULE(name, words) \
    case RuleOfPlan::name: \
        return words ""_s;
    FOR_EACH_AOT_PLAN_RULE(AOT_PLAN_RULE)
#undef AOT_PLAN_RULE
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CheckerOfPlan::expect(bool holds, RuleOfPlan rule, LayoutID layout)
{
    if (!holds) {
        m_build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .layout = layout,
            .count = static_cast<uint32_t>(rule),
            .detail = String(wordsOf(rule)),
        });
    }
    return holds;
}

void CheckerOfPlan::checkArea(LayoutID id, const PlannedLayout& planned, SlotStorage storage, uint64_t start, uint64_t limit, RuleOfPlan ruleOfBounds)
{
    m_extents.shrink(0);
    for (const PlacedSlot& slot : planned.slots) {
        if (slot.storage != storage)
            continue;
        uint64_t end = static_cast<uint64_t>(slot.offset) + slot.width();
        expect(!(slot.offset % slot.width()), RuleOfPlan::SlotAlignment, id);
        expect(slot.offset >= start && end <= limit && slot.offset <= SlotData::maxOffset, ruleOfBounds, id);
        m_extents.append({ slot.offset, end });
    }
    AOT::sort(m_extents, { }, &ExtentOfPlannedSlot::first);
    uint64_t endOfSlotsInFront = 0;
    for (const ExtentOfPlannedSlot& extent : m_extents) {
        expect(extent.first >= endOfSlotsInFront, RuleOfPlan::SlotsOverlap, id);
        endOfSlotsInFront = std::max(endOfSlotsInFront, extent.end);
    }
}

void CheckerOfPlan::checkLayout(LayoutID id)
{
    const PlannedLayout& planned = m_plan.layout(id);
    if (!planned.hasSlots())
        return;
    if (!expect(planned.slots.size() == m_program.at(id).fields.size() + planned.appendedKeys.size() + numberOfHiddenWords, RuleOfPlan::NumberOfSlots, id))
        return;
    bool hasSlotOutOfLine = planned.slots.containsIf([](const PlacedSlot& slot) {
        return slot.isOutOfLine();
    });
    expect(hasSlotOutOfLine == planned.hasExtension(), RuleOfPlan::ExtensionWord, id);

    bool isEnvironment = planned.storage == LayoutStorage::Environment;
    expect(!(planned.size % 8), RuleOfPlan::CellSize, id);
    checkArea(id, planned, SlotStorage::Inline, isEnvironment ? offsetOfFirstEnvironmentVariable : offsetOfFirstField, planned.size, RuleOfPlan::SlotOutsideCell);
    checkArea(id, planned, SlotStorage::OutOfLine, offsetOfFirstExtensionSlot, std::min(planned.extensionSize, maxExtensionSize), RuleOfPlan::SlotOutsideExtension);

    if (!m_plan.isAllocatable(id) || !(planned.holdsCompiledObjects() || isEnvironment))
        return;
    expect(m_plan.shapes().isAssigned(m_plan.shapes().generalShapeOf(id)), RuleOfPlan::ShapeMissing, id);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field)
        expect(planned.slots[field].hasStorage() || !planned.slots[field].indexInLayoutRecord, RuleOfPlan::StorageMissing, id);
}

void CheckerOfPlan::checkShapes()
{
    const ShapeTable& shapes = m_plan.shapes();
    uint32_t firstEnvironmentShape = shapes.firstEnvironmentShape();
    expect(firstEnvironmentShape, RuleOfPlan::FirstEnvironmentShape);
    for (uint32_t number = 1; number <= shapes.numberOfShapes(); ++number) {
        if (!shapes.isAssigned(number))
            continue;
        LayoutID layout = shapes.shape(number).layout;
        ShapeRange range = shapes.shapesOf(layout);
        expect(number >= range.first && number < range.end(), RuleOfPlan::ShapeOfOtherLayout, layout);
        expect(m_plan.layout(layout).holdsCompiledObjects() == (number < firstEnvironmentShape), RuleOfPlan::SideOfShape, layout);
    }
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        ShapeRange range = shapes.shapesOf(LayoutID(index));
        for (uint32_t number = range.first; number < range.end(); ++number)
            expect(shapes.isAssigned(number) && shapes.shape(number).layout == LayoutID(index), RuleOfPlan::ShapeOfOtherLayout, LayoutID(index));
    }
}

void CheckerOfPlan::checkKeyOrdersOf(LayoutID id)
{
    const KeyOrders& orders = m_plan.keyOrders();
    uint32_t numberOfOrders = orders.numberOfOrdersOf(id);
    if (!numberOfOrders)
        return;
    const PlannedLayout& planned = m_plan.layout(id);
    if (!expect(planned.holdsCompiledObjects(), RuleOfPlan::OrdersWithoutObjects, id))
        return;
    expect(numberOfOrders <= maxKeyOrdersPerLayout, RuleOfPlan::NumberOfKeyOrders, id);

    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> order = orders.order(id, number);
        m_isListed.clearAll();
        bool listsFieldsOnce = !order.empty() && order.size() <= maxFieldsInOrder && std::ranges::all_of(order, [&](FieldIndex field) {
            return field < planned.numberOfFields() && planned.slots[field].hasStorage() && !m_isListed.set(field);
        });
        expect(listsFieldsOnce, RuleOfPlan::FieldOfKeyOrder, id);
        if (!number)
            continue;
        std::span<const FieldIndex> orderInFront = orders.order(id, number - 1);
        bool beginsThisOrder = orderInFront.size() <= order.size() && std::ranges::equal(orderInFront, order.first(orderInFront.size()));
        expect(std::ranges::lexicographical_compare(orderInFront, order) && !beginsThisOrder, RuleOfPlan::SortingOfKeyOrders, id);
    }
}

void CheckerOfPlan::checkMethodTable()
{
    const CallingConventions& conventions = m_plan.conventions();
    size_t numberOfFamilies = conventions.methodFamilies().size();
    uint64_t numberOfTableUnits = 0;
    for (const MethodSelector& selector : conventions.methodSelectors()) {
        if (!expect(selector.family < numberOfFamilies && selector.firstUnit == numberOfTableUnits, RuleOfPlan::UnitsOfMethodTable))
            return;
        numberOfTableUnits += 1 + conventions.familyOf(selector).methods.size();
    }
    uint64_t numberOfUnits = static_cast<uint64_t>(conventions.numberOfFunctions()) * numberOfCodeRolesOfOneFunction + numberOfTableUnits;
    expect(conventions.numberOfUnits() == numberOfUnits, RuleOfPlan::UnitsOfMethodTable);
}

void CheckerOfPlan::check()
{
    expect(m_plan.usesWideReferences() == m_build.recordedOptions().useWideReferences(), RuleOfPlan::WideReferencesOfBuild);
    size_t numberOfVariables = m_program.variables.size() + m_plan.promotedEnvironments().variables.size();
    expect(m_plan.slots().numberOfVariables() == numberOfVariables, RuleOfPlan::NumberOfVariables);
    checkMethodTable();
    if (!expect(m_plan.slots().numberOfLayouts() == m_program.layouts.size(), RuleOfPlan::NumberOfLayouts))
        return;

    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        checkLayout(LayoutID(index));
        checkKeyOrdersOf(LayoutID(index));
    }
    checkShapes();
}

} // anonymous namespace

#undef FOR_EACH_AOT_PLAN_RULE

bool Plan::isImmutable(const Program& program, VariableID id) const
{
    if (const PromotedVariable* promoted = promotedEnvironments().find(id))
        return !!promoted->function;
    return program.at(id).flags.contains(VariableFlags::IsImmutable);
}

void Plan::findCallEntriesThatWalkBodies(const Program& program, const Facts& facts)
{
    auto storesListAtEntry = [&](FunctionID id, const BodyIndex& body) {
        const Function& function = program.at(id);
        BodyBlock entryOfBody = body.block(entryBlock);
        for (const StoreOfCapturedValue& store : promotedEnvironments().storesIn(id)) {
            std::optional<uint32_t> index = entryOfBody.indexOfParameter(store.value);
            if (!index)
                continue;
            EntryParameter kind = function.entryParameterKind(*index);
            if (kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && *index - function.firstParameterIndex() >= function.numberOfPositionalParameters()))
                return true;
        }
        return false;
    };
    m_parts.functionsWhoseCallEntryWalksBody.ensureSize(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!conventions().callEntryWalksBody(id) || facts.isTakenToRunOnce(id))
            continue;
        GenericEntryPlan entry = conventions().genericEntry({ id, CodeRole::GenericCallEntry });
        const BodyIndex* body = facts.bodies().find(id);
        if ((!entry.takesNewTarget || program.at(id).kind == FunctionKind::DerivedConstructor) && entry.target.role == CodeRole::Body && body && !storesListAtEntry(id, *body))
            m_parts.functionsWhoseCallEntryWalksBody.quickSet(index);
    }
}

void checkPlan(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    CheckerOfPlan(program, plan, build).check();
    checkStorageOfFields(program, facts, plan, build);

}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
