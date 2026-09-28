import js
name = "World"
count = 3
greeting = t"Hello {name}, {count!r:>4} times"
def tag(strings, *values):
    return (type(strings).__name__, list(strings), values)
def make(value):
    return t"<{value}>"
def with_javascript_values():
    m = js.Map.new()
    return t"{m}{js.undefined}{js.Math.PI:.2f}"
