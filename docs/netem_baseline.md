# H0 network-emulation baseline

`scripts/hermes-netem.sh` provides versioned, deterministic egress network
profiles for the Hermes H0 baseline. It uses Linux `tc netem` and fixed random
seeds so repeated runs exercise the same configured impairment model.

The script does not alter networking for `list`, `describe`, `show`, or
`dry-run`. Applying a profile requires root, an explicit non-loopback interface,
and `--force` because the operation replaces that interface's root qdisc.

## Profiles

| Profile | Added one-way delay | Other pressure |
| --- | ---: | --- |
| `lan` | 1 ms | 0.2 ms correlated jitter |
| `wifi` | 8 ms | long-tail jitter, 0.5% loss, short transmit slots |
| `wan` | 35 ms | jitter, 0.3% loss, 100 Mbit/s rate |
| `burst-loss` | 20 ms | Gilbert-Elliott burst loss |
| `bufferbloat` | 25 ms | 20 Mbit/s rate and a 4,000-packet queue |
| `reordering` | 15 ms | 5% reordering pressure |

These values are baseline fixtures, not claims that every network of a given
name behaves this way. Changes to them alter benchmark comparability and should
be reviewed like changes to benchmark code.

## Safe workflow

Find the egress interface used to reach the client, then inspect the exact
command without changing the host:

```bash
scripts/hermes-netem.sh dry-run wifi enp5s0
```

Start a stream and record at least 60 seconds of the per-session diagnostics
before applying impairment. Then apply exactly one profile:

```bash
sudo scripts/hermes-netem.sh apply wifi enp5s0 --force
scripts/hermes-netem.sh show enp5s0
```

After the measurement window, remove only the qdisc marked by this harness:

```bash
sudo scripts/hermes-netem.sh clear enp5s0
```

`clear` refuses to operate without the marker written by `apply`. It removes
the Hermes root qdisc only when the current root is still `netem`, then lets
Linux restore the interface's default qdisc. The marker is created atomically
before `tc` runs and is rolled back if `tc` fails; a second `apply` is refused.
If an administrator or network manager replaced the qdisc after `apply`,
`clear` leaves both the unrelated qdisc and marker untouched. Discard only that
stale marker with:

```bash
sudo scripts/hermes-netem.sh forget enp5s0 --force
```

The harness cannot reconstruct a custom root qdisc that existed before the
test. `show` and the state marker retain its textual preflight snapshot for
diagnosis, but capture and restore a custom configuration separately before
using `--force`.

## Measurement order

Use the same resolution, frame rate, encoder, bitrate target, application scene,
and client presentation settings for every run:

1. no netem, 60-second warm-up plus 60-second sample;
2. `lan`;
3. `wifi`;
4. `wan`;
5. `burst-loss`;
6. `bufferbloat`;
7. `reordering`;
8. no netem again to detect thermal or background-load drift.

Record the Hermes per-session p50/p95/p99 fields, wire bitrate, FEC overhead,
dropped frames, and the corresponding Hestia reassembly/decode/pacer/render
percentiles. Root netem affects Hermes egress only. For a symmetric path, mirror
the profile on the client's egress interface or run both endpoints behind a
dedicated network-emulation bridge.

## Capturing and reporting

`scripts/hermes-baseline.py` polls the certificate-authenticated Hestia
diagnostics endpoint. It uses the monotonic encode/network window sequences to
write each published state only once:

```bash
scripts/hermes-baseline.py capture \
  --url https://hermes-host:47990/api/hestia/v1/diagnostics \
  --cert client.pem \
  --key client-key.pem \
  --ca hermes-ca.pem \
  --profile wifi \
  --duration 60 \
  --output baseline-wifi.jsonl
```

Use `--insecure` only for an isolated test host whose certificate cannot be
validated. It disables server identity verification but does not replace the
paired client certificate.

For a local Hestia build that has already been paired, the harness can load its
QSettings certificate and key in memory instead of exporting them:

```bash
scripts/hermes-baseline.py capture \
  --url https://127.0.0.1:47990/api/hestia/v1/diagnostics \
  --hestia-identity \
  --profile wifi \
  --duration 60 \
  --output baseline-wifi.jsonl
```

The normal local Hermes CA at
`~/.config/sunshine/credentials/cacert.pem` is selected automatically when it
exists. `--hestia-settings PATH` supports a portable/explicit Hestia INI file.
Temporary PEM files are mode `0600` and removed immediately after the Python
TLS context loads them.

Generate a Markdown comparison from one or more runs:

```bash
scripts/hermes-baseline.py report \
  baseline-clean.jsonl \
  baseline-wifi.jsonl \
  baseline-wan.jsonl \
  --output baseline-report.md
```

The report deduplicates windows by run ID and sequence. Its percentile columns
are frame-count-weighted means of published one-second percentiles; because raw
frame samples never leave the bounded in-process collector, the report labels
these values explicitly and does not misrepresent them as reconstructed global
percentiles.

## Paired H2 acceptance gate

The H2 gate needs both sides of the real stream. Start Hestia with terminal
frame tracing enabled and retain its process log separately for every profile
and build:

```bash
HESTIA_FRAME_TRACE=1 hestia 2>&1 | tee hestia-candidate-wifi.log
```

Run one stream per log. Import its structured records and attach the same
profile label used by the Hermes capture:

```bash
scripts/hermes-baseline.py import-hestia \
  hestia-candidate-wifi.log \
  --profile wifi \
  --output candidate-hestia-wifi.jsonl
```

After collecting `clean`, `lan`, `wifi`, `wan`, `burst-loss`, `bufferbloat`,
and `reordering` for both the reference and candidate builds, run:

```bash
scripts/hermes-baseline.py gate \
  --reference reference-hermes-*.jsonl \
  --candidate candidate-hermes-*.jsonl \
  --client-reference reference-hestia-*.jsonl \
  --client-candidate candidate-hestia-*.jsonl \
  --output h2-acceptance.md
```

The default gate requires at least 30 encode/network windows and 300 presented
Hestia frames on each side of every profile. For `clean` and `lan`, host and
client p95/p99 may regress by at most 2 ms, host FPS by at most 2%, and client
frame-ID loss by at most 0.5 percentage points. Under constrained profiles,
Hermes send-queue p95/p99 and Hestia receive-to-present p95/p99 must improve by
at least 1%; the worst published host send-queue p99 must stay within the 100 ms
maximum queue budget, and inferred client loss must remain at or below 10%.
Packet-deadline behavior after dequeue remains covered by the deterministic
fake-clock tests because `capture_to_last_send` also contains capture and encode
time and therefore cannot correctly represent that deadline. All limits have
explicit CLI overrides so a changed benchmark contract is visible in the
command and generated report.

Hestia percentiles are computed directly from terminal per-frame durations
using nearest rank. Hermes percentiles remain the frame-count-weighted means
of its bounded one-second windows. Hestia `receive_to_present` is a local
receiver-to-display measurement; it does not pretend that the independent
Hermes and Hestia monotonic clocks share an epoch. Gaps in frame IDs estimate
network loss, so an incomplete client log invalidates that evidence.

## Header-only pcap

`scripts/hermes-pcap.sh` captures the video, control, and audio UDP port range
derived from the configured base port. It accepts only Ethernet-style
interfaces and limits each captured packet to 42 bytes: exactly the minimum
Ethernet, IPv4, and UDP headers. IPv4 options, VLAN tags, and IPv6 can only make
the stored packet end earlier relative to the transport payload. Pcap timestamps
and the original on-wire packet length remain available, while media and
decrypted application payload are not recorded.

Inspect the command first:

```bash
scripts/hermes-pcap.sh dry-run enp5s0 baseline-wifi.pcap \
  --duration 60 \
  --base-port 47989 \
  --client 192.0.2.10
```

Then run the same capture with sufficient packet-capture permission:

```bash
sudo scripts/hermes-pcap.sh capture enp5s0 baseline-wifi.pcap \
  --duration 60 \
  --base-port 47989 \
  --client 192.0.2.10
```

The script refuses loopback capture and existing output files. Pcap metadata
still contains client/host addresses, ports, sizes, and timings, so treat it as
diagnostic data even though application payload is truncated.
