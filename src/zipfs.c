#define FUSE_USE_VERSION 26

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#include "fuse_opt.h"
#include "zip.h"

#define VAL(x) #x
#define STR(x) VAL(x)

#define DEBUG_PRINT_PREFIX "[DEBUG " __FILE__ ":" STR(__LINE__) "] "

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define eprintfln(...)                                                         \
    do {                                                                       \
        eprintf(__VA_ARGS__);                                                  \
        eprintf("\n");                                                         \
    } while (0)

#if defined DEBUG
#define debug_eprintf(...) eprintf(DEBUG_PRINT_PREFIX __VA_ARGS__)
#else
#define debug_eprintf(...)
#endif

#if defined DEBUG
#define debug_eprintfln(...) eprintfln(DEBUG_PRINT_PREFIX __VA_ARGS__)
#else
#define debug_eprintfln(...)
#endif

#define DEFAULT_MIN_BUF_SIZE (4 * 1024 * 1024)
static size_t min_buf_size = DEFAULT_MIN_BUF_SIZE;
struct zip_buffer_t {
    int index;
    char *data;
    size_t buf_size;
    size_t entry_size;
};

static struct zip_buffer_t zip_buf;

static struct zip_t *zip;
static pthread_mutex_t zip_mutex;

#define MAX_ZIP_ENTRIES 65535
static char *zip_entry_names[MAX_ZIP_ENTRIES];

static struct zipfs_options {
    int show_help;
    size_t min_buf_size;
} zipfs_options;

#define ZIPFS_OPTION(t, p)                                                     \
    { t, offsetof(struct zipfs_options, p), 1 }
static const struct fuse_opt option_spec[] = {
    ZIPFS_OPTION("-h", show_help), ZIPFS_OPTION("--help", show_help),
    ZIPFS_OPTION("--min-buf=%zu", min_buf_size), FUSE_OPT_END};

inline static int starts_with(const char *pre, const char *str) {
    return strncmp(pre, str, strlen(pre)) == 0;
}

static void show_help(const char *progname) {
    eprintf("usage: %s <zip-file> <mountpoint> [options]\n\n", progname);
    eprintf("general options:\n"
            "    -h | --help         print help\n"
            "    -V | --version      print version\n"
            "\n");
    eprintf("file-system specific options:\n"
            "    --min-buf           Minimal buffer size in bytes for reading "
            "zip entries\n"
            "\n");
}

static void *zipfs_init(struct fuse_conn_info *conn) {
    (void)conn;

    debug_eprintfln("zipfs has initialized");
    return NULL;
}

static void zipfs_destroy(void *arg) {
    (void)arg;
    debug_eprintfln("zipfs has been destroyed");
}

static void zipfs_entry_getattr(struct zip_t *zip, struct stat *stbuf) {
    if (zip_entry_isdir(zip)) {
        debug_eprintfln("Entry is dir");
        stbuf->st_mode = S_IFDIR | 0755;
        // For consistency, always set the size of a dir to 0.
    } else {
        debug_eprintfln("Entry is regular file");
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = zip_entry_size(zip);
    }
    debug_eprintfln("Size of the entry is %lld",
                    (unsigned long long)stbuf->st_size);
}

static int zipfs_getattr(const char *path, struct stat *stbuf) {
    int ret = 0;
    memset(stbuf, 0, sizeof(struct stat));
    pthread_mutex_lock(&zip_mutex);

    if (strcmp(path, "/") == 0) {
        debug_eprintfln("Path is '/'");
        stbuf->st_mode = S_IFDIR | 0755;
    } else {
        debug_eprintfln("Entry is '%s'", path + 1);
        if (zip_entry_open(zip, path + 1) == 0) {
            // The entry can be directly opened.
            // We can infer it is likely to be a regular file.
            zipfs_entry_getattr(zip, stbuf);
            zip_entry_close(zip);
        } else {
            // Entry might be a directory
            for (int i = 0; i < MAX_ZIP_ENTRIES && zip_entry_names[i] != NULL;
                 ++i) {
                // Since the entry cannot be opened defore,
                // we can determine that the entry name must not be included
                // in zip_entry_names. Thus, any other entry name starting with
                // it must be longer than it.
                if (starts_with(path + 1, zip_entry_names[i]) &&
                    zip_entry_names[i][strlen(path + 1)] == '/') {
                    debug_eprintfln("Path '%s' is dir", path);
                    stbuf->st_mode = S_IFDIR | 0755;
                    goto unlock;
                }
            }
            debug_eprintfln("Entry '%s' cannot be opened", path + 1);
            ret = -ENOENT;
        }
    }

unlock:
    pthread_mutex_unlock(&zip_mutex);
    return ret;
}

static int zipfs_open(const char *path, struct fuse_file_info *fi) {
    int ret = 0;
    pthread_mutex_lock(&zip_mutex);

    if (strcmp(path, "/") == 0) {
        debug_eprintfln("Path '/' cannot be opened");
        ret = -ENOENT;
        goto unlock;
    }

    if (zip_entry_open(zip, path + 1) != 0) {
        debug_eprintfln("Entry '%s' cannot be opened", path + 1);
        // For simplicity, we return -ENOENT, but it could also be -EISDIR.
        ret = -ENOENT;
        goto unlock;
    }

    if (zip_entry_isdir(zip)) {
        debug_eprintfln("Entry '%s' is dir", path + 1);
        ret = -EISDIR;
        goto cleanup;
    }

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        debug_eprintfln("Access mode is not read-only");
        ret = -EACCES;
        goto cleanup;
    }

    fi->fh = zip_entry_index(zip);
    debug_eprintfln("Entry index is %" PRIu64, fi->fh);

cleanup:
    zip_entry_close(zip);
unlock:
    pthread_mutex_unlock(&zip_mutex);
    return ret;
}

static int zipfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    int ret;
    int index;

    pthread_mutex_lock(&zip_mutex);

    if (fi == NULL) {
        if (strcmp(path, "/") == 0) {
            debug_eprintfln("Path '/' cannot be read");
            ret = -ENOENT;
            goto unlock;
        }

        debug_eprintfln("Invoked with path '%s'", path);

        if (zip_entry_open(zip, path + 1) != 0) {
            ret = -ENOENT;
            goto unlock;
        }
        index = zip_entry_index(zip);
    } else {
        index = fi->fh;
        debug_eprintfln("Invoked with index %d", index);

        if (zip_entry_openbyindex(zip, index) != 0) {
            ret = -ENOENT;
            goto unlock;
        }
    }

    if (zip_entry_isdir(zip)) {
        debug_eprintfln("Entry '%s' is dir", path + 1);
        ret = -EISDIR;
        goto cleanup;
    }

    if (zip_buf.data == NULL) {
        debug_eprintfln("Buffer has not initialized");
        size_t entry_size = zip_entry_size(zip);
        debug_eprintfln("Entry size is %zu", entry_size);
        size_t buf_size = min_buf_size > entry_size ? min_buf_size : entry_size;
        zip_buf.data = (char *)malloc(buf_size);
        if (zip_buf.data == NULL) {
            perror("malloc()");
            ret = -errno;
            goto cleanup;
        }
        debug_eprintfln("Buffer with size %zu allocated", buf_size);
        zip_buf.buf_size = buf_size;
        zip_buf.entry_size = entry_size;
        zip_buf.index = index;
        // To be strict, only use entry_size instead of buf_size.
        assert(zip_entry_noallocread(zip, (void *)zip_buf.data,
                                     zip_buf.entry_size) != -1);
    } else if (zip_buf.index != index) {
        debug_eprintfln("Entry with index %d not found", index);
        size_t entry_size = zip_entry_size(zip);
        debug_eprintfln("Entry size is %zu", entry_size);
        if (zip_buf.buf_size < entry_size ||
            (zip_buf.buf_size > entry_size &&
             zip_buf.buf_size > min_buf_size)) {
            size_t buf_size =
                min_buf_size > entry_size ? min_buf_size : entry_size;
            char *new_data = (char *)realloc(zip_buf.data, buf_size);
            if (new_data == NULL) {
                perror("realloc()");
                ret = -errno;
                goto cleanup;
            }
            debug_eprintfln("Buffer with size %zu reallocated", buf_size);
            zip_buf.data = new_data;
            zip_buf.buf_size = buf_size;
        }
        zip_buf.entry_size = entry_size;
        zip_buf.index = index;
        // To be strict, only use entry_size instead of buf_size.
        assert(zip_entry_noallocread(zip, (void *)zip_buf.data,
                                     zip_buf.entry_size) != -1);
    }

    if (offset >= zip_buf.entry_size) {
        debug_eprintfln("Offset %lld is out of bound for entry size %zu",
                        offset, zip_buf.entry_size);
        ret = 0;
        goto cleanup;
    }
    ret =
        offset + size > zip_buf.entry_size ? zip_buf.entry_size - offset : size;
    memcpy(buf, zip_buf.data + offset, ret);
    debug_eprintfln("%d byte(s) copied to buffer from offset %lld", ret,
                    offset);

cleanup:
    zip_entry_close(zip);
unlock:
    pthread_mutex_unlock(&zip_mutex);
    return ret;
}

static int zipfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t off, struct fuse_file_info *fi) {
    (void)off;
    (void)fi;

    int ret = 0;
    int dir_index = -1;
    char dir_name[MAX_PATH] = "";
    char prev_name[MAX_PATH] = "";

    debug_eprintfln("Invoked with path '%s'", path);
    pthread_mutex_lock(&zip_mutex);

    if (strcmp(path, "/") != 0) {
        if (zip_entry_open(zip, path + 1) == 0) {
            if (!zip_entry_isdir(zip)) {
                debug_eprintfln("Entry '%s' is not dir", path + 1);
                ret = -ENOTDIR;
                zip_entry_close(zip);
                goto unlock;
            }

            dir_index = zip_entry_index(zip);
            debug_eprintfln("Dir entry index is %d", dir_index);

            zip_entry_close(zip);
        } else {
            debug_eprintfln("zip_entry_open() '%s' error", path + 1);
        }
        strcpy(dir_name, path + 1);
        strcat(dir_name, "/");
    } else {
        debug_eprintfln("Path is '/'");
    }

    debug_eprintfln("Dir name is resolved as '%s'", dir_name);

    for (int i = dir_index + 1;
         i < MAX_ZIP_ENTRIES && zip_entry_names[i] != NULL; ++i) {
        const char *name = zip_entry_names[i];

        debug_eprintfln("Current entry name is '%s'", name);

        if (!starts_with(dir_name, name)) {
            if (dir_index == -1) {
                continue;
            } else {
                break;
            }
        }

        struct stat st;
        memset(&st, 0, sizeof(struct stat));

        const char *local_name = name + strlen(dir_name);
        char *slashp = (char *)strchr(local_name, '/');

        if (slashp == NULL) {
            // The entry is a sub-file or sub-directory.
            if (strcmp(prev_name, local_name) != 0) {
                strcpy(prev_name, local_name);

                assert(zip_entry_open(zip, name) == 0);
                zipfs_entry_getattr(zip, &st);
                zip_entry_close(zip);

                filler(buf, local_name, &st, 0);
                debug_eprintfln("Entry '%s' filled", local_name);
            }
        } else {
            // The entry is a direct sub-file or sub-directory,
            // but its name implies that a sub-directory should exist.
            *slashp = '\0';
            if (strcmp(prev_name, local_name) != 0) {
                strcpy(prev_name, local_name);

                st.st_mode = S_IFDIR | 0755;

                filler(buf, local_name, &st, 0);
                debug_eprintfln("Entry '%s' filled", local_name);
            }
            *slashp = '/';
        }
    }

unlock:
    pthread_mutex_unlock(&zip_mutex);
    return ret;
}

static int zipfs_read_all_entry_names(char **entry_names, struct zip_t *zip) {
    int n = zip_total_entries(zip);
    debug_eprintfln("Total entries are %d", n);
    for (int i = 0; i < n && i < MAX_ZIP_ENTRIES; ++i) {
        assert(zip_entry_openbyindex(zip, i) == 0);
        const char *name = zip_entry_name(zip);
        entry_names[i] = (char *)malloc(strlen(name));
        if (entry_names[i] == NULL) {
            perror("malloc()");
            zip_entry_close(zip);
            return -1;
        }

        strcpy(entry_names[i], name);

        debug_eprintfln("Current entry name is '%s'", entry_names[i]);
        zip_entry_close(zip);
    }

    return 0;
}

static void zipfs_free_all_entry_names(char **entry_names) {
    for (int i = 0; i < MAX_ZIP_ENTRIES && entry_names[i] != NULL; ++i) {
        free(entry_names[i]);
    }
}

static struct fuse_operations zipfs_operations = {.getattr = zipfs_getattr,
                                                  .open = zipfs_open,
                                                  .read = zipfs_read,
                                                  .readdir = zipfs_readdir,
                                                  .init = zipfs_init,
                                                  .destroy = zipfs_destroy};

int main(int argc, char **argv) {
    int ret;

    char zip_file[PATH_MAX] = {0};

    if (argc >= 3) {
        if (realpath(argv[1], zip_file) == NULL) {
            eprintf("Resolve zip file path '%s' error", argv[1]);
            argv[1] = "--help";
            argv[2] = NULL;
            argc = 2;
        } else {
            zip = zip_open(zip_file, 0, 'r');
            if (zip == NULL) {
                eprintfln("Open ZIP file '%s' error", zip_file);
                return 1;
            }

            if (zipfs_read_all_entry_names(zip_entry_names, zip) != 0) {
                eprintfln("Read all entry names failed");
                return 1;
            }

            ++argv;
            --argc;
        }
    } else if (argc == 2) {
        if (argv[1][0] != '-') {
            argv[1] = "--help";
        }
    } else {
        char *new_argv[] = {argv[0], "--help", NULL};
        argv = new_argv;
        argc = 2;
    }

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &zipfs_options, option_spec, NULL) == -1) {
        eprintfln("Parse options error");
        return 1;
    }

    // When --help is specified, first print our own file-system specific help
    // text, then signal fuse_main to show additional help (by adding `--help`
    // to the options again) without usage: line (by setting argv[0] to the
    // empty string)
    if (zipfs_options.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "-ho") == 0);
    }

    ret = fuse_main(args.argc, args.argv, &zipfs_operations, NULL);
    fuse_opt_free_args(&args);

    zip_close(zip);
    free(zip_buf.data);
    zipfs_free_all_entry_names(zip_entry_names);

    return ret;
}
