/* SPDX-License-Identifier: MIT */
#ifndef PWNZ_COMMON_H
#define PWNZ_COMMON_H

#include <stdint.h>

/* Avahi service type for pipewire-net-zeroconf RTP audio announcements. */
#define PWNZ_SERVICE_TYPE "_pipewire-rtp._udp"

/* Avahi service type for unicast back-channel requests. A discover host
 * publishes this when it wants to subscribe to a remote mic over
 * unicast; the publishing host browses for matching requests and loads
 * one rtp-sink per requester to send the audio. */
#define PWNZ_REQUEST_SERVICE_TYPE "_pipewire-rtp-req._udp"

/* Request-only TXT keys (added to the standard keys). */
#define PWNZ_TXT_TARGET_HOST  "target-host"   /* host of the speaker we want from */
#define PWNZ_TXT_TARGET_CARD  "target-card"   /* card-name on that speaker */

/* TXT keys exchanged between publish and discover. */
#define PWNZ_TXT_NODE_NAME    "node-name"
#define PWNZ_TXT_DESCRIPTION  "description"
#define PWNZ_TXT_HOST         "host"
#define PWNZ_TXT_RATE         "rate"
#define PWNZ_TXT_CHANNELS     "channels"
#define PWNZ_TXT_FORMAT       "format"
#define PWNZ_TXT_PAYLOAD      "payload"
#define PWNZ_TXT_SESSION      "session"
#define PWNZ_TXT_MODE         "mode"        /* "sink" or "source" */
#define PWNZ_TXT_CODEC        "codec"       /* "pcm" or "opus" */
#define PWNZ_TXT_TRANSPORT    "transport"   /* "unicast" or "multicast" */
#define PWNZ_TXT_MCAST_IP     "multicast-ip"
#define PWNZ_TXT_MCAST_TTL    "multicast-ttl"
/* Stable identifier of the underlying audio card.
 * Two services from the same peer host with the same card-name are
 * different directions (sink+source) of the *same* card and should be
 * presented as one device with output/input profile on the discover
 * side. Derived from PW_KEY_DEVICE_NAME on publish side; falls back to
 * PW_KEY_NODE_NAME for orphan virtual nodes that have no parent
 * device. */
#define PWNZ_TXT_CARD_NAME    "card-name"

#define PWNZ_DEFAULT_RATE       48000u
#define PWNZ_DEFAULT_CHANNELS   2u
#define PWNZ_DEFAULT_FORMAT     "S16BE"
#define PWNZ_DEFAULT_CODEC      "pcm"
#define PWNZ_DEFAULT_PAYLOAD    96u
#define PWNZ_DEFAULT_TRANSPORT  "unicast"
#define PWNZ_DEFAULT_MCAST_IP   "224.0.0.56"
#define PWNZ_DEFAULT_MCAST_TTL  1u

/* Default unicast port allocation range. Kept tight so a single firewall
 * rule covers all expected cards on one host. */
#define PWNZ_DEFAULT_PORT_BASE   46000u
#define PWNZ_DEFAULT_PORT_RANGE  32u

/* Multicast RTP port (RFC 3551 / RTP/AVP convention). */
#define PWNZ_DEFAULT_MCAST_PORT  5004u

/* Packet/latency defaults.
 *
 * ptime is chosen so one RTP packet fits in the default MTU (1280):
 *   stereo 16-bit @ 48 kHz × 5 ms = 960 audio bytes + ~40 headers ≈ 1000 B.
 * Going higher (e.g. 10 ms) overflows MTU and the IP stack fragments
 * each packet, multiplying loss probability and producing clicks/crackle.
 *
 * latency is a clean multiple of ptime: 200 ms = 40 packets buffered,
 * enough to absorb LAN/wifi jitter without underruns.
 */
#define PWNZ_DEFAULT_PTIME_MS    5.0
#define PWNZ_DEFAULT_LATENCY_MS  200u

#endif /* PWNZ_COMMON_H */
