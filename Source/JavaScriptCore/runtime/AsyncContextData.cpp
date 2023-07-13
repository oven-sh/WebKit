#include "config.h"
#include "AsyncContextData.h"

#include "JSCInlines.h"

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(AsyncContextData);

const ClassInfo AsyncContextData::s_info = { "AsyncContextData"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(AsyncContextData) };

AsyncContextData::AsyncContextData(VM& vm, Structure* structure)
    : Base(vm, structure)
{
    setInternalValue(vm, jsUndefined());
}

} // namespace JSC
