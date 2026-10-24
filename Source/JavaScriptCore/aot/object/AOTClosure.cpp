/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosure.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCall.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeLayoutTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTStubEntrypoints.h"
#include "ArgList.h"
#include "AsyncGeneratorPrototype.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "GeneratorPrototype.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };

static_assert(Closure::offsetOfEntry() == offsetOfClosureEntry && Closure::offsetOfCaptures() == offsetOfClosureCaptures);

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool isBound)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CompiledClosureType, isBound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    RELEASE_ASSERT(numberOfReferences <= captures.size() && captures.size() <= maxNumberOfCaptures);
    FunctionTables& tables = FunctionTables::of(vm);
    ClosureFunctionRangeData closureFunctions = programTables().closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= function.index() && function.index() < closureFunctions.end && tables.words[function.index()]);
    CaptureCounts counts = captureCountsOf(function);
    RELEASE_ASSERT(counts.references == numberOfReferences && counts.captures == captures.size());
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize(captures.size()))) Closure(vm, structure, tables.executableOf(function), tables.words[function.index()]);
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
    visitFields(uncheckedDowncast<Closure>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(Closure);

void Closure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    Closure* closure = uncheckedDowncast<Closure>(cell);
    if (!closure->m_entry)
        return;
    for (unsigned i = 0; i < closure->numberOfReferences(); ++i) {
        if (JSValue reference = JSValue::decode(closure->captures()[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

State& Closure::state() const
{
    return *Instance::of(structure()->realm())->state;
}

static bool isBoundFunction(const Closure& closure)
{
    return closure.kind().callable() == CallableKind::Bound;
}

static ALWAYS_INLINE void validateIsNotInFrame(VM& vm, const Closure* closure)
{
    if (validatesRuntime(vm)) [[unlikely]]
        validateFunctionCellIsNotInFrame(vm, closure);
}

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    String name = classObject->kind().hasInferredName ? String() : writtenFunctionName(classObject->function());
    ConstructorKind kind = FunctionDescription(classObject->function()).isNakedConstructor() ? ConstructorKind::Naked : ConstructorKind::Base;
    return JSValue::encode(throwException(globalObject, scope, createConstructorCalledWithoutNewError(globalObject, kind, name)));
}

CallData Closure::getCallData(JSCell* cell)
{
    // The word of the closure says it as well as the record of its function, and lies in the cell.
    ASSERT(uncheckedDowncast<Closure>(cell)->kind().isConstructor == !!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit));
    if (!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit))
        return CallData::forCompiledCode(closureEntrypoint().taggedPtr(), cell->structure()->realm());
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callClassConstructor;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

CallData Closure::getConstructData(JSCell* cell)
{
    if (isBoundFunction(*uncheckedDowncast<Closure>(cell)))
        return constructDataOfBoundClosure(uncheckedDowncast<Closure>(cell)->function());
    if (!FunctionTables::canConstruct(uncheckedDowncast<Closure>(cell)->kind()))
        return CallData();
    return CallData::forCompiledCode(constructEntrypoint().taggedPtr(), cell->structure()->realm());
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
    const LayoutData* classLayout = closure->classData();
    return !classLayout || !closure->getDirect(vm, vm.propertyNames->prototype) || closure->state().objects->classes.staticField(*classLayout, uid);
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
    return kind().isConstructor ? state().objects->classes.classConstructedBy(function()) : nullptr;
}

bool Closure::isLazyClassObject() const
{
    if (!kind().isConstructor || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const LayoutData* classLayout = classData();
    if (!classLayout)
        return false;
    return classLayout->createsClassObjectOnMutation && (classLayout->createsPrototypeBeforeClassObject || state().objects->classes.classPrototypeIfExists(*classLayout));
}

bool Closure::hasStructureOfNewClosure(VM& vm) const
{
    unsigned index = kind().closureStructure;
    return index != classObjectClosureStructure && structure() == state().objects->closures.closureStructure(vm, index);
}

static NEVER_INLINE void createClassObject(Closure* classObject)
{
    State& state = classObject->state();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(classObject->vm());
    state.objects->classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::ClassObject);
    scope.assertNoException();
}

void Closure::adoptBoundFunctionStructure(VM& vm)
{
    ClosureTable& closures = state().objects->closures;
    Structure* unbound = closures.closureStructureFor(vm, function());
    if (structure() != unbound)
        return;
    setStructure(vm, closures.boundClosureStructure(vm, function()));
}

void Closure::ensureClassObject(VM&)
{
    if (isLazyClassObject())
        createClassObject(this);
}

// A cache answers for a static field by the Structure of the class object alone, so a class with static members shares its Structure with no other class.
static Structure* structureSharedWithClassesInheritingFrom(VM& vm, const Closure& classObject, JSObject* parent)
{
    const LayoutData& classLayout = *classObject.classData();
    if (!classLayout.staticMethods().empty() || !classLayout.staticFields().empty())
        return nullptr;
    // The table keeps each parent alive, so it is asked for the parents of which a program has a fixed number.
    State& state = classObject.state();
    if (parent != state.instance.globalObject->functionPrototype() && classLayout.evaluation() != EvaluationState::ProvenOnce)
        return nullptr;
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

void Closure::prepareForGenericAccess(VM& vm)
{
    if (isBoundFunction(*this)) [[unlikely]]
        adoptBoundFunctionStructure(vm);
    else
        ensureClassObject(vm);
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

JSObject* createPrototypeObjectOfFunction(VM& vm, JSGlobalObject* globalObject, JSObject* function)
{
    // Both Structures are in the transition tables of the realm's Structure for empty objects, for all functions.
    PropertyOffset offset;
    Structure* structure = Structure::becomePrototypeTransition(vm, globalObject->objectStructureForObjectConstructor());
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->constructor, static_cast<unsigned>(PropertyAttribute::DontEnum), offset);
    JSObject* prototype = constructEmptyObject(vm, structure);
    prototype->putDirectOffset(vm, offset, function);
    return prototype;
}

bool Closure::ensurePrototypeProperty(VM& vm)
{
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    prepareForGenericAccess(vm);
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    if (kind().isConstructor || !FunctionTables::hasPrototypePropertyAtCreation(kind()))
        return false;
    JSGlobalObject* globalObject = state().instance.globalObject;
    JSObject* prototype;
    if (kind().isGenerator)
        prototype = constructEmptyObject(globalObject, kind().isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
    else
        prototype = createPrototypeObjectOfFunction(vm, globalObject, this);
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    state().objects->classes.didCreatePrototypePropertyOf(this);
    state().objects->closures.didGivePrototypePropertyTo(vm, this);
    return true;
}

void Closure::setPrototypeProperty(VM& vm, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(!kind().isConstructor);
    validateIsNotInFrame(vm, this);
    prepareForGenericAccess(vm);
    if (FunctionRareData* rareData = this->rareData())
        rareData->clear("Store to prototype property of a function");
    putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void Closure::setNameProperty(VM& vm, JSString* name, unsigned attributes)
{
    validateIsNotInFrame(vm, this);
    prepareForGenericAccess(vm);
    putDirect(vm, vm.propertyNames->name, name, attributes);
    FunctionRareData* rareData = ensureRareData(vm);
    rareData->setHasReifiedName();
    rareData->setHasModifiedNameForBoundOrNonHostFunction();
}

static bool canDefineStaticMemberWithoutTransition(VM& vm, Closure& classObject, PropertyName key)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (key == names.prototype || key == names.name || key == names.length || key == names.displayName)
        return false;
    if (classObject.isLazyClassObject() || StaticFields::findOwnField(classObject, key.uid()))
        return false;
    StaticFields::ensureProperties(vm, classObject);
    return true;
}

namespace {

// Between NewClass and the end of the definition no code of the program can name the class, so no object inherits from its holders and no cache has walked past them.
class HolderThatNobodyHasSeen {
    WTF_MAKE_NONCOPYABLE(HolderThatNobodyHasSeen);
public:
    explicit HolderThatNobodyHasSeen(Structure* ownStructure)
        : m_structure(ownStructure)
        , m_mayBePrototype(ownStructure->mayBePrototype())
    {
        if (m_mayBePrototype)
            m_structure->setMayBePrototype(false);
    }

    ~HolderThatNobodyHasSeen()
    {
        if (m_mayBePrototype)
            m_structure->setMayBePrototype(true);
    }

private:
    Structure* m_structure;
    bool m_mayBePrototype;
};

} // anonymous namespace

bool tryDefineClassMemberWithoutTransition(JSGlobalObject* globalObject, Closure& classObject, const LayoutData& classLayout, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    validateIsNotInFrame(vm, &classObject);
    if (parseIndex(key))
        return false;
    if (classObject.state().objects->classes.keepsStructuresForNextEvaluation(classObject, classLayout))
        return false;
    bool isStatic = holder == &classObject;
    if (isStatic ? !canDefineStaticMemberWithoutTransition(vm, classObject, key) : holder->type() != FinalObjectType)
        return false;

    // The definition made a Structure for this object alone and has filled it in place.
    Structure* structure = holder->structure();
    bool isOwnStructure = structure->transitionKind() == TransitionKind::Unknown || structure->transitionKind() == TransitionKind::MakeListedPropertiesImmutable;
    if (!isOwnStructure || !structure->isPinnedPropertyTable() || structure->isDictionary())
        return false;
    if (!structure->isStructureExtensible() || structure->isImmutableProperty(key.uid()))
        return false;

    HolderThatNobodyHasSeen unseen(structure);
    constexpr unsigned accessorAttributes = PropertyAttribute::Accessor | PropertyAttribute::DontEnum;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, key, attributes);
    if (kind == MethodKind::Method) {
        if (isValidOffset(offset))
            return false;
        holder->putDirectWithoutTransition(vm, key, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
        return true;
    }

    JSObject* getter = kind == MethodKind::Getter ? function : nullptr;
    JSObject* setter = kind == MethodKind::Setter ? function : nullptr;
    if (!isValidOffset(offset)) {
        holder->putDirectNonIndexAccessorWithoutTransition(vm, key, GetterSetter::create(vm, globalObject, getter, setter), accessorAttributes);
        return true;
    }
    if (attributes != accessorAttributes)
        return false;
    auto* current = uncheckedDowncast<GetterSetter>(holder->getDirect(offset));
    if (!getter && !current->isGetterNull())
        getter = current->getter();
    if (!setter && !current->isSetterNull())
        setter = current->setter();
    structure->didReplaceProperty(offset);
    holder->putDirectOffset(vm, offset, GetterSetter::create(vm, globalObject, getter, setter));
    return true;
}

JSObject* boundFunctionTarget(VM& vm, State& state, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const BoundFunctionData& bound = boundFunctionData(function);
    std::optional<unsigned> slot = bound.targetClosureSlot();
    if (!slot)
        return asObject(state.objects->closures.closure(vm, FunctionID(bound.target)));
    RELEASE_ASSERT(*slot < numberOfReferences && numberOfReferences <= captures.size());
    return std::bit_cast<JSObject*>(captures[*slot]);
}

CallData constructDataOfBoundClosure(FunctionID function)
{
    CallData constructData;
    if (!boundFunctionData(function).constructs)
        return constructData;
    constructData.type = CallData::Type::Native;
    constructData.native.function = constructBoundClosure;
    constructData.native.isBoundFunction = false;
    constructData.native.isWasm = false;
    return constructData;
}

// [[Construct]] of a bound function: the target is constructed with the bound arguments in front of the given ones, and stands for the bound function where that is new.target.
JSC_DEFINE_HOST_FUNCTION(constructBoundClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* callee = callFrame->jsCallee();
    FunctionID function;
    std::span<const uint64_t> captures;
    unsigned numberOfReferences = 0;
    State* state = nullptr;
    auto take = [&](auto* closure) {
        function = closure->function();
        captures = closure->captures();
        numberOfReferences = closure->numberOfReferences();
        state = &closure->state();
    };
    if (auto* slim = dynamicDowncast<SlimClosure>(callee))
        take(slim);
    else if (auto* reified = dynamicDowncast<ReifiedSlimClosure>(callee))
        take(reified);
    else
        take(uncheckedDowncast<Closure>(callee));

    const BoundFunctionData& bound = boundFunctionData(function);
    RELEASE_ASSERT(bound.constructs && bound.firstArgumentSlot + bound.numberOfCapturedArguments <= numberOfReferences);
    JSObject* target = boundFunctionTarget(vm, *state, function, captures, numberOfReferences);
    MarkedArgumentBuffer arguments;
    for (unsigned index = 0; index < bound.numberOfCapturedArguments; ++index) {
        // An argument whose type lists its strings lies there as the number of the string.
        uint64_t word = captures[bound.firstArgumentSlot + index];
        arguments.append(isLiteralStringWord(word) ? JSValue(state->strings->stringForLiteralStringWord(word)) : JSValue::decode(word));
    }
    if (bound.numberOfCapturedArguments == bound.numberOfBoundArguments) {
        for (unsigned index = 0; index < callFrame->argumentCount(); ++index)
            arguments.append(callFrame->uncheckedArgument(index));
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return encodedJSValue();
    }
    JSValue newTarget = callFrame->newTarget();
    if (newTarget == JSValue(callee))
        newTarget = target;
    RELEASE_AND_RETURN(scope, JSValue::encode(constructAllowingPrimitiveResult(globalObject, target, JSC::getConstructData(target), arguments, newTarget)));
}

bool Closure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    closure->prepareForGenericAccess(vm);
    if (closure->kind().isConstructor)
        StaticFields::ensureProperties(vm, *closure);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && FunctionTables::hasPrototypePropertyAtCreation(closure->kind()) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
