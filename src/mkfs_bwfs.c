/** En forma general se crea un FS con inodos y bloques de datos
Lee la configuracion desde config.ini

No implementa de forma distributiva
Interpreta 1000x1000 como bytes, no como píxeles.


Idea de esqueleto inicial pero falta cumplir especificaciones
 * 
 * 
*/
 
 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#define META_FILE "bwfs_metadata.bin"
#define MAX_FILES 1024

typedef struct {
    int used;
    char name[256];
    size_t size;
    int mode;
} inode_t;

typedef struct {
    char storage_path[512];
    size_t max_block_bytes;
    inode_t inodes[MAX_FILES];
} bwfs_t;


static int read_config(const char *filename, char *storage_path, size_t *max_block_bytes) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("config.ini");
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "storage_path = %511s", storage_path) == 1) continue;
        if (sscanf(line, "max_block_bytes = %zu", max_block_bytes) == 1) continue;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        printf("Uso: mkfs.bwfs -c config.ini\n");
        return 1;
    }

    char storage_path[512] = "bwfs_storage";
    size_t max_block_bytes = 1000000;

    if (read_config(argv[2], storage_path, &max_block_bytes) != 0) {
        fprintf(stderr, "Error leyendo config.ini, usando valores por defecto.\n");
    }

    bwfs_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.max_block_bytes = max_block_bytes;

    /* Convertir storage_path a absoluto */
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    snprintf(meta.storage_path, sizeof(meta.storage_path),
             "%s/%s", cwd, storage_path);

    /* Crear directorio de almacenamiento */
    mkdir(meta.storage_path, 0755);

    /* Guardar metadata */
    FILE *fm = fopen(META_FILE, "wb");
    if (!fm) {
        perror("mkfs: metadata");
        return 1;
    }
    fwrite(&meta, sizeof(meta), 1, fm);
    fclose(fm);

    /* Crear backing files */
    char path[PATH_MAX];
    for (int i = 0; i < MAX_FILES; ++i) {
        snprintf(path, sizeof(path), "%s/file_%04d.dat", meta.storage_path, i);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror("mkfs: fopen file");
            return 1;
        }
        fclose(f);
    }

    printf("mkfs.bwfs: FS creado en '%s' con %d archivos de datos.\n",
           meta.storage_path, MAX_FILES);
    return 0;
}
