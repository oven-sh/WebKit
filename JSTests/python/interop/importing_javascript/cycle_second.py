# It is imported by cycle_first.mjs, which it imports. What JavaScript imports that is Python's is run before any of what imports it is. So it is here that cycle_first.mjs is first asked for,
# and here that it is run, as it would be by require().
import sys
before = "set before"
import cycle_first
first_when_this_was_run = [sorted(n for n in dir(cycle_first) if n[0] != "_"), cycle_first.hoisted(), cycle_first.early, sys.modules["cycle_first"] is cycle_first, cycle_first.__name__]
after = "set after"
