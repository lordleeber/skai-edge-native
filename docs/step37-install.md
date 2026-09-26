# Step 37: native install

`cmake --install` installs the executable, public UI, systemd unit, default
deployment configuration and an empty SQLite database built using production
migrations. The native Jetson build does not require GoogleTest, Python, Node or
the test RTSP-server development package when `BUILD_TESTING=OFF`.

## Dependencies and build

Use the compiler/CMake, GStreamer, OpenCV, yaml-cpp, Boost, SQLite, CURL, CUDA and
TensorRT dependencies described in the repository build instructions. Install
the corresponding shared runtime libraries and GStreamer plugins on the target
Jetson; the installer does not bundle OS libraries, drivers or TensorRT engines.
CUDA library search paths needed by the linked executable are retained in its
install RPATH. Build natively on the target Jetson, or provide a working target
emulator for the build-only database seed executable when cross-compiling.

The exact gitlink revisions are also shipped in
`/usr/local/share/skai-edge/dependency-revisions.txt`:

| Dependency | Pinned revision |
| --- | --- |
| `lordleeber/skai-ice` | `ad6574d97fa62f0aa71cb1138e3e2974abf19dcb` |
| `paullouisageneau/libdatachannel` v0.22.6 | `0d6adc021953d7263fd4503482ea7bde33553724` |

With Git credentials that can read the private `skai-ice` repository:

```sh
git clone --recurse-submodules https://github.com/lordleeber/skai-edge-native.git
cd skai-edge-native
git submodule update --init --recursive
```

Alternatively, clone without recursion and run the authenticated bootstrap. It
recursively initializes both dependencies and verifies their checked-out SHAs
against the gitlinks:

```sh
SKAI_GITHUB_TOKEN=... ./scripts/bootstrap_dependencies.sh
```

Keep the private token in your credential/secret store. Build the production
executable and its empty database seed:

```sh
cmake -S . -B build/production -DBUILD_TESTING=OFF -DSKAI_ENABLE_TENSORRT=ON
cmake --build build/production --target skai-edge -j2
```

## Install layout

| Path | Behavior |
| --- | --- |
| `/usr/local/bin/skai-edge` | Installed executable |
| `/usr/local/share/skai-edge/web/` | Public HTML, JavaScript and CSS assets |
| `/usr/local/share/skai-edge/dependency-revisions.txt` | Full dependency SHAs |
| `/usr/local/lib/systemd/system/skai-edge.service` | Vendor unit, separate from local overrides |
| `/etc/skai-edge/config.yaml` | Seeded only if absent; mode 0640 |
| `/var/lib/skai-edge/skai-edge.db` | Valid empty schema seeded only if absent; mode 0640 |
| `/var/lib/skai-edge/recordings/` | Persistent MP4 directory |
| `/var/lib/skai-edge/alerts/` | Persistent snapshot directory |
| `/var/lib/skai-edge/models/` | Directory for an operator-supplied TensorRT engine |

New configuration/state directories have mode 0750. Reinstalling updates the
executable, UI, dependency manifest and vendor unit. Existing config, SQLite,
media, symlinks and directory permissions are preserved. Runtime database
migrations remain responsible for upgrading an existing schema at startup.
No real camera data, local configuration, model or WHIP token is packaged.

The default prefix is `/usr/local`. To change it, configure
`-DCMAKE_INSTALL_PREFIX=/your/prefix` before building. The generated YAML and unit
use that prefix; `/etc/skai-edge` and `/var/lib/skai-edge` stay system paths.
A mismatched install-time `--prefix` is rejected before copying files. A unit
under a nonstandard prefix must be linked into systemd's load path using
`sudo systemctl link /your/prefix/lib/systemd/system/skai-edge.service`.

## First deployment

Create the service account once, then install and set its access to config and
state. These commands require administrative access:

```sh
sudo useradd --system --user-group --home-dir /var/lib/skai-edge \
  --no-create-home --shell /usr/sbin/nologin skai-edge
getent group video render
sudo cmake --install build/production
sudo chgrp skai-edge /etc/skai-edge /etc/skai-edge/config.yaml
sudo chown -R skai-edge:skai-edge /var/lib/skai-edge
sudo install -o root -g skai-edge -m 0640 /path/to/yolo.engine \
  /var/lib/skai-edge/models/yolo11s.engine
sudoedit /etc/skai-edge/config.yaml
```

Set the real RTSP URL, credentials if needed, actual LAN interface names, alert
rules and an available HTTP port. The generated config already uses absolute
web, engine, storage and recording paths. WHIP defaults to disabled; its optional
root-owned environment file is documented in the
[systemd guide](step36-systemd-service.md).

```sh
sudo systemctl daemon-reload
sudo systemctl start skai-edge
systemctl status skai-edge
journalctl -u skai-edge
curl --fail http://127.0.0.1:8080/health
sudo systemctl enable skai-edge
```

Use the actual configured HTTP port. Check readiness before enabling boot
startup. Local overrides in `/etc/systemd/system/` take priority over the vendor
unit; `systemctl cat skai-edge` shows the effective unit and drop-ins.

For an upgrade, build the updated executable, then install it while stopped:

```sh
sudo systemctl stop skai-edge
sudo cmake --install build/production
sudo systemctl daemon-reload
sudo systemctl start skai-edge
```

The installer does not start services or create host
accounts. Packaging as `.deb` is deferred until repeated deployment needs it.

## Staging and regression tests

Stage the same layout without writing to the host's `/etc`, `/usr/local` or
`/var/lib`:

```sh
DESTDIR="$PWD/build/install-stage" cmake --install build/production
build/install-stage/usr/local/bin/skai-edge --version
```

`DESTDIR` is a staging root, not a chroot: generated paths still name their final
target locations. This follows [CMake's DESTDIR convention](https://cmake.org/cmake/help/v3.22/envvar/DESTDIR.html).
Regression fixtures map those paths into an isolated root for runtime checks.

```sh
ctest --test-dir build -L install --output-on-failure
ctest --test-dir build --output-on-failure
```

The suite covers layout, valid empty schema, absolute YAML paths, permissions,
reinstall preservation, symlinks, runtime-only artifacts, exact pins and prefix
validation. It also starts the installed executable from an unrelated working
directory without `LD_LIBRARY_PATH`, receives RTSP frames, serves every installed
UI asset, reopens SQLite and stops cleanly. The TensorRT version is labeled
`install-jetson`; the TensorRT OFF version exercises the portable runtime.
Tests use temporary DESTDIRs and serialize installation to avoid racing CMake's
shared install manifest. They never install or start a system-wide service.

On 2026-09-26, local Jetson validation passed all eight install checks with
TensorRT enabled, all eight with TensorRT OFF, and the complete 325-test suite.
A separate `BUILD_TESTING=OFF` production build installed successfully into
`build/install-production-stage`; its executable ran without `LD_LIBRARY_PATH`.
The first six install regressions failed before the install rules were added.
These are local development results, not independent CI evidence.
