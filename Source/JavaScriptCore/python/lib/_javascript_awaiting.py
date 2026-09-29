# What JavaScript waits for is run as a coroutine. If it is not one, it is awaited by this, which is what asyncio.ensure_future() does with it, and for the same reason: what is being run may turn out to be a task of
# asyncio's, and a task is of a coroutine. See toPromise().


async def _wrap_awaitable(awaitable):
    return await awaitable
