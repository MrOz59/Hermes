# H2 paired Hermes/Hestia test

This runbook compares the installed Hermes reference with the locally built H2
candidate while keeping the same Hestia binary, pairing identity, stream
settings, application scene, and host configuration.

## Prepared artifacts

- Candidate Hermes: `build-test/sunshine`
- Hestia: `/home/ozzy/Projects/Hestia/app/moonlight`
- Host launcher: `scripts/hermes-h2-test.sh`
- Client launcher:
  `/home/ozzy/Projects/Hestia/scripts/hestia-h2-test.sh`
- Capture/gate tool: `scripts/hermes-baseline.py`

The host launcher refuses to run alongside `hermes.service`; it never stops or
restarts the installed service itself. Both launchers refuse to overwrite logs.

## Pair once

Keep the installed `hermes.service` running and start the prepared Hestia:

```bash
/home/ozzy/Projects/Hestia/scripts/hestia-h2-test.sh \
  setup h2-results/setup
```

Add/pair `localhost` in Hestia and enter the displayed PIN in the Hermes web UI
at `https://localhost:47990`. Hestia creates its normal QSettings identity
during this flow. The capture tool reads that identity in memory with
`--hestia-identity`; it does not export the private key to the result directory.

Close the setup client before collecting the first profile.

## Reference matrix

Create separate result directories:

```bash
mkdir -p h2-results/reference h2-results/candidate
```

For every profile, start Hestia in one terminal:

```bash
/home/ozzy/Projects/Hestia/scripts/hestia-h2-test.sh \
  wifi h2-results/reference
```

Start the same application/scene and allow a 60-second warm-up. For `clean`,
leave the interface untouched. For all other profiles, inspect and then apply
the matching fixture:

```bash
scripts/hermes-netem.sh dry-run wifi IFACE
sudo scripts/hermes-netem.sh apply wifi IFACE --force
```

Capture at least 60 seconds of authenticated host windows:

```bash
scripts/hermes-baseline.py capture \
  --url https://127.0.0.1:47990/api/hestia/v1/diagnostics \
  --hestia-identity \
  --profile wifi \
  --duration 60 \
  --output h2-results/reference/hermes-wifi.jsonl
```

Close Hestia after capture, then import the client trace:

```bash
scripts/hermes-baseline.py import-hestia \
  h2-results/reference/hestia-wifi.log \
  --profile wifi \
  --output h2-results/reference/hestia-wifi.jsonl
```

Always clear an applied profile before moving on:

```bash
sudo scripts/hermes-netem.sh clear IFACE
```

Repeat in this order: `clean`, `lan`, `wifi`, `wan`, `burst-loss`,
`bufferbloat`, `reordering`, then `clean` once more as a drift check.

## Candidate matrix

Stop the installed reference and start the local candidate in a dedicated
terminal:

```bash
systemctl --user stop hermes.service
scripts/hermes-h2-test.sh candidate h2-results/candidate
```

Repeat the identical matrix with outputs under `h2-results/candidate`. The
candidate uses the same configuration, TLS server identity, and pairing state
as the installed service.

After stopping the candidate with Ctrl-C, restore the installed service:

```bash
systemctl --user start hermes.service
```

## Acceptance report

Run the strict paired gate:

```bash
scripts/hermes-baseline.py gate \
  --reference h2-results/reference/hermes-*.jsonl \
  --candidate h2-results/candidate/hermes-*.jsonl \
  --client-reference h2-results/reference/hestia-*.jsonl \
  --client-candidate h2-results/candidate/hestia-*.jsonl \
  --output h2-results/h2-acceptance.md
```

The command exits nonzero for missing/short profiles or failed latency, FPS,
queue, and loss criteria. Do not apply `netem` over a remote administrative
connection unless a separate recovery path is available.
