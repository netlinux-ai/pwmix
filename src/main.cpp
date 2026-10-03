#include "desk.h"
#include "pw.h"
#include "ui.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static void render_png(PwClient &pw, Display *dpy, Mpris &mp, Ui &ui, const char *path, int w,
                       int h)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	DeskState ds;
	desk_scan_x11(dpy, ds);
	ds.players = mp.get();
	Graph g = pw.snapshot();
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(s);
	ui.draw(cr, w, h, g, ds);
	cairo_surface_write_to_png(s, path);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	fprintf(stderr, "wrote %s (%d nodes, %d links, %zu windows, %zu players)\n", path,
	        (int)g.nodes.size(), (int)g.links.size(), ds.wins.size(), ds.players.size());
}

int main(int argc, char **argv)
{
	const char *png = nullptr;
	int W = 1280, H = 820;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
			W = atoi(argv[++i]);
			H = atoi(argv[++i]);
		}
	}

	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy) {
		fprintf(stderr, "cannot open X display\n");
		return 1;
	}

	PwClient pw;
	if (!pw.start()) return 1;
	Mpris mpris;
	mpris.start();
	Ui ui;

	if (png) {
		render_png(pw, dpy, mpris, ui, png, W, H);
		mpris.stop();
		pw.stop();
		XCloseDisplay(dpy);
		return 0;
	}

	const int scr = DefaultScreen(dpy);
	Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, 0,
	                                 BlackPixel(dpy, scr));
	XStoreName(dpy, win, "pwconsole");
	XClassHint ch = {(char *)"pwconsole", (char *)"Pwconsole"};
	XSetClassHint(dpy, win, &ch);
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask);
	Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	XMapWindow(dpy, win);

	cairo_surface_t *xs = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, scr), W, H);
	cairo_surface_t *back = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);

	DeskState ds;
	Graph g = pw.snapshot();
	uint64_t seen_gen = pw.generation();
	desk_scan_x11(dpy, ds);
	ds.players = mpris.get();

	bool dirty = true, quit = false;
	auto last_desk = Clock::now();

	while (!quit) {
		struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};
		poll(&pfd, 1, 60);

		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			switch (ev.type) {
			case Expose:
				dirty = true;
				break;
			case ConfigureNotify:
				if (ev.xconfigure.width != W || ev.xconfigure.height != H) {
					W = ev.xconfigure.width;
					H = ev.xconfigure.height;
					cairo_xlib_surface_set_size(xs, W, H);
					cairo_surface_destroy(back);
					back = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
					dirty = true;
				}
				break;
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == wm_delete) quit = true;
				break;
			case KeyPress: {
				KeySym k = XLookupKeysym(&ev.xkey, 0);
				if (k == XK_q || k == XK_Escape) quit = true;
				else if (k == XK_Down) ui.scrollBy(60);
				else if (k == XK_Up) ui.scrollBy(-60);
				else if (k == XK_Next) ui.scrollBy(H - 120);
				else if (k == XK_Prior) ui.scrollBy(-(H - 120));
				else if (k == XK_Home) ui.scrollTo(0);
				dirty = true;
				break;
			}
			case ButtonPress: {
				const int b = ev.xbutton.button;
				Hit h;
				const bool hit = ui.hitTest(ev.xbutton.x, ev.xbutton.y, h);
				auto node = [&](uint32_t id) -> const Node * {
					auto it = g.nodes.find(id);
					return it == g.nodes.end() ? nullptr : &it->second;
				};
				if (b == 1 && hit && h.kind == Hit::Mute) {
					if (const Node *n = node(h.node)) pw.setMute(h.node, !n->mute);
				} else if ((b == 4 || b == 5) && hit) {
					if (const Node *n = node(h.node)) {
						if (n->have_vol) {
							double c = std::cbrt(std::max(0.0f, n->vol));
							c += (b == 4 ? 0.02 : -0.02);
							c = std::clamp(c, 0.0, 1.5);
							pw.setVolume(h.node, (float)(c * c * c));
						}
					}
				} else if (b == 4) {
					ui.scrollBy(-60);
				} else if (b == 5) {
					ui.scrollBy(60);
				}
				dirty = true;
				break;
			}
			}
		}

		const uint64_t gen = pw.generation();
		if (gen != seen_gen) {
			seen_gen = gen;
			g = pw.snapshot();
			dirty = true;
		}
		if (Clock::now() - last_desk > std::chrono::seconds(1)) {
			last_desk = Clock::now();
			desk_scan_x11(dpy, ds);
			ds.players = mpris.get();
			dirty = true;
		}

		if (dirty) {
			dirty = false;
			cairo_t *cr = cairo_create(back);
			ui.draw(cr, W, H, g, ds);
			cairo_destroy(cr);
			cairo_t *xc = cairo_create(xs);
			cairo_set_source_surface(xc, back, 0, 0);
			cairo_paint(xc);
			cairo_destroy(xc);
			cairo_surface_flush(xs);
			XFlush(dpy);
		}
	}

	mpris.stop();
	pw.stop();
	cairo_surface_destroy(back);
	cairo_surface_destroy(xs);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
