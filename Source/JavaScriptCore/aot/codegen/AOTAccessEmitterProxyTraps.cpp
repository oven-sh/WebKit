/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTShapeTestEmitter.h"
#include "AOTTypeArena.h"
#include "Structure.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(OptionSet<CompiledStructure::Flag>) == sizeof(uint8_t));

// What a layout that lists all its fields itself says of them is known here.
static bool mayBeBornWithFieldThatBindsTraps(const Program& program, const Facts& facts, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.isDerived() || writtenBaseOf(layout))
        return true;
    uint32_t numberOfFields = facts.numberOfFieldsOf(id);
    if (numberOfFields < layout.fields.size())
        return true;
    for (uint32_t index = 0; index < numberOfFields; ++index) {
        OptionSet<FieldForm> form = facts.field(FieldReference { id, index }).formOfValue;
        if (index < layout.fields.size())
            form.add(layout.fields[index].form);
        if (form.contains(FieldForm::IsNotConfigurable))
            return true;
    }
    return false;
}

// Only a target that is not extensible or has a property that is not configurable binds what a trap may answer.
// The Structure of a compiled object says yes for all of them: what was done to one object stands in the object, and what its fields are born with in the layout.
Condition AccessEmitter::mayBindAnswersOfTraps(ValueID id, uint32_t bitsOfStructureThatMayBind)
{
    CellPointer target = in<CellPointer>(id);
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    auto isNoLongerAsBorn = [&] {
        return hasBits(shapes.orderWordOf(target), ~orderAndCountMask);
    };
    auto askStructure = [&] {
        Pointer structure = loadStructure(target);
        return selectByBranch<Condition>(hasBits(loadField<Word32>(structure, m_heaps.Structure_bitField), bitsOfStructureThatMayBind), [&] {
            return selectByBranch<Condition>(shapes.isCompiledObject(target), [&] {
                Condition isBornFree = hasBits(loadByte(structure, m_heaps.AOTCompiledStructure_flags), static_cast<uint8_t>(CompiledStructure::Flag::EveryFieldIsBornConfigurable));
                return logicalNot(logicalAnd(isBornFree, logicalNot(isNoLongerAsBorn())));
            }, [&] {
                return boolean(true);
            });
        }, [&] {
            return boolean(false);
        });
    };

    Type type = typeOf(id);
    bool isBornFree = type.hasOnlyLayouts() && std::ranges::none_of(membersOf(types(), type), [&](LayoutID member) {
        return mayBeBornWithFieldThatBindsTraps(m_program, m_facts, member);
    });
    if (!isBornFree)
        return askStructure();
    return selectByBranch<Condition>(shapes.isCompiledObject(target), isNoLongerAsBorn, askStructure);
}

void AccessEmitter::generateMayBindAnswersOfGetAndSetTraps(const ValueContext& value)
{
    defineTest(value, mayBindAnswersOfTraps(value->targetOperand(), Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits));
}

void AccessEmitter::generateMayBindAnswersOfHasTraps(const ValueContext& value)
{
    defineTest(value, mayBindAnswersOfTraps(value->targetOperand(), Structure::s_didPreventExtensionsBits | Structure::s_hasNonConfigurablePropertiesBits));
}

void AccessEmitter::generateValidateAnswerOfGetTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateAnswerOfGetTrap, { boxed(value->targetOperand()), boxed(value->propertyOperand()), boxed(value->answerOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateValidateDenialOfHasTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateDenialOfHasTrap, { boxed(value->targetOperand()), boxed(value->propertyOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateValidateConsentOfDeleteTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateConsentOfDeleteTrap, { boxed(value->targetOperand()), boxed(value->propertyOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateHasOwnByAnswerOfDescriptorTrap(const ValueContext& value)
{
    define(value, callOperation<Condition>(value, RuntimeOperation::HasOwnByAnswerOfDescriptorTrap, { boxed(value->targetOperand()), boxed(value->propertyOperand()), boxed(value->answerOperand()) }));
}

void AccessEmitter::generateFinishStoreByTrap(const ValueContext& value)
{
    Word32 isStrict = word32(forMode(value->mode(), 1, 0));
    callVoidOperation(value, RuntimeOperation::FinishStoreByTrap, { boxed(value->targetOperand()), boxed(value->propertyOperand()), boxed(value->valueOperand()), boxed(value->answerOperand()), isStrict }, { .isOnRarePath = true });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
