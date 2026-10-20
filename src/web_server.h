// The Orb OS — the web server at http://theorb.local/. See web_server.cpp.
#pragma once

void web_server_begin();    // register every route and start listening; setup()
void web_server_handle();   // serve pending requests; loop(), core 1
