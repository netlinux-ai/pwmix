#pragma once
#include "desk.h"
#include "model.h"

#include <cairo/cairo.h>
#include <vector>

struct Hit {
	enum Kind { Strip, Mute } kind;
	uint32_t node;
	double x, y, w, h;
};

class Ui {
public:
	Ui();
	~Ui();

	void draw(cairo_t *cr, int w, int h, const Graph &g, const DeskState &ds);

	// Hit-test using the geometry of the last draw().
	bool hitTest(double x, double y, Hit &out) const;

	void scrollBy(double dy) { scroll_ += dy; }
	void scrollTo(double y) { scroll_ = y; }

private:
	struct Impl;
	Impl *d;
	double scroll_ = 0;
};
