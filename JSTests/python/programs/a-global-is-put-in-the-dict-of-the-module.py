# To give a global variable a value is to put it in the dict of the module. What the class of the module has to say about setting attributes is not asked.
import sys, types, time
log = []
class M(types.ModuleType):
    def __setattr__(self, name, value):
        log.append(("__setattr__", name))
        super().__setattr__(name, value)
    def __delattr__(self, name):
        log.append(("__delattr__", name))
        super().__delattr__(name)
    def __getattr__(self, name):
        log.append(("__getattr__", name))
        raise AttributeError(name)
    @property
    def prop(self): return "the property"
    @prop.setter
    def prop(self, v): log.append(("setter", v))
me = sys.modules[__name__]
me.__class__ = M
a = 1
prop = 2
def f():
    global b, prop
    b = 3
    prop = 4
f()
for i in range(3):
    c = i
del a
print(log, b, c, prop, me.prop, me.__dict__["prop"])
me.d = 5
print(log, d)
log.clear()
try:
    missing
except NameError as e:
    print(e, log)
def hot(n):
    global counter
    counter = 0
    for i in range(n):
        counter = counter + 1
    return counter
print(hot(200000), counter, log)
__class__ = 5
__dict__ = 6
__setattr__ = 7
print(__class__, __dict__, __setattr__, type(me).__name__, log)
del __class__, __dict__, __setattr__
me.e = 1
print(log)
