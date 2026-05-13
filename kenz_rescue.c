#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Path to the source directory (amba_files), set at mount time */
static char source_dir[4096];

/* Name of the virtual file */
#define VIRTUAL_FILE "/tujuan.txt"

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Build full path in source_dir for a given FUSE path */
static void full_path(char *out, size_t n, const char *path)
{
    snprintf(out, n, "%s%s", source_dir, path);
}

/*
 * Build the content of the virtual tujuan.txt on-the-fly.
 * Scans 1.txt .. 7.txt for lines starting with "KOORD: " and
 * concatenates them (in file order) to form:
 *   "Tujuan Mas Amba: <gabungan_fragmen>\n"
 */
static char *build_tujuan(size_t *out_len)
{
    /* Collect all KOORD fragments from files 1..7 */
    char fragments[7][512];
    int  nfrag = 0;

    for (int i = 1; i <= 7; i++) {
        char fpath[4096];
        snprintf(fpath, sizeof(fpath), "%s/%d.txt", source_dir, i);

        FILE *f = fopen(fpath, "r");
        if (!f) continue;

        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "KOORD: ", 7) == 0) {
                /* strip trailing newline */
                size_t l = strlen(line);
                if (l > 0 && line[l-1] == '\n') line[l-1] = '\0';
                /* store the part after "KOORD: " */
                strncpy(fragments[nfrag], line + 7, sizeof(fragments[nfrag]) - 1);
                fragments[nfrag][sizeof(fragments[nfrag])-1] = '\0';
                nfrag++;
                break; /* one KOORD per file */
            }
        }
        fclose(f);
    }

    /* Build "Tujuan Mas Amba: frag1frag2...frag7\n" */
    char buf[4096] = "Tujuan Mas Amba: ";
    for (int i = 0; i < nfrag; i++)
        strncat(buf, fragments[i], sizeof(buf) - strlen(buf) - 1);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    size_t len = strlen(buf);
    char  *result = malloc(len + 1);
    if (!result) { *out_len = 0; return NULL; }
    memcpy(result, buf, len + 1);
    *out_len = len;
    return result;
}

/* ------------------------------------------------------------------ */
/*  FUSE callbacks                                                      */
/* ------------------------------------------------------------------ */

static int kr_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    (void) fi;
    memset(st, 0, sizeof(*st));

    if (strcmp(path, VIRTUAL_FILE) == 0) {
        /* Virtual file: fake a regular read-only file */
        size_t len;
        char  *data = build_tujuan(&len);
        st->st_mode  = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size  = (off_t)(data ? len : 0);
        st->st_uid   = 0;
        st->st_gid   = 0;
        /* timestamps match epoch (1970) to look "virtual" */
        st->st_atime = st->st_mtime = st->st_ctime = 0;
        free(data);
        return 0;
    }

    char fp[4096];
    full_path(fp, sizeof(fp), path);

    if (lstat(fp, st) == -1)
        return -errno;
    return 0;
}

static int kr_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void) offset; (void) fi; (void) flags;

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* Real files from source_dir */
    char fp[4096];
    full_path(fp, sizeof(fp), "/");
    DIR *dp = opendir(fp);
    if (!dp) return -errno;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        filler(buf, de->d_name, NULL, 0, 0);
    }
    closedir(dp);

    /* Add virtual file */
    filler(buf, "tujuan.txt", NULL, 0, 0);

    return 0;
}

static int kr_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, VIRTUAL_FILE) == 0) {
        /* Allow read-only opens */
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        return 0;
    }

    char fp[4096];
    full_path(fp, sizeof(fp), path);

    int fd = open(fp, fi->flags);
    if (fd == -1) return -errno;

    fi->fh = (uint64_t) fd;
    return 0;
}

static int kr_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    if (strcmp(path, VIRTUAL_FILE) == 0) {
        size_t len;
        char  *data = build_tujuan(&len);
        if (!data) return -ENOMEM;

        size_t bytes = 0;
        if ((size_t)offset < len) {
            bytes = len - (size_t)offset;
            if (bytes > size) bytes = size;
            memcpy(buf, data + offset, bytes);
        }
        free(data);
        return (int) bytes;
    }

    /* Passthrough */
    int fd = (int) fi->fh;
    ssize_t res = pread(fd, buf, size, offset);
    if (res == -1) return -errno;
    return (int) res;
}

static int kr_release(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, VIRTUAL_FILE) == 0)
        return 0;
    close((int) fi->fh);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Operations table & main                                             */
/* ------------------------------------------------------------------ */

static struct fuse_operations kr_ops = {
    .getattr = kr_getattr,
    .readdir = kr_readdir,
    .open    = kr_open,
    .read    = kr_read,
    .release = kr_release,
};

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <source_directory> <mount_directory>\n",
                argv[0]);
        return 1;
    }

    /* Resolve source_dir to absolute path */
    if (!realpath(argv[1], source_dir)) {
        perror("realpath");
        return 1;
    }

    /*
     * Build new argv for FUSE:
     *   argv[0]   = program name
     *   argv[1]   = mount_directory
     *   (optional) -f for foreground, -s for single-thread
     * We add -f so it stays in foreground (easier for demo).
     */
    char *fuse_argv[] = {
        argv[0],
        argv[2],   /* mount point */
        "-f",      /* foreground */
        "-s",      /* single-threaded */
        NULL
    };
    int fuse_argc = 4;

    return fuse_main(fuse_argc, fuse_argv, &kr_ops, NULL);
}
