#pragma once

#include <string>
#include <vector>

class Graph;

// named rig snapshots, one plain-text file per preset under presets/. a preset
// captures the whole graph: every node and its position, the wiring, each param
// value and bypass state. the format is line-based and shareable: copy a .preset
// file to another machine and it loads as-is.
namespace preset {

std::string dir();                    // presets folder path, created on demand
std::vector<std::string> list();      // preset names (no extension), sorted
bool save(const std::string& name, const Graph& g);
bool load(const std::string& name, Graph& g);  // g must be rebuilt from stopped state
bool remove(const std::string& name);
bool exists(const std::string& name);

}  // namespace preset
