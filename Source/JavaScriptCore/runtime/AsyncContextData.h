#pragma once

#include "JSWrapperObject.h"

namespace JSC {

class AsyncContextData : public JSWrapperObject {
protected:
    JS_EXPORT_PRIVATE AsyncContextData(VM&, Structure*);
    DECLARE_DEFAULT_FINISH_CREATION;

public:
    using Base = JSWrapperObject;

    template<typename, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.asyncContextDataSpace();
    }

    static AsyncContextData* create(VM& vm, Structure* structure)
    {
        AsyncContextData* asyncContextData = new (NotNull, allocateCell<AsyncContextData>(vm)) AsyncContextData(vm, structure);
        asyncContextData->finishCreation(vm);
        return asyncContextData;
    }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }
        
    DECLARE_EXPORT_INFO;
};

} // namespace JSC
