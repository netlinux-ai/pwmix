#include "pw.h"

#include <pipewire/pipewire.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

struct PwClient::Impl {
	struct NodeProxy {
		Impl *d;
		uint32_t id;
		pw_node *node;
		spa_hook listener;
	};

	pw_thread_loop *loop = nullptr;
	pw_context *ctx = nullptr;
	pw_core *core = nullptr;
	spa_hook core_listener{};
	pw_registry *reg = nullptr;
	spa_hook reg_listener{};

	std::mutex mu;
	Graph g;
	std::atomic<uint64_t> gen{0};
	std::map<uint32_t, NodeProxy *> proxies;
};

static const char *get(const spa_dict *p, const char *k)
{
	const char *v = p ? spa_dict_lookup(p, k) : nullptr;
	return v ? v : "";
}

static void fill_node(Node &n, const spa_dict *p)
{
	n.cls = get(p, PW_KEY_MEDIA_CLASS);
	n.name = get(p, PW_KEY_NODE_NAME);
	n.desc = get(p, PW_KEY_NODE_DESCRIPTION);
	n.nick = get(p, PW_KEY_NODE_NICK);
	n.app = get(p, PW_KEY_APP_NAME);
	n.binary = get(p, PW_KEY_APP_PROCESS_BINARY);
	n.icon = get(p, PW_KEY_APP_ICON_NAME);
	n.media_name = get(p, PW_KEY_MEDIA_NAME);
	n.api = get(p, PW_KEY_CLIENT_API);
	n.pid = atoi(get(p, PW_KEY_APP_PROCESS_ID));
	n.has_device = *get(p, PW_KEY_DEVICE_ID) != 0;
}

// ---- node events -----------------------------------------------------

static void on_node_info(void *data, const pw_node_info *info)
{
	auto *p = static_cast<PwClient::Impl::NodeProxy *>(data);
	PwClient::Impl *d = p->d;
	std::lock_guard<std::mutex> lk(d->mu);
	auto it = d->g.nodes.find(p->id);
	if (it == d->g.nodes.end()) return;
	it->second.state = pw_node_state_as_string(info->state);
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && info->props) {
		Node &n = it->second;
		const bool dev = n.has_device;
		fill_node(n, info->props);
		n.has_device = n.has_device || dev;
	}
	d->gen++;
}

static void on_node_param(void *data, int, uint32_t id, uint32_t, uint32_t,
                          const spa_pod *param)
{
	if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param)) return;
	auto *p = static_cast<PwClient::Impl::NodeProxy *>(data);
	PwClient::Impl *d = p->d;

	bool have_mute = false, mute = false, have_vol = false;
	float vol = 1.0f;
	float ch[64];
	uint32_t nch = 0;

	const spa_pod_prop *prop;
	SPA_POD_OBJECT_FOREACH((const spa_pod_object *)param, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
			have_mute = spa_pod_get_bool(&prop->value, &mute) >= 0;
			break;
		case SPA_PROP_volume:
			if (spa_pod_get_float(&prop->value, &vol) >= 0) have_vol = true;
			break;
		case SPA_PROP_channelVolumes:
			nch = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, ch, 64);
			break;
		}
	}

	std::lock_guard<std::mutex> lk(d->mu);
	auto it = d->g.nodes.find(p->id);
	if (it == d->g.nodes.end()) return;
	Node &n = it->second;
	if (have_mute) n.mute = mute;
	if (nch > 0) {
		float m = 0;
		for (uint32_t i = 0; i < nch; i++) m = std::max(m, ch[i]);
		n.vol = m * (have_vol ? vol : 1.0f);
		n.nch = (int)nch;
		n.have_vol = true;
	} else if (have_vol) {
		n.vol = vol;
		n.have_vol = true;
	}
	d->gen++;
}

static const pw_node_events node_events = {
	.version = PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
	.param = on_node_param,
};

// ---- registry events -------------------------------------------------

static void on_global(void *data, uint32_t id, uint32_t, const char *type,
                      uint32_t, const spa_dict *props)
{
	auto *d = static_cast<PwClient::Impl *>(data);
	if (!props) return;

	if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
		Node n;
		n.id = id;
		fill_node(n, props);
		const bool audio = n.cls.rfind("Audio/", 0) == 0 || n.cls.rfind("Stream/", 0) == 0;
		{
			std::lock_guard<std::mutex> lk(d->mu);
			d->g.nodes[id] = n;
			d->gen++;
		}
		if (audio) {
			auto *p = new PwClient::Impl::NodeProxy{d, id, nullptr, {}};
			p->node = (pw_node *)pw_registry_bind(d->reg, id, type, PW_VERSION_NODE, 0);
			if (p->node) {
				pw_node_add_listener(p->node, &p->listener, &node_events, p);
				uint32_t ids[] = {SPA_PARAM_Props};
				pw_node_subscribe_params(p->node, ids, 1);
				d->proxies[id] = p;
			} else {
				delete p;
			}
		}
	} else if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
		Port p;
		p.id = id;
		p.node = (uint32_t)atoi(get(props, PW_KEY_NODE_ID));
		p.out = !strcmp(get(props, PW_KEY_PORT_DIRECTION), "out");
		p.name = get(props, PW_KEY_PORT_NAME);
		p.channel = get(props, PW_KEY_AUDIO_CHANNEL);
		p.format = get(props, PW_KEY_FORMAT_DSP);
		std::lock_guard<std::mutex> lk(d->mu);
		d->g.ports[id] = p;
		d->gen++;
	} else if (!strcmp(type, PW_TYPE_INTERFACE_Link)) {
		Link l;
		l.id = id;
		l.onode = (uint32_t)atoi(get(props, PW_KEY_LINK_OUTPUT_NODE));
		l.oport = (uint32_t)atoi(get(props, PW_KEY_LINK_OUTPUT_PORT));
		l.inode = (uint32_t)atoi(get(props, PW_KEY_LINK_INPUT_NODE));
		l.iport = (uint32_t)atoi(get(props, PW_KEY_LINK_INPUT_PORT));
		std::lock_guard<std::mutex> lk(d->mu);
		d->g.links[id] = l;
		d->gen++;
	}
}

static void on_global_remove(void *data, uint32_t id)
{
	auto *d = static_cast<PwClient::Impl *>(data);
	auto pit = d->proxies.find(id);
	if (pit != d->proxies.end()) {
		auto *p = pit->second;
		spa_hook_remove(&p->listener);
		pw_proxy_destroy((pw_proxy *)p->node);
		delete p;
		d->proxies.erase(pit);
	}
	std::lock_guard<std::mutex> lk(d->mu);
	d->g.nodes.erase(id);
	d->g.ports.erase(id);
	d->g.links.erase(id);
	d->gen++;
}

static const pw_registry_events registry_events = {
	.version = PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

static void on_core_info(void *data, const pw_core_info *info)
{
	auto *d = static_cast<PwClient::Impl *>(data);
	std::lock_guard<std::mutex> lk(d->mu);
	d->g.remote = info->name ? info->name : "";
	d->gen++;
}

static void on_core_error(void *, uint32_t id, int seq, int res, const char *msg)
{
	fprintf(stderr, "pw core error id=%u seq=%d res=%d: %s\n", id, seq, res, msg);
}

static const pw_core_events core_events = {
	.version = PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.error = on_core_error,
};

// ---- public API ------------------------------------------------------

PwClient::PwClient() : d(new Impl) {}

PwClient::~PwClient()
{
	stop();
	delete d;
}

bool PwClient::start()
{
	pw_init(nullptr, nullptr);
	d->loop = pw_thread_loop_new("pwmix-pw", nullptr);
	if (!d->loop) return false;
	d->ctx = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
	if (!d->ctx) return false;

	pw_thread_loop_lock(d->loop);
	d->core = pw_context_connect(d->ctx, nullptr, 0);
	if (!d->core) {
		pw_thread_loop_unlock(d->loop);
		fprintf(stderr, "cannot connect to PipeWire\n");
		return false;
	}
	pw_core_add_listener(d->core, &d->core_listener, &core_events, d);
	d->reg = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d->reg, &d->reg_listener, &registry_events, d);
	pw_thread_loop_start(d->loop);
	pw_thread_loop_unlock(d->loop);
	return true;
}

void PwClient::stop()
{
	if (!d->loop) return;
	pw_thread_loop_stop(d->loop);
	for (auto &kv : d->proxies) {
		spa_hook_remove(&kv.second->listener);
		pw_proxy_destroy((pw_proxy *)kv.second->node);
		delete kv.second;
	}
	d->proxies.clear();
	if (d->reg) pw_proxy_destroy((pw_proxy *)d->reg);
	if (d->core) pw_core_disconnect(d->core);
	if (d->ctx) pw_context_destroy(d->ctx);
	pw_thread_loop_destroy(d->loop);
	d->loop = nullptr;
	d->reg = nullptr;
	d->core = nullptr;
	d->ctx = nullptr;
}

Graph PwClient::snapshot()
{
	std::lock_guard<std::mutex> lk(d->mu);
	return d->g;
}

uint64_t PwClient::generation() const { return d->gen.load(); }

void PwClient::setMute(uint32_t id, bool mute)
{
	if (!d->loop) return;
	pw_thread_loop_lock(d->loop);
	auto it = d->proxies.find(id);
	if (it != d->proxies.end()) {
		uint8_t buf[256];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		spa_pod *pod = (spa_pod *)spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(mute));
		pw_node_set_param(it->second->node, SPA_PARAM_Props, 0, pod);
	}
	pw_thread_loop_unlock(d->loop);
}

void PwClient::setVolume(uint32_t id, float linear)
{
	if (!d->loop) return;
	int nch = 2;
	{
		std::lock_guard<std::mutex> lk(d->mu);
		auto n = d->g.nodes.find(id);
		if (n != d->g.nodes.end()) nch = std::clamp(n->second.nch, 1, 64);
	}
	float vols[64];
	for (int i = 0; i < nch; i++) vols[i] = linear;

	pw_thread_loop_lock(d->loop);
	auto it = d->proxies.find(id);
	if (it != d->proxies.end()) {
		uint8_t buf[512];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		spa_pod *pod = (spa_pod *)spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_channelVolumes,
			SPA_POD_Array(sizeof(float), SPA_TYPE_Float, (uint32_t)nch, vols));
		pw_node_set_param(it->second->node, SPA_PARAM_Props, 0, pod);
	}
	pw_thread_loop_unlock(d->loop);
}
