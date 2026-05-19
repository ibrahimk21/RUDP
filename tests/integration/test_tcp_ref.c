#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void write_source(const char *path)
{
    static const char contents[] = "checked partial TCP reference transfer\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, contents, sizeof(contents)) == (ssize_t)sizeof(contents));
    assert(close(fd) == 0);
}

static void assert_same_file(const char *left, const char *right)
{
    char left_bytes[128];
    char right_bytes[128];
    int left_fd = open(left, O_RDONLY);
    int right_fd = open(right, O_RDONLY);
    ssize_t left_count;
    ssize_t right_count;
    assert(left_fd >= 0 && right_fd >= 0);
    left_count = read(left_fd, left_bytes, sizeof(left_bytes));
    right_count = read(right_fd, right_bytes, sizeof(right_bytes));
    assert(left_count == right_count && left_count > 0);
    assert(memcmp(left_bytes, right_bytes, (size_t)left_count) == 0);
    assert(close(left_fd) == 0 && close(right_fd) == 0);
}

int main(void)
{
    const char *build_dir = getenv("BUILD_DIR");
    char directory[] = "/tmp/rudp-tcp-ref-XXXXXX";
    char source[128];
    char output[128];
    char binary[512];
    char port[16];
    struct timespec startup = {0, 100000000L};
    pid_t receiver;
    pid_t sender;
    int receiver_status;
    int sender_status;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(source, sizeof(source), "%s/source", directory) > 0);
    assert(snprintf(output, sizeof(output), "%s/output", directory) > 0);
    assert(snprintf(binary, sizeof(binary), "%s/tcp_ref", build_dir == NULL ? "build" : build_dir) >
           0);
    assert(snprintf(port, sizeof(port), "%u", 40000U + (unsigned int)getpid() % 20000U) > 0);
    write_source(source);

    receiver = fork();
    assert(receiver >= 0);
    if (receiver == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0)
            (void)dup2(null_fd, STDOUT_FILENO);
        execl(binary, binary, "receive", port, output, "cubic", (char *)NULL);
        _exit(127);
    }
    assert(nanosleep(&startup, NULL) == 0 || errno == EINTR);
    sender = fork();
    assert(sender >= 0);
    if (sender == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0)
            (void)dup2(null_fd, STDOUT_FILENO);
        execl(binary, binary, "send", "127.0.0.1", port, source, "cubic", (char *)NULL);
        _exit(127);
    }
    assert(waitpid(sender, &sender_status, 0) == sender);
    assert(waitpid(receiver, &receiver_status, 0) == receiver);
    assert(WIFEXITED(sender_status) && WEXITSTATUS(sender_status) == 0);
    assert(WIFEXITED(receiver_status) && WEXITSTATUS(receiver_status) == 0);
    assert_same_file(source, output);
    assert(unlink(source) == 0 && unlink(output) == 0 && rmdir(directory) == 0);
    puts("TCP CUBIC reference transfer test passed");
    return 0;
}
