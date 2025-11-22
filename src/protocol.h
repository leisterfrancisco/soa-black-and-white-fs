#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// Protocol constants
#define PROTOCOL_MAGIC 0xB5F5
#define PROTOCOL_VERSION 1
#define MAX_MESSAGE_SIZE 4096
#define MAX_PATH_LENGTH 256

typedef enum {
  MSG_TYPE_INVALID = 0,
  MSG_TYPE_ECHO = 1,
  MSG_TYPE_GETATTR = 2,
  MSG_TYPE_READDIR = 3,
  MSG_TYPE_READ = 4,
  MSG_TYPE_WRITE = 5,
  MSG_TYPE_TEST = 6,
  MSG_TYPE_RESPONSE = 128,
  MSG_TYPE_ERROR = 255
} message_type_t;

// Protocol message header (network byte order)
typedef struct {
  uint16_t magic;    // Protocol magic number
  uint8_t  version;  // Protocol version
  uint8_t  type;     // Message type
  uint32_t length;   // Payload length (network byte order)
  uint32_t sequence; // Sequence number for request/response matching
} message_header_t;

// Protocol message (header + payload)
typedef struct {
  message_header_t header;
  uint8_t          payload[MAX_MESSAGE_SIZE];
} message_t;

// Function declarations
int  protocol_send_message( int sockfd, const message_t *msg );
int  protocol_receive_message( int sockfd, message_t *msg );
int  protocol_create_message( message_t     *msg,
                              message_type_t type,
                              const void    *payload,
                              size_t         payload_len,
                              uint32_t       sequence );
void protocol_print_message( const message_t *msg );

#endif // PROTOCOL_H
