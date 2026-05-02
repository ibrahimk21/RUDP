# Benchmark topology preflight

`tools/preflight_topology.sh --check` is read-only. It checks command support,
netem, HTB, CUBIC, and BBR. `--run` requires root or equivalent
CAP_NET_ADMIN and creates this disposable topology:

```text
sender -- shaper -- delay -- receiver
```

The forward HTB bottleneck is on the shaper-to-delay link, then netem is
applied from delay to receiver. The reverse direction is symmetric: HTB is on
delay-to-shaper, followed by netem from shaper to sender. The script sets the
specified 1500-byte MTU defaults, disables TSO/GSO/GRO when supported, uses
the 20-Mbit/s/50-ms-FIFO starting configuration, and confirms end-to-end
connectivity before cleanup.

Every namespace has a generated `rudp...` name recorded in
`results/_work/<prefix>.topology`. Traps delete only the names listed in that
manifest. After an uncatchable interruption, inspect the manifest and remove
only its listed namespaces with `ip netns del`; do not use a broad namespace
or qdisc cleanup command. The script does not modify host qdiscs or host
sysctls.

This is a Phase 0 safety preflight, not the Phase 8 calibrated benchmark
harness. Phase 8 must add profile-specific loss seeds, capture verification,
100-probe calibration, queue/counter collection, and unprivileged endpoint
launches.
