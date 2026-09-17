/* Copyright (C) 2025 EchoStretch

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Serves the web UI on the given TCP port. Blocks until the server is
   asked to shut down. Returns 0 on a clean shutdown, -1 when the socket
   could not be opened. */
int http_server_run(int port);

/* Asks the accept loop to return. Safe to call from a request handler. */
void http_server_stop(void);

/* The port the server actually bound to, 0 before it is listening. */
int http_server_port(void);

#endif /* HTTP_SERVER_H */
