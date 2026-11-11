/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCellFormat.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTResultRules.h"
#include "AOTStringTable.h"
#include "AOTStubCatalog.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsClosures.h"
#include "AOTStubsConstants.h"
#include "AOTTypeArena.h"
#include "AOTTypeClosure.h"
#include "JSFunction.h"
#include "SmallStrings.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfConstants()
{
    return { Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt, Opcode::Builtin, Opcode::ModuleBuiltin, Opcode::Data, Opcode::NewClosure };
}

static std::optional<uint8_t> singleLatin1CharacterInStraightForm(std::span<const uint8_t> text)
{
    if (text.size() > 2)
        return std::nullopt;
    Vector<char16_t> units = decodeProgramString(text);
    if (units.size() != 1 || units[0] > maxSingleCharacterString)
        return std::nullopt;
    return static_cast<uint8_t>(units[0]);
}

static void generateStringConstant(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg cell = emitter.takeResultRegister();
    StringID string = value.string();
    std::span<const uint8_t> text = emitter.program().string(string);
    if (text.empty())
        jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), cell);
    else if (std::optional<uint8_t> character = singleLatin1CharacterInStraightForm(text)) {
        jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(singleCharacterStrings)), cell);
        jit.loadPtr(CCallHelpers::Address(cell, *character * sizeof(JSString*)), cell);
    } else
        emitter.callStub(value, createConstant(CreatedConstant::String), { }, { .numberedImmediate = NumberedImmediate { emitter.mentions().add(NumberSpace::StringCell, string) } });
    emitter.define(value, cell);
}

static bool firstReadOfBuiltinIsSeenInStraightForm(StraightEmitter& emitter, const BodyValue& value)
{
    const BuiltinTable& table = emitter.program().builtins();
    BuiltinID written = value.builtin();
    if (!standsForFirstReadOffHolder(table, written, value.isNotReadFromHolder()))
        return false;
    return isMadeWithCodeOfLibrary(table, written) || emitter.facts().listsKeysOf(table.canonicalID(table.at(written).holder));
}

static bool fieldOfDataIsPlainPropertyInStraightForm(StraightEmitter& emitter, FieldReference field)
{
    FieldFacts facts = emitter.facts().field(field);
    if (facts.facts.contains(FieldFact::IsAccessor) || !facts.formOfValue.isEmpty())
        return false;
    const Program& program = emitter.program();
    return field.field >= program.at(field.layout).fields.size() || fieldAt(program, declarationOf(program, field)).form.isEmpty();
}

static bool dataFitsTypeOfSlotInStraightForm(StraightEmitter& emitter, TypeID slotType, const DataNode& value)
{
    if (!slotType)
        return true;
    TypeArena& types = emitter.types();
    if (!includesData(types, types.at(slotType), value))
        return false;
    if (value.is<DataNode::Array>())
        return types.isSubset(types.builtinClass(BuiltinClasses::Array), slotType);
    if (!value.is<DataNode::Object>())
        return true;
    TypingContext typing { emitter.program(), types, PositionTypes::knowingNothing() };
    return types.isSubset(typeOfData(typing, value), slotType);
}

static bool tableOfImageTakesMemberInStraightForm(StraightEmitter& emitter, FieldReference field, const DataNode& value)
{
    const PlacedSlot& slot = emitter.plan().slot(field.layout, field.field);
    if (!slot.hasStorage())
        return true;
    if (!slot.indexInLayoutRecord || !fieldOfDataIsPlainPropertyInStraightForm(emitter, field))
        return false;
    FieldFacts facts = emitter.facts().field(field);
    bool isMarkerThatSlotHolds = (value.is<DataNode::Undefined>() || value.is<DataNode::Null>()) && facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull);
    if (slot.provenType && !dataFitsTypeOfSlotInStraightForm(emitter, slot.provenType, value))
        return false;
    return dataFitsTypeOfSlotInStraightForm(emitter, slot.type, value) && (isMarkerThatSlotHolds || dataFitsTypeOfSlotInStraightForm(emitter, facts.slotType, value));
}

static bool tableOfImageHoldsObjectInStraightForm(StraightEmitter& emitter, const DataNode::Object& object)
{
    const Program& program = emitter.program();
    const Plan& plan = emitter.plan();
    LayoutID layout = object.layout;
    bool isCompiledObject = false;
    if (layout) {
        const PlannedLayout& planned = plan.layout(layout);
        switch (planned.storage) {
        case LayoutStorage::CompiledObject:
            if (planned.isRecordAlone)
                return false;
            isCompiledObject = true;
            break;
        case LayoutStorage::OrdinaryObject:
            break;
        case LayoutStorage::None:
        case LayoutStorage::Environment:
            return false;
        }
    }

    Vector<FieldIndex, 16> fields;
    bool hasNamedMemberOutsideFields = false;
    for (const DataMember& member : object.members) {
        if (member.value.is<DataNode::Hole>())
            return false;
        std::optional<FieldIndex> field = isCompiledObject ? plan.slots().findField(program, layout, member.key) : std::nullopt;
        if (!field) {
            NameClass nameClass = program.nameClassOf(member.key);
            if (program.at(member.key).kind != PropertyKeyKind::Text || nameClass == NameClass::Proto)
                return false;
            hasNamedMemberOutsideFields |= nameClass != NameClass::Index;
            continue;
        }
        if (hasNamedMemberOutsideFields || !tableOfImageTakesMemberInStraightForm(emitter, { layout, *field }, member.value))
            return false;
        fields.append(*field);
    }
    return !isCompiledObject || plan.allocation(program, layout, fields.span());
}

static bool tableOfImageHoldsDataInStraightForm(StraightEmitter& emitter, const DataNode& root)
{
    Vector<const DataNode*, 16> aggregates;
    auto add = [&](const DataNode& node) {
        if (node.is<DataNode::Array>() || node.is<DataNode::Object>())
            aggregates.append(&node);
    };
    add(root);
    if (aggregates.isEmpty())
        return false;
    while (!aggregates.isEmpty()) {
        const DataNode& aggregate = *aggregates.takeLast();
        if (const auto* array = aggregate.tryAs<DataNode::Array>()) {
            for (const DataNode& element : array->elements)
                add(element);
            continue;
        }
        const auto& object = aggregate.as<DataNode::Object>();
        if (!tableOfImageHoldsObjectInStraightForm(emitter, object))
            return false;
        for (const DataMember& member : object.members)
            add(member.value);
    }
    return true;
}

static void generateData(StraightEmitter& emitter, const BodyValue& value)
{
    DataID data = value.data();
    if (!tableOfImageHoldsDataInStraightForm(emitter, emitter.program().at(data).root) || !constantListsFieldsHeldFilledInSlots(emitter.program(), emitter.facts().linked(), emitter.plan().slots(), data)) {
        emitter.fail(value);
        return;
    }
    uint32_t words[] { data.index(), static_cast<uint32_t>(DataEncoding::ReadOnlyTree) };
    emitter.callOperation(value, RuntimeOperation::NewFromData, { StraightArgument::number(emitter.mentions().request(RequestKind::Data, words)) });
    emitter.define(value);
}

static bool mayMentionFunctionInStraightForm(StraightEmitter& emitter, FunctionID function)
{
    const LinkTables& linked = emitter.facts().linked();
    return function.index() >= linked.functions.size() || linked.mayBeMentionedBy(emitter.unit(), function);
}

static bool closuresHoldCapturesInStraightForm(StraightEmitter& emitter, FunctionID function)
{
    return emitter.program().at(function).captures.size() > emitter.plan().promotedEnvironments().numberOfRemovedCapturesOf(function);
}

static bool closureIsHeldByTableInStraightForm(StraightEmitter& emitter, FunctionID function)
{
    const Program& program = emitter.program();
    LayoutID constructed = program.at(function).constructedClass;
    if (!constructed || closuresHoldCapturesInStraightForm(emitter, function))
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == function && emitter.facts().isEvaluatedAtMostOnce(constructed);
}

static void fillSlimClosureBehindRegistersInStraightForm(StraightEmitter& emitter, std::span<const ValueID> words)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg cell = emitter.resultOfCall();
    GPRReg word = StraightEmitter::temporary(1);
    for (size_t slot = ClosureRequest::maxSlimClosureCapturesInRegisters; slot < words.size(); ++slot) {
        emitter.load(words[slot], word);
        jit.store64(word, CCallHelpers::Address(cell, offsetOfSlimClosureCaptures + slot * sizeof(uint64_t)));
    }
    jit.storeFence();
}

static void callClosureStubInStraightForm(StraightEmitter& emitter, const BodyValue& value, StubKey stub, StraightArgument request, std::span<const ValueID> words)
{
    static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCapturesInRegisters);
    Vector<StraightArgument, 5> operands { request };
    for (ValueID word : words.first(std::min<size_t>(words.size(), ClosureRequest::maxSlimClosureCapturesInRegisters)))
        operands.append(StraightArgument(word));
    emitter.callStub(value.origin(), stub, operands.span(), { });
    if (words.size() > ClosureRequest::maxSlimClosureCapturesInRegisters)
        fillSlimClosureBehindRegistersInStraightForm(emitter, words);
    emitter.define(value);
}

static void fillFullClosureInStraightForm(StraightEmitter& emitter, FunctionKindData kind, uint32_t closureNumber, std::span<const ValueID> words)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg cell = emitter.resultOfCall();
    GPRReg word = StraightEmitter::temporary(1);
    GPRReg number = StraightEmitter::temporary(2);
    auto storeWordOfInstance = [&](ptrdiff_t offsetInInstance, ptrdiff_t offsetInCell) {
        jit.load64(CCallHelpers::Address(instanceGPR, offsetInInstance), word);
        jit.store64(word, CCallHelpers::Address(cell, offsetInCell));
    };
    if (kind.closureStructureOfKind())
        storeWordOfInstance(AOT_INSTANCE_OFFSET(closures.closureHeaders) + kind.closureStructureOfKind() * sizeof(uint64_t), 0);
    jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(cell, JSObject::butterflyOffset()));
    storeWordOfInstance(AOT_INSTANCE_OFFSET(globalObject), JSCallee::offsetOfScopeChain());
    storeWordOfInstance(AOT_INSTANCE_OFFSET(closures.executables) + static_cast<unsigned>(FunctionTables::executableKind(kind)) * sizeof(JSCell*), JSFunction::offsetOfExecutableOrRareData());
    for (size_t slot = 0; slot < words.size(); ++slot) {
        emitter.load(words[slot], word);
        jit.store64(word, CCallHelpers::Address(cell, offsetOfClosureCaptures + slot * sizeof(uint64_t)));
    }
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    emitter.relocations().moveNumber({ SymbolSpace::Number, closureNumber }, number);
    jit.load64(CCallHelpers::BaseIndex(word, number, CCallHelpers::TimesEight), word);
    jit.store64(word, CCallHelpers::Address(cell, offsetOfClosureEntry));
    jit.storeFence();
}

static void generateNewClosure(StraightEmitter& emitter, const BodyValue& value)
{
    const Program& program = emitter.program();
    const Plan& plan = emitter.plan();
    const CallingConventions& conventions = emitter.conventions();
    MentionList& mentions = emitter.mentions();
    FunctionID function = value.function();
    CodeUnit entry = conventions.closureEntryOf(function);
    if (!conventions.exists(entry) || !mayMentionFunctionInStraightForm(emitter, function)) {
        emitter.fail(value);
        return;
    }

    if (closureIsHeldByTableInStraightForm(emitter, function)) {
        if (closuresHoldCapturesInStraightForm(emitter, function) || !plan.hasClosureCell(function)) {
            emitter.fail(value);
            return;
        }
        mentions.add(NumberSpace::ClosureCell, function);
        emitter.callStub(value, AOT::closureOfTable(), { }, { .numberedImmediate = NumberedImmediate { mentions.add(NumberSpace::ClosureFunction, function) } });
        emitter.define(value);
        return;
    }

    std::span<const ValueID> captures = value.capturesOperands();
    Vector<ValueID, 4> words(FillWith { }, closureCaptureCountsOf(program, conventions, function).captures, ValueID());
    for (unsigned index = 0; index < captures.size(); ++index) {
        Representation form = closureCaptureRepresentationOf(program, conventions, function, index);
        if (form == Representation::Void)
            continue;
        if (form != Representation::JSValue && form != Representation::Cell) {
            emitter.fail(value);
            return;
        }
        words[closureCaptureSlotOf(program, conventions, function, index)] = captures[index];
    }

    FunctionKindData kind = plan.functionKind(program, entry);
    unsigned count = static_cast<unsigned>(words.size());
    switch (plan.slots().closureFormOf(function)) {
    case ClosureForm::Slim:
        if (count > ClosureRequest::maxSlimClosureCaptures)
            break;
        callClosureStubInStraightForm(emitter, value, AOT::newSlimClosure(kind.slimClosureKind(), count), StraightArgument::number(mentions.add(NumberSpace::ClosureFunction, function)), words.span());
        return;
    case ClosureForm::Full: {
        uint32_t allocation = mentions.allocation(plan.closureAllocation(count), { });
        if (kind.closureStructureOfKind() || count > ClosureRequest::maxNumberOfCaptures) {
            emitter.callStub(value, AOT::allocateCell(CellInitialization::Header), { StraightArgument::number(allocation) });
            fillFullClosureInStraightForm(emitter, kind, mentions.add(NumberSpace::ClosureFunction, function), words.span());
            emitter.define(value);
            return;
        }
        StubKey stub = AOT::newClosure(count);
        GPRReg request = emitter.catalog().summaryOf(stub).signature.operands[0].gpr();
        uint64_t restOfRequest = ClosureRequest::encode(0, static_cast<unsigned>(FunctionTables::executableKind(kind)), 0);
        emitter.relocations().moveNumber({ SymbolSpace::Number, allocation }, request);
        emitter.jit().lshift64(CCallHelpers::TrustedImm32(ClosureRequest::allocationShift), request);
        GPRReg functionAndRest = emitter.takeResultRegister();
        emitter.relocations().moveNumber({ SymbolSpace::Number, mentions.add(NumberSpace::ClosureFunction, function, static_cast<int32_t>(restOfRequest)) }, functionAndRest);
        emitter.jit().or64(functionAndRest, request);
        callClosureStubInStraightForm(emitter, value, stub, StraightArgument::inRegister(request), words.span());
        return;
    }
    case ClosureForm::Deferred:
    case ClosureForm::Singleton:
        break;
    }
    emitter.fail(value);
}

void StraightEmitter::generateConstant(const BodyValue& value)
{
    static constexpr OpcodeSet constantsThatOnlyTheirUsesSee { Opcode::Absent, Opcode::String, Opcode::BigInt, Opcode::Data, Opcode::NewClosure };
    if (constantsThatOnlyTheirUsesSee.contains(value.opcode()) && index().isUnused(value.id()))
        return;

    switch (value.opcode()) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
        defineConstant(value);
        return;
    case Opcode::Absent:
        jit().move(CCallHelpers::TrustedImm64(JSValue::encode(JSValue())), takeResultRegister());
        define(value);
        return;
    case Opcode::String:
        generateStringConstant(*this, value);
        return;
    case Opcode::BigInt:
        callOperation(value, RuntimeOperation::BigIntConstant, { StraightArgument::number(mentions().add(NumberSpace::BigInt, value.digits())) });
        define(value);
        return;
    case Opcode::Builtin: {
        BuiltinCellKey key = BuiltinCellKey::object(program().builtins().canonicalID(value.builtin()));
        if (!key.hasCell(program().builtins()) || value.isNotReadFromHolder()) {
            fail(value);
            return;
        }
        if (BuiltinCellKey read = BuiltinCellKey::value(value.builtin()); read.hasCell(program().builtins()) && firstReadOfBuiltinIsSeenInStraightForm(*this, value)) {
            callStub(value, AOT::builtinCell(), { }, { .numberedImmediate = NumberedImmediate { mentions().builtinCell(read) } });
            if (index().isUnused(value.id()))
                return;
        }
        callStub(value, AOT::builtinCell(), { }, { .numberedImmediate = NumberedImmediate { mentions().builtinCell(key) } });
        define(value);
        return;
    }
    case Opcode::ModuleBuiltin:
        callOperation(value, RuntimeOperation::ModuleBuiltinCell, { StraightArgument::word(value.builtin().index()), StraightArgument::word(value.module().index()) });
        define(value);
        return;
    case Opcode::Data:
        generateData(*this, value);
        return;
    case Opcode::NewClosure:
        generateNewClosure(*this, value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
