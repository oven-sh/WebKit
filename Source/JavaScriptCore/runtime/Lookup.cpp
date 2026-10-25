/*
 *  Copyright (C) 2008, 2012, 2015-2016 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "Lookup.h"

#include "DeferTermination.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include <wtf/text/MakeString.h>

#include "AOTMembersLeftOut.h"
#include "AOTRuntimeHooks.h"

namespace JSC {

void reifyStaticAccessor(VM& vm, const HashTableValue& value, JSObject& thisObject, PropertyName propertyName)
{
    JSGlobalObject* globalObject = thisObject.realm();
    JSObject* getter = nullptr;
    if (value.hasGetter()) {
        if (value.attributes() & PropertyAttribute::Builtin)
            getter = JSFunction::create(vm, globalObject, value.builtinAccessorGetterGenerator()(vm), globalObject);
        else {
            String getterName = tryMakeString("get "_s, String(*propertyName.publicName()));
            if (!getterName)
                return;
            getter = JSFunction::create(vm, globalObject, 0, getterName, value.accessorGetter(), ImplementationVisibility::Public);
        }
    }
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, getter, nullptr);
    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
    thisObject.putDirectNonIndexAccessor(vm, propertyName, accessor, attributesForStructure(value.attributes()));
}

bool setUpStaticFunctionSlot(VM& vm, const ClassInfo* classInfo, const HashTableValue* entry, JSObject* thisObject, PropertyName propertyName, PropertySlot& slot)
{
    ASSERT(thisObject->realm());
    ASSERT(entry->attributes() & PropertyAttribute::BuiltinOrFunctionOrAccessorOrLazyProperty);
    unsigned attributes;
    bool isAccessor = entry->attributes() & PropertyAttribute::Accessor;
    PropertyOffset offset = thisObject->getDirectOffset(vm, propertyName, attributes);

    if (!isValidOffset(offset)) {
        // If a property is ever deleted from an object with a static table, then we reify
        // all static functions at that time - after this we shouldn't be re-adding anything.
        if (thisObject->staticPropertiesReified())
            return false;

        {
            // A PropertyCallback builder can enter JS; defer termination (like
            // LazyProperty::callFunc) so it can't return with one pending.
            DeferTerminationForAWhile deferScope(vm);
            reifyStaticProperty(vm, classInfo, propertyName, *entry, *thisObject);
        }
        // The builder may still throw a non-termination exception; report the
        // slot as not found so JSValue::get / getOwnPropertyDescriptor's
        // EXCEPTION_ASSERT(!scope.exception() || !result) holds. No ThrowScope
        // here: a ThrowScope would simulate a throw on every first static-table
        // lookup, and callers of getOwnPropertySlot don't check for one.
        if (vm.exceptionForInspection()) [[unlikely]] {
#if ENABLE(AOT)
            JSValue standIn = vm.hasCompiledCode() && !isAccessor ? AOT::takeStandInForMemberLeftOut(thisObject->realm(), String(propertyName.uid())) : JSValue();
            if (!standIn)
                return false;
            AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
            thisObject->putDirect(vm, propertyName, standIn, attributesForStructure(entry->attributes()));
#else
            return false;
#endif
        }

        offset = thisObject->getDirectOffset(vm, propertyName, attributes);
        if (!isValidOffset(offset)) {
            dataLog("Static hashtable initialiation for ", propertyName, " did not produce a property.\n");
            RELEASE_ASSERT_NOT_REACHED();
        }

#if ENABLE(AOT)
        if (vm.hasCompiledCode() && !isAccessor) [[unlikely]]
            AOT::RuntimeHooks::didCreateStaticProperty(thisObject, propertyName.uid(), thisObject->getDirect(offset));
#endif
    }

    if (isAccessor)
        slot.setCacheableGetterSlot(thisObject, attributes, uncheckedDowncast<GetterSetter>(thisObject->getDirect(offset)), offset);
    else
        slot.setValue(thisObject, attributes, thisObject->getDirect(offset), offset);
    return true;
}

bool setUpCustomSlotWithoutRemovedFunctions(const ClassInfo* classInfo, const HashTableValue* entry, JSObject* thisObject, PropertySlot& slot)
{
    unsigned attributes = attributesForStructure(entry->attributes());
    if (entry->attributes() & PropertyAttribute::DOMJITAttribute) {
        const DOMJIT::GetterSetter* domJIT = entry->domJIT();
        slot.setCacheableCustom(thisObject, attributes, functionUnlessRemoved(GetValueFunc(domJIT->getter())), functionUnlessRemoved(entry->domJITSetter()), DOMAttributeAnnotation { classInfo, domJIT });
        return true;
    }

    GetValueFunc getter = functionUnlessRemoved(entry->propertyGetter());
    PutValueFunc setter = functionUnlessRemoved(entry->propertyPutter());
    if (entry->attributes() & PropertyAttribute::DOMAttribute)
        slot.setCacheableCustom(thisObject, attributes, getter, setter, DOMAttributeAnnotation { classInfo, nullptr });
    else
        slot.setCacheableCustom(thisObject, attributes, getter, setter);
    return true;
}

} // namespace JSC
