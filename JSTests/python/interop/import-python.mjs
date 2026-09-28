import shapes, { add, Point, numbers, __name__ as shapesName } from "./shapes.py";
import * as ns from "./shapes.py";
import inner, { VALUE, name } from "./pkg/inner.py";
import pkg from "./pkg/__init__.py";
print(typeof shapes, shapesName, add(1, 2), Point(3, 4).norm2(), numbers.length);
print(ns.default === shapes, ns.add === add, Object.keys(ns).filter(k => !k.startsWith("__")).join());
print(VALUE, name(), pkg.inner === inner, pkg.VALUE);
const again = await import("./shapes.py");
print(again.default === shapes);
try { await import("./nope.py"); } catch (e) { print("missing:", String(e).slice(0, 27)); }
try { await import("./bad.py"); } catch (e) { print("bad:", e instanceof SyntaxError, e.name, e.message); }
try { await import("./raises.py"); } catch (e) { print("raises:", e.name, e.message, e instanceof Error); }
