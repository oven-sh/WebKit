import builtins, sys
def gen(): yield
async def co(): pass
async def ag(): yield
class C:
    def m(self): pass
    @property
    def p(self): pass
c = co()
extra = {
    "function": type(gen), "generator": type(gen()), "coroutine": type(c), "async_generator": type(ag()), "method": type(C().m), "builtin_function_or_method": type(len),
    "method_descriptor": type(str.join), "wrapper_descriptor": type(int.__add__), "method-wrapper": type((1).__add__), "getset_descriptor": type(type.__dict__["__dict__"]),
    "member_descriptor": type(type(gen).__globals__) if False else type(type(gen).__dict__["__globals__"]), "code": type(gen.__code__), "frame": type(sys._getframe()), "module": type(sys), "NoneType": type(None),
    "NotImplementedType": type(NotImplemented), "ellipsis": type(...), "mappingproxy": type(C.__dict__), "dict_keys": type({}.keys()), "dict_values": type({}.values()), "dict_items": type({}.items()),
    "list_iterator": type(iter([])), "tuple_iterator": type(iter(())), "str_ascii_iterator": type(iter("")), "range_iterator": type(iter(range(1))), "set_iterator": type(iter(set())), "dict_keyiterator": type(iter({})),
    "cell": type((lambda x: lambda: x)(1).__closure__[0]), "traceback": None,
}
c.close()
try:
    raise ValueError
except ValueError as e:
    extra["traceback"] = type(e.__traceback__)
types = {n: v for n, v in vars(builtins).items() if isinstance(v, type)}
types.update(extra)
for name in sorted(types):
    print(name, " ".join(sorted(k + ":" + type(v).__name__ for k, v in vars(types[name]).items())))
print("builtins", " ".join(sorted(n for n in vars(builtins))))
