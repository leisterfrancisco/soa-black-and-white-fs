#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>

// Network configuration
#define DEFAULT_PORT 8080
#define DEFAULT_BACKLOG 10
#define MAX_CLIENTS 32

// Server functions
int  server_init( uint16_t port );
int  server_accept_client( int server_fd );
void server_close( int sockfd );

// Client functions
int  client_connect( const char *hostname, uint16_t port );
void client_close( int sockfd );
int  send_message( const void *payload, size_t size );

// Utility functions
const char *network_error_string( int err );

#endif // NETWORK_H
