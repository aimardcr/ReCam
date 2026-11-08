#pragma once

#include <string_view>

namespace recam {

enum class CameraClients { Unknown, Idle, Active };

// Fail closed on unsupported dumpsys layouts; event history is not current state.
inline CameraClients camera_clients(std::string_view dump)
{
    constexpr std::string_view marker = "Active Camera Clients:";
    const auto pos = dump.find(marker);
    if (pos == std::string_view::npos || (pos && dump[pos - 1] != '\n') ||
        dump.find(marker, pos + marker.size()) != std::string_view::npos)
        return CameraClients::Unknown;
    auto rest = dump.substr(pos + marker.size());
    constexpr std::string_view whitespace = " \t\r\n";
    const auto begin = rest.find_first_not_of(whitespace);
    if (begin == std::string_view::npos || rest[begin] != '[')
        return CameraClients::Unknown;
    rest.remove_prefix(begin + 1);
    const auto end = rest.find(']');
    if (end == std::string_view::npos) return CameraClients::Unknown;
    return rest.substr(0, end).find_first_not_of(whitespace) == std::string_view::npos
        ? CameraClients::Idle : CameraClients::Active;
}

} // namespace recam
