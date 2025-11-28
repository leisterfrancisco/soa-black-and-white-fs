#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <network.h>
#include <protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int         client_connect( const char *hostname, uint16_t port );
void        client_close( int sockfd );
const char *network_error_string( int err );
ssize_t     send_message( const void    *payload,
                          size_t         size,
                          message_type_t type,
                          const char    *c_hostname,
                          const uint16_t c_port,
                          char          *buff,
                          size_t         buff_size );
