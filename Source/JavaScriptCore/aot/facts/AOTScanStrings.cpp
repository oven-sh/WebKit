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

static constexpr OpcodeSet opcodesThatNameModulesInScanStrings = opcodesWhere([](Opcode opcode) {
    return PayloadWord::module(opcode) || PayloadWord::referrer(opcode);
});
static constexpr OpcodeSet opcodesThatGoByNumbersOfModulesInScanStrings { Opcode::ModuleNamespace, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::WasLoadedSynchronously, Opcode::SeesMockOf, Opcode::IsLinkedBelow, Opcode::MockSeenBy, Opcode::NamespaceSeenBy };

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

void notePathsOfModulesReadInScan(const BodyValue& value, SummaryBeingMade& summary)
{
    Opcode opcode = value.opcode();
    if (!opcodesThatNameModulesInScanStrings.contains(opcode) || opcodesThatGoByNumbersOfModulesInScanStrings.contains(opcode))
        return;
    if (opcode == Opcode::ImportModule && !value.as<Opcode::ImportModule>().optionsOperand())
        return;
    if (raisesOf(opcode) == ProgramEvent::LoadsModule || opcode == Opcode::ResolveDynamic)
        summary.mayReadPathOfAnyModule = true;
    ModulesNamedInScanStrings named { summary.modulesWhosePathIsRead };
    value.forEachReference(named);
}

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
    case PrimitiveKnownToFolding::Kind::Number:
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StringID textThatValueBeginsWith(const Program& program, BodyValue definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, Vector<uint32_t>& beginningsFound)
{
    static constexpr uint32_t beginsWithNoText = 1;
    static constexpr uint32_t firstText = 2;
    Vector<ValueID, 8> passed;
    auto answer = [&](StringID text) {
        for (ValueID value : passed)
            beginningsFound[value.index()] = text ? text.index() + firstText : beginsWithNoText;
        return text;
    };
    while (definition && !definition.isParameter()) {
        if (uint32_t found = beginningsFound[definition.id().index()])
            return answer(found == beginsWithNoText ? StringID() : StringID(found - firstText));
        passed.append(definition.id());
        ValueID first;
        switch (definition.opcode()) {
        case Opcode::String: {
            StringID text = definition.as<Opcode::String>().string();
            return answer(text && text != program.strings.emptyString() ? text : StringID());
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
            return answer({ });
        }
        if (!first || first >= definition.id())
            return answer({ });
        definition = definitionOf(first);
    }
    return answer({ });
}

PropertyKeyID nameKeyOfAccessInScan(const Program& program, PrimitiveKnownToFolding held, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, Vector<uint32_t>& beginningsFound, StringID& beginning)
{
    beginning = { };
    if (PropertyKeyID key = keyOfPrimitiveInScanStrings(program, held))
        return key;
    if (!held)
        beginning = textThatValueBeginsWith(program, definition, definitionOf, beginningsFound);
    return { };
}

template<typename IDType>
static void sortWithoutRepeatsInScanStrings(Vector<IDType>& list)
{
    std::ranges::sort(list);
    list.shrink(std::ranges::unique(list).begin() - list.begin());
}

void finishListsOfStringsInScan(SummaryBeingMade& summary)
{
    sortWithoutRepeatsInScanStrings(summary.modulesWhosePathIsRead);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
