#include "ui.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace {

struct Col {
	double r, g, b;
};

const Col BG = {0.075, 0.09, 0.105};
const Col PANEL = {0.12, 0.145, 0.17};
const Col EDGE = {0.2, 0.24, 0.28};
const Col TXT = {0.9, 0.93, 0.95};
const Col DIM = {0.55, 0.62, 0.66};
const Col ACC = {0.35, 0.85, 0.72};
const Col WARN = {0.95, 0.7, 0.3};
const Col RED = {0.9, 0.3, 0.27};
const Col GREEN = {0.35, 0.8, 0.45};
const Col COLC[3] = {{0.35, 0.78, 0.5}, {0.3, 0.78, 0.75}, {0.45, 0.6, 0.95}};

const double STRIP_H = 82;
const double STRIP_GAP = 10;
const double MARGIN = 18;
const double HEADER = 56;
const double FOOTER = 26;

int column_of(const Node &n)
{
	const std::string &c = n.cls;
	if (c == "Stream/Output/Audio") return 0;
	if (c == "Audio/Source") return n.has_device ? 0 : 1;
	if (c == "Audio/Source/Virtual") return 1;
	if (c == "Audio/Sink") return n.has_device ? 2 : 1;
	if (c == "Stream/Input/Audio") return 2;
	return -1;
}

void set(cairo_t *cr, const Col &c, double a = 1.0)
{
	cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

double cubic(float lin) { return std::cbrt(std::max(0.0f, lin)); }

} // namespace

struct Ui::Impl {
	PangoFontDescription *f_title, *f_sub, *f_small, *f_head;
	std::vector<Hit> hits;
};

Ui::Ui() : d(new Impl)
{
	d->f_title = pango_font_description_from_string("Sans Bold 11");
	d->f_sub = pango_font_description_from_string("Sans 9");
	d->f_small = pango_font_description_from_string("Sans 8.5");
	d->f_head = pango_font_description_from_string("Sans Bold 10");
}

Ui::~Ui()
{
	pango_font_description_free(d->f_title);
	pango_font_description_free(d->f_sub);
	pango_font_description_free(d->f_small);
	pango_font_description_free(d->f_head);
	delete d;
}

static void text(cairo_t *cr, PangoFontDescription *fd, const std::string &s, double x,
                 double y, double maxw, const Col &c, double a = 1.0, int align = 0)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(l, fd);
	pango_layout_set_text(l, s.c_str(), -1);
	pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	if (align == 1) pango_layout_set_alignment(l, PANGO_ALIGN_RIGHT);
	if (align == 2) pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	cairo_move_to(cr, x, y);
	set(cr, c, a);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

bool Ui::hitTest(double x, double y, Hit &out) const
{
	// Mute buttons sit on top of strips, so test them first.
	for (const Hit &h : d->hits)
		if (h.kind == Hit::Mute && x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
			out = h;
			return true;
		}
	for (const Hit &h : d->hits)
		if (h.kind == Hit::Strip && x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
			out = h;
			return true;
		}
	return false;
}

void Ui::draw(cairo_t *cr, int W, int H, const Graph &g, const DeskState &ds)
{
	d->hits.clear();
	set(cr, BG);
	cairo_paint(cr);

	const double gap = std::clamp(W * 0.11, 70.0, 220.0);
	const double cw = (W - 2 * MARGIN - 2 * gap) / 3.0;
	const double colx[3] = {MARGIN, MARGIN + cw + gap, MARGIN + 2 * (cw + gap)};

	// ---- group nodes into columns -----------------------------------
	std::vector<const Node *> cols[3];
	for (const auto &kv : g.nodes) {
		int c = column_of(kv.second);
		if (c >= 0) cols[c].push_back(&kv.second);
	}
	for (auto &v : cols)
		std::sort(v.begin(), v.end(), [](const Node *a, const Node *b) {
			const bool ra = a->state == "running", rb = b->state == "running";
			if (ra != rb) return ra;
			return a->label() < b->label();
		});

	const double view_top = HEADER, view_bot = H - FOOTER;
	size_t most = 0;
	for (auto &v : cols) most = std::max(most, v.size());
	const double content_h = most * (STRIP_H + STRIP_GAP);
	const double max_scroll = std::max(0.0, content_h - (view_bot - view_top));
	scroll_ = std::clamp(scroll_, 0.0, max_scroll);

	std::map<uint32_t, std::pair<double, double>> pos; // node -> (y centre, column)
	for (int c = 0; c < 3; c++)
		for (size_t i = 0; i < cols[c].size(); i++) {
			const double y = view_top + i * (STRIP_H + STRIP_GAP) - scroll_;
			pos[cols[c][i]->id] = {y, (double)c};
		}

	// ---- links: follow edges through hidden helper nodes ---------------
	std::multimap<uint32_t, uint32_t> edges;
	for (const auto &kv : g.links) edges.insert({kv.second.onode, kv.second.inode});

	struct Edge {
		uint32_t a, b;
		bool via;
	};
	std::vector<Edge> draw_edges;
	std::set<std::pair<uint32_t, uint32_t>> seen_pair;
	for (const auto &p : pos) {
		std::set<uint32_t> visited{p.first};
		std::vector<std::pair<uint32_t, bool>> stack;
		auto range = edges.equal_range(p.first);
		for (auto it = range.first; it != range.second; ++it) stack.push_back({it->second, false});
		while (!stack.empty()) {
			auto [n, via] = stack.back();
			stack.pop_back();
			if (!visited.insert(n).second) continue;
			if (pos.count(n)) {
				if (seen_pair.insert({p.first, n}).second) draw_edges.push_back({p.first, n, via});
				continue; // stop at the first visible node
			}
			auto r = edges.equal_range(n);
			for (auto it = r.first; it != r.second; ++it) stack.push_back({it->second, true});
		}
	}

	cairo_save(cr);
	cairo_rectangle(cr, 0, view_top - 6, W, view_bot - view_top + 12);
	cairo_clip(cr);

	double lane_base = view_top - scroll_ + cols[1].size() * (STRIP_H + STRIP_GAP) + 4;
	int skip_lane = 0;
	for (const Edge &e : draw_edges) {
		const auto &pa = pos[e.a], &pb = pos[e.b];
		if (pa.second >= pb.second && e.a != e.b) {
			if (pa.second > pb.second) continue; // backwards links not drawn in this prototype
		}
		const int ca = (int)pa.second, cb = (int)pb.second;
		const double x1 = colx[ca] + cw, y1 = pa.first + STRIP_H / 2;
		const double x2 = colx[cb], y2 = pb.first + STRIP_H / 2;
		if (cb == ca) continue;
		const bool live = g.nodes.at(e.a).state == "running";
		set(cr, live ? ACC : DIM, live ? 0.85 : 0.45);
		cairo_set_line_width(cr, live ? 2.2 : 1.6);
		if (e.via) {
			const double dash[] = {6, 4};
			cairo_set_dash(cr, dash, 2, 0);
		} else {
			cairo_set_dash(cr, nullptr, 0, 0);
		}
		if (cb - ca == 2) {
			// Skips the middle column: run it in a lane underneath that column.
			const double lane = lane_base + 8.0 * (skip_lane++);
			const double g1 = colx[1] - gap * 0.35 - 2.0 * skip_lane;
			const double g2 = colx[2] - gap * 0.65 + 2.0 * skip_lane;
			cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
			cairo_move_to(cr, x1, y1);
			cairo_line_to(cr, g1, y1);
			cairo_line_to(cr, g1, lane);
			cairo_line_to(cr, g2, lane);
			cairo_line_to(cr, g2, y2);
			cairo_line_to(cr, x2, y2);
			cairo_stroke(cr);
		} else {
			const double dx = std::max(40.0, (x2 - x1) * 0.5);
			cairo_move_to(cr, x1, y1);
			cairo_curve_to(cr, x1 + dx, y1, x2 - dx, y2, x2, y2);
			cairo_stroke(cr);
		}
		cairo_set_dash(cr, nullptr, 0, 0);
		cairo_arc(cr, x1, y1, 3.2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, x2, y2, 3.2, 0, 2 * M_PI);
		cairo_fill(cr);
	}

	// ---- strips -----------------------------------------------------------
	for (int c = 0; c < 3; c++) {
		for (const Node *n : cols[c]) {
			const double x = colx[c], y = pos[n->id].first;
			if (y + STRIP_H < view_top - 6 || y > view_bot + 6) continue;

			const bool running = n->state == "running";
			rrect(cr, x, y, cw, STRIP_H, 7);
			set(cr, PANEL);
			cairo_fill_preserve(cr);
			set(cr, running ? COLC[c] : EDGE, running ? 0.8 : 1.0);
			cairo_set_line_width(cr, 1.3);
			cairo_stroke(cr);

			// class colour bar
			rrect(cr, x, y, 5, STRIP_H, 2.5);
			set(cr, COLC[c], running ? 1.0 : 0.45);
			cairo_fill(cr);

			const bool stream = n->cls.rfind("Stream/", 0) == 0;
			const double tx = x + 16, tw = cw - 16 - 54;

			// title + state dot
			cairo_arc(cr, x + cw - 14, y + 15, 4.5, 0, 2 * M_PI);
			set(cr, running ? GREEN : DIM, running ? 1.0 : 0.6);
			cairo_fill(cr);
			text(cr, d->f_title, n->label(), tx, y + 7, tw, TXT);

			// sub line
			std::string sub;
			if (stream) {
				sub = n->media_name.empty() ? n->name : n->media_name;
				sub += "   #" + std::to_string(n->id);
				if (n->pid) sub += "   pid " + std::to_string(n->pid);
			} else {
				sub = n->name;
			}
			text(cr, d->f_small, sub, tx, y + 28, cw - 24, DIM);

			// window identification (the point of this app)
			const Ident id = identify(*n, ds);
			if (!id.text.empty()) {
				const Col &cc = id.confidence >= 2 ? ACC : WARN;
				text(cr, d->f_sub, (id.confidence >= 2 ? "▣ " : "? ") + id.text, tx,
				     y + 44, cw - 24, cc);
			}

			// volume bar
			const double by = y + STRIP_H - 17, bx = tx, bw = cw - 16 - 78, bh = 8;
			rrect(cr, bx, by, bw, bh, 4);
			set(cr, BG);
			cairo_fill(cr);
			if (n->have_vol) {
				const double v = cubic(n->vol);
				const double frac = std::min(v, 1.5) / 1.5;
				rrect(cr, bx, by, std::max(8.0, bw * frac), bh, 4);
				set(cr, n->mute ? DIM : (v > 1.0 ? WARN : COLC[c]), n->mute ? 0.5 : 0.95);
				cairo_fill(cr);
				// 100% tick
				set(cr, TXT, 0.5);
				cairo_rectangle(cr, bx + bw / 1.5, by - 2, 1, bh + 4);
				cairo_fill(cr);
				char buf[16];
				snprintf(buf, sizeof(buf), "%d%%", (int)std::lround(v * 100));
				text(cr, d->f_small, buf, bx + bw + 8, by - 2, 38, n->mute ? DIM : TXT);
			} else {
				text(cr, d->f_small, "no volume", bx + bw + 8, by - 2, 66, DIM, 0.7);
			}

			// mute button
			const double mx = x + cw - 32, my = by - 5, mw = 22, mh = 18;
			rrect(cr, mx, my, mw, mh, 4);
			set(cr, n->mute ? RED : EDGE, n->mute ? 0.9 : 1.0);
			cairo_fill(cr);
			text(cr, d->f_small, "M", mx, my + 1, mw, n->mute ? TXT : DIM, 1.0, 2);
			d->hits.push_back({Hit::Mute, n->id, mx, my, mw, mh});
			d->hits.push_back({Hit::Strip, n->id, x, y, cw, STRIP_H});
		}
	}
	cairo_restore(cr);

	// ---- headers / footer -----------------------------------------------
	static const char *heads[3] = {"SOURCES", "MIDDLE", "SINKS"};
	static const char *subs[3] = {"apps playing · inputs", "routing · virtual",
	                              "outputs · recorders"};
	for (int c = 0; c < 3; c++) {
		set(cr, BG);
		cairo_rectangle(cr, colx[c] - 2, 0, cw + 4, view_top - 6);
		cairo_fill(cr);
		text(cr, d->f_head, heads[c], colx[c], 12, cw, COLC[c]);
		text(cr, d->f_small, subs[c], colx[c], 31, cw, DIM);
		set(cr, COLC[c], 0.7);
		cairo_rectangle(cr, colx[c], view_top - 12, cw, 2);
		cairo_fill(cr);
	}

	set(cr, BG);
	cairo_rectangle(cr, 0, view_bot + 6, W, FOOTER);
	cairo_fill(cr);
	std::string foot = "PipeWire " + (g.remote.empty() ? "?" : g.remote) + "   ·   " +
	                   std::to_string(g.nodes.size()) + " nodes  " +
	                   std::to_string(g.links.size()) + " links  " +
	                   std::to_string(ds.wins.size()) + " windows  " +
	                   std::to_string(ds.players.size()) + " media players" +
	                   "      wheel: volume   click M: mute   ↑↓ PgUp PgDn: scroll   q: quit";
	text(cr, d->f_small, foot, MARGIN, H - FOOTER + 8, W - 2 * MARGIN, DIM);
}
