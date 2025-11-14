/**
 * Este codigo es una base que implementa un sistema de archivos llamado BWFS
usando la biblioteca FUSE, al inicializar carga el metadata guardada en el
mkfs.bwfs (ruta de almacenamiento, tamaño máximo de bloque e inodos) y 
normaliza la ruta; con getattr devuelve los atributos de archivos y d
irectorios; con readdir lista el contenido del directorio raíz mostrando
los nombres de los inodos activos; con create permite crear nuevos 
archivos asignando un inodo y generando un archivo físico en el directorio
de almacenamiento; y con open valida que un archivo exista para poder 
abrirlo. En resumen, el código define la lógica básica de un sistema de
archivos virtual que puede montar, listar y crear archivos, pero aún 
falta añadir las operaciones de lectura y escritura para que sea 
completamente funcional.

creo que el gettatr deberia devolver los permisos correctos y funciona

Estoy teniendo problemas con create en crear un archivo, open me parece que
funciona y readdir me parece que funciona
 * 
 */

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_FILES 1024
#define MAX_NAME_LEN 255
#define META_FILENAME "bwfs_metadata.bin"

typedef struct {
    int used;
    char name[256];
    size_t size;
    int mode;
} inode_disk_t;

/* Metadata layout persisted by mkfs.bwfs */
typedef struct {
    char storage_path[512];
    size_t max_block_bytes;
    inode_disk_t inodes[MAX_FILES];
} bwfs_disk_t;

/* In-memory inode */
typedef struct {
    int used;
    char name[MAX_NAME_LEN + 1];
    mode_t mode;
    uid_t uid;
    gid_t gid;
    size_t size;
    time_t atime;
    time_t mtime;
    time_t ctime;
} inode_t;

typedef struct {
    inode_t inodes[MAX_FILES];
    char storage_path[512];
    size_t max_block_bytes;
} bwfs_t;

static bwfs_t bwfs;

/* ---- Persistence helpers ---- */
static int load_metadata(void) {
    bwfs_disk_t disk;
    FILE *fp = fopen(META_FILENAME, "rb");
    if (!fp) return -1;
    size_t r = fread(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    if (r != 1) return -1;

    /* Copy core fields */
    strncpy(bwfs.storage_path, disk.storage_path, sizeof(bwfs.storage_path) - 1);
    bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
    bwfs.max_block_bytes = disk.max_block_bytes;

    /* Map disk inodes to runtime inodes */
    for (int i = 0; i < MAX_FILES; i++) {
        bwfs.inodes[i].used = disk.inodes[i].used;
        strncpy(bwfs.inodes[i].name, disk.inodes[i].name, MAX_NAME_LEN);
        bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
        bwfs.inodes[i].mode = (disk.inodes[i].used ? (S_IFREG | 0644) : 0);
        bwfs.inodes[i].uid = getuid();
        bwfs.inodes[i].gid = getgid();
        bwfs.inodes[i].size = disk.inodes[i].size;
        time_t now = time(NULL);
        bwfs.inodes[i].atime = now;
        bwfs.inodes[i].mtime = now;
        bwfs.inodes[i].ctime = now;
    }
    return 0;
}

static int save_metadata(void) {
    /* Persist only fields expected by mkfs.bwfs layout */
    bwfs_disk_t disk;
    memset(&disk, 0, sizeof(disk));
    strncpy(disk.storage_path, bwfs.storage_path, sizeof(disk.storage_path) - 1);
    disk.max_block_bytes = bwfs.max_block_bytes;
    for (int i = 0; i < MAX_FILES; i++) {
        disk.inodes[i].used = bwfs.inodes[i].used;
        strncpy(disk.inodes[i].name, bwfs.inodes[i].name, sizeof(disk.inodes[i].name) - 1);
        disk.inodes[i].size = bwfs.inodes[i].size;
        disk.inodes[i].mode = bwfs.inodes[i].mode;
    }

    FILE *fp = fopen(META_FILENAME, "wb");
    if (!fp) return -1;
    size_t w = fwrite(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    return (w == 1) ? 0 : -1;
}

 
static const char *basename_from_path(const char *path) {
    return (path[0] == '/') ? path + 1 : path;
}

static int find_inode_by_name(const char *path) {
    const char *fname = basename_from_path(path);
    if (!fname || !fname[0]) return -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && strcmp(bwfs.inodes[i].name, fname) == 0)
            return i;
    }
    return -1;
}

static int allocate_inode(const char *path, mode_t mode) {
    const char *fname = basename_from_path(path);
    if (!fname || !fname[0]) return -EINVAL;
    if (strlen(fname) > MAX_NAME_LEN) return -ENAMETOOLONG;
    if (find_inode_by_name(path) >= 0) return -EEXIST;

    for (int i = 0; i < MAX_FILES; ++i) {
        if (!bwfs.inodes[i].used) {
            bwfs.inodes[i].used = 1;
            strncpy(bwfs.inodes[i].name, fname, MAX_NAME_LEN);
            bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
            bwfs.inodes[i].mode = S_IFREG | (mode ? (mode & 0777) : 0644);
            bwfs.inodes[i].uid = getuid();
            bwfs.inodes[i].gid = getgid();
            bwfs.inodes[i].size = 0;
            time_t now = time(NULL);
            bwfs.inodes[i].atime = bwfs.inodes[i].mtime = bwfs.inodes[i].ctime = now;

            /* Prepare backing file path and create/truncate it */
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, i);
            int fd = open(filepath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd < 0) {
                /* Rollback inode allocation on failure */
                bwfs.inodes[i].used = 0;
                bwfs.inodes[i].name[0] = '\0';
                return -EIO;
            }
            close(fd);
            (void)save_metadata();
            return i;
        }
    }
    return -ENOSPC;
}

/* Normalize storage_path to absolute without creating directories */
static void normalize_storage_path(void) {
    if (bwfs.storage_path[0] == '/') return; /* already absolute */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        char abs_storage[PATH_MAX];
        snprintf(abs_storage, sizeof(abs_storage), "%s/%s", cwd, bwfs.storage_path);
        strncpy(bwfs.storage_path, abs_storage, sizeof(bwfs.storage_path) - 1);
        bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
    }
}

/* ---- FUSE: init ---- */
static void *bwfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1;

    if (load_metadata() != 0) {
        /* Minimal defaults when metadata missing */
        memset(&bwfs, 0, sizeof(bwfs));
        strncpy(bwfs.storage_path, "bwfs_storage", sizeof(bwfs.storage_path) - 1);
        bwfs.max_block_bytes = 1000000;
        (void)save_metadata();
    }
    normalize_storage_path();

    fprintf(stderr, "[bwfs_init] storage_path='%s'\n", bwfs.storage_path);
    return NULL;
}

/* ---- FUSE: getattr ---- */
static int bwfs_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    int idx = find_inode_by_name(path);
    if (idx < 0) return -ENOENT;

    inode_t *ino = &bwfs.inodes[idx];
    stbuf->st_mode  = ino->mode;
    stbuf->st_nlink = 1;
    stbuf->st_size  = ino->size;
    stbuf->st_uid   = ino->uid;
    stbuf->st_gid   = ino->gid;
    stbuf->st_atime = ino->atime;
    stbuf->st_mtime = ino->mtime;
    stbuf->st_ctime = ino->ctime;

    return 0;
}

/* ---- FUSE: readdir ---- */
static int bwfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;

    /* Solo la raíz es un directorio válido por ahora */
    if (strcmp(path, "/") != 0) return -ENOENT;

    /* Entradas estándar */
    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* Lista de archivos (inodos usados) */
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && bwfs.inodes[i].name[0] != '\0') {
            filler(buf, bwfs.inodes[i].name, NULL, 0, 0);
        }
    }
    return 0;
}


/* ---- FUSE: create ---- */
static int bwfs_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {
    (void)fi;
    int idx = allocate_inode(path, mode);
    return (idx < 0) ? idx : 0;
}

/* ---- FUSE: open ---- */
static int bwfs_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    int idx = find_inode_by_name(path);
    return (idx < 0) ? -ENOENT : 0;
}

/* ---- Operations table ---- */
static struct fuse_operations bwfs_oper = {
    .init    = bwfs_init,
    .getattr = bwfs_getattr,
    .create  = bwfs_create,
    .open    = bwfs_open,
    .readdir = bwfs_readdir,  
};

/* ---- main ---- */
int main(int argc, char *argv[]) {
    /* Strip custom -c config if provided (kept for CLI compatibility) */
    const char *config_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[i + 1];
            for (int j = i + 2; j < argc; j++) argv[j - 2] = argv[j];
            argc -= 2;
            break;
        }
    }
    if (config_file) {
        printf("Usando configuración: %s\n", config_file);
        /* Note: mkfs.bwfs already persisted metadata; bwfs.c uses bwfs_metadata.bin */
    }
    return fuse_main(argc, argv, &bwfs_oper, NULL);
}
