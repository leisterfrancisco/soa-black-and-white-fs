#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <network.h>
#include <protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * Connect to a server
 * Returns socket file descriptor on success, -1 on error
 */
int client_connect( const char *hostname, uint16_t port ) {
  struct sockaddr_in server_addr;
  struct hostent    *host;
  int                sockfd;

  sockfd = socket( AF_INET, SOCK_STREAM, 0 );

  if ( sockfd < 0 ) {
    perror( "socket() failed" );

    return -1;
  }

  // Resolve hostname
  host = gethostbyname( hostname );

  if ( !host ) {
    fprintf( stderr, "Failed to resolve hostname: %s\n", hostname );
    close( sockfd );

    return -1;
  }

  // Configure server address
  memset( &server_addr, 0, sizeof( server_addr ) );
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons( port );
  memcpy( &server_addr.sin_addr, host->h_addr, host->h_length );

  if ( connect( sockfd,
                (struct sockaddr *)&server_addr,
                sizeof( server_addr ) ) < 0 ) {
    perror( "connect() failed" );
    close( sockfd );

    return -1;
  }

  printf( "Connected to %s:%u\n", hostname, port );

  return sockfd;
}

/**
 * Close client socket
 */
void client_close( int sockfd ) {
  if ( sockfd >= 0 ) {
    close( sockfd );
  }
}

/**
 * Get error string for network errors
 */
const char *network_error_string( int err ) { return strerror( err ); }

/**
 * Simple test client
 */
int main( int argc, char *argv[] ) {
  const char *hostname = "localhost";
  uint16_t    port = DEFAULT_PORT;
  int         sockfd;
  message_t   msg, response;
  uint32_t    sequence = 1;

  // Parse command line arguments
  if ( argc > 1 ) {
    hostname = argv[1];
  }

  if ( argc > 2 ) {
    port = (uint16_t)atoi( argv[2] );

    if ( port == 0 ) {
      fprintf( stderr, "Invalid port number: %s\n", argv[2] );

      return 1;
    }
  }

  // Connect to server
  sockfd = client_connect( hostname, port );

  if ( sockfd < 0 ) {
    fprintf( stderr, "Failed to connect to server\n" );

    return 1;
  }

  const char *test_message = "Hello, Server!";

  if ( protocol_create_message( &msg,
                                MSG_TYPE_ECHO,
                                test_message,
                                strlen( test_message ),
                                sequence++ ) < 0 ) {
    fprintf( stderr, "Failed to create message\n" );

    client_close( sockfd );

    return 1;
  }

  printf( "Sending message:\n" );

  protocol_print_message( &msg );

  if ( protocol_send_message( sockfd, &msg ) < 0 ) {
    perror( "Failed to send message" );
    client_close( sockfd );

    return 1;
  }

  if ( protocol_receive_message( sockfd, &response ) < 0 ) {
    perror( "Failed to receive response" );
    client_close( sockfd );

    return 1;
  }

  printf( "Received response:\n" );
  protocol_print_message( &response );

  // Print response payload as string if it's a response
  if ( response.header.type == MSG_TYPE_RESPONSE &&
       response.header.length > 0 ) {
    printf( "Response payload: %.*s\n",
            (int)response.header.length,
            (char *)response.payload );
  }

  client_close( sockfd );

  return 0;
}
