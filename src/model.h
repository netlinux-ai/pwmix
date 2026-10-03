#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Node {
	uint32_t id = 0;
	std::string cls, name, desc, nick, app, binary, icon, media_name, api, state;
	int pid = 0;
	bool has_device = false;
	bool have_vol = false;
	float vol = 1.0f;
	bool mute = false;
	int nch = 2;

	std::string label() const
	{
		if (!app.empty() && cls.rfind("Stream/", 0) == 0) return app;
		if (!desc.empty()) return desc;
		if (!nick.empty()) return nick;
		return name;
	}
};

struct Port {
	uint32_t id = 0, node = 0;
	bool out = false;
	std::string name, channel, format;
};

struct Link {
	uint32_t id = 0, onode = 0, oport = 0, inode = 0, iport = 0;
};

struct Graph {
	std::map<uint32_t, Node> nodes;
	std::map<uint32_t, Port> ports;
	std::map<uint32_t, Link> links;
	std::string remote;
};
