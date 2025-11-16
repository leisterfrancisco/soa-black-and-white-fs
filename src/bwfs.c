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

#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Config / límites */
#define MAX_FILES 1024
#define MAX_NAME_LEN 255
#ifndef META_FILENAME
// Default fallback if not defined at compile time
#define META_FILENAME "bwfs_metadata.bin"
#endif
#define BWFS_MAGIC 0x42574653u /* "BWFS" */
#define DEFAULT_STORAGE "bwfs_storage"
#define DEFAULT_MAX_BLOCK_BYTES 1000000UL /* 1000 x 1000 */

/* --- estructuras en disco (persistentes) --- */
typedef struct {
  int    used;
  char   name[256];
  size_t size;
  int    mode;
} inode_disk_t;

typedef struct {
  unsigned int magic;
  char         storage_path[512];
  size_t       max_block_bytes;
  inode_disk_t inodes[MAX_FILES];
} bwfs_disk_t;

/* --- estructuras en memoria (runtime) --- */
typedef struct {
  int    used;
  char   name[MAX_NAME_LEN + 1];
  mode_t mode;
  uid_t  uid;
  gid_t  gid;
  size_t size;
  time_t atime;
  time_t mtime;
  time_t ctime;
} inode_t;

typedef struct {
  inode_t inodes[MAX_FILES];
  char    storage_path[512];
  size_t  max_block_bytes;
} bwfs_t;

static bwfs_t bwfs; /* estado global */

/* ---- utilidades de path ---- */
static void compact_slashes( const char *src, char *dst, size_t dstsz ) {
  size_t j = 0;
  for ( size_t i = 0; src[i] && j + 1 < dstsz; ++i ) {
    dst[j++] = src[i];
    if ( src[i] == '/' ) {
      while ( src[i + 1] == '/' )
        i++;
    }
  }
  dst[j] = '\0';
}

static void normalize_storage_path( void ) {
  char tmp[PATH_MAX + sizeof( bwfs.storage_path )];
  if ( bwfs.storage_path[0] == '/' ) {
    compact_slashes( bwfs.storage_path, tmp, sizeof( tmp ) );
    strncpy( bwfs.storage_path, tmp, sizeof( bwfs.storage_path ) - 1 );
    bwfs.storage_path[sizeof( bwfs.storage_path ) - 1] = '\0';
    return;
  }
  char cwd[PATH_MAX];
  if ( getcwd( cwd, sizeof( cwd ) ) != NULL ) {
    snprintf( tmp, sizeof( tmp ), "%s/%s", cwd, bwfs.storage_path );
    char compacted[PATH_MAX + sizeof( bwfs.storage_path )];
    compact_slashes( tmp, compacted, sizeof( compacted ) );
    strncpy( bwfs.storage_path, compacted, sizeof( bwfs.storage_path ) - 1 );
    bwfs.storage_path[sizeof( bwfs.storage_path ) - 1] = '\0';
  }
}

/* Asegura que exista el directorio de storage */
static int ensure_storage_dir_exists( void ) {
  if ( strcmp( bwfs.storage_path, "/" ) == 0 ||
       strncmp( bwfs.storage_path, "//", 2 ) == 0 ) {
    fprintf( stderr,
             "[bwfs] storage_path inválido: '%s'\n",
             bwfs.storage_path );
    return -ENOTDIR;
  }
  struct stat st;
  if ( stat( bwfs.storage_path, &st ) == 0 ) {
    if ( S_ISDIR( st.st_mode ) )
      return 0;
    fprintf( stderr,
             "[bwfs] storage_path existe pero no es directorio: %s\n",
             bwfs.storage_path );
    return -ENOTDIR;
  }
  if ( mkdir( bwfs.storage_path, 0755 ) != 0 ) {
    perror( "[bwfs] mkdir storage_path" );
    return -EIO;
  }
  return 0;
}

/* ---------------- metadata load/save ---------------- */
static int load_metadata( void ) {
  bwfs_disk_t disk;
  char        meta_path[PATH_MAX];

  if ( !realpath( META_FILENAME, meta_path ) ) {
    /* puede no existir aún */
    return -1;
  }

  FILE *fp = fopen( meta_path, "rb" );
  if ( !fp )
    return -1;

  size_t r = fread( &disk, sizeof( disk ), 1, fp );
  fclose( fp );
  if ( r != 1 )
    return -1;

  if ( disk.magic != BWFS_MAGIC )
    return -1;

  /* copiar parámetros */
  strncpy( bwfs.storage_path,
           disk.storage_path,
           sizeof( bwfs.storage_path ) - 1 );
  bwfs.storage_path[sizeof( bwfs.storage_path ) - 1] = '\0';
  bwfs.max_block_bytes =
      disk.max_block_bytes ? disk.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
  normalize_storage_path();

  /* inicializar inodos en memoria */
  for ( int i = 0; i < MAX_FILES; ++i ) {
    bwfs.inodes[i].used = disk.inodes[i].used;
    if ( disk.inodes[i].used ) {
      strncpy( bwfs.inodes[i].name, disk.inodes[i].name, MAX_NAME_LEN );
      bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
      bwfs.inodes[i].mode =
          disk.inodes[i].mode ? disk.inodes[i].mode : ( S_IFREG | 0644 );
      bwfs.inodes[i].size = disk.inodes[i].size;
    } else {
      bwfs.inodes[i].name[0] = '\0';
      bwfs.inodes[i].mode = 0;
      bwfs.inodes[i].size = 0;
    }
    bwfs.inodes[i].uid = getuid();
    bwfs.inodes[i].gid = getgid();
    time_t now = time( NULL );
    bwfs.inodes[i].atime = now;
    bwfs.inodes[i].mtime = now;
    bwfs.inodes[i].ctime = now;
  }

  return 0;
}

static int save_metadata( void ) {
  bwfs_disk_t disk;
  memset( &disk, 0, sizeof( disk ) );
  disk.magic = BWFS_MAGIC;
  strncpy( disk.storage_path,
           bwfs.storage_path,
           sizeof( disk.storage_path ) - 1 );
  disk.max_block_bytes =
      bwfs.max_block_bytes ? bwfs.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
  for ( int i = 0; i < MAX_FILES; ++i ) {
    disk.inodes[i].used = bwfs.inodes[i].used;
    if ( bwfs.inodes[i].used ) {
      strncpy( disk.inodes[i].name,
               bwfs.inodes[i].name,
               sizeof( disk.inodes[i].name ) - 1 );
      disk.inodes[i].size = bwfs.inodes[i].size;
      disk.inodes[i].mode = bwfs.inodes[i].mode;
    } else {
      disk.inodes[i].name[0] = '\0';
      disk.inodes[i].size = 0;
      disk.inodes[i].mode = 0;
    }
  }

  FILE *fp = fopen( META_FILENAME, "wb" );
  if ( !fp ) {
    fprintf( stderr, "[bwfs] save_metadata: fopen failed %s\n", META_FILENAME );
    return -1;
  }
  size_t w = fwrite( &disk, sizeof( disk ), 1, fp );
  fclose( fp );
  return ( w == 1 ) ? 0 : -1;
}

/* ---- init hook ---- */
static void *bwfs_init( struct fuse_conn_info *conn, struct fuse_config *cfg ) {
  (void)conn;
  cfg->kernel_cache = 1;

  if ( load_metadata() != 0 ) {
    /* inicializar defaults */
    memset( &bwfs, 0, sizeof( bwfs ) );
    strncpy( bwfs.storage_path,
             DEFAULT_STORAGE,
             sizeof( bwfs.storage_path ) - 1 );
    bwfs.storage_path[sizeof( bwfs.storage_path ) - 1] = '\0';
    bwfs.max_block_bytes = DEFAULT_MAX_BLOCK_BYTES;
    normalize_storage_path();
    (void)ensure_storage_dir_exists();
    (void)save_metadata();
  } else {
    /* si cargó metadata, aseguramos directorio */
    (void)ensure_storage_dir_exists();
  }

  fprintf( stderr,
           "[bwfs_init] storage_path='%s' max_block_bytes=%zu\n",
           bwfs.storage_path,
           bwfs.max_block_bytes );
  return NULL;
}

/* ---- helpers de nombres e inodos ---- */
static const char *basename_from_path( const char *path ) {
  return ( path && path[0] == '/' ) ? path + 1 : path;
}

static int find_inode_by_name( const char *path ) {
  const char *fname = basename_from_path( path );
  if ( !fname || !fname[0] )
    return -1;
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 )
      return i;
  }
  return -1;
}

/* Reserva un inodo y crea el backing file. Retorna índice >=0 o -errno */
static int allocate_inode( const char *path, mode_t mode ) {
  const char *fname = basename_from_path( path );
  if ( !fname || !fname[0] )
    return -EINVAL;
  if ( strlen( fname ) > MAX_NAME_LEN )
    return -ENAMETOOLONG;
  if ( find_inode_by_name( path ) >= 0 )
    return -EEXIST;

  if ( bwfs.storage_path[0] == '\0' )
    return -EIO;
  if ( ensure_storage_dir_exists() != 0 )
    return -EIO;

  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( !bwfs.inodes[i].used ) {
      bwfs.inodes[i].used = 1;
      strncpy( bwfs.inodes[i].name, fname, MAX_NAME_LEN );
      bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
      bwfs.inodes[i].mode = S_IFREG | ( mode & 0777 );
      bwfs.inodes[i].uid = getuid();
      bwfs.inodes[i].gid = getgid();
      bwfs.inodes[i].size = 0;
      time_t now = time( NULL );
      bwfs.inodes[i].atime = bwfs.inodes[i].mtime = bwfs.inodes[i].ctime = now;

      char filepath[PATH_MAX];
      int  rc = snprintf( filepath,
                         sizeof( filepath ),
                         "%s/file_%04d.dat",
                         bwfs.storage_path,
                         i );
      if ( rc < 0 || (size_t)rc >= sizeof( filepath ) ) {
        bwfs.inodes[i].used = 0;
        bwfs.inodes[i].name[0] = '\0';
        return -ENAMETOOLONG;
      }

      int fd = open( filepath, O_CREAT | O_EXCL | O_WRONLY, 0644 );
      if ( fd < 0 ) {
        if ( errno == EEXIST ) {
          /* si ya existe, truncamos para asegurar estado consistente */
          fd = open( filepath, O_TRUNC | O_WRONLY );
          if ( fd < 0 ) {
            perror( "[bwfs] allocate_inode open truncate" );
            bwfs.inodes[i].used = 0;
            bwfs.inodes[i].name[0] = '\0';
            return -EIO;
          }
        } else {
          perror( "[bwfs] allocate_inode open" );
          bwfs.inodes[i].used = 0;
          bwfs.inodes[i].name[0] = '\0';
          return -EIO;
        }
      }
      close( fd );

      if ( save_metadata() != 0 ) {
        /* rollback */
        unlink( filepath );
        bwfs.inodes[i].used = 0;
        bwfs.inodes[i].name[0] = '\0';
        return -EIO;
      }

      fprintf( stderr,
               "[bwfs] allocate_inode: allocated idx=%d name='%s' file='%s'\n",
               i,
               bwfs.inodes[i].name,
               filepath );
      return i;
    }
  }
  return -ENOSPC;
}

/* ---- FUSE operations ---- */

static int bwfs_getattr( const char            *path,
                         struct stat           *stbuf,
                         struct fuse_file_info *fi ) {
  (void)fi;
  memset( stbuf, 0, sizeof( struct stat ) );

  if ( strcmp( path, "/" ) == 0 ) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    return 0;
  }

  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  inode_t *ino = &bwfs.inodes[idx];
  stbuf->st_mode = ino->mode;
  stbuf->st_nlink = 1;
  stbuf->st_size = ino->size;
  stbuf->st_uid = ino->uid;
  stbuf->st_gid = ino->gid;
  stbuf->st_atime = ino->atime;
  stbuf->st_mtime = ino->mtime;
  stbuf->st_ctime = ino->ctime;
  return 0;
}

static int bwfs_readdir( const char             *path,
                         void                   *buf,
                         fuse_fill_dir_t         filler,
                         off_t                   offset,
                         struct fuse_file_info  *fi,
                         enum fuse_readdir_flags flags ) {
  (void)offset;
  (void)fi;
  (void)flags;
  if ( strcmp( path, "/" ) != 0 )
    return -ENOENT;
  filler( buf, ".", NULL, 0, 0 );
  filler( buf, "..", NULL, 0, 0 );
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used )
      filler( buf, bwfs.inodes[i].name, NULL, 0, 0 );
  }
  return 0;
}

static int
bwfs_create( const char *path, mode_t mode, struct fuse_file_info *fi ) {
  (void)fi;
  int idx = allocate_inode( path, mode );
  if ( idx < 0 ) {
    fprintf( stderr,
             "[bwfs] create: allocate_inode failed %d for path='%s'\n",
             idx,
             path );
    return idx;
  }
  return 0;
}

static int bwfs_unlink( const char *path ) {
  const char *fname = basename_from_path( path );
  if ( !fname )
    return -ENOENT;
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 ) {
      char filepath[PATH_MAX];
      snprintf( filepath,
                sizeof( filepath ),
                "%s/file_%04d.dat",
                bwfs.storage_path,
                i );
      unlink( filepath ); /* ignore error */
      bwfs.inodes[i].used = 0;
      bwfs.inodes[i].name[0] = '\0';
      bwfs.inodes[i].size = 0;
      bwfs.inodes[i].mode = 0;
      save_metadata();
      return 0;
    }
  }
  return -ENOENT;
}

/* open: validar existencia (no hace mucho más, FUSE pedirá create antes si aplica) */
static int bwfs_open( const char *path, struct fuse_file_info *fi ) {
  (void)fi;
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;
  return 0;
}

/* read: signature correcta (ssize_t) */
static ssize_t bwfs_read( const char            *path,
                          char                  *buf,
                          size_t                 size,
                          off_t                  offset,
                          struct fuse_file_info *fi ) {
  (void)fi;
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  inode_t *ino = &bwfs.inodes[idx];
  if ( (size_t)offset >= ino->size )
    return 0; /* EOF */

  if ( offset + size > ino->size )
    size = ino->size - offset;

  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );

  int fd = open( filepath, O_RDONLY );
  if ( fd < 0 )
    return -EIO;

  if ( lseek( fd, offset, SEEK_SET ) == (off_t)-1 ) {
    close( fd );
    return -EIO;
  }

  ssize_t r = read( fd, buf, size );
  close( fd );
  if ( r < 0 )
    return -EIO;

  /* actualizar atime */
  ino->atime = time( NULL );
  save_metadata();
  return r;
}

/* write: signature correcta (ssize_t) */
static ssize_t bwfs_write( const char            *path,
                           const char            *buf,
                           size_t                 size,
                           off_t                  offset,
                           struct fuse_file_info *fi ) {
  (void)fi;
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  inode_t *ino = &bwfs.inodes[idx];

  /* validar límite por bloque */
  uint64_t endpos = (uint64_t)offset + (uint64_t)size;
  if ( endpos > bwfs.max_block_bytes )
    return -EFBIG;

  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );

  int fd = open( filepath, O_RDWR );
  if ( fd < 0 )
    return -EIO;

  if ( lseek( fd, offset, SEEK_SET ) == (off_t)-1 ) {
    close( fd );
    return -EIO;
  }

  ssize_t w = write( fd, buf, size );
  close( fd );
  if ( w < 0 )
    return -EIO;

  size_t newsize = (size_t)offset + (size_t)w;
  if ( newsize > ino->size )
    ino->size = newsize;
  ino->mtime = time( NULL );
  save_metadata();

  return w;
}

/* utimens: actualizar tiempos */
static int bwfs_utimens( const char            *path,
                         const struct timespec  tv[2],
                         struct fuse_file_info *fi ) {
  (void)fi;
  const char *fname = basename_from_path( path );
  if ( !fname )
    return -ENOENT;
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 ) {
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
    .init = bwfs_init,
    .getattr = bwfs_getattr,
    .readdir = bwfs_readdir,
    .create = bwfs_create,
    .open = bwfs_open,
    .read = bwfs_read,
    .write = bwfs_write,
    .unlink = bwfs_unlink,
    .utimens = bwfs_utimens,
};

/* ---- main: procesa -c config.ini opcional y arranca FUSE ---- */
int main( int argc, char *argv[] ) {
  const char *config_file = NULL;
  for ( int i = 1; i < argc; ++i ) {
    if ( strcmp( argv[i], "-c" ) == 0 && i + 1 < argc ) {
      config_file = argv[i + 1];
      /* eliminar ambos argumentos para fuse_main */
      for ( int j = i + 2; j < argc; ++j )
        argv[j - 2] = argv[j];
      argc -= 2;
      break;
    }
  }
  if ( config_file )
    printf( "Usando configuración: %s\n", config_file );
  printf(
      "********* EJECUTANDO BWFS (Opción 1 - archivo por inodo) *********\n" );
  return fuse_main( argc, argv, &bwfs_oper, NULL );
}
