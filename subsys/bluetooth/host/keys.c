/* keys.c - Bluetooth key handling */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <string.h>
#include <atomic.h>
#include <misc/util.h>

#include <settings/settings.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/hci.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_KEYS)
#include "common/log.h"

#include "common/rpa.h"
#include "hci_core.h"
#include "smp.h"
#include "settings.h"
#include "keys.h"

static struct bt_keys key_pool[CONFIG_BT_MAX_PAIRED];

struct bt_keys *bt_keys_get_addr(const bt_addr_le_t *addr)
{
	struct bt_keys *keys;
	int i;

	BT_DBG("%s", bt_addr_le_str(addr));

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		keys = &key_pool[i];

		if (!bt_addr_le_cmp(&keys->addr, addr)) {
			return keys;
		}

		if (!bt_addr_le_cmp(&keys->addr, BT_ADDR_LE_ANY)) {
			bt_addr_le_copy(&keys->addr, addr);
			BT_DBG("created %p for %s", keys, bt_addr_le_str(addr));
			return keys;
		}
	}

	BT_DBG("unable to create keys for %s", bt_addr_le_str(addr));

	return NULL;
}

void bt_keys_foreach(int type, void (*func)(struct bt_keys *keys))
{
	int i;

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		if ((key_pool[i].keys & type)) {
			func(&key_pool[i]);
		}
	}
}

struct bt_keys *bt_keys_find(int type, const bt_addr_le_t *addr)
{
	int i;

	BT_DBG("type %d %s", type, bt_addr_le_str(addr));

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		if ((key_pool[i].keys & type) &&
		    !bt_addr_le_cmp(&key_pool[i].addr, addr)) {
			return &key_pool[i];
		}
	}

	return NULL;
}

struct bt_keys *bt_keys_get_type(int type, const bt_addr_le_t *addr)
{
	struct bt_keys *keys;

	BT_DBG("type %d %s", type, bt_addr_le_str(addr));

	keys = bt_keys_find(type, addr);
	if (keys) {
		return keys;
	}

	keys = bt_keys_get_addr(addr);
	if (!keys) {
		return NULL;
	}

	bt_keys_add_type(keys, type);

	return keys;
}

struct bt_keys *bt_keys_find_irk(const bt_addr_le_t *addr)
{
	int i;

	BT_DBG("%s", bt_addr_le_str(addr));

	if (!bt_addr_le_is_rpa(addr)) {
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		if (!(key_pool[i].keys & BT_KEYS_IRK)) {
			continue;
		}

		if (!bt_addr_cmp(&addr->a, &key_pool[i].irk.rpa)) {
			BT_DBG("cached RPA %s for %s",
			       bt_addr_str(&key_pool[i].irk.rpa),
			       bt_addr_le_str(&key_pool[i].addr));
			return &key_pool[i];
		}
	}

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		if (!(key_pool[i].keys & BT_KEYS_IRK)) {
			continue;
		}

		if (bt_rpa_irk_matches(key_pool[i].irk.val, &addr->a)) {
			BT_DBG("RPA %s matches %s",
			       bt_addr_str(&key_pool[i].irk.rpa),
			       bt_addr_le_str(&key_pool[i].addr));

			bt_addr_copy(&key_pool[i].irk.rpa, &addr->a);

			return &key_pool[i];
		}
	}

	BT_DBG("No IRK for %s", bt_addr_le_str(addr));

	return NULL;
}

struct bt_keys *bt_keys_find_addr(const bt_addr_le_t *addr)
{
	int i;

	BT_DBG("%s", bt_addr_le_str(addr));

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		if (!bt_addr_le_cmp(&key_pool[i].addr, addr)) {
			return &key_pool[i];
		}
	}

	return NULL;
}

void bt_keys_add_type(struct bt_keys *keys, int type)
{
	keys->keys |= type;
}

void bt_keys_clear(struct bt_keys *keys)
{
	BT_DBG("%s (keys 0x%04x)", bt_addr_le_str(&keys->addr), keys->keys);

	if (keys->keys & BT_KEYS_IRK) {
		bt_id_del(keys);
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		char key[BT_SETTINGS_KEY_MAX];

		/* Delete stored keys from flash */
		bt_settings_encode_key(key, sizeof(key), "keys", &keys->addr,
				       NULL);
		BT_DBG("Deleting key %s", key);
		settings_save_one(key, NULL);
	}

	memset(keys, 0, sizeof(*keys));
}

void bt_keys_clear_all(void)
{
	bt_keys_foreach(BT_KEYS_ALL, bt_keys_clear);
}

#if defined(CONFIG_BT_SETTINGS)
int bt_keys_store(struct bt_keys *keys)
{
	char val[BT_SETTINGS_SIZE(BT_KEYS_STORAGE_LEN)];
	char key[BT_SETTINGS_KEY_MAX];
	char *str;
	int err;

	str = settings_str_from_bytes(keys->storage_start, BT_KEYS_STORAGE_LEN,
				      val, sizeof(val));
	if (!str) {
		BT_ERR("Unable to encode bt_keys as value");
		return -EINVAL;
	}

	bt_settings_encode_key(key, sizeof(key), "keys", &keys->addr, NULL);

	err = settings_save_one(key, val);
	if (err) {
		BT_ERR("Failed to save keys (err %d)", err);
		return err;
	}

	BT_DBG("Stored keys for %s (%s)", bt_addr_le_str(&keys->addr), key);

	return 0;
}

static int keys_set(int argc, char **argv, char *val)
{
	struct bt_keys *keys;
	bt_addr_le_t addr;
	int len, err;

	if (argc < 1) {
		BT_ERR("Insufficient number of arguments");
		return -EINVAL;
	}

	BT_DBG("argv[0] %s val %s", argv[0], val ? val : "(null)");

	err = bt_settings_decode_key(argv[0], &addr);
	if (err) {
		BT_ERR("Unable to decode address %s", argv[0]);
		return -EINVAL;
	}

	if (!val) {
		keys = bt_keys_find(BT_KEYS_ALL, &addr);
		if (keys) {
			memset(keys, 0, sizeof(*keys));
			BT_DBG("Cleared keys for %s", bt_addr_le_str(&addr));
		} else {
			BT_WARN("Unable to find deleted keys for %s",
				bt_addr_le_str(&addr));
		}

		return 0;
	}

	keys = bt_keys_get_addr(&addr);
	if (!keys) {
		BT_ERR("Failed to allocate keys for %s", bt_addr_le_str(&addr));
		return -ENOMEM;
	}

	len = BT_KEYS_STORAGE_LEN;
	err = settings_bytes_from_str(val, keys->storage_start, &len);
	if (err) {
		BT_ERR("Failed to decode value (err %d)", err);
		bt_keys_clear(keys);
		return err;
	}

	if (len != BT_KEYS_STORAGE_LEN) {
		BT_ERR("Invalid key length %d != %d", len, BT_KEYS_STORAGE_LEN);
		bt_keys_clear(keys);
		return -EINVAL;
	}

	BT_DBG("Successfully restored keys for %s", bt_addr_le_str(&addr));
	return 0;
}

static int keys_commit(void)
{
	BT_DBG("");

	/* We do this in commit() rather than add() since add() may get
	 * called multiple times for the same address, especially if
	 * the keys were already removed.
	 */
	bt_keys_foreach(BT_KEYS_IRK, (bt_keys_func_t)bt_id_add);

	return 0;
}

BT_SETTINGS_DEFINE(keys, keys_set, keys_commit, NULL);
#endif /* CONFIG_BT_SETTINGS */
