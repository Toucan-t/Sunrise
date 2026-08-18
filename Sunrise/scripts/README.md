# Sunrise server scripts

These Lua files are intentionally external to the compiled shim DLL. At runtime the server resolves
the directory containing the actual loaded DLL and looks for a sibling `scripts` directory there.
The build copies this tree to the output directory so a normal development build is immediately
editable in place.

Activity scripts are session-scoped. Each joined activity session receives its own Lua VM and
persistent `sunrise.state` table. A supported activity composes two files in the same environment:

1. A larger reusable activity-type framework, for example `activities/strikes/main.lua`.
2. A smaller per-activity file, for example `activities/strikes/garden_world/main.lua`.

Saving either loaded file triggers a transactional hot reload. A syntax/runtime error leaves the
previous working environment active and logs the Lua error.

## First API

- `sunrise.log(text)`
- `sunrise.after(seconds, callback)`
- `sunrise.reload()`
- `sunrise.state` — per-session table preserved across successful hot reloads
- `activity.session_id()` — hex string, avoiding signed-64-bit Lua integer ambiguity
- `activity.name()` — destination/package name such as `strike_bond`
- `activity.type()` / `activity.script_name()`
- `activity.id()`
- `activity.region_index()` / `activity.region_hash()`
- `activity.state()` / `activity.state_revision()`
- `activity.set_state(value)` / `activity.advance_state()`

`activity.set_state` currently changes Sunrise's authoritative server-side session state only. It
logs `client_publish=pending`; the value is **not yet encoded into Destiny's activity-script/mission
director replication**. That missing retail binding is the next reverse-engineering target.

Optional callbacks are `on_start`, `on_reload`, `on_stop`, `on_region_entered(index, hash)`, and
`on_state_changed(previous, current, revision)`.
