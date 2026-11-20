#ifndef PUZZLLM_WORKSPACE_H
#define PUZZLLM_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

/* Room numbers in archive names are one-based. */
bool migrate_workspace(const char *workspace, int number);
bool archive_workspace(const char *workspace, int number);
bool restore_workspace(const char *workspace, int number);
bool archive_guide(const char *workspace, int number, const char *name);
bool write_guide(const char *workspace, int number, const char *name, const char *temp,
                 const char *body, size_t length);

#endif
