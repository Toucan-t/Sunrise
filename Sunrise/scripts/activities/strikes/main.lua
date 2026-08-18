-- Shared strike framework.
--
-- Reusable strike lifecycle helpers belong here. Individual strike sequencing stays in that
-- strike's own subfolder so dozens of strikes can share one server-side framework.

strike = {}

function strike.start(name)
    sunrise.state.strike_name = name
    sunrise.state.start_count = (sunrise.state.start_count or 0) + 1
    sunrise.log(string.format(
        "strike framework started: %s session=%s server_state=%d",
        name,
        activity.session_id(),
        activity.state()
    ))
end

function strike.after(seconds, callback)
    return sunrise.after(seconds, callback)
end

function strike.state()
    return activity.state()
end

function strike.set_state(value)
    return activity.set_state(value)
end

function strike.advance_state()
    return activity.advance_state()
end

function strike.region_name(index)
    return string.format("PRV%02d.%02d", index, index)
end
