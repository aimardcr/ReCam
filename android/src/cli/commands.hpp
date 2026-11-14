#pragma once

namespace recam::cli {

int inject();
int stop();
int status();
int watch();
// argv[0] is "feed"; argv[1], if supplied, is the source file.
int feed(int argc, char** argv);

}  // namespace recam::cli
