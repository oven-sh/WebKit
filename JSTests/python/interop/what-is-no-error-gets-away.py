# JavaScript can throw anything. What is no Error is no exception, so `except` does not catch it, and if nothing of JavaScript's does either it is said what it was.
import js

try:
    js.eval("throw 5")
except BaseException as e:
    print("caught", e)
finally:
    print("on the way out")
