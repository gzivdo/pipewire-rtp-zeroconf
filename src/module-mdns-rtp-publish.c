/* SPDX-License-Identifier: MIT
 *
 * pipewire-net-zeroconf — mDNS/RTP publish module.
 *
 * Watches local PipeWire nodes and, for each Audio/Sink (and optionally
 * Audio/Source), allocates a UDP port, loads libpipewire-module-rtp-source
 * to receive RTP and route it into the local node, and publishes the
 * resulting endpoint via Avahi under _pipewire-rtp._udp.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/conf.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/alternative.h>
#include <avahi-common/domain.h>

#include <pipewire/extensions/metadata.h>

#include "avahi-poll.h"
#include "common.h"

/* Metadata key prefix for runtime per-card toggles.
 *   pipewire-net-zeroconf.publish.<node-name>.enabled = "true" | "false"
 * Subject = 0 (PW_ID_CORE). Default (key absent) = enabled.
 * Set / clear via standard tooling, e.g.:
 *   pw-metadata 0 "pipewire-net-zeroconf.publish.alsa_output.pci-0000_00_1f.3.analog-stereo.enabled" "false"
 *   pw-metadata -d 0 "pipewire-net-zeroconf.publish.alsa_output.pci-0000_00_1f.3.analog-stereo.enabled"
 */
#define PWNZ_META_PREFIX  "pipewire-net-zeroconf.publish."
#define PWNZ_META_SUFFIX  ".enabled"

#define NAME "mdns-rtp-publish"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE \
	"( publish.sink=<bool, default true> ) " \
	"( publish.source=<bool, default false> ) " \
	"( publish.rules=<match-action rules> ) " \
	"( publish.rate=<int, default 48000> ) " \
	"( publish.channels=<int, default 2> ) " \
	"( publish.format=<S16BE|S24BE|S32BE, default S16BE> ) " \
	"( publish.codec=<pcm|opus, default pcm> ) " \
	"( publish.port=<fixed port> ) " \
	"( publish.port.base=<int, default 46000> ) " \
	"( publish.port.range=<int, default 32> ) " \
	"( publish.transport=<unicast|multicast, default unicast> ) " \
	"( publish.multicast.ip=<group, default 224.0.0.56> ) " \
	"( publish.multicast.port=<int, default 5004> ) " \
	"( publish.multicast.ttl=<int, default 1> ) " \
	"( publish.multicast.loop=<bool, default false> ) " \
	"( publish.sap=<bool, default false> ) "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "pipewire-net-zeroconf" },
	{ PW_KEY_MODULE_DESCRIPTION, "Publish local audio nodes over mDNS+RTP" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl;

struct service {
	struct spa_list link;
	struct impl *impl;

	uint32_t node_id;
	char *node_name;
	char *node_description;
	bool is_sink;
	uint32_t channels;

	uint16_t port;
	char session_id[37]; /* uuid-ish */

	char service_name[AVAHI_LABEL_MAX];
	int name_collision_count;
	AvahiEntryGroup *entry_group;

	struct pw_impl_module *rtp_mod;
	struct spa_hook rtp_mod_listener;
};

struct meta_entry {
	struct spa_list link;
	char *node_name;     /* part between PREFIX and SUFFIX */
	bool enabled;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *props;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	/* "default" metadata for runtime toggle keys. */
	struct pw_proxy *metadata;
	struct spa_hook metadata_listener;
	uint32_t metadata_id;
	struct spa_list meta_entries;

	AvahiPoll *avahi_poll;
	AvahiClient *avahi_client;

	char host_name[HOST_NAME_MAX + 1];

	bool publish_sink;
	bool publish_source;
	char *publish_rules;          /* JSON match-rules, may be NULL */
	uint32_t rate;
	uint32_t channels;
	char *format;
	char *codec;

	bool multicast;
	char *multicast_ip;
	uint32_t multicast_ttl;
	uint16_t multicast_port;
	bool multicast_loop;

	double  ptime_ms;
	uint32_t latency_ms;

	bool port_fixed;
	uint16_t port_fixed_value;
	uint16_t port_base;
	uint16_t port_range;
	uint16_t next_port_offset;

	bool sap_enabled;
	struct pw_impl_module *sap_mod;
	struct spa_hook sap_mod_listener;

	struct spa_list services;
};

static int start_avahi_client(struct impl *impl);
static void service_free(struct service *s);
static int publish_service(struct service *s);
static void unpublish_service(struct service *s);
static void service_apply_state(struct service *s);

/* -- metadata cache --------------------------------------------------- */

static struct meta_entry *meta_find(struct impl *impl, const char *node_name)
{
	struct meta_entry *e;
	spa_list_for_each(e, &impl->meta_entries, link)
		if (spa_streq(e->node_name, node_name))
			return e;
	return NULL;
}

static bool is_enabled(struct impl *impl, const char *node_name)
{
	struct meta_entry *e = meta_find(impl, node_name);
	return e == NULL ? true : e->enabled;
}

static void meta_entry_free(struct meta_entry *e)
{
	spa_list_remove(&e->link);
	free(e->node_name);
	free(e);
}

static struct service *find_service_by_node_name(struct impl *impl, const char *name)
{
	struct service *s;
	spa_list_for_each(s, &impl->services, link)
		if (spa_streq(s->node_name, name))
			return s;
	return NULL;
}

static int allocate_port(struct impl *impl, uint16_t *out_port)
{
	if (impl->port_fixed) {
		/* fixed-port mode: only the first card gets it */
		if (impl->next_port_offset >= 1)
			return -EADDRINUSE;
		*out_port = impl->port_fixed_value;
		impl->next_port_offset = 1;
		return 0;
	}
	if (impl->next_port_offset >= impl->port_range)
		return -EADDRINUSE;
	uint16_t base = impl->multicast ? impl->multicast_port : impl->port_base;
	*out_port = base + impl->next_port_offset++;
	return 0;
}

/* -- helpers ----------------------------------------------------------- */

static void gen_session_id(char *buf, size_t buflen)
{
	uint32_t a = pw_rand32(), b = pw_rand32(), c = pw_rand32(), d = pw_rand32();
	snprintf(buf, buflen, "%08x-%04x-%04x-%04x-%08x%04x",
		 a, b & 0xffff, (b >> 16) & 0xffff,
		 c & 0xffff, (c >> 16), d & 0xffff);
}

static bool ends_with(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lt = strlen(suffix);
	return ls >= lt && strcmp(s + ls - lt, suffix) == 0;
}

/* -- registry event handling ------------------------------------------- */

static struct service *find_service_by_id(struct impl *impl, uint32_t id)
{
	struct service *s;
	spa_list_for_each(s, &impl->services, link)
		if (s->node_id == id)
			return s;
	return NULL;
}

/* match-rules callback: a rule matched, action is "publish" or "exclude".
 * Stores the decision in match_info and stops further evaluation by
 * leaving the loop on first match (callback is invoked per matching
 * rule; we just record the latest, but the spec says "first match wins"
 * — so we ignore subsequent matches by remembering matched=true). */
struct match_info {
	bool matched;
	bool publish;
};

static int rule_matched_cb(void *data, const char *location SPA_UNUSED,
			   const char *action,
			   const char *str SPA_UNUSED, size_t len SPA_UNUSED)
{
	struct match_info *mi = data;
	if (mi->matched)
		return 0;  /* first match wins */
	mi->matched = true;
	if (spa_streq(action, "publish"))
		mi->publish = true;
	else if (spa_streq(action, "exclude"))
		mi->publish = false;
	return 0;
}

static bool should_publish(struct impl *impl, const struct spa_dict *props,
			   bool *is_sink_out)
{
	const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	const char *node_name   = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	const char *device_api  = spa_dict_lookup(props, PW_KEY_DEVICE_API);
	const char *node_network = spa_dict_lookup(props, PW_KEY_NODE_NETWORK);

	if (!media_class || !node_name)
		return false;

	bool is_sink, is_source;
	if (spa_streq(media_class, "Audio/Sink"))
		is_sink = true, is_source = false;
	else if (spa_streq(media_class, "Audio/Source"))
		is_sink = false, is_source = true;
	else
		return false;

	if (is_sink && !impl->publish_sink)
		return false;
	if (is_source && !impl->publish_source)
		return false;

	/* Default excludes. */
	if (ends_with(node_name, ".monitor"))
		return false;
	if (device_api && spa_streq(device_api, "bluez5"))
		return false;
	if (node_network && spa_atob(node_network))
		return false;
	/* Skip our own discover-side virtual sinks (they start with
	 * "network." per discover's deterministic node.name). Otherwise
	 * a single-host test setup chains forever. */
	if (strncmp(node_name, "network.", 8) == 0)
		return false;

	/* User-supplied match rules — first match wins. Default if no rule
	 * matches (or rules absent): publish. */
	if (impl->publish_rules) {
		struct match_info mi = { .matched = false, .publish = true };
		pw_conf_match_rules(impl->publish_rules,
				    strlen(impl->publish_rules),
				    NAME, props, rule_matched_cb, &mi);
		if (mi.matched && !mi.publish)
			return false;
	}

	*is_sink_out = is_sink;
	return true;
}

static int meta_property(void *data, uint32_t subject, const char *key,
			 const char *type SPA_UNUSED, const char *value);

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = meta_property,
};

static void registry_global(void *data, uint32_t id,
			    uint32_t permissions SPA_UNUSED,
			    const char *type,
			    uint32_t version SPA_UNUSED,
			    const struct spa_dict *props)
{
	struct impl *impl = data;
	bool is_sink;

	/* Bind the default metadata once we see it in the registry. */
	if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
		if (impl->metadata != NULL || props == NULL)
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
		return;
	}

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;
	if (props == NULL)
		return;
	if (!should_publish(impl, props, &is_sink))
		return;
	if (find_service_by_id(impl, id))
		return;

	const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	if (desc == NULL)
		desc = spa_dict_lookup(props, PW_KEY_DEVICE_NICK);
	if (desc == NULL)
		desc = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);
	if (desc == NULL)
		desc = node_name;

	const char *ch_str = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNELS);
	uint32_t channels = impl->channels;
	if (ch_str) {
		uint32_t v;
		if (spa_atou32(ch_str, &v, 10) && v > 0 && v <= 32)
			channels = v;
	}

	uint16_t port;
	if (allocate_port(impl, &port) < 0) {
		pw_log_warn("no free port for node id=%u (%s) — skipping",
			    id, node_name);
		return;
	}

	struct service *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return;

	s->impl = impl;
	s->node_id = id;
	s->node_name = strdup(node_name);
	s->node_description = strdup(desc);
	s->is_sink = is_sink;
	s->channels = channels;
	s->port = port;
	gen_session_id(s->session_id, sizeof(s->session_id));

	/* Avahi labels max 63 bytes; truncate components to fit. */
	snprintf(s->service_name, sizeof(s->service_name), "%.20s: %.40s",
		 impl->host_name, s->node_description);

	spa_list_append(&impl->services, &s->link);

	pw_log_info("tracking node id=%u name=%s desc='%s' channels=%u port=%u%s",
		    id, node_name, desc, channels, s->port,
		    is_enabled(impl, node_name) ? "" : " [disabled by metadata]");

	service_apply_state(s);
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;

	if (id == impl->metadata_id && impl->metadata) {
		pw_log_info("default metadata gone");
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy(impl->metadata);
		impl->metadata = NULL;
		impl->metadata_id = SPA_ID_INVALID;
		return;
	}

	struct service *s = find_service_by_id(impl, id);
	if (s == NULL)
		return;
	pw_log_info("local node id=%u gone — unpublishing", id);
	service_free(s);
}

/* -- metadata events -------------------------------------------------- */

static int meta_property(void *data, uint32_t subject,
			 const char *key, const char *type SPA_UNUSED,
			 const char *value)
{
	struct impl *impl = data;

	/* We only care about per-host keys (subject = PW_ID_CORE = 0) that
	 * carry our prefix. Ignore everything else; the default metadata
	 * carries `default.audio.sink` etc. */
	if (subject != PW_ID_CORE || key == NULL)
		return 0;
	if (strncmp(key, PWNZ_META_PREFIX, sizeof(PWNZ_META_PREFIX) - 1) != 0)
		return 0;

	const char *tail = key + sizeof(PWNZ_META_PREFIX) - 1;
	size_t tail_len = strlen(tail);
	size_t suf_len = sizeof(PWNZ_META_SUFFIX) - 1;
	if (tail_len < suf_len ||
	    strcmp(tail + tail_len - suf_len, PWNZ_META_SUFFIX) != 0) {
		pw_log_debug("ignoring unrelated key '%s'", key);
		return 0;
	}

	size_t node_name_len = tail_len - suf_len;
	char *node_name = strndup(tail, node_name_len);
	if (node_name == NULL)
		return -ENOMEM;

	struct meta_entry *e = meta_find(impl, node_name);

	if (value == NULL) {
		/* cleared → revert to default (enabled) */
		if (e) {
			pw_log_info("metadata key for '%s' cleared", node_name);
			meta_entry_free(e);
		}
	} else {
		bool en = spa_atob(value);
		if (e == NULL) {
			e = calloc(1, sizeof(*e));
			if (e == NULL) {
				free(node_name);
				return -ENOMEM;
			}
			e->node_name = strdup(node_name);
			spa_list_append(&impl->meta_entries, &e->link);
		}
		e->enabled = en;
		pw_log_info("metadata: %s = %s", node_name, en ? "true" : "false");
	}

	struct service *s = find_service_by_node_name(impl, node_name);
	if (s != NULL)
		service_apply_state(s);

	free(node_name);
	return 0;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* -- child module (module-rtp-source) loading -------------------------- */

static void rtp_mod_destroyed(void *data)
{
	struct service *s = data;
	spa_hook_remove(&s->rtp_mod_listener);
	s->rtp_mod = NULL;
}

static const struct pw_impl_module_events rtp_mod_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = rtp_mod_destroyed,
};

static int load_rtp_source_for(struct service *s)
{
	struct impl *impl = s->impl;
	FILE *f;
	char *args;
	size_t size;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{ ");
	if (impl->multicast) {
		fprintf(f, "source.ip = \"%s\" ", impl->multicast_ip);
		fprintf(f, "net.ttl = %u ", impl->multicast_ttl);
		fprintf(f, "net.loop = %s ", impl->multicast_loop ? "true" : "false");
	} else {
		fprintf(f, "source.ip = \"0.0.0.0\" ");
	}
	fprintf(f, "source.port = %u ", s->port);
	fprintf(f, "sess.media = \"%s\" ",
		spa_streq(impl->codec, "opus") ? "opus" : "audio");
	fprintf(f, "audio.format = \"%s\" ", impl->format);
	fprintf(f, "audio.rate = %u ", impl->rate);
	fprintf(f, "audio.channels = %u ", s->channels);
	fprintf(f, "sess.latency.msec = %u ", impl->latency_ms);
	char pbuf[32];
	spa_dtoa(pbuf, sizeof(pbuf), impl->ptime_ms);
	fprintf(f, "stream.props = { ");
	fprintf(f, "node.network = true ");
	fprintf(f, "stream.may-pause = true ");
	fprintf(f, "rtp.ptime = %s ", pbuf);
	/* Suppress upstream's bogus IGMP recovery for unicast bindings.
	 * rtp-source unconditionally enables an IGMP-rejoin timer even when
	 * source.ip=0.0.0.0 (unicast). And its `last_packet_time` defaults
	 * to 0, so the deadline check `current_time - 0 >= deadline` is
	 * always true on the first tick. Killing `check.interval` ensures
	 * the timer never fires. */
	if (!impl->multicast)
		fprintf(f, "igmp.check.interval.sec = 31536000 ");
	if (impl->sap_enabled && impl->multicast) {
		const char *mime = spa_streq(impl->codec, "opus")
			? "audio/opus" : "audio/L16";
		fprintf(f, "sess.sap.announce = true ");
		fprintf(f, "sess.name = \"%s\" ", s->service_name);
		fprintf(f, "rtp.destination.ip = \"%s\" ", impl->multicast_ip);
		fprintf(f, "rtp.destination.port = %u ", s->port);
		fprintf(f, "rtp.ttl = %u ", impl->multicast_ttl);
		fprintf(f, "rtp.media = \"audio\" ");
		fprintf(f, "rtp.mime = \"%s\" ", mime);
		fprintf(f, "rtp.payload = %u ", PWNZ_DEFAULT_PAYLOAD);
		fprintf(f, "rtp.rate = %u ", impl->rate);
		fprintf(f, "rtp.channels = %u ", s->channels);
	}
	/* Route received audio into the specific local node. */
	if (s->is_sink) {
		fprintf(f, "media.class = \"Stream/Output/Audio\" ");
		fprintf(f, "node.target = \"%s\" ", s->node_name);
	} else {
		fprintf(f, "media.class = \"Stream/Input/Audio\" ");
		fprintf(f, "node.target = \"%s\" ", s->node_name);
	}
	fprintf(f, "node.description = \"net-rx %s\" ", s->node_name);
	fprintf(f, "} }");
	fclose(f);

	pw_log_debug("loading rtp-source: %s", args);

	s->rtp_mod = pw_context_load_module(impl->context,
					    "libpipewire-module-rtp-source",
					    args, NULL);
	free(args);

	if (s->rtp_mod == NULL) {
		pw_log_error("failed to load rtp-source for %s: %m",
			     s->node_name);
		return -errno;
	}

	pw_impl_module_add_listener(s->rtp_mod, &s->rtp_mod_listener,
				    &rtp_mod_events, s);
	return 0;
}

/* -- Avahi publish ----------------------------------------------------- */

static void entry_group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state,
			   void *userdata)
{
	struct service *s = userdata;

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		pw_log_info("Avahi service '%s' established", s->service_name);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION: {
		char *alt = avahi_alternative_service_name(s->service_name);
		pw_log_warn("name collision, retrying as '%s'", alt);
		snprintf(s->service_name, sizeof(s->service_name), "%s", alt);
		avahi_free(alt);
		if (++s->name_collision_count < 5)
			publish_service(s);
		break;
	}
	case AVAHI_ENTRY_GROUP_FAILURE:
		pw_log_error("Avahi entry group failure: %s",
			     avahi_strerror(avahi_client_errno(
					     avahi_entry_group_get_client(g))));
		break;
	default:
		break;
	}
}

static AvahiStringList *build_txt(struct service *s)
{
	struct impl *impl = s->impl;
	AvahiStringList *t = NULL;
	char buf[256];

	t = avahi_string_list_add_pair(t, PWNZ_TXT_NODE_NAME, s->node_name);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_DESCRIPTION, s->node_description);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_HOST, impl->host_name);
	snprintf(buf, sizeof(buf), "%u", impl->rate);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_RATE, buf);
	snprintf(buf, sizeof(buf), "%u", s->channels);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_CHANNELS, buf);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_FORMAT, impl->format);
	snprintf(buf, sizeof(buf), "%u", PWNZ_DEFAULT_PAYLOAD);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_PAYLOAD, buf);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_SESSION, s->session_id);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_MODE,
				       s->is_sink ? "sink" : "source");
	t = avahi_string_list_add_pair(t, PWNZ_TXT_CODEC, impl->codec);
	t = avahi_string_list_add_pair(t, PWNZ_TXT_TRANSPORT,
				       impl->multicast ? "multicast" : "unicast");
	if (impl->multicast) {
		t = avahi_string_list_add_pair(t, PWNZ_TXT_MCAST_IP, impl->multicast_ip);
		snprintf(buf, sizeof(buf), "%u", impl->multicast_ttl);
		t = avahi_string_list_add_pair(t, PWNZ_TXT_MCAST_TTL, buf);
	}
	return t;
}

static void sap_mod_destroyed(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->sap_mod_listener);
	impl->sap_mod = NULL;
}

static const struct pw_impl_module_events sap_mod_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = sap_mod_destroyed,
};

static int load_sap_module(struct impl *impl)
{
	static const char sap_args[] =
		"{ stream.rules = ["
		"  { matches = [ { sess.sap.announce = true } ]"
		"    actions = { announce-stream = { } } }"
		"] }";

	impl->sap_mod = pw_context_load_module(impl->context,
					       "libpipewire-module-rtp-sap",
					       sap_args, NULL);
	if (impl->sap_mod == NULL) {
		pw_log_error("failed to load libpipewire-module-rtp-sap: %m");
		return -errno;
	}
	pw_impl_module_add_listener(impl->sap_mod, &impl->sap_mod_listener,
				    &sap_mod_events, impl);
	pw_log_info("SAP announcements enabled");
	return 0;
}

static int publish_service(struct service *s)
{
	struct impl *impl = s->impl;
	AvahiStringList *txt;
	int err;

	if (!is_enabled(impl, s->node_name))
		return 0;

	if (impl->avahi_client == NULL ||
	    avahi_client_get_state(impl->avahi_client) != AVAHI_CLIENT_S_RUNNING)
		return 0;

	if (s->rtp_mod == NULL && load_rtp_source_for(s) < 0)
		return -1;

	if (s->entry_group == NULL) {
		s->entry_group = avahi_entry_group_new(impl->avahi_client,
						       entry_group_cb, s);
		if (s->entry_group == NULL) {
			pw_log_error("avahi_entry_group_new: %s",
				     avahi_strerror(avahi_client_errno(impl->avahi_client)));
			return -1;
		}
	} else {
		avahi_entry_group_reset(s->entry_group);
	}

	for (;;) {
		txt = build_txt(s);
		err = avahi_entry_group_add_service_strlst(
			s->entry_group,
			AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
			0,
			s->service_name,
			PWNZ_SERVICE_TYPE,
			NULL, NULL,
			s->port,
			txt);
		avahi_string_list_free(txt);

		if (err == AVAHI_OK)
			break;

		if (err == AVAHI_ERR_COLLISION && s->name_collision_count < 16) {
			char *alt = avahi_alternative_service_name(s->service_name);
			if (alt == NULL) {
				pw_log_error("out of memory generating alt name");
				return -1;
			}
			pw_log_info("name '%s' collides, retrying as '%s'",
				    s->service_name, alt);
			snprintf(s->service_name, sizeof(s->service_name), "%s", alt);
			avahi_free(alt);
			s->name_collision_count++;
			avahi_entry_group_reset(s->entry_group);
			continue;
		}

		pw_log_error("avahi_entry_group_add_service: %s",
			     avahi_strerror(err));
		return -1;
	}

	if ((err = avahi_entry_group_commit(s->entry_group)) < 0) {
		pw_log_error("avahi_entry_group_commit: %s",
			     avahi_strerror(err));
		return -1;
	}

	pw_log_info("published '%s' port=%u mode=%s", s->service_name,
		    s->port, s->is_sink ? "sink" : "source");
	return 0;
}

static void republish_all(struct impl *impl)
{
	struct service *s;
	spa_list_for_each(s, &impl->services, link)
		service_apply_state(s);
}

static void unpublish_all(struct impl *impl)
{
	struct service *s;
	spa_list_for_each(s, &impl->services, link) {
		if (s->entry_group) {
			avahi_entry_group_free(s->entry_group);
			s->entry_group = NULL;
		}
	}
}

static void unpublish_service(struct service *s)
{
	if (s->entry_group) {
		avahi_entry_group_free(s->entry_group);
		s->entry_group = NULL;
	}
	if (s->rtp_mod) {
		spa_hook_remove(&s->rtp_mod_listener);
		pw_impl_module_destroy(s->rtp_mod);
		s->rtp_mod = NULL;
	}
	s->name_collision_count = 0;
}

static void service_apply_state(struct service *s)
{
	bool want = is_enabled(s->impl, s->node_name);
	bool have = s->entry_group != NULL || s->rtp_mod != NULL;
	if (want == have)
		return;
	if (want) {
		pw_log_info("enabling '%s' via metadata", s->node_name);
		publish_service(s);
	} else {
		pw_log_info("disabling '%s' via metadata", s->node_name);
		unpublish_service(s);
	}
}

static void service_free(struct service *s)
{
	spa_list_remove(&s->link);
	unpublish_service(s);
	free(s->node_name);
	free(s->node_description);
	free(s);
}

/* -- Avahi client state ------------------------------------------------ */

static void avahi_client_cb(AvahiClient *c, AvahiClientState state, void *userdata)
{
	struct impl *impl = userdata;
	int err;

	impl->avahi_client = c;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		pw_log_info("Avahi daemon up");
		republish_all(impl);
		break;
	case AVAHI_CLIENT_S_COLLISION:
		pw_log_warn("Avahi host name collision");
		unpublish_all(impl);
		break;
	case AVAHI_CLIENT_FAILURE:
		err = avahi_client_errno(c);
		pw_log_error("Avahi client failure: %s", avahi_strerror(err));
		unpublish_all(impl);
		avahi_client_free(impl->avahi_client);
		impl->avahi_client = NULL;
		if (err == AVAHI_ERR_DISCONNECTED)
			start_avahi_client(impl);
		break;
	case AVAHI_CLIENT_CONNECTING:
		pw_log_info("connecting to Avahi daemon");
		break;
	case AVAHI_CLIENT_S_REGISTERING:
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

/* -- pw core listener -------------------------------------------------- */

static void on_core_error(void *data, uint32_t id, int seq, int res,
			  const char *message)
{
	struct impl *impl = data;
	pw_log_error("core error id:%u seq:%d res:%d (%s): %s",
		     id, seq, res, spa_strerror(res), message);
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

/* -- module lifecycle -------------------------------------------------- */

static void impl_free(struct impl *impl)
{
	struct service *s;
	struct meta_entry *e;

	spa_list_consume(s, &impl->services, link)
		service_free(s);
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

	if (impl->avahi_client)
		avahi_client_free(impl->avahi_client);
	if (impl->avahi_poll)
		pw_avahi_poll_free(impl->avahi_poll);

	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		pw_core_disconnect(impl->core);
	}

	if (impl->sap_mod) {
		spa_hook_remove(&impl->sap_mod_listener);
		pw_impl_module_destroy(impl->sap_mod);
	}

	free(impl->format);
	free(impl->codec);
	free(impl->multicast_ip);
	free(impl->publish_rules);
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
	const char *str;
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
	spa_list_init(&impl->services);
	spa_list_init(&impl->meta_entries);

	if (gethostname(impl->host_name, sizeof(impl->host_name)) < 0)
		snprintf(impl->host_name, sizeof(impl->host_name), "pipewire");
	impl->host_name[sizeof(impl->host_name) - 1] = '\0';

	impl->publish_sink   = pw_properties_get_bool(props, "publish.sink",   true);
	impl->publish_source = pw_properties_get_bool(props, "publish.source", false);
	str = pw_properties_get(props, "publish.rules");
	impl->publish_rules = str ? strdup(str) : NULL;
	impl->rate     = pw_properties_get_uint32(props, "publish.rate",     PWNZ_DEFAULT_RATE);
	impl->channels = pw_properties_get_uint32(props, "publish.channels", PWNZ_DEFAULT_CHANNELS);
	str = pw_properties_get(props, "publish.format");
	impl->format = strdup(str ? str : PWNZ_DEFAULT_FORMAT);
	str = pw_properties_get(props, "publish.codec");
	impl->codec = strdup(str ? str : PWNZ_DEFAULT_CODEC);

	str = pw_properties_get(props, "publish.transport");
	impl->multicast = str && spa_streq(str, "multicast");
	str = pw_properties_get(props, "publish.multicast.ip");
	impl->multicast_ip = strdup(str ? str : PWNZ_DEFAULT_MCAST_IP);
	impl->multicast_ttl = pw_properties_get_uint32(props,
					"publish.multicast.ttl", PWNZ_DEFAULT_MCAST_TTL);
	impl->multicast_port = (uint16_t) pw_properties_get_uint32(props,
					"publish.multicast.port", PWNZ_DEFAULT_MCAST_PORT);
	impl->multicast_loop = pw_properties_get_bool(props,
					"publish.multicast.loop", false);

	impl->latency_ms = pw_properties_get_uint32(props,
					"publish.latency.msec", PWNZ_DEFAULT_LATENCY_MS);
	str = pw_properties_get(props, "publish.ptime.msec");
	impl->ptime_ms = str ? atof(str) : PWNZ_DEFAULT_PTIME_MS;
	if (impl->ptime_ms <= 0)
		impl->ptime_ms = PWNZ_DEFAULT_PTIME_MS;

	uint32_t port_fixed = pw_properties_get_uint32(props, "publish.port", 0);
	impl->port_fixed = port_fixed != 0;
	impl->port_fixed_value = (uint16_t) port_fixed;
	impl->port_base = (uint16_t) pw_properties_get_uint32(props,
					"publish.port.base", PWNZ_DEFAULT_PORT_BASE);
	impl->port_range = (uint16_t) pw_properties_get_uint32(props,
					"publish.port.range", PWNZ_DEFAULT_PORT_RANGE);
	if (impl->port_range == 0)
		impl->port_range = 1;
	impl->next_port_offset = 0;

	impl->sap_enabled = pw_properties_get_bool(props, "publish.sap", false);
	if (impl->sap_enabled && !impl->multicast) {
		pw_log_warn("publish.sap requires publish.transport=multicast; disabling SAP");
		impl->sap_enabled = false;
	}

	pw_log_info("publish.sink=%d publish.source=%d rate=%u channels=%u format=%s codec=%s transport=%s%s sap=%d host=%s",
		    impl->publish_sink, impl->publish_source, impl->rate,
		    impl->channels, impl->format, impl->codec,
		    impl->multicast ? "multicast " : "unicast ",
		    impl->multicast ? impl->multicast_ip : "",
		    impl->sap_enabled, impl->host_name);

	pw_impl_module_add_listener(module, &impl->module_listener,
				    &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	impl->core = pw_context_connect(context, NULL, 0);
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("pw_context_connect: %m");
		goto error;
	}
	pw_proxy_add_listener((struct pw_proxy *) impl->core,
			      &impl->core_proxy_listener,
			      &core_proxy_events, impl);
	pw_core_add_listener(impl->core, &impl->core_listener,
			     &core_events, impl);

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
	if (impl->registry == NULL) {
		res = -errno;
		goto error;
	}
	pw_registry_add_listener(impl->registry, &impl->registry_listener,
				 &registry_events, impl);

	impl->avahi_poll = pw_avahi_poll_new(context);
	if (impl->avahi_poll == NULL) {
		res = -ENOMEM;
		goto error;
	}

	if ((res = start_avahi_client(impl)) < 0)
		goto error;

	if (impl->sap_enabled && load_sap_module(impl) < 0) {
		/* non-fatal: keep going without SAP */
		impl->sap_enabled = false;
	}

	return 0;

error:
	impl_free(impl);
	return res;
}
