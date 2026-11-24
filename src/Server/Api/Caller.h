// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <string>

namespace camel::server {

// Who a Server call comes from while it runs on this thread (docs/Plan.md §17.8): a paired device through the Server
// API, or nobody for the Server's own window.
struct Caller {
    std::string device, name;
    uint64_t connection = 0; // the Server API connection it came on
};

const Caller& currentCaller();

// Makes `caller` the current one on this thread until it ends.
class CallerScope {
public:
    explicit CallerScope(Caller caller);
    ~CallerScope();
    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

private:
    Caller previous_;
};

} // namespace camel::server
