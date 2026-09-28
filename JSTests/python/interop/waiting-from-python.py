# Nothing of JavaScript's begins this: a program in Python, which sets coroutines going and comes to its end. They go on when it has.
import js
log = []
async def tick(name, n):
    for i in range(n):
        print(name, i)
        await js.Promise.resolve()
    return name
async def main():
    print("main begins")
    done = await js.Promise.all([tick("a", 2), tick("b", 3)])
    print("both done:", list(done))
    await js.Promise.new(lambda resolve, reject: js.setTimeout(resolve, 1))
    print("after a timer")
    try:
        await js.Promise.reject(js.TypeError.new("rejected"))
    except TypeError as e:
        print("caught", type(e).__name__, e)
    return "main is done"
p = js.Promise.resolve(main())
p.then(lambda v: print("then:", v))
print("the program comes to its end")
