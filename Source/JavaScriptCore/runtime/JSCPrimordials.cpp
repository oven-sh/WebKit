/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

#include "JSCPrimordials.h"

#include "GetterSetter.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "JSCustomGetterFunction.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "LazyPropertyInlines.h"
#include "LinkTimeConstant.h"
#include "Lookup.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "Symbol.h"
#include "TopExceptionScope.h"

namespace JSC {

namespace Primordials {

struct Entry {
    PrimordialKind kind;
    Identifier (*makeKey)(VM&);
    ASCIILiteral name;
};

#define JSC_PRIMORDIAL_KEY_Method(key) [](VM& vm) -> Identifier { return Identifier::fromString(vm, key ""_s); }
#define JSC_PRIMORDIAL_KEY_Getter(key) [](VM& vm) -> Identifier { return Identifier::fromString(vm, key ""_s); }
#define JSC_PRIMORDIAL_KEY_SymbolMethod(key) [](VM& vm) -> Identifier { return vm.propertyNames->key##Symbol; }
#define JSC_PRIMORDIAL_KEY_SymbolGetter(key) [](VM& vm) -> Identifier { return vm.propertyNames->key##Symbol; }
#define JSC_PRIMORDIAL_ENTRY(name, key, kind) { PrimordialKind::kind, JSC_PRIMORDIAL_KEY_##kind(key), #name ""_s },
static const Entry s_entries[numberOfPrimordials] = {
    JSC_FOREACH_PRIMORDIAL_NAME(JSC_PRIMORDIAL_ENTRY)
};
#undef JSC_PRIMORDIAL_ENTRY
#undef JSC_PRIMORDIAL_KEY_Method
#undef JSC_PRIMORDIAL_KEY_Getter
#undef JSC_PRIMORDIAL_KEY_SymbolMethod
#undef JSC_PRIMORDIAL_KEY_SymbolGetter

#define JSC_PRIMORDIAL_COUNT_ENTRY(name, key, kind) 1 +
#define JSC_PRIMORDIAL_HOLDER_COUNT(Holder) (JSC_FOREACH_PRIMORDIAL_##Holder(JSC_PRIMORDIAL_COUNT_ENTRY) 0),
static constexpr unsigned s_holderCounts[] = {
    JSC_FOREACH_PRIMORDIAL_HOLDER(JSC_PRIMORDIAL_HOLDER_COUNT)
};
#undef JSC_PRIMORDIAL_HOLDER_COUNT
#undef JSC_PRIMORDIAL_COUNT_ENTRY

#define JSC_PRIMORDIAL_HOLDER_NAME(Holder) #Holder ""_s,
static const ASCIILiteral s_holderNames[] = {
    JSC_FOREACH_PRIMORDIAL_HOLDER(JSC_PRIMORDIAL_HOLDER_NAME)
};
#undef JSC_PRIMORDIAL_HOLDER_NAME

static unsigned firstLinkTimeConstantIndex()
{
    return numberOfLinkTimeConstants - numberOfPrimordials;
}

static unsigned indexFor(LinkTimeConstant constant)
{
    unsigned index = static_cast<unsigned>(constant) - firstLinkTimeConstantIndex();
    ASSERT(index < numberOfPrimordials);
    return index;
}

static LinkTimeConstant linkTimeConstantFor(unsigned index)
{
    return static_cast<LinkTimeConstant>(firstLinkTimeConstantIndex() + index);
}

static constexpr unsigned numberOfHolders = std::size(s_holderCounts);

// s_holderBegin[h] is the first primordial index of holder h; the extra entry is the total.
static constexpr auto s_holderBegin = [] {
    std::array<unsigned, numberOfHolders + 1> begins { };
    for (unsigned h = 0; h < numberOfHolders; ++h)
        begins[h + 1] = begins[h] + s_holderCounts[h];
    return begins;
}();

static PrimordialHolder holderFor(unsigned index)
{
    return static_cast<PrimordialHolder>(std::upper_bound(s_holderBegin.begin(), s_holderBegin.end(), index) - s_holderBegin.begin() - 1);
}

static std::pair<unsigned, unsigned> rangeFor(PrimordialHolder holder)
{
    unsigned h = static_cast<unsigned>(holder);
    return { s_holderBegin[h], s_holderBegin[h + 1] };
}

static bool isGetterKind(PrimordialKind kind)
{
    return kind == PrimordialKind::Getter || kind == PrimordialKind::SymbolGetter;
}

// A table entry not matched by any of reifyStaticProperty's typed cases is a
// C++ getter/setter pair reified as a CustomGetterSetter.
static bool isCustomAccessorEntry(const HashTableValue& entry)
{
    static constexpr unsigned typedMask = PropertyAttribute::Builtin | PropertyAttribute::Function | PropertyAttribute::ConstantInteger
        | PropertyAttribute::Accessor | PropertyAttribute::CellProperty | PropertyAttribute::ClassStructure | PropertyAttribute::PropertyCallback
        | PropertyAttribute::DOMJITAttribute | PropertyAttribute::DOMJITFunction | PropertyAttribute::DOMAttribute;
    return !(entry.attributes() & typedMask);
}

static bool matchesEntry(VM& vm, JSFunction* function, const HashTableValue& entry, bool asGetter)
{
    unsigned attributes = entry.attributes();
    if (attributes & PropertyAttribute::Builtin) {
        BuiltinGenerator generator = asGetter ? entry.builtinAccessorGetterGenerator() : entry.builtinGenerator();
        if (!function->isBuiltinFunction())
            return false;
        return uncheckedDowncast<FunctionExecutable>(function->executable())->unlinkedExecutable() == generator(vm)->unlinkedExecutable();
    }
    NativeFunction native = asGetter ? NativeFunction(entry.accessorGetter()) : entry.function();
    return function->isHostFunction() && function->nativeFunction() == native;
}

// Unwraps an own property value of the requested kind, ignoring anything that
// is not a function (or accessor with a getter function).
static JSCell* cellForOwnProperty(JSValue value, PrimordialKind kind)
{
    if (!value)
        return nullptr;
    if (isGetterKind(kind)) {
        if (!value.isGetterSetter())
            return nullptr;
        return uncheckedDowncast<GetterSetter>(value.asCell())->getter();
    }
    if (!value.isCell() || !value.asCell()->isObject())
        return nullptr;
    return value.asCell();
}

// A reified static property is trusted only if it is still the exact function
// (or accessor) the ClassInfo table describes; anything else is user-installed.
static JSCell* cellIfMatchesEntry(VM& vm, JSValue value, const HashTableValue& entry, PrimordialKind kind)
{
    if (!value)
        return nullptr;
    if (isGetterKind(kind)) {
        if (!value.isGetterSetter())
            return nullptr;
        auto* getter = dynamicDowncast<JSFunction>(uncheckedDowncast<GetterSetter>(value.asCell())->getter());
        if (!getter || !matchesEntry(vm, getter, entry, true))
            return nullptr;
        return getter;
    }
    auto* function = value.isCell() ? dynamicDowncast<JSFunction>(value.asCell()) : nullptr;
    if (!function || !matchesEntry(vm, function, entry, false))
        return nullptr;
    return function;
}

// Builds a fresh, un-installed copy of the property from its immutable table
// entry; used when the holder's own state can't be trusted.
static JSCell* materializeFromEntry(VM& vm, JSGlobalObject* globalObject, const Identifier& key, const HashTableValue& entry, PrimordialKind kind)
{
    unsigned attributes = entry.attributes();
    bool wantGetter = isGetterKind(kind);
    if (isCustomAccessorEntry(entry)) {
        ASSERT(wantGetter);
        return customGetterFunctionForPrimordial(globalObject, vm, key, entry.propertyGetter());
    }
    if (attributes & PropertyAttribute::Builtin) {
        BuiltinGenerator generator = wantGetter ? entry.builtinAccessorGetterGenerator() : entry.builtinGenerator();
        return JSFunction::create(vm, globalObject, generator(vm), globalObject);
    }
    if (attributes & PropertyAttribute::Accessor) {
        ASSERT(wantGetter);
        String getterName = tryMakeString("get "_s, String(PropertyName(key).publicName()));
        return JSFunction::create(vm, globalObject, 0, getterName, entry.accessorGetter(), ImplementationVisibility::Public);
    }
    ASSERT(attributes & PropertyAttribute::Function);
    StringImpl* name = PropertyName(key).publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    return JSFunction::create(vm, globalObject, entry.functionLength(), name, entry.function(), ImplementationVisibility::Public, entry.intrinsic());
}

// Reuses an already-reified pristine value for identity with `holder[key]`;
// reifies the (untouched) static property first if nothing has yet.
static JSCell* pristineFromHolderOwnProperty(VM& vm, JSObject* holder, const ClassInfo* entryClassInfo, const Identifier& key, const HashTableValue& entry, PrimordialKind kind)
{
    if (isCustomAccessorEntry(entry))
        return nullptr;
    unsigned attributes;
    PropertyOffset offset = holder->getDirectOffset(vm, key, attributes);
    if (!isValidOffset(offset)) {
        if (holder->staticPropertiesReified())
            return nullptr;
        {
            auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            DeferTerminationForAWhile deferScope(vm);
            reifyStaticProperty(vm, entryClassInfo, key, entry, *holder);
            catchScope.assertNoExceptionExceptTermination();
        }
        offset = holder->getDirectOffset(vm, key, attributes);
        if (!isValidOffset(offset))
            return nullptr;
    }
    return cellIfMatchesEntry(vm, holder->getDirect(offset), entry, kind);
}

JSC_DEFINE_HOST_FUNCTION(primordialUnavailableHostFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = uncheckedDowncast<JSFunction>(callFrame->jsCallee())->name(vm);
    RETURN_IF_EXCEPTION(scope, { });
    return throwVMTypeError(globalObject, scope, makeString("primordial "_s, name, " is not available in this configuration"_s));
}

static JSFunction* unavailableFunction(VM& vm, JSGlobalObject* globalObject, ASCIILiteral name)
{
    return JSFunction::create(vm, globalObject, 0, name, primordialUnavailableHostFunction, ImplementationVisibility::Public);
}

static bool isUnavailableFunction(JSCell* cell)
{
    auto* function = dynamicDowncast<JSFunction>(cell);
    return function && function->isHostFunction() && function->nativeFunction() == primordialUnavailableHostFunction;
}

#define JSC_PRIMORDIAL_COUNT_HOLDER(Holder) 1 +
static constexpr unsigned numberOfEagerHolders = JSC_FOREACH_PRIMORDIAL_EAGER_HOLDER(JSC_PRIMORDIAL_COUNT_HOLDER) 0;
#undef JSC_PRIMORDIAL_COUNT_HOLDER

} // namespace Primordials

void JSGlobalObject::initializePrimordialLinkTimeConstants()
{
    using namespace Primordials;
    auto materializeLater = [](const Initializer<JSCell>& init) {
        unsigned slot = &init.property - init.owner->m_linkTimeConstants.begin();
        init.set(init.owner->materializePrimordial(init.vm, static_cast<LinkTimeConstant>(slot)));
    };
    for (unsigned i = 0; i < numberOfPrimordials; ++i)
        m_linkTimeConstants[static_cast<unsigned>(linkTimeConstantFor(i))].initLater(materializeLater);
}

void JSGlobalObject::snapshotEagerPrimordials(VM& vm)
{
    using namespace Primordials;
    for (unsigned h = 0; h < numberOfEagerHolders; ++h) {
        auto holder = static_cast<PrimordialHolder>(h);
        snapshotPrimordialsFromHolder(vm, primordialHolderObject(vm, holder), holder);
    }
}

// Only own properties are read; static-table properties are materialized from
// the tables on first link. A namespace holder can be created more than once (a
// PropertyCallback re-run), so an existing snapshot wins unless the host overrides.
void JSGlobalObject::snapshotPrimordialsFromHolder(VM& vm, JSObject* holder, PrimordialHolder which, bool overrideExisting)
{
    using namespace Primordials;
    ASSERT(holder);
    auto [begin, end] = rangeFor(which);
    for (unsigned i = begin; i < end; ++i) {
        auto& slot = m_linkTimeConstants[static_cast<unsigned>(linkTimeConstantFor(i))];
        if (slot.isInitialized() && !overrideExisting)
            continue;
        JSCell* cell = cellForOwnProperty(holder->getDirect(vm, s_entries[i].makeKey(vm)), s_entries[i].kind);
        if (!cell)
            continue;
        slot.set(vm, this, cell);
    }
}

void JSGlobalObject::overridePrimordialsFromHolder(VM& vm, JSObject* holder, PrimordialHolder which)
{
    snapshotPrimordialsFromHolder(vm, holder, which, true);
}

JSCell* JSGlobalObject::materializePrimordialFromTables(VM& vm, JSObject* holder, unsigned index)
{
    using namespace Primordials;
    const Entry& info = s_entries[index];
    Identifier key = info.makeKey(vm);
    if (holder) {
        if (auto entry = holder->findPropertyHashEntry(key)) {
            if (JSCell* cell = pristineFromHolderOwnProperty(vm, holder, entry->table->classForThis, key, *entry->value, info.kind))
                return cell;
            return materializeFromEntry(vm, this, key, *entry->value, info.kind);
        }
    }
    return unavailableFunction(vm, this, info.name);
}

JSCell* JSGlobalObject::materializePrimordial(VM& vm, LinkTimeConstant constant)
{
    using namespace Primordials;
    unsigned index = indexFor(constant);
    auto& slot = m_linkTimeConstants[static_cast<unsigned>(constant)];
    // Forcing a lazy holder into existence snapshots its own properties, which may
    // initialize this slot.
    JSObject* holder = primordialHolderObject(vm, holderFor(index));
    if (slot.isInitialized())
        return slot.getInitializedOnMainThread(this);
    return materializePrimordialFromTables(vm, holder, index);
}

JSValue JSGlobalObject::auditPrimordials(JSGlobalObject* globalObject)
{
    using namespace Primordials;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* result = constructEmptyArray(globalObject, nullptr, numberOfPrimordials);
    RETURN_IF_EXCEPTION(scope, { });
    static constexpr ASCIILiteral kindNames[] = { "Method"_s, "Getter"_s, "SymbolMethod"_s, "SymbolGetter"_s };
    for (unsigned i = 0; i < numberOfPrimordials; ++i) {
        JSCell* value = globalObject->linkTimeConstant(linkTimeConstantFor(i));
        Identifier key = s_entries[i].makeKey(vm);
        JSObject* row = constructEmptyObject(globalObject);
        row->putDirect(vm, Identifier::fromString(vm, "name"_s), jsString(vm, String(s_entries[i].name)));
        row->putDirect(vm, Identifier::fromString(vm, "holder"_s), jsString(vm, String(s_holderNames[static_cast<unsigned>(holderFor(i))])));
        row->putDirect(vm, Identifier::fromString(vm, "kind"_s), jsString(vm, String(kindNames[static_cast<unsigned>(s_entries[i].kind)])));
        row->putDirect(vm, Identifier::fromString(vm, "key"_s), key.isSymbol() ? JSValue(Symbol::create(vm, static_cast<SymbolImpl&>(*key.impl()))) : jsString(vm, String(key.impl())));
        row->putDirect(vm, vm.propertyNames->value, value);
        row->putDirect(vm, Identifier::fromString(vm, "available"_s), jsBoolean(!isUnavailableFunction(value)));
        result->putDirectIndex(globalObject, i, row);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
