/**
 * BWFS - Implementación con FUSE parte de inicialización, metadata, rutas
 * y gestión de inodos

*/

/**
 *  Primera Parte, carga la metada del FS (fingerprint, storage_path, max_block_bytes, tabla de inodos) y luego lo normaliza para que sea 
 *  segura y manejable. Mantiene una tabla de inodos en memoria con atributos POSIX para integrarse con las operaciones FUSE. Luego asegura
 *  que el directorio de almacenamiento exista y que las rutas no estén corruptas. Permite asignar un inodo nuevo y crear su respectivo
 *  archivo fisico distribuido en bwfs_storage, persistiendo el cambio en bwfs_metadata.bin. Prepara el FS para que las operaciones 
 *  FUSE puedan trabajar sobre esta estructura
 * 
 *  Segunda Parte, Se implementan las operaciones básicas de FUSE que permiten que BWFS se comporte como un sistema de archivos real, cuando el
 *  usuario ejecuta comandos en el punto de montaje, FUSE invoca estas funciones, entonces:
 *     1. bwfs_getattr devuelve atributos de archivos y del directorio raiz (lo que hace posible ls -l o stat)
 *     2. bwfs_readdir lista el contenido del directorio raíz mostrando los nombres de los inodos activos
 *     3. bwfs_create reserva un inodo libre y lo inicializa para crear un nuevo archivo (touch)
 *     4. bwfs_unlink libera un inodo y borra el archivo (rm)
 *    -extra bwfs_utimens(extra ayuda para solucionar un error en consola) actualiza los tiempos de acceso y modificación (touch actualiza atime/mtime)
 * 
 *  Luego estos cambios se guardan en bwfs_metadata.bin mediante save_metadata, asegurando pesistencia, finalmente la tabla bwfs_oper hace un registro
 *  de estas funciones y main arranca FUSE con ellas, de forma que cualquier operacion del usuario en el directorio montado se traduzca en llamadas a estas
 *  funciones
 */
 
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
#include <stdlib.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Config / límites */
#define MAX_FILES 1024 // Número máximo de los inodos/ archivos en el FS
#define MAX_NAME_LEN 255 // Largo máximo de nombre de archivo
#define META_FILENAME "/home/elias/DesktopWSL/Proyecto-BWFS/bwfs_metadata.bin"
#define BWFS_MAGIC 0x42574653u   // Fingerprint "BWFS"
#define DEFAULT_STORAGE "bwfs_storage" // Nombre donde se almancenarán
#define DEFAULT_MAX_BLOCK_BYTES 1000000UL // Representa 1 millón de bytes = 1000x1000 píxeles

/* --- estructuras en disco (persistentes) --- */
typedef struct {
    int used; // En caso de ser 0 esta libre y 1 ocupado
    char name[256]; //Número lógico del archivo
    size_t size; //Tamaño actual
    int mode; // Permisos/Tipo
} inode_disk_t;

typedef struct {
    unsigned int magic; // Fingerprint BWFS
    char storage_path[512]; // Ruta del directorio de almacenamiento
    size_t max_block_bytes; // Tamaño máximo de bloque
    inode_disk_t inodes[MAX_FILES]; // Tabla de inodos persistente
} bwfs_disk_t;

/* --- Estructuras en memoria (runtime) --- */
typedef struct {
    int used;   //Indica si el nodo esta ocupado 1 o libre en 0
    char name[MAX_NAME_LEN + 1]; // Nombre del archivo en memoria
    mode_t mode; // Permisos y tipo
    uid_t uid; // ID de usuario propietario del archivo
    gid_t gid; // ID del grupo del propietario del archivo
    size_t size; // Tamaño actual del archivo en bytes
    time_t atime; // último accceso, actualiza ada vez que lee el archivo
    time_t mtime; // última modificación se actualiza cuando se escribe o modifica
    time_t ctime; // Tiempo de cambio o creación 
} inode_t;

typedef struct {
    inode_t inodes[MAX_FILES];  // Tabla de inodos en memoria
    char storage_path[512];  // Ruta del storage
    size_t max_block_bytes;  // Tamaño máximo de bloque
} bwfs_t;

static bwfs_t bwfs; /* estado global */

/* ---- utilidades de path ---- */
static void compact_slashes(const char *src, char *dst, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstsz; ++i) {
        dst[j++] = src[i];
        if (src[i] == '/') {
            while (src[i + 1] == '/') i++;
        }
    }
    dst[j] = '\0';
}

// Se realiza una normalización de la ruta del storage para que sea absoluta y segura
static void normalize_storage_path(void) {
    char tmp[PATH_MAX + sizeof(bwfs.storage_path)];
    if (bwfs.storage_path[0] == '/') {
        compact_slashes(bwfs.storage_path, tmp, sizeof(tmp));
        strncpy(bwfs.storage_path, tmp, sizeof(bwfs.storage_path)-1);
        bwfs.storage_path[sizeof(bwfs.storage_path)-1] = '\0';
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // Realiza la concatenación del directorio actual y el storage relativo
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, bwfs.storage_path);
        char compacted[PATH_MAX + sizeof(bwfs.storage_path)];
        compact_slashes(tmp, compacted, sizeof(compacted));
        strncpy(bwfs.storage_path, compacted, sizeof(bwfs.storage_path)-1);
        bwfs.storage_path[sizeof(bwfs.storage_path)-1] = '\0';
    }
}

/* Asegura que exista el directorio de storage, sino lo crea */
static int ensure_storage_dir_exists(void) {
    // Valida la ruta
    if (strcmp(bwfs.storage_path, "/") == 0 || strncmp(bwfs.storage_path, "//", 2) == 0) {
        fprintf(stderr, "[bwfs] storage_path inválido: '%s'\n", bwfs.storage_path);
        return -ENOTDIR;
    }
    struct stat st;
    if (stat(bwfs.storage_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "[bwfs] storage_path existe pero no es directorio: %s\n", bwfs.storage_path);
        return -ENOTDIR;
    }
    // Creacion del directorio
    if (mkdir(bwfs.storage_path, 0755) != 0) {
        perror("[bwfs] mkdir storage_path");
        return -EIO;
    }
    return 0;
}

// La metadata que se carga y guarda
static int load_metadata(void) {
    bwfs_disk_t disk;    // Lectura de datos desde el archivo metadata
    char meta_path[PATH_MAX];  // Ruta absoluta al archivo de metadata
    // Obtiene la ruta absoluta del archivo (bwfs_metadata.bin)
    if (!realpath(META_FILENAME, meta_path)) {
        // Por el momento aún no existe el archivo de metadata
        return -1;
    }

    // Se abre el archivo de metadata en modo de lectura binaria
    FILE *fp = fopen(meta_path, "rb");
    if (!fp) return -1;

    // Ahora se lee todo el contenido del archivo de metadata en la estructura disk
    size_t r = fread(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    if (r != 1) return -1; // En caso que no lo lea correctamente, retorna el error
    // Validación del magic number para asegurar de que es un BWFS válido
    if (disk.magic != BWFS_MAGIC) return -1;

    // Realiza la copa de los parámetros del disco a memoria
    strncpy(bwfs.storage_path, disk.storage_path, sizeof(bwfs.storage_path)-1);
    bwfs.storage_path[sizeof(bwfs.storage_path)-1] = '\0';
    // Se copia el tamaño del bloque, si no está definido, usar valores por defecto
    bwfs.max_block_bytes = disk.max_block_bytes ? disk.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
    // Ahora normaliza la ruta del storage 
    normalize_storage_path();

    /* inicializar inodos en memoria */
    for (int i = 0; i < MAX_FILES; ++i) {
        bwfs.inodes[i].used = disk.inodes[i].used;
        if (disk.inodes[i].used) {
            // En caso que el inodo esta ocupado, pues copia el nombre, permisos y tamaño
            strncpy(bwfs.inodes[i].name, disk.inodes[i].name, MAX_NAME_LEN);
            bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
            bwfs.inodes[i].mode = disk.inodes[i].mode ? disk.inodes[i].mode : (S_IFREG | 0644);
            bwfs.inodes[i].size = disk.inodes[i].size;
        } else {
            // Para el caso que esta libre, inicializa los valores por defecto
            bwfs.inodes[i].name[0] = '\0';
            bwfs.inodes[i].mode = 0;
            bwfs.inodes[i].size = 0;
        }
        // Asignar UID/GID actual, el propietario del proceso
        bwfs.inodes[i].uid = getuid();
        bwfs.inodes[i].gid = getgid();
        time_t now = time(NULL);
         // Inicializa el time timestamps del acceso, modificación y creación al tiempo actual
        bwfs.inodes[i].atime = now;
        bwfs.inodes[i].mtime = now;
        bwfs.inodes[i].ctime = now;
    }

    return 0;
}

// Almacena la metadata de memoria en el bwfs_metadata.bin
static int save_metadata(void) {
    bwfs_disk_t disk;
    // Inicializa la estructura temporal en disco a cero
    // Asegura que cualquier padding o campo sin utilizar se guarde como 0
    memset(&disk, 0, sizeof(disk));
    // Escribe en el magic number BWFS, para validar que el archivo es el correcto al leerlo
    disk.magic = BWFS_MAGIC;
    // Copiar la ruta de almacenamiento desde la estructura en memoria global bwfs, a la estructura
    // en disco. Limita la copia al tamaño máximo de storage path
    strncpy(disk.storage_path, bwfs.storage_path, sizeof(disk.storage_path)-1);
    disk.max_block_bytes = bwfs.max_block_bytes ? bwfs.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
    // Ahora copia los inodos desde la memoria, a la estructura de disco
    for (int i = 0; i < MAX_FILES; ++i) {
        disk.inodes[i].used = bwfs.inodes[i].used;
        if (bwfs.inodes[i].used) {
            // Si el inodo está activo, pues copia el nombre, tamaño y permisos
            strncpy(disk.inodes[i].name, bwfs.inodes[i].name, sizeof(disk.inodes[i].name)-1);
            disk.inodes[i].size = bwfs.inodes[i].size;
            disk.inodes[i].mode = bwfs.inodes[i].mode;
        } else {
            // Para cuando el inodo está libre, inicializamos los campos a 0 
            disk.inodes[i].name[0] = '\0';
            disk.inodes[i].size = 0;
            disk.inodes[i].mode = 0;
        }
    }

    // Abre el archivo de metadata en modo binario escritura, truncando cualquier contenido previo
    FILE *fp = fopen(META_FILENAME, "wb");
    if (!fp) {
        // En caso que falle abrir el archivo, informar sobre el error
        fprintf(stderr, "[bwfs] save_metadata: fopen failed %s\n", META_FILENAME);
        return -1;
    }
    // Escritura de la esctructura completa de disco en bwfs_metadata.bin
    size_t w = fwrite(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    return (w == 1) ? 0 : -1;
}

/* ---- init hook ---- */
// Esta funcion se ejecuta de forma automatica cuando se monta el FS usando FUSE
// Como tal tiene el objetivo de inicializar el estado del FS en memoria, cargar metadata y
// asegurar que el directorio de almacenamiento exista.
static void *bwfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void) conn;
    cfg->kernel_cache = 1;

    // 1. Intentar cargar metadata
    if (load_metadata() != 0) {
        /* inicializar defaults */
        memset(&bwfs, 0, sizeof(bwfs));
        strncpy(bwfs.storage_path, DEFAULT_STORAGE, sizeof(bwfs.storage_path)-1);
        bwfs.storage_path[sizeof(bwfs.storage_path)-1] = '\0';
        bwfs.max_block_bytes = DEFAULT_MAX_BLOCK_BYTES;
        normalize_storage_path();
        (void) ensure_storage_dir_exists();
        (void) save_metadata();
    } else {
        /* si cargó metadata, aseguramos directorio */
        (void) ensure_storage_dir_exists();
    }

    fprintf(stderr, "[bwfs_init] storage_path='%s' max_block_bytes=%zu\n",
            bwfs.storage_path, bwfs.max_block_bytes);
    return NULL;
}

/* ---- helpers de nombres e inodos ---- */
// Se busca extraer el nombre base del path absoluta
static const char *basename_from_path(const char *path) {
    return (path && path[0] == '/') ? path + 1 : path;
}

// Busca un inodo activo en memoria dado por un path
// Hace un retorno del índica del inodo en bwfs.inodes[] o -1 en caso que no exista
static int find_inode_by_name(const char *path) {
    const char *fname = basename_from_path(path);
    if (!fname || !fname[0]) return -1;
    for (int i = 0; i < MAX_FILES; ++i) {
        if (bwfs.inodes[i].used && strcmp(bwfs.inodes[i].name, fname) == 0)
            return i;
    }
    return -1;
}

/* Reserva un inodo y crea el backing file. Retorna índice >=0 o -errno */
static int allocate_inode(const char *path, mode_t mode) {
    const char *fname = basename_from_path(path);
    // Validaciones iniciales
    if (!fname || !fname[0]) return -EINVAL;
    if (strlen(fname) > MAX_NAME_LEN) return -ENAMETOOLONG;
    if (find_inode_by_name(path) >= 0) return -EEXIST;
    // Valida que storage path esté definido y exista
    if (bwfs.storage_path[0] == '\0') return -EIO;
    if (ensure_storage_dir_exists() != 0) return -EIO;

    // Busca de un nodo libre
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!bwfs.inodes[i].used) { // En caso que el nodo esté libre, se incializa
            bwfs.inodes[i].used = 1; // Marcarlo como ocupado
            strncpy(bwfs.inodes[i].name, fname, MAX_NAME_LEN); // Guarda el nombre
            bwfs.inodes[i].name[MAX_NAME_LEN] = '\0'; // Asegura el null terminator
            bwfs.inodes[i].mode = S_IFREG | (mode & 0777); // Tipo de archivo regular y permisos POSIX
            bwfs.inodes[i].uid = getuid(); // Propietario actual
            bwfs.inodes[i].gid = getgid(); // Grupo actual
            bwfs.inodes[i].size = 0; // Tamaño incial 0
            time_t now = time(NULL); // Obtener el timestampo actual
            bwfs.inodes[i].atime = bwfs.inodes[i].mtime = bwfs.inodes[i].ctime = now; // Tiempos

            // Creacion de ruta del archivo fisico
            char filepath[PATH_MAX];
            int rc = snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, i);
            if (rc < 0 || (size_t)rc >= sizeof(filepath)) {
                // En caso que la ruta exceda el tamaño, deshace la asignación
                bwfs.inodes[i].used = 0;
                bwfs.inodes[i].name[0] = '\0';
                return -ENAMETOOLONG;
            }

            // Creación de un archivo físico vacío
            int fd = open(filepath, O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) {
                if (errno == EEXIST) {
                    /* si ya existe, truncamos para asegurar estado consistente */
                    fd = open(filepath, O_TRUNC | O_WRONLY);
                    if (fd < 0) {
                        perror("[bwfs] allocate_inode open truncate");
                        bwfs.inodes[i].used = 0;
                        bwfs.inodes[i].name[0] = '\0';
                        return -EIO;
                    }
                } else { // Error diferente a la existencia
                    perror("[bwfs] allocate_inode open");
                    bwfs.inodes[i].used = 0;
                    bwfs.inodes[i].name[0] = '\0';
                    return -EIO;
                }
            }
            // Cierra el descriptor de archivo
            close(fd);

            // Almacena el metadata actualizada
            if (save_metadata() != 0) {
                // En caso que falla, se deshace todo (rollback)
                unlink(filepath); //Borra el archivo físico
                bwfs.inodes[i].used = 0; // Libera el inodo
                bwfs.inodes[i].name[0] = '\0';
                return -EIO;
            }
            // Imprime información de debugging
            fprintf(stderr, "[bwfs] allocate_inode: allocated idx=%d name='%s' file='%s'\n",
                    i, bwfs.inodes[i].name, filepath);
            return i; // Imprime información de debugging
        }
    }
    return -ENOSPC; // Return para cuando no se encuentran inodos libres
}

/* ---- FUSE operations ---- */
/**
 * 1. Función getattr: Esta función es una llamada de FUSE cada vez que se busca
 * conocer por ejemplo la información de un archivo o directorio.
 * 
 */
static int bwfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;

    // Inicializamos con la estructura que se devolverá, esto evita que los valores que no se usen
    // en campos que no estan escritos
    memset(stbuf, 0, sizeof(struct stat));

    // Presentamos un caso especial, la raíz del FS '/' debe presentarse como un directorio.
    // En este bloque devuelve el modo de directorio y número de los enlaces
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755; // Tipo, directorio, permisos 0755 (rwxr-xr-x) default 
        stbuf->st_nlink = 2; // Ahora la convención '.' y '..' para 2 enlaces mínimos
        return 0;
    }

    // Para cualquier otro path se espera un nombre simple '/nombre' sin subdirectorios
    // luego find inode by name busca la tabla de inodos en memoria y devuelve el índice
    // del nodo si existe, o -1 si no lo encuentra
    int idx = find_inode_by_name(path);
    if (idx < 0) return -ENOENT; // En caso que no exista devuelve, no such file or directory
    
    // En caso que exista, se obtiene un puntero al inodo en memoria para tomar los campos POSIX
    inode_t *ino = &bwfs.inodes[idx];
    /**
     * Realizamos un relleno de la estructura de stat con los campos:
     * - st_mode = tipo y permisos, archivo regular, permisos que estan almacenados en inodo, mode
     * - st_nlink = número de los enlaces, para archivos normales usamos 1 
     * - st_size =  tamaño en bytes, importante para el read/ls
     * - st_uid/ st_uid = propietario, permiten que el statu
     * - st_atime/ st_mtime / s_ctime  = tiempos de acceso/modificación/cambio
     * 
     */
    stbuf->st_mode = ino->mode;
    stbuf->st_nlink = 1;
    stbuf->st_size = ino->size;
    stbuf->st_uid = ino->uid;
    stbuf->st_gid = ino->gid;
    // Tiempos, conversión directa desde time_t almacenado en el inodo
    stbuf->st_atime = ino->atime;
    stbuf->st_mtime = ino->mtime;
    stbuf->st_ctime = ino->ctime;
    return 0;
}

/**
 * 2. Función readdir, muestra la lista de archivos del directorio raíz par FUSE, entonces para esta funcion
 * se ejecuta cuando el usuairo hace '/', asi que si se pide otro, se devuelve  -ENOENT
 *  Por lo tanto, verifica que el path sea '/' luego agrega las entradas '.' y '..', por 
 *  ultimo recorre la tabal de inodos y agrega los nombres de los archivos utilizados
 */
static int bwfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;

    // Solo se permite listar el directorio raiz
    if (strcmp(path, "/") != 0) return -ENOENT;
    // Se presentan las entradas basicas de cualquier directorio
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    // Ahora se agregan los archivos existentes segun los inodos
    for (int i = 0; i < MAX_FILES; ++i) {
        if (bwfs.inodes[i].used)
            filler(buf, bwfs.inodes[i].name, NULL, 0, 0);
    }
    return 0;
}

/**
 * 3. Función create, realiza el manejo de la creación de archivos en el FS, por ejemplo un 'touch'
 *  En esta funcion es llamada por FUSE cuando el usuario intenta crear un archivo
 * 
 */
static int bwfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) fi;
    // Realiza el intento de reservar un inodo para este nuevo archivo
    int idx = allocate_inode(path, mode);
    // Maneja un error si no hay espacio o path invalido
    if (idx < 0) {
        fprintf(stderr, "[bwfs] create: allocate_inode failed %d for path='%s'\n", idx, path);
        return idx; // Propaga el error hacia FUSE
    }
    return 0;
}

/**
 * 4. Funcion unlink, elimina un archivo del FS (rm)
 * 
 */
static int bwfs_unlink(const char *path) {
    // Obtiene el nombre del archivo a partir de la ruta
    const char *fname = basename_from_path(path);
    if (!fname) return -ENOENT;
    // Recorre todo los inodos buscando uno que coincida por nombre
    for (int i = 0; i < MAX_FILES; ++i) {
        // Coincidencia, inodo usado y nombre igual
        if (bwfs.inodes[i].used && strcmp(bwfs.inodes[i].name, fname) == 0) {
            char filepath[PATH_MAX];
            // Ruta especifica del archivo guardado en bwfs_storage
            snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, i);
            // Intenta borrar el archivo fisico, si falla no hay problema
            unlink(filepath);

            // Limpieza de inodo
            bwfs.inodes[i].used = 0;
            bwfs.inodes[i].name[0] = '\0';
            bwfs.inodes[i].size = 0;
            bwfs.inodes[i].mode = 0;

            // Guarda los cambios en la metadata del FS
            save_metadata();
            return 0;
        }
    }
    return -ENOENT;
}


/**
 * 5. Funcion open, FUSE hace un llamado a esta funcion cuando un programa intenta abrir un 
 * archivo(nano, etc). 
 * 
 */
static int bwfs_open(const char *path, struct fuse_file_info *fi) {
    (void) fi;
    // Realiza la busqueda del indice del inodo que corresponde al archivo dado
    int idx = find_inode_by_name(path);
    // Realiza la busqueda del indice del inodo que corresponde al archivo dado
    if (idx < 0) return -ENOENT;
    return 0; // Si existe, toma el archivo valido y lo abre
}

/**
 * 6. Funcion read, esta funcion se ejecuta cuando un programa intenta leer el contenido
 * de un archivo (cat,head,tail)
 * 
 */
static ssize_t bwfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    // Busca el inodo correspondiente al archivo solicitado
    int idx = find_inode_by_name(path);
    if (idx < 0) return -ENOENT;

    // Para cuando el offset es mayor o igual al tamaño del archivo
    inode_t *ino = &bwfs.inodes[idx];
    if ((size_t)offset >= ino->size) return 0; /* EOF */

    // Si el cliente pide más bytes que los disponibles desde el offset, ajustamos el tamaño real
    // que sí se puede leer
    if (offset + size > ino->size) size = ino->size - offset;

    // Construimos la ruta del archivo real "file_xxx.dat"
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, idx);

    // Ahora abrimos el archivo fisico
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -EIO; // en caso de error de lectura fisico

    // Posiciona el cursor en el offset solicitado
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) { close(fd); return -EIO; }

    // Lee desde el archivo real hacia el buffer FUSE
    ssize_t r = read(fd, buf, size);
    close(fd);
    if (r < 0) return -EIO;

    // Actualiza el atime, es decir el ultimo acceso
    ino->atime = time(NULL);
    save_metadata(); // Guarda los cambios en el metadata.bin
    return r; 
}

/**
 * 7. Funcion write, ejecuta cuando un programa quiere escribir datos en un archivo
 * (echo, nano, etc)
 */
static ssize_t bwfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    int idx = find_inode_by_name(path);
    if (idx < 0) return -ENOENT;

    // Busca el inodo correspondiente al archivo
    inode_t *ino = &bwfs.inodes[idx];

    // Realiza la verificacion que no nos pasemos del maximo que es permitido por bloque
    // el endpos es la posicion final donde terminaria la escritura
    uint64_t endpos = (uint64_t)offset + (uint64_t)size;
    if (endpos > bwfs.max_block_bytes) return -EFBIG; // Archivo seria demasiado grande

    // Contruye la ruta del archivo real
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, idx);

    // Abre el archivo fisico en modo de lectura/escritura
    int fd = open(filepath, O_RDWR);
    if (fd < 0) return -EIO;

    // Ahora posiciona el cursor en el offset solicitado
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) { close(fd); return -EIO; }

    // Escribe desde el buffer al archivo real
    ssize_t w = write(fd, buf, size);
    close(fd);
    if (w < 0) return -EIO; // Falla fisica de escritura

    // Realiza una actualizacion del nuevo tamaño en caso que crezca
    size_t newsize = (size_t)offset + (size_t)w;
    if (newsize > ino->size) ino->size = newsize;
    // Realiza una actualizacion del nodo
    ino->mtime = time(NULL);
    // Ahora almacena los cambios en metadata
    save_metadata();

    return w; // devuelve la cantidad de bytes que fueron efectivamente escritos
}

 /** 
 * 8. Funcion utimens(extra),  porque ayuda para actualizar los tiempos de acceso (atime)
 * modificacion (mtime) de un archivo
 */
static int bwfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void) fi;
    // Obtenemos solo el nombre del archivo desde la ruta completa
    const char *fname = basename_from_path(path);
    if (!fname) return -ENOENT;
    // Ahora se recorre toda la tabla de inodos buscando el archivo
    for (int i = 0; i < MAX_FILES; ++i) {
        // Se realiza una verificación si este inodo esta ocupado y coincide el nombre del archivo
        if (bwfs.inodes[i].used && strcmp(bwfs.inodes[i].name, fname) == 0) {
            // Entonces el tv[0] representa el tiempo de ultimo acceso atime
            // y el tv[1] es el tiempo de la ultima modificacion mtime
            bwfs.inodes[i].atime = tv[0].tv_sec;
            bwfs.inodes[i].mtime = tv[1].tv_sec;
            save_metadata();
            return 0;
        }
    }
    return -ENOENT;
}

/* operaciones registradas en FUSE */
static struct fuse_operations bwfs_oper = {
    .init       = bwfs_init,
    .getattr    = bwfs_getattr,
    .readdir    = bwfs_readdir,
    .create     = bwfs_create,
    .open       = bwfs_open,
    .read       = bwfs_read,
    .write      = bwfs_write,
    .unlink     = bwfs_unlink,
    .utimens    = bwfs_utimens,
};

/* ---- main: procesa -c config.ini opcional y arranca FUSE ---- */
int main(int argc, char *argv[]) {
    const char *config_file = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[i + 1];
            /* eliminar ambos argumentos para fuse_main */
            for (int j = i + 2; j < argc; ++j) argv[j-2] = argv[j];
            argc -= 2;
            break;
        }
    }
    if (config_file) printf("Usando configuración: %s\n", config_file);
    printf("********* EJECUTANDO BWFS (Opción 1 - archivo por inodo) *********\n");
    return fuse_main(argc, argv, &bwfs_oper, NULL);
}

