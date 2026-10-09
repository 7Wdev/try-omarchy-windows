/* SPDX-License-Identifier: MIT */
#include "wddm-allocation-map.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    WddmAllocationCommand command;
    const char *bad[] = { "", "map", "map:", "map:0:4096:4096", "map:0:4096:4096:1:2",
        "map:-1:4096:4096:1", "map:+1:4096:4096:1", "map:128:4096:4096:1", "map:0:0:4096:1",
        "map:0:4097:4096:1", "map:0:4096:0:1", "map:0:4096:4097:1", "map:0:4096:1052672:1",
        "map:0:4096:4096:0", "map:0:140737488355328:4096:1", "map:0:140737488351232:8192:1",
        "map:0:18446744073709547520:4096:1", "map:0:18446744073709551616:4096:1",
        "map:0:4096:4096:18446744073709551616", "unmap:0", "unmap:0:0", "unmap:128:1",
        "unmap:0:1:", "unmap:0:1 ", "MAP:0:4096:4096:1" };
    for (size_t n = 0; n < sizeof bad / sizeof bad[0]; ++n) assert(!wddm_allocation_command(bad[n], &command));
    char long_input[130]; memset(long_input, '1', sizeof long_input); long_input[129] = 0;
    assert(!wddm_allocation_command(long_input, &command));
    assert(!wddm_allocation_command(NULL, &command)); assert(!wddm_allocation_command("unmap:0:1", NULL));
    assert(wddm_allocation_command("map:15:4096:1048576:10", &command));
    assert(command.map && command.slot == 15 && command.source == 4096 && command.bytes == 1048576 && command.generation == 10);
    assert(wddm_allocation_transition(&command, 9, 0));
    assert(!wddm_allocation_transition(&command, 10, 0)); assert(!wddm_allocation_transition(&command, 9, 7));
    assert(wddm_allocation_command("unmap:15:10", &command));
    assert(!command.map && !command.source && !command.bytes);
    assert(wddm_allocation_transition(&command, 10, 10));
    assert(!wddm_allocation_transition(&command, 10, 9)); assert(!wddm_allocation_transition(&command, 10, 0));
    assert(wddm_allocation_command("map:0:140737488351232:4096:18446744073709551615", &command));
    assert(wddm_allocation_command("map:31:4096:4096:1", &command) && command.slot == 31);
    assert(wddm_allocation_command("map:63:4096:4096:1", &command) && command.slot == 63);
    assert(wddm_allocation_command("unmap:63:1", &command));
    assert(wddm_allocation_command("map:127:4096:4096:1", &command) && command.slot == 127);
    assert(wddm_allocation_command("unmap:127:1", &command));
    for (uint32_t slots = 0; slots <= 256; ++slots)
        assert(wddm_allocation_slots_valid(slots) == (slots == 16 || slots == 32 || slots == 64 || slots == 128));
    puts("PASS: bounded writable allocation commands, range overflow and generation ownership");
    return 0;
}
