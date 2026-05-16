# pipewire-net-zeroconf

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

## Packaging

Three Debian binaries:

- `pipewire-net-zeroconf-publish` — speaker-host (machine with cards).
- `pipewire-net-zeroconf-discover` — app-host (machine playing audio).
- `pipewire-net-zeroconf` — metapackage installing both.

```bash
debian/rules clean && debian/rules binary
sudo dpkg -i ../pipewire-net-zeroconf-<role>_*.deb
```

The published `.so` files live at `/usr/lib/<triplet>/pipewire-0.3/` and the
`30-net-zeroconf-publish.conf` / `31-net-zeroconf-discover.conf` drop-ins at
`/usr/share/pipewire/pipewire.conf.d/`.

## Build (without packaging)

```bash
sudo apt install meson ninja-build pkgconf \
    libpipewire-0.3-dev libspa-0.2-dev libavahi-client-dev
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
systemctl --user restart pipewire
```

## Configuration

### publish (speaker-host)

`/usr/share/pipewire/pipewire.conf.d/30-net-zeroconf-publish.conf`:

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

`/usr/share/pipewire/pipewire.conf.d/31-net-zeroconf-discover.conf`:

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
