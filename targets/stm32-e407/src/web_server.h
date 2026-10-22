#ifndef WEB_SERVER_H
#define WEB_SERVER_H

/* Starts the HTTP thread serving common/src/web_ui.c on port 80.
 * Call after network_bringup() (needs lwIP up). */
void web_server_start(void);

#endif /* WEB_SERVER_H */
