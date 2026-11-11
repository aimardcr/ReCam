"""Human-readable diagnostics, shared by the UI and dependency-free tests."""


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


def diagnostic_summary(ring):
    diag = ring.get("diagnostics", {})
    messages = []
    if diag.get("unsupported_format"):
        fmt = diag.get("last_unsupported_format", 0)
        name = "Qualcomm UBWC" if fmt == 0x7FA30C06 else f"0x{fmt:x}"
        messages.append(f"unsupported format {name}: {diag['unsupported_format']} frames passed through")
    for key, label in (("lock_failed", "buffer lock failures"),
                       ("invalid_layout", "invalid plane layouts"),
                       ("unsupported_dimensions", "unsupported dimensions"),
                       ("no_free_lane", "frames without an available lane")):
        if diag.get(key):
            messages.append(f"{diag[key]} {label}")
    if diag.get("fallback_frames"):
        messages.append(f"{diag['fallback_frames']} fallback-bar frames")
    return "; ".join(messages) + " (since injection)" if messages else "No buffer warnings recorded"
