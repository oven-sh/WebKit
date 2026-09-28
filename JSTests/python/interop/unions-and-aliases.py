import js
def show(label, f):
    try: print(label, "=>", f())
    except BaseException as e: print(label, "!!", type(e).__name__, e)
show("js class in a union", lambda: (js.Map | int, int | js.Map, js.Map | None, js.Map | js.Set, (js.Map | int).__args__))
show("collapses", lambda: js.Map | js.Map)
show("isinstance", lambda: (isinstance(js.Map.new(), js.Map | int), isinstance(js.Set.new(), js.Map | int), isinstance(1, js.Map | int), issubclass(js.Map, js.Map | int)))
show("js class as an argument", lambda: (list[js.Map], dict[str, js.Promise], list[js.Map].__args__[0] is js.Map))
show("js class is not generic", lambda: js.Map[int])
show("alias of a js class", lambda: (type(list[int])(js.Map, int), type(type(list[int])(js.Map, int)()).__name__))
show("equal and hash", lambda: ((js.Map | int) == (int | js.Map), hash(js.Map | int) == hash(int | js.Map)))
def annotated(a: js.Map | None, b: list[js.Date]) -> js.Promise: pass
show("annotations", lambda: annotated.__annotations__)
IntOrStr = int | str
ListOfInt = list[int]
Int, Str, List = int, str, list
class Py: pass
