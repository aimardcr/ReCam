// FrameSource backed by a shared frame ring the payload creates and a helper fills.
#pragma once

#include <stdint.h>
#include "framesource.hpp"
#include "../common/shmframe.hpp"

namespace recam {

// Returns the memfd, which the payload publishes for a feeder to open.
bool shmsource_create(int* fdOut, uint64_t* sizeOut);

// nullptr if the ring was never created.
FrameSource* shmsource_get();

// The mapped ring; the self-test plays feeder through it.
ShmHeader* shmsource_header();
uint8_t*   shmsource_base();

}  // namespace recam
