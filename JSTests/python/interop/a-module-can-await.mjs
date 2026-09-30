// JavaScript imports a module of Python's that awaits, and has all of it.
import { first, second } from "./awaits_at_the_top.py";
print(first, second);
const again = await import("./awaits_at_the_top.py");
print(again.first === first, again.second);
