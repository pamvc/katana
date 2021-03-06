#include "tsuba/NameServerClient.h"

#include "GlobalState.h"

void
tsuba::SetMakeNameServerClientCB(
    std::function<galois::Result<std::unique_ptr<tsuba::NameServerClient>>()>
        cb) {
  GlobalState::set_make_name_server_client_cb(cb);
}
