/**
 * En este codigo implementa las funciones de FS utilizando FUSE.  Se utiliza un modelo de 'un file por nodo', donde cada file se almacena dentro de un 
 * directorio de respaldo denominado 'bwfs_storage'. El FS mantiene una tabla de nodos persistente en el file por bwfs_metadata.bin y una tabla de inodos 
 * en memoria que se carga al montar el FS.
 * 
 * Para cada nodo se describe lo siguiente, nombre del archivo, permisos POSIX, tamaño lógico, uid/gid propietarios, timestamps (atime, mtime, ctime) y el
 * indicador used/free.
 * 
 * ====== Parte 1 - Inicializacion y el manejo de metadata ==========
 * Se realiza la carga de bwfs_metadata.bin, valida el magic number BWFS, en caso que no exista pues crea un metadata inicial. Luego hacer una normalizacion 
 * y validacion de la ruta del directorio de almacenamiento, luego verifica que bwfs_storage exista o lo crea, luego copia la tabla de inodos persistente a memoria, 
 * ademas mantiene max block bytes como limite maximo por archivo
 * 
 * Por lo que estas funciones permiten al BWFS poder recordar el estado del FS entre montajes.
 * 
 * ====== Parte 2 - Implementación de operaciones FUSE  ========
 * Ahora cada operacion POSIX que ocurre sobre el directorio montado realiza la traduccion en las llamadas a estas funciones:
 *   
 *   - getattr,  información de archivos (stat, ls -l)
 *   - readdir, listar archivos en '/'
 *   - create , crear archivo (touch)
 *   - open, abrir archivo
 *   - read, leer contenido del archivo
 *   - write, escribir en archivo
 *   - unlink, borrar archivo (rm)
 *   - rename, cambiar nombre (mv)
 *   - chmod, cambiar permisos
 *   - utimens, actualizar timestamps
 *   - mkdir, crear directorios 
 *   - statfs, reporta el estado del FS, ej; df -h o stat -f
 *   - fsync, verifica que todos los datos pendientes en un archivo sean escritos de forma segura
 *   - flush, busca que los cambios en el file este sincronizado con el almacenamiento
 *   - lseek, function que permite que nuestro FS no use file handles
 * 
 *  Cada file corresponde por ejemplo:
 *      /mnt/archivo.txt   representa en bwfs_storage/file_xxxx.dat
 * 
 *  ====== Main - un tipo de arranque del FUSE
 *  Entonces primero se procesa la opcion -c config.ini, luego se muestra la informacion de montaje
 *  , entonces llama a fuse main registrando las operaciones por este FS.
 */
#include <bwfs.h>
#include <stdio.h>

static bwfs_t bwfs; /* estado global */

/* forward declarations for internal FUSE operations used earlier */
static int save_metadata( void );
static int bwfs_unlink( const char *path );
static int bwfs_read( const char            *path,
                      char                  *buf,
                      size_t                 size,
                      off_t                  offset,
                      struct fuse_file_info *fi );
static int bwfs_write( const char            *path,
                       const char            *buf,
                       size_t                 size,
                       off_t                  offset,
                       struct fuse_file_info *fi );

// Se llama cuando el archivo no existe en el master
ssize_t read_remote_file( const char *path, void *buffer, size_t buffer_size ) {
  if ( !path || !buffer || buffer_size == 0 )
    return -1;

  const size_t size = strlen( path );

  return send_message( path,
                       size,
                       MSG_TYPE_READ,
                       "localhost",
                       8081,
                       buffer,
                       buffer_size );
}

// Se llama cuando el archivo se escribe por en el master y se necesita replicar en el esclavo
void write_remote_file( const char *path,
                        const void *content,
                        size_t      content_size ) {
  if ( !path || !content || content_size == 0 ) {
    return;
  }

  size_t path_len = strlen( path );
  size_t total_size =
      path_len + 1 + content_size; // path + null terminator + content

  // Create buffer: path (null-terminated) + content
  char buffer[total_size];
  memcpy( buffer, path, path_len );
  buffer[path_len] = '\0'; // Null terminator
  memcpy( buffer + path_len + 1, content, content_size );

  send_message( buffer,
                total_size,
                MSG_TYPE_WRITE,
                "localhost",
                8081,
                NULL,
                0 );
}

// Lo ejecuta el esclavo por una peticion del master
ssize_t read_local_file( const char *path, void *buffer, size_t buffer_size ) {
  printf( "FILE PATH TO READ: %s\n", path );

  if ( !buffer || buffer_size == 0 ) {
    return -1;
  }

  int fd = open( path, O_RDONLY );

  if ( fd < 0 ) {
    perror( "[bwfs] read_local_file: open failed" );
    return -1;
  }

  // Read file content directly into the provided buffer
  ssize_t total_read = 0;
  ssize_t bytes_read;

  while ( total_read < (ssize_t)buffer_size ) {
    bytes_read =
        read( fd, (char *)buffer + total_read, buffer_size - total_read );

    if ( bytes_read < 0 ) {
      perror( "[bwfs] read_local_file: read failed" );
      close( fd );
      return -1;
    }

    if ( bytes_read == 0 ) {
      break; // EOF
    }

    total_read += bytes_read;
  }

  close( fd );

  return total_read;
}

// Lo ejecuta el esclavo por una peticion del master
ssize_t
write_local_file( const char *path, const void *content, size_t content_size ) {
  printf( "WRITE FILE PATH: %s\n", path );

  if ( !path || !path[0] || !content || content_size == 0 ) {
    fprintf( stderr, "[bwfs] write_local_file: invalid parameters\n" );
    return -1;
  }

  // Open file for writing (create if doesn't exist, truncate if exists)
  int fd = open( path, O_CREAT | O_WRONLY | O_TRUNC, 0644 );
  if ( fd < 0 ) {
    perror( "[bwfs] write_local_file: open failed" );
    return -1;
  }

  // Write content to file
  ssize_t total_written = 0;
  ssize_t bytes_written;

  while ( total_written < (ssize_t)content_size ) {
    bytes_written = write( fd,
                           (const char *)content + total_written,
                           content_size - total_written );

    if ( bytes_written < 0 ) {
      perror( "[bwfs] write_local_file: write failed" );
      close( fd );
      return -1;
    }

    if ( bytes_written == 0 ) {
      break; // Shouldn't happen, but handle it
    }

    total_written += bytes_written;
  }

  close( fd );

  printf( "Successfully wrote %zd bytes to file: %s\n", total_written, path );
  return total_written;
}

static int write_backing_file( int idx, const void *content, size_t size ) {
  if ( idx < 0 )
    return -EINVAL;

  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );

  int fd = open( filepath, O_WRONLY | O_TRUNC );

  if ( fd < 0 ) {
    perror( "[bwfs] write_backing_file: open failed" );
    return -EIO;
  }

  size_t total_written = 0;

  while ( total_written < size ) {
    ssize_t w = write( fd,
                       (const char *)content + total_written,
                       size - total_written );
    if ( w < 0 ) {
      perror( "[bwfs] write_backing_file: write failed" );
      close( fd );

      return -EIO;
    }

    if ( w == 0 )
      break;

    total_written += (size_t)w;
  }

  if ( ftruncate( fd, (off_t)size ) != 0 )
    perror( "[bwfs] write_backing_file: ftruncate warning" );

  close( fd );

  inode_t *ino = &bwfs.inodes[idx];
  ino->size = size;
  time_t now = time( NULL );
  ino->atime = now;
  ino->mtime = now;
  ino->ctime = now;
  save_metadata();

  return 0;
}

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

  // Obtiene la ruta absoluta del archivo (bwfs_metadata.bin)
  if ( !realpath( META_FILENAME, meta_path ) ) {
    /* puede no existir aún */
    return -1;
  }

  // Se abre el archivo de metadata en modo de lectura binaria
  FILE *fp = fopen( meta_path, "rb" );
  if ( !fp )
    return -1;
  // Ahora se lee todo el contenido del archivo de metadata en la estructura disk
  size_t r = fread( &disk, sizeof( disk ), 1, fp );
  fclose( fp );
  if ( r != 1 )
    return -1;

  if ( disk.magic != BWFS_MAGIC )
    return -1;

  // Realiza la copia de los parámetros del disco a memoria
  strncpy( bwfs.storage_path,
           disk.storage_path,
           sizeof( bwfs.storage_path ) - 1 );
  bwfs.storage_path[sizeof( bwfs.storage_path ) - 1] = '\0';
  bwfs.max_block_bytes =
  // Se copia el tamaño del bloque, si no está definido, usar valores por defecto
      disk.max_block_bytes ? disk.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
  normalize_storage_path();

  /* inicializar inodos en memoria */
  for ( int i = 0; i < MAX_FILES; ++i ) {
    // Marca el inodo como usado o libre según el metadata
    bwfs.inodes[i].used = disk.inodes[i].used;
    if ( disk.inodes[i].used ) {
        // En caso que el inodo esta ocupado, pues copia el nombre, permisos y tamaño
      strncpy( bwfs.inodes[i].name, disk.inodes[i].name, MAX_NAME_LEN );
      bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
      bwfs.inodes[i].mode =
          disk.inodes[i].mode ? disk.inodes[i].mode : ( S_IFREG | 0644 );
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
    // Inicializa el time timestamps del acceso, modificación y creación al tiempo actual
    time_t now = time( NULL );
    bwfs.inodes[i].atime = now;
    bwfs.inodes[i].mtime = now;
    bwfs.inodes[i].ctime = now;
  }

  return 0;
}

// Almacena la metadata de memoria en el bwfs_metadata.bin
static int save_metadata( void ) {
  bwfs_disk_t disk;
  // Inicializa la estructura temporal en disco a cero
  // Asegura que cualquier padding o campo sin utilizar se guarde como 0
  memset( &disk, 0, sizeof( disk ) );
  // Escribe en el magic number BWFS, para validar que el archivo es el correcto al leerlo
  disk.magic = BWFS_MAGIC;
  // Copiar la ruta de almacenamiento desde la estructura en memoria global bwfs, a la estructura
  // en disco. Limita la copia al tamaño máximo de storage path
  strncpy( disk.storage_path,
           bwfs.storage_path,
           sizeof( disk.storage_path ) - 1 );
  // Almacena el tamaño máximo del bloque, utilizando el valor por defecto si es cero
  disk.max_block_bytes =
      bwfs.max_block_bytes ? bwfs.max_block_bytes : DEFAULT_MAX_BLOCK_BYTES;
  // Ahora copia los inodos desde la memoria, a la estructura de disco
  for ( int i = 0; i < MAX_FILES; ++i ) {
    disk.inodes[i].used = bwfs.inodes[i].used;
    if ( bwfs.inodes[i].used ) {
      // Si el inodo está activo, pues copia el nombre, tamaño y permisos
      strncpy( disk.inodes[i].name,
               bwfs.inodes[i].name,
               sizeof( disk.inodes[i].name ) - 1 );
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
  FILE *fp = fopen( META_FILENAME, "wb" );
  if ( !fp ) {
    // En caso que falle abrir el archivo, informar sobre el error
    fprintf( stderr, "[bwfs] save_metadata: fopen failed %s\n", META_FILENAME );
    return -1;
  }
  // Escritura de la esctructura completa de disco en bwfs_metadata.bin
  size_t w = fwrite( &disk, sizeof( disk ), 1, fp );
  fclose( fp );
  return ( w == 1 ) ? 0 : -1;
}

/* ---- init hook ---- */
// Esta funcion se ejecuta de forma automatica cuando se monta el FS usando FUSE
// Como tal tiene el objetivo de inicializar el estado del FS en memoria, cargar metadata y
// asegurar que el directorio de almacenamiento exista.
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
// Se busca extraer el nombre base del path absoluta
static const char *basename_from_path( const char *path ) {
  return ( path && path[0] == '/' ) ? path + 1 : path;
}
// Busca un inodo activo en memoria dado por un path
// Hace un retorno del índica del inodo en bwfs.inodes[] o -1 en caso que no exista
static int find_inode_by_name( const char *path ) {
  const char *fname = basename_from_path( path ); // Extrae solamente el nombre
  if ( !fname || !fname[0] ) // path vacio o no valido
    return -1;
  // Se recorre todos los inodos activos
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 )
    // Retornamos el índice si coincide con el nombre  
    return i;
  }
  // En caso que no encuentra inodo
  return -1;
}

/* Reserva un inodo y crea el backing file. Retorna índice >=0 o -errno */
static int allocate_inode( const char *path, mode_t mode ) {
  // Obtiene solo el nombre de archivo desde la ruta completa
  const char *fname = basename_from_path( path );

  // Validaciones iniciales
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
  // Busca de un nodo libre
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( !bwfs.inodes[i].used ) { // En caso que el nodo esté libre, se incializa
      bwfs.inodes[i].used = 1;  // Marcarlo como ocupado
      strncpy( bwfs.inodes[i].name, fname, MAX_NAME_LEN ); // Guarda el nombre
      bwfs.inodes[i].name[MAX_NAME_LEN] = '\0'; // Asegura el null terminator
      bwfs.inodes[i].mode = S_IFREG | ( mode & 0777 ); // Tipo de archivo regular y permisos POSIX
      bwfs.inodes[i].uid = getuid(); // Propietario actual
      bwfs.inodes[i].gid = getgid(); // Grupo actual
      bwfs.inodes[i].size = 0; // Tamaño incial 0
      time_t now = time( NULL ); // Obtener el timestampo actual
      bwfs.inodes[i].atime = bwfs.inodes[i].mtime = bwfs.inodes[i].ctime = now; 

      // Creacion de ruta del archivo fisico
      char filepath[PATH_MAX];
      int  rc = snprintf( filepath,
                         sizeof( filepath ),
                         "%s/file_%04d.dat",
                         bwfs.storage_path,
                         i );
      if ( rc < 0 || (size_t)rc >= sizeof( filepath ) ) {
        // En caso que la ruta exceda el tamaño, deshace la asignación
        bwfs.inodes[i].used = 0;
        bwfs.inodes[i].name[0] = '\0';
        return -ENAMETOOLONG;
      }
      // Creación de un archivo físico vacío
      int fd = open( filepath, O_CREAT | O_EXCL | O_WRONLY, 0644 ); // Crear solo si no existe
      if ( fd < 0 ) {
        if ( errno == EEXIST ) {
          // En caso que exista, se trunca para asegurar el estado consistente
          fd = open( filepath, O_TRUNC | O_WRONLY );
          if ( fd < 0 ) {
            perror( "[bwfs] allocate_inode open truncate" );
            bwfs.inodes[i].used = 0;
            bwfs.inodes[i].name[0] = '\0';
            return -EIO;
          }
        } else { // Error diferente a la existencia
          perror( "[bwfs] allocate_inode open" );
          bwfs.inodes[i].used = 0;
          bwfs.inodes[i].name[0] = '\0';
          return -EIO;
        }
      }
      // Cierra el descriptor de archivo
      close( fd );

      // Almacena el metadata actualizada
      if ( save_metadata() != 0 ) {
        // En caso que falla, se deshace todo (rollback)
        unlink( filepath ); //Borra el archivo físico
        bwfs.inodes[i].used = 0; // Libera el inodo
        bwfs.inodes[i].name[0] = '\0';
        return -EIO;
      }
      // Imprime información de debugging
      fprintf( stderr,
               "[bwfs] allocate_inode: allocated idx=%d name='%s' file='%s'\n",
               i,
               bwfs.inodes[i].name,
               filepath );
      return i; //Hace un retorno al índice del inodo asignado
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
static int bwfs_getattr( const char            *path,
                         struct stat           *stbuf,
                         struct fuse_file_info *fi ) {
  (void)fi;
  // Inicializamos con la estructura que se devolverá, esto evita que los valores que no se usen
  // en campos que no estan escritos
  memset( stbuf, 0, sizeof( struct stat ) );
  // Presentamos un caso especial, la raíz del FS '/' debe presentarse como un directorio.
  // En este bloque devuelve el modo de directorio y número de los enlaces
  if ( strcmp( path, "/" ) == 0 ) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;   // Ahora la convención '.' y '..' para 2 enlaces mínimos
    return 0;
  }

  // Para cualquier otro path se espera un nombre simple '/nombre' sin subdirectorios
  // luego find inode by name busca la tabla de inodos en memoria y devuelve el índice
  // del nodo si existe, o -1 si no lo encuentra
  int idx = find_inode_by_name( path );

  // En caso que no exista devuelve, no such file or directory
  if ( idx < 0 ) {
    printf( "%s\n", "File does not exists. Trying reading remote" );

    char dest[PATH_MAX];
    snprintf( dest, sizeof( dest ), "mnt%s", path );

    size_t buffer_size = bwfs.max_block_bytes;

    if ( buffer_size == 0 )
      buffer_size = DEFAULT_MAX_BLOCK_BYTES;

    char *content = malloc( buffer_size );

    if ( !content )
      return -ENOMEM;

    ssize_t remote_size = read_remote_file( dest, content, buffer_size );

    if ( remote_size < 0 ) {
      free( content );

      return -ENOENT;
    }

    int new_idx = allocate_inode( path, S_IFREG | 0644 );

    if ( new_idx < 0 ) {
      free( content );
      return new_idx;
    }

    if ( write_backing_file( new_idx, content, (size_t)remote_size ) != 0 ) {
      free( content );
      bwfs_unlink( path );

      return -EIO;
    }

    free( content );

    idx = new_idx;
  }
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

static int bwfs_readdir( const char             *path,
                         void                   *buf,
                         fuse_fill_dir_t         filler,
                         off_t                   offset,
                         struct fuse_file_info  *fi,
                         enum fuse_readdir_flags flags ) {
  (void)offset;
  (void)fi;
  (void)flags;
  // Solo se permite listar el directorio raiz
  if ( strcmp( path, "/" ) != 0 )
    return -ENOENT;
  // Se presentan las entradas basicas de cualquier directorio
  filler( buf, ".", NULL, 0, 0 );
  filler( buf, "..", NULL, 0, 0 );
  // Ahora se agregan los archivos existentes segun los inodos
  for ( int i = 0; i < MAX_FILES; ++i ) {
    if ( bwfs.inodes[i].used )
      filler( buf, bwfs.inodes[i].name, NULL, 0, 0 );
  }

  return 0;
}

/**
 * 3. Función create, realiza el manejo de la creación de archivos en el FS, por ejemplo un 'touch'
 *  En esta funcion es llamada por FUSE cuando el usuario intenta crear un archivo
 * 
 */
static int
bwfs_create( const char *path, mode_t mode, struct fuse_file_info *fi ) {
  (void)fi;
  // Realiza el intento de reservar un inodo para este nuevo archivo
  int idx = allocate_inode( path, mode );
  // Maneja un error si no hay espacio o path invalido
  if ( idx < 0 ) {
    fprintf( stderr,
             "[bwfs] create: allocate_inode failed %d for path='%s'\n",
             idx,
             path );
    return idx; // Propaga el error hacia FUSE
  }
  return 0;
}

/**
 * 4. Funcion unlink, elimina un archivo del FS (rm)
 * 
 */
static int bwfs_unlink( const char *path ) {
  // Obtiene el nombre del archivo a partir de la ruta  
  const char *fname = basename_from_path( path );
  if ( !fname )
    return -ENOENT;
  // Recorre todo los inodos buscando uno que coincida por nombre
  for ( int i = 0; i < MAX_FILES; ++i ) {
    // Coincidencia, inodo usado y nombre igual
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 ) {
      char filepath[PATH_MAX];
      // Ruta especifica del archivo guardado en bwfs_storage
      snprintf( filepath,
                sizeof( filepath ),
                "%s/file_%04d.dat",
                bwfs.storage_path,
                i );
      // Intenta borrar el archivo fisico, si falla no hay problema
      unlink( filepath ); /* ignore error */
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
/* open: validar existencia (no hace mucho más, FUSE pedirá create antes si aplica) */
static int bwfs_open( const char *path, struct fuse_file_info *fi ) {
  (void)fi;
  // Realiza la busqueda del indice del inodo que corresponde al archivo dado
  int idx = find_inode_by_name( path );
  // En caso que el archivo no exista en la tabla de inodos, devuelve un error
  if ( idx < 0 )
    return -ENOENT;
  return 0; // Si existe, toma el archivo valido y lo abre
}

/**
 * 6. Funcion read, esta funcion se ejecuta cuando un programa intenta leer el contenido
 * de un archivo (cat,head,tail)
 * 
 */
/* read: signature correcta (ssize_t) */
static int bwfs_read( const char            *path,
                      char                  *buf,
                      size_t                 size,
                      off_t                  offset,
                      struct fuse_file_info *fi ) {
  (void)fi;
  printf( "BWFS_READ: path='%s' size=%zu offset=%jd\n",
          path,
          size,
          (intmax_t)offset );
  // Busca el inodo correspondiente al archivo solicitado
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;
  // Para cuando el offset es mayor o igual al tamaño del archivo
  inode_t *ino = &bwfs.inodes[idx];
  if ( (size_t)offset >= ino->size )
    return 0; /* EOF */
  // Si el cliente pide más bytes que los disponibles desde el offset, ajustamos el tamaño real
  // que sí se puede leer
  if ( offset + size > ino->size )
    size = ino->size - offset;
  // Construimos la ruta del archivo real "file_xxx.dat"
  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );
  // Ahora abrimos el archivo fisico
  int fd = open( filepath, O_RDONLY );
  if ( fd < 0 )
    return -EIO;
  // Posiciona el cursor en el offset solicitado
  if ( lseek( fd, offset, SEEK_SET ) == (off_t)-1 ) {
    close( fd );
    return -EIO;
  }
  // Lee desde el archivo real hacia el buffer FUSE
  ssize_t r = read( fd, buf, size );
  close( fd );
  if ( r < 0 )
    return -EIO;

  /* actualizar atime */
  ino->atime = time( NULL );
  save_metadata();
  return (int)r;
}
/**
 * 7. Funcion write, ejecuta cuando un programa quiere escribir datos en un archivo
 * (echo, nano, etc)
 */
/* write: signature correcta (ssize_t) */
static int bwfs_write( const char            *path,
                       const char            *buf,
                       size_t                 size,
                       off_t                  offset,
                       struct fuse_file_info *fi ) {
  (void)fi;
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;
  // Busca el inodo correspondiente al archivo
  inode_t *ino = &bwfs.inodes[idx];

  /* validar límite por bloque */
  uint64_t endpos = (uint64_t)offset + (uint64_t)size;
  if ( endpos > bwfs.max_block_bytes ) // Archivo seria demasiado grande
    return -EFBIG;
  // Contruye la ruta del archivo real
  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );
  // Abre el archivo fisico en modo de lectura/escritura
  int fd = open( filepath, O_RDWR );
  if ( fd < 0 )
    return -EIO;
  // Ahora posiciona el cursor en el offset solicitado
  if ( lseek( fd, offset, SEEK_SET ) == (off_t)-1 ) {
    close( fd );
    return -EIO;
  }
  // Escribe desde el buffer al archivo real
  ssize_t w = write( fd, buf, size );
  close( fd );
  if ( w < 0 )
    return -EIO; // Falla fisica de escritura
  // Realiza una actualizacion del nuevo tamaño en caso que crezca
  size_t newsize = (size_t)offset + (size_t)w;
  if ( newsize > ino->size )
    ino->size = newsize;
  ino->mtime = time( NULL ); // Realiza una actualizacion del nodo
  save_metadata();

  char dest[50] = "mnt";
  strncat( dest, path, sizeof( dest ) - strlen( dest ) - 1 );

  write_remote_file( dest, buf, size );

  return (int)w;
}

 /** 
 * 8. Funcion utimens(extra),  porque ayuda para actualizar los tiempos de acceso (atime)
 * modificacion (mtime) de un archivo
 */
/* utimens: actualizar tiempos */
static int bwfs_utimens( const char            *path,
                         const struct timespec  tv[2],
                         struct fuse_file_info *fi ) {
  (void)fi;
  // Obtenemos solo el nombre del archivo desde la ruta completa
  const char *fname = basename_from_path( path );
  if ( !fname )
    return -ENOENT;
  // Ahora se recorre toda la tabla de inodos buscando el archivo
  for ( int i = 0; i < MAX_FILES; ++i ) {
    
    if ( bwfs.inodes[i].used && strcmp( bwfs.inodes[i].name, fname ) == 0 ) {
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

/**
 * 9. Funcion rename, esta funcion cambia el nombre de una archivo dentro del FS  (mv mnt/prueba1.txt mnt/nuevo_nombre.txt)
 *  entonces este FS no renombra el archivo fisico del file_xxxx.dat sino que modifica el nombre
 *  logico almacenado en la metadata es decir inodos, esto para evitar conflictos y eficiencia en estructura que no modifique
 *  los datos dentro de los archivos sino que su inodo como tal 
)
 * 
 */
static int bwfs_rename( const char *from, const char *to, unsigned int flags ) {
  (void)flags; // fuse3 pasa flags, usualmente ignorables
               // Realiza una busqueda del archivo original a renombrar
  int idx = find_inode_by_name( from );
  if ( idx < 0 )
    return -ENOENT;
  // Extrae solo el nuevo nombre desde la ruta destino
  const char *newname = basename_from_path( to );
  if ( !newname || !newname[0] )
    return -EINVAL;
  if ( strlen( newname ) > MAX_NAME_LEN )
    return -ENAMETOOLONG; // Nombre muy extenso

  // Realiza la verificacion que no exista otro archivo con el mismo nombre
  if ( find_inode_by_name( to ) >= 0 )
    return -EEXIST; // ya existe un archivo con este nombre destino

  // Ahora construye paths fisicos, aunque no se renombra de forma fisica
  char oldpath[PATH_MAX], newpath[PATH_MAX];
  snprintf( oldpath,
            sizeof( oldpath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );
  snprintf( newpath,
            sizeof( newpath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );

  // Importante destacar que el oldpath y newpath terminan siendo iguales porque BWFS no
  // renombra files_xxxx.dat. El archivo fisico permanece intacto
  strncpy( bwfs.inodes[idx].name, newname, MAX_NAME_LEN );
  bwfs.inodes[idx].name[MAX_NAME_LEN] = '\0';
  bwfs.inodes[idx].ctime = time( NULL );

  // Ahora se realiza el cambio del nombre en el inodo y lo guarda en disco
  if ( save_metadata() != 0 ) {
    return -EIO;
  }

  // Mensaje util para el debugging
  fprintf( stderr, "[bwfs] rename: '%s' -> '%s'\n", from, newname );
  return 0;
}

/**
 * 10. Funcion access, en esta función se implementa la lógica de accesos tipo POSIX, entonces
 * FUSE la llama cuando un proceso requiere de: comprobar si un archivo existe, verificar permisos de lectura
 * verificar permisos de escritura, verificar permisos de ejecucion. La validacion de permisos segun 
 * el dueño del archivo (uid), grupo(gid), permisos 'other' y root siempre pasa.
 * 
 */


static int bwfs_access( const char *path, int mask ) {
  // Si se piden acceso al directorio raiz '/' devuelve éxito
  if ( strcmp( path, "/" ) == 0 )
    return 0; /* root dir siempre accesible */

  // Realiza la búsqueda del inodo que corresponde al archivo solicitado
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  inode_t *ino = &bwfs.inodes[idx];

  // Condicion par cuando se pregunta si existe(F_OK) no hay que revisar permisos
  if ( mask == 0 )
    return 0;

  // Obtiene la información del proceso que hace la llamada a FUSE
  struct fuse_context *ctx = fuse_get_context();
  // El usuairo llamante
  uid_t uid = ctx ? ctx->uid : getuid();
  // El grupo llamante
  gid_t gid = ctx ? ctx->gid : getgid();

  // Para la condicion de uid=0 simepre que tiene permiso para todo
  if ( uid == 0 )
    return 0;

  // Ahora los permisos POSIX almacenados en el inodo
  mode_t m = ino->mode;

  /**
     * Parte de la revisión de los permisos, owner
     * 
     */
  if ( uid == ino->uid ) {
    if ( ( mask & R_OK ) && !( m & S_IRUSR ) )
      return -EACCES;
    if ( ( mask & W_OK ) && !( m & S_IWUSR ) )
      return -EACCES;
    if ( ( mask & X_OK ) && !( m & S_IXUSR ) )
      return -EACCES;
    return 0; // Todos los permisos solicitados fueron validos
  }

  /**
     * Parte de revisión permisos, group
     * 
     */
  if ( gid == ino->gid ) {
    if ( ( mask & R_OK ) && !( m & S_IRGRP ) )
      return -EACCES;
    if ( ( mask & W_OK ) && !( m & S_IWGRP ) )
      return -EACCES;
    if ( ( mask & X_OK ) && !( m & S_IXGRP ) )
      return -EACCES;
    return 0;
  }

  /**
     * Parte de los permisos, other
     * 
     */
  if ( ( mask & R_OK ) && !( m & S_IROTH ) )
    return -EACCES;
  if ( ( mask & W_OK ) && !( m & S_IWOTH ) )
    return -EACCES;
  if ( ( mask & X_OK ) && !( m & S_IXOTH ) )
    return -EACCES;

  // si se logra pasar, pues todos los permisos solicitados estan permitidos
  return 0;
}

/**
 * 11. Función chmod (extra), funcion cambio de permisos de un archivo o directorio dentro del FS
 * entonces esta funcion es llamada cuando un usuario ejecuta el chmod sobre un archivo. La implementamos
 * para un mayor manejo en el file system (ej, chmod 777 mnt/file full accesos, chmod 444 mnt/file solo r-r-r)
 */

static int
bwfs_chmod( const char *path, mode_t mode, struct fuse_file_info *fi ) {
  (void)fi;

  // Se realiza la busqueda del inodo correspondiente al path
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;
  // Se obtiene el inodo dentro del sistema
  inode_t *node = &bwfs.inodes[idx];

  /**
     *  Ahora se realiza el cambio unicamente de los bits de modo que dan los permisos
     * 
     */
  node->mode =
      ( node->mode & S_IFMT ) |
      ( mode &
        07777 ); // Se conserva unicamente los bits del tipo de archivo (directorio, archivo, etc)
  node->ctime =
      time( NULL ); // Se actualiza el tiempo de cambio de estado (ctime)

  save_metadata(); //Guarda los cambios en el archivo de metadata del FS
  return 0;
}

 /**
  * 
  * 12. Funcion statfs, es la encargada de reportar el estado del FS, cuando el usuario ejecuta los comandos
  *  df -h mnt/, df -T mnt/, stat -f mnt/
  * 
  */

static int bwfs_statfs( const char *path, struct statvfs *st ) {
  (void)path;

  // Se inicializa la estructura con ceros
  memset( st, 0, sizeof( *st ) );

  // Capacidad total del sistema de archivos, entonces
  // MAX_FILES archivos * max_block_bytes por archivo
  unsigned long long total_bytes =
      (unsigned long long)MAX_FILES * bwfs.max_block_bytes;

  // Contador de inodos usados
  int used_inodes = 0;
  for ( int i = 0; i < MAX_FILES; i++ ) {
    if ( bwfs.inodes[i].used )
      used_inodes++;
  }
  // Inodos libres, espacio disponible para crear archivos
  int free_inodes = MAX_FILES - used_inodes;

  /**
     * Se establecen los campos de statvfs
     * 
     */
  st->f_bsize = 4096; // tamaño lógico de bloque reportado al sistema
  st->f_frsize =
      bwfs.max_block_bytes; // tamaño real del bloque de almacenamiento (1 file = 1 bloque)
  st->f_blocks = MAX_FILES; // cantidad total de bloques disponibles
  st->f_bfree =
      free_inodes; // bloques libres = archivos disponibles (cada file necesita 1 bloque)
  st->f_bavail =
      free_inodes; // bloques disponibles para usuarios sin privilegios

  st->f_files = MAX_FILES;   // total de inodos
  st->f_ffree = free_inodes; // inodos libres

  st->f_favail = free_inodes;   // disponible a usuario
  st->f_namemax = MAX_NAME_LEN; // longitud max nombre archivo

  // Mensaje de debugging
  printf( "[DEBUG] statfs: used=%d free=%d total_bytes=%llu\n",
          used_inodes,
          free_inodes,
          total_bytes );

  return 0;
}

/**
 * 13. Funcion mkdir, se implementa la operación de mkdir para crear un directorio dentro de FS
 * 
 */
static int bwfs_mkdir( const char *path, mode_t mode ) {
  // Se extrae unicamente el nombre final del path
  const char *name = basename_from_path( path );
  // Ahora se valida que el nombre exista
  if ( !name || !name[0] )
    return -EINVAL;

  // Verificacion que no exceda el tamaño máximo
  if ( strlen( name ) > MAX_NAME_LEN )
    return -ENAMETOOLONG;

  // Verificacion que existe un archivo/directorio con ese nombre
  if ( find_inode_by_name( path ) >= 0 )
    return -EEXIST;

  // Realiza la busqueda de un inodo libre
  for ( int i = 0; i < MAX_FILES; i++ ) {
    if ( !bwfs.inodes[i].used ) {

      // Se marca como usado
      bwfs.inodes[i].used = 1;
      // Almacena el nombre
      strncpy( bwfs.inodes[i].name, name, MAX_NAME_LEN );
      bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';

      // Ahora marca que este inodo es un directorio
      bwfs.inodes[i].mode = S_IFDIR | ( mode & 0777 );

      // Establece el propietario y grupo
      bwfs.inodes[i].uid = getuid();
      bwfs.inodes[i].gid = getgid();
      // Tamaño del directorio, 0 por simplicidad
      bwfs.inodes[i].size = 0;

      // Establece tiempos
      time_t now = time( NULL );
      bwfs.inodes[i].atime = now;
      bwfs.inodes[i].mtime = now;
      bwfs.inodes[i].ctime = now;

      // Ahora se guarda el metadata en el disco
      save_metadata();

      fprintf( stderr, "[bwfs] mkdir: '%s'\n", name );
      return 0;
    }
  }

  return -ENOSPC;
}

/**
 * 14. Funcion fsync, en esta funcion es llamada por FUSE cuando un proceso solicita forzar que todos los
 * datos pendientes en un archivo sean escritos de forma segura. Simplemente se abren los file_xxxx.dat asociado
 * al inodo y ejecutamos fsync(fd) por lo que se busca garantizar que todos los datos escritos por la funcion write
 * ya se encuentren persistidos
 * 
 * Ejemplos para prueba:
 *      - echo "HELLO" | dd of=mnt/fs_test1.bin bs=1 conv=fsync
 *           Genera una cadena de letras y un salto de linea, el dd escribe esos bytes en el archivo
 *           y el conv=fsync inidica que despues de escribir el archivo, ejecuta el fsync()
 *      - echo -n "A" | dd of=mnt/fs_test2.bin oflag=sync
 *          Escribe un solo byte de A, entonces cada write se comporta como un write + fsync
 *      - strace -e fsync dd if=/dev/zero of=mnt/fs_test3.bin bs=1 count=1 conv=fsync
 *          Muestra solamente las llamadas fsync realizadas por el comando, entonces escribe 1 byte y ejecuta el fsync
 * 
 */

static int
bwfs_fsync( const char *path, int datasync, struct fuse_file_info *fi ) {
  (void)fi;
  (void)datasync;
  // Se busca el inodo por nombre
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  // Contruimos la ruta fisica del file
  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );
  // Abre el file fisico
  int fd = open( filepath, O_RDWR );
  if ( fd < 0 )
    return -EIO;
  // Ahora realiza la ejecucion del fsync a nivel de sistema
  int r = fsync( fd );
  close( fd );

  if ( r < 0 )
    return -EIO;
  // Mensaje para debugging
  fprintf( stderr, "[bwfs] fsync(): %s\n", path );
  return 0;
}

/**
 * 15. Funcion flush, se ejecuta cuando un proceso cierra un archivo, pero otra instancia podria continuar abierta
 * , no realiza la liberacion de recursos. Busca por ejemplo que los cambios en el archivo fisico file_xxxx.dat esten 
 * sincronizados con el almacenamiento
 * 
 */
static int bwfs_flush( const char *path, struct fuse_file_info *fi ) {
  (void)fi;

  // Busca el inodo del archivo
  int idx = find_inode_by_name( path );
  if ( idx < 0 )
    return -ENOENT;

  // Construye ruta del archivo físico
  char filepath[PATH_MAX];
  snprintf( filepath,
            sizeof( filepath ),
            "%s/file_%04d.dat",
            bwfs.storage_path,
            idx );

  // Abre el archivo real
  int fd = open( filepath, O_RDWR );
  if ( fd < 0 )
    return -EIO;

  // Sincroniza cambios con el disco
  if ( fsync( fd ) < 0 ) {
    close( fd );
    return -EIO;
  }

  close( fd );

  // También guardamos metadata en disco
  save_metadata();
  return 0;
}

/**
 * 16. Funcion lseek, esta funcion permite que nuestro FS no use file handles, por lo que no guardamos el offset
 * pero si realizamos una validacion de que el archivo exista, el nuevo offset sea valido y que no supere el maximo
 * de bloque
 * 
 * Nosotros estamos utilizando FUSE3, pero para la funcion de lseek fue eliminado de la estructura de fuse_operations,
 * los offsets ahora los maneja FUSE de forma automatica, basandose de las funciones write y read. Lo anterior significa
 * que FUSE actualiza de forma interna el offset de cada file hadle sin que el FS tenga que preocuparse
 * 
 * Por lo tanto, el implementar un lseek en el FUSE3 no tiene mucho sentido y genera warming porque la estructura fuse_operations
 * ya no la reconoce 
 */

/* operaciones registradas en FUSE */
#if defined( EXCLUDE_BWFS_MAIN )
struct fuse_operations bwfs_oper = {
#else
static struct fuse_operations bwfs_oper = {
#endif
    .init       = bwfs_init,
    .getattr    = bwfs_getattr,
    .readdir    = bwfs_readdir,
    .create     = bwfs_create,
    .open       = bwfs_open,
    .read       = bwfs_read,
    .write      = bwfs_write,
    .unlink     = bwfs_unlink,
    .utimens    = bwfs_utimens,
    .rename     = bwfs_rename,
    .access     = bwfs_access,
    .chmod      = bwfs_chmod,
    .statfs     = bwfs_statfs,
    .mkdir      = bwfs_mkdir,
    .fsync      = bwfs_fsync,
    .flush      = bwfs_flush,
};

/* ---- main: procesa -c config.ini opcional y arranca FUSE ---- */
#ifndef EXCLUDE_BWFS_MAIN
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
#endif // EXCLUDE_BWFS_MAIN
