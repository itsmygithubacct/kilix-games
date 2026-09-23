#define _POSIX_C_SOURCE 200809L
#include "chess_bash.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *log_path;

static void sleep_ms(long ms)
{
    struct timespec delay = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static Engine fresh_engine(void)
{
    Engine e = {0};
    e.in_fd = -1;
    e.out_fd = -1;
    return e;
}

static bool reset_fake(const char *mode)
{
    FILE *f = fopen(log_path, "w");
    if (!f) return false;
    fclose(f);
    return setenv("FAKE_UCI_MODE", mode, 1) == 0;
}

static bool log_contains(const char *needle)
{
    FILE *f = fopen(log_path, "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool wait_for_log(const char *needle, int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 5) {
        if (log_contains(needle)) return true;
        sleep_ms(5);
    }
    return log_contains(needle);
}

static int await_result(Engine *e, char move[8], int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 5) {
        int result = engine_poll(e, move, NULL, 0);
        if (result != ENGINE_PENDING) return result;
        sleep_ms(5);
    }
    return ENGINE_PENDING;
}

static bool stopped_cleanly(const Engine *e)
{
    return !e->ok && e->pid == 0 && e->in_fd == -1 && e->out_fd == -1;
}

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        goto out; \
    } \
} while (0)

static bool test_start_and_bestmove(void)
{
    bool passed = false;
    Engine e = fresh_engine();
    char move[8] = {0};

    REQUIRE(reset_fake("normal"), "reset normal fake engine");
    REQUIRE(engine_start(&e), "synchronous UCI startup");
    REQUIRE(e.ok && e.pid > 0 && e.in_fd >= 0 && e.out_fd >= 0,
            "startup owns a live process and both pipes");
    REQUIRE(strcmp(e.name, "Chess Bash Fake UCI") == 0,
            "startup records the engine name");
    REQUIRE(engine_bestmove(&e, "", 120, move, NULL, 0),
            "synchronous bestmove succeeds");
    REQUIRE(strcmp(move, "e2e4") == 0, "synchronous bestmove is preserved");
    REQUIRE(log_contains("setoption name Skill Level value 3"),
            "difficulty option reaches the UCI peer");
    passed = true;

out:
    engine_stop(&e);
    if (!stopped_cleanly(&e)) {
        fprintf(stderr, "FAIL: synchronous cleanup resets process and descriptors\n");
        passed = false;
    }
    return passed;
}

static bool test_async_start(void)
{
    bool passed = false;
    Engine e = fresh_engine();
    char move[8] = {0};

    REQUIRE(reset_fake("normal"), "reset async fake engine");
    REQUIRE(engine_request(&e, "", 450), "async startup request accepted");
    REQUIRE(!engine_request(&e, "", 450), "second request rejected while busy");
    REQUIRE(await_result(&e, move, 4000) == ENGINE_DONE,
            "async startup and search complete");
    REQUIRE(e.ok, "async startup leaves the engine reusable");
    REQUIRE(strcmp(move, "e2e4") == 0, "async bestmove is published");
    passed = true;

out:
    engine_cancel_all();
    engine_stop(&e);
    if (!stopped_cleanly(&e)) {
        fprintf(stderr, "FAIL: async cleanup resets process and descriptors\n");
        passed = false;
    }
    return passed;
}

static bool test_cancel_and_reuse(void)
{
    bool passed = false;
    Engine e = fresh_engine();
    char move[8] = {0};
    pid_t original_pid = 0;

    REQUIRE(reset_fake("cancel"), "reset cancellable fake engine");
    REQUIRE(engine_request(&e, "", 1500), "cancellable request accepted");
    REQUIRE(wait_for_log("go 1", 4000), "first search reaches the UCI peer");

    engine_cancel_all();
    original_pid = e.pid;
    REQUIRE(original_pid > 0, "cancellable request started a process");
    REQUIRE(e.ok && e.pid == original_pid,
            "cancel drains bestmove and retains the engine process");
    REQUIRE(log_contains("stop"), "cancel sends the UCI stop command");

    REQUIRE(engine_request(&e, "e2e4 e7e5", 450),
            "request accepted after cancellation");
    REQUIRE(await_result(&e, move, 4000) == ENGINE_DONE,
            "reused engine completes its next search");
    REQUIRE(strcmp(move, "d7d5") == 0,
            "reuse cannot consume the cancelled search result");
    REQUIRE(e.pid == original_pid, "reuse does not restart a healthy engine");
    passed = true;

out:
    engine_cancel_all();
    engine_stop(&e);
    if (!stopped_cleanly(&e)) {
        fprintf(stderr, "FAIL: cancelled engine cleanup resets process and descriptors\n");
        passed = false;
    }
    return passed;
}

static bool test_eof_failure(const char *mode, const char *label)
{
    bool passed = false;
    Engine e = fresh_engine();
    char move[8] = {0};

    REQUIRE(reset_fake(mode), "reset EOF fake engine");
    REQUIRE(engine_request(&e, "", 120), label);
    REQUIRE(await_result(&e, move, 4000) == ENGINE_FAILED,
            "EOF is reported as an engine failure");
    REQUIRE(stopped_cleanly(&e), "EOF failure reaps the process and closes pipes");
    passed = true;

out:
    engine_cancel_all();
    engine_stop(&e);
    return passed;
}

int main(int argc, char **argv)
{
    const char *fake_engine = argc > 1 ? argv[1] : "tests/fake-uci.sh";
    if (access(fake_engine, X_OK) != 0) {
        fprintf(stderr, "FAIL: fake UCI engine is not executable: %s\n", fake_engine);
        return 1;
    }
    if (setenv("STOCKFISH", fake_engine, 1) != 0) {
        perror("setenv STOCKFISH");
        return 1;
    }

    char temp[] = "/tmp/chess-bash-engine-lifecycle.XXXXXX";
    int fd = mkstemp(temp);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    log_path = temp;
    if (setenv("FAKE_UCI_LOG", log_path, 1) != 0) {
        perror("setenv FAKE_UCI_LOG");
        unlink(log_path);
        return 1;
    }

    int failures = 0;
    if (!test_start_and_bestmove()) failures++;
    if (!test_async_start()) failures++;
    if (!test_cancel_and_reuse()) failures++;
    if (!test_eof_failure("eof-start", "startup EOF request accepted")) failures++;
    if (!test_eof_failure("eof-search", "search EOF request accepted")) failures++;

    unlink(log_path);
    if (failures) {
        fprintf(stderr, "engine-lifecycle: %d test%s failed\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: fake-UCI startup, bestmove, cancellation/reuse, EOF, cleanup\n");
    return 0;
}
