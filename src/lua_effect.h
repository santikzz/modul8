#pragma once

#include <string>
#include <vector>

// lua-scripted effects: *.lua files in the effects/ folder next to the exe.
// each script declares a name, a params list and a process() function; see the
// bundled scripts in effects/ for the format. loaded with luajit, so process()
// runs jit-compiled and is safe for the realtime block sizes this app uses.
namespace luafx {

// scans effects/*.lua and registers every valid script in the Registry. safe to
// call again to pick up new or edited scripts (existing nodes keep the old code
// until they are re-added). returns one message per script that failed to load.
std::vector<std::string> registerAll();

// the scripts folder, created on demand.
std::string dir();

}  // namespace luafx
