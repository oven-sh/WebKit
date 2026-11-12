/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosure.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCall.h"
#include "AOTCompiledCallee.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTModuleTable.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTStubEntrypoints.h"
#include "AOTVMState.h"
#include "ArgList.h"
#include "AsyncGeneratorPrototype.h"
#include "BuiltinNames.h"
#include "ButterflyInlines.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "FunctionRareData.h"
#include "GeneratorPrototype.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "StructureCreateInlines.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };
const ClassInfo SlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SlimClosure) };
const ClassInfo ReifiedSlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ReifiedSlimClosure) };

template<typename Visitor>
static void visitReferencesOfClosure(JSCell* closure, Visitor& visitor)
{
    if (isCompiledClosure(closure))
        Closure::visitFields(uncheckedDowncast<Closure>(closure), visitor);
    else if (closure->type() == SlimClosureType)
        SlimClosure::tryVisitFields(uncheckedDowncast<SlimClosure>(closure), visitor);
    else
        ReifiedSlimClosure::visitFields(uncheckedDowncast<ReifiedSlimClosure>(closure), visitor);
}

namespace {

class ScopeInHeapSnapshot final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static ScopeInHeapSnapshot* create(VM& vm, Structure* structure, JSCell* closure)
    {
        auto* scope = new (NotNull, allocateCell<ScopeInHeapSnapshot>(vm)) ScopeInHeapSnapshot(vm, structure, closure);
        scope->finishCreation(vm);
        return scope;
    }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    ScopeInHeapSnapshot(VM& vm, Structure* structure, JSCell* closure)
        : Base(vm, structure)
        , m_closure(closure)
    {
    }

    JSCell* m_closure;
};

const ClassInfo ScopeInHeapSnapshot::s_info = { "JSLexicalEnvironment"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ScopeInHeapSnapshot) };

template<typename Visitor>
void ScopeInHeapSnapshot::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    JSCell* closure = uncheckedDowncast<ScopeInHeapSnapshot>(cell)->m_closure;
    visitor.appendUnbarriered(closure);
    if (visitor.isAnalyzingHeap())
        visitReferencesOfClosure(closure, visitor);
}

DEFINE_VISIT_CHILDREN(ScopeInHeapSnapshot);

} // anonymous namespace

Structure* createStructureOfScopesInHeapSnapshots(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, ScopeInHeapSnapshot::StructureFlags), ScopeInHeapSnapshot::info());
}

JSCell* createScopeInHeapSnapshot(VM& vm, Structure* structure, JSCell* closure)
{
    return ScopeInHeapSnapshot::create(vm, structure, closure);
}

bool hasScopeInHeapSnapshots(JSCell* cell)
{
    if (!CompiledCallee::isCompiledCallee(cell))
        return false;
    struct {
        void appendUnbarriered(JSValue value) { found |= value && value.isCell(); }
        bool found { false };
    } finder;
    visitReferencesOfClosure(cell, finder);
    return finder.found;
}

static JSCell* scopeInHeapSnapshotOf(JSCell* closure)
{
    Instance* instance = closure->structure()->realm()->aotInstance();
    return instance ? instance->state->statics->scopeInHeapSnapshotOf(closure) : nullptr;
}

template<typename Visitor>
static bool visitScopeInHeapSnapshot(JSCell* closure, Visitor& visitor)
{
    JSCell* scope = visitor.isAnalyzingHeap() ? scopeInHeapSnapshotOf(closure) : nullptr;
    if (scope)
        visitor.appendUnbarriered(scope);
    return scope;
}

static void analyzeCaptures(JSCell* cell, uint32_t closureNumber, std::span<const uint64_t> captures, unsigned numberOfReferences, HeapAnalyzer& analyzer)
{
    JSCell* scope = scopeInHeapSnapshotOf(cell);
    JSCell* holder = scope ? scope : cell;
    Instance* instance = cell->structure()->realm()->aotInstance();
    JSCell* environment = instance ? instance->state->statics->environmentOfModuleThatDefines(closureNumber) : nullptr;
    if (environment)
        analyzer.analyzeEdge(holder, environment, RootMarkReason::None);
    if (String name = cell->inherits<JSFunction>() ? String() : CompiledCallee::displayName(asObject(cell)); !name.isEmpty())
        analyzer.setLabelForCell(cell, name);
    const ProgramIdentifiers& identifiers = *cell->vm().aotState().identifiers;
    for (unsigned i = 0; i < numberOfReferences; ++i) {
        JSValue reference = JSValue::decode(captures[i]);
        if (!reference || !reference.isCell())
            continue;
        UniquedStringImpl* name = identifiers.nameInHeapSnapshots(closureNumber, i);
        if (name && environment && identifiers.isVariableOfModuleInHeapSnapshots(closureNumber, i))
            instance->state->statics->analyzeVariableOfModule(environment, reference.asCell(), name, analyzer);
        else if (name)
            analyzer.analyzeVariableNameEdge(holder, reference.asCell(), name);
        else
            analyzer.analyzeIndexEdge(holder, reference.asCell(), i);
    }
}

static CallData nativeCallData(TaggedNativeFunction function)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = function;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

static CallData constructDataOf(JSCell* cell, uint32_t closureNumber)
{
    if (!FunctionTables::canConstruct(programTables().kindOfClosureNumber(closureNumber)))
        return CallData();
    return CallData::forCompiledCode(constructEntrypoint().taggedPtr(), cell->structure()->realm());
}

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CompiledClosureType, StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    RELEASE_ASSERT(numberOfReferences <= captures.size() && captures.size() <= maxNumberOfCaptures);
    FunctionTables& tables = FunctionTables::of(vm);
    const ProgramTables& programTables = AOT::programTables();
    ClosureFunctionRangeData closureFunctions = programTables.closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= function.index() && function.index() < closureFunctions.end);
    uint32_t closureNumber = programTables.closureNumberOf(function.index());
    uint64_t word = tables.words[closureNumber];
    CaptureCounts counts = programTables.captureCountsOfClosureNumber(closureNumber);
    RELEASE_ASSERT(word && counts.references == numberOfReferences && counts.captures == captures.size());
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize(captures.size()))) Closure(vm, structure, tables.executableOf(function), word);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    return closure;
}

Closure* Closure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= Closure::allocationSize(0));
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize)) Closure(vm, structure, nullptr, 0);
    closure->finishCreation(vm);
    return closure;
}

template<typename Visitor>
void Closure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    if (!visitScopeInHeapSnapshot(cell, visitor))
        visitFields(uncheckedDowncast<Closure>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(Closure);

void Closure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    Closure* closure = uncheckedDowncast<Closure>(cell);
    if (closure->m_entry)
        analyzeCaptures(cell, closure->closureNumber(), closure->captures(), closure->numberOfReferences(), analyzer);
}

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    String name = classObject->kind().hasInferredName ? String() : writtenFunctionName(classObject->function());
    ConstructorKind kind = FunctionDescription(classObject->function()).isNakedConstructor() ? ConstructorKind::Naked : ConstructorKind::Base;
    SetForScope shownFrame(vm.aotState().classCalledWithoutNew, callFrame);
    return JSValue::encode(throwException(globalObject, scope, createConstructorCalledWithoutNewError(globalObject, kind, name)));
}

CallData Closure::getCallData(JSCell* cell)
{
    ASSERT(uncheckedDowncast<Closure>(cell)->kind().isConstructor == !!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit));
    if (!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit))
        return CallData::forCompiledCode(closureEntrypoint().taggedPtr(), cell->structure()->realm());
    return nativeCallData(callClassConstructor);
}

CallData Closure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<Closure>(cell)->closureNumber());
}

JSC_DEFINE_HOST_FUNCTION(callCompiledClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* callee = callFrame->jsCallee();
    return JSValue::encode(callCompiledCode(globalObject->vm(), callee, Closure::getCallData(callee), callFrame->thisValue(), nullptr, ArgList(callFrame)));
}

JSC_DEFINE_HOST_FUNCTION(constructCompiledClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* callee = callFrame->jsCallee();
    return JSValue::encode(constructCompiledCode(globalObject->vm(), callee, Closure::getConstructData(callee), ArgList(callFrame), callFrame->newTarget()));
}

bool mayHaveLazyProperty(VM& vm, JSCell* cell, UniquedStringImpl* uid, LookupPosition position)
{
    if (cell->type() == ErrorInstanceType)
        return ErrorInstance::isLazilyMaterialized(vm, uid);
    if (uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype)
        return true;
    if (isSlimClosureType(cell->type()))
        return position == LookupPosition::PrototypeChain;
    if (cell->type() != CompiledClosureType)
        return true;
    auto* closure = uncheckedDowncast<Closure>(cell);
    if (!closure->kind().isConstructor)
        return false;
    return !closure->classData() || !closure->getDirect(vm, vm.propertyNames->prototype) || closure->canHoldStaticFieldInVariable(uid);
}

static JSC_DECLARE_CUSTOM_GETTER(closurePrototypeProperty);

JSC_DEFINE_CUSTOM_GETTER(closurePrototypeProperty, (JSGlobalObject* globalObject, EncodedJSValue encodedClosure, PropertyName name))
{
    JSCell* cell = JSValue::decode(encodedClosure).asCell();
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        return JSValue::encode(closure->canConstruct() ? closure->get(globalObject, name) : jsUndefined());
    }
    JSValue prototype = uncheckedDowncast<Closure>(cell)->prototypeProperty(globalObject->vm());
    return JSValue::encode(prototype ? prototype : jsUndefined());
}

bool closureHandlesPrototypeProperty(JSCell* cell)
{
    return cell->type() == SlimClosureType || cell->type() == CompiledClosureType;
}

GetValueFunc closurePrototypePropertyGetter()
{
    return closurePrototypeProperty;
}

const LayoutData* Closure::classData() const
{
    return kind().isConstructor ? state().objects->classes.classConstructedByClosureWithNumber(closureNumber()) : nullptr;
}

bool Closure::isLazyClassObject() const
{
    const LayoutData* classLayout = classData();
    return classLayout && isLazyClassObjectOf(*classLayout);
}

bool Closure::isLazyClassObjectOf(const LayoutData& layout) const
{
    const LayoutData* classLayout = &layout;
    if (!classLayout->createsClassObjectOnMutation || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const ClassTable& classes = state().objects->classes;
    return classLayout->createsClassObjectOnMutation && (classLayout->createsPrototypeBeforeClassObject || classes.classPrototypeIfExists(*classLayout) || classes.prototypeAwaitsFirstTouch(*classLayout));
}

bool Closure::hasStructureOfNewClosure(VM& vm) const
{
    unsigned index = kind().closureStructureOfKind();
    if (index == classObjectClosureStructure)
        return false;
    Structure* ofNewClosure = state().objects->closures.closureStructure(vm, index);
    return structureID() == ofNewClosure->id();
}

bool Closure::hasStaticFieldsLeft() const
{
    const ClassTable& classes = state().objects->classes;
    if (!classes.leavesSomeStaticFields())
        return false;
    const LayoutData* classLayout = classData();
    return classLayout && classes.hasStaticFieldsLeft(*classLayout) && StaticFields::areReadInVariables(*this);
}

bool Closure::canHoldStaticFieldInVariable(UniquedStringImpl* uid) const
{
    return (hasStaticFieldsLeft() || isLazyClassObject()) && state().objects->classes.staticField(*classData(), uid);
}

static NEVER_INLINE void createClassObject(Closure* classObject, ClassTable::StaticFieldsOfPart staticFields)
{
    State& state = classObject->state();
    state.objects->classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::ClassObject, staticFields);
}

void Closure::ensureClassObject(VM& vm)
{
    if (isLazyClassObject())
        createClassObject(this, ClassTable::StaticFieldsOfPart::MoveToProperties);
    else if (hasStaticFieldsLeft()) [[unlikely]]
        StaticFields::moveToProperties(vm, *this);
}

void Closure::ensureStaticMethods(VM&)
{
    if (isLazyClassObject())
        createClassObject(this, ClassTable::StaticFieldsOfPart::StayInVariables);
}

static Structure* structureSharedWithClassesInheritingFrom(VM& vm, const Closure& classObject, JSObject* parent)
{
    const LayoutData& classLayout = *classObject.classData();
    if (!classLayout.staticMethods().empty() || !classLayout.staticFields().empty())
        return nullptr;
    State& state = classObject.state();
    if (classLayout.evaluation() != EvaluationState::ProvenOnce) {
        bool isFirstEvaluation = state.objects->closures.closureIfCreated(state.objects->classes.functionOfOwnClosure(classObject)) == &classObject;
        if (parent != state.instance.globalObject->functionPrototype() || !isFirstEvaluation)
            return nullptr;
    }
    return state.objects->classes.classObjectStructureInheritingFrom(vm, parent);
}

void Closure::initializeClassObject(VM& vm, JSObject* parent, JSObject* prototype)
{
    ASSERT(kind().isConstructor && classData());
    Structure* shared = structureSharedWithClassesInheritingFrom(vm, *this, parent);
    if (!shared) {
        Structure* own = createStructure(vm, structure()->realm(), parent);
        if (butterfly())
            nukeStructureAndSetButterfly(vm, structureID(), nullptr);
        setStructure(vm, own);
        putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return;
    }

    ASSERT(shared->transitionPropertyName() == vm.propertyNames->prototype.impl());
    bool wasPrototype = mayBePrototype();
    nukeStructureAndSetButterfly(vm, structureID(), Butterfly::create(vm, this, shared));
    setStructure(vm, shared);
    putDirectOffset(vm, shared->transitionOffset(), prototype);
    if (wasPrototype)
        didBecomePrototype(vm);
}

JSValue Closure::prototypeProperty(VM& vm)
{
    if (JSValue stored = getDirect(vm, vm.propertyNames->prototype))
        return stored;
    if (isLazyClassObject())
        return state().objects->classes.classPrototype(vm, *classData());
    ensurePrototypeProperty(vm);
    return getDirect(vm, vm.propertyNames->prototype);
}

static JSObject* createPrototypeObjectOfFunction(VM& vm, JSGlobalObject* globalObject, JSObject* function)
{
    PropertyOffset offset;
    Structure* structure = Structure::becomePrototypeTransition(vm, globalObject->objectStructureForObjectConstructor());
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->constructor, static_cast<unsigned>(PropertyAttribute::DontEnum), offset);
    JSObject* prototype = constructEmptyObject(vm, structure);
    prototype->putDirectOffset(vm, offset, function);
    countEvent(vm, RuntimeCounterID::PrototypeObjectsOfFunctionsCreated);
    return prototype;
}

bool Closure::ensurePrototypeProperty(VM& vm)
{
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    ensureClassObject(vm);
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    if (kind().isConstructor || !FunctionTables::hasPrototypePropertyAtCreation(kind()))
        return false;
    JSGlobalObject* globalObject = state().instance.globalObject;
    JSObject* prototype;
    FunctionRareData* rareData = kind().isGenerator ? this->rareData() : nullptr;
    if (Structure* structureOfGenerators = rareData ? rareData->internalFunctionAllocationStructure() : nullptr)
        prototype = structureOfGenerators->storedPrototypeObject();
    else if (kind().isGenerator)
        prototype = constructEmptyObject(globalObject, kind().isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
    else
        prototype = createPrototypeObjectOfFunction(vm, globalObject, this);
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    state().objects->classes.didCreatePrototypePropertyOf(this);
    state().objects->closures.didGivePrototypePropertyTo(vm, this);
    return true;
}

bool Closure::tryCreatePrototypeProperty(VM& vm, JSValue value)
{
    if (classData() || kind().isConstructor || kind().isGenerator || !FunctionTables::hasPrototypePropertyAtCreation(kind()))
        return false;
    if (!structure()->isStructureExtensible() || getDirect(vm, vm.propertyNames->prototype) || !state().objects->classes.definesNoClassOf(state().objects->classes.functionOfOwnClosure(*this)))
        return false;
    if (FunctionRareData* rareData = this->rareData())
        rareData->clear("Store to prototype property of a function");
    putDirect(vm, vm.propertyNames->prototype, value, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    state().objects->closures.didGivePrototypePropertyTo(vm, this);
    return true;
}

void Closure::setPrototypeProperty(VM& vm, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(!kind().isConstructor);
    if (FunctionRareData* rareData = this->rareData())
        rareData->clear("Store to prototype property of a function");
    putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void Closure::setNameProperty(VM& vm, JSString* name, unsigned attributes)
{
    ensureClassObject(vm);
    putDirect(vm, vm.propertyNames->name, name, attributes);
    FunctionRareData* rareData = ensureRareData(vm);
    rareData->setHasReifiedName();
    rareData->setHasModifiedNameForBoundOrNonHostFunction();
}

static bool canDefineStaticMemberWithoutTransition(VM& vm, Closure& classObject, PropertyName key)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (key == names.prototype || key == names.name || key == names.length)
        return false;
    return !classObject.isLazyClassObject();
}

static bool tryCompleteAccessorInSlot(JSGlobalObject* globalObject, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    Structure* structure = holder->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, key, attributes);
    if (!isValidOffset(offset) || attributes != (PropertyAttribute::Accessor | PropertyAttribute::DontEnum))
        return false;
    auto* current = uncheckedDowncast<GetterSetter>(holder->getDirect(offset));
    JSObject* getter = kind == MethodKind::Getter ? function : current->isGetterNull() ? nullptr : current->getter();
    JSObject* setter = kind == MethodKind::Setter ? function : current->isSetterNull() ? nullptr : current->setter();
    structure->didReplaceProperty(offset);
    holder->putDirectOffset(vm, offset, GetterSetter::create(vm, globalObject, getter, setter));
    return true;
}

bool tryDefineClassMemberWithoutTransition(JSGlobalObject* globalObject, Closure& classObject, const LayoutData& classLayout, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    if (parseIndex(key))
        return false;
    bool isStatic = holder == &classObject;
    if (isStatic ? !canDefineStaticMemberWithoutTransition(vm, classObject, key) : holder->type() != FinalObjectType)
        return false;

    Structure* structure = holder->structure();
    if (!structure->isStructureExtensible() || structure->isImmutableProperty(key.uid()))
        return false;
    if (kind != MethodKind::Method && tryCompleteAccessorInSlot(globalObject, holder, key, function, kind))
        return true;
    if (classObject.state().objects->classes.keepsStructuresForNextEvaluation(classObject, classLayout))
        return false;
    bool isOwnStructure = structure->transitionKind() == TransitionKind::Unknown || structure->transitionKind() == TransitionKind::MakeListedPropertiesImmutable;
    if (!isOwnStructure || !structure->isPinnedPropertyTable() || structure->isDictionary())
        return false;

    bool isMarkedAsPrototype = structure->mayBePrototype();
    if (isMarkedAsPrototype)
        structure->setMayBePrototype(false);
    auto restoreMark = makeScopeExit([&] {
        if (isMarkedAsPrototype)
            structure->setMayBePrototype(true);
    });
    if (isValidOffset(structure->get(vm, key)))
        return false;
    if (kind == MethodKind::Method)
        holder->putDirectWithoutTransition(vm, key, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
    else {
        JSObject* getter = kind == MethodKind::Getter ? function : nullptr;
        JSObject* setter = kind == MethodKind::Setter ? function : nullptr;
        holder->putDirectNonIndexAccessorWithoutTransition(vm, key, GetterSetter::create(vm, globalObject, getter, setter), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
    }
    return true;
}

void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    closure->ensureClassObject(vm);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && FunctionTables::hasPrototypePropertyAtCreation(closure->kind()) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

[[noreturn]] static void crashOnFalseProofOfClosure(uint32_t closureNumber, ASCIILiteral mutation)
{
    FunctionID function = closureFunctionWithNumber(closureNumber);
    RuntimeCheckFailure failure("object mutation check failed"_s);
    failure.inFunction(function);
    failure.detail(mutation, " on a closure of ", reportedFunctionName(function), ", but the compiler proved that this never happens");
    failure.report();
    CRASH_WITH_INFO(function.index());
}

static std::optional<uint32_t> numberOfClosureHeldToListedNames(VM& vm, const JSCell* closure)
{
    std::optional<uint32_t> number = closureNumberOf(closure);
    if (!number || !kindOfClosureNumber(*number).closuresGetOnlyListedNames || vm.aotState().nativeTestChangesObjectsUnseen)
        return std::nullopt;
    return number;
}

JSValue findOnChainAbove(VM& vm, const JSObject* object, UniquedStringImpl* uid, unsigned& attributes)
{
    for (JSValue above = object->getPrototypeDirect(); above.isObject(); above = asObject(above)->getPrototypeDirect()) {
        if (JSValue found = asObject(above)->getDirect(vm, PropertyName(uid), attributes))
            return found;
    }
    return { };
}

bool holdsWhatIsFoundAbove(VM& vm, const JSObject* object, UniquedStringImpl* uid, JSValue value, unsigned attributes)
{
    unsigned attributesAbove = 0;
    JSValue above = findOnChainAbove(vm, object, uid, attributesAbove);
    if (!above || ((attributes | attributesAbove) & PropertyAttribute::CustomAccessorOrValue))
        return false;
    if (!((attributes | attributesAbove) & PropertyAttribute::Accessor))
        return above == value;
    if (!(attributes & attributesAbove & PropertyAttribute::Accessor))
        return false;
    auto* own = uncheckedDowncast<GetterSetter>(value.asCell());
    auto* found = uncheckedDowncast<GetterSetter>(above.asCell());
    return own->getter() == found->getter() && own->setter() == found->setter();
}

bool closureAdmitsName(VM& vm, const JSCell* closure, UniquedStringImpl* uid)
{
    std::optional<uint32_t> number = numberOfClosureHeldToListedNames(vm, closure);
    if (!number)
        return true;
    const CommonIdentifiers& names = *vm.propertyNames;
    if (uid == names.name.impl() || uid == names.length.impl() || uid == names.prototype.impl() || uid == names.arguments.impl() || uid == names.caller.impl())
        return true;
    if (uid->isSymbol() && static_cast<SymbolImpl*>(uid)->isPrivate())
        return true;
    std::optional<uint32_t> name = identifiersOf(*Instance::owning(closure)).numberOf(uid);
    return name && std::ranges::binary_search(programTables().closurePropertyNames(), ClosurePropertyNameData { closureFunctionWithNumber(*number).index(), *name });
}

void holdClosureToLackingNameSlowCase(VM& vm, const JSCell* closure, UniquedStringImpl* uid)
{
    if (closureAdmitsName(vm, closure, uid))
        return;
    const JSObject* object = static_cast<const JSObject*>(closure);
    unsigned attributes = 0;
    JSValue value = object->getDirect(vm, PropertyName(uid), attributes);
    if (!value || holdsWhatIsFoundAbove(vm, object, uid, value, attributes))
        return;
    crashOnFalseProofOfClosure(*numberOfClosureHeldToListedNames(vm, closure), "AddProperty"_s);
}

void holdClosureToItsPrototype(VM& vm, const JSCell* closure)
{
    if (std::optional<uint32_t> number = numberOfClosureHeldToListedNames(vm, closure))
        crashOnFalseProofOfClosure(*number, "ChangePrototype"_s);
}

static void holdClosureToItsPrototypeProperty(VM& vm, Closure& closure, const LayoutData& constructed)
{
    if (!closure.kind().prototypePropertyStaysAsMade || vm.aotState().nativeTestChangesObjectsUnseen)
        return;
    if (closure.state().objects->closures.closureIfCreated(FunctionID(constructed.classData().classConstructor)) == &closure)
        crashOnFalseProofOfClosure(closure.closureNumber(), "ChangePrototypeProperty"_s);
}

static void didChangePrototypeProperty(Closure* closure)
{
    VM& vm = closure->vm();
    if (FunctionRareData* rareData = closure->rareData())
        rareData->clear("Store to prototype property of a function");
    ClassTable& classes = closure->state().objects->classes;
    if (const LayoutData* constructed = classes.classConstructedBy(classes.functionOfOwnClosure(*closure))) {
        classes.didChangeMember(*constructed, ClassSide::Static, vm.propertyNames->prototype.impl());
        JSObject* prototypeOfClass = classes.isDefinedByClass(*constructed) ? nullptr : classes.classPrototypeIfExists(*constructed);
        if (prototypeOfClass && closure->getDirect(vm, vm.propertyNames->prototype) != JSValue(prototypeOfClass)) {
            holdClosureToItsPrototypeProperty(vm, *closure, *constructed);
            classes.disarmMemberGuards(*constructed);
        }
    }
}

static constexpr unsigned initialAttributesOfName = PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly;

static bool tryDefineInitialName(VM& vm, Closure& closure, PropertyName name, const PropertyDescriptor& descriptor)
{
    if (name != vm.propertyNames->name || closure.classData() || descriptor.isAccessorDescriptor() || !descriptor.value() || !descriptor.value().isString())
        return false;
    FunctionRareData* rareData = closure.rareData();
    if ((rareData && rareData->hasReifiedName()) || uncheckedDowncast<NativeExecutable>(closure.executable())->isNamedAsBound() || closure.getDirect(vm, vm.propertyNames->name))
        return false;
    unsigned overriddenAttributes = (descriptor.writablePresent() ? PropertyAttribute::ReadOnly : PropertyAttribute::None) | (descriptor.enumerablePresent() ? PropertyAttribute::DontEnum : PropertyAttribute::None) | (descriptor.configurablePresent() ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    unsigned attributes = (descriptor.attributes() & overriddenAttributes) | (initialAttributesOfName & ~overriddenAttributes);
    if (attributes == initialAttributesOfName && !closure.kind().hasInitialNamePrefix()) {
        auto defined = asString(descriptor.value())->tryGetValue();
        auto initial = initialNamePropertyString(vm, closure.state(), closure.function())->tryGetValue();
        if (!defined->isNull() && !initial->isNull() && defined.data == initial.data)
            return true;
    }
    Structure* structure = closure.structure();
    if (!structure->isStructureExtensible() || structure->mayHaveImmutableProperties())
        return false;
    closure.setNameProperty(vm, asString(descriptor.value()), attributes);
    countEvent(vm, RuntimeCounterID::InitialNamesReplaced);
    return true;
}

static bool reifyWrittenName(VM& vm, Closure& closure)
{
    FunctionRareData* rareData = closure.rareData();
    if ((rareData && rareData->hasReifiedName()) || uncheckedDowncast<NativeExecutable>(closure.executable())->isNamedAsBound())
        return false;
    JSString* name = initialNamePropertyString(vm, closure.state(), closure.function());
    closure.ensureRareData(vm)->setHasReifiedName();
    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
    closure.putDirect(vm, vm.propertyNames->name, name, initialAttributesOfName);
    return true;
}

bool Closure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    const LayoutData* classLayout = closure->classData();
    bool isLazyClassObject = classLayout && closure->isLazyClassObjectOf(*classLayout);
    if (classLayout && !classLayout->staticFields().empty()) {
        if (JSValue value = StaticFields::tryRead(*closure, propertyName.uid())) {
            slot.setValue(closure, static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
    }
    if (isLazyClassObject) {
        if (propertyName != vm.propertyNames->prototype && propertyName != vm.propertyNames->name && propertyName != vm.propertyNames->length && !closure->state().objects->classes.hasStaticMethodNamed(*closure->classData(), propertyName.uid()))
            return false;
        closure->ensureStaticMethods(vm);
        if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
            return true;
    }
    if (propertyName == vm.propertyNames->prototype)
        return closure->ensurePrototypeProperty(vm) && JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot);
    bool isLength = propertyName == vm.propertyNames->length;
    bool isName = propertyName == vm.propertyNames->name;
    if ((isLength || (isName && !closure->kind().hasInitialNamePrefix())) && closure->hasStructureOfNewClosure(vm)) {
        JSValue value = isLength ? jsNumber(functionLengthOf(closure->function())) : JSValue(initialNamePropertyString(vm, closure->state(), closure->function()));
        slot.setValue(closure, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, value);
        return true;
    }
    if (isName && !closure->kind().hasInitialNamePrefix() && reifyWrittenName(vm, *closure))
        return JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot);
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

bool Closure::mayHoldOwnProperties(VM& vm, std::span<UniquedStringImpl* const> names) const
{
    for (UniquedStringImpl* name : names) {
        if (isValidOffset(structure()->get(vm, PropertyName(name))))
            return true;
    }
    const LayoutData* classLayout = classData();
    if (!classLayout || (classLayout->staticMethods().empty() && classLayout->staticFields().empty()))
        return false;
    const ClassTable& classes = state().objects->classes;
    return std::ranges::any_of(names, [&](UniquedStringImpl* name) {
        return classes.hasStaticMethodNamed(*classLayout, name) || classes.staticField(*classLayout, name);
    });
}

bool Closure::isNameThatBuiltinFunctionsRefuse(VM& vm, UniquedStringImpl* uid)
{
    if (uid->isSymbol())
        return uid == vm.propertyNames->hasInstanceSymbol.impl();
    auto& names = vm.propertyNames->builtinNames();
    return uid == vm.propertyNames->bind.impl() || uid == names.callPublicName().impl() || uid == names.applyPublicName().impl();
}

bool Closure::refusesOwnProperty(VM& vm, PropertyName name) const
{
    return !name.isPrivateName() && isNameThatBuiltinFunctionsRefuse(vm, name.uid()) && isBuiltinFunction();
}

bool Closure::isBuiltinFunction() const
{
    return kind().isInRuntimeModule && ModuleTable(state().instance.tables()).isRuntimeFunction(function());
}

static bool canPutDirectly(VM& vm, Closure& closure, PropertyName name, const PutPropertySlot& slot)
{
    JSObject* above = closure.getPrototypeDirect().getObject();
    if (!above || (above->type() != CompiledClosureType && !isCompiledObject(above)) || slot.thisValue() != JSValue(&closure))
        return false;
    const CommonIdentifiers& names = *vm.propertyNames;
    if (name == names.prototype || name == names.name || name == names.length)
        return false;
    Structure* structure = closure.structure();
    if (structure->isImmutableProperty(name.uid()) || structure->hasFrozenElementsInPlace() || structure->hasNonReifiedStaticProperties())
        return false;
    unsigned attributes = 0;
    if (isValidOffset(structure->get(vm, name, attributes)))
        return !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
    return storeThroughChain(walkChain({ structure->realm(), structure, &closure, ChainWalkMode::MayAllocate, ChainWalkUse::UsedAtOnce }, name.uid())) == StoreThroughChain::CreatesOwnProperty;
}

bool Closure::holdsStaticFieldUnder(UniquedStringImpl* uid) const
{
    const LayoutData* classLayout = classData();
    if (!classLayout)
        return false;
    const ClassTable& classes = state().objects->classes;
    return classes.staticField(*classLayout, uid) && !classes.hasStaticMethodNamed(*classLayout, uid);
}

bool Closure::valueUnderIsHeardByNobody(UniquedStringImpl* uid) const
{
    const ClassTable& classes = state().objects->classes;
    if (const LayoutData* classLayout = classData())
        return holdsStaticFieldUnder(uid) || classes.nobodyHearsOfValueUnder(*classLayout, ClassSide::Static, uid);
    ClassSide side = ClassSide::Static;
    const LayoutData* constructed = classes.classHeldBy(const_cast<Closure*>(this), side);
    return constructed && side == ClassSide::Static && classes.nobodyHearsOfValueUnder(*constructed, side, uid);
}

static bool tryReplaceStaticFieldOfClassObject(VM& vm, Closure& closure, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Structure* structure = closure.structure();
    if (structure->isDictionary() || structure->isImmutableProperty(name.uid()) || structure->hasFrozenElementsInPlace() || !closure.valueUnderIsHeardByNobody(name.uid()))
        return false;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, name, attributes);
    if (!isValidOffset(offset) || (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue))
        return false;
    structure->didReplaceProperty(offset);
    closure.putDirectOffset(vm, offset, value);
    slot.setExistingProperty(&closure, offset);
    return true;
}

bool Closure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    if (slot.thisValue() == JSValue(closure) && closure->refusesOwnProperty(vm, name)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        slot.disableCaching();
        bool isTold = slot.isStrictMode() || (slot.context() != PutPropertySlot::ReflectSet && name != vm.propertyNames->hasInstanceSymbol);
        return typeError(globalObject, scope, isTold, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    }
    if (name == vm.propertyNames->prototype && slot.thisValue() == JSValue(closure) && closure->tryCreatePrototypeProperty(vm, value)) {
        slot.disableCaching();
        return true;
    }
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    bool isStoreToClassObject = classLayout && slot.thisValue() == JSValue(closure);
    if (isStoreToClassObject && StaticFields::tryStore(*closure, name.uid(), value))
        return true;
    if (isStoreToClassObject && StaticFields::tryCreateByStore(vm, *closure, name, value)) {
        holdClosureToLackingNameSlowCase(vm, closure, name.uid());
        slot.disableCaching();
        return true;
    }
    if (slot.thisValue() == JSValue(closure) && tryReplaceStaticFieldOfClassObject(vm, *closure, name, value, slot))
        return true;
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = canPutDirectly(vm, *closure, name, slot) ? closure->putInlineFast(globalObject, name, value, slot) : Base::put(cell, globalObject, name, value, slot);
    if (slot.thisValue() == JSValue(closure))
        holdClosureToLackingNameSlowCase(vm, closure, name.uid());
    if (isStoreToClassObject) {
        bool callsSetter = slot.type() == PutPropertySlot::SetterProperty;
        bool replacesUndeclaredProperty = slot.type() == PutPropertySlot::ExistingProperty && !classes.hasStaticMethodNamed(*classLayout, name.uid());
        if (!replacesUndeclaredProperty && !callsSetter)
            slot.disableCaching();
        if (!callsSetter) {
            classes.holdMemberToProof(vm, *classLayout, ClassSide::Static, closure, name.uid());
            classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
        }
    }
    if (name == vm.propertyNames->prototype) [[unlikely]] {
        slot.disableCaching();
        didChangePrototypeProperty(closure);
    }
    return result;
}

bool Closure::hasOrdinaryDefineOwnProperty(VM& vm, PropertyName name) const
{
    return !classData() && !refusesOwnProperty(vm, name) && ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(vm, name);
}

static bool leavesStaticFieldPlain(const PropertyDescriptor& descriptor)
{
    if (descriptor.isAccessorDescriptor())
        return false;
    return (!descriptor.writablePresent() || descriptor.writable())
        && (!descriptor.enumerablePresent() || descriptor.enumerable())
        && (!descriptor.configurablePresent() || descriptor.configurable());
}

static bool tryDefineStaticFieldInVariable(Closure& classObject, PropertyName name, const PropertyDescriptor& descriptor)
{
    if (!leavesStaticFieldPlain(descriptor))
        return false;
    if (StaticFields::tryRedefine(classObject, name.uid(), descriptor.value()))
        return true;
    bool createsPlainProperty = descriptor.writablePresent() && descriptor.enumerablePresent() && descriptor.configurablePresent();
    return createsPlainProperty && StaticFields::tryCreate(classObject, name.uid(), descriptor.value() ? descriptor.value() : jsUndefined());
}

bool Closure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (closure->refusesOwnProperty(vm, name)) [[unlikely]]
        return typeError(globalObject, scope, shouldThrow, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    if (tryDefineInitialName(vm, *closure, name, descriptor))
        return true;
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    if (classLayout && tryDefineStaticFieldInVariable(*closure, name, descriptor))
        return true;
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception)) [[unlikely]]
        return false;
    holdClosureToLackingNameSlowCase(vm, closure, name.uid());
    if (classLayout) {
        classes.holdMemberToProof(vm, *classLayout, ClassSide::Static, closure, name.uid());
        classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]]
        didChangePrototypeProperty(closure);
    return result;
}

bool Closure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    const LayoutData* classLayout = closure->classData();
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::deleteProperty(cell, globalObject, name, slot);
    if (result && classLayout) {
        ClassTable& classes = closure->state().objects->classes;
        classes.holdMemberToProof(vm, *classLayout, ClassSide::Static, closure, name.uid());
        classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]]
        didChangePrototypeProperty(closure);
    return result;
}

bool Closure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    closure->ensureClassObject(vm);
    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool Closure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    closure->ensureClassObject(vm);
    if (const LayoutData* classLayout = closure->classData())
        closure->state().objects->classes.didChangeEveryMember(*classLayout, ClassSide::Static);
    return Base::preventExtensions(object, globalObject);
}

bool Closure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (object->getPrototypeDirect() != prototype && closure->isBuiltinFunction()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return typeError(globalObject, scope, shouldThrow, prototypeChangeOfLockedBuiltinMessage("a built-in function"_s));
    }
    closure->ensureClassObject(vm);
    const LayoutData* classLayout = closure->classData();
    if (classLayout && object->getPrototypeDirect() != prototype)
        closure->state().objects->classes.didChangePrototypeLink(*classLayout, ClassSide::Static);
    JSValue prototypeBefore = object->getPrototypeDirect();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = Base::setPrototype(object, globalObject, prototype, shouldThrow);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception)) [[unlikely]]
        return false;
    if (object->getPrototypeDirect() != prototypeBefore)
        holdClosureToItsPrototype(vm, closure);
    if (classLayout)
        closure->state().objects->classes.holdLinkToProof(vm, *classLayout, ClassSide::Static, closure);
    return result;
}

void PropertyWalk::willPutDirectOnClosure(VM& vm, JSObject* object, UniquedStringImpl* uid)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    const LayoutData* classLayout = closure->classData();
    if (classLayout && !classLayout->staticFields().empty() && StaticFields::tryRead(*closure, uid))
        closure->ensureClassObject(vm);
}

void PropertyWalk::didPutDirectOnClosure(VM& vm, JSObject* object, UniquedStringImpl* uid)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    holdClosureToLackingNameSlowCase(vm, closure, uid);
    const LayoutData* classLayout = closure->classData();
    if (!classLayout)
        return;
    ClassTable& classes = closure->state().objects->classes;
    classes.holdMemberToProof(vm, *classLayout, ClassSide::Static, closure, uid);
    ClassSide side;
    bool isEnteredHolder = classes.classHeldBy(closure, side) == classLayout && side == ClassSide::Static;
    if (isEnteredHolder || closure->isLazyClassObject())
        classes.didChangeMember(*classLayout, ClassSide::Static, uid);
}

Structure* SlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype)
{
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureType, StructureFlags), info(), NonArray, 0);
}

Structure* ReifiedSlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype)
{
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureWithPropertiesType, StructureFlags), info(), NonArray, 0);
}

SlimClosure* SlimClosure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const ProgramTables& programTables = AOT::programTables();
    uint32_t closureNumber = programTables.closureNumberOf(function.index());
    uint64_t word = FunctionTables::of(vm).words[closureNumber];
    CaptureCounts counts = programTables.captureCountsOfClosureNumber(closureNumber);
    RELEASE_ASSERT(word && programTables.kindOfClosureNumber(closureNumber).isSlim && counts.references == numberOfReferences && counts.captures == captures.size());
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize(captures.size()))) SlimClosure(vm, structure, word);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    return closure;
}

SlimClosure* SlimClosure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= SlimClosure::allocationSize(0));
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize)) SlimClosure(vm, structure, 0);
    closure->finishCreation(vm);
    return closure;
}

static ALWAYS_INLINE State& stateOfClosure(const JSCell* closure)
{
    return State::of(closure->structure()->realm());
}

State& Closure::state() const
{
    return stateOfClosure(this);
}

State& SlimClosure::state() const
{
    return stateOfClosure(this);
}

template<typename Visitor>
void SlimClosure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    if (!visitScopeInHeapSnapshot(cell, visitor) && !tryVisitFields(closure, visitor)) {
        visitor.didRace(cell, "SlimClosure::reify");
        return;
    }
    JSCell::visitChildren(cell, visitor);
}

DEFINE_VISIT_CHILDREN(SlimClosure);

template<typename Visitor>
void ReifiedSlimClosure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    Base::visitChildren(cell, visitor);
    if (!visitScopeInHeapSnapshot(cell, visitor))
        visitFields(closure, visitor);
}

DEFINE_VISIT_CHILDREN(ReifiedSlimClosure);

void SlimClosure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    if (closure->m_word)
        analyzeCaptures(cell, closure->closureNumber(), closure->captures(), closure->numberOfReferences(), analyzer);
}

void ReifiedSlimClosure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    Base::analyzeHeap(cell, analyzer);
    analyzeCaptures(cell, closure->closureNumber(), closure->captures(), closure->numberOfReferences(), analyzer);
}

size_t SlimClosure::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

CallData SlimClosure::getCallData(JSCell* cell)
{
    return CallData::forCompiledCode(slimClosureEntrypoint().taggedPtr(), cell->structure()->realm());
}

CallData ReifiedSlimClosure::getCallData(JSCell* cell)
{
    return CallData::forCompiledCode(slimClosureEntrypoint().taggedPtr(), cell->structure()->realm());
}

CallData SlimClosure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<SlimClosure>(cell)->closureNumber());
}

CallData ReifiedSlimClosure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber());
}

Structure* SlimClosure::fillInitialProperties(VM& vm, std::span<JSValue, numberOfInitialProperties> values)
{
    State& state = this->state();
    FunctionID function = this->function();
    Structure* structure = state.objects->closures.reifiedSlimClosureStructure(vm, function);
    values[0] = jsNumber(closureNumberOf(function));
    values[1] = jsNumber(functionLengthOf(function));
    values[2] = JSValue(initialNamePropertyString(vm, state, function));
    values[3] = FunctionTables::hasPrototypePropertyAtCreation(kind()) ? JSValue(state.objects->closures.uncreatedPrototypeProperty()) : JSValue();
    return structure;
}

ReifiedSlimClosure* SlimClosure::becomeReified(VM& vm, Structure* structure, std::span<const JSValue> values)
{
    static_assert(offsetOfWord() == butterflyOffset());
    RELEASE_ASSERT(structure->outOfLineCapacity() == values.size() && getPrototypeDirect() == structure->storedPrototype());
    Butterfly* butterfly = Butterfly::createUninitialized(vm, this, 0, values.size(), false, 0);
    for (unsigned i = 0; i < values.size(); ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(firstOutOfLineOffset + i)].setWithoutWriteBarrier(values[i]);
    setStructureIDDirectly(structureID().nuke());
    WTF::storeStoreFence();
    m_word = std::bit_cast<uint64_t>(butterfly);
    WTF::storeStoreFence();
    JSCell::setStructure(vm, structure);
    vm.writeBarrier(this);
    return uncheckedDowncast<ReifiedSlimClosure>(static_cast<JSObject*>(this));
}

ReifiedSlimClosure* SlimClosure::reify(VM& vm, PrototypeObject prototypeObject)
{
    JSValue values[numberOfInitialProperties];
    static_assert(numberOfInitialProperties == initialOutOfLineCapacity);
    Structure* withProperties = fillInitialProperties(vm, values);
    if (values[3] && prototypeObject == PrototypeObject::MadeNow) {
        withProperties = Structure::attributeChangeTransition(vm, withProperties, vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
        values[3] = createPrototypeObjectOfFunction(vm, state().instance.globalObject, this);
    }
    return becomeReified(vm, withProperties, values);
}

ReifiedSlimClosure* SlimClosure::reifyWithAddedProperty(VM& vm, Structure* structureWithProperty, PropertyOffset offset, JSValue value)
{
    JSValue values[2 * numberOfInitialProperties];
    Structure* withoutProperty = fillInitialProperties(vm, std::span(values).first<numberOfInitialProperties>());
    unsigned index = offset - firstOutOfLineOffset;
    unsigned capacity = structureWithProperty->outOfLineCapacity();
    RELEASE_ASSERT(structureWithProperty->isTransitionFrom(withoutProperty) && capacity <= std::size(values) && index < capacity && !values[index]);
    values[index] = value;
    return becomeReified(vm, structureWithProperty, std::span<const JSValue>(values).first(capacity));
}

JSC_DEFINE_CUSTOM_GETTER(createPrototypeProperty, (JSGlobalObject*, EncodedJSValue encodedClosure, PropertyName))
{
    JSObject* closure = asObject(JSValue::decode(encodedClosure));
    VM& vm = closure->vm();
    JSObject* prototype = createPrototypeObjectOfFunction(vm, closure->structure()->realm(), closure);
    unsigned attributes = 0;
    closure->getDirect(vm, vm.propertyNames->prototype, attributes);
    closure->putDirect(vm, vm.propertyNames->prototype, prototype, attributes & ~PropertyAttribute::CustomValue);
    return JSValue::encode(prototype);
}

bool SlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    if (name == vm.propertyNames->length) {
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(functionLengthOf(closure->function())));
        return true;
    }
    if (name == vm.propertyNames->name) {
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, initialNamePropertyString(vm, closure->state(), closure->function()));
        return true;
    }
    if (name != vm.propertyNames->prototype || !FunctionTables::hasPrototypePropertyAtCreation(closure->kind()))
        return false;
    closure->reify(vm, PrototypeObject::MadeNow);
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

bool ReifiedSlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    return Base::getOwnPropertySlot(object, globalObject, name, slot);
}

void SlimClosure::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

void ReifiedSlimClosure::getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode)
{
}

bool SlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool result = ReifiedSlimClosure::put(closure, globalObject, name, value, slot);
    slot.disableCaching();
    return result;
}

bool ReifiedSlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (slot.thisValue() == JSValue(cell))
        holdClosureToLackingNameSlowCase(vm, cell, name.uid());
    if (name == vm.propertyNames->prototype) [[unlikely]]
        slot.disableCaching();
    return result;
}

bool SlimClosure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool SlimClosure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool isDeleted = JSObject::deleteProperty(cell, globalObject, name, slot);
    slot.disableCaching();
    return isDeleted;
}

bool SlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return ReifiedSlimClosure::defineOwnProperty(closure, globalObject, name, descriptor, shouldThrow);
}

bool ReifiedSlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    holdClosureToLackingNameSlowCase(globalObject->vm(), object, name.uid());
    return result;
}

bool ReifiedSlimClosure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    JSValue prototypeBefore = object->getPrototypeDirect();
    bool result = Base::setPrototype(object, globalObject, prototype, shouldThrow);
    if (object->getPrototypeDirect() != prototypeBefore)
        holdClosureToItsPrototype(globalObject->vm(), object);
    return result;
}

bool ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(VM& vm, PropertyName name)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    return name != names.prototype && name != names.name && name != names.length;
}

bool SlimClosure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return JSObject::preventExtensions(object, globalObject);
}

bool SlimClosure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    if (object->getPrototypeDirect() == prototype)
        return true;
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return ReifiedSlimClosure::setPrototype(object, globalObject, prototype, shouldThrow);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
