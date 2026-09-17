#pragma once
#include <cJSON.h>
#include <string>

struct GeniusCommandResult {
    bool recognized = false;
    bool accepted = false;
    std::string error;
};

// Transport-neutral adapter. A future queue consumer supplies one decoded
// command here; this layer owns no HTTP endpoint, polling or credentials.
GeniusCommandResult DispatchGeniusCommand(const cJSON* command);
