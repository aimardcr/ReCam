"""Camera lane status formatting."""


def lane_summary(lane):
    width, height = lane.get("width", 0), lane.get("height", 0)
    requested = f"{lane.get('req_width', width)}x{lane.get('req_height', height)}"
    state = lane.get("state", "served" if lane.get("served") else "waiting_for_frame")
    description = {
        "served": "served",
        "feeder_stopped": "feeder stopped; fallback bars",
        "waiting_for_frame": "waiting for frame; fallback bars",
        "stale_frame": "stale frame; fallback bars",
        "unsupported_dimensions": "unsupported dimensions",
    }.get(state, state)
    if lane.get("scaled"):
        requested += f" (scaled from {width}x{height})"
    age = lane.get("age_ms", -1)
    return f"{requested}: {description}" + (f", {age} ms" if age >= 0 else "")
