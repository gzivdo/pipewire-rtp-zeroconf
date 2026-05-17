/* SPDX-License-Identifier: MIT
 *
 * Per-peer-card SPA Device with a direction-aware profile dropdown.
 * Modelled after spa/plugins/jack/jack-device.c.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/keys.h>
#include <spa/utils/hook.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/param/param.h>
#include <spa/param/profile.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include "peer-device.h"

/* Profile slots — fixed table, we just filter by `available_dirs`. */
struct profile_def {
	const char *name;
	const char *description;
	uint32_t needs;   /* exact directions this profile activates */
	int priority;
};

static const struct profile_def PROFILES[] = {
	{ "off",          "Off",            0,                                  0 },
	{ "output",       "Output",         PEER_DIR_OUTPUT,                  100 },
	{ "input",        "Input",          PEER_DIR_INPUT,                    90 },
	{ "output+input", "Output + Input", PEER_DIR_OUTPUT | PEER_DIR_INPUT, 110 },
};
#define N_PROFILES (sizeof(PROFILES) / sizeof(PROFILES[0]))

static const char *profile_name_for_dirs(uint32_t dirs)
{
	for (size_t i = 0; i < N_PROFILES; i++)
		if (PROFILES[i].needs == dirs)
			return PROFILES[i].name;
	return "off";
}

struct peer_device;
static void sync_device_profile_prop(struct peer_device *dev);

#define IDX_EnumProfile  0
#define IDX_Profile      1
#define N_INFO_PARAMS    2

struct peer_device {
	struct spa_device device;
	struct spa_hook_list hooks;

	char *node_name;
	char *node_description;
	char *peer_host;
	uint32_t available_dirs;
	uint32_t active_dirs;

	/* Persistent spa_device_info + spa_param_info array, mirroring
	 * alsa-acp-device.c. On each emit_info we toggle the
	 * SPA_PARAM_INFO_SERIAL bit (XOR) on every param whose .user was
	 * bumped and then reset .user to 0 — this is the actual signal
	 * pw_impl_device watches for to invalidate its param cache.
	 * Plain monotonic .user++ is not enough on PW 1.0.x. */
	struct spa_device_info info;
	struct spa_param_info params[N_INFO_PARAMS];

	peer_device_profile_cb cb;
	void *cb_user_data;

	struct pw_impl_device *impl_device;
};

/* -- helpers ---------------------------------------------------------- */

static bool profile_is_offered(const struct peer_device *this,
			       const struct profile_def *p)
{
	/* "Off" is always available. Other profiles must match the card's
	 * available directions exactly — we don't let the user pick a
	 * profile that needs a direction the peer doesn't offer. */
	if (p->needs == 0)
		return true;
	return (p->needs & ~this->available_dirs) == 0;
}

static int find_profile_by_dirs(const struct peer_device *this, uint32_t dirs)
{
	for (size_t i = 0; i < N_PROFILES; i++) {
		if (PROFILES[i].needs == dirs && profile_is_offered(this, &PROFILES[i]))
			return (int) i;
	}
	return -1;
}

/* -- SPA Device impl -------------------------------------------------- */

static int emit_info(struct peer_device *this)
{
	struct spa_dict_item items[10];
	int n = 0;

	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API,         "network-rtp");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NICK,        this->node_name);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME,        this->node_name);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, this->node_description);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS,        "Audio/Device");
	items[n++] = SPA_DICT_ITEM_INIT("pipewire-net-zeroconf.peer.host", this->peer_host);

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PROPS;
	this->info.props = &SPA_DICT_INIT(items, n);

	/* Toggle SPA_PARAM_INFO_SERIAL on params whose user>0, then reset
	 * user. This is what alsa-acp-device.c does (line 226-233) and is
	 * the actual cache-invalidation signal pw_impl_device watches for. */
	if (this->info.change_mask & SPA_DEVICE_CHANGE_MASK_PARAMS) {
		for (size_t i = 0; i < N_INFO_PARAMS; i++) {
			if (this->params[i].user > 0) {
				this->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
				this->params[i].user = 0;
			}
		}
	}

	spa_device_emit_info(&this->hooks, &this->info);
	this->info.change_mask = 0;
	return 0;
}

static struct spa_pod *build_profile(struct peer_device *this,
				     struct spa_pod_builder *b,
				     uint32_t id, int profile_idx)
{
	const struct profile_def *p;
	struct spa_pod_frame f, fs, fc;

	if (profile_idx < 0 || (size_t) profile_idx >= N_PROFILES) {
		errno = EINVAL;
		return NULL;
	}
	p = &PROFILES[profile_idx];

	spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,       SPA_POD_Int(profile_idx),
		SPA_PARAM_PROFILE_name,        SPA_POD_String(p->name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(p->description),
		SPA_PARAM_PROFILE_priority,    SPA_POD_Int(p->priority),
		SPA_PARAM_PROFILE_available,
			SPA_POD_Id(profile_is_offered(this, p)
				   ? SPA_PARAM_AVAILABILITY_yes
				   : SPA_PARAM_AVAILABILITY_no),
		0);

	/* Emit classes Struct so WirePlumber's policy can see which
	 * audio node-classes the profile activates. Shape matches what
	 * alsa-acp-device emits: (n_classes, (class, n_nodes,
	 * "card.profile.devices", Array(Int))*). */
	uint32_t n_classes = 0;
	if (p->needs & PEER_DIR_OUTPUT) n_classes++;
	if (p->needs & PEER_DIR_INPUT)  n_classes++;

	spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
	spa_pod_builder_push_struct(b, &fs);
	spa_pod_builder_int(b, (int32_t) n_classes);
	if (p->needs & PEER_DIR_OUTPUT) {
		spa_pod_builder_push_struct(b, &fc);
		spa_pod_builder_string(b, "Audio/Sink");
		spa_pod_builder_int(b, 1);
		spa_pod_builder_string(b, "card.profile.devices");
		spa_pod_builder_array(b, sizeof(int32_t), SPA_TYPE_Int, 0, NULL);
		spa_pod_builder_pop(b, &fc);
	}
	if (p->needs & PEER_DIR_INPUT) {
		spa_pod_builder_push_struct(b, &fc);
		spa_pod_builder_string(b, "Audio/Source");
		spa_pod_builder_int(b, 1);
		spa_pod_builder_string(b, "card.profile.devices");
		spa_pod_builder_array(b, sizeof(int32_t), SPA_TYPE_Int, 0, NULL);
		spa_pod_builder_pop(b, &fc);
	}
	spa_pod_builder_pop(b, &fs);

	return spa_pod_builder_pop(b, &f);
}

static int impl_add_listener(void *object, struct spa_hook *listener,
			     const struct spa_device_events *events, void *data)
{
	struct peer_device *this = object;
	struct spa_hook_list save;
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	if (events->info) {
		/* Force full info on every new listener — pw_impl_device
		 * (and on subsequent re-binds, pavucontrol via PA-bridge)
		 * needs to see both PROPS and PARAMS so it populates
		 * EnumProfile/Profile in its cache. Without this a peer_device
		 * that's created from Avahi without a subsequent set_param
		 * (e.g. an Output-only HDMI card with no user click) ends
		 * up with params:(0) and an empty profile dropdown. */
		this->info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS |
					 SPA_DEVICE_CHANGE_MASK_PARAMS;
		emit_info(this);
	}
	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct peer_device *this = object;
	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);
	return 0;
}

static int impl_enum_params(void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	struct peer_device *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_device_params result;
	uint32_t count = 0;

	result.id = id;
	result.next = start;

next:
	result.index = result.next++;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
		/* Walk all profiles, skip ones we don't offer. */
		while (result.index < N_PROFILES &&
		       !profile_is_offered(this, &PROFILES[result.index]))
			result.index = result.next++;
		if (result.index >= N_PROFILES)
			return 0;
		param = build_profile(this, &b, id, (int) result.index);
		break;
	case SPA_PARAM_Profile:
		if (result.index >= 1)
			return 0;
		{
			int idx = find_profile_by_dirs(this, this->active_dirs);
			if (idx < 0)
				idx = 0;
			param = build_profile(this, &b, id, idx);
		}
		break;
	default:
		return -ENOENT;
	}

	if (param == NULL)
		return -errno;

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			       SPA_RESULT_TYPE_DEVICE_PARAMS, &result);
	if (++count != num)
		goto next;
	return 0;
}

static int impl_set_param(void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct peer_device *this = object;
	(void) flags;

	if (id != SPA_PARAM_Profile)
		return -ENOENT;

	int32_t idx;
	if (spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamProfile, NULL,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx)) < 0)
		return -EINVAL;
	pw_log_info("peer_device '%s': set_param Profile index=%d (active was 0x%x)",
		    this->node_name, idx, this->active_dirs);
	if (idx < 0 || (size_t) idx >= N_PROFILES)
		return -EINVAL;
	if (!profile_is_offered(this, &PROFILES[idx]))
		return -EINVAL;

	uint32_t new_dirs = PROFILES[idx].needs;
	if (new_dirs == this->active_dirs) {
		/* Reaffirm device.profile property even on no-op so it
		 * stays consistent with Profile.current — WP / pulse-bridge
		 * read this property when computing PA card active profile. */
		sync_device_profile_prop(this);
		return 0;
	}

	this->active_dirs = new_dirs;
	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Profile].user++;
	sync_device_profile_prop(this);
	emit_info(this);
	if (this->cb)
		this->cb(this->cb_user_data, new_dirs);
	return 0;
}

static const struct spa_device_methods peer_device_methods = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync         = impl_sync,
	.enum_params  = impl_enum_params,
	.set_param    = impl_set_param,
};

/* -- public API ------------------------------------------------------- */

struct peer_device *peer_device_new(struct pw_context *context,
				    const struct peer_device_info *info,
				    peer_device_profile_cb cb,
				    void *user_data)
{
	struct peer_device *dev = calloc(1, sizeof(*dev));
	if (dev == NULL)
		return NULL;

	dev->node_name        = strdup(info->node_name);
	dev->node_description = strdup(info->node_description);
	dev->peer_host        = strdup(info->peer_host);
	dev->available_dirs   = info->available_dirs;
	dev->active_dirs      = info->default_dirs & info->available_dirs;
	dev->cb               = cb;
	dev->cb_user_data     = user_data;

	/* Persistent device info + per-param user counters (see
	 * alsa-acp-device.c). spa_device_info.params *must* point at a
	 * stable buffer that lives as long as the device — pw_impl_device
	 * keeps the pointer for change detection across emit_info calls.
	 *
	 * The cache-invalidation trick (line 226-233 of alsa-acp-device.c)
	 * is to TOGGLE the SPA_PARAM_INFO_SERIAL flag bit (XOR) on every
	 * emit_info where the param's user counter is non-zero, then
	 * reset user to 0. pw_impl_device watches for the flags change
	 * (not the user value!) and uses that to invalidate its cache. */
	dev->info = (struct spa_device_info) SPA_DEVICE_INFO_INIT();
	dev->params[IDX_EnumProfile] = (struct spa_param_info){
		.id    = SPA_PARAM_EnumProfile,
		.flags = SPA_PARAM_INFO_READ,
	};
	dev->params[IDX_Profile] = (struct spa_param_info){
		.id    = SPA_PARAM_Profile,
		.flags = SPA_PARAM_INFO_READWRITE,
	};
	dev->info.params   = dev->params;
	dev->info.n_params = N_INFO_PARAMS;

	dev->device.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE,
		&peer_device_methods, dev);
	spa_hook_list_init(&dev->hooks);

	struct pw_properties *props = pw_properties_new(
		SPA_KEY_DEVICE_API,         "network-rtp",
		SPA_KEY_DEVICE_NAME,        dev->node_name,
		SPA_KEY_DEVICE_NICK,        dev->node_name,
		SPA_KEY_DEVICE_DESCRIPTION, dev->node_description,
		SPA_KEY_MEDIA_CLASS,        "Audio/Device",
		"device.profile",           profile_name_for_dirs(dev->active_dirs),
		"pipewire-net-zeroconf.peer.host", dev->peer_host,
		NULL);
	if (props == NULL)
		goto err;

	dev->impl_device = pw_context_create_device(context, props, 0);
	if (dev->impl_device == NULL) {
		pw_properties_free(props);
		goto err;
	}
	pw_impl_device_set_implementation(dev->impl_device, &dev->device);
	if (pw_impl_device_register(dev->impl_device, NULL) < 0) {
		pw_impl_device_destroy(dev->impl_device);
		dev->impl_device = NULL;
		goto err;
	}

	return dev;

err:
	free(dev->node_name);
	free(dev->node_description);
	free(dev->peer_host);
	free(dev);
	return NULL;
}

void peer_device_destroy(struct peer_device *dev)
{
	if (dev == NULL)
		return;
	if (dev->impl_device)
		pw_impl_device_destroy(dev->impl_device);
	free(dev->node_name);
	free(dev->node_description);
	free(dev->peer_host);
	free(dev);
}

uint32_t peer_device_get_id(struct peer_device *dev)
{
	if (dev == NULL || dev->impl_device == NULL)
		return SPA_ID_INVALID;
	struct pw_global *g = pw_impl_device_get_global(dev->impl_device);
	return g ? pw_global_get_id(g) : SPA_ID_INVALID;
}

/* Mirror active_dirs into the device.profile property so WirePlumber's
 * find-best-profile hook (which checks device.properties["device.profile"]
 * with absolute priority before falling back to "highest priority available")
 * picks the user's choice instead of overwriting it with output+input on
 * every params-changed / pavucontrol reopen. */
static void sync_device_profile_prop(struct peer_device *dev)
{
	if (dev->impl_device == NULL)
		return;
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("device.profile",
				   profile_name_for_dirs(dev->active_dirs)),
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);
	pw_impl_device_update_properties(dev->impl_device, &dict);
}

int peer_device_set_active_dirs(struct peer_device *dev, uint32_t active_dirs)
{
	if (dev == NULL)
		return -EINVAL;
	if (active_dirs & ~dev->available_dirs)
		return -EINVAL;
	if (active_dirs == dev->active_dirs) {
		/* Even when unchanged, keep the property in sync — WP may
		 * have stripped/ignored it before we registered impl_device. */
		sync_device_profile_prop(dev);
		return 0;
	}
	dev->active_dirs = active_dirs;
	dev->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	dev->params[IDX_Profile].user++;
	sync_device_profile_prop(dev);
	emit_info(dev);
	if (dev->cb)
		dev->cb(dev->cb_user_data, active_dirs);
	return 0;
}

uint32_t peer_device_get_active_dirs(struct peer_device *dev)
{
	return dev ? dev->active_dirs : 0;
}

int peer_device_set_available_dirs(struct peer_device *dev,
				   uint32_t available_dirs)
{
	if (dev == NULL)
		return -EINVAL;
	if (available_dirs == dev->available_dirs)
		return 0;
	dev->available_dirs = available_dirs;
	uint32_t clamped = dev->active_dirs & available_dirs;
	bool dirs_changed = (clamped != dev->active_dirs);
	dev->active_dirs = clamped;
	dev->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	dev->params[IDX_EnumProfile].user++;
	dev->params[IDX_Profile].user++;
	sync_device_profile_prop(dev);
	emit_info(dev);
	if (dirs_changed && dev->cb)
		dev->cb(dev->cb_user_data, clamped);
	return 0;
}
