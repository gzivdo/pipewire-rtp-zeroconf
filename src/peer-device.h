/* SPDX-License-Identifier: MIT
 *
 * SPA Device representing one remote peer card. Exposes
 * SPA_PARAM_EnumProfile + SPA_PARAM_Profile so pavucontrol (and any
 * other PW Configuration-tab UI) shows the card with a Profile dropdown.
 *
 * The available profiles depend on which directions the card actually
 * offers (i.e. which TXT services have been seen for it on the LAN):
 *
 *   available_dirs       available profiles
 *   -----------------    -------------------------------
 *   OUTPUT               Off, Output
 *   INPUT                Off, Input
 *   OUTPUT | INPUT       Off, Output, Output+Input
 *
 * Profile switch fires a user-supplied callback with the new active
 * direction bitmask; the callback is responsible for loading /
 * unloading the actual rtp child modules per direction.
 */
#ifndef PWNZ_PEER_DEVICE_H
#define PWNZ_PEER_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <pipewire/impl.h>

struct peer_device;

enum peer_dir {
	PEER_DIR_OUTPUT = (1u << 0),  /* card sends audio to the LAN; we create local sink */
	PEER_DIR_INPUT  = (1u << 1),  /* card receives audio from the LAN; we create local source */
};

typedef int (*peer_device_profile_cb)(void *user_data, uint32_t active_dirs);

struct peer_device_info {
	const char *node_name;
	const char *node_description;
	const char *peer_host;
	uint32_t available_dirs;     /* OR of PEER_DIR_* */
	uint32_t default_dirs;       /* initial active state (subset of available) */
};

/* Create + register the device. Returns NULL on failure (errno set). */
struct peer_device *peer_device_new(struct pw_context *context,
				    const struct peer_device_info *info,
				    peer_device_profile_cb cb,
				    void *user_data);

void peer_device_destroy(struct peer_device *dev);

uint32_t peer_device_get_id(struct peer_device *dev);

/* Programmatic profile change. Fires the user callback if active_dirs
 * differs from the current state. Returns -EINVAL if active_dirs has
 * bits set that aren't in available_dirs. */
int peer_device_set_active_dirs(struct peer_device *dev, uint32_t active_dirs);

uint32_t peer_device_get_active_dirs(struct peer_device *dev);

/* Update what the card offers (e.g. a new direction service appears or
 * disappears on the LAN). Re-emits info so pavucontrol refreshes its
 * profile dropdown. If the current active_dirs falls outside the new
 * available_dirs, it is clamped (and the callback fires). */
int peer_device_set_available_dirs(struct peer_device *dev,
				   uint32_t available_dirs);

#endif /* PWNZ_PEER_DEVICE_H */
