// Phone remote: a tiny HTTP server on the LAN (control page + /state + /set), a UDP beacon so the
// Android app finds the computer by itself, and a QR code with the address printed to the terminal.
// Runs on its own thread; the main loop drains commands with RemotePoll and publishes RemoteSetState.
#ifndef REMOTE_H
#define REMOTE_H

typedef struct { char key[24]; float val; } RemoteCmd;

#define REMOTE_PORT   8777     // HTTP (REMOTE_PORT=... in the environment); busy -> next free one
#define REMOTE_BEACON 8778     // UDP broadcast "OVERTONE <port> <host>" once a second

void RemoteStart(int port);
int  RemotePoll(RemoteCmd *out, int max);   // commands received since the last call
void RemoteSetState(const char *json);      // what GET /state returns

#endif
