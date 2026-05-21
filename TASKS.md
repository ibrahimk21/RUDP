# Execution plan

[PLAN.md](PLAN.md) defines scope; [BENCHMARKING.md](BENCHMARKING.md) defines experiments. Implement phases in order. Run the entire accumulated `make test` suite at every checkpoint; after Phase 2 also run `make integration`, and after Phase 1 run `make sanitize`. Tests must have watchdogs and return nonzero on failure. Performance targets never gate correctness or project completion.

## Protocol contract (version 1)

These rules are normative starting decisions. Record implementation details and any justified contract revisions in `docs/design.md` before coding the affected feature; revise this contract and its tests together.

### Wire format and validation

Use explicit byte serialization, not a packed C struct as the wire representation. All multibyte integers are unsigned, big-endian. Fixed header is 36 bytes:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type: SYN=1, SYN_ACK=2, OPEN=3, OPEN_ACK=4, DATA=5, ACK=6, PROBE=7, FIN=8, FIN_ACK=9, ABORT=10 |
| 2 | 1 | sack_count, 0..4; nonzero only for ACK |
| 3 | 1 | reserved = 0 |
| 4 | 8 | client nonce |
| 12 | 8 | server nonce (zero only in SYN) |
| 20 | 4 | seq, DATA packet index; FIN/FIN_ACK carry final next-sequence |
| 24 | 4 | ack, next contiguous DATA sequence expected |
| 28 | 4 | receive_limit, exclusive sequence right edge |
| 32 | 2 | payload length |
| 34 | 2 | Internet checksum |

SACK blocks follow the header, each two uint32 values `[start,end)`; payload follows blocks. Maximum DATA payload is 1024 bytes. Controls have their exact type-specific length below; reject trailing bytes. ACK/PROBE/OPEN/OPEN_ACK have no payload; ABORT carries a uint32 error code. SYN/SYN_ACK carry identical metadata: uint64 declared length followed by 16-byte MD5. File mode has a known length; benchmark streaming mode uses UINT64_MAX and zero digest, with final length/digest in FIN. FIN/FIN_ACK carry uint64 actual length and 16-byte digest. Only DATA/FIN/FIN_ACK set seq; only receiver ACK/OPEN_ACK/FIN_ACK set ack/receive_limit (OPEN_ACK advertises initial credit W). Other seq/ack/limit fields are zero. Only ACK carries SACK information. Validate nonce fields according to setup state before requiring an established nonce pair.

Checksum covers the complete serialized header, SACK bytes, and payload with checksum bytes zeroed for calculation; odd length is padded by a virtual zero byte. Fold carries and complement the 16-bit one's-complement sum; verify the full received sum. It complements the enabled UDP checksum. No pseudoheader is added to this application checksum.

Before touching state: check datagram truncation (`recvmsg`/MSG_TRUNC), exact lengths, version/type/reserved/count, checksum, peer address/port and nonce pair, valid state transition, and sequence/ACK/SACK bounds. Reject malformed, wrong-session and impossible future acknowledgments without allocating from their fields or replying. Cap allocations locally. Cast-free byte decoding avoids alignment/aliasing problems. ACK/SACK can only acknowledge DATA already sent; ACKs must not regress sender state. Corruption is dropped and recovered as loss. Fuzz the decoder; test byte order with independent expected byte vectors.

### Session, framing, and completion

One active transfer per receiver process. Sender chooses a fresh random 64-bit client nonce with Linux `getrandom`; receiver creates an independent random server nonce. Fail if secure OS randomness is unavailable. The pair identifies the session; neither is authentication.

States: sender CLOSED -> SYN_SENT -> OPEN_SENT -> ESTABLISHED -> FIN_WAIT -> CLOSED; receiver LISTEN -> PENDING -> ESTABLISHED -> LINGER -> CLOSED. Any active state can fail. SYN contains metadata; SYN_ACK echoes it and supplies the server nonce; OPEN confirms the pair; OPEN_ACK permits DATA. Repeated controls in the same session receive the same response without resetting progress timers or reallocating buffers. A duplicate SYN for a pending/active client nonce reuses its server nonce. Ignore other peers/sessions while busy. Receiver restart generates a new server nonce, so old OPEN/DATA cannot establish a new session. Pending state expires after 30 seconds.

Only DATA consumes sequence space, starting at zero. ACK k means every DATA sequence before k was accepted contiguously: after DATA N, the next-expected ACK is N+1. ACK-only packets never solicit ACKs. DATA payloads are 1024 bytes except the final one (1..1024); empty transfers contain no DATA. Preserve the final short packet length and reject subsequent DATA beyond it. Default maximum transfer length is 16 GiB, enforced for streaming too; reject oversized known metadata at setup.

Send FIN only after every DATA packet is cumulatively acknowledged. It declares final next-sequence, actual byte length, and MD5. Receiver accepts it only after all data is contiguous, length/digest agree (including setup metadata in file mode), and output checks succeed. Receiver then commits output and sends FIN_ACK echoing final values. Duplicate FIN receives the same FIN_ACK during a 35-second LINGER. Sender success requires a matching FIN_ACK; if confirmation is lost until deadline, report completion-unknown rather than claiming rollback or success. Lost final DATA/ACK uses the DATA timer; lost FIN/FIN_ACK uses control retries.

Control retries start at 1 second, double up to 4 seconds, and stop after 30 seconds per setup or closing exchange. DATA no-progress deadline is 120 seconds, reset only by new DATA acceptance (receiver) or new cumulative/SACK acknowledgment (sender), not duplicate traffic/probes. Overall transfer deadline defaults to 1800 seconds and is configurable; deadlines are monotonic and independent of RTO. A stalled application does not extend them automatically. Send best-effort ABORT on local failure after establishment; accept ABORT only for the valid peer/session. Local errors and peer disappearance release resources and produce distinct error codes. Restart/resume requires a fresh transfer.

CLI writes a temporary sibling output, checks short writes/disk errors, flushes/closes successfully, then commits on verified FIN with an atomic no-replace operation (Linux renameat2/RENAME_NOREPLACE or equivalent checked link/unlink). Fail if the destination already exists, including a concurrent creation; do not overwrite it. Remove temporary output on handled failure; document recovery/removal of leftovers after process crashes. Durable survival across power loss is outside scope. Prehash an unchanged source file, check read errors, and detect a changed length/digest at completion.

### Sequence space, windows, and SACK

Use modulo-2^32 serial comparisons: a is after b only when `0 < (a-b mod 2^32) < 2^31`; equality and the ambiguous half-range have separate handling. Every live window is below 2^31. Validate distances before indexing. Tests initialize internal state near wrap; never rely on ordinary unsigned `<` ordering across wrap.

Receive capacity W = 8192 DATA slots (8 MiB payload plus bounded metadata); sender retains at most W slots. Separate `expected` (next contiguous receipt) from `consumed` (next packet not yet released by the application). Advertise `receive_limit = consumed + W`, exclusive. Accept DATA only in the reserved window and retain SACKed data until application consumption; no SACK reneging. Advancing `expected` alone does not free capacity. Right edges only advance within a session; stale smaller advertisements never close a previously granted window. Verify newly advertised credit is no more than W beyond that packet's valid cumulative ACK.

Sender may introduce DATA only below receive_limit, with retained sequence span below W and estimated unsacked flight below floor(cwnd). Reserve room for missing packets; do not equate SACK-freed flight with free ring slots. Keep payloads until cumulative ACK releases them. Retransmissions remain permitted for previously granted sequences even when new-data credit is zero. On application consumption, send a window-update ACK; sender PROBEs at 1, 2, then 4-second intervals while credit-blocked. Receiver answers a valid PROBE with current ACK/limit/SACK. Probes neither consume DATA sequence nor evade failure deadlines.

ACK immediately on every valid DATA reception, including duplicate/out-of-order data, and on credit reopening. Ignore far-future DATA; re-ACK old DATA. SACK ranges are half-open, nonempty, merged, ordered by distance from cumulative ack, start strictly after that ack (the missing packet), and lie wholly within sent/receive bounds. Send the lowest four disjoint ranges if more exist; omitted ranges remain retained. Sender maintains the union of observed SACK information until cumulative ACK, never treating omission as loss or counting repeated ACKs as new delivery.

Declare an unsacked DATA packet lost for fast recovery after at least three distinct higher DATA packets have been acknowledged. Track evidence per transmission: after retransmitting, the same evidence cannot trigger another fast retransmission; require three newly acknowledged higher packets since that retransmission. Otherwise use RTO, including tail loss, retransmission loss and small windows. Prioritize retransmissions over new DATA, honor pacing, and record loss/retransmission events once per transmission. Multiple holes must recover without repeated ACK-driven storms.

### Timers and congestion-control contract

Use a nonblocking socket with `poll` and one monotonic scheduler; handle EINTR/EAGAIN and partial application I/O explicitly. Linux IPv4/MTU >=1500 is required; fail clearly on EMSGSIZE rather than fragmenting application packets. Resolve IPv4 names with `getaddrinfo`; broader portability is optional.

Initial DATA RTO = 1000 ms. Using floating-point milliseconds initially, first sample gives SRTT=R, RTTVAR=R/2. Later updates use the old SRTT: `RTTVAR = 0.75*RTTVAR + 0.25*fabs(SRTT-R)`, then `SRTT = 0.875*SRTT + 0.125*R`. RTO is `clamp(SRTT + max(1 ms, 4*RTTVAR), 100 ms, 60000 ms)`. The 100-ms floor is a deliberate prototype parameter, not TCP equivalence.

At most one RTT sample per advancing cumulative ACK, taken from its highest newly acknowledged packet only if never retransmitted; suppress the sample if that ACK advance covers any retransmitted packet. No SACK-only RTT samples in v1. Start one timer when the first outstanding DATA is sent; on advancing cumulative ACK restart it for remaining unsacked DATA. Duplicate/SACK-only ACKs do not postpone it. Stop when none remains. On expiry retransmit the lowest unsacked packet, double backed-off RTO up to 60 seconds, and restart. A clean RTT sample restores the computed RTO. Fake-clock tests cover restart, backoff, ACK loss and all-SACKed states.

CC interface is present with a fixed-window test implementation before Phase 4. It receives unique original-send events, newly acknowledged DATA count, first inferred-loss events, RTO events, flight size, monotonic time and recovery boundary; returns cwnd in packets. All accounting uses packets, including a short final DATA packet. ACK growth occurs once per newly acknowledged packet across cumulative ACK and SACK; retransmissions/duplicate ACKs never create extra credit.

ACK processing order: validate; snapshot pre-ACK unsacked flight; update cumulative/SACK delivery and RTT; exit completed recovery; apply eligible ACK growth; infer new losses/update density and apply reductions; schedule sends. A timeout snapshots current unsacked flight before retransmitting. Flight is sent DATA not cumulatively/SACK acknowledged, with retransmissions counted once. Fast retransmissions may proceed despite flight exceeding a reduced cwnd, but only once per fresh loss evidence and under pacing; new DATA remains cwnd-limited. This deliberately conservative flight approximation differs from TCP's recovery algorithms. Test this event order explicitly.

RUDP-AIMD: initial cwnd=10, ssthresh=W; cwnd is floating point clamped to [1,W]. Outside recovery, for each newly acknowledged packet increment cwnd by 1 in slow start, else 1/cwnd. First fast loss in an episode sets ssthresh=max(flight/2,2), cwnd=ssthresh; suppress growth and further fast-loss reductions until cumulative ACK passes the episode's highest sent DATA sequence. Each RTO sets ssthresh=max(flight/2,2), cwnd=1 and starts/restarts that recovery boundary. No TCP Reno fast-recovery inflation is claimed.

RUDP-Sat uses the same growth and timeout response, but sparse fast loss does not cut cwnd. Keep a 50-entry ring of original DATA sends, each marked at most once on its first inferred loss; density = marked/occupied entries (evaluate only when all 50 entries exist). Retransmissions do not enter the ring; an evicted sequence cannot mark it. Recompute on insertion/marking; apply one AIMD fast-loss reduction when density >10% outside an existing reduction episode, including when insertion changes the ring. Exit/suppress growth during a reduction episode exactly as AIMD. This is a bounded recent-send heuristic, potentially blind to old losses, not a corruption classifier; test that limitation and report it.

Both algorithms pace DATA including retransmissions at `1000*cwnd*1024/max(SRTT_ms,1)` payload bytes/s (use 1000 ms before sampling), with a two-datagram burst allowance. Update flight conservatively after timeout and do not dump a full window. Control traffic is outside cwnd but duplicate-triggered ACKs are capped at one per millisecond per session. No-loss ACKs for new DATA are not subject to this cap. Pre-CC implementations run only in local correctness tests.

## Phase 0 — Environment and skeleton

- [x] Inspect existing setup; initialize Git only if absent. Create the PLAN.md layout, Make targets `all`, `lib`, `cli`, `tcp_ref`, `udp_ref`, `test`, `integration`, `sanitize`, `clean`, and build-only ignores.
- [x] Confirm Linux C11 toolchain, Make, Python with matplotlib/pandas/numpy, and a licensed self-contained MD5 source. Benchmark dependencies: iproute2/tc, network namespaces/veth, netem including gemodel/seed, HTB, ethtool, tcpdump, time, and CUBIC/BBR availability. Record versions.
- [x] Preflight the isolated topology from BENCHMARKING.md including privileges, pacing/offload controls and cleanup. Never alter the host's default qdisc or sysctls.
- [x] Record `correctness_ready` and `benchmark_ready` separately. If WSL2 lacks required kernel facilities, continue deterministic/local work and document a compatible Linux VM/host; do not substitute a UDP proxy for TCP comparisons.

**Checkpoint:** stubs build; dependency report states which later gates are available. Missing benchmark facilities block Phase 8 experiments, not Phases 1-7 local work.

## Phase 1 — Codec

- [x] Implement header/control payload codecs and checksum from the contract, bounded parsing, error codes, and independent golden vectors.
- [x] Test all types, 0/1/4 SACK blocks, malformed lengths/counts/ranges, endian/alignment cases, odd checksums, corruption, and bounded randomized parser inputs under ASan/UBSan.

**Checkpoint:** byte-level vectors and sanitizer tests pass; malformed inputs cannot mutate transport state.

## Phase 2 — Session plumbing

- [x] Implement socket wrappers, injected clock/randomness/I/O interfaces, peer binding and idempotent setup/abort states. Start `docs/design.md` with state transitions and public API ownership/backpressure/error semantics.
- [x] Test duplicate/lost setup messages, timeout, busy receiver, wrong peer/session, receiver restart and syscall failures. Add bounded live loopback setup tests.

**Checkpoint:** setup succeeds or fails within its deadline; stale sessions cannot deliver data.

## Phase 3 — Bounded stop-and-wait

- [x] Implement cumulative ACK=N+1, retained retransmit data, fixed 1-second DATA timer, deadlines, framing, FIN/FIN_ACK and linger with an in-memory sink.
- [x] Build a fake-clock packet scheduler with scripted events and independent seeded directional PRNGs (specified algorithm, not platform `rand`). Tests select loss/delay/duplicate/reorder/corruption by type, sequence and transmission occurrence.
- [x] Test DATA/ACK/FIN/FIN_ACK loss, duplicates, empty/final-short payloads, peer death, completion-unknown, and permanent-loss bounded failure.

**Checkpoint:** exactly-once ordered delivery and bounded termination; no wall-clock sleeping in unit tests.

## Phase 4 — Windows, flow control and SACK

- [x] Implement bounded rings, consumed-versus-received tracking, serial arithmetic, zero-credit PROBEs, SACK scoreboard and recovery suppression; wire the fixed-window CC interface.
- [x] Test wrap, invalid future ACKs, stale credit, slow/stopped consumers, lost updates, >4 holes, reordered/duplicated ACKs, retransmission loss and tail loss. Assert memory caps and no repeated fast retransmit from unchanged evidence.

**Checkpoint:** interior loss with sufficient evidence uses fast recovery; every other loss has timer/deadline coverage without overflow or deadlock.

## Phase 5 — Adaptive timers

- [x] Implement the estimator/timer contract and counters for clean samples, suppressed samples, fast retransmits and timeouts.
- [x] Test synthetic traces against independently computed values. Live integration: 50-ms delay in each direction predicts about 100-ms RTT, not 50 ms; measure uninjected baseline first and use a documented scheduler tolerance (initially 30 ms).

**Checkpoint:** fake-clock timer assertions pass exactly; bounded live tests either pass tolerances or report measured environment failure, never silently change expected values.

## Phase 6 — CLI and correctness milestone

- [x] Implement file CLI, MD5 standard vectors, source stability checks, output commit/failure handling, limits, and machine-readable status/counters. No benchmark speed claim yet.
- [x] Matrix: all three profile RTT/burst models from BENCHMARKING.md; random loss {0,1,5,10,25,50,75,100}% as an explicitly separate override of each profile's loss model; modes {none,reorder,duplicate,combined}; payload lengths {0,1,1023,1024,1025,64 KiB,1 MiB}. Reorder mode delays every seventh eligible datagram by two profile RTTs; duplicate mode copies every eleventh once after one profile one-way delay. Apply independently in each direction, numbering original datagrams before impairment; duplicates do not recursively duplicate. Use one fixed seed per case initially and retain exact event traces on failure.
- [x] Define the expected outcome of each seed/case before running it: scripted recovery traces must succeed; 100% loss must time out. Finite high-loss cases may specify success or bounded failure based on a checked delivery schedule, never require probabilistic eventual success. Every success must match bytes and MD5; every failure must preserve output/error guarantees. Keep native burst-profile cases in addition to random-loss overrides.
- [x] Add targeted corruption/control-loss/restart/disk-full/short-I/O tests not covered by the matrix; print actual case counts. Large transfers and wrap use synthetic state where practical. Live tests cover all profiles when benchmark facilities exist, otherwise mark those integration cases unavailable explicitly.

**Checkpoint:** complete accumulated suite and available integration tests pass; CLI transfer and failure demo are repeatable. Unavailable profile integration remains a tracked prerequisite for Phase 8.

## Phase 7 — CC and reference tools

- [x] Implement RUDP-AIMD and shared pacing. Test ACK accounting, loss episodes, cwnd floors, RTO restart, small windows and idle/credit-limited behavior. After an idle period >=RTO cap cwnd at 10 before resuming; retain ssthresh.
- [x] Build TCP tools using the same file/status semantics, checked partial stream I/O, and benchmark record codec. Select CUBIC/BBR per socket and verify with getsockopt; fail a requested unavailable algorithm. Record TCP_INFO and actual socket buffer sizes.
- [x] Build paced raw-UDP reference with record IDs, payload validation, receiver unique-byte accounting and bounded completion through the harness control channel; it performs no retransmissions.
- [x] Add the timed benchmark stream mode, common monotonic record timestamps and process CPU timers defined in BENCHMARKING.md. FIN still validates final bytes/digest. Verify TCP and RUDP file outputs and benchmark generators independently.

**Checkpoint:** AIMD passes all reliability tests; both TCP algorithms pass where available; raw UDP correctly reports deliberate loss/duplicates. Any unavailable BBR gate stays open, never silently uses another CC.

## Phase 8 — Validated benchmark harness

- [x] Implement the exact BENCHMARKING.md configuration, disposable topology, validation, randomized schedule, timeouts, manifests/raw/sample schemas, CPU/counter capture and plotting with confidence intervals.
- [x] Implement latency/setup, controlled loss, and simultaneous-flow fairness workloads, including congestion-only and capacity-step cases. Validate metric calculations with small known traces; failed runs remain visible.
- [ ] Check baseline throughput/delay/loss/burst distributions, TCP segmentation and offloads, queue occupancy/drops, and cleanup on failure before collecting results.

**Checkpoint:** all dependencies and profile integration tests pass; each workload yields auditable artifacts and correct graphs, including an intentionally failed run. Smoke runs do not count as experimental repetitions.

## Phase 9 — Baseline data

- [ ] Freeze revision/configuration; run RUDP-AIMD, TCP-CUBIC, TCP-BBR and raw UDP under BENCHMARKING.md, including fixed-RTT sweeps and coexistence controls.
- [ ] Preserve raw/sample/manifests and report all required metrics, uncertainty, failures and overhead. Investigate configuration errors without requiring theory agreement.

**Checkpoint:** complete versioned baseline artifacts reproduce tables/graphs; unfavorable or inconclusive results are acceptable.

## Phase 10 — RUDP-Sat evaluation

- [ ] Implement the exact heuristic above; test sparse loss, >10% density, ring eviction, retransmitted/duplicate signals, threshold crossing during recovery and RTO response.
- [ ] Use separately labeled pilot data for any tuning. Freeze parameters before final experiments; rerun AIMD and both TCP baselines alongside Sat in the same randomized blocks, not against old-session numbers alone.
- [ ] Run every mandatory workload, especially congestion-only/capacity-step coexistence. Report target attainment, failure rate, queue delay and TCP harm; restrict the heuristic to lab use even if solo throughput improves.

**Checkpoint:** reliability still passes and complete comparative artifacts exist. A harmful heuristic is a documented negative result, not a reason to omit fairness data.

## Phase 11 — Reproduction and report

- [ ] Finish design/API/state/CC notes; document deviations from TCP and links to primary references. Count source lines by core/apps/tests separately with a recorded command, no size cap.
- [ ] Report metrics and target outcomes with uncertainty, all failures, configurations, overhead, coexistence, and BENCHMARKING.md threats to validity.
- [ ] README: build/test/CLI commands, prerequisites, privileges/cleanup, limitations, exact experiment and plot-reproduction commands.
- [ ] Perform a clean-checkout build/test and regenerate every graph from committed artifacts; rehearse one representative run on the documented benchmark environment.

**Checkpoint:** PLAN.md deliverables are traceable to tests/results. Optional optimizations use Phase 9/10 CPU and throughput data as their baseline; no implementation of stretch goals is required.
