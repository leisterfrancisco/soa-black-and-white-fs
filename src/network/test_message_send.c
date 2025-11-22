#include <client.h>
#include <stdio.h>

void send_basic_message() {
  const char  *test_message = "Hello, Server!";
  const size_t msg_size = strlen( test_message );

  send_message( test_message, msg_size, MSG_TYPE_ECHO );
}

void send_custom_body_message() {

  typedef struct {
    uint16_t model;
    char     brand[32];
  } car_model_t;

  car_model_t car = {
      .model = 2025,
      .brand = "Ford",
  };

  size_t msg_size = sizeof( car );

  send_message( &car, msg_size, MSG_TYPE_ECHO );
}

void send_echo_message() { send_message( NULL, 0, MSG_TYPE_ECHO ); }

int main( int argc, char *argv[] ) {
  (void)argc;
  (void)argv;

  printf( "\n\n%s\n", "Sending echo message..." );
  send_echo_message();

  printf( "\n\n%s\n", "Sending basic message..." );
  send_basic_message();

  printf( "\n\n%s\n", "Sending custom message..." );
  send_custom_body_message();

  return 0;
}
