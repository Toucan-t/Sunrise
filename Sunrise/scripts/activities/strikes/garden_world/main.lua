-- A Garden World / strike_bond.
--
-- This first session-scoped scripting patch intentionally does not issue spawns or synthetic client
-- events. The server owns a mission-state value now, while the retail client-Script replication
-- path that consumes that state remains a separate reverse-engineering target.

function on_start()
    strike.start("A Garden World")
    sunrise.log(string.format(
        "Garden World script loaded: activity=%d destination=%s",
        activity.id(),
        activity.name()
    ))

    strike.after(2.0, function()
        sunrise.log("Garden World session Lua timer fired")
    end)
end

function on_region_entered(region_index, region_hash)
    sunrise.state.last_region_index = region_index
    sunrise.state.last_region_hash = region_hash
    sunrise.log(string.format(
        "Garden World entered region %d (0x%08X)",
        region_index,
        region_hash
    ))
end

function on_state_changed(previous, current, revision)
    sunrise.log(string.format(
        "Garden World server state changed %d -> %d (revision %d, client publish pending)",
        previous,
        current,
        revision
    ))
end

function on_reload()
    sunrise.state.reload_count = (sunrise.state.reload_count or 0) + 1
    sunrise.log("Garden World activity script hot reloaded")
end

function on_stop()
    sunrise.log("Garden World activity-session script stopped")
end
