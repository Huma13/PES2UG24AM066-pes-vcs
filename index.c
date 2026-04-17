// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// PROVIDED functions: index_find, index_remove, index_status
// IMPLEMENTED functions: index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Load the index from .pes/index.
// If the file does not exist, initializes an empty index (not an error).
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // no index file yet — empty index is fine

    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long mtime;
        unsigned int size;
        char path[512];

        if (sscanf(line, "%o %64s %lu %u %511s",
                   &mode, hex, &mtime, &size, path) != 5)
            continue;

        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) continue;

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically (temp file + rename).
//
// KEY FIX: sort on a HEAP-allocated copy, NOT a stack copy.
// Index struct is ~5MB — putting it on the stack causes stack overflow.
int index_save(const Index *index) {
    // Allocate sorted copy on the heap
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    memcpy(sorted, index, sizeof(Index));
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_entries);

    FILE *f = fopen(".pes/index.tmp", "w");
    if (!f) { free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %lu %u %s\n",
                sorted->entries[i].mode,
                hex,
                (unsigned long)sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    if (rename(".pes/index.tmp", INDEX_FILE) != 0) {
        unlink(".pes/index.tmp");
        return -1;
    }
    return 0;
}

// Stage a file: read contents, write blob, update index entry.
int index_add(Index *index, const char *path) {
    // 1. Stat the file
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    // 2. Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    void *data = malloc(file_size + 1); // +1 handles zero-size files safely
    if (!data) { fclose(f); return -1; }

    size_t bytes_read = fread(data, 1, file_size, f);
    fclose(f);

    // 3. Write as blob to object store
    ObjectID id;
    if (object_write(OBJ_BLOB, data, bytes_read, &id) != 0) {
        free(data);
        fprintf(stderr, "error: failed to write blob for '%s'\n", path);
        return -1;
    }
    free(data);

    // 4. Determine mode
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // 5. Update existing entry or append new one
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        e = &index->entries[index->count++];
    }

    e->mode      = mode;
    e->hash      = id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';

    // 6. Persist
    return index_save(index);
}
