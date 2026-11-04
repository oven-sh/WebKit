/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkStrings.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

// ---- Texts that meet others

// Where texts meet, a value is one of a few texts that no written type need name: at a parameter of a block, and behind a call of a function that returns them.
void noteTextThatMeetsOthersInScan(const BodyValue& definition, ThinSummary& summary)
{
    if (!definition || definition.isParameter() || definition.opcode() != Opcode::String)
        return;
    StringID text = definition.as<Opcode::String>().string();
    Vector<StringID>& texts = summary.textsThatMeetOthers;
    if (text && (texts.isEmpty() || texts.last() != text))
        texts.append(text);
}

void noteTextsThatMeetBehindTerminatorInScan(const BodyTerminator& terminator, const BodyValue& definitionOfWhatIsReturned, ThinSummary& summary)
{
    if (terminator.kind() == TerminatorKind::Return) {
        noteTextThatMeetsOthersInScan(definitionOfWhatIsReturned, summary);
        return;
    }
    if (terminator.kind() != TerminatorKind::SwitchString)
        return;
    for (StringID text : terminator.as<TerminatorKind::SwitchString>().strings()) {
        if (text)
            summary.textsThatMeetOthers.append(text);
    }
}

// ---- Paths of modules

static constexpr OpcodeSet opcodesThatNameModulesInScanStrings = opcodesWhere([](Opcode opcode) {
    return PayloadWord::module(opcode) || PayloadWord::referrer(opcode);
});
// The run time finds the module of each by its number. What the walk builds of EvaluateModule and RecordRequire is the plan's to say: ModuleEvaluationPlan::isPathNamedBySites(). A module that may be mocked is known by its path anyway.
// WHATEVER ELSE NAMES A MODULE READS ITS PATH: an operation that is new to the schema does, until somebody has looked.
static constexpr OpcodeSet opcodesThatGoByNumbersOfModulesInScanStrings { Opcode::ModuleNamespace, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::WasLoadedSynchronously, Opcode::SeesMockOf, Opcode::MockSeenBy };

namespace {

struct ModulesNamedInScanStrings {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role...)
    {
        if constexpr (std::is_same_v<IDType, ModuleID>) {
            if (id && (modules.isEmpty() || modules.last() != id))
                modules.append(id);
        }
    }

    Vector<ModuleID>& modules;
};

} // anonymous namespace

void notePathsOfModulesReadInScan(const BodyValue& value, ThinSummary& summary)
{
    Opcode opcode = value.opcode();
    if (!opcodesThatNameModulesInScanStrings.contains(opcode) || opcodesThatGoByNumbersOfModulesInScanStrings.contains(opcode))
        return;
    // The run time finds the module of an import() by its number. Only where attributes arrive does it look, under the path of the file, for the form that they ask for.
    if (opcode == Opcode::ImportModule && !value.as<Opcode::ImportModule>().optionsOperand())
        return;
    // A name that is made when the program runs may be that of any file, and `require.resolve(name)` hands the path of what it finds to the program.
    if (raisesOf(opcode) == ProgramEvent::LoadsModule || opcode == Opcode::ResolveDynamic)
        summary.mayReadPathOfAnyModule = true;
    // No RecordRequire stands beside a RequireModule: the run time itself keeps who asked, as the parent of the module and as the file that the host resolves a mocked name against.
    ModulesNamedInScanStrings named { summary.modulesWhosePathIsRead };
    value.forEachReference(named);
}

// ---- Keys

// An element key goes by the elements, and `__proto__` finds the link of the object where it is assigned and a property where it is defined. A text without a key in the program's table names nothing that the program spells.
static PropertyKeyID keyOfTextInScanStrings(const Program& program, StringID text)
{
    if (!text || text.index() >= program.strings.size())
        return { };
    if (mayBeElementKey(program.nameClass(text)))
        return { };
    if (program.nameClass(text) == NameClass::Proto)
        return { };
    std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
    return key && program.types.hasStableNumber(*key) ? *key : PropertyKeyID();
}

static PropertyKeyID keyOfPrimitiveInScanStrings(const Program& program, PrimitiveKnownToFolding held)
{
    switch (held.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return keyOfTextInScanStrings(program, program.strings.textOfUndefined());
    case PrimitiveKnownToFolding::Kind::Null:
        return keyOfTextInScanStrings(program, program.strings.textOfNull());
    case PrimitiveKnownToFolding::Kind::Boolean:
        return keyOfTextInScanStrings(program, program.strings.textOf(held.asBoolean()));
    case PrimitiveKnownToFolding::Kind::Text:
        return keyOfTextInScanStrings(program, held.asText());
    // Whatever number it is, it is printed as an index or as another canonical numeric string.
    case PrimitiveKnownToFolding::Kind::Number:
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Where the left operand of a sum is a string the sum is that string with something behind it, whatever the right operand is converted to.
StringID textThatValueBeginsWith(const Program& program, BodyValue definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf)
{
    while (definition && !definition.isParameter()) {
        ValueID first;
        switch (definition.opcode()) {
        case Opcode::String: {
            StringID text = definition.as<Opcode::String>().string();
            return text && text != program.strings.emptyString() ? text : StringID();
        }
        case Opcode::Add:
            first = definition.as<Opcode::Add>().leftOperand();
            break;
        case Opcode::Strcat: {
            std::span<const ValueID> parts = definition.as<Opcode::Strcat>().partsOperands();
            if (!parts.empty())
                first = parts[0];
            break;
        }
        default:
            return { };
        }
        // Each step leads to a value that stands in front.
        if (!first || first >= definition.id())
            return { };
        definition = definitionOf(first);
    }
    return { };
}

PropertyKeyID nameKeyOfAccessInScan(const Program& program, PrimitiveKnownToFolding held, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, StringID& beginning)
{
    beginning = { };
    if (PropertyKeyID key = keyOfPrimitiveInScanStrings(program, held))
        return key;
    // A key that is known and is no name goes by its value, and nobody needs to be told how it begins.
    if (!held)
        beginning = textThatValueBeginsWith(program, definition, definitionOf);
    return { };
}

// ---- Behind the body

template<typename IDType>
static void sortWithoutRepeatsInScanStrings(Vector<IDType>& list)
{
    std::ranges::sort(list);
    list.shrink(std::ranges::unique(list).begin() - list.begin());
    list.shrinkToFit();
}

void finishListsOfStringsInScan(ThinSummary& summary)
{
    sortWithoutRepeatsInScanStrings(summary.textsThatMeetOthers);
    sortWithoutRepeatsInScanStrings(summary.modulesWhosePathIsRead);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
