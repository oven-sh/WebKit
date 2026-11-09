/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTFacts.h"
#include "AOTFunctionKinds.h"
#include "AOTNameClass.h"
#include "AOTPendingStars.h"
#include "AOTSlotPlan.h"
#include "AOTStubCatalog.h"
#include "AOTStubsConstants.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsObjectCopy.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThinSummary.h"
#include "JSCJSValueInlines.h"
#include "PropertyName.h"
#include <algorithm>
#include <limits>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfAccesses()
{
    return {
        Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::PropHas, Opcode::PropDelete,
        Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine,
        Opcode::DefineGetter, Opcode::DefineSetter, Opcode::TryDefineLiveBinding, Opcode::TryReExportFields,
        Opcode::SetFunctionName, Opcode::SetLiteralPrototype, Opcode::CopyDataProperties, Opcode::CopyObject,
        Opcode::GetSuperBase, Opcode::SuperGet, Opcode::SuperSet,
    };
}

static bool isSymbolOfRealmInStraightForm(StraightEmitter& emitter, PropertyKeyID key)
{
    return emitter.program().at(key).kind == PropertyKeyKind::CreatedSymbol && !emitter.plan().hasOneInstance();
}

static PutMode putModeOfStraightForm(ECMAMode mode)
{
    return mode == ECMAMode::Strict ? PutMode::Strict : PutMode::Sloppy;
}

static StraightCallOptions optionsWithNameInStraightAccess(StraightEmitter& emitter, PropertyKeyID key)
{
    return { .numberedImmediate = NumberedImmediate { emitter.mentionOfName(key) } };
}

static StraightArgument nameAsArgumentInStraightAccess(StraightEmitter& emitter, PropertyKeyID key)
{
    return StraightArgument::number(emitter.mentionOfName(key));
}

static GPRReg operandRegisterInStraightAccess(StraightEmitter& emitter, StubKey stub, unsigned operand)
{
    return emitter.catalog().summaryOf(stub).signature.operands[operand].gpr();
}

static StraightArgument makeKeyCellInStraightAccess(StraightEmitter& emitter, const BodyValue& value, PropertyKeyID key, GPRReg operand)
{
    if (emitter.program().at(key).kind == PropertyKeyKind::PrivateName) {
        emitter.fail(value);
        return StraightArgument::inRegister(operand);
    }
    emitter.callStub(value, createConstant(CreatedConstant::Key), { }, { .numberedImmediate = NumberedImmediate { emitter.mentions().add(NumberSpace::KeyCell, key) } });
    emitter.jit().move(emitter.resultOfCall(), operand);
    return StraightArgument::inRegister(operand);
}

static StraightArgument keyArgumentInStraightAccess(StraightEmitter& emitter, const BodyValue& value, GPRReg operand)
{
    if (PropertyKeyID key = value.key())
        return makeKeyCellInStraightAccess(emitter, value, key, operand);
    return value.computedKeyOperand();
}

static void callStubWithKeyCellInStraightAccess(StraightEmitter& emitter, const BodyValue& value, StubKey stub)
{
    StraightArgument key = makeKeyCellInStraightAccess(emitter, value, value.key(), operandRegisterInStraightAccess(emitter, stub, 1));
    emitter.callStub(value, stub, { value.objectOperand(), key });
}

static bool freeNameHasGlobalBindingInStraightAccess(const Program& program, PropertyKeyID key)
{
    if (!program.types.hasStableNumber(key))
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::CreatedSymbol:
        break;
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

static void generateFreeNameRead(StraightEmitter& emitter, const BodyValue& value)
{
    PropertyKeyID key = value.key();
    if (freeNameHasGlobalBindingInStraightAccess(emitter.program(), key)) {
        OptionSet<GlobalBindingFlags> flags { GlobalBindingFlags::SeesScriptBindings };
        if (value.isForTypeOf())
            flags.add(GlobalBindingFlags::IsForTypeOf);
        emitter.callStub(value, AOT::getGlobalBinding(GlobalBindingScope::ScriptsAndGlobalObject), { }, { .numberedImmediate = NumberedImmediate { emitter.mentions().globalBinding(key, flags) } });
    } else {
        RuntimeOperation operation = value.isForTypeOf() ? RuntimeOperation::GetFreeNameForTypeOf : RuntimeOperation::GetFreeName;
        emitter.callOperation(value, operation, { nameAsArgumentInStraightAccess(emitter, key), StraightArgument::word(0) });
    }
    emitter.define(value);
}

static std::optional<NumberedImmediate> answersAtSiteInStraightAccess(StraightEmitter& emitter, const BodyValue& value, PropertyKeyID key, SiteUse use)
{
    const Function& function = emitter.program().at(emitter.function());
    if (liesInThread(key) || function.isModuleBody() || !value.hasPosition())
        return std::nullopt;
    Wire::SourceLocation place = function.locationAt(value.position());
    if (!hasModule(place))
        return std::nullopt;
    return NumberedImmediate { emitter.mentions().siteCache(Vector<uint32_t, 12> { place.module, place.position }, key, MentionList::firstUseOfSiteAnswers + static_cast<uint32_t>(use)) };
}

static void generatePropGet(StraightEmitter& emitter, const BodyValue& value)
{
    if (value.isFreeName()) {
        generateFreeNameRead(emitter, value);
        return;
    }
    PropertyKeyID key = value.key();
    bool hasStubOfItsOwn = emitter.program().nameClassOf(key) == NameClass::Length || isSymbolOfRealmInStraightForm(emitter, key);
    const bool readsSizeOfTable = namesSizeOfTables(emitter.program(), emitter.facts(), key);
    const bool asksShapeFirst = emitter.plan().slots().someLayoutWrittenInHasField(emitter.program().at(emitter.function()).module, key);
    const SiteUse use = !readsSizeOfTable && !asksShapeFirst && emitter.plan().slots().someLayoutHasField(key) ? SiteUse::ReadUnderNameOfField : SiteUse::Read;
    if (std::optional<NumberedImmediate> answers = hasStubOfItsOwn ? std::nullopt : answersAtSiteInStraightAccess(emitter, value, key, use)) {
        StubKey atSite = readsSizeOfTable ? readSizeOfTableAtSite() : asksShapeFirst ? readAtSite(ReceiverAtSite::IsAnyValue) : readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, use);
        emitter.callStub(value, atSite, { value.objectOperand() }, { .numberedImmediate = *answers });
        emitter.define(value);
        emitter.countReadThatAsksItsSite();
        return;
    }
    StubKey stub = emitter.program().nameClassOf(key) == NameClass::Length ? getLengthAtSite() : namesSizeOfTables(emitter.program(), emitter.facts(), key) ? getSizeOfTable() : getByIdCalledByCode();
    if (isSymbolOfRealmInStraightForm(emitter, key))
        stub = getByIdOfSymbolOfRealm();
    emitter.callStub(value, stub, { value.objectOperand() }, optionsWithNameInStraightAccess(emitter, key));
    emitter.define(value);
}

static void putByNameInStraightAccess(StraightEmitter& emitter, const BodyValue& value, PutMode mode)
{
    PropertyKeyID key = value.key();
    StubKey stub = putByIdHashed(mode);
    if (isSymbolOfRealmInStraightForm(emitter, key))
        stub = putByIdOfSymbolOfRealm(mode);
    emitter.callStub(value, stub, { value.objectOperand(), value.valueOperand() }, optionsWithNameInStraightAccess(emitter, key));
}

static void generatePropSet(StraightEmitter& emitter, const BodyValue& value)
{
    if (value.isFreeName()) {
        RuntimeOperation operation = value.mode() == ECMAMode::Strict ? RuntimeOperation::SetFreeName : RuntimeOperation::SetFreeNameSloppy;
        emitter.callOperation(value, operation, { nameAsArgumentInStraightAccess(emitter, value.key()), StraightArgument::word(0), value.valueOperand() });
        return;
    }
    putByNameInStraightAccess(emitter, value, putModeOfStraightForm(value.mode()));
}

static std::optional<int32_t> elementIndexOfNameInStraightAccess(const Program& program, PropertyKeyID key)
{
    const PropertyKey& name = program.at(key);
    if (name.kind != PropertyKeyKind::Text || program.nameClass(name.text) != NameClass::Index)
        return std::nullopt;
    std::optional<uint32_t> index = parseIndex(byteCast<Latin1Character>(program.strings[name.text]));
    if (!index || *index > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return std::nullopt;
    return static_cast<int32_t>(*index);
}

static void generatePropDefine(StraightEmitter& emitter, const BodyValue& value)
{
    if (std::optional<int32_t> index = elementIndexOfNameInStraightAccess(emitter.program(), value.key())) {
        emitter.callStub(value, defineByValCached(), { value.objectOperand(), StraightArgument::boxed(jsNumber(*index)), value.valueOperand() });
        return;
    }
    putByNameInStraightAccess(emitter, value, PutMode::Define);
}

static void generatePropHas(StraightEmitter& emitter, const BodyValue& value)
{
    PropertyKeyID key = value.key();
    if (isSymbolOfRealmInStraightForm(emitter, key))
        callStubWithKeyCellInStraightAccess(emitter, value, inByValCached(PresenceQuestion::In));
    else
        emitter.callStub(value, inById(PresenceQuestion::In), { value.objectOperand() }, optionsWithNameInStraightAccess(emitter, key));
    emitter.defineBoolean(value);
}

static void generatePropDelete(StraightEmitter& emitter, const BodyValue& value)
{
    PropertyKeyID key = value.key();
    PutMode mode = putModeOfStraightForm(value.mode());
    if (isSymbolOfRealmInStraightForm(emitter, key))
        callStubWithKeyCellInStraightAccess(emitter, value, deleteByValCached(mode));
    else if (value.isFreeName())
        emitter.callOperation(value, RuntimeOperation::DeleteFreeName, { nameAsArgumentInStraightAccess(emitter, key) });
    else
        emitter.callStub(value, deleteByIdCached(mode), { value.objectOperand(), nameAsArgumentInStraightAccess(emitter, key) });
    emitter.defineBoolean(value);
}

static void generateAccessorDefinition(StraightEmitter& emitter, const BodyValue& value, uint32_t accessorFlags)
{
    StraightArgument key = keyArgumentInStraightAccess(emitter, value, operationArgumentGPR(2));
    emitter.callOperation(value, RuntimeOperation::DefineAccessor, { value.objectOperand(), key, value.functionOperand(), StraightArgument::word(accessorFlags) });
}

static void generateTryReExportFields(StraightEmitter& emitter, const BodyValue& value)
{
    const Program& program = emitter.program();
    const CallingConventions& conventions = emitter.conventions();
    FunctionID function = value.function();
    auto isHeldAsBoxedWord = [&](unsigned capture) {
        Representation held = closureCaptureRepresentationOf(program, conventions, function, capture);
        return held == Representation::JSValue || held == Representation::Cell;
    };
    if (!conventions.exists(conventions.closureEntryOf(function)) || program.at(function).captures.size() != 2 || !isHeldAsBoxedWord(0) || !isHeldAsBoxedWord(1)) {
        emitter.bring(StraightArgument::boxed(jsBoolean(false)), emitter.takeResultRegister());
        emitter.define(value);
        return;
    }
    uint32_t keySlot = closureCaptureSlotOf(program, conventions, function, value.keyIsFirstCapture() ? 0 : 1);
    RELEASE_ASSERT(!(keySlot & ~starRuleKeyCaptureSlot));
    uint32_t rule = keySlot | (value.reusesDescriptors() ? starRuleReusesDescriptors : 0);
    StraightArgument closureNumber = StraightArgument::number(emitter.mentions().add(NumberSpace::ClosureFunction, function));
    emitter.callOperation(value, RuntimeOperation::TryReExportFields, { value.targetOperand(), value.sourceOperand(), closureNumber, StraightArgument::word(rule) });
    emitter.defineBoolean(value);
}

static void generatePropertyCopy(StraightEmitter& emitter, const BodyValue& value)
{
    Vector<uint32_t, 8> namedKeys;
    for (PropertyKeyID key : value.excludedKeys())
        namedKeys.append(key.index());
    std::span<const uint32_t> names = namedKeys.span();
    std::span<const ValueID> computedKeys = value.excludedOperands();
    bool hasRun = canBePropertyRun(emitter.program(), names);
    size_t count = computedKeys.size() + (hasRun ? 0 : names.size());
    if (count > emitter.wordsOfScratchArea()) {
        emitter.fail(value);
        return;
    }

    uint32_t word = 0;
    if (!hasRun) {
        for (uint32_t name : names) {
            GPRReg result = emitter.takeResultRegister();
            emitter.bring(nameAsArgumentInStraightAccess(emitter, PropertyKeyID(name)), result);
            emitter.boxInForm(Representation::Int32, result);
            emitter.jit().store64(result, emitter.wordOfScratchArea(word++));
        }
    }
    for (ValueID key : computedKeys)
        emitter.storeInScratchArea(key, word++);

    StraightArgument run = hasRun ? StraightArgument::number(emitter.mentions().add(NumberSpace::PropertyRun, names, 1)) : StraightArgument::word(0);
    StraightArgument keys = count ? StraightArgument::addressInScratchArea(0) : StraightArgument::word(0);
    if (value.opcode() == Opcode::CopyDataProperties) {
        emitter.callOperation(value, RuntimeOperation::CopyDataProperties, { value.targetOperand(), value.sourceOperand(), run, StraightArgument::word(count), keys });
        return;
    }
    bool assigns = value.assigns();
    emitter.callStub(value, assigns ? assignToNewObjectFrontEnd() : copyObjectFrontEnd(), { value.sourceOperand(), run, StraightArgument::word(count), keys }, { .calledBuiltin = assigns ? Builtins::ObjectAssign : BuiltinID() });
    emitter.define(value);
}

static void generateSuperGet(StraightEmitter& emitter, const BodyValue& value)
{
    PropertyKeyID key = value.key();
    if (key && !isSymbolOfRealmInStraightForm(emitter, key))
        emitter.callOperation(value, RuntimeOperation::GetSuperPropertyById, { value.baseOperand(), value.receiverOperand(), nameAsArgumentInStraightAccess(emitter, key) });
    else {
        StraightArgument property = keyArgumentInStraightAccess(emitter, value, operandRegisterInStraightAccess(emitter, getSuperByVal(), 1));
        emitter.callStub(value, getSuperByVal(), { value.baseOperand(), property, value.receiverOperand() });
    }
    emitter.define(value);
}

static void generateSuperSet(StraightEmitter& emitter, const BodyValue& value)
{
    StraightArgument key = keyArgumentInStraightAccess(emitter, value, operationArgumentGPR(2));
    emitter.callOperation(value, RuntimeOperation::SuperSet, { value.baseOperand(), key, value.valueOperand(), value.receiverOperand(), StraightArgument::word(static_cast<uint32_t>(value.mode())) });
}

void StraightEmitter::generateAccess(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::PropGet:
        generatePropGet(*this, value);
        return;
    case Opcode::PropSet:
        generatePropSet(*this, value);
        return;
    case Opcode::PropDefine:
        generatePropDefine(*this, value);
        return;
    case Opcode::PropHas:
        generatePropHas(*this, value);
        return;
    case Opcode::PropDelete:
        generatePropDelete(*this, value);
        return;
    case Opcode::ElemGet:
        callStub(value, getByValCached(), { value.objectOperand(), value.propertyOperand() });
        define(value);
        return;
    case Opcode::ElemSet:
        callStub(value, putByValCached(putModeOfStraightForm(value.mode())), { value.objectOperand(), value.propertyOperand(), value.valueOperand() });
        return;
    case Opcode::ElemHas:
        callStub(value, inByValCached(PresenceQuestion::In), { value.objectOperand(), value.propertyOperand() });
        defineBoolean(value);
        return;
    case Opcode::ElemDelete:
        callStub(value, deleteByValCached(putModeOfStraightForm(value.mode())), { value.objectOperand(), value.propertyOperand() });
        defineBoolean(value);
        return;
    case Opcode::ElemDefine:
        callStub(value, defineByValCached(), { value.objectOperand(), value.propertyOperand(), value.valueOperand() }, { .calledBuiltin = value.calledBuiltin() });
        return;
    case Opcode::DefineGetter:
        generateAccessorDefinition(*this, value, accessorIsEnumerable | accessorIsGetter);
        return;
    case Opcode::DefineSetter:
        generateAccessorDefinition(*this, value, accessorIsEnumerable);
        return;
    case Opcode::TryDefineLiveBinding:
        callOperation(value, RuntimeOperation::TryDefineLiveBinding, { value.targetOperand(), value.sourceOperand(), value.sourceKeyOperand(), value.targetKeyOperand(), value.getterOperand() });
        defineBoolean(value);
        return;
    case Opcode::TryReExportFields:
        generateTryReExportFields(*this, value);
        return;
    case Opcode::SetFunctionName:
        callOperation(value, RuntimeOperation::SetFunctionName, { value.functionOperand(), value.computedKeyOperand(), StraightArgument::word(static_cast<uint32_t>(value.methodKind())) });
        return;
    case Opcode::SetLiteralPrototype:
        callOperation(value, RuntimeOperation::SetLiteralPrototype, { value.objectOperand(), value.prototypeOperand(), StraightArgument::word(value.refusesOtherValues()) }, { .calledBuiltin = value.refusesOtherValues() ? Builtins::ObjectCreate : BuiltinID() });
        return;
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject:
        generatePropertyCopy(*this, value);
        return;
    case Opcode::GetSuperBase:
        callOperation(value, RuntimeOperation::GetSuperBase, { value.homeOperand(), StraightArgument::word(static_cast<uint32_t>(value.homeKind())) });
        define(value);
        return;
    case Opcode::SuperGet:
        generateSuperGet(*this, value);
        return;
    case Opcode::SuperSet:
        generateSuperSet(*this, value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
