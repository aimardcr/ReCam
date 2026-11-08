#include "commands.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

static void usage(FILE* out)
{
    fputs("Usage: recam <command>\n"
          "  inject       Load the payload and install the camera hook\n"
          "  feed [FILE]  Feed video, live H.264, or a test pattern\n"
          "  stop         Remove the hook and restore the real camera\n"
          "  status       Print device and payload state as JSON\n"
          "  test         Run the payload self-test in cameraserver\n"
          "  watch        Re-inject when cameraserver respawns\n"
          "  --help       Show this help\n", out);
}

int main(int argc, char** argv)
{
    if (argc == 1) {
        usage(stderr);
        return 2;
    }
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout);
        return 0;
    }

    if (!strcmp(argv[1], "feed") && argc <= 3) {
        const int rc = recam::cli::feed(argc - 1, argv + 1);
        // feed() has closed codecs, sockets and mappings. Android's bufferpool
        // owns detached eviction threads with no public shutdown/join API.
        // Normal exit destroys their static mutexes while those threads run.
        // End the process after our cleanup, without platform static teardown.
        fflush(nullptr);
        _exit(rc);
    }

    struct Command { const char* name; int (*run)(); };
    static const Command commands[] = {
        {"inject", recam::cli::inject},
        {"stop", recam::cli::stop},
        {"status", recam::cli::status},
        {"test", recam::cli::test},
        {"watch", recam::cli::watch},
    };
    if (argc == 2) {
        for (const auto& command : commands)
            if (!strcmp(argv[1], command.name)) return command.run();
    }

    fprintf(stderr, "Unknown command or invalid arguments: %s\n", argv[1]);
    usage(stderr);
    return 2;
}
