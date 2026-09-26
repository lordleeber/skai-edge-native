# Step 36: systemd service

`systemd/skai-edge.service` runs the foreground executable with
`/etc/skai-edge/config.yaml`, under the dedicated `skai-edge` account. It targets
Jetson Linux with systemd 249. The service uses `video` and `render` groups for
GPU access, keeps network interfaces available, and writes persistent state in
`/var/lib/skai-edge`. systemd creates that state directory with mode 0750.

The unit uses `Type=exec`: start fails if the executable or service account cannot
be used. An active unit does not imply that RTSP/inference are ready; use
`GET /health` for application readiness. Failed processes restart after five
seconds, with at most five starts in a 60-second window. Explicit stop does not
restart the process. A stop sends SIGTERM to the service's process group and
allows 30 seconds for recorder finalization and module shutdown.
See the upstream [service documentation](https://github.com/systemd/systemd/blob/v249/man/systemd.service.xml)
and [execution settings](https://github.com/systemd/systemd/blob/v249/man/systemd.exec.xml).

## Stage a deployment

Use the [Step 37 native installation guide](step37-install.md) to build and
install the executable, UI, deployment config, SQLite and vendor unit. It covers
recursive dependency checkout, the dedicated account, file ownership and the
operator-supplied TensorRT engine. Edit the installed config for the actual RTSP
source, LAN interfaces and HTTP port before starting the service.

When WHIP is enabled, put `WHIP_TOKEN=YOUR_TOKEN` in the optional environment file
using an editor, then restrict its permissions:

```sh
sudo install -o root -g root -m 0600 /dev/null /etc/skai-edge/skai-edge.env
sudoedit /etc/skai-edge/skai-edge.env
```

Create this file once; repeating `install /dev/null` empties an existing file.
The system service manager reads it before changing to the service account.
Do not use shell `export` syntax. Leaving the file absent is supported when WHIP
is disabled. Environment and configuration files stay outside Git.

The installer places the vendor unit under `/usr/local/lib/systemd/system/`.
Verify the effective deployment and reload systemd:

```sh
sudo systemd-analyze verify /usr/local/lib/systemd/system/skai-edge.service
systemctl cat skai-edge
sudo systemctl daemon-reload
```

## Operate the service

```sh
sudo systemctl start skai-edge
sudo systemctl stop skai-edge
sudo systemctl restart skai-edge
systemctl status skai-edge
journalctl -u skai-edge
journalctl -u skai-edge -f
```

Both stdout and stderr go to journald with identifier `skai-edge`; the existing
structured timestamps, levels and modules are retained. Enable boot startup
after validating the deployment:

```sh
sudo systemctl enable skai-edge
curl --fail http://127.0.0.1:8080/health
```

Use the configured HTTP port in the health request. RTSP outages are handled by
the application reconnect loop and may leave the process active while health
is degraded. `Restart=on-failure` handles process exit failures. After correcting
repeated startup failures, clear the start limit and start again:

```sh
sudo systemctl reset-failed skai-edge
sudo systemctl start skai-edge
```

## Validation

`SystemdService.*` covers the executable/config paths, restart delay and limit,
SIGTERM/shutdown timeout, journal routing, account/state/GPU settings and boot
ordering. Verifier tests run `systemd-analyze verify` against a temporary copy
pointing at the real build executable; the installed binary path remains covered
by the contract test. Passing requires exit code zero and no warning/error
diagnostics attributed to the temporary unit or its drop-ins. Warnings from
unrelated installed units are retained in the captured output but do not fail
this unit's syntax check. Logging is forced to warning level on the console,
so inherited logging settings cannot hide diagnostics. The invocation remains
compatible with systemd 249 and does not require `--recursive-errors`.
Negative regressions inject an unknown directive and a directive in the wrong
section; both must be rejected even when the verifier returns zero. These tests
do not start or install a system service. If `systemd-analyze` is unavailable,
the three verifier tests are explicitly skipped.

```sh
ctest --test-dir build -R '^SystemdService\.' --output-on-failure
ctest --test-dir build --output-on-failure
```

On 2026-09-26, systemd 249 on the Jetson ran an isolated user service with the
real executable, configured RTSP and TensorRT engine. Only deployment identity,
state/config/executable paths and the optional environment file were overridden;
the restart, signal, timeout, journal and privilege settings came from this unit.
Start and manual restart reached a running API. Killing its main process with
SIGKILL produced `NRestarts=1` and a new PID. SIGTERM stop returned
`ExecMainStatus=0`, `Result=success`, and remained inactive after six seconds.
Journald retained the application's ready/stopped messages. The temporary unit
was removed after verification. Evidence is in ignored
`build/systemd-step36/report.json` and `journal-final.log`.

This verifies process supervision using the user manager; it does not claim that
the dedicated account or system-wide deployment was installed on this host.
Deployment identity and state paths are covered by the unit contract tests.
