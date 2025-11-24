#include "protocol.h"
#include <client.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void send_echo_message() {
  printf( "\n=== Sending ECHO message ===\n" );
  int result = send_message( NULL, 0, MSG_TYPE_ECHO );

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
  int result = send_message( test_message, msg_size, MSG_TYPE_ECHO );

  if ( result == 0 ) {
    printf( "\n✓ Basic message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send basic message\n" );
  }
}

void send_read_message() {
  const char  *test_message = "mnt/file1.txt";
  const size_t msg_size = strlen( test_message );

  printf( "\n=== Sending READ message for: %s ===\n", test_message );
  int result = send_message( test_message, msg_size, MSG_TYPE_READ );

  if ( result == 0 ) {
    printf( "\n✓ READ message sent successfully\n" );
  } else {
    printf( "\n✗ Failed to send READ message\n" );
  }
}

void send_write_message() {
  const char *path = "mnt/file2.txt";
  const char *content =
      "This is the content to write to file2.txt\nLine 2 of content\n";

  size_t path_len = strlen( path );
  size_t content_len = strlen( content );
  size_t total_size =
      path_len + 1 + content_len; // path + null terminator + content

  // Create buffer: path (null-terminated) + content
  char buffer[total_size];
  memcpy( buffer, path, path_len );
  buffer[path_len] = '\0'; // Null terminator
  memcpy( buffer + path_len + 1, content, content_len );

  printf( "\n=== Sending WRITE message ===\n" );
  printf( "Path: %s\n", path );
  printf( "Content (%zu bytes): %s\n", content_len, content );

  int result = send_message( buffer, total_size, MSG_TYPE_WRITE );

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

  send_message( &car, msg_size, MSG_TYPE_READ );
}

int main( int argc, char *argv[] ) {
  (void)argc;
  (void)argv;

  // printf( "\n\n%s\n", "Sending echo message..." );
  // send_echo_message();

  // printf( "\n\n%s\n", "Sending basic message..." );
  // send_basic_message();

  printf( "\n\n%s\n", "Sending read message..." );
  send_read_message();

  printf( "\n\n%s\n", "Sending write message..." );
  send_write_message();
  //
  // printf( "\n\n%s\n", "Sending custom message..." );
  // send_custom_body_message();

  return 0;
}
