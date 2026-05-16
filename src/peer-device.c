/* SPDX-License-Identifier: MIT
 *
 * Per-peer-card SPA Device with Off/On profile, registered into the
 * local PipeWire context so pavucontrol Configuration tab shows the
 * card with a profile dropdown.
 *
 * Modelled after spa/plugins/jack/jack-device.c (the smallest upstream
 * SPA device that ships profile-aware enum_params/set_param).
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

struct peer_device {
	/* SPA device exposed to PipeWire. */
	struct spa_device device;
	struct spa_hook_list hooks;

	/* Owned by us. */
	char *node_name;
	char *node_description;
	char *peer_host;
	uint32_t profile;       /* 0 = Off, 1 = On */

	peer_device_profile_cb cb;
	void *cb_user_data;

	/* Registered impl. */
	struct pw_impl_device *impl_device;
};

/* -- SPA Device implementation ---------------------------------------- */

static int emit_info(struct peer_device *this, bool full)
{
	struct spa_dict_item items[10];
	struct spa_device_info dinfo = SPA_DEVICE_INFO_INIT();
	struct spa_param_info params[2];
	int n = 0;
	(void) full;

	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API,         "network-rtp");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NICK,        this->node_name);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME,        this->node_name);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, this->node_description);
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS,        "Audio/Device");
	items[n++] = SPA_DICT_ITEM_INIT("device.intended-roles",    "Music");
	items[n++] = SPA_DICT_ITEM_INIT("pipewire-net-zeroconf.peer.host", this->peer_host);

	dinfo.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS |
			    SPA_DEVICE_CHANGE_MASK_PARAMS;
	dinfo.props = &SPA_DICT_INIT(items, n);

	spa_zero(params);
	params[0].id    = SPA_PARAM_EnumProfile;
	params[0].flags = SPA_PARAM_INFO_READ;
	params[1].id    = SPA_PARAM_Profile;
	params[1].flags = SPA_PARAM_INFO_READWRITE;
	dinfo.n_params = SPA_N_ELEMENTS(params);
	dinfo.params = params;

	spa_device_emit_info(&this->hooks, &dinfo);
	return 0;
}

static struct spa_pod *build_profile(struct peer_device *this SPA_UNUSED,
				     struct spa_pod_builder *b,
				     uint32_t id, uint32_t index)
{
	struct spa_pod_frame f;
	const char *name, *desc;
	int prio;

	switch (index) {
	case 0:
		name = "off";
		desc = "Off";
		prio = 0;
		break;
	case 1:
		name = "on";
		desc = "On";
		prio = 1;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,       SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name,        SPA_POD_String(name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(desc),
		SPA_PARAM_PROFILE_priority,    SPA_POD_Int(prio),
		0);
	return spa_pod_builder_pop(b, &f);
}

static int impl_add_listener(void *object, struct spa_hook *listener,
			     const struct spa_device_events *events, void *data)
{
	struct peer_device *this = object;
	struct spa_hook_list save;
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	if (events->info)
		emit_info(this, true);
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
		if (result.index >= 2)
			return 0;
		param = build_profile(this, &b, id, result.index);
		break;
	case SPA_PARAM_Profile:
		if (result.index >= 1)
			return 0;
		param = build_profile(this, &b, id, this->profile);
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			       SPA_RESULT_TYPE_DEVICE_PARAMS, &result);
	if (++count != num)
		goto next;
	return 0;
}

static int impl_set_param(void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct peer_device *this = object;
	(void) flags;

	if (id != SPA_PARAM_Profile)
		return -ENOENT;

	uint32_t idx;
	if (spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamProfile, NULL,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx)) < 0)
		return -EINVAL;
	if (idx > 1)
		return -EINVAL;

	bool new_on = (idx == 1);
	bool cur_on = (this->profile == 1);
	if (new_on == cur_on)
		return 0;

	this->profile = idx;
	emit_info(this, false);

	if (this->cb)
		this->cb(this->cb_user_data, new_on);
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
	dev->profile          = info->default_on ? 1 : 0;
	dev->cb               = cb;
	dev->cb_user_data     = user_data;

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

int peer_device_set_profile(struct peer_device *dev, bool on)
{
	if (dev == NULL)
		return -EINVAL;
	uint32_t new_idx = on ? 1 : 0;
	if (dev->profile == new_idx)
		return 0;
	dev->profile = new_idx;
	emit_info(dev, false);
	if (dev->cb)
		dev->cb(dev->cb_user_data, on);
	return 0;
}

bool peer_device_is_on(struct peer_device *dev)
{
	return dev && dev->profile == 1;
}
