// The one translation unit that knows when this image was built.
//
// It exists to be tiny. tools/gen_build_stamp.py rewrites build_stamp.h on
// every build, so this file is recompiled every build too -- which is the whole
// point, and is why the timestamp does not live in web_server.cpp where it
// would only be refreshed on the builds that happened to touch that file.
#include "app_state.h"

#if __has_include("build_stamp.h")
#include "build_stamp.h"
#endif

#ifndef EASYPILL_BUILD_STAMP
// Building without the generator -- a bare `g++` for a quick syntax check, or a
// checkout where the pre-build script did not run. Fall back to the macros,
// which are at least right for this file, and say so rather than claiming a
// precision we do not have.
#define EASYPILL_BUILD_STAMP __DATE__ " " __TIME__ " (ungenerated)"
#endif

namespace ep {

const char* buildStamp() { return EASYPILL_BUILD_STAMP; }

}  // namespace ep
