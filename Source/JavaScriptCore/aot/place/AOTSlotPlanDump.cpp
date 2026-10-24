/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))

#include "AOTSlotPlan.h"

#include "AOTIRPrinter.h"
#include "AOTPlacementTestCommands.h"
#include "AOTSlotPlanSteps.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(LayoutStorage storage)
{
    switch (storage) {
    case LayoutStorage::None:
        return "None"_s;
    case LayoutStorage::CompiledObject:
        return "CompiledObject"_s;
    case LayoutStorage::OrdinaryObject:
        return "OrdinaryObject"_s;
    case LayoutStorage::Environment:
        return "Environment"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(VariableStorage storage)
{
    switch (storage) {
    case VariableStorage::Statics:
        return "Statics"_s;
    case VariableStorage::None:
        return "None"_s;
    case VariableStorage::ClosureTable:
        return "ClosureTable"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VariableEncoding encoding)
{
    switch (encoding) {
    case VariableEncoding::Plain:
        return "Plain"_s;
    case VariableEncoding::UndefinedAsZero:
        return "UndefinedAsZero"_s;
    case VariableEncoding::FunctionAsZero:
        return "FunctionAsZero"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(MemberCreation creation)
{
    switch (creation) {
    case MemberCreation::AtDefinition:
        return "AtDefinition"_s;
    case MemberCreation::ByClassRecord:
        return "ByClassRecord"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printFlag(PrintStream& out, bool isSet, ASCIILiteral name)
{
    if (isSet)
        out.print(" "_s, name);
}

static void printOptional(PrintStream& out, ASCIILiteral prefix, std::optional<uint32_t> number)
{
    if (number)
        out.print(prefix, *number);
}

void printSlot(PrintStream& out, const PlacedSlot& slot)
{
    switch (slot.storage) {
    case SlotStorage::None:
        out.print("none"_s);
        return;
    case SlotStorage::Inline:
        out.print(nameOf(slot.representation), " @"_s, slot.offset);
        break;
    case SlotStorage::OutOfLine:
        out.print(nameOf(slot.representation), " out@"_s, slot.offset);
        break;
    }
    printFlag(out, slot.mayExistWithoutStorage, "unlisted"_s);
    if (slot.deferredFunction)
        out.print(" "_s, letterOf(IDPrefix::Function), slot.deferredFunction.index());
    printFlag(out, slot.deferredClosureCapturesObject, "capturesObject"_s);
    printFlag(out, slot.deferredClosureCapturesEnvironment, "capturesEnvironment"_s);
    printFlag(out, slot.defersFlattenedObject, "flattenedObject"_s);
    printOptional(out, " record="_s, slot.indexInLayoutRecord);
}

static void dumpHiddenWord(PrintStream& out, ASCIILiteral name, const PlacedSlot& slot)
{
    out.print("  "_s, name, " "_s);
    printSlot(out, slot);
    out.print("\n"_s);
}

static void dumpLayout(IRPrinter& printer, LayoutID id, const PlannedLayout& layout)
{
    PrintStream& out = printer.out();
    printer.print(id);
    out.print(" "_s, nameOf(layout.storage), " size="_s, layout.size);
    if (layout.extensionSize)
        out.print(" extension="_s, layout.extensionSize);
    if (layout.numberOfRecordFields)
        out.print(" fields="_s, layout.numberOfRecordFields);
    printFlag(out, layout.holdsWideReferences, "wide"_s);
    printFlag(out, layout.keysAreNeverEnumerated, "unordered"_s);
    printFlag(out, layout.mayBeInnerObject, "inner"_s);
    out.print("\n"_s);
    for (const PlacedInnerObject& inner : layout.innerObjects) {
        out.print("  inner object of field "_s, inner.field, " "_s);
        if (inner.layout)
            printer.print(inner.layout);
        else
            out.print("array of capacity "_s, inner.arrayCapacity);
        out.print(" @"_s, inner.offset, inner.isAlwaysInPlace ? " always"_s : ""_s, "\n"_s);
    }

    const Vector<Field>& fields = printer.program().at(id).fields;
    for (FieldIndex index = 0; index < layout.numberOfFields(); ++index) {
        const PlacedSlot& slot = layout.slots[index];
        if (!slot.hasStorage())
            continue;
        out.print("  ."_s, index, " "_s);
        printer.print(index < fields.size() ? fields[index].key : PropertyKeyID());
        out.print(" "_s);
        printSlot(out, slot);
        out.print("\n"_s);
    }
    if (layout.hasExtension())
        dumpHiddenWord(out, "extension"_s, layout.slots[layout.extensionWordIndex()]);
    if (layout.hasExtraPropertiesWord())
        dumpHiddenWord(out, "extra"_s, layout.slots[layout.extraPropertiesWordIndex()]);
    for (ReservedBytes bytes : layout.bytesKeptZero)
        out.print("  zero @"_s, bytes.offset, "+"_s, bytes.width, "\n"_s);
}

static bool isDefault(const PlannedVariable& variable)
{
    constexpr PlannedVariable unplanned { };
    return variable.storage == unplanned.storage
        && variable.encoding == unplanned.encoding
        && variable.representation == unplanned.representation
        && variable.isUsedBeforeProgramEntry == unplanned.isUsedBeforeProgramEntry;
}

static void dumpVariable(IRPrinter& printer, VariableID id, const PlannedVariable& variable)
{
    PrintStream& out = printer.out();
    printer.print(id);
    out.print(" "_s, nameOf(variable.storage));
    if (variable.storage == VariableStorage::Statics) {
        out.print(" "_s, nameOf(variable.representation));
        if (variable.encoding != VariableEncoding::Plain)
            out.print(" "_s, nameOf(variable.encoding));
    }
    printFlag(out, variable.isUsedBeforeProgramEntry, "early"_s);
    out.print("\n"_s);
}

static bool isDefault(const ClassCreationPlan& plan)
{
    return !plan.createsMembersLazily && !plan.createsPrototypeBeforeClassObject && !plan.createsClassObjectOnMutation && !plan.guardsHoldWhileClear && plan.members.isEmpty();
}

static void dumpClassCreation(IRPrinter& printer, LayoutID id, const ClassCreationPlan& plan)
{
    PrintStream& out = printer.out();
    out.print("class "_s);
    printer.print(id);
    printFlag(out, plan.createsMembersLazily, "lazy"_s);
    printFlag(out, plan.createsPrototypeBeforeClassObject, "prototypeFirst"_s);
    printFlag(out, plan.createsClassObjectOnMutation, "onMutation"_s);
    printFlag(out, plan.guardsHoldWhileClear, "guardsHoldWhileClear"_s);
    out.print(" "_s);
    printList(out, plan.members, [&](MemberCreation creation) {
        out.print(nameOf(creation));
    });
    out.print("\n"_s);
}

void dumpFillEstimates(PrintStream& out, const Program& program, const SlotUses& uses)
{
    IRPrinter printer(out, program);
    SlotRows rows(program);
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Vector<Field>& fields = program.at(id).fields;
        if (fields.isEmpty() || !uses.rows[rows.rowOf(id, 0)].holderHeat)
            continue;
        auto percentOfHolders = [&](const SlotRowUse& use, double heat) {
            return static_cast<unsigned>(std::min(9999.0, 100 * heat / use.holderHeat));
        };
        printer.print(id);
        out.print(" allocated="_s, uses.layouts[index].creationHeat, "\n"_s);
        for (FieldIndex field = 0; field < fields.size(); ++field) {
            const SlotRowUse& use = uses.rows[rows.rowOf(id, field)];
            out.print("  ."_s, field, " "_s);
            printer.print(fields[field].key);
            out.print(" filled="_s, percentOfHolders(use, use.fillHeat), "%"_s);
            if (use.heatFilledBesideCreation)
                out.print(" besideCreation="_s, percentOfHolders(use, use.heatFilledBesideCreation), "%"_s);
            printFlag(out, use.storesOfConstructorWithValue, "byConstructor"_s);
            printFlag(out, use.isLikelyFilled(), "likely"_s);
            out.print("\n"_s);
        }
    }
}

void SlotPlan::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (unsigned index = 0; index < m_layouts.size(); ++index) {
        if (m_layouts[index].storage != LayoutStorage::None)
            dumpLayout(printer, LayoutID(index), m_layouts[index]);
    }
    for (unsigned index = 0; index < m_variables.size(); ++index) {
        if (!isDefault(m_variables[index]))
            dumpVariable(printer, VariableID(index), m_variables[index]);
    }
    for (unsigned index = 0; index < m_closureForms.size(); ++index) {
        if (m_closureForms[index] != ClosureForm::Slim)
            continue;
        printer.print(FunctionID(index));
        out.print(" "_s, nameOf(ClosureForm::Slim), "\n"_s);
    }
    for (unsigned index = 0; index < m_classCreation.size(); ++index) {
        if (!isDefault(m_classCreation[index]))
            dumpClassCreation(printer, LayoutID(index), m_classCreation[index]);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))
