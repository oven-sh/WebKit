/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNotes.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet testsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return familyOf(opcode) == OperationFamily::Guard && accessOf(opcode) == Access::None;
});
static constexpr OpcodeSet allocationsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return interpretedOf(opcode) == Interpretation::Allocation;
});

static constexpr OpcodeSet cachedLookupsInWalkNotes { Opcode::GetValueAtSiteOrAbsent, Opcode::HasPropertyOrAbsent, Opcode::GetKeyedValueOrAbsent, Opcode::GetOwnValueUnderKeyOrAbsent, Opcode::GetValueUnderKeyOrAbsent, Opcode::GetCachedGetter, Opcode::GetCachedSetter };

static AccessKind kindOfCallInWalkNote(const Operation& call)
{
    bool constructs = entersByValueOf(call.opcode) == EntryByValue::Construction || (call.opcode == Opcode::CallDirect && call.isConstruction());
    return constructs ? AccessKind::Construct : AccessKind::Call;
}

static AccessKind kindOfAccessWithoutColumnInWalkNote(const Operation& access)
{
    if (access.numberOfNamedOperands() && access.operandUses(0).contains(OperandUse::DeletesByKey))
        return AccessKind::Delete;
    switch (interpretedOf(access.opcode)) {
    case Interpretation::Read:
        return AccessKind::Read;
    case Interpretation::Store:
        return AccessKind::Store;
    case Interpretation::Test:
        return AccessKind::Has;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessKind accessKindInWalkNote(const Operation& operation)
{
    Opcode opcode = operation.opcode;
    if (cachedLookupsInWalkNotes.contains(opcode))
        return AccessKind::Read;
    switch (accessOf(opcode)) {
    case Access::Read:
        return AccessKind::Read;
    case Access::Store:
        return AccessKind::Store;
    case Access::Define:
        return AccessKind::Define;
    case Access::Has:
        return AccessKind::Has;
    case Access::None:
        break;
    }

    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return kindOfCallInWalkNote(operation);
    case AccessClass::Slot:
    case AccessClass::ByName:
        return kindOfAccessWithoutColumnInWalkNote(operation);
    case AccessClass::None:
        break;
    }

    switch (operatorKindOf(opcode)) {
    case OperatorKind::Additive:
    case OperatorKind::Multiplicative:
    case OperatorKind::Bitwise:
        return AccessKind::Arithmetic;
    case OperatorKind::Relation:
        return AccessKind::Comparison;
    case OperatorKind::Equality:
        return AccessKind::Equality;
    case OperatorKind::NumberConversion:
        return AccessKind::Conversion;
    case OperatorKind::None:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkNoteKind kindOfOperationInWalkNote(const Operation& operation)
{
    if (testsItsOwnGuess(operation))
        return WalkNoteKind::TestThatRejoins;

    Opcode opcode = operation.opcode;
    if (cachedLookupsInWalkNotes.contains(opcode))
        return WalkNoteKind::GenericOperation;
    switch (accessClassOf(opcode)) {
    case AccessClass::Slot:
    case AccessClass::DirectCall:
        return WalkNoteKind::ResolvedOperation;
    case AccessClass::ByName:
    case AccessClass::IndirectCall:
        return WalkNoteKind::GenericOperation;
    case AccessClass::None:
        break;
    }
    if (PayloadWord::form(opcode))
        return operation.form() == OperatorForm::Generic ? WalkNoteKind::GenericOperation : WalkNoteKind::ResolvedOperation;
    return formOf(opcode) == Form::Open ? WalkNoteKind::ResolvedOperation : WalkNoteKind::GenericOperation;
}

static AccessCost costOfWalkNoteKind(WalkNoteKind kind)
{
    switch (kind) {
    case WalkNoteKind::ResolvedOperation:
        return AccessCost::Free;
    case WalkNoteKind::TestThatRejoins:
    case WalkNoteKind::TestThatLeaves:
    case WalkNoteKind::TestThatThrows:
        return AccessCost::TestsReceiver;
    case WalkNoteKind::GenericOperation:
    case WalkNoteKind::CallNotInlined:
    case WalkNoteKind::ObjectBuilt:
    case WalkNoteKind::ObjectNotInFrame:
    case WalkNoteKind::ObjectStayedVirtual:
    case WalkNoteKind::LoopWalkedAgain:
    case WalkNoteKind::WayEnds:
        return AccessCost::ByName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnprovenCause causeInWalkNote(AccessKind access)
{
    switch (access) {
    case AccessKind::Read:
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Has:
    case AccessKind::Delete:
        return UnprovenCause::UnknownObject;
    case AccessKind::Call:
    case AccessKind::Construct:
        return UnprovenCause::UnknownCallee;
    case AccessKind::Truthiness:
    case AccessKind::Equality:
    case AccessKind::Arithmetic:
    case AccessKind::Comparison:
    case AccessKind::Conversion:
    case AccessKind::TypeOf:
    case AccessKind::InstanceOf:
    case AccessKind::Iteration:
    case AccessKind::Spread:
        return UnprovenCause::Untracked;
    case AccessKind::Unknown:
        return UnprovenCause::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool placeWalkNote(const Code& code, const Operation& operation, Origin& origin, FunctionID& writtenIn)
{
    std::span<const InlineFrame> frames = code.inlineFrames();
    if (!operation.hasPosition() || operation.inlineFrame >= frames.size())
        return false;
    origin = operation.origin();
    writtenIn = frames[operation.inlineFrame].callee;
    return true;
}

static LayoutID layoutOfValueInWalkNote(TypeReader types, const Code& code, ValueID value)
{
    return singleLayoutOf(types, types.at(code.at(value).type));
}

static BitVector findUsualBlocks(const Code& code)
{
    BitVector usual(code.numberOfBlocks());
    Vector<BlockID, 64> worklist;
    auto enter = [&](BlockID block) {
        if (block && code.isReached(block) && !usual.quickSet(block.index()))
            worklist.append(block);
    };
    for (const EntryOfCode& entry : code.entries())
        enter(entry.block);
    while (!worklist.isEmpty()) {
        const Block& block = code.at(worklist.takeLast());
        auto isRare = [&](EdgeID edge) {
            BlockID to = code.at(edge).to;
            return !to || code.at(to).frequency == BlockFrequency::Rare;
        };
        bool everyTargetIsUnlikely = true;
        for (uint32_t target = 0; target < block.terminator.numberOfEdges; ++target)
            everyTargetIsUnlikely &= isRare(block.terminator.edge(target));
        for (uint32_t target = 0; target < block.terminator.numberOfEdges; ++target) {
            EdgeID edge = block.terminator.edge(target);
            if (everyTargetIsUnlikely || !isRare(edge))
                enter(code.at(edge).to);
        }
        if (block.handler && !isRare(block.handler))
            enter(code.at(block.handler).to);
    }
    return usual;
}

static bool excludesOnlyNullish(const Type& before, const Type& after)
{
    static constexpr uint16_t nullish = Primitive::UndefinedOrNull | Primitive::Absent;
    return !(before.primitives & ~nullish) && before.layouts == after.layouts && before.classes == after.classes && before.objects == after.objects
        && before.unnamedLayouts == after.unnamedLayouts && before.unnamedClasses == after.unnamedClasses && before.anyObject == after.anyObject && before.anyFunction == after.anyFunction;
}

class GuardsOfAccesses {
public:
    GuardsOfAccesses(TypeReader types, const Code& code, const BitVector& usualBlocks)
        : m_types(types)
        , m_code(code)
        , m_taken(code.numberOfValues())
    {
        for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
            const Operation& check = code.at(ValueID(index));
            bool isGuard = check.opcode == Opcode::IsSlotFilled || (check.opcode != Opcode::TypeTest && testsInWalkNotes.contains(check.opcode));
            if (isGuard && check.hasPosition() && check.block() && usualBlocks.get(check.block().index()) && code.isNeeded(ValueID(index)))
                m_guards.append({ placeOf(check), ValueID(index) });
        }
        std::ranges::stable_sort(m_guards, { }, &Guard::place);
    }

    AccessCost take(const Operation& access, ValueID receiver)
    {
        AccessCost cost = AccessCost::Free;
        m_hasTakenCheckOfKeys = false;
        if (access.opcode == Opcode::IsSlotFilled)
            return cost;
        auto take = [&](ValueID id) {
            const Operation& check = m_code.at(id);
            if (!check.block() || m_code.at(check.block()).isInLoop != m_code.at(access.block()).isInLoop || m_taken.quickSet(id.index()))
                return;
            cost = std::min(cost, check.opcode == Opcode::IsSlotFilled ? AccessCost::TestsSlot : AccessCost::TestsReceiver);
            if (check.opcode == Opcode::HasKeyOrder || check.opcode == Opcode::HasNoExtraProperties)
                m_hasTakenCheckOfKeys = true;
        };
        for (const Guard& guard : std::ranges::equal_range(m_guards, placeOf(access), { }, &Guard::place))
            take(guard.check);
        while (receiver) {
            const Operation& record = m_code.at(receiver);
            if (record.opcode == Opcode::Narrowed) {
                ValueID test = record.testOperand();
                if (test && (!m_code.at(test).hasPosition() || placeOf(m_code.at(test)) != placeOf(access)))
                    break;
                if (test && !excludesOnlyNullish(m_types.at(m_code.at(record.valueOperand()).type), m_types.at(record.type)))
                    take(test);
                receiver = record.valueOperand();
                continue;
            }
            if (familyOf(record.opcode) != OperationFamily::Representation)
                break;
            receiver = record.operand(0);
        }
        return cost;
    }

    bool hasTakenCheckOfKeys() const { return m_hasTakenCheckOfKeys; }

private:
    struct Guard {
        uint64_t place;
        ValueID check;
    };

    static uint64_t placeOf(const Operation& operation) { return static_cast<uint64_t>(operation.inlineFrame) << 32 | operation.origin().position; }

    TypeReader m_types;
    const Code& m_code;
    Vector<Guard> m_guards;
    BitVector m_taken;
    bool m_hasTakenCheckOfKeys { false };
};

class WideSources {
public:
    WideSources(TypeReader types, const Code& code)
        : m_code(code)
        , m_sources(code.numberOfValues())
        , m_roots(code.numberOfValues())
    {
        BitVector wide(code.numberOfValues());
        for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
            ValueID id(index);
            if (!isPlaced(id) || !code.at(id).type)
                continue;
            Type type = types.at(code.at(id).type);
            if (!type.hasOnlyLayouts() && !type.isNumberOrNullish())
                wide.quickSet(index);
        }
        Vector<ValueID> results;
        Vector<ValueID> reads;
        for (size_t index : wide) {
            ValueID id(static_cast<uint32_t>(index));
            const Operation& operation = code.at(id);
            auto addInput = [&](ValueID input) {
                if (input && isPlaced(input) && wide.quickGet(input.index()))
                    m_edges.append({ input, id });
            };
            AccessClass accessClass = accessClassOf(operation.opcode);
            if (operation.opcode == Opcode::Narrowed)
                addInput(operation.valueOperand());
            else if (operation.opcode == Opcode::Parameter)
                continue;
            else if (familyOf(operation.opcode) == OperationFamily::Representation || operatorKindOf(operation.opcode) != OperatorKind::None) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    addInput(operand);
                });
            } else if (accessClass == AccessClass::DirectCall || accessClass == AccessClass::IndirectCall || operation.opcode == Opcode::CatchValue)
                results.append(id);
            else if (types.at(operation.type).hasUnboundedUnknown()) {
                reads.append(id);
                if (std::optional<uint8_t> receiver = receiverOf(operation.opcode))
                    addInput(stripped(operation.child(*receiver)));
            }
        }
        for (uint32_t block = 0; block < code.numberOfBlocks(); ++block) {
            if (!code.isReached(BlockID(block)))
                continue;
            std::span<const ValueID> parameters = code.at(BlockID(block)).parameters();
            for (EdgeID edge = code.at(BlockID(block)).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
                std::span<const ValueID> arguments = code.at(edge).arguments();
                for (size_t position = 0; position < std::min(parameters.size(), arguments.size()); ++position) {
                    ValueID argument = arguments[position];
                    if (argument && isPlaced(argument) && wide.quickGet(argument.index()) && wide.quickGet(parameters[position].index()))
                        m_edges.append({ argument, parameters[position] });
                }
            }
        }
        std::ranges::stable_sort(m_edges, { }, [](const FlowEdge& edge) { return edge.from.index(); });
        spread(WTF::move(results));
        spread(WTF::move(reads));
    }

    ValueID sourceOf(const Operation& operation) const
    {
        ValueID found;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            ValueID value = found || !operand ? ValueID() : stripped(operand);
            if (value && isPlaced(value))
                found = m_sources[value.index()];
        });
        return found;
    }

private:
    struct FlowEdge {
        ValueID from;
        ValueID to;
    };

    bool isPlaced(ValueID id) const { return m_code.at(id).block() && m_code.isReached(m_code.at(id).block()); }

    ValueID stripped(ValueID value) const
    {
        auto isOnlyNamed = [&](ValueID id) {
            return !m_code.isConstant(id) && (m_code.at(id).opcode == Opcode::Narrowed || familyOf(m_code.at(id).opcode) == OperationFamily::Representation) && m_code.at(id).operand(0);
        };
        ValueID root = value;
        while (root && isOnlyNamed(root))
            root = m_roots[root.index()] ? m_roots[root.index()] : m_code.at(root).operand(0);
        for (ValueID id = value; id && id != root;) {
            ValueID next = m_roots[id.index()] ? m_roots[id.index()] : m_code.at(id).operand(0);
            m_roots[id.index()] = root;
            id = next;
        }
        return root;
    }

    void spread(Vector<ValueID>&& level)
    {
        level.removeAllMatching([&](ValueID id) {
            return !!m_sources[id.index()];
        });
        for (ValueID id : level)
            m_sources[id.index()] = id;
        while (!level.isEmpty()) {
            Vector<ValueID> next;
            for (ValueID id : level) {
                for (const FlowEdge& edge : std::ranges::equal_range(m_edges, id.index(), { }, [](const FlowEdge& edge) { return edge.from.index(); })) {
                    if (m_sources[edge.to.index()])
                        continue;
                    m_sources[edge.to.index()] = m_sources[id.index()];
                    next.append(edge.to);
                }
            }
            level = WTF::move(next);
        }
    }

    const Code& m_code;
    Vector<ValueID> m_sources;
    mutable Vector<ValueID> m_roots;
    Vector<FlowEdge> m_edges;
};

static BuiltinID builtinOfResult(TypeReader types, const Operation& call)
{
    return call.opcode == Opcode::CallBuiltin ? types.builtins().at(call.overload()).builtin : BuiltinID();
}

static void noteWideSource(TypeReader types, const Code& code, ValueID source, const BitVector& usualBlocks, WalkNote& note)
{
    const Operation& record = code.at(source);
    if (!placeWalkNote(code, record, note.sourceAt, note.sourceIn))
        return;
    AccessClass accessClass = accessClassOf(record.opcode);
    note.sourceBuiltin = builtinOfResult(types, record);
    note.sourceFunction = record.opcode == Opcode::CallDirect ? record.function() : FunctionID();
    note.sourceIsOnUsualPath = usualBlocks.get(record.block().index());
    note.sourceIsCall = accessClass == AccessClass::DirectCall || accessClass == AccessClass::IndirectCall;
    std::span<const InlineFrame> frames = code.inlineFrames();
    const Program* program = types.program();
    while (program && note.sourceAt.inlineFrame && note.sourceIn && isLibraryCode(*program, program->at(note.sourceIn))) {
        const InlineFrame& frame = frames[note.sourceAt.inlineFrame];
        note.sourceBuiltin = frame.calledBuiltin;
        note.sourceFunction = frame.callee;
        note.sourceIsCall = true;
        note.sourceAt.position = frame.callPosition;
        note.sourceAt.inlineFrame = frame.parent;
        note.sourceIn = frames[frame.parent].callee;
    }
}

static Wire::TypeOriginReason reasonOfWalkNote(TypeReader types, const Code& code, const Operation& operation, AccessCost cost)
{
    if (cost == AccessCost::TestsSlot)
        return Wire::TypeOriginReason::MissingField;
    switch (operation.opcode) {
    case Opcode::GetArrayElement:
    case Opcode::PutArrayElement:
    case Opcode::HasArrayElement:
    case Opcode::GetTypedArrayElement:
    case Opcode::PutTypedArrayElement:
        return Wire::TypeOriginReason::CheckedElement;
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemHas:
    case Opcode::ElemDefine:
    case Opcode::GetKeyedValueOrAbsent:
    case Opcode::GetOwnValueUnderKeyOrAbsent:
    case Opcode::GetValueUnderKeyOrAbsent:
        return Wire::TypeOriginReason::ComputedName;
    case Opcode::PropDelete:
    case Opcode::ElemDelete:
        return Wire::TypeOriginReason::ChangedShape;
    case Opcode::PropDefine:
        return types.at(code.at(operation.child(0)).type).hasOnlyClosures() ? Wire::TypeOriginReason::StaticField : Wire::TypeOriginReason::Unknown;
    default:
        break;
    }
    std::optional<uint8_t> receiver = receiverOf(operation.opcode);
    ValueID object = receiver ? operation.child(*receiver) : ValueID();
    if (!object)
        return Wire::TypeOriginReason::Unknown;
    Type type = types.at(code.at(object).type);
    return type.layouts || type.hasUnboundedUnknown() ? Wire::TypeOriginReason::Unknown : Wire::TypeOriginReason::BuiltinProperty;
}

WalkNotes notesOfSweptCode(TypeReader types, const Code& code, bool notesResolvedOperations, bool explainsOperations)
{
    RELEASE_ASSERT(code.isSwept());
    WalkNotes result { code.unit(), { }, 0 };
    BitVector usualBlocks = findUsualBlocks(code);
    GuardsOfAccesses guards(types, code, usualBlocks);
    std::optional<WideSources> wideSources;
    if (explainsOperations)
        wideSources.emplace(types, code);

    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        ValueID id(index);
        const Operation& operation = code.at(id);
        BlockID block = operation.block();
        if (!block || !code.isReached(block))
            continue;
        result.numberOfValuesInDouble += code.isNeeded(id) && (operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble);

        WalkNote note;
        if (!placeWalkNote(code, operation, note.origin, note.writtenIn))
            continue;
        note.loopDepth = code.at(block).isInLoop;
        note.isOnUsualPath = usualBlocks.quickGet(block.index());

        if (allocationsInWalkNotes.contains(operation.opcode)) {
            note.kind = code.isNeeded(id) ? WalkNoteKind::ObjectBuilt : WalkNoteKind::ObjectStayedVirtual;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = layoutOfValueInWalkNote(types, code, id);
            if (note.kind == WalkNoteKind::ObjectBuilt) {
                note.objectBuilt = ObjectBuiltCause::OperationNeedsCell;
                if (ValueID user = code.onlyUserOf(id); user && placeWalkNote(code, code.at(user), note.neededAt, note.neededIn))
                    note.opcode = code.at(user).opcode;
            }
            result.notes.append(note);
            continue;
        }
        if (!code.isNeeded(id))
            continue;

        note.opcode = operation.opcode;
        if (code.unit().role == CodeRole::ParameterGuard) {
            if (operation.opcode != Opcode::TypeTest)
                continue;
            note.kind = WalkNoteKind::TestThatLeaves;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = singleLayoutOf(types, types.at(operation.testedType()));
            result.notes.append(note);
            continue;
        }
        if (testsInWalkNotes.contains(operation.opcode)) {
            note.kind = WalkNoteKind::TestThatRejoins;
            note.cost = costOfWalkNoteKind(note.kind);
            if (ValueID tested = operation.child(0))
                note.layout = layoutOfValueInWalkNote(types, code, tested);
            result.notes.append(note);
            continue;
        }

        note.access = accessKindInWalkNote(operation);
        if (note.access == AccessKind::Unknown)
            continue;
        note.kind = kindOfOperationInWalkNote(operation);
        if (note.kind == WalkNoteKind::ResolvedOperation && !notesResolvedOperations)
            continue;
        note.cost = costOfWalkNoteKind(note.kind);
        if (std::optional<uint8_t> receiver = receiverOf(operation.opcode)) {
            if (ValueID object = operation.child(*receiver)) {
                note.layout = layoutOfValueInWalkNote(types, code, object);
                if (note.cost == AccessCost::Free && note.isOnUsualPath) {
                    note.cost = guards.take(operation, object);
                    if (guards.hasTakenCheckOfKeys())
                        note.reason = Wire::TypeOriginReason::CheckedKeys;
                }
            }
        }
        if (note.cost != AccessCost::Free) {
            note.cause = causeInWalkNote(note.access);
            if (note.reason == Wire::TypeOriginReason::Unknown)
                note.reason = reasonOfWalkNote(types, code, operation, note.cost);
            ValueID source = wideSources && note.isOnUsualPath ? wideSources->sourceOf(operation) : ValueID();
            if (source && code.at(source).opcode == Opcode::CatchValue) {
                if (note.reason == Wire::TypeOriginReason::Unknown)
                    note.reason = Wire::TypeOriginReason::CaughtException;
            } else if (source)
                noteWideSource(types, code, source, usualBlocks, note);
        }
        result.notes.append(note);
    }
    for (const Code::TestOfGuardInFrontOfCopy& noted : code.testsOfGuardsInFrontOfCopies()) {
        const Operation& operation = code.at(noted.test);
        BlockID block = operation.block();
        if (operation.opcode != Opcode::TypeTest || !block || !code.isReached(block) || !code.isNeeded(noted.test))
            continue;
        WalkNote note;
        note.kind = WalkNoteKind::TestThatLeaves;
        note.opcode = operation.opcode;
        note.cost = costOfWalkNoteKind(note.kind);
        note.loopDepth = code.at(block).isInLoop;
        note.origin = Origin { noted.positionOfCallee, 0, false, false };
        note.writtenIn = noted.callee;
        note.layout = singleLayoutOf(types, types.at(operation.testedType()));
        result.notes.append(note);
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
