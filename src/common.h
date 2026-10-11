/* Shared between client and server. */
#ifndef COMMON_H
#define COMMON_H

#define SERVER_PORT 8085

/*
 * Wire protocol: the server drives the dialogue and sends plain text.
 * When it needs a line of input it sends one of these marker bytes; the
 * client then reads one line from stdin and sends it back terminated by '\n'.
 */
#define PROMPT_MARK '\x01' /* normal input */
#define SECRET_MARK '\x02' /* input that must not be echoed (passwords) */

#endif
