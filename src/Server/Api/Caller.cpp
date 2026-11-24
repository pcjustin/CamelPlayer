// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Api/Caller.h"

#include <utility>

namespace camel::server {

namespace {

thread_local Caller current;

} // namespace

const Caller& currentCaller() {
    return current;
}

CallerScope::CallerScope(Caller caller) : previous_(std::exchange(current, std::move(caller))) {}

CallerScope::~CallerScope() {
    current = std::move(previous_);
}

} // namespace camel::server
