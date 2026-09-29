# JavaScript waits for what uses asyncio, in a host that has no event loop to turn one of asyncio's with. See "What JavaScript waits for, and asyncio" in README.md.
import asyncio

loop = asyncio.new_event_loop()


class PythonFuture(asyncio.futures._PyFuture):
    pass


async def asks():
    "There is no loop, as there is none at the top of a program"
    try:
        return asyncio.get_running_loop()
    except RuntimeError as e:
        return "RuntimeError: " + str(e)


async def awaits(future):
    "What a future needs is a task, and there is no loop for this to be a task of. It is told so, and can go on"
    try:
        return await future
    except RuntimeError as e:
        return "RuntimeError: " + str(e)


def pending(kind):
    return (PythonFuture if kind == "Python" else asyncio.Future)(loop=loop)


def done(kind, how):
    future = pending(kind)
    if how == "result":
        future.set_result("the result")
    elif how == "exception":
        future.set_exception(KeyError("k"))
    else:
        future.cancel("why")
    return future


class Generator:
    def __await__(self):
        return (yield from awaits(done("C", "result")).__await__())


async def in_a_loop():
    "With a loop of Python's running, what JavaScript is given to wait for is a task of that loop from when it first asks"
    import js
    seen = []

    async def child():
        seen.append([asyncio.get_running_loop() is asyncio.get_event_loop(), asyncio.current_task() is not mine, asyncio.current_task() in asyncio.all_tasks(), asyncio.current_task().get_coro().__name__])
        await asyncio.sleep(0)
        seen.append("went on, run by the loop")
        return "the child's"

    mine = asyncio.current_task()
    coroutine = child()
    js.eval("coroutine => { coroutine.then(() => { }); }")(coroutine)
    seen.append(["it was begun at once, and this is still the task that is running", asyncio.current_task() is mine])
    task, = asyncio.all_tasks() - {mine}
    seen.append([type(task).__name__, task.get_coro() is coroutine, await task])
    return seen


def run_in_a_loop():
    return asyncio.run(in_a_loop())
