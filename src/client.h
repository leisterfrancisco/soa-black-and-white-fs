#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <network.h>
#include <protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int         client_connect( const char *hostname, uint16_t port );
void        client_close( int sockfd );
const char *network_error_string( int err );
int send_message( const void *payload, size_t size, message_type_t type );
