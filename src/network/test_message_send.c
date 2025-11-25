#include "protocol.h"
#include <client.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void send_echo_message() {
  printf( "\n=== Sending ECHO message ===\n" );
  int result = send_message( NULL, 0, MSG_TYPE_ECHO, "localhost", 8080 );

  if ( result == 0 ) {
    printf( "\n✓ ECHO message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send ECHO message\n" );
  }
}

void send_basic_message() {
  const char  *test_message = "Hello, Server!";
  const size_t msg_size = strlen( test_message );

  printf( "\n=== Sending basic ECHO message: %s ===\n", test_message );
  int result =
      send_message( test_message, msg_size, MSG_TYPE_ECHO, "localhost", 8080 );

  if ( result == 0 ) {
    printf( "\n✓ Basic message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send basic message\n" );
  }
}

void send_read_message( const char    *hostname,
                        const uint16_t port,
                        const char    *file_path ) {
  const size_t msg_size = strlen( file_path );

  printf( "\n=== Sending READ message for: %s ===\n", file_path );
  int result =
      send_message( file_path, msg_size, MSG_TYPE_READ, hostname, port );

  if ( result == 0 ) {
    printf( "\n✓ READ message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send READ message\n" );
  }
}

void send_write_message( const char    *hostname,
                         const uint16_t port,
                         const char    *file_path ) {
  // const char *path = "mnt/file2.txt";
  const char *content =
      "This is the content to write to file2.txt\nLine 2 of content\n";

  size_t path_len = strlen( file_path );
  size_t content_len = strlen( content );
  size_t total_size =
      path_len + 1 + content_len; // path + null terminator + content

  // Create buffer: path (null-terminated) + content
  char buffer[total_size];
  memcpy( buffer, file_path, path_len );
  buffer[path_len] = '\0'; // Null terminator
  memcpy( buffer + path_len + 1, content, content_len );

  printf( "\n=== Sending WRITE message ===\n" );
  printf( "Path: %s\n", file_path );
  printf( "Content (%zu bytes): %s\n", content_len, content );

  int result =
      send_message( buffer, total_size, MSG_TYPE_WRITE, hostname, port );

  if ( result == 0 ) {
    printf( "\n✓ WRITE message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send WRITE message\n" );
  }
}

void send_custom_body_message() {

  typedef struct {
    uint16_t model;
    bool     available;
    char     brand[32];
  } car_model_t;

  car_model_t car = {
      .model = 2025,
      .available = true,
      .brand = "Ford",
  };

  size_t msg_size = sizeof( car );

  send_message( &car, msg_size, MSG_TYPE_READ, "localhost", 8080 );
}

int main( int argc, char *argv[] ) {
  if ( argc != 5 ) {
    fprintf( stderr, "Usage: %s <hostname> <port> <file_path> <operation>\n", argv[0] );
    fprintf( stderr, "  operation: 'read' or 'write'\n" );
    return 1;
  }

  const char    *hostname = argv[1];
  const uint16_t port = (uint16_t)atoi( argv[2] );
  const char    *file_path = argv[3];
  const char    *operation = argv[4];

  if ( port == 0 ) {
    fprintf( stderr, "Error: Invalid port number: %s\n", argv[2] );
    return 1;
  }

  // Determine which operation to perform
  if ( strcmp( operation, "read" ) == 0 ) {
    printf( "\n\n%s\n", "Sending read message..." );
    send_read_message( hostname, port, file_path );
  } else if ( strcmp( operation, "write" ) == 0 ) {
    printf( "\n\n%s\n", "Sending write message..." );
    send_write_message( hostname, port, file_path );
  } else {
    fprintf( stderr, "Error: Invalid operation '%s'. Must be 'read' or 'write'\n", operation );
    return 1;
  }

  return 0;
}
