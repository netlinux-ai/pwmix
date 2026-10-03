#include "desk.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

// ---- X11 ---------------------------------------------------------------

static bool prop_longs(Display *d, Window w, Atom prop, std::vector<long> &out)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = nullptr;
	out.clear();
	if (XGetWindowProperty(d, w, prop, 0, 65536, False, AnyPropertyType, &type, &fmt,
	                       &n, &after, &data) != Success || !data)
		return false;
	if (fmt == 32)
		for (unsigned long i = 0; i < n; i++) out.push_back(((long *)data)[i]);
	XFree(data);
	return !out.empty();
}

static std::string prop_string(Display *d, Window w, Atom prop)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = nullptr;
	std::string s;
	if (XGetWindowProperty(d, w, prop, 0, 4096, False, AnyPropertyType, &type, &fmt,
	                       &n, &after, &data) == Success && data) {
		if (fmt == 8) s.assign((char *)data, n);
		XFree(data);
	}
	return s;
}

void desk_scan_x11(Display *dpy, DeskState &out)
{
	static Atom a_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	static Atom a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
	static Atom a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	static Atom a_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	static Atom a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	static Atom a_act = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

	Window root = DefaultRootWindow(dpy);
	std::vector<long> v;
	out.wins.clear();
	out.active = 0;
	out.cur_desktop = 0;

	if (prop_longs(dpy, root, a_cur, v)) out.cur_desktop = (int)v[0];
	if (prop_longs(dpy, root, a_act, v)) out.active = (unsigned long)v[0];

	std::vector<long> clients;
	if (!prop_longs(dpy, root, a_list, clients)) return;
	for (long id : clients) {
		Win w;
		w.xid = (unsigned long)id;
		if (prop_longs(dpy, (Window)id, a_pid, v)) w.pid = (int)v[0];
		if (prop_longs(dpy, (Window)id, a_desk, v)) w.desktop = (int)v[0];
		w.title = prop_string(dpy, (Window)id, a_name);
		if (w.title.empty()) {
			char *nm = nullptr;
			if (XFetchName(dpy, (Window)id, &nm) && nm) {
				w.title = nm;
				XFree(nm);
			}
		}
		XClassHint ch;
		if (XGetClassHint(dpy, (Window)id, &ch)) {
			if (ch.res_class) w.wmclass = ch.res_class;
			if (ch.res_name) XFree(ch.res_name);
			if (ch.res_class) XFree(ch.res_class);
		}
		out.wins.push_back(std::move(w));
	}
}

// ---- /proc -------------------------------------------------------------

static int parent_of(int pid)
{
	std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
	std::string line;
	if (!std::getline(f, line)) return 0;
	size_t p = line.rfind(')');
	if (p == std::string::npos) return 0;
	std::istringstream is(line.substr(p + 1));
	char state;
	int ppid = 0;
	is >> state >> ppid;
	return ppid;
}

std::vector<int> pid_chain(int pid)
{
	std::vector<int> c;
	while (pid > 1 && c.size() < 32) {
		c.push_back(pid);
		pid = parent_of(pid);
	}
	return c;
}

// ---- MPRIS -------------------------------------------------------------

struct Mpris::Impl {
	std::thread th;
	std::atomic<bool> run{false};
	std::mutex mu;
	std::vector<Player> players;
};

static void read_metadata(sd_bus_message *m, Player &p)
{
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = nullptr;
		sd_bus_message_read(m, "s", &key);
		char type;
		const char *contents;
		sd_bus_message_peek_type(m, &type, &contents);
		sd_bus_message_enter_container(m, 'v', contents);
		if (key && !strcmp(key, "xesam:title") && !strcmp(contents, "s")) {
			const char *s = nullptr;
			sd_bus_message_read(m, "s", &s);
			if (s) p.title = s;
		} else if (key && !strcmp(key, "xesam:artist") && !strcmp(contents, "as")) {
			sd_bus_message_enter_container(m, 'a', "s");
			const char *s = nullptr;
			if (sd_bus_message_read(m, "s", &s) > 0 && s) p.artist = s;
			while (sd_bus_message_read(m, "s", &s) > 0) {}
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, contents);
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

static void poll_players(sd_bus *bus, std::vector<Player> &out)
{
	out.clear();
	sd_bus_message *names = nullptr;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                       "org.freedesktop.DBus", "ListNames", &err, &names, "") < 0) {
		sd_bus_error_free(&err);
		return;
	}
	std::vector<std::string> bus_names;
	sd_bus_message_enter_container(names, 'a', "s");
	const char *nm;
	while (sd_bus_message_read(names, "s", &nm) > 0)
		if (!strncmp(nm, "org.mpris.MediaPlayer2.", 23)) bus_names.push_back(nm);
	sd_bus_message_unref(names);

	for (const auto &name : bus_names) {
		Player p;
		p.bus = name;
		sd_bus_creds *creds = nullptr;
		if (sd_bus_get_name_creds(bus, name.c_str(), SD_BUS_CREDS_PID, &creds) >= 0) {
			pid_t pid = 0;
			sd_bus_creds_get_pid(creds, &pid);
			p.pid = pid;
			sd_bus_creds_unref(creds);
		}
		sd_bus_message *m = nullptr;
		sd_bus_error e2 = SD_BUS_ERROR_NULL;
		if (sd_bus_call_method(bus, name.c_str(), "/org/mpris/MediaPlayer2",
		                       "org.freedesktop.DBus.Properties", "GetAll", &e2, &m, "s",
		                       "org.mpris.MediaPlayer2.Player") >= 0) {
			sd_bus_message_enter_container(m, 'a', "{sv}");
			while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
				const char *key = nullptr;
				sd_bus_message_read(m, "s", &key);
				char type;
				const char *contents;
				sd_bus_message_peek_type(m, &type, &contents);
				sd_bus_message_enter_container(m, 'v', contents);
				if (key && !strcmp(key, "PlaybackStatus")) {
					const char *s = nullptr;
					sd_bus_message_read(m, "s", &s);
					if (s) p.status = s;
				} else if (key && !strcmp(key, "Metadata")) {
					read_metadata(m, p);
				} else {
					sd_bus_message_skip(m, contents);
				}
				sd_bus_message_exit_container(m);
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_unref(m);
		}
		sd_bus_error_free(&e2);
		out.push_back(std::move(p));
	}
}

Mpris::Mpris() : d(new Impl) {}
Mpris::~Mpris()
{
	stop();
	delete d;
}

void Mpris::start()
{
	if (d->run.exchange(true)) return;
	d->th = std::thread([this] {
		sd_bus *bus = nullptr;
		if (sd_bus_open_user(&bus) < 0) return;
		sd_bus_set_method_call_timeout(bus, 500000);
		while (d->run) {
			std::vector<Player> ps;
			poll_players(bus, ps);
			{
				std::lock_guard<std::mutex> lk(d->mu);
				d->players = std::move(ps);
			}
			for (int i = 0; i < 20 && d->run; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		sd_bus_unref(bus);
	});
}

void Mpris::stop()
{
	d->run = false;
	if (d->th.joinable()) d->th.join();
}

std::vector<Player> Mpris::get()
{
	std::lock_guard<std::mutex> lk(d->mu);
	return d->players;
}

// ---- identification ------------------------------------------------------

static std::string strip_app_suffix(std::string t)
{
	static const char *sufs[] = {" - Google Chrome", " - Chromium", " — Mozilla Firefox",
	                             " - Mozilla Firefox", " - Brave"};
	for (const char *s : sufs) {
		size_t n = strlen(s);
		if (t.size() > n && t.compare(t.size() - n, n, s) == 0) return t.substr(0, t.size() - n);
	}
	return t;
}

static std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return tolower(c); });
	return s;
}

Ident identify(const Node &n, const DeskState &ds)
{
	Ident id;
	if (n.pid <= 0 || n.cls.rfind("Stream/", 0) != 0) return id;

	// The closest process in the ancestry that owns visible windows wins.
	std::vector<const Win *> wins;
	int owner = 0;
	for (int pid : pid_chain(n.pid)) {
		for (const Win &w : ds.wins)
			if (w.pid == pid) wins.push_back(&w);
		if (!wins.empty()) {
			owner = pid;
			break;
		}
	}
	if (wins.empty()) return id;

	if (wins.size() == 1) {
		id.text = strip_app_suffix(wins[0]->title);
		id.confidence = 2;
		return id;
	}

	// Several windows (typical for a browser): try the media session titles.
	std::vector<const Win *> hit;
	for (const Player &p : ds.players) {
		if (p.pid != owner || p.title.empty()) continue;
		const std::string t = lower(p.title);
		for (const Win *w : wins)
			if (lower(w->title).find(t) != std::string::npos) hit.push_back(w);
	}
	if (!hit.empty()) {
		id.text = strip_app_suffix(hit[0]->title);
		if (hit.size() > 1) id.text += "  (+" + std::to_string(hit.size() - 1) + ")";
		id.confidence = 2;
		return id;
	}

	id.text = std::to_string(wins.size()) + " windows - tab not identified";
	id.confidence = 1;
	return id;
}
