/* SPDX-License-Identifier: MIT
 *
 * pipewire-net-zeroconf — mDNS/RTP discover module.
 *
 * Browses Avahi for _pipewire-rtp._udp services and, for each peer
 * card, instantiates a local virtual Audio/Sink (libpipewire-module-rtp-sink)
 * or Audio/Source (libpipewire-module-rtp-source) with deterministic
 * names so per-app routing survives reconnect.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>

#define PTIME_BUF_LEN 32

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/alternative.h>
#include <avahi-common/domain.h>

#include <pipewire/extensions/metadata.h>

#include "avahi-poll.h"
#include "common.h"
#include "peer-device.h"

/* Metadata key prefix for runtime per-peer-card toggles.
 *   pipewire-net-zeroconf.discover.<peer-host>.<peer-node>.enabled = "true"|"false"
 * Subject = 0. Default (key absent) = enabled.
 */
#define PWNZ_META_PREFIX        "pipewire-net-zeroconf.discover."
#define PWNZ_META_SUFFIX_EN     ".enabled"
#define PWNZ_META_SUFFIX_PROF   ".profile"
/* (Variant 4 used PWNZ_META_SUFFIX; that constant stayed for the
 * boolean enable key. Profile string key added in Stage 24 follow-up.) */
#define PWNZ_META_SUFFIX        PWNZ_META_SUFFIX_EN

#define NAME "mdns-rtp-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE \
	"( discover.sink=<bool, default true> ) " \
	"( discover.source=<bool, default false> ) " \
	"( discover.local=<bool, default false> ) " \
	"( discover.protocol=<ipv4|ipv6|any, default ipv4> ) "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "pipewire-net-zeroconf" },
	{ PW_KEY_MODULE_DESCRIPTION, "Discover remote PipeWire audio over mDNS+RTP" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl;
struct peer_card;

/* One Avahi service = one direction (sink OR source) of a peer card. */
struct tunnel {
	struct spa_list link;            /* into peer_card::tunnels */
	struct spa_list impl_link;       /* into impl::tunnels (flat lookup) */
	struct peer_card *card;

	char *avahi_name;    /* Avahi service instance name */
	char *peer_node;     /* TXT node-name */
	bool  is_sink_mode;  /* TXT mode=sink → output direction */

	/* Cached resolution payload so we can (re)create the rtp module
	 * later when the profile flips back on. */
	char *peer_addr;
	uint16_t peer_port;
	char *rate;
	char *channels;
	char *format;
	char *codec;
	char *description;
	char *transport;
	char *multicast_ip;
	char *multicast_ttl;

	struct pw_impl_module *rtp_mod;
	struct spa_hook rtp_mod_listener;
};

/* Aggregates all directions of one peer card into a single PipeWire
 * Audio/Device with a direction-aware profile dropdown
 * (Off / Output / Output+Input / Input as available). */
struct peer_card {
	struct spa_list link;
	struct impl *impl;

	char *peer_host;     /* TXT host */
	char *card_name;     /* TXT card-name (grouping key) */
	char *description;   /* user-facing label */

	uint32_t available_dirs;   /* OR of PEER_DIR_* the card actually offers */
	uint32_t loaded_dirs;      /* OR of PEER_DIR_* we've already attached
				    * a tunnel for; used to dedup duplicate
				    * service announcements (IPv4+IPv6, multi-
				    * NIC, bridge etc) before any tunnel /
				    * peer_device state is created — the
				    * tunnel-list check fired too late since
				    * the new tunnel is appended below. */

	struct spa_list tunnels;   /* of struct tunnel via tunnel::link */
	struct peer_device *device;

	/* When the user picks an Input profile for this card AND the peer
	 * publishes its source-direction as unicast, we publish a
	 * back-channel request via Avahi so the speaker knows to start
	 * sending. The request stays up until the profile leaves Input. */
	AvahiEntryGroup *req_entry_group;
	uint16_t req_port;            /* local UDP port advertised in request */
	char req_service_name[128];
	int req_collision_count;
};

struct meta_entry {
	struct spa_list link;
	char *key_tail;   /* peer-host.peer-card */
	bool  enabled_set;
	bool  enabled;
	bool  profile_set;
	char *profile_str;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *props;

	/* PW client + registry + default metadata (bound lazily). */
	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_proxy *metadata;
	struct spa_hook metadata_listener;
	uint32_t metadata_id;
	struct spa_list meta_entries;

	AvahiPoll *avahi_poll;
	AvahiClient *avahi_client;
	AvahiServiceBrowser *browser;

	bool discover_sink;
	bool discover_source;
	bool discover_local;
	AvahiProtocol discover_protocol;  /* AVAHI_PROTO_INET | INET6 | UNSPEC */

	double   ptime_ms;
	uint32_t latency_ms;

	char host_name[256];
	uint16_t req_next_port;    /* port-pool head for back-channel requests */

	struct spa_list tunnels;  /* flat list, impl_link */
	struct spa_list cards;    /* of struct peer_card */
};

static int start_avahi_client(struct impl *impl);
static void tunnel_free(struct tunnel *t);
static void peer_card_free(struct peer_card *c);
static int load_local_endpoint(struct tunnel *t,
			       const char *peer_addr, uint16_t peer_port,
			       const char *rate, const char *channels,
			       const char *format, const char *codec,
			       const char *description,
			       const char *transport,
			       const char *multicast_ip,
			       const char *multicast_ttl);

static struct peer_card *find_card(struct impl *impl,
				   const char *peer_host, const char *card_name)
{
	struct peer_card *c;
	spa_list_for_each(c, &impl->cards, link) {
		if (spa_streq(c->peer_host, peer_host) &&
		    spa_streq(c->card_name, card_name))
			return c;
	}
	return NULL;
}

/* -- helper: derive metadata-key-tail and lookup --------------------- */

static char *make_key_tail(const char *peer_host, const char *peer_node)
{
	char *out = NULL;
	if (asprintf(&out, "%s.%s", peer_host, peer_node) < 0)
		return NULL;
	return out;
}

static struct meta_entry *meta_find(struct impl *impl, const char *key_tail)
{
	struct meta_entry *e;
	spa_list_for_each(e, &impl->meta_entries, link)
		if (spa_streq(e->key_tail, key_tail))
			return e;
	return NULL;
}

static bool is_enabled(struct impl *impl, const char *peer_host, const char *card_name)
{
	char *kt = make_key_tail(peer_host, card_name);
	if (kt == NULL)
		return true;
	struct meta_entry *e = meta_find(impl, kt);
	free(kt);
	return e == NULL || !e->enabled_set ? true : e->enabled;
}

/* Returns -1 if no stored profile; otherwise a bitmask of PEER_DIR_*. */
static int stored_profile(struct impl *impl, const char *peer_host,
			  const char *card_name)
{
	char *kt = make_key_tail(peer_host, card_name);
	if (kt == NULL)
		return -1;
	struct meta_entry *e = meta_find(impl, kt);
	free(kt);
	if (e == NULL || !e->profile_set || e->profile_str == NULL)
		return -1;
	if (spa_streq(e->profile_str, "off"))          return 0;
	if (spa_streq(e->profile_str, "output"))       return PEER_DIR_OUTPUT;
	if (spa_streq(e->profile_str, "input"))        return PEER_DIR_INPUT;
	if (spa_streq(e->profile_str, "output+input")) return PEER_DIR_OUTPUT | PEER_DIR_INPUT;
	return -1;
}

static void meta_entry_free(struct meta_entry *e)
{
	spa_list_remove(&e->link);
	free(e->key_tail);
	free(e->profile_str);
	free(e);
}

static const char *dirs_to_profile_name(uint32_t dirs)
{
	switch (dirs) {
	case 0:                                   return "off";
	case PEER_DIR_OUTPUT:                     return "output";
	case PEER_DIR_INPUT:                      return "input";
	case PEER_DIR_OUTPUT | PEER_DIR_INPUT:    return "output+input";
	default: return NULL;
	}
}

/* Write our chosen profile to PW metadata so it survives module reload
 * (or peer-down/up). No-op if metadata not bound (= no WP running). */
static void persist_profile(struct impl *impl,
			    const char *peer_host, const char *card_name,
			    uint32_t active_dirs)
{
	if (impl->metadata == NULL)
		return;
	const char *name = dirs_to_profile_name(active_dirs);
	if (name == NULL)
		return;
	char key[256];
	snprintf(key, sizeof(key), "%s%s.%s%s",
		 PWNZ_META_PREFIX, peer_host, card_name, PWNZ_META_SUFFIX_PROF);
	pw_metadata_set_property((struct pw_metadata *) impl->metadata,
				 PW_ID_CORE, key, "Spa:String:JSON", name);
}

/* -- helpers ----------------------------------------------------------- */

static struct tunnel *find_tunnel(struct impl *impl, const char *avahi_name)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnels, impl_link)
		if (spa_streq(t->avahi_name, avahi_name))
			return t;
	return NULL;
}

/* FNV-1a 32-bit, used to derive a stable RTP SSRC from peer identity so
 * sender restarts don't churn the SSRC and trigger receiver-side drops. */
static uint32_t fnv1a_32(const char *s1, const char *s2)
{
	uint32_t h = 0x811c9dc5u;
	for (const char *p = s1; p && *p; p++)
		h = (h ^ (uint8_t)*p) * 0x01000193u;
	h = (h ^ (uint8_t)'/') * 0x01000193u;
	for (const char *p = s2; p && *p; p++)
		h = (h ^ (uint8_t)*p) * 0x01000193u;
	/* Avoid 0 — some RTP receivers treat 0 as "unset". */
	if (h == 0)
		h = 1;
	return h;
}

static char *sanitize_for_node_name(const char *s)
{
	char *out = strdup(s);
	if (!out)
		return NULL;
	for (char *p = out; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') ||
		      (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') ||
		      *p == '.' || *p == '_' || *p == '-'))
			*p = '_';
	}
	return out;
}

/* -- child module loading --------------------------------------------- */

static void unload_local_endpoint(struct tunnel *t)
{
	if (t->rtp_mod) {
		spa_hook_remove(&t->rtp_mod_listener);
		pw_impl_module_destroy(t->rtp_mod);
		t->rtp_mod = NULL;
	}
}

/* Bring the rtp child module into the requested state. */
static void tunnel_set_loaded(struct tunnel *t, bool loaded)
{
	bool have = t->rtp_mod != NULL;
	if (loaded == have)
		return;
	if (loaded) {
		if (t->peer_addr)
			load_local_endpoint(t, t->peer_addr, t->peer_port,
					    t->rate, t->channels, t->format,
					    t->codec, t->description,
					    t->transport, t->multicast_ip,
					    t->multicast_ttl);
	} else {
		unload_local_endpoint(t);
	}
}

/* -- Avahi request-record (Stage 24b unicast back-channel) ---------- */

static void req_entry_group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state,
			       void *userdata)
{
	struct peer_card *c = userdata;
	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		pw_log_info("request '%s' established", c->req_service_name);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION: {
		char *alt = avahi_alternative_service_name(c->req_service_name);
		if (alt) {
			snprintf(c->req_service_name, sizeof(c->req_service_name),
				 "%s", alt);
			avahi_free(alt);
			if (++c->req_collision_count < 8) {
				avahi_entry_group_reset(g);
				/* re-add deferred — caller will republish */
			}
		}
		break;
	}
	case AVAHI_ENTRY_GROUP_FAILURE:
		pw_log_error("request entry group failure: %s",
			     avahi_strerror(avahi_client_errno(
					     avahi_entry_group_get_client(g))));
		break;
	default:
		break;
	}
}

static int card_publish_request(struct peer_card *c)
{
	struct impl *impl = c->impl;

	if (impl->avahi_client == NULL ||
	    avahi_client_get_state(impl->avahi_client) != AVAHI_CLIENT_S_RUNNING)
		return 0;

	if (c->req_entry_group != NULL)
		return 0;  /* already up */

	if (c->req_port == 0)
		c->req_port = impl->req_next_port++;

	/* Avahi service names are limited to ~63 bytes, so we can't paste
	 * the full card_name in. The old "%.20s: req %.20s:%.20s" truncation
	 * collapsed cards like alsa_card.pci-0000_06_00.6 and ...03_00.0 to
	 * the same prefix → Avahi returned Local name collision and the
	 * second card's request was never published. Use an FNV hash of
	 * (peer_host, card_name) to guarantee uniqueness while still
	 * keeping a short readable prefix for avahi-browse. */
	uint32_t h = fnv1a_32(c->peer_host, c->card_name);
	snprintf(c->req_service_name, sizeof(c->req_service_name),
		 "%.16s: req %.10s %08x", impl->host_name, c->peer_host, h);

	c->req_entry_group = avahi_entry_group_new(impl->avahi_client,
						   req_entry_group_cb, c);
	if (c->req_entry_group == NULL) {
		pw_log_error("avahi_entry_group_new (req): %s",
			     avahi_strerror(avahi_client_errno(impl->avahi_client)));
		return -1;
	}

	AvahiStringList *t = NULL;
	char buf[32];
	t = avahi_string_list_add_pair(t, PWNZ_TXT_HOST, impl->host_name);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_TARGET_HOST, c->peer_host);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_TARGET_CARD, c->card_name);
	snprintf(buf, sizeof(buf), "%u", impl->latency_ms);
	t = avahi_string_list_add_pair(t, "latency-msec", buf);

	int err = avahi_entry_group_add_service_strlst(
		c->req_entry_group,
		AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0,
		c->req_service_name,
		PWNZ_REQUEST_SERVICE_TYPE,
		NULL, NULL, c->req_port, t);
	avahi_string_list_free(t);
	if (err < 0) {
		pw_log_error("avahi_entry_group_add_service (req): %s",
			     avahi_strerror(err));
		avahi_entry_group_free(c->req_entry_group);
		c->req_entry_group = NULL;
		return -1;
	}
	if ((err = avahi_entry_group_commit(c->req_entry_group)) < 0) {
		pw_log_error("avahi_entry_group_commit (req): %s",
			     avahi_strerror(err));
		avahi_entry_group_free(c->req_entry_group);
		c->req_entry_group = NULL;
		return -1;
	}
	pw_log_info("published request '%s' port=%u (asking %s:%s)",
		    c->req_service_name, c->req_port, c->peer_host, c->card_name);
	return 0;
}

static void card_withdraw_request(struct peer_card *c)
{
	if (c->req_entry_group) {
		avahi_entry_group_free(c->req_entry_group);
		c->req_entry_group = NULL;
		pw_log_info("withdrew request '%s'", c->req_service_name);
	}
}

/* Should this card maintain an outgoing request right now? Only if
 * Input direction is active AND at least one source-mode tunnel uses
 * unicast transport (peer's announcement). */
static bool card_needs_request(struct peer_card *c)
{
	uint32_t active = c->device ? peer_device_get_active_dirs(c->device) : 0;
	if (!(active & PEER_DIR_INPUT))
		return false;
	struct tunnel *t;
	spa_list_for_each(t, &c->tunnels, link) {
		if (t->is_sink_mode)
			continue;
		bool is_mc = t->transport && spa_streq(t->transport, "multicast");
		if (!is_mc)
			return true;
	}
	return false;
}

static void card_update_request(struct peer_card *c)
{
	if (card_needs_request(c))
		card_publish_request(c);
	else
		card_withdraw_request(c);
}

/* Called by peer_device when the user picks a new profile via
 * pavucontrol / pw-cli set-param. active_dirs is the bitmask of the
 * directions the user wants on: 0 = Off, OUTPUT = playback only,
 * OUTPUT|INPUT = duplex, etc. Load/unload each tunnel based on its
 * direction matching the mask. */
static int card_profile_cb(void *user_data, uint32_t active_dirs)
{
	struct peer_card *c = user_data;
	struct tunnel *t;
	pw_log_info("profile change for card '%s/%s' → 0x%x (output=%d input=%d)",
		    c->peer_host, c->card_name, active_dirs,
		    !!(active_dirs & PEER_DIR_OUTPUT),
		    !!(active_dirs & PEER_DIR_INPUT));
	spa_list_for_each(t, &c->tunnels, link) {
		uint32_t need = t->is_sink_mode ? PEER_DIR_OUTPUT : PEER_DIR_INPUT;
		tunnel_set_loaded(t, (active_dirs & need) != 0);
	}
	card_update_request(c);
	persist_profile(c->impl, c->peer_host, c->card_name, active_dirs);
	return 0;
}

static uint32_t card_default_dirs(struct peer_card *c)
{
	struct impl *impl = c->impl;
	uint32_t want = 0;
	if (impl->discover_sink   && (c->available_dirs & PEER_DIR_OUTPUT))
		want |= PEER_DIR_OUTPUT;
	if (impl->discover_source && (c->available_dirs & PEER_DIR_INPUT))
		want |= PEER_DIR_INPUT;
	return want;
}

static void card_apply_state(struct peer_card *c)
{
	bool en = is_enabled(c->impl, c->peer_host, c->card_name);
	uint32_t want = en ? card_default_dirs(c) : 0;
	if (c->device)
		peer_device_set_active_dirs(c->device, want);
	else {
		struct tunnel *t;
		spa_list_for_each(t, &c->tunnels, link) {
			uint32_t need = t->is_sink_mode ? PEER_DIR_OUTPUT : PEER_DIR_INPUT;
			tunnel_set_loaded(t, (want & need) != 0);
		}
	}
}

static void rtp_mod_destroyed(void *data)
{
	struct tunnel *t = data;
	spa_hook_remove(&t->rtp_mod_listener);
	t->rtp_mod = NULL;
}

static const struct pw_impl_module_events rtp_mod_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = rtp_mod_destroyed,
};

static int load_local_endpoint(struct tunnel *t,
			       const char *peer_addr, uint16_t peer_port,
			       const char *rate, const char *channels,
			       const char *format, const char *codec,
			       const char *description,
			       const char *transport,
			       const char *multicast_ip,
			       const char *multicast_ttl)
{
	FILE *f;
	char *args;
	size_t size;
	const char *child_mod;
	bool is_multicast = transport && spa_streq(transport, "multicast")
				&& multicast_ip != NULL;
	const char *net_addr = is_multicast ? multicast_ip : peer_addr;
	char *peer_host_safe = sanitize_for_node_name(t->card->peer_host);
	char *peer_node_safe = sanitize_for_node_name(t->peer_node);

	if ((f = open_memstream(&args, &size)) == NULL) {
		free(peer_host_safe);
		free(peer_node_safe);
		return -errno;
	}

	uint32_t ssrc = fnv1a_32(t->card->peer_host, t->peer_node);

	char pbuf[PTIME_BUF_LEN];
	spa_dtoa(pbuf, sizeof(pbuf), t->card->impl->ptime_ms);

	if (t->is_sink_mode) {
		child_mod = "libpipewire-module-rtp-sink";
		fprintf(f, "{ ");
		fprintf(f, "destination.ip = \"%s\" ", net_addr);
		fprintf(f, "destination.port = %u ", peer_port);
		fprintf(f, "sess.latency.msec = %u ", t->card->impl->latency_ms);
		if (is_multicast && multicast_ttl)
			fprintf(f, "net.ttl = %s ", multicast_ttl);
		fprintf(f, "sess.media = \"%s\" ",
			spa_streq(codec, "opus") ? "opus" : "audio");
		fprintf(f, "audio.format = \"%s\" ", format);
		fprintf(f, "audio.rate = %s ", rate);
		fprintf(f, "audio.channels = %s ", channels);
		fprintf(f, "stream.props = { ");
		fprintf(f, "node.name = \"network.%s.%s\" ",
			peer_host_safe, peer_node_safe);
		fprintf(f, "node.description = \"Network: %s: %s\" ",
			t->card->peer_host, description);
		fprintf(f, "media.class = \"Audio/Sink\" ");
		fprintf(f, "node.network = true ");
		fprintf(f, "node.virtual = true ");
		/* Stop sending packets when no client is playing into the
		 * virtual sink — saves LAN bandwidth, matches PA-tunnel
		 * idle behaviour. Receiver pauses via stream.may-pause. */
		fprintf(f, "node.always-process = false ");
		/* PW assigns a driver clock to every Audio/Sink by default, so
		 * the rtp-sink process callback fires even with no client →
		 * silent packets flood the network. suspend-on-idle = true lets
		 * the session manager actually park the node and stop the driver
		 * tick after a brief idle period. */
		fprintf(f, "node.suspend-on-idle = true ");
		/* Don't let WP auto-link other monitors/fallbacks into us as
		 * passive sources — we should only push when a real client
		 * targets this sink. */
		fprintf(f, "node.passive = true ");
		fprintf(f, "node.dont-fallback = true ");
		fprintf(f, "rtp.ptime = %s ", pbuf);
		fprintf(f, "rtp.sender-ssrc = %u ", ssrc);
		fprintf(f, "} }");
	} else {
		child_mod = "libpipewire-module-rtp-source";
		fprintf(f, "{ ");
		fprintf(f, "source.ip = \"%s\" ", net_addr);
		fprintf(f, "source.port = %u ", peer_port);
		fprintf(f, "sess.latency.msec = %u ", t->card->impl->latency_ms);
		fprintf(f, "sess.media = \"%s\" ",
			spa_streq(codec, "opus") ? "opus" : "audio");
		fprintf(f, "audio.format = \"%s\" ", format);
		fprintf(f, "audio.rate = %s ", rate);
		fprintf(f, "audio.channels = %s ", channels);
		fprintf(f, "stream.props = { ");
		fprintf(f, "node.name = \"network.%s.%s\" ",
			peer_host_safe, peer_node_safe);
		fprintf(f, "node.description = \"Network: %s: %s\" ",
			t->card->peer_host, description);
		fprintf(f, "media.class = \"Audio/Source\" ");
		fprintf(f, "node.network = true ");
		fprintf(f, "node.virtual = true ");
		fprintf(f, "stream.may-pause = true ");
		fprintf(f, "rtp.ptime = %s ", pbuf);
		fprintf(f, "rtp.receiver-ssrc = %u ", ssrc);
		fprintf(f, "} }");
	}
	fclose(f);

	pw_log_debug("loading %s: %s", child_mod, args);

	t->rtp_mod = pw_context_load_module(t->card->impl->context,
					    child_mod, args, NULL);
	free(args);
	free(peer_host_safe);
	free(peer_node_safe);

	if (t->rtp_mod == NULL) {
		pw_log_error("failed to load %s: %m", child_mod);
		return -errno;
	}
	pw_impl_module_add_listener(t->rtp_mod, &t->rtp_mod_listener,
				    &rtp_mod_events, t);
	return 0;
}

/* -- Avahi resolver --------------------------------------------------- */

struct txt_kv {
	char *node_name, *card_name, *peer_host, *description, *mode;
	char *rate, *channels, *format, *codec;
	char *transport, *mcast_ip, *mcast_ttl;
};

static void txt_kv_free(struct txt_kv *kv)
{
	avahi_free(kv->node_name);
	avahi_free(kv->card_name);
	avahi_free(kv->peer_host);
	avahi_free(kv->description);
	avahi_free(kv->mode);
	avahi_free(kv->rate);
	avahi_free(kv->channels);
	avahi_free(kv->format);
	avahi_free(kv->codec);
	avahi_free(kv->transport);
	avahi_free(kv->mcast_ip);
	avahi_free(kv->mcast_ttl);
}

static void resolver_cb(AvahiServiceResolver *r,
			AvahiIfIndex interface SPA_UNUSED,
			AvahiProtocol protocol SPA_UNUSED,
			AvahiResolverEvent event,
			const char *name,
			const char *type SPA_UNUSED,
			const char *domain SPA_UNUSED,
			const char *host_name,
			const AvahiAddress *a,
			uint16_t port,
			AvahiStringList *txt,
			AvahiLookupResultFlags flags SPA_UNUSED,
			void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel *t = NULL;
	AvahiStringList *l;
	struct txt_kv kv = {0};
	char addr_buf[AVAHI_ADDRESS_STR_MAX];

	if (event != AVAHI_RESOLVER_FOUND) {
		pw_log_warn("resolve '%s' failed: %s", name,
			    avahi_strerror(avahi_client_errno(impl->avahi_client)));
		goto done;
	}

	for (l = txt; l; l = l->next) {
		char *key = NULL, *value = NULL;
		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0) {
			avahi_free(key);
			avahi_free(value);
			break;
		}
		char **slot = NULL;
		if      (spa_streq(key, PWNZ_TXT_NODE_NAME))    slot = &kv.node_name;
		else if (spa_streq(key, PWNZ_TXT_CARD_NAME))    slot = &kv.card_name;
		else if (spa_streq(key, PWNZ_TXT_HOST))         slot = &kv.peer_host;
		else if (spa_streq(key, PWNZ_TXT_DESCRIPTION))  slot = &kv.description;
		else if (spa_streq(key, PWNZ_TXT_MODE))         slot = &kv.mode;
		else if (spa_streq(key, PWNZ_TXT_RATE))         slot = &kv.rate;
		else if (spa_streq(key, PWNZ_TXT_CHANNELS))     slot = &kv.channels;
		else if (spa_streq(key, PWNZ_TXT_FORMAT))       slot = &kv.format;
		else if (spa_streq(key, PWNZ_TXT_CODEC))        slot = &kv.codec;
		else if (spa_streq(key, PWNZ_TXT_TRANSPORT))    slot = &kv.transport;
		else if (spa_streq(key, PWNZ_TXT_MCAST_IP))     slot = &kv.mcast_ip;
		else if (spa_streq(key, PWNZ_TXT_MCAST_TTL))    slot = &kv.mcast_ttl;

		if (slot && *slot == NULL) {
			*slot = value;
			value = NULL;
		}
		avahi_free(key);
		avahi_free(value);
	}

	if (kv.node_name == NULL || kv.mode == NULL) {
		pw_log_warn("service '%s' missing required TXT records", name);
		goto done;
	}

	bool is_sink_mode = spa_streq(kv.mode, "sink");
	if (is_sink_mode && !impl->discover_sink)
		goto done;
	if (!is_sink_mode && !impl->discover_source)
		goto done;

	t = find_tunnel(impl, name);
	if (t != NULL && t->rtp_mod != NULL) {
		pw_log_debug("'%s' already tracked", name);
		goto done;
	}

	const char *peer_host = kv.peer_host ? kv.peer_host : host_name;
	const char *peer_card_name = kv.card_name ? kv.card_name : kv.node_name;
	uint32_t new_dir = is_sink_mode ? PEER_DIR_OUTPUT : PEER_DIR_INPUT;

	/* Dedup by (peer_host, card_name, direction): same service announced
	 * on multiple local interfaces or both IPv4 and IPv6 hits
	 * resolver_cb several times with different avahi service names but
	 * identical TXT identity. We use card->loaded_dirs (set BEFORE any
	 * tunnel/peer_device creation, so it's visible to the second event
	 * even if the first hasn't finished load_local_endpoint yet). */
	{
		struct peer_card *dup_card = find_card(impl, peer_host,
						       peer_card_name);
		if (dup_card != NULL && (dup_card->loaded_dirs & new_dir)) {
			pw_log_info("dup service '%s' for %s/%s dir=%s — ignoring",
				    name, peer_host, peer_card_name,
				    is_sink_mode ? "sink" : "source");
			goto done;
		}
	}

	/* Find or create the per-card aggregation. */
	struct peer_card *card = find_card(impl, peer_host, peer_card_name);
	bool card_is_new = (card == NULL);
	if (card_is_new) {
		card = calloc(1, sizeof(*card));
		if (card == NULL)
			goto done;
		card->impl = impl;
		card->peer_host = strdup(peer_host);
		card->card_name = strdup(peer_card_name);
		card->description = strdup(kv.description ? kv.description :
					   peer_card_name);
		spa_list_init(&card->tunnels);
		spa_list_append(&impl->cards, &card->link);
	}

	uint32_t new_avail = card->available_dirs | new_dir;
	bool avail_changed = (new_avail != card->available_dirs);
	card->available_dirs = new_avail;

	if (card_is_new) {
		char node_name[256];
		char *peer_host_safe = sanitize_for_node_name(card->peer_host);
		char *card_safe = sanitize_for_node_name(card->card_name);
		snprintf(node_name, sizeof(node_name), "network.%s.%s",
			 peer_host_safe ? peer_host_safe : card->peer_host,
			 card_safe ? card_safe : card->card_name);
		free(peer_host_safe);
		free(card_safe);

		char description[512];
		snprintf(description, sizeof(description), "Network: %s: %s",
			 card->peer_host, card->description);

		uint32_t initial_dirs;
		int saved = stored_profile(impl, card->peer_host, card->card_name);
		if (saved >= 0) {
			initial_dirs = (uint32_t) saved & card->available_dirs;
		} else {
			initial_dirs = is_enabled(impl, card->peer_host,
						  card->card_name)
				       ? card_default_dirs(card) : 0;
		}
		struct peer_device_info pdi = {
			.node_name        = node_name,
			.node_description = description,
			.peer_host        = card->peer_host,
			.available_dirs   = card->available_dirs,
			.default_dirs     = initial_dirs,
		};
		card->device = peer_device_new(impl->context, &pdi,
					       card_profile_cb, card);
		if (card->device == NULL)
			pw_log_warn("peer_device_new failed for card '%s/%s': %m",
				    card->peer_host, card->card_name);
	} else if (avail_changed && card->device) {
		/* second direction service appeared — refresh profile list AND
		 * re-apply the desired profile so a peer_card whose Input
		 * arrived after its Output (or vice versa) grows active_dirs
		 * to include the now-available second direction. Without
		 * this, the initial default — computed when only one
		 * direction was visible — stays in place forever. */
		peer_device_set_available_dirs(card->device, card->available_dirs);

		int saved = stored_profile(impl, card->peer_host, card->card_name);
		uint32_t desired;
		if (saved >= 0) {
			desired = (uint32_t) saved & card->available_dirs;
		} else {
			desired = is_enabled(impl, card->peer_host, card->card_name)
				  ? card_default_dirs(card) : 0;
		}
		peer_device_set_active_dirs(card->device, desired);
	}

	if (t == NULL) {
		t = calloc(1, sizeof(*t));
		if (t == NULL)
			goto done;
		t->card = card;
		t->avahi_name = strdup(name);
		spa_list_append(&impl->tunnels, &t->impl_link);
		spa_list_append(&card->tunnels, &t->link);
	}
	t->is_sink_mode = is_sink_mode;
	card->loaded_dirs |= new_dir;
	free(t->peer_node);
	t->peer_node = strdup(kv.node_name);

	avahi_address_snprint(addr_buf, sizeof(addr_buf), a);

	/* Cache resolution payload so the metadata-toggle path can re-load
	 * later. Old values freed first. */
	#define REPLACE_OPT(field, src) do { free(t->field); t->field = (src) ? strdup(src) : NULL; } while (0)
	#define REPLACE(field, src)     do { free(t->field); t->field = strdup(src); } while (0)
	REPLACE(peer_addr,    addr_buf);
	REPLACE(rate,         kv.rate     ? kv.rate     : "48000");
	REPLACE(channels,     kv.channels ? kv.channels : "2");
	REPLACE(format,       kv.format   ? kv.format   : PWNZ_DEFAULT_FORMAT);
	REPLACE(codec,        kv.codec    ? kv.codec    : PWNZ_DEFAULT_CODEC);
	REPLACE(description,  kv.description ? kv.description : kv.node_name);
	REPLACE_OPT(transport,    kv.transport);
	REPLACE_OPT(multicast_ip, kv.mcast_ip);
	REPLACE_OPT(multicast_ttl, kv.mcast_ttl);
	#undef REPLACE
	#undef REPLACE_OPT
	t->peer_port = port;

	/* Apply current desired state for this direction. Profile-on +
	 * direction in active_dirs ⇒ load, else don't. */
	if (card->device) {
		uint32_t active = peer_device_get_active_dirs(card->device);
		tunnel_set_loaded(t, (active & new_dir) != 0);
	} else if (is_enabled(impl, card->peer_host, card->card_name)) {
		tunnel_set_loaded(t, true);
	}

	/* Publish/withdraw the unicast back-channel request based on
	 * whether the active profile asks for the peer's mic. */
	card_update_request(card);

done:
	txt_kv_free(&kv);
	avahi_service_resolver_free(r);
}

/* -- Avahi browser ---------------------------------------------------- */

static void browser_cb(AvahiServiceBrowser *b SPA_UNUSED,
		       AvahiIfIndex interface, AvahiProtocol protocol,
		       AvahiBrowserEvent event,
		       const char *name, const char *type, const char *domain,
		       AvahiLookupResultFlags flags,
		       void *userdata)
{
	struct impl *impl = userdata;

	if ((flags & AVAHI_LOOKUP_RESULT_LOCAL) && !impl->discover_local)
		return;

	switch (event) {
	case AVAHI_BROWSER_NEW:
		pw_log_info("BROWSER_NEW '%s'", name);
		if (find_tunnel(impl, name) != NULL) {
			pw_log_debug("duplicate, skipping");
			return;
		}
		if (avahi_service_resolver_new(impl->avahi_client,
					       interface, protocol,
					       name, type, domain,
					       impl->discover_protocol, 0,
					       resolver_cb, impl) == NULL) {
			pw_log_error("avahi_service_resolver_new: %s",
				     avahi_strerror(avahi_client_errno(impl->avahi_client)));
		}
		break;
	case AVAHI_BROWSER_REMOVE: {
		pw_log_info("BROWSER_REMOVE '%s'", name);
		struct tunnel *t = find_tunnel(impl, name);
		if (t)
			tunnel_free(t);
		break;
	}
	case AVAHI_BROWSER_FAILURE:
		pw_log_error("Avahi browser failure: %s",
			     avahi_strerror(avahi_client_errno(impl->avahi_client)));
		break;
	default:
		break;
	}
}

static void tunnel_free(struct tunnel *t)
{
	struct peer_card *card = t->card;
	if (card != NULL) {
		uint32_t bit = t->is_sink_mode ? PEER_DIR_OUTPUT : PEER_DIR_INPUT;
		card->loaded_dirs &= ~bit;
	}
	spa_list_remove(&t->link);        /* off peer_card::tunnels */
	spa_list_remove(&t->impl_link);   /* off impl::tunnels */
	unload_local_endpoint(t);
	free(t->avahi_name);
	free(t->peer_node);
	free(t->peer_addr);
	free(t->rate);
	free(t->channels);
	free(t->format);
	free(t->codec);
	free(t->description);
	free(t->transport);
	free(t->multicast_ip);
	free(t->multicast_ttl);
	free(t);

	if (card == NULL)
		return;
	if (spa_list_is_empty(&card->tunnels)) {
		peer_card_free(card);
	} else {
		/* Recompute available_dirs from whatever remains. */
		uint32_t avail = 0;
		struct tunnel *r;
		spa_list_for_each(r, &card->tunnels, link)
			avail |= r->is_sink_mode ? PEER_DIR_OUTPUT : PEER_DIR_INPUT;
		if (avail != card->available_dirs) {
			card->available_dirs = avail;
			if (card->device)
				peer_device_set_available_dirs(card->device, avail);
		}
	}
}

static void peer_card_free(struct peer_card *c)
{
	if (c == NULL)
		return;
	card_withdraw_request(c);
	struct tunnel *t;
	spa_list_consume(t, &c->tunnels, link) {
		t->card = NULL;          /* prevent recursion via tunnel_free */
		spa_list_remove(&t->impl_link);
		unload_local_endpoint(t);
		free(t->avahi_name);
		free(t->peer_node);
		free(t->peer_addr);
		free(t->rate);
		free(t->channels);
		free(t->format);
		free(t->codec);
		free(t->description);
		free(t->transport);
		free(t->multicast_ip);
		free(t->multicast_ttl);
		free(t);
	}
	if (c->device)
		peer_device_destroy(c->device);
	spa_list_remove(&c->link);
	free(c->peer_host);
	free(c->card_name);
	free(c->description);
	free(c);
}

/* -- Avahi client state ----------------------------------------------- */

static int make_browser(struct impl *impl)
{
	impl->browser = avahi_service_browser_new(impl->avahi_client,
						  AVAHI_IF_UNSPEC,
						  impl->discover_protocol,
						  PWNZ_SERVICE_TYPE, NULL, 0,
						  browser_cb, impl);
	if (impl->browser == NULL) {
		pw_log_error("avahi_service_browser_new: %s",
			     avahi_strerror(avahi_client_errno(impl->avahi_client)));
		return -EIO;
	}
	return 0;
}

static void avahi_client_cb(AvahiClient *c, AvahiClientState state, void *userdata)
{
	struct impl *impl = userdata;

	impl->avahi_client = c;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_S_COLLISION:
		if (impl->browser == NULL)
			make_browser(impl);
		break;
	case AVAHI_CLIENT_FAILURE:
		if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
			start_avahi_client(impl);
		SPA_FALLTHROUGH;
	case AVAHI_CLIENT_CONNECTING:
		if (impl->browser) {
			avahi_service_browser_free(impl->browser);
			impl->browser = NULL;
		}
		break;
	}
}

static int start_avahi_client(struct impl *impl)
{
	int err;
	impl->avahi_client = avahi_client_new(impl->avahi_poll,
					      AVAHI_CLIENT_NO_FAIL,
					      avahi_client_cb, impl, &err);
	if (impl->avahi_client == NULL) {
		pw_log_error("avahi_client_new: %s", avahi_strerror(err));
		return -EIO;
	}
	return 0;
}

/* -- metadata events -------------------------------------------------- */

/* (no longer needed — metadata path acts on peer_card via find_card) */

static int meta_property(void *data, uint32_t subject,
			 const char *key, const char *type SPA_UNUSED,
			 const char *value)
{
	struct impl *impl = data;

	if (subject != PW_ID_CORE || key == NULL)
		return 0;
	if (strncmp(key, PWNZ_META_PREFIX, sizeof(PWNZ_META_PREFIX) - 1) != 0)
		return 0;
	const char *tail = key + sizeof(PWNZ_META_PREFIX) - 1;
	size_t tail_len = strlen(tail);

	bool is_profile_key;
	size_t suf_len;
	if (tail_len >= sizeof(PWNZ_META_SUFFIX_PROF) - 1 &&
	    strcmp(tail + tail_len - (sizeof(PWNZ_META_SUFFIX_PROF) - 1),
		   PWNZ_META_SUFFIX_PROF) == 0) {
		is_profile_key = true;
		suf_len = sizeof(PWNZ_META_SUFFIX_PROF) - 1;
	} else if (tail_len >= sizeof(PWNZ_META_SUFFIX_EN) - 1 &&
		   strcmp(tail + tail_len - (sizeof(PWNZ_META_SUFFIX_EN) - 1),
			  PWNZ_META_SUFFIX_EN) == 0) {
		is_profile_key = false;
		suf_len = sizeof(PWNZ_META_SUFFIX_EN) - 1;
	} else {
		return 0;
	}

	size_t kt_len = tail_len - suf_len;
	char *kt = strndup(tail, kt_len);
	if (kt == NULL)
		return -ENOMEM;
	char *dot = strchr(kt, '.');
	if (dot == NULL) { free(kt); return 0; }

	struct meta_entry *e = meta_find(impl, kt);

	if (is_profile_key) {
		if (value == NULL) {
			if (e && e->profile_set) {
				pw_log_info("metadata profile cleared: %s", kt);
				e->profile_set = false;
				free(e->profile_str);
				e->profile_str = NULL;
			}
		} else {
			if (e == NULL) {
				e = calloc(1, sizeof(*e));
				if (e == NULL) { free(kt); return -ENOMEM; }
				e->key_tail = strdup(kt);
				spa_list_append(&impl->meta_entries, &e->link);
			}
			free(e->profile_str);
			e->profile_str = strdup(value);
			e->profile_set = true;
			pw_log_info("metadata profile: %s = %s", kt, value);
		}
	} else {
		if (value == NULL) {
			if (e && e->enabled_set) {
				pw_log_info("metadata enabled cleared: %s", kt);
				e->enabled_set = false;
			}
		} else {
			if (e == NULL) {
				e = calloc(1, sizeof(*e));
				if (e == NULL) { free(kt); return -ENOMEM; }
				e->key_tail = strdup(kt);
				spa_list_append(&impl->meta_entries, &e->link);
			}
			e->enabled_set = true;
			e->enabled = spa_atob(value);
			pw_log_info("metadata enabled: %s = %s", kt,
				    e->enabled ? "true" : "false");
		}
	}

	if (e && !e->enabled_set && !e->profile_set)
		meta_entry_free(e);

	*dot = '\0';
	const char *peer_host = kt;
	const char *card_name = dot + 1;
	struct peer_card *c = find_card(impl, peer_host, card_name);
	if (c != NULL) {
		if (is_profile_key && c->device) {
			int dirs = stored_profile(impl, peer_host, card_name);
			if (dirs >= 0)
				peer_device_set_active_dirs(c->device,
							    (uint32_t) dirs);
		} else {
			card_apply_state(c);
		}
	}

	free(kt);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = meta_property,
};

/* -- pw registry / core listener ------------------------------------- */

static void registry_global(void *data,
			    uint32_t id,
			    uint32_t permissions SPA_UNUSED,
			    const char *type,
			    uint32_t version SPA_UNUSED,
			    const struct spa_dict *props)
{
	struct impl *impl = data;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata) || props == NULL)
		return;
	if (impl->metadata != NULL)
		return;
	if (!spa_streq(spa_dict_lookup(props, "metadata.name"), "default"))
		return;

	impl->metadata = pw_registry_bind(impl->registry, id, type,
					  PW_VERSION_METADATA, 0);
	if (impl->metadata == NULL)
		return;
	impl->metadata_id = id;
	pw_metadata_add_listener((struct pw_metadata *) impl->metadata,
				 &impl->metadata_listener,
				 &metadata_events, impl);
	pw_log_info("bound default metadata id=%u", id);
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	if (id != impl->metadata_id || impl->metadata == NULL)
		return;
	pw_log_info("default metadata gone");
	spa_hook_remove(&impl->metadata_listener);
	pw_proxy_destroy(impl->metadata);
	impl->metadata = NULL;
	impl->metadata_id = SPA_ID_INVALID;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void on_core_error(void *data, uint32_t id, int seq, int res,
			  const char *message)
{
	struct impl *impl = data;
	pw_log_error("core error id:%u seq:%d res:%d: %s", id, seq, res, message);
	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_core_proxy_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->core_proxy_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_core_proxy_destroy,
};

/* -- module lifecycle ------------------------------------------------- */

static void impl_free(struct impl *impl)
{
	struct peer_card *c;
	struct meta_entry *e;

	spa_list_consume(c, &impl->cards, link)
		peer_card_free(c);
	spa_list_consume(e, &impl->meta_entries, link)
		meta_entry_free(e);

	if (impl->metadata) {
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy(impl->metadata);
		impl->metadata = NULL;
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy *) impl->registry);
	}
	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		pw_core_disconnect(impl->core);
	}

	if (impl->browser)
		avahi_service_browser_free(impl->browser);
	if (impl->avahi_client)
		avahi_client_free(impl->avahi_client);
	if (impl->avahi_poll)
		pw_avahi_poll_free(impl->avahi_poll);

	pw_properties_free(impl->props);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -errno;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->props = props;
	impl->metadata_id = SPA_ID_INVALID;
	impl->req_next_port = PWNZ_DEFAULT_PORT_BASE + PWNZ_DEFAULT_PORT_RANGE;
	spa_list_init(&impl->tunnels);
	spa_list_init(&impl->cards);
	spa_list_init(&impl->meta_entries);

	if (gethostname(impl->host_name, sizeof(impl->host_name)) < 0)
		snprintf(impl->host_name, sizeof(impl->host_name), "pipewire");
	impl->host_name[sizeof(impl->host_name) - 1] = '\0';

	impl->discover_sink   = pw_properties_get_bool(props, "discover.sink",   true);
	impl->discover_source = pw_properties_get_bool(props, "discover.source", false);
	impl->discover_local  = pw_properties_get_bool(props, "discover.local",  false);

	impl->latency_ms = pw_properties_get_uint32(props,
					"discover.latency.msec", PWNZ_DEFAULT_LATENCY_MS);
	const char *pt = pw_properties_get(props, "discover.ptime.msec");
	impl->ptime_ms = pt ? atof(pt) : PWNZ_DEFAULT_PTIME_MS;
	if (impl->ptime_ms <= 0)
		impl->ptime_ms = PWNZ_DEFAULT_PTIME_MS;

	const char *proto_str = pw_properties_get(props, "discover.protocol");
	if (proto_str == NULL || spa_streq(proto_str, "ipv4"))
		impl->discover_protocol = AVAHI_PROTO_INET;
	else if (spa_streq(proto_str, "ipv6"))
		impl->discover_protocol = AVAHI_PROTO_INET6;
	else
		impl->discover_protocol = AVAHI_PROTO_UNSPEC;

	pw_log_info("discover.sink=%d discover.source=%d discover.local=%d protocol=%s",
		    impl->discover_sink, impl->discover_source, impl->discover_local,
		    proto_str ? proto_str : "ipv4");

	pw_impl_module_add_listener(module, &impl->module_listener,
				    &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	/* Connect to PW + bind default metadata so we can listen for
	 * per-peer toggle keys (pipewire-net-zeroconf.discover.*.enabled).
	 * Failures here are non-fatal — the module still works without
	 * runtime toggle support. */
	impl->core = pw_context_connect(context, NULL, 0);
	if (impl->core != NULL) {
		pw_proxy_add_listener((struct pw_proxy *) impl->core,
				      &impl->core_proxy_listener,
				      &core_proxy_events, impl);
		pw_core_add_listener(impl->core, &impl->core_listener,
				     &core_events, impl);
		impl->registry = pw_core_get_registry(impl->core,
						      PW_VERSION_REGISTRY, 0);
		if (impl->registry != NULL)
			pw_registry_add_listener(impl->registry,
						 &impl->registry_listener,
						 &registry_events, impl);
	} else {
		pw_log_warn("pw_context_connect failed: %m — metadata toggles disabled");
	}

	impl->avahi_poll = pw_avahi_poll_new(context);
	if (impl->avahi_poll == NULL) {
		res = -ENOMEM;
		goto error;
	}

	if ((res = start_avahi_client(impl)) < 0)
		goto error;

	return 0;

error:
	impl_free(impl);
	return res;
}
