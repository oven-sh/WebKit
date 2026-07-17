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

        offset = thisObject->getDirectOffset(vm, propertyName, attributes);
        if (!isValidOffset(offset)) {
            // reifyStaticProperty skips the putDirect when a PropertyCallback
            // builder throws (returns empty) or when the defer scope re-throws
            // a suspended termination. Report the slot as not found so
            // JSValue::get / getOwnPropertyDescriptor's
            // EXCEPTION_ASSERT(!scope.exception() || !result) still holds. A
            // ThrowScope here would force every getOwnNonIndexPropertySlot
            // caller to add an exception check on the property-lookup fast
            // path, so key on the missing offset instead.
            ASSERT(vm.exceptionForInspection());
            return false;
        }
    }

    if (isAccessor)
        slot.setCacheableGetterSlot(thisObject, attributes, uncheckedDowncast<GetterSetter>(thisObject->getDirect(offset)), offset);
    else
        slot.setValue(thisObject, attributes, thisObject->getDirect(offset), offset);
    return true;
}

} // namespace JSC
