"""Desk-testing relay: forwards TCP from the LAN to a Signal K server the display can't reach
directly (e.g. one reached over a VPN). Set the display's Signal K server to
this PC on its setup page (or SK_HOST_OVERRIDE/SK_PORT_OVERRIDE in src/secrets.h).

    python tools/sk_relay.py --listen 3001 --target <server>:80
"""

import argparse
import asyncio


async def pipe(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, default=3001)
    ap.add_argument("--target", required=True, help="Signal K server, host:port")
    args = ap.parse_args()
    host, port = args.target.rsplit(":", 1)

    async def handle(client_r, client_w):
        peer = client_w.get_extra_info("peername")
        try:
            server_r, server_w = await asyncio.open_connection(host, int(port))
        except OSError as e:
            print(f"{peer}: cannot reach {args.target}: {e}")
            client_w.close()
            return
        print(f"{peer} -> {args.target}")
        await asyncio.gather(pipe(client_r, server_w), pipe(server_r, client_w))
        print(f"{peer} closed")

    server = await asyncio.start_server(handle, "0.0.0.0", args.listen)
    print(f"relaying 0.0.0.0:{args.listen} -> {args.target}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
