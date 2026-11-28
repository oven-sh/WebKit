/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTEnforcedFields.h"
#include "AOTFacts.h"
#include "AOTIRPrinter.h"
#include "AOTPlan.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned numberOfSourcesInEnforcedFieldsText = 6;
static_assert(static_cast<unsigned>(SourceOfSlotType::Seen) + 1 == numberOfSourcesInEnforcedFieldsText);

static ASCIILiteral nameInEnforcedFieldsText(SourceOfSlotType source)
{
    switch (source) {
    case SourceOfSlotType::Nothing:
        return "nothing"_s;
    case SourceOfSlotType::Compiler:
        return "compiler"_s;
    case SourceOfSlotType::Written:
        return "written"_s;
    case SourceOfSlotType::WrittenInFamily:
        return "family"_s;
    case SourceOfSlotType::Proven:
        return "proven"_s;
    case SourceOfSlotType::Seen:
        return "seen"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printEnforcedFields(TestCommandContext& context, StagedBuild& stage, const Facts& facts, const Plan& plan, bool printsCountsAlone, bool printsWhatIsLost)
{
    PrintStream& out = context.out;
    Program& program = stage.program();
    IRPrinter printer(out, program);
    TypeArena types(program.types, ArenaKeeps::InProgram);
    bool hasTypedSlots = withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots);
    FamiliesOfInstances familiesOfInstances(program, facts.families());
    WrittenTypesOfFamilies writtenInFamilies(program, facts.families(), types);

    std::array<uint64_t, numberOfSourcesInEnforcedFieldsText> numberBySource { };
    uint64_t numberOfFields = 0;
    uint64_t numberTakenBack = 0;
    uint64_t numberOfInt32sByGuess = 0;
    uint64_t numberSparedTest = 0;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const PlannedLayout& planned = plan.layout(id);
        if (!planned.holdsCompiledObjects())
            continue;
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            FieldReference reference { id, field };
            FieldFacts row = facts.field(reference);
            EnforcedField answer = hasTypedSlots ? enforcedTypeOfField(program, facts.linked(), facts.held(), facts.families(), familiesOfInstances, writtenInFamilies, types, reference) : EnforcedField();
            bool differsFromTable = answer.type != row.slotType || !!answer.type != row.facts.contains(FieldFact::IsEnforced) || answer.holdsUndefinedAndNull != row.facts.contains(FieldFact::SlotHoldsUndefinedAndNull);
            bool sparesTest = answer.type && enforcedTypeSparesTestOfStoresByName(types, answer.type);

            ++numberOfFields;
            ++numberBySource[static_cast<unsigned>(answer.source)];
            numberTakenBack += answer.wasTakenBack;
            numberOfInt32sByGuess += answer.isInt32ByGuess;
            numberSparedTest += sparesTest;
            if (printsWhatIsLost && !answer.type && field < program.at(id).fields.size()) {
                const DeclaredType& declared = fieldAt(program, declarationOf(program, reference)).declared;
                if (declared.isGuarded() && declared.written()) {
                    out.print("lost "_s);
                    printer.print(id);
                    out.print("."_s, field, " "_s);
                    if (PropertyKeyID key = plan.slots().keyOf(program, reference))
                        printer.print(key);
                    out.print(" written="_s);
                    printer.print(declared.written());
                    out.println();
                }
            }
            if (printsCountsAlone || (!answer.type && !answer.wasTakenBack && !differsFromTable))
                continue;

            out.print("field "_s);
            printer.print(id);
            out.print("."_s, field);
            if (PropertyKeyID key = plan.slots().keyOf(program, reference)) {
                out.print(" "_s);
                printer.print(key);
            }
            if (field >= program.at(id).fields.size())
                out.print(" appended"_s);
            if (answer.type) {
                out.print(" source="_s, nameInEnforcedFieldsText(answer.source), " type="_s);
                printer.print(answer.type);
            }
            out.print(answer.wasTakenBack ? " taken-back"_s : ""_s, answer.holdsUndefinedAndNull ? " markers"_s : ""_s, sparesTest ? " spares-test"_s : ""_s, answer.isInt32ByGuess ? " int32-by-guess"_s : ""_s);
            const PlacedSlot& slot = planned.slots[field];
            out.print(" slot="_s, nameOf(slot.representation));
            if (slot.type) {
                out.print(" held="_s);
                printer.print(slot.type);
            }
            out.println(differsFromTable ? " differs-from-table"_s : ""_s);
        }
    }

    auto numberFrom = [&](SourceOfSlotType source) {
        return numberBySource[static_cast<unsigned>(source)];
    };
    uint64_t numberHeld = numberOfFields - numberFrom(SourceOfSlotType::Nothing);
    out.println("counts fields "_s, numberOfFields, " | written "_s, numberFrom(SourceOfSlotType::Written), " | family "_s, numberFrom(SourceOfSlotType::WrittenInFamily), " | proven "_s, numberFrom(SourceOfSlotType::Proven),
        " | compiler "_s, numberFrom(SourceOfSlotType::Compiler), " | taken back "_s, numberTakenBack, " | int32 by guess "_s, numberOfInt32sByGuess, " | every value "_s, numberFrom(SourceOfSlotType::Nothing) - numberTakenBack, " | seen "_s, numberFrom(SourceOfSlotType::Seen));
    out.println("counts stores by name into held slots | may be spared the test "_s, numberSparedTest, " | tested "_s, numberHeld - numberSparedTest);
}

void runFactsCommand(TestCommandContext&);
void runFactsCommand(TestCommandContext& context)
{
    bool printsEnforcedFields = false;
    bool printsCountsAlone = false;
    bool printsWhatIsLost = false;
    for (const String& word : context.arguments) {
        if (word == "enforced"_s)
            printsEnforcedFields = true;
        else if (word == "counts"_s)
            printsCountsAlone = true;
        else if (word == "lost"_s)
            printsWhatIsLost = true;
        else {
            context.out.println("bad argument "_s, word);
            return;
        }
    }
    if (!printsEnforcedFields) {
        context.out.println("bad argument: name the facts to print: enforced"_s);
        return;
    }

    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    const Plan* plan = facts ? stage.plan() : nullptr;
    if (!plan)
        return;
    context.out.print("mechanisms "_s);
    printMechanisms(context.out, facts->mechanisms());
    context.out.println();
    printEnforcedFields(context, stage, *facts, *plan, printsCountsAlone, printsWhatIsLost);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
