// FrameSource backed by a shared frame ring the payload creates and a helper fills.
#pragma once

#include <stdint.h>
#include "framesource.hpp"
#include "../common/shmframe.hpp"

namespace recam {

// Returns the memfd, which the payload publishes for a feeder to open.
bool shmsource_create(int* fdOut, uint64_t* sizeOut);

// Only after hook substitution has been disabled and in-flight callbacks drained.
void shmsource_destroy();

// nullptr if the ring was never created or has been released.
FrameSource* shmsource_get();

}  // namespace recam
