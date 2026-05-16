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
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "avahi-poll.h"
#include "common.h"

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

/* Identity for a remote service: stable across drop/recover. */
struct tunnel {
	struct spa_list link;
	struct impl *impl;

	char *avahi_name;    /* Avahi service instance name */
	char *peer_host;     /* from TXT or fallback host_name */
	char *peer_node;     /* TXT node-name */
	bool  is_sink_mode;  /* TXT mode=sink → we create local Audio/Sink */

	struct pw_impl_module *rtp_mod;
	struct spa_hook rtp_mod_listener;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *props;

	AvahiPoll *avahi_poll;
	AvahiClient *avahi_client;
	AvahiServiceBrowser *browser;

	bool discover_sink;
	bool discover_source;
	bool discover_local;
	AvahiProtocol discover_protocol;  /* AVAHI_PROTO_INET | INET6 | UNSPEC */

	double   ptime_ms;
	uint32_t latency_ms;

	struct spa_list tunnels;
};

static int start_avahi_client(struct impl *impl);
static void tunnel_free(struct tunnel *t);

/* -- helpers ----------------------------------------------------------- */

static struct tunnel *find_tunnel(struct impl *impl, const char *avahi_name)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnels, link)
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
	char *peer_host_safe = sanitize_for_node_name(t->peer_host);
	char *peer_node_safe = sanitize_for_node_name(t->peer_node);

	if ((f = open_memstream(&args, &size)) == NULL) {
		free(peer_host_safe);
		free(peer_node_safe);
		return -errno;
	}

	uint32_t ssrc = fnv1a_32(t->peer_host, t->peer_node);

	char pbuf[PTIME_BUF_LEN];
	spa_dtoa(pbuf, sizeof(pbuf), t->impl->ptime_ms);

	if (t->is_sink_mode) {
		child_mod = "libpipewire-module-rtp-sink";
		fprintf(f, "{ ");
		fprintf(f, "destination.ip = \"%s\" ", net_addr);
		fprintf(f, "destination.port = %u ", peer_port);
		fprintf(f, "sess.latency.msec = %u ", t->impl->latency_ms);
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
			t->peer_host, description);
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
		fprintf(f, "sess.latency.msec = %u ", t->impl->latency_ms);
		fprintf(f, "sess.media = \"%s\" ",
			spa_streq(codec, "opus") ? "opus" : "audio");
		fprintf(f, "audio.format = \"%s\" ", format);
		fprintf(f, "audio.rate = %s ", rate);
		fprintf(f, "audio.channels = %s ", channels);
		fprintf(f, "stream.props = { ");
		fprintf(f, "node.name = \"network.%s.%s\" ",
			peer_host_safe, peer_node_safe);
		fprintf(f, "node.description = \"Network: %s: %s\" ",
			t->peer_host, description);
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

	t->rtp_mod = pw_context_load_module(t->impl->context,
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
	char *node_name, *peer_host, *description, *mode;
	char *rate, *channels, *format, *codec;
	char *transport, *mcast_ip, *mcast_ttl;
};

static void txt_kv_free(struct txt_kv *kv)
{
	avahi_free(kv->node_name);
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
	if (t == NULL) {
		t = calloc(1, sizeof(*t));
		if (t == NULL)
			goto done;
		t->impl = impl;
		t->avahi_name = strdup(name);
		spa_list_append(&impl->tunnels, &t->link);
	}
	t->is_sink_mode = is_sink_mode;
	free(t->peer_host);
	t->peer_host = strdup(kv.peer_host ? kv.peer_host : host_name);
	free(t->peer_node);
	t->peer_node = strdup(kv.node_name);

	avahi_address_snprint(addr_buf, sizeof(addr_buf), a);

	load_local_endpoint(t, addr_buf, port,
			    kv.rate     ? kv.rate     : "48000",
			    kv.channels ? kv.channels : "2",
			    kv.format   ? kv.format   : PWNZ_DEFAULT_FORMAT,
			    kv.codec    ? kv.codec    : PWNZ_DEFAULT_CODEC,
			    kv.description ? kv.description : kv.node_name,
			    kv.transport,
			    kv.mcast_ip,
			    kv.mcast_ttl);

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
	spa_list_remove(&t->link);
	if (t->rtp_mod) {
		spa_hook_remove(&t->rtp_mod_listener);
		pw_impl_module_destroy(t->rtp_mod);
	}
	free(t->avahi_name);
	free(t->peer_host);
	free(t->peer_node);
	free(t);
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

/* -- module lifecycle ------------------------------------------------- */

static void impl_free(struct impl *impl)
{
	struct tunnel *t;

	spa_list_consume(t, &impl->tunnels, link)
		tunnel_free(t);

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
	spa_list_init(&impl->tunnels);

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
