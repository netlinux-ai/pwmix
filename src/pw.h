#pragma once
#include "model.h"
#include <cstdint>

class PwClient {
public:
	PwClient();
	~PwClient();

	bool start();
	void stop();

	// Copy of the current graph, safe to call from any thread.
	Graph snapshot();
	uint64_t generation() const;

	void setMute(uint32_t node_id, bool mute);
	void setVolume(uint32_t node_id, float linear);

	struct Impl;

private:
	Impl *d;
};
