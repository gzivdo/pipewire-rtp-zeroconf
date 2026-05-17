# Profile-flip fix: cache invalidation + dedup + property sync

Дата: 2026-05-17. Версии: PW 1.0.5 (test-pc1 / Ubuntu 24.04), PW 1.6.2 (test-pc2 / Ubuntu 26.04), WirePlumber 0.4.17.

## Симптом

В `pavucontrol` на app-host: выбор профиля (`Output` / `Input` / `Output+Input`) для network-RTP карт «передёргивается» — клик в один профиль, через секунду откатывается. После закрытия и повторного открытия `pavucontrol` выбор сбрасывается. Иногда всплывало KDE-уведомление «обнаружена новая звуковая карта».

## Корни (3 независимых)

### 1. Дубли peer_device от Avahi IPv4+IPv6

`avahi-browse -trp _pipewire-rtp._udp` показал, что **один и тот же сервис** анонсируется дважды — раз через IPv4, раз через IPv6 (одинаковый session UUID, разные `=;enp195s0;IPv4;…` / `=;enp195s0;IPv6;…`).

Наш resolver_cb создавал два `peer_device` для одной карты, и каждый set_param обрабатывался дважды. `find_card()` по (peer_host, card_name) находил существующий card, но `find_tunnel()` шёл по avahi-service-name (разный для IPv4/IPv6) — поэтому дубликатов не отлавливал.

**Фикс:** ввёл `peer_card.loaded_dirs` — bitmask направлений уже загруженных tunnel'ов. Устанавливается **сразу после `spa_list_append(card->tunnels)`** (то есть до выхода из обработчика), второй event для того же direction видит флаг и `goto done` с `pw_log_info("dup service ...")`. Сбрасывается в `tunnel_free` для корректного hot-unplug/replug.

См. `module-mdns-rtp-discover.c` — поиск `loaded_dirs`.

### 2. Server-side кэш Profile param никогда не инвалидировался

`pw-metadata` показывает наш `value:'input'` (source-of-truth), `pw-cli enum-params $DEV Profile` возвращает `Int 3 "output+input"` (stale). `impl_enum_params` **никогда не вызывался** (трассировка через `pw_log_warn`). Значит `pw_impl_device` отдаёт Profile из своего внутреннего кэша.

Пробовали:
- `params[i].user = ++params_serial` (monotonic) + `SPA_PARAM_INFO_SERIAL` flag → НЕ работает на 1.0.5.
- Убрать `SPA_PARAM_INFO_SERIAL` → НЕ помогает (всё равно кэшируется).
- Persistent `info` + `params[]` в struct (вместо стека) → НЕ помогает.

**Реальный фикс** найден в `spa/plugins/alsa/alsa-acp-device.c:226-233` — на каждом `emit_info`, для каждого param с `user > 0`:
```c
p->flags ^= SPA_PARAM_INFO_SERIAL;   // XOR toggle bit
p->user   = 0;                       // reset counter
```

`pw_impl_device` отслеживает изменение **`flags`**, не значения `user`. Bit `SPA_PARAM_INFO_SERIAL` тогглится при каждой эмиссии — для PW это сигнал «param-info изменился, кэш invalidate».

Также:
- `info.params` указывает на persistent `dev->params[N_INFO_PARAMS]` в struct (стабильный pointer для PW сравнения через emit'ы).
- `info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS` ставится при изменении и сбрасывается в `0` после `emit_info`.

См. `peer-device.c` — поиск `XOR toggle` / `IDX_Profile`.

### 3. `device.profile` property рассинхронизирован с `Profile.current`

`impl_set_param` напрямую писал `this->active_dirs = new_dirs` и `cb()`, но **не звал** `sync_device_profile_prop()`. Снаружи (через PA-bridge / pavucontrol) приходил set_param → active менялся → property оставался от прошлого внутреннего set'а → `pactl list cards` показывал «Активный профиль» по property = старое значение → клиент пытался «исправить» → второй set_param.

**Фикс:** `sync_device_profile_prop(this)` теперь вызывается:
- в `peer_device_set_active_dirs` (даже на no-op — для consistency после WP-стрипа props),
- в `impl_set_param` (внешний set от pavucontrol/pulse),
- в `peer_device_set_available_dirs` (clamp может изменить active).

## Что ещё пробовали и отбросили

- **Priority boost** активного профиля (+10000) в EnumProfile — сломало pulse-bridge active-profile calculation. Откатили.
- **Disable destroy node on profile change** (`tunnel_set_loaded(true)` всегда) — устраняет KDE-нотификацию, но семантически неверно (нода активна вне профиля). Откатили.
- `pulse-bridge` / `policy-device-profile.lua` (WP 0.4.x) как «overrider» — оказалось ложным следом, реальной перезаписи active не было (доказали через статический CALL#-counter в `impl_set_param` — реально вызов был один).
- Лог-дубли в `journalctl` — артефакт journald, не двойная обработка (тот же CALL#).

## Подтверждение (тест 13:01)

```
pw-cli enum-params $DEV Profile   # BEFORE: Int 3
pw-cli s $DEV Profile 'index: 2'  # set to input
pw-cli enum-params $DEV Profile   # AFTER: Int 2    ← работает!
pw-dump ... → current="input"
pactl list cards → "Активный профиль: input"
journalctl ... → impl_enum_params id=9 присутствует
```

В pavucontrol: профиль больше не флипает, запоминается между запусками.

## Открытые вопросы (отдельные тикеты)

- **Дубли в journalctl** — `set_param Profile ... CALL#3` пишется 2-3 раза подряд при один реальный call. Артефакт systemd-journal merge, не код. Заметка: возможно `_TRANSPORT=stdout` + `_TRANSPORT=syslog` для того же pid сливаются.
- **Один port 46032 для request-record при выборе Input на 2 картах** — port allocation в request-record back-channel (Stage 24b) не уникализирует по карте. Должно быть 46032 + 46033. Регрессия после дедупа или давний баг — TBD.

## Файлы изменены

- `src/peer-device.c` — persistent `info`+`params[]`, XOR SERIAL toggle, sync property из всех путей.
- `src/peer-device.h` — без изменений (API сохранён).
- `src/module-mdns-rtp-discover.c` — `peer_card.loaded_dirs` дедуп, sync property сохранён, avail-changed reapplies stored profile.
- `src/common.h` — `PWNZ_TXT_*` для card-name (давно).

## Reference

- `spa/plugins/alsa/alsa-acp-device.c:198-238` — эталонный `emit_info`.
- `spa/plugins/bluez5/bluez5-device.c:1115` — аналогичный паттерн.
- `src/pipewire/impl-device.c:619-755` — `pw_impl_device` обработка info_changed / params_changed.
