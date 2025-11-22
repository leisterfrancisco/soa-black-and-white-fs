#include <client.h>

void send_basic_message() {
  const char  *test_message = "Hello, Server!";
  const size_t msg_size = strlen( test_message );

  send_message( test_message, msg_size );
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

  send_message( &car, msg_size );
}

int main( int argc, char *argv[] ) {
  (void)argc;
  (void)argv;

  send_basic_message();
  send_custom_body_message();

  return 0;
}
