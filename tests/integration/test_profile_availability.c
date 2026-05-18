#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *ready = getenv("RUDP_BENCHMARK_READY");

    if (ready == NULL || strcmp(ready, "1") != 0) {
        puts("UNAVAILABLE: 3 live profile cases (terrestrial, GEO, synthetic LEO); "
             "validated CAP_NET_ADMIN topology is required for Phase 8");
        return 0;
    }
    fputs("RUDP_BENCHMARK_READY=1 requires the Phase 8 isolated topology harness; "
          "refusing to claim live profile coverage\n",
          stderr);
    return 1;
}
