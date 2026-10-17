#include <assert.h>
#include <spawn.h>
#include <sys/wait.h>
#include <net/if.h>
#include <string.h>

static unsigned interfaces[2], removals;
static int fail_second, child_status;
static unsigned test_if_index(const char *name) {
    return interfaces[!strcmp(name, "feth1")];
}
static int test_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                      const posix_spawnattr_t *attributes, char *const args[], char *const env[]) {
    (void)actions; (void)attributes; (void)env;
    assert(!strcmp(path, "/sbin/ifconfig") && !strcmp(args[2], "destroy"));
    unsigned i = !strcmp(args[1], "feth1");
    child_status = i && fail_second ? 256 : 0;
    if (!child_status) { interfaces[i] = 0; removals++; }
    *pid = 123;
    return 0;
}
static pid_t test_waitpid(pid_t pid, int *status, int options) {
    (void)options; *status = child_status; return pid;
}
#define if_nametoindex test_if_index
#define posix_spawn test_spawn
#define waitpid test_waitpid
#define main driver_main
#include "../edimax_driver.c"
#undef main

int main(void) {
    char path[] = "/private/tmp/edimax-lifecycle.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    struct timeval boot = { .tv_sec = 123, .tv_usec = 456 };
    unsigned owned[] = {41, 42};
    assert(recover_stale_interfaces(fd, &boot) == 0);
    interfaces[0] = 41; interfaces[1] = 42;
    assert(recover_stale_interfaces(fd, &boot) == -1 && removals == 0);
    assert(write(fd, "1234\n", 5) == 5); // Legacy PID files prove no interface ownership.
    assert(recover_stale_interfaces(fd, &boot) == -1 && removals == 0);
    assert(save_interface_state(fd, &boot, owned) == 0);
    struct timeval other_boot = boot; other_boot.tv_usec++;
    assert(recover_stale_interfaces(fd, &other_boot) == -1 && removals == 0);
    interfaces[1] = 99; // The name now belongs to a different interface.
    assert(recover_stale_interfaces(fd, &boot) == -1 && removals == 0 && interfaces[0] == 41);
    interfaces[1] = 42;
    fail_second = 1;
    assert(recover_stale_interfaces(fd, &boot) == -1 && interfaces[0] == 0 && interfaces[1] == 42);
    fail_second = 0;
    assert(recover_stale_interfaces(fd, &boot) == 0 && interfaces[1] == 0);
    interfaces[0] = 51;
    unsigned partial[] = {51, 0};
    assert(save_interface_state(fd, &boot, partial) == 0);
    assert(recover_stale_interfaces(fd, &boot) == 0 && interfaces[0] == 0);
    interfaces[0] = 51; interfaces[1] = 99;
    assert(remove_owned_interfaces(partial) == 0 && interfaces[0] == 0 && interfaces[1] == 99);
    assert(save_interface_state(-1, &boot, owned) == -1);
    close(fd); unlink(path);
    puts("Crash recovery, boot identity, partial cleanup, and foreign-interface protection passed.");
}
