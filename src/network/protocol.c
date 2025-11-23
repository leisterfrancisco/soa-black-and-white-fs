#include <arpa/inet.h>
#include <protocol.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * Send a protocol message over the socket
 * Returns 0 on success, -1 on error
 */
int protocol_send_message( int sockfd, const message_t *msg ) {
  if ( !msg ) {
    return -1;
  }

  // Convert header fields to network byte order
  message_header_t header = msg->header;
  header.magic = htons( header.magic );
  header.length = htonl( header.length );
  header.sequence = htonl( header.sequence );

  // Send header
  ssize_t sent = send( sockfd, &header, sizeof( header ), 0 );
  if ( sent != sizeof( header ) ) {
    return -1;
  }

  // Send payload if present
  uint32_t payload_len = ntohl( header.length );
  if ( payload_len > 0 && payload_len <= MAX_MESSAGE_SIZE ) {
    sent = send( sockfd, msg->payload, payload_len, 0 );
    if ( sent != (ssize_t)payload_len ) {
      return -1;
    }
  }

  return 0;
}

/**
 * Receive a protocol message from the socket
 * Returns 0 on success, -1 on error
 */
int protocol_receive_message( int sockfd, message_t *msg ) {
  if ( !msg ) {
    return -1;
  }

  // Receive header
  ssize_t received =
      recv( sockfd, &msg->header, sizeof( msg->header ), MSG_WAITALL );

  if ( received != sizeof( msg->header ) ) {
    if ( received == 0 ) {
      // Connection closed
      return -2;
    }
    return -1;
  }

  // Convert header fields from network byte order
  msg->header.magic = ntohs( msg->header.magic );
  msg->header.length = ntohl( msg->header.length );
  msg->header.sequence = ntohl( msg->header.sequence );

  // Validate magic number
  if ( msg->header.magic != PROTOCOL_MAGIC ) {
    return -1;
  }

  // Receive payload if present
  uint32_t payload_len = msg->header.length;
  if ( payload_len > 0 ) {
    if ( payload_len > MAX_MESSAGE_SIZE ) {
      return -1; // Payload too large
    }
    received = recv( sockfd, msg->payload, payload_len, MSG_WAITALL );
    if ( received != (ssize_t)payload_len ) {
      return -1;
    }
  }

  return 0;
}

/**
 * Create a protocol message
 * Returns 0 on success, -1 on error
 */
int protocol_create_message( message_t     *msg,
                             message_type_t type,
                             const void    *payload,
                             size_t         payload_len,
                             uint32_t       sequence ) {
  if ( !msg ) {
    return -1;
  }

  if ( payload_len > MAX_MESSAGE_SIZE ) {
    return -1;
  }

  msg->header.magic = PROTOCOL_MAGIC;
  msg->header.version = PROTOCOL_VERSION;
  msg->header.type = (uint8_t)type;
  msg->header.length = (uint32_t)payload_len;
  msg->header.sequence = sequence;

  if ( payload && payload_len > 0 ) {
    memcpy( msg->payload, payload, payload_len );
  }

  return 0;
}

/**
 * Print message details (for debugging)
 */
void protocol_print_message( const message_t *msg ) {
  if ( !msg ) {
    return;
  }

  printf( "Message:\n" );
  printf( "  Magic: 0x%04X\n", msg->header.magic );
  printf( "  Version: %u\n", msg->header.version );
  printf( "  Type: %u\n", msg->header.type );
  printf( "  Length: %u\n", msg->header.length );
  printf( "  Sequence: %u\n", msg->header.sequence );

  if ( msg->header.length > 0 ) {
    printf( "  Payload: " );

    size_t len = msg->header.length < 64 ? msg->header.length : 64;

    for ( size_t i = 0; i < len; i++ ) {
      printf( "%02X ", msg->payload[i] );
    }

    if ( msg->header.length > 64 ) {
      printf( "..." );
    }

    printf( "\n" );
  }
}

void protocol_print_object( const message_t *msg ) {
  if ( !msg ) {
    return;
  }

  printf( "Message:\n" );
  printf( "  Magic: 0x%04X\n", msg->header.magic );
  printf( "  Version: %u\n", msg->header.version );
  printf( "  Type: %u\n", msg->header.type );
  printf( "  Length: %u\n", msg->header.length );
  printf( "  Sequence: %u\n", msg->header.sequence );

  if ( msg->header.length > 0 ) {
    printf( "  Payload: " );

    typedef struct {
      uint16_t model;
      bool     available;
      char     brand[32];
    } car_model_t;

    car_model_t car;

    switch ( msg->header.type ) {
    case MSG_TYPE_READ: {
      memcpy( &car, msg->payload, sizeof( car_model_t ) );

      printf( "\nModel: %d\nAvailable: %d\nBrand: %s\n",
              car.model,
              car.available,
              car.brand );

      break;
    }
    default: {
      printf( "%s\n", "Invalid format to decode" );
      break;
    }
    }

    printf( "\n" );
  }
}
