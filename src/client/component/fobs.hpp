#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace fobs
{
	// Process dispatch request, returns whether interception was successful
	bool process_dispatch_request(const std::string& cmd, const nlohmann::json& request);
	
	// Check if there's a pending dispatch response to process
	bool should_process_dispatch_response();
	
	// Get the fake response
	nlohmann::json get_fake_dispatch_response();
	
	// Clear pending dispatch
	void clear_pending_dispatch();
}
