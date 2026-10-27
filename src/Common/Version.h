// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// CAMEL_VERSION (from the VERSION file) is defined by the projects that show it; see Directory.Build.props.
#pragma once

#define CAMEL_STRINGIZE_(x) #x
#define CAMEL_STRINGIZE(x) CAMEL_STRINGIZE_(x)
#define CAMEL_VERSION_STRING CAMEL_STRINGIZE(CAMEL_VERSION)
