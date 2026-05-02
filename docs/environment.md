# Phase 0 environment report

Checked on 2026-09-06 in Ubuntu 24.04 under WSL2, running Linux
5.15.167.4-microsoft-standard-WSL2. This report records the development
environment, not a claim that it is suitable for benchmark collection.

## Local development gate

| Requirement | Result |
|---|---|
| C11 compiler | `cc` (GCC 13.3.0) available |
| Make | GNU Make 4.3 available |
| Python | Python 3.12.3 available |
| NumPy | 1.26.4 available |
| matplotlib | 3.10.9 available |
| pandas | unavailable (`ModuleNotFoundError`) |
| `ip` / `tc` | iproute2 6.1.0 available |
| `ethtool` | 6.7 available |
| `tcpdump` | 4.99.4 / libpcap 1.10.4 available |
| `time` | available |
| TCP CUBIC | available (`reno cubic`) |
| TCP BBR | unavailable in `net.ipv4.tcp_available_congestion_control` |

The local build and deterministic test gates are available. Install pandas
before running plotting code. The later benchmark gate remains unavailable;
the reasons are recorded below.

## MD5 implementation choice

Phase 6 will vendor the reference C implementation from RFC 1321, Appendix A,
under its included permissive license. It is a self-contained C implementation
with no OpenSSL runtime dependency. The installed system does not provide the
OpenSSL MD5 development header, so OpenSSL is not an acceptable fallback.
The vendored source and its license text will be committed together before it
is called by production code.

## Benchmark gate

This WSL2 session has the required user-space tools but not the required
network administration privileges: creating a network namespace returned
`Permission denied`, and adding a temporary loopback netem qdisc returned
`Operation not permitted`. No host qdisc or sysctl was changed during the
check. BBR is also absent.

Use a Linux VM or bare-metal Linux host with CAP_NET_ADMIN (or equivalent
root access inside the benchmark environment), `sch_netem` with `gemodel` and
seed support, HTB, veth/network namespaces, `fq` where pacing is tested,
offload controls, packet capture permissions, and a kernel exposing both
CUBIC and BBR. The benchmark preflight script in Phase 8 must still validate
these capabilities on that host before any measurements are collected.
