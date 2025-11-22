#include <arpa/inet.h>
#include <netinet/in.h>
#include <network.h>
#include <protocol.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int          server_fd = -1;
static volatile int running = 1;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler( int sig ) {
  (void)sig;
  running = 0;
  if ( server_fd >= 0 ) {
    close( server_fd );
    server_fd = -1;
  }
}

/**
 * Handle a client connection
 */
static void handle_client( int client_fd ) {
  message_t msg;
  int       ret;

  printf( "Client connected (fd=%d)\n", client_fd );

  while ( running ) {
    ret = protocol_receive_message( client_fd, &msg );

    if ( ret < 0 ) {
      if ( ret == -2 ) {
        printf( "Client disconnected (fd=%d)\n", client_fd );
      } else {
        perror( "Error receiving message" );
      }
      break;
    }

    printf( "Received message:\n" );
    protocol_print_message( &msg );

    switch ( msg.header.type ) {
    case MSG_TYPE_ECHO: {
      message_t response;

      protocol_create_message( &response,
                               MSG_TYPE_RESPONSE,
                               msg.payload,
                               msg.header.length,
                               msg.header.sequence );
      if ( protocol_send_message( client_fd, &response ) < 0 ) {
        perror( "Error sending echo response" );
        goto cleanup;
      }
      printf( "Sent echo response\n" );
      break;
    }

    case MSG_TYPE_READ: {
      printf( "%s\n", "Performing READ operation" );

      protocol_print_object( &msg );

      // CONNECTION: look for for data and return it

      message_t   response;
      const char *ack = "ACK";

      protocol_create_message( &response,
                               MSG_TYPE_RESPONSE,
                               ack,
                               strlen( ack ),
                               msg.header.sequence );

      if ( protocol_send_message( client_fd, &response ) < 0 ) {
        perror( "Error sending response" );

        goto cleanup;
      }

      printf( "Sent acknowledgment\n" );

      break;
    }
    case MSG_TYPE_WRITE: {
      printf( "%s\n", "Performing WRITE operation" );

      protocol_print_object( &msg );

      // CONNECTION: write to memory

      message_t   response;
      const char *ack = "ACK";

      protocol_create_message( &response,
                               MSG_TYPE_RESPONSE,
                               ack,
                               strlen( ack ),
                               msg.header.sequence );

      if ( protocol_send_message( client_fd, &response ) < 0 ) {
        perror( "Error sending response" );

        goto cleanup;
      }

      printf( "Sent acknowledgment\n" );

      break;
    }
    case MSG_TYPE_TEST: {
      message_t   response;
      const char *ack = "ACK";

      protocol_create_message( &response,
                               MSG_TYPE_RESPONSE,
                               ack,
                               strlen( ack ),
                               msg.header.sequence );

      if ( protocol_send_message( client_fd, &response ) < 0 ) {
        perror( "Error sending response" );
        goto cleanup;
      }

      printf( "Sent acknowledgment\n" );

      break;
    }

    default: {
      // Unknown message type - send error
      message_t   error;
      const char *error_msg = "Unknown message type";
      protocol_create_message( &error,
                               MSG_TYPE_ERROR,
                               error_msg,
                               strlen( error_msg ),
                               msg.header.sequence );
      protocol_send_message( client_fd, &error );
      break;
    }
    }
  }

cleanup:
  close( client_fd );
  printf( "Client connection closed (fd=%d)\n", client_fd );
}

/**
 * Initialize and start the server
 * Returns server file descriptor on success, -1 on error
 */
int server_init( uint16_t port ) {
  struct sockaddr_in server_addr;
  int                opt = 1;

  // Create socket
  server_fd = socket( AF_INET, SOCK_STREAM, 0 );
  if ( server_fd < 0 ) {
    perror( "socket() failed" );
    return -1;
  }

  // Set socket options
  if ( setsockopt( server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof( opt ) ) <
       0 ) {
    perror( "setsockopt() failed" );
    close( server_fd );
    return -1;
  }

  // Configure server address
  memset( &server_addr, 0, sizeof( server_addr ) );
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons( port );

  // Bind socket
  if ( bind( server_fd,
             (struct sockaddr *)&server_addr,
             sizeof( server_addr ) ) < 0 ) {
    perror( "bind() failed" );
    close( server_fd );
    return -1;
  }

  // Listen for connections
  if ( listen( server_fd, DEFAULT_BACKLOG ) < 0 ) {
    perror( "listen() failed" );
    close( server_fd );
    return -1;
  }

  // Set up signal handlers for graceful shutdown
  signal( SIGINT, signal_handler );
  signal( SIGTERM, signal_handler );

  printf( "Server listening on port %u\n", port );
  return server_fd;
}

/**
 * Accept a new client connection
 * Returns client file descriptor on success, -1 on error
 */
int server_accept_client( int server_fd ) {
  struct sockaddr_in client_addr;
  socklen_t          client_len = sizeof( client_addr );
  int                client_fd;

  client_fd = accept( server_fd, (struct sockaddr *)&client_addr, &client_len );
  if ( client_fd < 0 ) {
    if ( running ) {
      perror( "accept() failed" );
    }
    return -1;
  }

  printf( "Accepted connection from %s:%d\n",
          inet_ntoa( client_addr.sin_addr ),
          ntohs( client_addr.sin_port ) );

  return client_fd;
}

/**
 * Close server socket
 */
void server_close( int sockfd ) {
  if ( sockfd >= 0 ) {
    close( sockfd );
  }
}

/**
 * Main server loop
 */
int main( int argc, char *argv[] ) {
  uint16_t port = DEFAULT_PORT;
  int      client_fd;

  // Parse command line arguments
  if ( argc > 1 ) {
    port = (uint16_t)atoi( argv[1] );
    if ( port == 0 ) {
      fprintf( stderr, "Invalid port number: %s\n", argv[1] );
      return 1;
    }
  }

  // Initialize server
  if ( server_init( port ) < 0 ) {
    fprintf( stderr, "Failed to initialize server\n" );
    return 1;
  }

  printf( "Server started. Press Ctrl+C to stop.\n" );

  // Main accept loop
  while ( running ) {
    client_fd = server_accept_client( server_fd );
    if ( client_fd < 0 ) {
      if ( !running ) {
        break; // Server is shutting down
      }
      continue;
    }

    // Handle client (for now, single-threaded)
    // In a production server, you'd fork() or use threads here
    handle_client( client_fd );
  }

  printf( "Server shutting down...\n" );
  server_close( server_fd );

  return 0;
}
