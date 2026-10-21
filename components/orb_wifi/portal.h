// The Orb OS — "The Orb Setup": the access point's captive DNS and setup page. Private to
// orb_wifi; the manager task starts and stops it as the PROVISIONING state comes and goes.
#pragma once

namespace portal {

// Start the DNS responder and the web server on the access point's address. The access point
// itself (APSTA mode, its config) is the manager's job and is up before this is called.
bool start();
void stop();
bool running();

}  // namespace portal
