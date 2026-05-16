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

The upstream `module-zeroconf-discover` / `-publish` modules use `libpulse`
to tunnel audio — they pull in PulseAudio dependencies, only work over TCP
(PA-protocol), don't auto-reconnect, can't disable the always-on record
stream, and don't show as native PipeWire devices. This project goes
RTP-over-UDP, native PW modules, no PulseAudio dependency.

See upstream issue
[pipewire/pipewire#865](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/865)
and the precedent set by `module-snapcast-discover` for the same
architectural pattern applied to a different protocol.

## License

MIT. See `debian/copyright`.
