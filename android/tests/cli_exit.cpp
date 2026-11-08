// Exercise the real multicall boundary: feed cleanup returns before process exit,
// buffered diagnostics survive, and unsafe global teardown is skipped only there.
#define main recam_main
#include "../src/cli/main.cpp"
#undef main
#include <cassert>
#include <cstdlib>
#include <sys/wait.h>
#include <string>

namespace recam::cli {
int feed(int, char**) { fputs("cleaned up\n", stdout); return 23; }
int inject() { return 0; }
int stop() { return 0; }
int status() { return 17; }
int test() { return 0; }
int watch() { return 0; }
}
static void destructor() { fputs("global teardown\n", stdout); }

static void check(const char* command, int expected, const char* output)
{
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(fds[0]);
        assert(dup2(fds[1], STDOUT_FILENO) >= 0);
        close(fds[1]);
        char buffer[1024];
        setvbuf(stdout, buffer, _IOFBF, sizeof(buffer));
        assert(atexit(destructor) == 0);
        char name[] = "recam";
        char* argv[] = {name, const_cast<char*>(command), nullptr};
        exit(recam_main(2, argv));
    }
    close(fds[1]);
    std::string got;
    char buf[128];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) got.append(buf, n);
    close(fds[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
    assert(got == output);
}

int main()
{
    check("feed", 23, "cleaned up\n");
    check("status", 17, "global teardown\n");
    puts("CLI process-exit checks passed");
}
