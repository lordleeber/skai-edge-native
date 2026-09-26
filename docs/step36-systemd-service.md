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

Step 37 adds `cmake --install` and packaging. For this step, manually stage an
already built executable and its runtime dependencies on the target Jetson.
Run these commands from the repository root. Create the account once:

```sh
sudo useradd --system --user-group --home-dir /var/lib/skai-edge \
  --no-create-home --shell /usr/sbin/nologin skai-edge
getent group video render
sudo install -d -o root -g skai-edge -m 0750 /etc/skai-edge
sudo install -d -o skai-edge -g skai-edge -m 0750 \
  /var/lib/skai-edge /var/lib/skai-edge/models
sudo install -m 0755 build/skai-edge /usr/local/bin/skai-edge
sudo install -o root -g skai-edge -m 0640 \
  config/config.example.yaml /etc/skai-edge/config.yaml
sudo install -d -m 0755 /usr/local/share/skai-edge
sudo cp -R web /usr/local/share/skai-edge/
sudo install -o root -g skai-edge -m 0640 /path/to/yolo.engine \
  /var/lib/skai-edge/models/yolo.engine
sudoedit /etc/skai-edge/config.yaml
```

Set the actual RTSP URL, LAN interface names and these deployment paths in YAML:

```yaml
detector:
  engine: /var/lib/skai-edge/models/yolo.engine
web:
  root: /usr/local/share/skai-edge/web
storage:
  database_path: /var/lib/skai-edge/skai-edge.db
  alert_directory: /var/lib/skai-edge/alerts
recording:
  directory: /var/lib/skai-edge/recordings
```

These snippets identify fields to edit in the full configuration. Keep the
remaining detector, web and recording settings. Select an available HTTP port.
The service's working directory is `/var/lib/skai-edge`; explicit paths avoid
depending on the source checkout or a login shell's current directory.

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

Install the unit and reload systemd:

```sh
sudo install -m 0644 systemd/skai-edge.service /etc/systemd/system/skai-edge.service
sudo systemd-analyze verify /etc/systemd/system/skai-edge.service
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
ordering. A seventh test runs `systemd-analyze verify` against a temporary copy
pointing at the real build executable; the installed binary path remains covered
by the contract test. It does not start or install a system service. If
`systemd-analyze` is unavailable, only that syntax test is explicitly skipped.

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
