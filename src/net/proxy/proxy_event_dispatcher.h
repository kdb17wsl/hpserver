#pragma once

#include <cstdint>

#include "proxy_session_manager.h"

class proxy_event_dispatcher {
public:
    static int handle_client_session_io(proxy_session& session, std::uint32_t events_mask);
    static int handle_upstream_session_io(proxy_session& session, std::uint32_t events_mask);

private:
    static int finalize_connect_if_needed(proxy_session& session);
};