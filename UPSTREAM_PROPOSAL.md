# Proposal: native PipeWire mDNS+RTP audio-sharing modules

Draft text for filing as an issue on
<https://gitlab.freedesktop.org/pipewire/pipewire/-/issues>. Copy
verbatim or trim as you like before submitting — written
maintainer-friendly (no bikeshedding on UX, focus on architecture +
scope).

---

## Summary

We have a working out-of-tree pair of PipeWire modules that share LAN
audio over RTP with mDNS/Avahi auto-discovery, intended as a native
replacement for the libpulse-based `module-zeroconf-discover` /
`module-zeroconf-publish` pair. The implementation has been in
production-style testing on Ubuntu 24.04 (PipeWire 1.0.5) and Ubuntu
26.04 (PipeWire 1.6.2). Source, packaging, README, CI, and a tagged
v0.1.6 release with debian artifacts for both distros are public at:

  <https://github.com/gzivdo/pipewire-rtp-zeroconf>

MIT-licensed. Filing this as a **design discussion / interest gauge**
before opening a Merge Request — would the project welcome integration
as upstream `libpipewire-module-mdns-rtp-publish` /
`-discover` (replacing or complementing the legacy zeroconf modules)?

## Why not the existing zeroconf modules

The shipped `module-zeroconf-discover` / `-publish` (PipeWire 1.x) tunnel
audio over the PulseAudio native protocol via libpulse. From a
PipeWire-first deployment angle the limitations are:

- Pulls in the full PulseAudio dependency surface even on
  PipeWire-only hosts.
- TCP-based: stalls under backpressure, hard to tune for low latency.
- Loads both a sink-tunnel and a source-tunnel for every announced peer
  and streams the remote microphone 24/7 regardless of local consumers
  (no opt-out).
- No reconnect on transient peer drop: a brief Avahi blip kills the
  tunnel module and it does not come back without a manual reload.
- Created sink/source nodes have unstable names, so WirePlumber
  `restore-stream` cannot reliably reattach apps after reconnect / USB
  hotplug.
- No idle suspension — keeps a TCP stream open and sends silence.

## Design

Two daemon-loadable modules:

- **`libpipewire-module-mdns-rtp-publish`** — watches local Audio/Sink
  (and optionally Audio/Source) nodes, allocates a UDP port per local
  card, loads the existing `libpipewire-module-rtp-source` /
  `-rtp-sink` as a child via `pw_context_load_module()`, and announces
  the endpoint via Avahi under `_pipewire-rtp._udp`. Per-card
  enable/disable through the default `pw_metadata`.
- **`libpipewire-module-mdns-rtp-discover`** — browses Avahi for the
  same service type, groups sink+source TXT records by their published
  `card-name` field, exposes each remote card as a single
  `Audio/Device` (small in-tree SPA device that publishes
  `Profile`/`EnumProfile` and routes through the same metadata path),
  and lazily instantiates `libpipewire-module-rtp-sink` / `-source`
  per active direction with deterministic `node.name = network.<peer-host>.<peer-node>`
  so WirePlumber routing survives re-publish.

Wire-protocol choices:

- Default codec: L16 BE, 48 kHz, S16BE. Opus, S24BE, S32BE optional.
- Default transport: UDP unicast. Multicast optional (with SAP via
  `publish.sap = true` for VLC / gstreamer interop, RFC 2974).
- Stable RTP SSRC derived from FNV-1a(peer-host, peer-node) so a
  sender restart does not cause receiver drops.
- Back-channel for the mic (source) direction is unicast via a second
  Avahi service type `_pipewire-rtp-req._udp` — the discover host
  publishes a request only when its Input profile is active **and** an
  application is actually recording (input-on-demand, debounced
  withdraw). Solves the always-streaming mic problem from the legacy
  stack.

Packaging is split into three debs:

- `pipewire-rtp-zeroconf-publish` — speaker-host
- `pipewire-rtp-zeroconf-discover` — app-host
- `pipewire-rtp-zeroconf` — metapackage pulling both

A single combined package failed because PipeWire merges (not
overrides) `context.modules` across drop-in confs, so a partial
disable on one role required two binaries.

## Status

- Tagged v0.1.6, deb artifacts attached:
  <https://github.com/gzivdo/pipewire-rtp-zeroconf/releases/tag/v0.1.6>
- README:
  <https://github.com/gzivdo/pipewire-rtp-zeroconf/blob/main/README.md>
  including a "Limitations" section that openly discusses the
  single-producer-per-card gap vs PulseAudio's `module-rtp-recv`
  (which used SAP-discovered per-sender sessions) and lays out four
  sized implementation options for closing it.

## Known scope choices to discuss

1. **Relation to `libpipewire-module-rtp-session`.** Existing module is
   apple-midi-flavoured, mid-stream announcement, designed for
   peer-to-peer DAW/MIDI. This proposal is targeted at the
   `module-zeroconf-*` ergonomic niche (LAN speakers, USB DAC sharing,
   easy auto-discovery). We deliberately do not subsume rtp-session;
   the modules can coexist.
2. **Encryption / auth.** Currently none; the existing zeroconf modules
   are also plaintext, and a LAN-trust assumption is documented. Open
   to making it a Stage-N work item.
3. **Single producer per sink card.** Documented as a limitation; we
   intentionally did not carry over PA's SAP-based multi-producer
   mechanism because (a) it is multicast-only and awkward to firewall,
   (b) the SAP-on-PA layering is exactly what we are stepping back
   from. Four implementation options are sized in the repo README if
   upstream prefers a different approach before merge.
4. **Out-of-tree SPA `peer-device.c`.** Small in-tree SPA device that
   aggregates sink+source directions of one remote card as a single
   profile-dropdown device. Could be either upstreamed as-is or
   reworked to use `spa_device_emit_object_info` once the multi-stream
   case is reconsidered.

## Question to maintainers

Is this direction welcome upstream? Specifically:

- Would you accept the new modules as siblings to the existing
  `module-zeroconf-*` (with a clear "PipeWire-native replacement"
  framing in the docs), or would you prefer the existing modules be
  rewritten in place?
- Any preference on the Avahi service-type name (`_pipewire-rtp._udp`
  vs something else upstream-flavoured)?
- Any blockers in the design above before we open an MR with the
  ported tree?

Happy to refactor against any feedback. The code is intentionally
small (~3.5k lines including peer-device, headers, conf, CI) to keep
review tractable.
