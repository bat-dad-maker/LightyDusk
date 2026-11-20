# Companion ZeppOS

The current implementation is in [companion/zepp](../companion/zepp/README.md).

Version 0.2.0 maintains the map in the Amazon GTR 4 New and receives coordinates, Link direction, pins/stamps and indicators of the selected mission. The crown controls four zoom levels; the touch moves the map and the button **Center Link** resumes the accompaniment.

```text
Leitura de estado no runtime → HTTP autenticado em loopback
    → Side Service do Zepp no mesmo Android → Bluetooth → mapa local no relógio
```

The target is deviceSource 9437441, 480×480 and API 1.0, according to Zeus' installed catalog. The native bridge is in `runtime/watch_map.c`; the server and activation are in `WatchMapServer.java`. Only the validated Switch 1.0.0 revision is accepted. Data extracted from the local dump is off the Git.

The README contains export instructions, installation, controls, protocol, checks performed and limits. The previous prototype of minimap capture is available as a legacy diagnosis.
