/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTInlineCall.h"
#include "AOTRepresentationEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTTypeTestEmitter.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t doneTargetOfInlineCall = 0;
static constexpr uint32_t missTargetOfInlineCall = 1;

static ValueID resultParameterOfInlineCall(const Code& code, const TerminatorContext& terminator)
{
    const Edge& done = code.at(terminator->done());
    std::span<const ValueID> parameters = code.at(done.to).parameters();
    size_t numberOfArguments = done.arguments().size();
    RELEASE_ASSERT(numberOfArguments <= parameters.size() && parameters.size() - numberOfArguments <= 1);
    return numberOfArguments < parameters.size() ? parameters[numberOfArguments] : ValueID();
}

bool OperationEmitter::hasOneInstance() const
{
    return m_plan.hasOneInstance();
}

FieldAccess OperationEmitter::fieldAccessFor(PropertyKeyID key, LayoutSetID receivers, bool isStore) const
{
    if (holdsOneMember(receivers))
        return m_plan.slots().fieldAccessOfOneLayout(m_program, key, LayoutID(positionOf(receivers)));
    return m_plan.fieldAccess(m_program, key, receivers, membersOf(m_generator.inputs().types, receivers), isStore);
}

ValueID OperationEmitter::definitionOf(ValueID id) const
{
    while (id && opcodeOf(id) == Opcode::Narrowed)
        id = operationAt(id).valueOperand();
    return id;
}

void OperationEmitter::defineNarrowed(const ValueContext& value, Operand operand)
{
    if (value.representation() == Representation::Void) {
        define(value, Operand());
        return;
    }
    if (operand.representation == value.representation()) [[likely]] {
        define(value, operand);
        return;
    }

    ConversionInSteps path = findConversionInSteps(types(), value->type, operand.representation, value.representation());
    if (!path.isFound) {
        fail(value, InternalErrorKind::UnsupportedConversion);
        return;
    }
    Type type = typeOf(value.id());
    for (uint8_t index = 0; index < path.numberOfSteps; ++index)
        operand = emitter<RepresentationEmitter>().convert(value, path.steps[index].opcode, operand, type, path.steps[index].to);
    define(value, operand);
}

Integer OperationEmitter::integerOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Word32>())
        return signExtended(operand.as<Word32>());
    return operand.as<Integer>();
}

BuiltinID OperationEmitter::functionOf(BuiltinOverloadID overload) const
{
    return m_builtins.canonicalID(m_builtins.at(overload).builtin);
}

Operand OperationEmitter::callOperation(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callOperation(context, operation, arguments, result, options);
}

Operand OperationEmitter::callWithoutSite(RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callWithoutSite(operation, arguments, result, options);
}

Operand OperationEmitter::callLeaf(RuntimeOperation operation, std::span<const Operand> arguments, Representation result)
{
    return emitter<RuntimeCallEmitter>().callLeaf(operation, arguments, result);
}

StubCallResult OperationEmitter::callStub(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callStub(context, key, operands, options);
}

void OperationEmitter::throwAt(const OperationContext& context, ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b)
{
    emitter<ThrowEmitter>().throwAt(context, ThrowSite { kind, a, b }, operand);
}

void OperationEmitter::requireOrThrow(const OperationContext& context, Condition mustHold, ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b)
{
    emitter<ThrowEmitter>().requireOrThrow(context, mustHold, ThrowSite { kind, a, b }, operand);
}

const TypeTestPlan& OperationEmitter::typeTestPlanFor(TypeID from, Representation representation, TypeID to)
{
    return emitter<TypeTestEmitter>().planFor(from, representation, to);
}

Word32 OperationEmitter::int32InBox(Boxed value)
{
    if (const OperationContext* context = emittersCheckProofs() ? generator().contextBeingGenerated() : nullptr)
        emitter<ThrowEmitter>().requireOrCrash(*context, isInt32(value), value, ProofSource::OtherOperation);
    return unboxInt32(value);
}

Condition OperationEmitter::hasType(Operand operand, const TypeTestPlan& plan)
{
    return emitter<TypeTestEmitter>().emitBoolean(operand, plan);
}

void OperationEmitter::branchOnType(Operand operand, const TypeTestPlan& plan, BranchTargets targets)
{
    emitter<TypeTestEmitter>().emitBranch(operand, plan, targets);
}

void OperationEmitter::barrierAfterStore(const ValueContext& store, CellPointer owner, const Type& storedType)
{
    emitter<BarrierEmitter>().barrierAfterStore(store, owner, storedType);
}

Representation OperationEmitter::resultRepresentationOf(const InlineCall& call) const
{
    if (const ValueContext* value = call.value())
        return value->representation();
    ValueID parameter = resultParameterOfInlineCall(m_code, *call.terminator());
    return parameter ? representationOf(parameter) : Representation::Void;
}

Type OperationEmitter::resultTypeOf(const InlineCall& call) const
{
    if (const ValueContext* value = call.value())
        return typeOf(value->id());
    ValueID parameter = resultParameterOfInlineCall(m_code, *call.terminator());
    return parameter ? typeOf(parameter) : m_program.at(TypeTable::undefined);
}

TypeID OperationEmitter::resultTypeIDOf(const InlineCall& call) const
{
    if (const ValueContext* value = call.value())
        return typeIDOf(value->id());
    ValueID parameter = resultParameterOfInlineCall(m_code, *call.terminator());
    return parameter ? typeIDOf(parameter) : TypeID(TypeTable::undefined);
}

bool OperationEmitter::resultIsRead(const InlineCall& call) const
{
    RELEASE_ASSERT(call.canMiss());
    ValueID parameter = resultParameterOfInlineCall(m_code, *call.terminator());
    return parameter && m_code.isNeeded(parameter);
}

LBasicBlock OperationEmitter::missBlockOf(const InlineCall& call)
{
    RELEASE_ASSERT(call.canMiss());
    if (!call.m_missBlock)
        call.m_missBlock = m_generator.edgeTo(*call.terminator(), missTargetOfInlineCall).block();
    return call.m_missBlock;
}

void OperationEmitter::finishInlineCall(const InlineCall& call, Operand result)
{
    if (const ValueContext* value = call.value()) {
        define(*value, result);
        return;
    }
    const TerminatorContext& terminator = *call.terminator();
    std::span<const Operand> declaredResults;
    if (resultParameterOfInlineCall(m_code, terminator))
        declaredResults = singleElementSpan(result);
    m_generator.jumpTo(terminator, doneTargetOfInlineCall, declaredResults);
}

Pointer OperationEmitter::argumentBuffer(std::span<const Boxed> values)
{
    return emitter<RuntimeCallEmitter>().argumentBuffer(values);
}

uint32_t OperationEmitter::mentionOfName(PropertyKeyID key)
{
    RELEASE_ASSERT(key);
    if (!liesInThread(key))
        return m_mentions.add(NumberSpace::Name, key);
    const PropertyKey& threadLocalKey = types().at(key);
    RELEASE_ASSERT(threadLocalKey.kind == PropertyKeyKind::Text);
    return m_mentions.add(NumberSpace::NameOfText, threadLocalKey.text);
}

Word32 OperationEmitter::identifierNumber(PropertyKeyID key)
{
    return numberOf(mentionOfName(key));
}

std::optional<Vector<uint32_t, 12>> OperationEmitter::modulesAndPositionsOf(const OperationContext& context) const
{
    return modulesAndPositionsOf(context.origin());
}

std::optional<Vector<uint32_t, 12>> OperationEmitter::modulesAndPositionsOf(Origin origin) const
{
    std::optional<Vector<uint32_t, 12>> places { std::in_place };
    auto frame = origin.inlineFrame;
    auto position = origin.position;
    while (true) {
        const InlineFrame& inlineFrame = m_code.inlineFrames()[frame];
        if (!inlineFrame.callee)
            break;
        Wire::SourceLocation place = m_program.at(inlineFrame.callee).locationAt(position);
        if (!hasModule(place))
            break;
        places->append(place.position);
        places->append(place.module);
        if (!frame) {
            places->reverse();
            return places;
        }
        position = inlineFrame.callPosition;
        frame = inlineFrame.parent;
    }
    places.reset();
    return places;
}

std::optional<Pointer> OperationEmitter::siteCacheAddress(const OperationContext& context, PropertyKeyID key, SlotAccess access)
{
    if (liesInThread(key))
        return std::nullopt;
    std::optional<Vector<uint32_t, 12>> places = modulesAndPositionsOf(context);
    if (!places)
        return std::nullopt;
    return Pointer(m_out.valueOf(instanceWord(m_heaps.AOTInstance_siteCaches, m_mentions.siteCache(WTF::move(*places), key, static_cast<uint32_t>(access)))));
}

std::optional<NumberedImmediate> OperationEmitter::answersAtSite(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    return answersAtSite(context.origin(), key, use);
}

std::optional<NumberedImmediate> OperationEmitter::answersAtSite(Origin origin, PropertyKeyID key, SiteUse use)
{
    return answersAtSiteOfUse(origin, key, MentionList::firstUseOfSiteAnswers + static_cast<uint32_t>(use));
}

std::optional<NumberedImmediate> OperationEmitter::answersOfPresenceTestAtSite(Origin origin, PropertyKeyID key)
{
    static_assert(MentionList::useOfPresenceTests > MentionList::firstUseOfSiteAnswers + static_cast<uint32_t>(SiteUse::ReadUnderNameOfField));
    return answersAtSiteOfUse(origin, key, MentionList::useOfPresenceTests);
}

std::optional<NumberedImmediate> OperationEmitter::answersAtSiteOfUse(Origin origin, PropertyKeyID key, uint32_t use)
{
    if (liesInThread(key))
        return std::nullopt;
    std::optional<Vector<uint32_t, 12>> places = modulesAndPositionsOf(origin);
    if (!places)
        return std::nullopt;
    return NumberedImmediate { m_mentions.siteCache(WTF::move(*places), key, use) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
