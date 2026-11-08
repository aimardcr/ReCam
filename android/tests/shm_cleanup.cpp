#include "../src/payload/shmsource.hpp"
#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
int main() {
    for(int i=0;i<100;++i){
        int fd=-1;uint64_t size=0;
        assert(recam::shmsource_create(&fd,&size));
        assert(fd>=0 && size>4096 && fcntl(fd,F_GETFD)>=0);
        assert(recam::shmsource_get() && recam::shmsource_header());
        recam::shmsource_destroy();
        assert(!recam::shmsource_get() && !recam::shmsource_base());
        assert(fcntl(fd,F_GETFD)<0);
        // Idempotent cleanup must not close an unrelated fd reusing its number.
        int other=open("/dev/null",O_RDONLY);assert(other>=0);
        recam::shmsource_destroy();assert(fcntl(other,F_GETFD)>=0);close(other);
    }
    puts("100 shared-ring cleanup/recreation checks passed");
}
