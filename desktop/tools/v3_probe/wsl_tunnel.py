# Copied from the 2026-11-04 assessment scripts for the M4 run (docs/signal-integration/). Loopback only.
# Reverse tunnel so a Linux app in WSL can reach a server on Windows' 127.0.0.1 (assessment only).
# WSL side:     python3 revtunnel.py wsl   -> app port 127.0.0.1:3055, agent port 127.0.0.1:3056 (both loopback in WSL)
# Windows side: python -I revtunnel.py win -> keeps idle connections to WSL's 3056 (reachable through WSL's localhost
#               forwarding); when one is claimed it connects to Windows 127.0.0.1:3055 and pipes the bytes.
import asyncio
import sys

APP_PORT, AGENT_PORT, TARGET = 3055, 3056, ("127.0.0.1", 3055)
POOL = 16


async def pipe(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, OSError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def wsl_side():
    idle: asyncio.Queue = asyncio.Queue()

    async def on_agent(reader, writer):
        await idle.put((reader, writer))

    async def on_app(app_reader, app_writer):
        while True:
            agent_reader, agent_writer = await idle.get()
            if agent_writer.is_closing():
                continue
            try:
                agent_writer.write(b"G")  # go: the agent opens the real connection
                await agent_writer.drain()
            except (ConnectionError, OSError):
                continue
            break
        await asyncio.gather(pipe(app_reader, agent_writer), pipe(agent_reader, app_writer))

    a = await asyncio.start_server(on_agent, "127.0.0.1", AGENT_PORT)
    b = await asyncio.start_server(on_app, "127.0.0.1", APP_PORT)
    print("wsl side ready", flush=True)
    async with a, b:
        await asyncio.gather(a.serve_forever(), b.serve_forever())


async def win_side():
    async def one():
        while True:
            try:
                reader, writer = await asyncio.open_connection("127.0.0.1", AGENT_PORT)
                go = await reader.readexactly(1)
                if go != b"G":
                    writer.close()
                    continue
                t_reader, t_writer = await asyncio.open_connection(*TARGET)
                asyncio.ensure_future(asyncio.gather(pipe(reader, t_writer), pipe(t_reader, writer)))
            except (ConnectionError, OSError, asyncio.IncompleteReadError):
                await asyncio.sleep(0.5)

    print("windows side running", flush=True)
    await asyncio.gather(*(one() for _ in range(POOL)))


asyncio.run(wsl_side() if sys.argv[1] == "wsl" else win_side())
