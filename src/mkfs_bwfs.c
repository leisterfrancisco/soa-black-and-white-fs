/**
 *  En este codigo como actualizacion
 * 
 *  Muestra la creacion de un nuevo FS BWFS, donde se inicializa el bwfs_t, crea
 *  bwfs_metadata.bin y backing files.
 * 
 *  Luego se establece el máximo bloque de 1000x1000px en la constante de
 *  MAX_BLOCK_BYTES y validación en main 
 * 
 *  Se utilizan los inodos como indexación, estructura de inode_t incluida bwfs_t
 * 
 *  FS distribuido, Crea 1024 archivos (file_0000.dat … file_1023.dat) en el storage.
 *  
 *  Configuración desde ini, Función read_config lee storage_path y max_block_bytes.
 * 
 *  Sintaxis mkfs.bwfs -c config.ini, Validación de argumentos al inicio
 * 
 *  Explicacion sobre diseño:
 *  Creacion de 1024 archivos vacíos dentro del directorio de almacenamiento, con el objetivo
 *  de que represente un bloque de datos de forma independiente, entonces en lugar de tener un único
 *  archivo grande, el FS se distribuye en muchos archivos pequeños. Esto facilita la gestión, donde
 *  se puede mapear cada archivo a un inodo y simular un sistema que sea distribuido
 * 
 *  Para la especificacion de que cada bloque BWFS debe tener un máximo de 1000x10000px, en
 *  el bloque para el archivo file_xxx.dat puede que almacene 1 000 000 bytes de forma equivalente
 *  a una matriz de 1000x1000 píxeles considerando que cada píxel necesita 1 byte. La forma que se puede
 *  interpretar es que cada archivo es un contenedor de imagen, entonces el limite asegura que ningun bloque
 *  supere el tamaño máximo definido por la especificación.
 * 
 *  El funcionamiento de los inodos de indexación, donde cada inodo corresponde a un archivo
 *  dentro del FS, used es para indicar que si el inodo esta ocupado, name es el nombre del archivo logico dentro
 *  del FS, size es el tamaño del archivo, mode para los permisos y tipo. Como funcion los inodos son la tabla 
 *  de indexacion que enlaza los nombres de archivos del FS con los backing files (file_xxxx.dat). Cuando el 
 *  usuario ingresa el comando touch para crear un archivo pues se asigna un inodo libre y se enlaza con un bloque 
 *  fisico
 * 
 *  Para lograr que sea distribuido, El FS no guarda todo en un único archivo, en cambio se tiene que
 *  Metadata (bwfs_metadata.bin) guarda fingerprint, parámetros y tabla de inodos, Storage (bwfs_storage/)
 *  contiene 1024 backing files. Cada archivo lógico del FS puede mapearse a uno o varios backing files. Esto simula
 *  un sistema distribuido los datos están repartidos en múltiples contenedores.
 * 
 *  Para obtener la configuración de .ini con la función read_config, abre config.ini y busca parámetros
 *  storage_path donde crea la ruta que se crean los backing files, max_block_byte es el tamño máximo de bloque
 *  . Si no se encuentra los valores válido pues usa defaults (bwfs_storage, MAX_BLOCK_BYTES).
 * 
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

// Archivo del metadata donde se busca almacenar el fingerprint y parámetros
#define META_FILE "bwfs_metadata.bin"
// Número máximo de archivos de datos
#define MAX_FILES 1024
// Máximo tamaño de bloque: 1000px x 1000px = 1,000,000 bytes
#define MAX_BLOCK_BYTES 1000000
// Fingerprint del FS: "BWFS" en ASCII
#define BWFS_MAGIC 0x42574653u 

// Definición de las estructuras  

// Inodo, la estructura de indexación de bloques
typedef struct {
    int used;      // flag si el inodo esta en uso
    char name[256]; // Nombre del archivo
    size_t size; // Tamaño del archivo
    int mode; // Permisos y tipo de archivo
} inode_t;

// Metadata del FS, fingerprint, ruta de storage, tamaño máximo de bloque e inodos
typedef struct {
    unsigned int magic;      // Fingerprint BWFS_MAGIC
    char storage_path[512]; // Ruta absoluta al storage
    size_t max_block_bytes;  // Límite de tamaño de bloque
    inode_t inodes[MAX_FILES]; // Tabla de inodos
} bwfs_t; 


// Lectura de la configuración desde el config.ini
static int read_config(const char *filename, char *storage_path, size_t *max_block_bytes) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("config.ini");  // Muestra un error si no se puede abrir
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Busca lo que son parámetros en la sección, más general
        if (sscanf(line, "storage_path = %511s", storage_path) == 1) continue;
        if (sscanf(line, "max_block_bytes = %zu", max_block_bytes) == 1) continue;
    }
    fclose(f);
    return 0;
}

// Realiza una normalización de a ruta absoluta
static void make_absolute_path(char *dst, size_t dstsz, const char *storage_path) {
    if (storage_path[0] == '/') {
        // En caso que si ya es absoluta, pues copia de forma directa
        strncpy(dst, storage_path, dstsz-1);
        dst[dstsz-1] = '\0';
        return;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        // En caso que falle getcwd, pues copia tal cual
        strncpy(dst, storage_path, dstsz-1);
        dst[dstsz-1] = '\0';
        return;
    }
    // Contruye la ruta absoluta, de cmd mas el storage_path
    snprintf(dst, dstsz, "%s/%s", cwd, storage_path);
    // Compactar dobles barras "//" en la ruta
    for (char *p = dst; *p; ++p) {
        if (p[0] == '/' && p[1] == '/') {
            memmove(p, p+1, strlen(p));
        }
    }
}

// Programa principal de este mkfs_bwfs
int main(int argc, char **argv) {
    // S realiza la validacion de sintaxis, mkfs.bwfs -c config.ini
    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        printf("Uso: mkfs.bwfs -c config.ini\n");
        return 1;
    }
    // Se colocan valores por defecto
    char storage_path_cfg[512] = "bwfs_storage";
    size_t max_block_bytes_cfg = MAX_BLOCK_BYTES;

    // Ahora realiza una lectura de la configuracion desde el config.ini
    if (read_config(argv[2], storage_path_cfg, &max_block_bytes_cfg) != 0) {
        fprintf(stderr, "Advertencia: Error leyendo config.ini, usando valores por defecto.\n");
    }

    // Ahora realiza un enforce límite de los bloques, máximo de 1 000 000 bytes
    if (max_block_bytes_cfg > MAX_BLOCK_BYTES) {
        fprintf(stderr, "max_block_bytes (%zu) > %d, ajustando.\n",
                max_block_bytes_cfg, MAX_BLOCK_BYTES);
        max_block_bytes_cfg = MAX_BLOCK_BYTES;
    }

    // Inicializa la metadata
    bwfs_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = BWFS_MAGIC;   // El fingerprint
    meta.max_block_bytes = max_block_bytes_cfg; // Tamaño máximo de bloque

    // Convierte el storage_path a absoluto y copia a metadata
    make_absolute_path(meta.storage_path, sizeof(meta.storage_path), storage_path_cfg);

    // Crea el directorio de almacenamiento en caso que no exista
    if (mkdir(meta.storage_path, 0755) != 0 && errno != EEXIST) {
        perror("mkfs: mkdir storage_path");
        return 1;
    }

    // Ahora almacena el fingerprint en bwf_metadata.bin
    FILE *fm = fopen(META_FILE, "wb");
    if (!fm) {
        perror("mkfs: metadata fopen");
        return 1;
    }
    if (fwrite(&meta, sizeof(meta), 1, fm) != 1) {
        perror("mkfs: metadata fwrite");
        fclose(fm);
        return 1;
    }
    fclose(fm);

    // Se crea el backing de files que estan vacios distribuidos
    char path[PATH_MAX];
    for (int i = 0; i < MAX_FILES; ++i) {
        snprintf(path, sizeof(path), "%s/file_%04d.dat", meta.storage_path, i);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror("mkfs: fopen backing file");
            return 1;
        }
        fclose(f);
    }
    // Mensaje final que fue creado el FS
    printf("mkfs.bwfs: FS creado en '%s' con %d archivos de datos.\n",
           meta.storage_path, MAX_FILES);
    return 0;
}
