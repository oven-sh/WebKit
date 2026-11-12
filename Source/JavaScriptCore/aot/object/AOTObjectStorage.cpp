/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTCompiledFrame.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTState.h"
#include "BuiltinExecutables.h"
#include "BuiltinNames.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include "StackVisitor.h"
#include <bit>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Object::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Object) };

CompiledStructure* Object::createStructure(VM& vm, const CompiledStructure::ObjectParameters& parameters)
{
    unsigned flags = parameters.hasOtherPrototype ? StructureFlags | IsPristine : StructureFlags;
    return CompiledStructure::createForObject(vm, TypeInfo(CompiledObjectType, flags), info(), parameters);
}

Object* Object::create(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    ASSERT(size >= layout.size);
    Object* object = new (NotNull, allocateCell<Object>(vm, size)) Object(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), size - sizeof(JSCell) });
    bool isBornUnfinished = false;
    for (const FieldData& field : layout.fields()) {
        if (!field.isOutOfLine() && bitsBeforeCreation(field.slot.representation()))
            object->clear(field);
        isBornUnfinished |= field.isFilledAtBirth;
    }
    object->at<uint32_t>(offsetOfOrderWord) = orderWord;
    object->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    object->setIsInBirth(isBornUnfinished);
    object->JSCell::finishCreation(vm);
    return object;
}

Instance& Object::instance() const
{
    Instance* instance = structure()->realm()->aotInstance();
    return *instance;
}

State& Object::state() const
{
    return *instance().state;
}

VM& Object::vm() const
{
    return *instance().vm;
}

void* Object::slotOf(const FieldData& field) const
{
    if (!field.isOutOfLine()) [[likely]]
        return &at<uint8_t>(field.slot.offset);
    Extension* extension = this->extension();
    return extension ? extension->slotOrNull(field.slot.offset) : nullptr;
}

bool Object::isInSlot(const FieldData& field) const
{
    const void* slot = slotOf(field);
    SlotRepresentation representation = field.slot.representation();
    if (!slot || slotHoldsAbsent(representation, slot))
        return false;
    if (isListedByKeyOrderAlone(representation) && readSlotBits(representation, slot) == bitsBeforeCreation(representation)) [[unlikely]]
        return isInKeyOrder(state(), field);
    return true;
}

bool Object::holdsEveryFieldHeldFilled() const
{
    for (const FieldData& field : layout().fields()) {
        if (field.isHeldFilled && !isInSlot(field))
            return false;
    }
    return true;
}

JSValue Object::read(const FieldData& field) const
{
    SlotRepresentation representation = field.slot.representation();
    if (representation == SlotRepresentation::DeferredClosure)
        return isInSlot(field) ? const_cast<Object*>(this)->materializeDeferredClosure(vm(), field) : JSValue();
    const void* slot = slotOf(field);
    State& state = this->state();
    if (isListedByKeyOrderAlone(representation) && !isInSlot(field)) [[unlikely]]
        return { };
    JSValue value = slot ? readSlot(representation, slot, state.instance) : JSValue();
    if (!value && field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field) && !livesInExtraProperties(*state.instance.vm, *this, field))
        return jsUndefined();
    return value;
}

bool Object::tryStore(VM& vm, const FieldData& field, JSValue value)
{
    return tryInitialize(vm, field, value);
}

bool Object::tryInitialize(VM& vm, const FieldData& field, JSValue value)
{
    std::optional<AcceptedValue> accepted = state().objects->layouts.accept(field, value);
    return accepted && store(vm, field, *accepted);
}

bool Object::store(VM& vm, const FieldData& field, AcceptedValue accepted)
{
    return store(vm, field, accepted.value());
}

ALWAYS_INLINE_INTO_OTHER_FILES void Object::willStoreMarker(const FieldData& field)
{
    if (field.holdsRawValueFromBirth)
        setBearsMark(true);
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    if (value.isUndefined() && field.flags & fieldMayExistWithoutStorage && !slotOf(field)) {
        willStoreMarker(field);
        return true;
    }
    ensureStorageFor(vm, field);
    void* slot = slotOf(field);
    if (!slot)
        return false;
    SlotRepresentation representation = field.slot.representation();
    if (!tryWriteSlot(instance(), representation, slot, value))
        return false;
    if (value.isUndefinedOrNull())
        willStoreMarker(field);
    if (traitsOf(representation).holdsCell)
        vm.writeBarrier(this, value);
    return true;
}

void Object::ensureStorageFor(VM& vm, const FieldData& field)
{
    if (!field.isOutOfLine())
        return;
    ASSERT(field.slot.offset + widthOf(field.slot.representation()) <= layout().extensionSize());
    ensureExtensionGranules(vm, granuleBitOf(field.slot.offset));
}

void Object::ensureExtensionGranules(VM& vm, uint64_t granuleBits)
{
    ensureExtensionGranules(vm, granuleBits, vm.heap.mutatorShouldBeFenced());
}

// While the collector may be visiting the Extension, granules do not move inside it: a granule that belongs between others goes into a copy, and the old Extension stays as it is.
void Object::ensureExtensionGranules(VM& vm, uint64_t granuleBits, bool collectorMayVisit)
{
    ASSERT(layout().hasExtension());
    Extension* current = extension();
    uint64_t held = current ? current->granuleBits() : 0;
    uint64_t missing = granuleBits & ~held;
    if (!missing || (current && current->tryAppend(missing)))
        return;
    if (!current)
        missing |= layout().granulesOfFirstExtension();
    if (current && !collectorMayVisit && current->tryInsert(missing))
        return;
    Extension* replacement = Extension::create(vm, held | missing);
    const uint64_t* from = current ? current->granules() : nullptr;
    uint64_t* to = replacement->granules();
    for (uint64_t remaining = held | missing; remaining; remaining &= remaining - 1, ++to) {
        if (held & remaining & -remaining)
            *to = *from++;
    }
    setExtension(vm, replacement);
}

void Object::setExtension(VM& vm, Extension* extension)
{
    std::optional<SlotData> slot = compiledStructure().extensionSlot();
    RELEASE_ASSERT(slot);
    WTF::storeStoreFence();
    if (slot->representation() == SlotRepresentation::Cell)
        at<Extension*>(slot->offset) = extension;
    else {
        RELEASE_ASSERT(slot->representation() == SlotRepresentation::Cell32);
        at<uint32_t>(slot->offset) = extension ? MarkedBlockCage::encode(cageBaseOf(this), reinterpret_cast<uint8_t*>(extension) - offsetOfEmptyExtensionInCage) : 0;
    }
    vm.writeBarrier(this);
}

void Object::clear(const FieldData& field)
{
    if (void* slot = slotOf(field))
        clearSlot(field.slot.representation(), slot);
}

template<typename Visitor>
void Object::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSCell::visitChildren(cell, visitor);
    auto* object = uncheckedDowncast<Object>(cell);
    visitFields(object, object->compiledStructure(), visitor);
}

DEFINE_VISIT_CHILDREN(Object);

size_t Object::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Object::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    const ProgramIdentifiers& identifiers = *identifiersOfRealm(cell->vm(), cell->structure()->realm());
    uintptr_t cageBase = cageBaseOf(object);
    for (const FieldData& field : object->layout().fields()) {
        SlotRepresentation representation = field.slot.representation();
        if (!traitsOf(representation).holdsCell)
            continue;
        const void* slot = object->slotOf(field);
        JSCell* target = slot ? cellInSlot(representation, slot, cageBase) : nullptr;
        if (!target)
            continue;
        if (UniquedStringImpl* name = identifiers.tryGetIdentifierConcurrently(field.name))
            analyzer.analyzePropertyNameEdge(object, target, name);
    }
    JSObject* extraProperties = extraPropertiesIfCreated(*object);
    if (!extraProperties)
        return;
    for (const auto& entry : extraProperties->structure()->getPropertiesConcurrently()) {
        if (JSValue value = extraProperties->getDirect(entry.offset()); value && value.isCell())
            analyzer.analyzePropertyNameEdge(object, value.asCell(), entry.key());
    }
}

Extension* Extension::create(VM& vm, uint64_t granuleBits)
{
    RELEASE_ASSERT(granuleBits);
    void* storage = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>()->allocate(vm, extensionCellSizeFor(std::popcount(granuleBits)), nullptr, AllocationFailureMode::Assert);
    static_assert(minExtensionSize == MarkedBlock::atomSize);
    zeroSpan(std::span { static_cast<uint8_t*>(storage), static_cast<HeapCell*>(storage)->cellSize() });
    Extension* extension = static_cast<Extension*>(storage);
    extension->m_granuleBits = granuleBits;
    vm.mutatorFence();
    return extension;
}

bool Extension::tryAppend(uint64_t granuleBits)
{
    ASSERT(m_granuleBits && granuleBits && !(m_granuleBits & granuleBits));
    if (std::countr_zero(granuleBits) < std::bit_width(m_granuleBits))
        return false;
    uint64_t bitsAfter = m_granuleBits | granuleBits;
    if (static_cast<uint32_t>(std::popcount(bitsAfter)) > extensionCapacityFor(std::popcount(m_granuleBits)))
        return false;
    m_granuleBits = bitsAfter;
    return true;
}

bool Extension::tryInsert(uint64_t granuleBits)
{
    ASSERT(m_granuleBits && granuleBits && !(m_granuleBits & granuleBits));
    uint64_t bitsAfter = m_granuleBits | granuleBits;
    uint32_t from = std::popcount(m_granuleBits);
    uint32_t to = std::popcount(bitsAfter);
    if (to > extensionCapacityFor(from))
        return false;
    uint64_t* words = granules();
    for (uint64_t remaining = bitsAfter; from != to;) {
        uint64_t last = 1ull << (std::bit_width(remaining) - 1);
        remaining &= ~last;
        --to;
        words[to] = m_granuleBits & last ? words[--from] : 0;
    }
    m_granuleBits = bitsAfter;
    return true;
}

[[noreturn]] static void crashOnFalseProof(const Object& object, ObjectMutation mutation, const FieldData* field)
{
    RuntimeCheckFailure failure("object mutation check failed"_s);
    failure.atTopCall(object.vm());
    failure.detail(nameOf(mutation), " on ", reportedObjectName(object.layout()));
    if (field && field->name && !field->isPrivate())
        failure.detail(", property '", String(propertyNameOf(object.vm(), object, *field)), "'");
    failure.detail(", but the compiler proved that this never happens");
    failure.report();
    CRASH_WITH_INFO(std::bit_cast<uintptr_t>(&object));
}

void crashOnImpossibleMutation(VM&, const Object& object, ObjectMutation mutation)
{
    crashOnFalseProof(object, mutation, nullptr);
}

bool isAskedByCodeThatInspectorBrought(VM& vm, const Object& object)
{
    return isAskedByCodeThatInspectorBrought(vm, object.state().instance);
}

static bool isOfInjectedScriptOfInspector(VM& vm, CodeBlock* codeBlock)
{
    SourceCode whole = vm.builtinExecutables()->injectedScriptSourceCreateInspectorInjectedScriptCodeSource();
    const SourceCode& source = codeBlock->ownerExecutable()->source();
    return source.provider() == whole.provider() && source.startOffset() >= whole.startOffset() && source.endOffset() <= whole.endOffset();
}

bool isAskedByCodeThatInspectorBrought(VM& vm, const Instance& instance, FrameThatAsks frameThatAsks)
{
    if (!hasFlag(instance.options().admitted(), AdmittedCode::Inspector))
        return false;
    bool isAskedByConsole = false;
    bool textWithoutOriginAsks = false;
    bool standsInEnteredFrame = frameThatAsks == FrameThatAsks::IsAboveTheTopFrame;
    StackVisitor::visitWithCompiledFrames(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (visitor->isCompiledFrame()) {
            if (!std::exchange(standsInEnteredFrame, false))
                return IterationStatus::Done;
            CompiledFrameIterator& frames = visitor.compiledFrames();
            frames.advanceToCallerMachineFrame();
            return frames.isAtEnd() || frames.isAmongFramesThatWait() ? IterationStatus::Continue : IterationStatus::Done;
        }
        CodeBlock* codeBlock = visitor->codeBlock();
        if (!codeBlock)
            return IterationStatus::Continue;
        if (codeBlock->unlinkedCodeBlock()->isBuiltinFunction()) {
            if (!textWithoutOriginAsks || !isOfInjectedScriptOfInspector(vm, codeBlock))
                return IterationStatus::Continue;
            isAskedByConsole = true;
            return IterationStatus::Done;
        }
        const SourceOrigin& origin = codeBlock->ownerExecutable()->sourceOrigin();
        if (ScriptFetcher* fetcher = origin.fetcher(); fetcher && fetcher->fetcherType() == ScriptFetcher::Type::NodeVM)
            return IterationStatus::Continue;
        if (origin.isNull()) {
            textWithoutOriginAsks = true;
            return IterationStatus::Continue;
        }
        isAskedByConsole = !textWithoutOriginAsks && !(origin.url().protocolIsFile() && (findModule(origin.url().fileSystemPath()) || isOriginOfCompiledFile(instance, origin)));
        return IterationStatus::Done;
    }, false, StackVisitor::CompiledFrameVisit::EachRunOnce);
    return isAskedByConsole;
}

bool refusesMutation(VM& vm, const Object& object, ObjectMutation mutation)
{
    if (!isAskedByCodeThatInspectorBrought(vm, object))
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!scope.exception())
        throwTypeError(object.structure()->realm(), scope, changeThatCompiledCodeRulesOutMessage(mutation));
    return true;
}

void crashOnImpossibleMutation(VM&, const Object& object, PropertyName, ObjectMutation mutation)
{
    crashOnFalseProof(object, mutation, nullptr);
}

void crashOnImpossibleMutation(VM&, const Object& object, const FieldData& field, ObjectMutation mutation)
{
    crashOnFalseProof(object, mutation, &field);
}

UniquedStringImpl* propertyNameOf(VM& vm, const Object& object, const FieldData& field)
{
    return object.state().objects->layouts.nameOf(vm, field);
}

static JSObject* extraPropertiesThatMayHold(const Object& object, const FieldData& field)
{
    return isBoundByPendingStar(object, field).value_or(false) ? object.extraProperties() : extraPropertiesIfCreated(object);
}

FieldProperty findFieldProperty(VM& vm, const Object& object, const FieldData& field)
{
    JSValue value = object.readInline(field);
    if (!value && !field.isOutOfLine())
        value = object.read(field);
    if (value)
        return { value, object.attributesOf(field), true };
    JSObject* extra = extraPropertiesThatMayHold(object, field);
    if (!extra)
        return { };
    unsigned attributes;
    value = extra->getDirect(vm, propertyNameOf(vm, object, field), attributes);
    if (!value)
        return { };
    return { value, field.isPrivate() ? static_cast<unsigned>(PropertyAttribute::None) : attributes, false };
}

bool livesInExtraProperties(VM& vm, const Object& object, const FieldData& field)
{
    if (std::optional<bool> isBound = isBoundByPendingStar(object, field))
        return *isBound;
    JSObject* extra = extraPropertiesIfCreated(object);
    return extra && extra->getDirect(vm, propertyNameOf(vm, object, field));
}

bool tryCreateField(VM& vm, State& state, Object& object, const FieldData& field, JSValue value)
{
    ASSERT(!object.isInSlot(field) && !livesInExtraProperties(vm, object, field));
    std::optional<AcceptedValue> accepted = state.objects->layouts.accept(field, value);
    if (!accepted || !object.store(vm, field, *accepted))
        return false;
    object.didCreateField(state, field, false);
    if (field.isFilledAtBirth)
        object.tryFinishBirth();
    if (object.structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    return true;
}

void putProperty(VM& vm, JSObject* holder, PropertyName name, Object::LeavingProperty property)
{
    if (property.attributes & PropertyAttribute::Accessor)
        holder->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(property.valueOrAccessor), property.attributes);
    else if (property.attributes & PropertyAttribute::CustomAccessorOrValue)
        holder->putDirectCustomAccessor(vm, name, property.valueOrAccessor, property.attributes);
    else
        holder->putDirect(vm, name, property.valueOrAccessor, property.attributes);
}

void didChangeOwnProperties(VM& vm, Object& object, VM::StructureChainIntegrityEvent event, UniquedStringImpl* nameOrNull)
{
    if (!object.perCellBit() && object.hasAccessorAmongExtraProperties()) [[unlikely]]
        object.setBearsMark(true);
    if (object.structure()->mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(event, &object, nameOrNull);
}

bool interceptsIndexedAccesses(const Object& object)
{
    JSObject* extra = extraPropertiesIfCreated(object);
    return extra && extra->structure()->mayInterceptIndexedAccesses();
}

void didChangeIndexedProperties(VM& vm, Object& object, bool interceptedIndexedAccessesBefore)
{
    if (!interceptedIndexedAccessesBefore && object.mayBePrototype() && interceptsIndexedAccesses(object))
        object.structure()->realm()->haveABadTime(vm, &object);
}

const ClassInfo Environment::s_info = { "JSLexicalEnvironment"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Environment) };

static_assert(offsetOfEnvironmentShapeLowByte == JSCell::indexingTypeAndMiscOffset() && offsetOfEnvironmentShapeHighByte == JSCell::typeInfoFlagsOffset());

CompiledStructure* EnvironmentShapes::structureFor(VM& vm, Instance& instance, uint32_t shape)
{
    uint16_t page = shape >> environmentShapesPerPageShift;
    while (structuresByPage.size() <= page)
        structuresByPage.append(nullptr);
    if (!structuresByPage[page]) {
        CompiledStructure* created = Environment::createStructure(vm, instance, *this, page);
        structuresByPage[page] = created;
        instance.state->immortalCells->add(created);
    }
    return structuresByPage[page];
}

CompiledStructure* Environment::createStructure(VM& vm, Instance& instance, const EnvironmentShapes& shapes, uint16_t shapePage)
{
    return CompiledStructure::createForEnvironment(vm, instance.globalObject, TypeInfo(CompiledEnvironmentType, StructureFlags), info(), shapes, shapePage);
}

Environment* Environment::create(VM& vm, Structure* structure, const EnvironmentData& layout, size_t size)
{
    ASSERT(size >= layout.size);
    ASSERT(layout.kind != LayoutKind::Closure);
    void* cell = vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>()->allocate(vm, WTF::roundUpToMultipleOf<atomSize>(size), nullptr, AllocationFailureMode::Assert);
    static_cast<JSCell*>(cell)->clearStructure();
    Environment* environment = new (NotNull, cell) Environment(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(environment) + sizeof(JSCell), size - sizeof(JSCell) });
    for (auto& variable : layout.describedVariables()) {
        if (traitsOf(variable.slot.representation()).absentBits)
            clearSlot(variable.slot.representation(), &environment->at<uint8_t>(variable.slot.offset));
    }
    environment->at<uint8_t>(offsetOfEnvironmentShapeLowByte) = lowByteOfEnvironmentShape(layout.shape);
    environment->at<uint8_t>(offsetOfEnvironmentShapeHighByte) = highByteOfEnvironmentShape(layout.shape);
    ASSERT(environment->at<uint64_t>(0) == headerFor(structure, layout.shape));
    vm.mutatorFence();
    return environment;
}

uint64_t Environment::headerFor(Structure* structure, uint32_t shape)
{
    ASSERT(uncheckedDowncast<CompiledStructure>(structure)->describesEnvironment());
    ASSERT(uncheckedDowncast<CompiledStructure>(structure)->environmentShapePage() == shape >> environmentShapesPerPageShift);
    uint64_t header = cellHeaderFor(structure);
    header &= ~(0xffull << 8 * offsetOfEnvironmentShapeLowByte | 0xffull << 8 * offsetOfEnvironmentShapeHighByte);
    header |= static_cast<uint64_t>(lowByteOfEnvironmentShape(shape)) << 8 * offsetOfEnvironmentShapeLowByte | static_cast<uint64_t>(highByteOfEnvironmentShape(shape)) << 8 * offsetOfEnvironmentShapeHighByte;
    return header;
}

template<typename Visitor>
void Environment::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* environment = uncheckedDowncast<Environment>(cell);
    visitFields(environment, environment->layout(), visitor);
}

DEFINE_VISIT_CHILDREN(Environment);

size_t Environment::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Environment::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Environment* environment = uncheckedDowncast<Environment>(cell);
    Base::analyzeHeap(cell, analyzer);
    const ProgramIdentifiers& identifiers = *environment->vm().aotState().identifiers;
    uintptr_t cageBase = cageBaseOf(environment);
    const EnvironmentData& layout = environment->layout();
    Instance* instance = layout.moduleAfterNone ? environment->structure()->realm()->aotInstance() : nullptr;
    JSCell* environmentOfModule = instance ? instance->state->statics->environmentOfModule(layout.moduleAfterNone - 1) : nullptr;
    if (environmentOfModule)
        analyzer.analyzeEdge(environment, environmentOfModule, RootMarkReason::None);
    for (auto& variable : layout.describedVariables()) {
        if (!traitsOf(variable.slot.representation()).holdsCell)
            continue;
        JSCell* target = cellInSlot(variable.slot.representation(), &environment->at<uint8_t>(variable.slot.offset), cageBase);
        UniquedStringImpl* name = target ? identifiers.nameInHeapSnapshots(&variable) : nullptr;
        if (name && environmentOfModule && variable.isVariableOfModule())
            instance->state->statics->analyzeVariableOfModule(environmentOfModule, target, name, analyzer);
        else if (name)
            analyzer.analyzeVariableNameEdge(environment, target, name);
    }
}

const ClassInfo EvalScope::s_info = { "EvalScope"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalScope) };

EvalScope::EvalScope(VM& vm, Structure* structure, JSScope* next, JSObject* bindings, JSValue newTarget)
    : Base(vm, structure, next)
    , m_bindings(bindings, WriteBarrierEarlyInit)
    , m_newTarget(newTarget, WriteBarrierEarlyInit)
{
}

Structure* EvalScope::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(StrictEvalActivationType, StructureFlags), info());
}

EvalScope* EvalScope::create(VM& vm, Structure* structure, JSScope* next, JSObject* bindings, JSValue newTarget)
{
    EvalScope* scope = new (NotNull, allocateCell<EvalScope>(vm)) EvalScope(vm, structure, next, bindings, newTarget);
    scope->finishCreation(vm);
    return scope;
}

template<typename Visitor>
void EvalScope::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* scope = uncheckedDowncast<EvalScope>(cell);
    Base::visitChildren(scope, visitor);
    visitor.append(scope->m_bindings);
    visitor.append(scope->m_newTarget);
}

DEFINE_VISIT_CHILDREN(EvalScope);

bool EvalScope::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto* scope = uncheckedDowncast<EvalScope>(object);
    if (name == vm.propertyNames->builtinNames().newTargetLocalPrivateName()) {
        JSValue newTarget = scope->m_newTarget.get();
        if (!newTarget)
            return false;
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete | PropertyAttribute::DontEnum, newTarget);
        return true;
    }
    JSObject* bindings = scope->m_bindings.get();
    return bindings->methodTable()->getOwnPropertySlot(bindings, globalObject, name, slot);
}

bool EvalScope::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    JSObject* bindings = uncheckedDowncast<EvalScope>(cell)->m_bindings.get();
    PutPropertySlot bindingsSlot(bindings, slot.isStrictMode());
    return bindings->methodTable()->put(bindings, globalObject, name, value, bindingsSlot);
}

bool EvalScope::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    return !uncheckedDowncast<EvalScope>(cell)->m_bindings->hasOwnProperty(globalObject, name);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
