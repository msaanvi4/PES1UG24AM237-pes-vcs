// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Write an object to the store.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob"; break;
        case OBJ_TREE:   type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 1. Build the full object: header ("type size\0") + data
    char header[64];
    int header_len = sprintf(header, "%s %zu", type_str, len) + 1;
    size_t total_len = header_len + len;
    uint8_t *full_obj = malloc(total_len);
    if (!full_obj) return -1;
    memcpy(full_obj, header, header_len);
    memcpy(full_obj + header_len, data, len);

    // 2. Compute SHA-256 hash of the FULL object
    compute_hash(full_obj, total_len, id_out);

    // 3. Check if object already exists (deduplication)
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // 4. Create shard directory (.pes/objects/XX/) if it doesn't exist
    char path[512];
    object_path(id_out, path, sizeof(path));
    
    char dir[512];
    strncpy(dir, path, sizeof(dir));
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755); // Ignore error if it exists
    }

    // 5. Write to a temporary file in the same shard directory
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", path, (int)getpid());
    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }
    if (write(fd, full_obj, total_len) != (ssize_t)total_len) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // 6. fsync() the temporary file to ensure data reaches disk
    fsync(fd);
    close(fd);

    // 7. rename() the temp file to the final path (atomic on POSIX)
    if (rename(temp_path, path) < 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // 8. Open and fsync() the shard directory to persist the rename
    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_obj);
    return 0;
}

// Read an object from the store.
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build the file path from the hash using object_path()
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *content = malloc(file_size);
    if (!content) {
        fclose(f);
        return -1;
    }
    if (fread(content, 1, file_size, f) != (size_t)file_size) {
        fclose(f);
        free(content);
        return -1;
    }
    fclose(f);

    // 4. Verify integrity: recompute the SHA-256 of the file contents
    // and compare to the expected hash (from *id). Return -1 if mismatch.
    ObjectID actual_id;
    compute_hash(content, file_size, &actual_id);
    if (memcmp(id->hash, actual_id.hash, HASH_SIZE) != 0) {
        free(content);
        return -1;
    }

    // 3. Parse the header to extract the type string and size
    char *null_byte = memchr(content, '\0', file_size);
    if (!null_byte) {
        free(content);
        return -1;
    }

    char type_name[16];
    size_t sz;
    if (sscanf((char*)content, "%15s %zu", type_name, &sz) != 2) {
        free(content);
        return -1;
    }

    // 5. Set *type_out to the parsed ObjectType
    if (strcmp(type_name, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_name, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_name, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(content);
        return -1;
    }

    // 6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
    size_t header_len = (null_byte - (char*)content) + 1;
    size_t data_len = file_size - header_len;
    
    // Safety check: header size should match actual data length
    if (data_len != sz) {
        // Optional: you could return -1 here if you want strict header matching
    }

    *data_out = malloc(data_len + 1);  /* +1 for null terminator safety */
    if (!*data_out) {
        free(content);
        return -1;
    }
    memcpy(*data_out, content + header_len, data_len);
    ((char *)*data_out)[data_len] = '\0';  /* null-terminate for text objects */
    *len_out = data_len;

    free(content);
    return 0;
}

// Phase 4: commit_create integrates tree_from_index, head_read, and head_update
