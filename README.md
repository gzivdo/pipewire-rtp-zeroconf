# pipewire-rtp-zeroconf

Native PipeWire modules that share LAN audio over RTP, with mDNS (Avahi)
auto-discovery — a replacement for the legacy `module-zeroconf-discover` /
`module-zeroconf-publish` pair that uses `libpulse` under the hood.

Two modules:

- `libpipewire-module-mdns-rtp-publish` — exposes each local Audio/Sink (and
  optionally Audio/Source) under `_pipewire-rtp._udp` in Avahi; LAN peers can
  send RTP to a UDP port allocated per card.
- `libpipewire-module-mdns-rtp-discover` — browses Avahi for the same service
  type and, for each remote card, instantiates a local virtual sink/source
  with a stable `node.name` (so per-app routing survives reconnect / USB
  hotplug).

## Installation — which package where

Three Debian binaries are produced from a single source tree:

| Package | Install on | Why |
|---|---|---|
| `pipewire-rtp-zeroconf-publish` | machines that **own audio hardware** (speakers, amplifier inputs, USB DACs, HDMI audio outputs) | publishes each local card to the LAN as an RTP receiver |
| `pipewire-rtp-zeroconf-discover` | machines that **run apps producing audio** (browser, music player, video conferencing) | makes each remote card appear locally as a virtual sink that apps can play to |
| `pipewire-rtp-zeroconf` | metapackage; install when **the same host plays both roles** | depends on both above |

A typical setup:

```bash
# living-room PC connected to the actual speakers:
sudo dpkg -i pipewire-rtp-zeroconf-publish_*.deb

# laptop with the apps:
sudo dpkg -i pipewire-rtp-zeroconf-discover_*.deb

# desktop with speakers AND apps that should also reach other hosts:
sudo dpkg -i pipewire-rtp-zeroconf_*.deb         # pulls both
```

After install on either side:

```bash
systemctl --user restart pipewire
```

Speaker-host should now show up on the LAN under `_pipewire-rtp._udp` (one
service per local card). App-host should grow one virtual `Audio/Sink` per
discovered remote card, named `Network: <peer-host>: <card description>`,
which apps can target via `pavucontrol` or by setting it as default with
`wpctl set-default <id>`.

> **Finding network streams in pavucontrol.** The in-process RTP
> streams that move audio between hosts (the receiver-side `net-rx ...`
> on a publish host, the back-channel `net-tx (req) ...` on a publish
> host) are **virtual streams**, not application streams, so they do
> not appear under the default *Playback* / *Recording* views. Switch
> the *Show* dropdown at the bottom of pavucontrol to **Virtual
> Streams** (or *All Streams*) to see and volume-adjust them.

## Build (without packaging)

```bash
sudo apt install meson ninja-build pkgconf \
    libpipewire-0.3-dev libspa-0.2-dev libavahi-client-dev
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
systemctl --user restart pipewire
```

The `.so` files install to `/usr/lib/<triplet>/pipewire-0.3/` and the
drop-in confs to `/usr/share/pipewire/pipewire.conf.d/`.

## Firewall

Two things to allow on **both** hosts (publish *and* discover sides need
mDNS to work):

- **UDP 5353** — mDNS / Avahi. The protocol is symmetric multicast:
  every host both listens for and sends queries / responses on this port.
  Allow it **inbound on every host**; outbound is usually permitted by
  default. (Port 5354 — unicast DNS-SD — is **not** needed for our use
  case; if you opened it on a previous setup you can close it.)
- **UDP 46000–46031** — the audio RTP ports allocated by `publish.port.range
  = 32` (default). Allow them **inbound on the publish host(s) only**; the
  discover host opens an ephemeral outbound socket for sending and does
  not need an inbound hole here.

iptables example on a publish host:

```bash
iptables -A INPUT -p udp -i <lan-iface> --dport 5353 -j ACCEPT
iptables -A INPUT -p udp -i <lan-iface> --dport 46000:46031 -j ACCEPT
```

On a discover-only host (apps, no speakers) you only need 5353:

```bash
iptables -A INPUT -p udp -i <lan-iface> --dport 5353 -j ACCEPT
```

If you raise `publish.port.range` widen the second rule; if you switch
to `publish.transport = multicast` you may additionally need to allow
IGMP through any L3 hop (most home/office LANs are flat L2 and do not
need this).

## Migrating from the legacy PulseAudio zeroconf

If your hosts previously used `module-zeroconf-discover` /
`module-zeroconf-publish` (either the PulseAudio modules or the PipeWire
`libpulse`-based wrappers), **turn those off first** — otherwise both
stacks will publish/consume audio at the same time and you will get
duplicated sinks in pavucontrol, two competing tunnels per peer, and
extra LAN traffic.

### PulseAudio (`pulseaudio.service` is the audio server)

Edit `/etc/pulse/default.pa` (or `~/.config/pulse/default.pa` for a
per-user override) and comment out / remove:

```
load-module module-zeroconf-discover
load-module module-zeroconf-publish
```

Then `systemctl --user restart pulseaudio` (or `pulseaudio -k`).
`apt purge pulseaudio-module-zeroconf` if you want to remove the
package entirely.

### PipeWire-only host (pipewire-pulse provides PA compatibility)

The upstream `libpipewire-module-zeroconf-discover` / `-publish` are
**not** loaded by default in any of Ubuntu's `pipewire.conf` /
`pipewire-pulse.conf` shipped configs, so there is usually nothing to
remove. To be sure:

```bash
# nothing should reference the legacy modules:
grep -rE 'zeroconf-discover|zeroconf-publish' \
    /etc/pipewire/ /usr/share/pipewire/ ~/.config/pipewire/ 2>/dev/null
```

If you ever ran `pactl load-module module-zeroconf-discover` or
`module-zeroconf-publish` manually, unload them:

```bash
pactl unload-module module-zeroconf-discover  2>/dev/null
pactl unload-module module-zeroconf-publish   2>/dev/null
```

…or just restart `pipewire-pulse`:

```bash
systemctl --user restart pipewire-pulse
```

The new service type used here (`_pipewire-rtp._udp`) is intentionally
different from the legacy `_pulse-sink._tcp` / `_pulse-source._tcp` so
the two stacks can coexist during migration without confusing each
other's discovery — but you should still disable the old one once
you're satisfied with the new setup, to keep one source of truth and
free up the CPU/network cost of the libpulse tunnels.

## Options reference

Override either by editing the system file
(`/usr/share/pipewire/pipewire.conf.d/30-rtp-zeroconf-publish.conf` or
`-discover.conf`) or — preferred — by copying it to
`~/.config/pipewire/pipewire.conf.d/` and editing there. A user-side file
**replaces** the system one with the same name; you do **not** end up with
two stacked module loads.

### publish (speaker-host)

| Option | Default | Effect |
|---|---|---|
| `publish.sink` | `true` | publish local `Audio/Sink` nodes to the LAN |
| `publish.source` | `false` | publish local `Audio/Source` (mic) too — **opt-in** |
| `publish.rules` | unset | per-card match rules — see [Per-card filtering](#per-card-filtering) below |
| `publish.rate` | `48000` | sample rate advertised in TXT and used by the RTP receiver |
| `publish.channels` | `2` | channel count advertised |
| `publish.format` | `"S16BE"` | wire format. `S16BE` \| `S24BE` \| `S32BE` |
| `publish.codec` | `"pcm"` | `"pcm"` raw L16/L24/L32, or `"opus"` for encoded audio |
| `publish.port.base` | `46000` | first UDP port to allocate per card |
| `publish.port.range` | `32` | how many consecutive ports; cards beyond this are dropped |
| `publish.port` | unset | force a single fixed port (only the first card published) |
| `publish.transport` | `"unicast"` | `"unicast"` \| `"multicast"` |
| `publish.multicast.ip` | `"224.0.0.56"` | multicast group used when `transport=multicast` |
| `publish.multicast.port` | `5004` | base port for multicast (RTP standard) |
| `publish.multicast.ttl` | `1` | hop limit for multicast packets |
| `publish.multicast.loop` | `false` | loop multicast packets back to the sending interface |
| `publish.sap` | `false` | also announce sessions via SAP (multicast-only; lets VLC/gstreamer find the stream) |
| `publish.ptime.msec` | `5` | RTP packet duration — see [Tuning](#tuning) |
| `publish.latency.msec` | `200` | jitter buffer target on the receiver side; must be an integer multiple of `ptime` |

### Per-card filtering

If you want to publish only some local cards (or exclude a few), set
`publish.rules` to a JSON match-action list using the same shape as
upstream `module-rtp-sap`:

```
publish.rules = [
  # Only publish USB DACs:
  { matches = [ { node.name = "~^alsa_output\\.usb-.*" } ]
    actions = { publish = { } } }

  # ...and exclude HDMI outputs by description:
  { matches = [ { node.description = "~.*HDMI.*" } ]
    actions = { exclude = { } } }

  # Default-deny everything else:
  { matches = [ { node.name = "~.*" } ]
    actions = { exclude = { } } }
]
```

Semantics:

- Each rule's `matches` is an array of property predicates. Within a
  single predicate object, **all** key/value pairs must match the
  node's properties. Between sibling predicates in the same `matches`
  array, **any** matching predicate counts.
- Prefix a value with `~` for a POSIX-extended regex; prefix `!` to
  negate the equality check.
- Actions are `publish = { }` (allow) or `exclude = { }` (skip).
- **First matching rule wins.** Order matters.
- Rules are evaluated **after** the built-in default excludes (monitor
  sources, bluez5 devices, anything already wearing
  `node.network = true`, and our own discover-side sinks named
  `network.*`). You don't need to repeat those.
- If `publish.rules` is unset, behaviour is the historical default:
  publish every Audio/Sink (and Audio/Source, when
  `publish.source = true`) that passes the built-in excludes.

For ad-hoc temporary toggling without restarting pipewire, use the
runtime metadata path described in
[Runtime toggles via metadata](#runtime-toggles-via-metadata).

### discover (app-host)

| Option | Default | Effect |
|---|---|---|
| `discover.sink` | `true` | consume remote sinks (create local virtual `Audio/Sink` for each) |
| `discover.source` | `false` | consume remote sources (mic) too — **opt-in** |
| `discover.local` | `false` | when running two pipewire daemons on the same host for testing, allow services published locally |
| `discover.protocol` | `"ipv4"` | which Avahi protocol family to browse. `ipv4` \| `ipv6` \| `any`. Default IPv4 matches the publish-side `0.0.0.0` bind; switch to `any`/`ipv6` only if peers are IPv6-only |
| `discover.ptime.msec` | `5` | must match the publish side's value |
| `discover.latency.msec` | `200` | jitter buffer target; must be a clean multiple of `ptime` |

## Runtime toggles via metadata

Both modules listen for per-card `enabled` keys on the default PipeWire
metadata (provided by WirePlumber on every modern desktop). This lets you
disable / re-enable a specific card or peer **without editing config and
restarting pipewire**.

### Disable / enable a local card on the publish side

```bash
# stop publishing one specific local sink (its Avahi entry is withdrawn
# and the rtp-source child module is unloaded immediately):
pw-metadata 0 "pipewire-rtp-zeroconf.publish.alsa_output.pci-0000_00_1f.3.analog-stereo.enabled" "false"

# re-enable:
pw-metadata 0 "pipewire-rtp-zeroconf.publish.alsa_output.pci-0000_00_1f.3.analog-stereo.enabled" "true"

# clear the explicit value — falls back to default (= enabled):
pw-metadata -d 0 "pipewire-rtp-zeroconf.publish.alsa_output.pci-0000_00_1f.3.analog-stereo.enabled"
```

The key is `pipewire-rtp-zeroconf.publish.<node.name>.enabled`. Look up the
local `node.name` with `wpctl status` or `pactl list short sinks`.

### Disable / enable a discovered remote card on the discover side

```bash
# stop creating local virtual nodes for a specific peer card
# (every direction belonging to this card is unloaded — output and
# input together, since the card is the unit of toggle):
pw-metadata 0 "pipewire-rtp-zeroconf.discover.<peer-host>.<peer-card-name>.enabled" "false"

# re-enable:
pw-metadata 0 "pipewire-rtp-zeroconf.discover.<peer-host>.<peer-card-name>.enabled" "true"
```

The key parts come from the TXT records of the published service:
`<peer-host>` is the publishing machine's hostname, `<peer-card-name>`
is the remote card's identifier (the `card-name` TXT field; defaults
to the node's `node.name` when no parent PipeWire device exists, e.g.
for virtual loopback sinks). Read both from
`avahi-browse -r _pipewire-rtp._udp`.

### Listing current toggle state

```bash
pw-metadata 0 | grep pipewire-rtp-zeroconf
```

### Persistence

Metadata is **runtime state** — it does not survive a `pipewire` restart
on its own. If you want a permanent disable, drop a small script in your
session autostart that sets the key after pipewire is up, or simply edit
the conf-file to use the policy you want at boot. A persistent metadata
store (similar to WirePlumber's `restore-stream`) is a candidate for a
future iteration.

## pavucontrol Card view (experimental)

Each peer **card** — a set of sink + source directions that share the
same underlying audio device on the publishing host — is exposed as a
single native PipeWire `Audio/Device` with `device.api = network-rtp`
and an Off/On `SPA_PARAM_EnumProfile`. WirePlumber picks the device
up automatically, so on most desktops the peer cards show up in
pavucontrol's **Configuration** tab right next to local cards, in the
GNOME / KDE sound panels, and in `wpctl status`.

Grouping is driven by the `card-name` TXT field which the publish
side derives from each node's parent `device.name`. Two services with
different node names (e.g. `alsa_output.pci-…` and
`alsa_input.pci-…`) that share the same parent device end up under
one card on the discover side; orphan virtual nodes that have no
parent (a `module-loopback` sink, say) fall back to their `node.name`
and appear as their own card.

What works today:

- The device appears with the right description (`Network: <peer-host>:
  <card-description>`).
- `pw-cli enum-params <device-id> EnumProfile` lists the Off/On
  profiles we publish, plus whatever WirePlumber's policy adds on top.

What is not wired yet (deferred):

- A Profile change applied via pavucontrol's dropdown does not
  currently round-trip through to our `impl_set_param` callback because
  WirePlumber owns the policy decision for `Audio/Device` nodes and
  doesn't forward unknown profile classes to the SPA implementation.
  Use the [runtime metadata toggle](#runtime-toggles-via-metadata) for
  enable/disable until the WP-side hook lands.
- Profile change does not yet emit `spa_device_object_info` events to
  let WP own the child sink/source nodes the way it does for ALSA
  cards; the rtp-sink module is loaded by us directly as a separate
  PipeWire module.

Both gaps will close once the device emits proper per-profile
`object_info` events and the WP `policy-device.lua` rules learn the
`network-rtp` device class. Tracked for a follow-up.

## Tuning

Defaults are chosen so it just works on a normal home LAN with 1500-byte
MTU. If you want to push latency lower, raise stability on lossy wifi, or
exploit a switch fabric that supports jumbo frames, here are the knobs.

### Latency

- `*.latency.msec = 200` (default) = 40 packets buffered at 5 ms ptime.
  Comfortable for wifi, multi-hop LAN, or sender hosts under heavy load.
- `100` is the lowest reasonable value on a clean wired LAN; below that
  the receiver risks underrun on every CPU hiccup on either side.
- `400` is a good value to try first if you hear stuttering on wifi.

Always keep `latency.msec` an **integer multiple** of `ptime.msec`, on
both sides. If not, the upstream rtp-source logs a warning and rounds
internally, but you've effectively misaligned the jitter buffer.

### Packet time (`ptime`)

`ptime` controls how many audio frames go in one RTP packet. Bigger
packets = less per-packet overhead but more loss damage and tighter MTU
constraints. The byte-size of one packet is:

```
audio_bytes = ptime_msec × rate × channels × sample_size
             (5 ms × 48000 × 2 ch × 2 B = 960 B at default settings)
total_bytes = audio_bytes + RTP_header (12 B) + UDP_header (8 B)
                                              + IP_header (20 B IPv4 / 40 IPv6)
            = ~1000 B at default settings
```

One packet **must fit inside the path MTU**, otherwise the IP stack
fragments it. Each fragment can independently be lost or reordered;
losing any one of them loses the whole packet — and that's what causes
the audible **crackle / clicks**.

Maximum safe ptime for common MTUs at stereo 16-bit @ 48 kHz:

| MTU | Headroom | Max ptime (audio bytes ≤ headroom) |
|---|---|---|
| **1280** (default, IPv6-safe) | 1240 B | ~6.4 ms |
| **1500** (standard Ethernet) | 1460 B | ~7.6 ms |
| **9000** (jumbo frames) | 8960 B | ~46 ms |
| **9216** (some 10 GbE NICs) | 9176 B | ~47 ms |

We default `ptime = 5` to stay comfortably inside the 1280-MTU envelope.

### Jumbo frames

If both ends of your network — every switch in between included — support
jumbo frames, you can push ptime up to 20 ms or higher. That cuts packet
rate from ~200 pps to ~50 pps and reduces CPU overhead noticeably on
low-power boxes (raspberry-pi-class receivers).

Check MTU on each interface in the path:

```bash
ip link show <iface>           # look for "mtu 1500" or "mtu 9000"
```

Set jumbo MTU per interface (NetworkManager):

```bash
nmcli connection modify "<conn>" 802-3-ethernet.mtu 9000
nmcli connection up "<conn>"
```

…or via systemd-networkd / `/etc/network/interfaces` / your favourite
config. Switches: most managed ones have a per-port or global "jumbo
frame" enable; consult the switch manual.

Then on both sides set, in your conf:

```
publish.ptime.msec   = 20
publish.latency.msec = 200   # still a multiple
# discover side mirrors these
discover.ptime.msec   = 20
discover.latency.msec = 200
```

Note: we don't currently override the upstream rtp-sink's default
`net.mtu = 1280` from this module, so even with jumbo frames you should
verify that no fragmentation happens in practice. To pass an explicit
`net.mtu` to the child rtp-sink, drop into `stream.props` via your own
overlay until we expose `*.mtu` properly (planned).

## Troubleshooting

### Stuttering, clicks, regular crackle

Most likely cause is **IP fragmentation** (one packet split into multiple
UDP datagrams). Check:

```bash
# on the sender, watch for "fragment" in dmesg or sniff:
sudo tcpdump -i any -n udp port 46001 -vv 2>&1 | head -5
# look for "[+]" flags in the IP header — that's fragmentation
```

Fix in order of preference:
1. Make sure both ends are running the same `ptime.msec`.
2. Lower `*.ptime.msec` (try 5, then 4, then 2).
3. Raise `*.latency.msec` to give the jitter buffer more cushion.
4. If you have switches and NICs that support it, enable jumbo frames
   (see [Tuning](#tuning)).

### No audio at all

```bash
# on speaker-host — services published?
avahi-browse -r -t _pipewire-rtp._udp

# on app-host — is the virtual sink present?
wpctl status | grep -i network
pactl list short sinks | grep -i network

# packets actually moving?
sudo tcpdump -i any -n -c 5 udp port 46001
```

Common culprits:

- **Firewall blocking UDP 46000–46031**. The packets reach the receiver
  but the kernel drops them.
- **IPv4 vs IPv6 mismatch**. We default to IPv4 on the discover side
  because the publish side binds `0.0.0.0` (IPv4-only). If your LAN is
  IPv6-only, set `discover.protocol = "any"` on the discover side and
  the publish bind is still 0.0.0.0 — switch the network or wait until
  we expose `publish.bind` for `::`.
- **WirePlumber didn't pick the network sink as default.** Use
  `pavucontrol` Output Devices tab, or `wpctl set-default <id>`.
- **Both ends advertise the same hostname** (e.g. two machines named
  `pc`). Rename one — Avahi adds `#2` to the published service name but
  TXT `host` clashes cause confusion.

### Traffic flows even when nothing is playing

Almost certainly a **VU-meter app is open**: `pavucontrol`,
`plasma-pa`/Plasma volume widget, GNOME settings → Sound. They open a
record stream on the sink's monitor port, which keeps the sink driver
active. Close the meter and the UDP traffic stops within a quantum.

To verify:

```bash
pkill pavucontrol plasma-pa 2>/dev/null; sleep 3
sudo timeout 8 tcpdump -i any -n udp port 46001 2>&1 | grep -c "UDP, length"
# should report 0 or very close to 0
```

### Sender restart drops the app's stream

When the host running discover restarts pipewire (or the network blips
long enough that Avahi removes the peer), the virtual sink disappears.
After it comes back the app's PA/PW stream may not auto-reattach — this
is a WirePlumber policy decision, not RTP. The deterministic
`node.name = network.<peer-host>.<peer-node-name>` gives WP everything it
needs to restore the link via `restore-stream`, but some PA-shim clients
don't honour it. Workarounds:

- Modern PW-native or libcpal apps reconnect automatically.
- For stubborn PA-shim apps (mpv with explicit `--audio-device`,
  Audacity), restart the app or re-select the sink in `pavucontrol`
  Playback tab once.
- This is the same UX as USB hotplug: the system can recreate the
  device, but it cannot un-stick an app that hard-bound itself to an
  ID that no longer exists.

### "unexpected SSRC" / "unexpected seq" in the log

Both are harmless one-shots after a sender restart — receiver re-syncs
within a packet or two and audio resumes. The deterministic SSRC
(derived from peer identity) eliminates the SSRC variant; the seq jump
is intrinsic to RTP and only logs at debug levels.

### IGMP recovery / "failed to re-join IPv4 multicast group: 0.0.0.0"

Upstream `module-rtp-source` enables a multicast IGMP-rejoin timer even
when configured for unicast (`source.ip = 0.0.0.0`). This module
suppresses it by setting `igmp.check.interval.sec = 31536000` on the
child rtp-source — you should not see these messages with the bundled
defaults. If you do, the .so is stale; reinstall.

## Configuration

### publish (speaker-host)

`/usr/share/pipewire/pipewire.conf.d/30-rtp-zeroconf-publish.conf`:

```
context.modules = [
    { name = libpipewire-module-mdns-rtp-publish
      args = {
          publish.sink   = true      # publish local sinks (default)
          publish.source = false     # opt-in: also publish microphones

          # publish.rate     = 48000
          # publish.channels = 2
          # publish.format   = "S16BE"  # S16BE | S24BE | S32BE
          # publish.codec    = "pcm"    # pcm | opus

          # publish.port.base  = 46000
          # publish.port.range = 32       # firewall: open 46000:46031/udp

          # publish.transport       = "unicast"     # unicast | multicast
          # publish.multicast.ip    = "224.0.0.56"
          # publish.multicast.port  = 5004          # RTP-standard
          # publish.multicast.ttl   = 1
          # publish.sap             = false         # SAP announce (multicast only, for VLC)
      }
      flags = [ ifexists nofail ]
    }
]
```

### discover (app-host)

`/usr/share/pipewire/pipewire.conf.d/31-rtp-zeroconf-discover.conf`:

```
context.modules = [
    { name = libpipewire-module-mdns-rtp-discover
      args = {
          discover.sink   = true       # consume remote sinks (default)
          discover.source = false      # opt-in: also consume remote mics
          # discover.protocol = "ipv4" # ipv4 | ipv6 | any
      }
      flags = [ ifexists nofail ]
    }
]
```

## Behaviour

- **Per-card granularity**: one Avahi service per local card; each card gets
  its own UDP port; remote machines see each card as a separate sink.
- **USB hotplug**: cards appearing/disappearing locally are tracked via the
  PipeWire registry and re-published/unpublished immediately. No grace
  period — gone is gone.
- **Stable identity**: virtual sinks on the app-host are named
  `network.<peer-host>.<peer-node-name>` derived from the TXT record, so
  WirePlumber `restore-stream` can reconnect apps after peer or hotplug
  events without the user re-routing.
- **No traffic when silent**: `node.suspend-on-idle = true` on the sink-side
  pauses the underlying driver when no client is playing, so the UDP socket
  is silent. (Note: a VU-meter app — pavucontrol, Plasma volume control —
  reading the monitor port keeps the sink active and traffic flows.)
- **Stable RTP SSRC**: derived from a hash of `<peer-host>/<peer-node>`, so a
  sender restart doesn't cause the receiver to reject packets.
- **MTU-aware ptime**: default 5 ms keeps each RTP packet under the default
  1280 MTU at stereo 16-bit @ 48 kHz, avoiding IP fragmentation that
  manifests as crackle.
- **IPv4 by default**: publish binds to `0.0.0.0`; discover picks the IPv4
  Avahi resolve. Switchable to IPv6 or dual-stack via `discover.protocol`.

## VLC / generic RTP interop

Enable `publish.sap = true` and switch to `publish.transport = multicast`
to advertise sessions via SAP (RFC 2974). VLC sees them under "Playlist →
Service Discovery → SAP announces"; ffmpeg/gstreamer pick them up the same
way.

## Limitations

### Single producer per remote sink card

For every published Audio/Sink, the publish host binds exactly one UDP
port and loads one `libpipewire-module-rtp-source` on it. Upstream
rtp-source uses one ringbuffer locked to one SSRC, so if two discover
hosts both try to play to the same speaker card concurrently their
packets either (a) get dropped as "unexpected SSRC" for one of them, or
(b) with `sess.ignore-ssrc = true` get interleaved into the same
ringbuffer at incompatible timestamps producing audible chaos. Neither
is a useful "party speaker" mode.

PulseAudio's `module-rtp-recv` *did* multi-producer via SAP: each sender
announced its own multicast session, the receiver created a separate
sink-input per session, and the local sink mixed them. We deliberately
don't carry SAP over as the primary path (it's multicast-only,
non-trivial to firewall, and the PA stack on top is the very thing we
escaped from). One sender per sink covers homelab playback and is the
common case; the multi-producer extension is queued as future work.

#### Future-work options (sized so the next pass can just pick one)

| # | Approach | Effort | Tradeoff |
|---|---|---|---|
| **A** | **Eager port pool + hash dispatch.** Publish allocates N ports per card up-front, advertises them in TXT, discover hashes `(local-hostname) mod N` to pick a slot. | ~80 lines | With N=4 and 3 producers a hash collision is ~75% likely → audio chaos for the unlucky pair. Doesn't scale. |
| **Y / Z2** | **Dynamic per-requester Avahi play-request / play-response.** Discover publishes `_pipewire-rtp-play._udp` when it wants to play; publish allocates a fresh port + loads rtp-source on demand and publishes `_pipewire-rtp-play-resp._udp` with the port; discover sends there. Eager slot 0 + lazy slots 1..N-1. Adds inactivity timer for hard-disconnect cleanup. Add `rtp.receiver-ssrc` in the request to defend against SSRC contamination on fast bounce. | ~400 lines + 30 (timer) + 10 (SSRC) | Adds ~50–200 ms to first-packet ramp-up because of the extra mDNS round-trip. Graceful — every requester gets a private port. Mirrors the existing back-channel architecture. **The preferred option when we revisit.** |
| **X** | **Custom SSRC demultiplexer on publish.** Publish keeps one UDP listener per card; on each incoming packet it dispatches to a per-`(src_ip, ssrc)` ringbuffer + PW stream that it creates lazily. No new Avahi services. | ~300–400 lines | Replaces upstream rtp-source with our own receiver — we re-implement the ringbuffer / jitter / dll / clock dance. Easy to introduce subtle audio glitches; debugging is much harder than the Avahi route. |
| **Z1** | **Eager per-discoverer allocation.** Discover hosts publish `_pipewire-rtp-discoverer._udp` presence at startup; publish proactively allocates a slot for each `(local-card, discoverer)`. | ~250 lines | Wasteful — every discoverer-on-the-LAN reserves a port on every publish card, whether it ever plays or not. Untenable in busy LANs (10 idle laptops × 4 publish cards = 40 reserved ports). |

Decision in v0.1.6: **none** — keep the single-producer model, accept it
as a documented gap. Pick this up if/when a real "party speaker" use
case comes in.

### Multi-card publish hosts vs free routing in pavucontrol

You can route the inbound network `net-rx ...` stream on a publish host
to any local sink via pavucontrol (it's a virtual stream, see the note
under "Installation"). The Avahi-published `node.target` is just the
default — pavucontrol can override at runtime. So even without
multi-producer, a single discover host can play to any of N speakers on
the publish side by changing the routing.

## Status

Working baseline, in production-style testing on Ubuntu 24.04 (PW 1.0.5) and
26.04 (PW 1.6.2). UI/per-card toggle is the next step.

## Why this exists

### What was wrong with the legacy stack

**PulseAudio `module-zeroconf-discover` / `-publish`** (still shipped today
in `pulseaudio-module-zeroconf`) tunnels audio over **PulseAudio native
protocol over TCP**. Pre-PipeWire era it was the only option; it ages
poorly:

- Pulls in the full PulseAudio dependency surface. On a PipeWire-only host
  you have to install `pulseaudio-utils`/`-modules` just for the tunnel.
- TCP-based: stalls under backpressure, hard to tune for low latency
  (typical 100–200 ms with stutter under load).
- For every announced peer it **always loads both a sink-tunnel and a
  source-tunnel**, and the source-tunnel **streams the remote microphone
  continuously**, 24/7, regardless of whether anything on the local side
  consumes it. This eats LAN bandwidth and (worse) hands the remote mic
  audio to your machine permanently. There is no opt-out short of editing
  the module source.
- No reconnect on transient peer drop: a brief Avahi blip kills the tunnel
  module, it does not come back without a manual reload.
- The created sink/source nodes have whatever names libpulse felt like, so
  WirePlumber `restore-stream` cannot reliably reattach apps after a
  reconnect or USB hotplug.
- No idle suspension — the tunnel keeps a TCP stream open and sends silence
  even when nothing is playing.

**PipeWire's own `module-zeroconf-discover` (and `module-protocol-pulse`'s
`module-zeroconf-publish`)** look like a replacement but in practice are a
thin wrapper that loads `libpipewire-module-pulse-tunnel`, which **uses
`libpulse` internally** to talk to the remote PulseAudio (or PipeWire's
PA-compat layer). The architecture and the user-visible problems are the
same as the PA module — same mandatory record stream, same TCP, same
PA-protocol on the wire, same reconnect gaps. Plus:

- Default `reconnect.interval.ms = 0` in `module-pulse-tunnel` means a
  dropped TCP socket is **fatal** — the tunnel module destroys itself and
  is gone until mDNS re-announces. `module-zeroconf-discover` doesn't even
  forward a custom interval option.
- Loading `libpipewire-module-zeroconf-discover` directly under the
  `pipewire` daemon (instead of from inside `pipewire-pulse`) frequently
  fails or produces a half-working tunnel because the module's
  `pw_context_get_object(...Core)` fallback expects a client-side core
  proxy that only the pulse-server context has. In other words it works in
  the `pipewire-pulse` shim and is unreliable as a true native module.
- No UI for per-card publish/subscribe toggle — both sinks and sources
  appear unconditionally for every peer.

So even on a "pipewire-only" system you're still effectively running the
PulseAudio tunnel protocol; the only thing that changed is who hosts the
process.

### What this project does instead

Everything is native PipeWire and **no `libpulse` anywhere**:

- Audio rides over **RTP/UDP** with the upstream
  `libpipewire-module-rtp-sink` / `-source` as the per-stream transport.
  These are pure-`pw_stream` modules; no pulse code touched.
- Discovery is **mDNS** under a fresh service type `_pipewire-rtp._udp`,
  not the legacy `_pulse-{sink,source}._tcp`. Coexists peacefully with the
  old PA modules if you still need them somewhere.
- **Microphone stream is opt-in, separately on both sides.** Default is
  publish sinks only, discover sinks only. To share or consume mics you
  must explicitly set `publish.source = true` / `discover.source = true`.
  Solves the "remote mic always streams" complaint.
- **No traffic when silent.** `node.suspend-on-idle = true` on the sink
  side parks the sink driver when no client is playing into it; the rtp-
  sink callback stops firing; the UDP socket goes quiet. Wake-up on the
  next playback is near-instant. Receiver pauses symmetrically via
  `stream.may-pause = true`. The only caveat is that opening a VU-meter
  app (`pavucontrol`, Plasma volume widget) keeps the monitor port active
  and traffic flows — that is the meter's doing, not ours.
- **Latency is honest and tunable.** Defaults are 5 ms RTP packet time and
  200 ms jitter buffer (= 40 packets in flight). 5 ms ptime was chosen so
  one RTP packet at stereo 16-bit @ 48 kHz fits under the standard 1280
  MTU — going higher (e.g. the upstream-default 6.45 ms or 10 ms) forces
  IP fragmentation and produces audible crackle. Lower the latency to ~50
  ms on a clean wired LAN if you want it tighter; raise it past 200 ms on
  congested wifi. `publish.ptime.msec` / `publish.latency.msec` and the
  symmetric `discover.*` options are exposed for both sides.
- **Reconnect is built into the transport, not bolted on.** UDP has no
  session to lose; sender-restart causes a brief sequence-number jump that
  the receiver re-syncs within a packet or two. A stable RTP SSRC derived
  from `FNV-1a(peer-host, peer-node)` keeps the receiver from rejecting
  the new packets as "wrong sender" after a sender bounce.
- **Per-card identity and granularity.** Each local card is its own Avahi
  service on its own UDP port; each remote card surfaces as a separate
  virtual sink with a stable, content-derived `node.name`. WirePlumber's
  `restore-stream` reattaches apps after USB hotplug or peer reboot
  without manual re-routing.
- **VLC / generic RTP interop.** Optional SAP announcement
  (`publish.sap = true`, multicast only) lets VLC, gstreamer and ffmpeg
  pick the stream up via their standard SAP service discovery, without
  hand-crafting an SDP file.

### References

- Open upstream issue requesting exactly this functionality:
  [pipewire/pipewire#865 "Play media over network"](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/865)
- Architectural precedent in upstream itself:
  `src/modules/module-snapcast-discover.c` — same Avahi-browse + spawn-child-
  stream-module pattern, just for Snapcast servers instead of RTP audio.

## License

MIT. See `debian/copyright`.

## Authors

- [gzivdo](https://github.com/gzivdo) — design, requirements, testing,
  bug reports against real-world LAN setups.
- Claude Opus 4.7 (Anthropic) — implementation under gzivdo's guidance.
  See per-commit `Co-Authored-By` trailers for attribution.
