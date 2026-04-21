// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
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

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
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
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
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
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
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
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
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

// Forward declarations (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);
int object_exists(const ObjectID *id);
void object_path(const ObjectID *id, char *path_out, size_t path_size);

// Helper for qsort
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // Index doesn't exist yet, not an error

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        if (fscanf(f, "%o %64s %llu %u ", &e->mode, hex, &e->mtime_sec, &e->size) != 4)
            break;
        
        // Read the path (which might contain spaces, but let's assume no spaces for now
        // based on the standard simplified format. Or use fgets/custom logic.)
        // Actually, the format says: <mode> <hash> <mtime> <size> <path>
        // We can read the rest of the line as the path.
        if (fgets(e->path, sizeof(e->path), f) == NULL)
            break;
        
        // Remove trailing newline
        size_t len = strlen(e->path);
        if (len > 0 && e->path[len-1] == '\n') e->path[len-1] = '\0';
        
        // Trim leading space left by fscanf
        char *p = e->path;
        while (*p == ' ') p++;
        if (p != e->path) memmove(e->path, p, strlen(p) + 1);

        if (hex_to_hash(hex, &e->hash) != 0) break;

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // Sort entries by path
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", INDEX_FILE, (int)getpid());

    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted_index.count; i++) {
        const IndexEntry *e = &sorted_index.entries[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n", e->mode, hex, (unsigned long long)e->mtime_sec, e->size, e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(temp_path, INDEX_FILE) < 0) {
        unlink(temp_path);
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return -1;
    }

    void *contents = malloc(st.st_size);
    if (!contents && st.st_size > 0) {
        fclose(f);
        return -1;
    }
    if (st.st_size > 0 && fread(contents, 1, st.st_size, f) != (size_t)st.st_size) {
        perror("fread");
        free(contents);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID hash;
    if (object_write(OBJ_BLOB, contents, st.st_size, &hash) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    // Update or add entry
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path));
    }

    e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->hash = hash;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;

    return index_save(index);
}
