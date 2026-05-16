/* SPDX-License-Identifier: MIT
 *
 * SPA Device representing one remote peer card. Exposes
 * SPA_PARAM_EnumProfile + SPA_PARAM_Profile so pavucontrol (and any
 * other PW Configuration-tab UI) shows the card with a Profile dropdown:
 *   - 0: Off    — no child rtp module loaded, nothing on the LAN
 *   - 1: On     — child rtp-sink (or -source) module loaded, audio flows
 *
 * Profile switch fires a user-supplied callback that loads/unloads the
 * actual streams; this header is transport-agnostic.
 */
#ifndef PWNZ_PEER_DEVICE_H
#define PWNZ_PEER_DEVICE_H

#include <stdbool.h>
#include <pipewire/impl.h>

struct peer_device;

/* Callback invoked when the user picks a new profile via pavucontrol /
 * pw-cli set-param / WP. `on` is true for profile 1 (On) and false for
 * profile 0 (Off). Returns 0 on success or a negative errno; the device
 * accepts the change either way.
 */
typedef int (*peer_device_profile_cb)(void *user_data, bool on);

struct peer_device_info {
	const char *node_name;       /* stable, e.g. network.uvi.alsa_output... */
	const char *node_description;
	const char *peer_host;
	bool default_on;             /* whether profile 1 (On) is initial */
};

/* Create + register the device. Returns NULL on failure (errno set). */
struct peer_device *peer_device_new(struct pw_context *context,
				    const struct peer_device_info *info,
				    peer_device_profile_cb cb,
				    void *user_data);

/* Destroy the device and unregister it. The profile callback is NOT
 * invoked during destroy; the caller is responsible for any teardown.
 */
void peer_device_destroy(struct peer_device *dev);

/* Get the registered pw_impl_device's global id, useful for setting
 * `device.id` on child stream nodes so PA-shim links sink-to-card. */
uint32_t peer_device_get_id(struct peer_device *dev);

/* Programmatic profile change (mirrors a set-param call). Triggers the
 * user callback if the value changes. */
int peer_device_set_profile(struct peer_device *dev, bool on);

bool peer_device_is_on(struct peer_device *dev);

#endif /* PWNZ_PEER_DEVICE_H */
