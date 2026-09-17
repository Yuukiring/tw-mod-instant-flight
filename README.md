# tw-mod-instant-flight

Replaces the default taxi flight system with an instant teleport to the destination flight master for the normal flight cost.

## Configuration

Copy `conf/tw-mod-instant-flight.conf.dist` to your module config directory as `tw-mod-instant-flight.conf` and adjust:

```ini
[InstantFlight]
InstantFlight.Enable = 1
InstantFlight.RequiredItemId = 0
```

- `Enable`: set to `0` to disable the module and restore the default flying behavior.
- `RequiredItemId`: item the player must carry in their bags to activate instant flight. Accepts a single item id or a comma separated list of item ids. Leave at `0` (default) to require no item — the player then flies normally.

## How it works

The module hooks `SERVERHOOK_CAN_PACKET_RECEIVE` and intercepts `CMSG_ACTIVATETAXI` and `CMSG_ACTIVATETAXIEXPRESS`.

When a flight is requested:

1. The module validates that the player knows all requested taxi nodes. This check is skipped when the player has taxi cheat enabled (e.g. GMs or when the world config `AllFlightPaths` is enabled), matching the core taxi handler behavior.
2. It calculates the normal flight cost by summing the cost of each leg using `sObjectMgr.GetTaxiPath()`.
3. If the player has enough money, the cost is deducted and the player is teleported to the final destination node.
4. A success response is sent to the client so the taxi UI closes cleanly, and the original flight packet is cancelled so no flight path animation occurs.

If validation fails or the player cannot afford the flight, the packet is passed through to the normal handler so the client receives the appropriate error.

## Build

```sh
cmake -S . -B build -DMODULES=static
cmake -S . -B build -DMODULES=dynamic
# or per-module:
cmake -S . -B build -DMODULE_TW_MOD_INSTANT_FLIGHT=static
```
