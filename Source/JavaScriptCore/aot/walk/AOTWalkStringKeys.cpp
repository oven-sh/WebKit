/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned mostValuesFollowedBehindTextInStringKeys = 4;

static StringID textOfConstantField(const Walker& walker, const BodyValue& read)
{
    if (read.isFreeName())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID holder = index.stripCasts(read.objectOperand());
    if (holder && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::CheckInitialized)
        holder = index.stripCasts(index.value(holder).operand(0));
    if (!holder || index.isParameter(holder) || index.opcodeOf(holder) != Opcode::GetModuleVariable)
        return { };
    VariableID variable = index.value(holder).variable();
    TypeID held = walker.program().at(variable).flags.contains(VariableFlags::IsImmutable) ? walker.positions().variable(TypeLevel::Proven, variable) : TypeID();
    if (!held || holdsOnlyPrimitives(held))
        return { };
    TypeArena& types = walker.types();
    LayoutID layout = singleLayoutOf(types, types.at(types.withOnlyObjects(held)));
    std::optional<FieldReference> field = layout ? findField(walker.program(), layout, read.key()) : std::nullopt;
    const LinkedCell* cell = field ? &walker.facts().linked().field(*field) : nullptr;
    if (!cell || !cell->isFilledAtEveryRead || cell->isWrittenAfterConstruction || !cell->proven)
        return { };
    Type type = types.at(cell->proven);
    return type.hasOnlyPrimitives(Primitive::LiteralString) ? singleStringLiteralOf(type) : StringID();
}

static PrimitiveKnownToFolding keyKnownWhereverAskedInStringKeys(const Walker& walker, ValueID written, ValueID access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID definition = written ? index.stripCasts(written) : ValueID();
    if (!definition)
        return { };
    if (!index.isParameter(definition)) {
        BodyValue operation = index.value(definition);
        switch (operation.opcode()) {
        case Opcode::String:
            return PrimitiveKnownToFolding::text(operation.as<Opcode::String>().string());
        case Opcode::Undefined:
            return PrimitiveKnownToFolding::undefined();
        case Opcode::Null:
            return PrimitiveKnownToFolding::null();
        case Opcode::PropGet:
            if (StringID text = textOfConstantField(walker, operation))
                return PrimitiveKnownToFolding::text(text);
            break;
        case Opcode::Boolean:
            return PrimitiveKnownToFolding::boolean(operation.as<Opcode::Boolean>().boolean());
        default:
            break;
        }
    }
    if (BlockID blockOfKey = index.hasLoops() ? index.blockOf(definition) : BlockID()) {
        bool wayHoldsKeyOfAccess = index.isParameter(definition) && index.isInBlock(access, blockOfKey) && walker.isWalkingForThisWay(blockOfKey);
        LoopID loopOfKey = index.loopOf(blockOfKey);
        if (loopOfKey && !wayHoldsKeyOfAccess && !walker.armHoldsKeyOf(access) && !walker.turnWrittenOutHoldsKeyOf(access, loopOfKey))
            return { };
    }
    Reference bound = walker.referenceBoundTo(definition);
    return bound && bound.isRecord() ? primitiveKnownForStrings(walker, bound) : PrimitiveKnownToFolding();
}

static ALWAYS_INLINE PropertyKeyID nameOfKnownKeyInStringKeys(const Walker& walker, PrimitiveKnownToFolding key)
{
    const Program& program = walker.program();
    StringID text;
    switch (key.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        text = program.strings.textOfUndefined();
        break;
    case PrimitiveKnownToFolding::Kind::Null:
        text = program.strings.textOfNull();
        break;
    case PrimitiveKnownToFolding::Kind::Boolean:
        text = program.strings.textOf(key.asBoolean());
        break;
    case PrimitiveKnownToFolding::Kind::Text:
        text = key.asText();
        break;
    case PrimitiveKnownToFolding::Kind::Number:
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    if (!text || liesInThread(text))
        return { };
    PropertyKeyID name = walker.types().textKey(text);
    if (!name || liesInThread(name) || !program.types.hasStableNumber(name) || mayBeElementKey(program.nameClassOf(name)))
        return { };
    return name;
}

static bool nameFindsOnlyFunctionsThatUnitLeadsToInStringKeys(const Walker& walker, TypeID lookedAt, PropertyKeyID name)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    TypeArena& types = walker.types();
    CodeUnit unit = walker.unit();
    auto mayBeMentioned = [&](FunctionID function) {
        return !function || linked.mayBeMentionedBy(unit, function);
    };
    auto findsOnlySuch = [&](const MemberFound& found) {
        return mayBeMentioned(found.method) && mayBeMentioned(found.getter) && mayBeMentioned(found.setter);
    };

    Type type = types.at(types.withoutNullish(lookedAt));
    for (LayoutID layout : membersOf(types, type)) {
        if (!findsOnlySuch(findMemberAboveObjectsOf(program, linked, layout, name)))
            return false;
        Vector<FunctionID, 4> laidAbove;
        if (linked.findFunctionsLaidAbove(program, layout, name, laidAbove) && !std::ranges::all_of(laidAbove, mayBeMentioned))
            return false;
    }
    ObjectID named = type.isObject() && type.isJavaScriptValue() ? singleObjectOf(type) : ObjectID();
    return !named || findsOnlySuch(findMemberOfNamedObject(program, linked, types.objects().at(named), name));
}

static PropertyKeyID nameThatAccessGoesByInStringKeys(const Walker& walker, ValueID access, ValueID key, ValueID lookedAt)
{
    PropertyKeyID name = nameOfKnownKeyInStringKeys(walker, keyKnownWhereverAskedInStringKeys(walker, key, access));
    if (!name)
        return { };
    Reference start = lookedAt ? walker.referenceBoundTo(lookedAt) : Reference();
    TypeID type = start && start.isRecord() ? walker.typeOf(start) : TypeID();
    return type && nameFindsOnlyFunctionsThatUnitLeadsToInStringKeys(walker, type, name) ? name : PropertyKeyID();
}

PropertyKeyID nameOfTextThatAccessMayGoBy(const Walker& walker, const BodyValue& access, StringID text)
{
    PropertyKeyID name = nameOfKnownKeyInStringKeys(walker, PrimitiveKnownToFolding::text(text));
    if (!name || !access.objectOperand())
        return { };
    Reference start = walker.referenceBoundTo(access.objectOperand());
    TypeID type = start && start.isRecord() ? walker.typeOf(start) : TypeID();
    return type && nameFindsOnlyFunctionsThatUnitLeadsToInStringKeys(walker, type, name) ? name : PropertyKeyID();
}

PropertyKeyID keyOfKnownTextThatAccessGoesBy(const Walker& walker, const BodyValue& access)
{
    if (!accessesUnderComputedKeys.contains(access.opcode()))
        return { };
    return nameThatAccessGoesByInStringKeys(walker, access.id(), access.propertyOperand(), access.objectOperand());
}

static ValueID findReadThatAloneTakesTextAsKeyInStringKeys(const BodyIndex& index, ValueID text)
{
    BlockID block = index.blockOf(text);
    for (unsigned link = 0; link < mostValuesFollowedBehindTextInStringKeys; ++link) {
        ValueID user = index.onlyUsingValue(text);
        if (!user || index.blockOf(user) != block)
            return { };
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::ElemGet)
            return operation.operand(OperandIndex::ElemGet::property) == text ? user : ValueID();
        if (!opcodesThatStateTypes.contains(operation.opcode()) || operation.operand(0) != text)
            return { };
        text = user;
    }
    return { };
}

bool buildReadByPartsOfKey(Walker& walker, const BodyValue& read, const InputsOfTypedOperation& inputs)
{
    if (read.opcode() != Opcode::ElemGet || inputs.areOnlyTypes())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID text = index.stripCasts(read.propertyOperand());
    static constexpr OpcodeSet callsThatMayStandCopied { Opcode::Call, Opcode::CallDirect };
    if (!text || index.isParameter(text) || !(operationsThatJoinTexts | callsThatMayStandCopied).contains(index.opcodeOf(text)) || findReadThatAloneTakesTextAsKeyInStringKeys(index, text) != read.id())
        return false;

    Reference object = walker.referenceOf(read.objectOperand());
    Reference key = walker.referenceOf(read.propertyOperand());
    Reference root = key && key.isRecord() ? rootForStrings(walker, key) : Reference();
    if (!object || !root || !root.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& joined = code.at(root.id());
    bool isGivenByCopy = !operationsThatJoinTexts.contains(index.opcodeOf(text));
    if (std::optional<uint32_t> loop = isGivenByCopy ? walker.innermostOpenLoop() : std::nullopt; loop && walker.standsInFrontOfOpenLoop(*loop, root))
        return false;
    if (joined.opcode != Opcode::Strcat || joined.numberOfVariadicOperands() != 2)
        return false;
    std::array<Reference, 2> parts;
    for (unsigned position = 0; position < parts.size(); ++position) {
        ValueID part = joined.variadicOperands()[position];
        parts[position] = code.isConstant(part) ? Reference::constant(part) : Reference::value(part);
        if (!hasTextByItself(walker.types().at(walker.typeOf(parts[position]))))
            return false;
    }

    Draft written = walker.draftOf(read);
    Draft byParts(Opcode::ElemGetByKeyParts, written.origin());
    walker.chooseConservativeFields(byParts);
    byParts.setObjectOperand(object);
    byParts.setFirstPartOperand(parts[0]);
    byParts.setSecondPartOperand(parts[1]);
    byParts.expected = written.expected;
    byParts.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    byParts.inputValueGiven = read.id();
    if (Reference found = walker.build(WTF::move(byParts)))
        walker.bind(read.id(), found);
    return true;
}

static BuiltinID lockedObjectPrototypeInStringKeys(const Program& program)
{
    const BuiltinTable& table = program.builtins();
    BuiltinClassID objectClass = BuiltinClasses::Object;
    BuiltinID objectPrototype = table.canonicalID(table.at(objectClass).prototype);
    bool isLocked = table.at(objectPrototype).flags.contains(BuiltinFlag::IsLockedObject) && table.openNamesOf(objectPrototype).empty();
    return isLocked ? objectPrototype : BuiltinID();
}

static bool isRecordBelowObjectPrototypeInStringKeys(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction) && !layout.has(LayoutFlags::IsOfCreatedObject);
}

static const LinkedLayout* layoutThatStaysInSightInStringKeys(const Walker& walker, LayoutID id)
{
    const LinkTables& linked = walker.facts().linked();
    if (id.index() >= linked.layouts.size() || linked.layout(id).reachesUnseenCode)
        return nullptr;
    return &linked.layout(id);
}

bool buildLookupUnderAnyKeyOnPlainRecords(Walker& walker, const BodyValue& access, const InputsOfTypedOperation& inputs)
{
    if ((access.opcode() != Opcode::ElemGet && access.opcode() != Opcode::ElemHas) || inputs.areOnlyTypes())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfObject = inputs.typeOf(access.objectOperand());
    TypeID typeOfKey = inputs.typeOf(access.propertyOperand());
    if (!typeOfObject || !typeOfKey)
        return false;
    Type object = types.at(typeOfObject);
    if (!object.isJavaScriptValue() || !object.hasOnlyLayouts() || !hasTextByItself(types.at(typeOfKey)))
        return false;
    if (access.opcode() == Opcode::ElemGet && !lockedObjectPrototypeInStringKeys(walker.program()))
        return false;
    bool mayMakeClosureOfField = false;
    for (LayoutID layout : membersOf(types, object)) {
        const LinkedLayout* known = isRecordBelowObjectPrototypeInStringKeys(walker.program(), layout) ? layoutThatStaysInSightInStringKeys(walker, layout) : nullptr;
        if (!known || known->possibleChanges.containsAny({ LayoutChange::GetsAccessor, LayoutChange::ChangesPrototype }))
            return false;
        mayMakeClosureOfField |= access.opcode() == Opcode::ElemGet && walker.plan().layout(layout).environmentOfDeferredClosures;
    }

    Effects effects = Effects::ofReadByNameThatRunsNoCode();
    if (mayMakeClosureOfField)
        effects.writes.add(AbstractHeap::ObjectFields);
    if (object.mayBeUndefinedOrNull())
        effects.mayThrow = ThrowEffect(true);

    Draft lookup = walker.draftOf(access);
    walker.chooseConservativeFields(lookup);
    if (Reference found = walker.build(WTF::move(lookup), &effects))
        walker.bind(access.id(), found);
    return true;
}

static constexpr OpcodeSet opcodesTakingEveryOperandAsKeyInStringKeys {
    Opcode::StrictEq, Opcode::Eq, Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine, Opcode::SuperGet,
    Opcode::SuperSet, Opcode::GetByValWithThis, Opcode::ToPropertyKey, Opcode::CallBuiltin, Opcode::CallInline
};

static bool isOnlyKeptInStringKeys(OperandUses uses)
{
    return !uses.isEmpty() && uses.containsOnly({ OperandUse::Retains, OperandUse::Returns });
}

constinit const OpcodeSet opcodesThatTakeTextsAsKeys = opcodesTakingEveryOperandAsKeyInStringKeys
    | OpcodeSet { Opcode::DefineGetter, Opcode::DefineSetter, Opcode::SetFunctionName, Opcode::CopyDataProperties, Opcode::CopyObject, Opcode::DefineMethod, Opcode::DefineOwnProperty };

void takeTextsUsedAsKeysAsAtomsOfItsOpcodes(Walker& walker, Draft& draft)
{
    if (!walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return;
    bool takesEveryOperandAsKey = opcodesTakingEveryOperandAsKeyInStringKeys.contains(draft.opcode);
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        if (!operand || !operand.isConstant())
            continue;
        if (!takesEveryOperandAsKey && draft.operandKind(operandIndex) != OperandKind::PropertyKey)
            continue;
        if (isOnlyKeptInStringKeys(draft.operandUses(operandIndex)))
            continue;
        const Operation& text = walker.code().at(operand.id());
        if (text.opcode != Opcode::String || text.isAtom())
            continue;
        Draft atom(Opcode::String, text.origin());
        atom.setString(text.string());
        atom.setIsAtom(true);
        atom.representation = text.representation;
        Reference copy = walker.constant(WTF::move(atom));
        if (!copy)
            return;
        draft.setOperand(operandIndex, copy);
    }
}

static bool memberRunsNoCodeOfProgramInStringKeys(const BuiltinTable& table, BuiltinID location, Protocols resolved)
{
    BuiltinID member = table.canonicalID(location);
    if (table.at(member).origin != BuiltinOrigin::Engine || table.at(member).flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }))
        return false;
    bool hasRowOfCalls = false;
    for (const BuiltinOverload& row : table.overloadsOf(member)) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        hasRowOfCalls = true;
        if (row.isUndeclared() || row.libraryFunction() || (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode)))
            return false;
        if (row.receiver && !row.receiver->protocols.containsOnly(resolved))
            return false;
    }
    return hasRowOfCalls;
}

bool recordFindsNothingAbove(const Walker& walker, LayoutID layout, PropertyKeyID key)
{
    const Program& program = walker.program();
    BuiltinID objectPrototype = lockedObjectPrototypeInStringKeys(program);
    return isRecordBelowObjectPrototypeInStringKeys(program, layout) && objectPrototype && !findOwnMember(program, objectPrototype, key) && listsEveryMemberUnder(program, objectPrototype, key);
}

static bool nameIsFoundNowhereFromObjectsOfInStringKeys(const Walker& walker, LayoutID id, const LinkedLayout& known, PropertyKeyID key)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    if (known.possibleChanges.contains(LayoutChange::ChangesPrototype) || !linked.findsNothing(program, id, ClassSide::Prototype, key, Of::ObjectItself))
        return false;
    if (!isRecordBelowObjectPrototypeInStringKeys(program, id) && !findsNothingAboveObjectsOf(program, linked, id, key))
        return false;
    BuiltinID objectPrototype = lockedObjectPrototypeInStringKeys(program);
    return objectPrototype && !findOwnMember(program, objectPrototype, key) && listsEveryMemberUnder(program, objectPrototype, key);
}

static bool protocolRunsNoCodeOnObjectsOfInStringKeys(const Walker& walker, LayoutID id, Protocol protocol)
{
    const Program& program = walker.program();
    const LinkedLayout* known = isFoundByShapeTest(program.at(id)) ? layoutThatStaysInSightInStringKeys(walker, id) : nullptr;
    if (!known)
        return false;
    switch (protocol) {
    case Protocol::OwnKeys:
        return true;
    case Protocol::OwnValues:
        return !known->possibleChanges.contains(LayoutChange::GetsAccessor);
    case Protocol::PrototypeChain:
        return !known->possibleChanges.contains(LayoutChange::ChangesPrototype) && (isRecordBelowObjectPrototypeInStringKeys(program, id) || known->listsEveryMemberAbove);
    case Protocol::Thenable:
    case Protocol::ToJSON:
    case Protocol::HasInstance:
    case Protocol::ToPrimitive: {
        std::span<const BuiltinName> names = program.builtins().keysReadBy(protocol);
        return !names.empty() && std::ranges::all_of(names, [&](BuiltinName name) {
            return nameIsFoundNowhereFromObjectsOfInStringKeys(walker, id, *known, program.keyOf(name));
        });
    }
    case Protocol::Named:
    case Protocol::CallsNamed:
    case Protocol::Iterate:
    case Protocol::Species:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool protocolFindsNativeMembersOffInStringKeys(const Walker& walker, TypeID objects, Protocol protocol, Protocols resolved)
{
    const Program& program = walker.program();
    std::span<const BuiltinName> names = program.builtins().keysReadBy(protocol);
    return !names.empty() && std::ranges::all_of(names, [&](BuiltinName name) {
        BuiltinID member = findConstantMemberReadOff(walker, objects, program.keyOf(name));
        return member && memberRunsNoCodeOfProgramInStringKeys(program.builtins(), member, resolved);
    });
}

std::optional<Effects> effectsOfProtocolsResolvedOn(Walker& walker, OptionSet<Protocol> protocols, TypeID operand)
{
    constexpr Protocols protocolsNeverResolvedFromType { Protocol::Named, Protocol::CallsNamed, Protocol::Iterate, Protocol::Species };
    constexpr Protocols protocolsThatCallNoMember { Protocol::OwnKeys, Protocol::OwnValues, Protocol::PrototypeChain, Protocol::Thenable };

    if (!walker.has(Mechanism::TypedOperations) || protocols.isEmpty() || protocols.containsAny(protocolsNeverResolvedFromType) || !operand)
        return std::nullopt;
    TypeArena& types = walker.types();
    Type type = types.at(operand);
    if (!type.isJavaScriptValue() || type.mayBeUnspecified() || !type.hasObjects() || type.hasUnboundedUnknown())
        return std::nullopt;
    if (protocols.contains(Protocol::ToJSON) && (type.primitives & Primitive::BigInt))
        return std::nullopt;

    TypeID objects = types.withOnlyObjects(operand);
    Type typeOfObjects = types.at(objects);
    bool areOfProgram = typeOfObjects.hasOnlyLayouts();
    for (Protocol protocol : protocols) {
        bool runsNoCode = areOfProgram
            ? std::ranges::all_of(membersOf(types, typeOfObjects), [&](LayoutID layout) { return protocolRunsNoCodeOnObjectsOfInStringKeys(walker, layout, protocol); })
            : protocolFindsNativeMembersOffInStringKeys(walker, objects, protocol, protocols);
        if (!runsNoCode)
            return std::nullopt;
    }

    Effects effects = Effects::none();
    if (protocolsThatCallNoMember.containsAll(protocols)) {
        effects.reads.add(perObjectHeaps);
        effects.writes.add(AbstractHeap::MemberGuards);
    } else {
        effects = Effects::unknownCode();
        effects.writes.remove(compiledCodeHeaps);
        effects.mayCallUnknownCode = false;
    }
    if (type.primitives) {
        TypingContext typing { walker.program(), types, PositionTypes::knowingNothing() };
        effects.merge(effectsOfProtocolsOn(typing, protocols, types.withoutObjects(operand)));
    }
    if (effects.mayRunOtherCode())
        return std::nullopt;
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
