# Deployment

## Network requirements

The relay listens on one UDP port. Allow inbound UDP traffic in the
operating-system and server firewalls. TCP is no longer used by the relay. The
default port is `34197`.

Minimum practical private-server size is one CPU core and 1 GB RAM. Bandwidth
depends on lobby size and the rate of semantic rendering snapshots.

## Windows

The release ZIP contains a self-contained command-line server and an optional
graphical launcher. No Visual C++ redistributable installation is required.

For unattended use, run the command-line server under a service wrapper or
scheduled task that restarts it after failure or reboot. Preserve the
`--public-host` and `--public-port` arguments because they determine the relay
code printed at startup.

## Linux

Configure the relay-only project with the launcher disabled, then build and
test it:

```sh
cmake -S tools/multiplayer_relay -B build/relay-release \
  -DCMAKE_BUILD_TYPE=Release -DDUSKLIGHT_RELAY_BUILD_LAUNCHER=OFF
cmake --build build/relay-release
ctest --test-dir build/relay-release --output-on-failure
```

## Updating

1. Stop the old relay process.
2. Replace the relay executable and, on Windows, its launcher.
3. Start it with the same endpoint arguments.
4. Confirm the printed relay code and test from outside the server network.

Rooms exist only in memory. Updating disconnects active lobbies but cannot
damage game saves because the relay never stores save data.

## VPN port forwarding

Bind locally to `0.0.0.0` and advertise the VPN provider's public address and
forwarded port. If either public value changes, restart the relay and distribute
the new relay code. Existing UDP sessions cannot survive an endpoint
change.
