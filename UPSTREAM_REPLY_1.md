# Reply to maintainer feedback — round 1

Draft answer to:
> Is this comparable to module-rtp-{sap,session,sink,source} already
> upstream? I think this should be merged with rtp-session somehow.
> Also it should use pw_zeroconf.

---

Thanks for the quick look. Comparing to each of the upstream RTP modules:

| Upstream module | Relationship to this proposal |
|---|---|
| **`module-rtp-sink` / `module-rtp-source`** | We **use these as child modules** via `pw_context_load_module()` — they are our transport primitives, not competition. Our publish module loads one `rtp-source` per incoming RTP card; the discover module loads one `rtp-sink` per outgoing card. We are strictly on top, not parallel. |
| **`module-rtp-sap`** | SAP (RFC 2974) advertises sessions over a known multicast group. PA-compatible, multicast-only. Discovery is by listening on that group. Our use-case is unicast-first with Avahi/mDNS, so the discovery mechanism is orthogonal — though we do support multicast + SAP via the `publish.sap = true` config knob for VLC/gstreamer interop. |
| **`module-rtp-session`** | Real overlap, and you're right that it's the closest existing thing. apple-midi-based session setup + Avahi auto-pair, supports `media = audio | midi | opus`. The differences are mostly in **what gets discovered**: our publish module monitors the local PW node graph for Audio/Sink (and optionally Audio/Source) globals and announces one mDNS service per local card, mapping `node.name` / parent `device.name` into a stable `card-name` TXT key so two directions of the same card group into one device on the receiver. rtp-session today is one-session-per-module: you load it and it announces one stream. We instead have one module that fans out N child rtp-source / rtp-sink as cards come and go on the host. |
| **`module-zeroconf-discover` / `module-zeroconf-publish`** | These tunnel via the PulseAudio native protocol (libpulse), not RTP. They are the things we are explicitly trying to replace. |

**`pw_zeroconf` — agreed, will port to it.** I had been looking at the 1.6.2 source tree where `pw_zeroconf` doesn't exist yet, so we have our own avahi-client/avahi-poll glue (~180 lines including the poll adapter, lifted from the same `module-zeroconf-discover/avahi-poll.{c,h}` that existed in 1.6 — verbatim copy). Migrating to `pw_zeroconf` (now in `src/modules/zeroconf-utils/`) drops that glue and unifies us with rtp-session, snapcast-discover, zeroconf-discover. Easy win.

**On merging with `module-rtp-session`** — happy to, but want to align on shape before I refactor. Two interpretations:

1. **rtp-session grows a "node-monitor" mode.** We add a new property (say `sess.monitor = "audio-sinks"|"audio-sources"|"both"`) that tells one rtp-session instance to watch local PW globals (matching some rule set) and self-announce/teardown a service per matching node, instead of being a single-session module. The audio-sharing config layer becomes a thin conf drop-in (`30-rtp-sharing.conf`) that just loads rtp-session in monitor mode. Our `module-mdns-rtp-publish` body collapses into ~50 lines of "card-name derivation + PW global watcher", contributed as helpers inside rtp-session.
2. **rtp-session is the transport, a new "rtp-sharing" module is the policy layer.** rtp-session keeps its current shape (one session per instance, apple-midi or audio). A new thinner upstream module wraps it: monitors local cards, loads one rtp-session child per card with appropriate args. This keeps the apple-midi machinery out of the path for the simple speaker-sharing use case (the receiver currently doesn't need session control packets — we just want a unidirectional audio stream + back-channel for mic-on-demand).

Question for you on (1) vs (2):

- Is the apple-midi session-control layer mandatory in rtp-session, or can it be skipped for plain audio streams (so RTP transport is the only thing on the wire)? Looking at the module today it always runs the apple-midi handshake — that's overhead for one-shot speaker sharing where there is no real session lifecycle beyond "Avahi service exists → consume".
- The current proposal also has a **back-channel for the mic direction** (`_pipewire-rtp-req._udp`) so the publish host only opens the local mic when a remote consumer is actually recording (input-on-demand). I don't see this pattern in rtp-session — would adding it cleanly in (1) or (2) be the right shape, or is there an existing mechanism I should hook into instead?
- We currently expose remote multi-direction cards as a single in-process SPA `Audio/Device` with `Profile`/`EnumProfile` so pavucontrol shows one row with a profile dropdown rather than two separate sink+source rows. Want it kept, dropped, or replaced by a different abstraction?

Happy to do the port in whichever shape you prefer — just want to make sure we converge on the design before I send a 4000-line MR you'll have to bounce.
