#define _GNU_SOURCE
#include "workspace.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_folder(int parent, const char *name, bool create)
{
    if (create && mkdirat(parent, name, 0700) < 0 && errno != EEXIST) return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Retrying an interrupted move never overwrites a namesake. */
static bool move_entry(int source, const char *name, int dest, const char *base)
{
    char candidate[256];
    snprintf(candidate, sizeof candidate, "%s", base);
    for (unsigned suffix = 1; ; suffix++) {
        if (renameat2(source, name, dest, candidate, RENAME_NOREPLACE) == 0 ||
            errno == ENOENT) return true;
        if (errno != EEXIST && errno != ENOTEMPTY) return false;
        snprintf(candidate, sizeof candidate, "%.240s.%u", base, suffix);
    }
}

static bool is_guide(const char *name)
{
    return !strcmp(name, "GOAL.md") || !strcmp(name, "HINTS.md");
}

static bool move_guide(int source, const char *name, int prior, int number)
{
    char archived[48];
    snprintf(archived, sizeof archived, "%.*s_level_%d.md", (int)strcspn(name, "."), name, number);
    return move_entry(source, name, prior, archived);
}

static bool archive_guide_at(int home, int number, const char *name)
{
    struct stat st;
    if (fstatat(home, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return errno == ENOENT;
    int prior = open_folder(home, "prior_levels", true);
    if (prior < 0) return false;
    bool ok = move_guide(home, name, prior, number);
    close(prior);
    return ok;
}

bool archive_guide(const char *workspace, int number, const char *name)
{
    int home = open_folder(AT_FDCWD, workspace, false);
    if (home < 0) return false;
    bool ok = archive_guide_at(home, number, name);
    close(home);
    return ok;
}

static bool guide_matches(int home, const char *name, const char *body, size_t length)
{
    int fd = openat(home, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool same = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)length;
    for (size_t offset = 0; same && offset < length;) {
        char buf[512];
        size_t want = length - offset < sizeof buf ? length - offset : sizeof buf;
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || memcmp(buf, body + offset, (size_t)n)) same = false;
        else offset += (size_t)n;
    }
    close(fd);
    return same;
}

bool write_guide(const char *workspace, int number, const char *name, const char *temp,
                 const char *body, size_t length)
{
    int home = open_folder(AT_FDCWD, workspace, false);
    if (home < 0) return false;
    bool ok = guide_matches(home, name, body, length);
    if (!ok) {
        unlinkat(home, temp, 0);
        int fd = openat(home, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            ok = write(fd, body, length) == (ssize_t)length;
            if (close(fd) != 0) ok = false;
            if (ok) ok = archive_guide_at(home, number, name) &&
                         renameat2(home, temp, home, name, RENAME_NOREPLACE) == 0;
            if (!ok) unlinkat(home, temp, 0);
        }
    }
    close(home);
    return ok;
}

enum transfer_kind { STORE, RESTORE, MIGRATE };

static bool transfer_files(int source, int dest, int prior, int number, enum transfer_kind kind)
{
    int scan = open_folder(source, ".", false);
    if (scan < 0) return false;
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return false; }
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { ok = errno == 0; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (is_guide(name)) {
            if (kind != STORE) ok = move_guide(source, name, prior, number);
            continue;
        }
        if (kind == STORE && (!strcmp(name, "shell.log") || !strcmp(name, "docs") ||
            !strcmp(name, ".system") || !strcmp(name, "prior_levels")))
            continue;
        ok = move_entry(source, name, dest, name);
    }
    closedir(dir);
    return ok;
}

static bool transfer_room(const char *workspace, int number, enum transfer_kind kind)
{
    char folder[32];
    snprintf(folder, sizeof folder, "level_%d", number);
    int home = open_folder(AT_FDCWD, workspace, false);
    if (home < 0) return false;
    if (kind == MIGRATE) {
        struct stat st;
        int status = fstatat(home, folder, &st, AT_SYMLINK_NOFOLLOW);
        if (status < 0 || !S_ISDIR(st.st_mode)) {
            bool absent = status == 0 || errno == ENOENT;
            close(home);
            return absent;
        }
    }
    bool create = kind != RESTORE;
    int prior = open_folder(home, "prior_levels", create);
    bool ok = prior >= 0 || (!create && errno == ENOENT);
    if (prior >= 0) {
        int room = open_folder(prior, folder, create);
        ok = room >= 0 || (!create && errno == ENOENT);
        if (room >= 0) {
            if (kind == MIGRATE) {
                int old = open_folder(home, folder, false);
                ok = old >= 0 && transfer_files(old, room, prior, number, MIGRATE);
                if (old >= 0) close(old);
                if (ok) ok = unlinkat(home, folder, AT_REMOVEDIR) == 0 || errno == ENOENT;
            }
            if (ok && kind == STORE) ok = transfer_files(home, room, prior, number, STORE);
            if (ok && kind == RESTORE) ok = transfer_files(room, home, prior, number, RESTORE);
            close(room);
        }
        close(prior);
    }
    close(home);
    return ok;
}

bool archive_workspace(const char *workspace, int number)
{
    return transfer_room(workspace, number, STORE);
}

bool restore_workspace(const char *workspace, int number)
{
    return transfer_room(workspace, number, RESTORE);
}

bool migrate_workspace(const char *workspace, int number)
{
    return transfer_room(workspace, number, MIGRATE);
}
