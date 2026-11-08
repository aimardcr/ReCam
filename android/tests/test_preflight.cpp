#include "../src/injector/test_preflight.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using recam::camera_clients;
    using C = recam::CameraClients;
    assert(camera_clients("Active Camera Clients:\n[]\nAllowed user IDs: 0") == C::Idle);
    assert(camera_clients("header\nActive Camera Clients:\r\n[ \t\r\n]\n") == C::Idle);
    assert(camera_clients("Active Camera Clients:\n[(Camera ID: 0, PID: 123)]") == C::Active);
    assert(camera_clients("Active Camera Clients:\n[]\nCONNECT device 0") == C::Idle);
    assert(camera_clients("") == C::Unknown);
    assert(camera_clients("Permission Denial: can't dump CameraService") == C::Unknown);
    assert(camera_clients("Active Camera Clients:\n[") == C::Unknown);
    assert(camera_clients("Active Camera Clients: unavailable") == C::Unknown);
    assert(camera_clients("history: Active Camera Clients: []") == C::Unknown);
    assert(camera_clients("Active Camera Clients: []\nActive Camera Clients: []") == C::Unknown);
    puts("self-test preflight checks passed");
}
