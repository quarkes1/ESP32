#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>

/* ================================================================
 *  Public API
 * ================================================================ */

// Start WiFi AP, HTTP server, WebSocket, and push task
void web_server_init();

#endif // WEB_SERVER_H
