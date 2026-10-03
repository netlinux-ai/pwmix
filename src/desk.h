#pragma once
#include "model.h"

#include <X11/Xlib.h>
#include <string>
#include <vector>

struct Win {
	unsigned long xid = 0;
	int pid = 0;
	int desktop = 0;
	std::string title, wmclass;
};

struct Player {
	std::string bus;
	int pid = 0;
	std::string status, title, artist;
};

struct DeskState {
	std::vector<Win> wins;
	std::vector<Player> players;
	unsigned long active = 0;
	int cur_desktop = 0;
};

// Fills wins/active/cur_desktop from the EWMH properties of the root window.
void desk_scan_x11(Display *dpy, DeskState &out);

// Chain of process ids: pid, parent, grandparent ... (excluding 1 and 0).
std::vector<int> pid_chain(int pid);

// Polls MPRIS players over the session bus on a worker thread.
class Mpris {
public:
	Mpris();
	~Mpris();
	void start();
	void stop();
	std::vector<Player> get();

	struct Impl;
private:
	Impl *d;
};

struct Ident {
	std::string text;   // what to show under the node name ("" = nothing)
	int confidence = 0; // 0 none, 1 guess (several windows), 2 matched
};

// Works out which desktop window(s) an application stream belongs to.
Ident identify(const Node &n, const DeskState &ds);
