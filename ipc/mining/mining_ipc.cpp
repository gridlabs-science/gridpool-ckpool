/*
 * Copyright (c) 2026 Con Kolivas
 *
 * C++ implementation of the Bitcoin Core mining IPC C interface declared in
 * mining_ipc.h. All Cap'n Proto / kj complexity is confined to this file so
 * the rest of ckpool stays pure C.
 *
 * Transport: the Bitcoin Core IPC socket speaks Cap'n Proto RPC using the
 * libmultiprocess conventions. We talk to it directly with capnp's EzRpcClient
 * and the generated client stubs, performing the libmultiprocess handshake by
 * hand: exchange thread maps via Init.construct(), then supply a
 * Proxy.Context{thread, callbackThread} on every Mining call. This avoids a
 * build dependency on libmultiprocess itself.
 *
 * Threading: an EzRpcClient runs its kj event loop on the thread that
 * constructs it, and every call on it MUST be made from that same thread.
 * A single context is therefore created, connected and used entirely from one
 * caller thread (ckpool's ipcnotify thread). This is sufficient for the Phase 1
 * tip-notification surface; wider cross-thread use (block template generation)
 * will route calls through the event loop in a later phase.
 *
 * Phase 1 scope: connect, getTip, waitTipChanged, and reconnection.
 */

#include "mining_ipc.h"

#include <capnp/ez-rpc.h>
#include <kj/exception.h>

#include "init.capnp.h"
#include "mining.capnp.h"
#include "common.capnp.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>

namespace {

using namespace ipc::capnp::messages;

/* Minimal Thread / ThreadMap server implementation. bitcoind requires a valid
 * thread handle in every Proxy.Context; we serve a ThreadMap so it can create
 * callback thread handles that route back to us. */
class ThreadServer final : public mp::Thread::Server {
public:
	explicit ThreadServer(kj::StringPtr name) : name_(kj::str(name)) {}

	kj::Promise<void> getName(GetNameContext context) override
	{
		context.getResults().setResult(name_);
		return kj::READY_NOW;
	}

private:
	kj::String name_;
};

class ThreadMapServer final : public mp::ThreadMap::Server {
public:
	kj::Promise<void> makeThread(MakeThreadContext context) override
	{
		auto name = context.getParams().getName();
		context.getResults().setResult(mp::Thread::Client(kj::heap<ThreadServer>(name)));
		return kj::READY_NOW;
	}
};

/* Convert a 64 char hex string to 32 bytes. Returns false on malformed input. */
static bool hex_to_bin(const char *hex, uint8_t *out, size_t bytes)
{
	for (size_t i = 0; i < bytes; ++i) {
		char c[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
		if (!isxdigit((unsigned char)c[0]) || !isxdigit((unsigned char)c[1]))
			return false;
		out[i] = (uint8_t)strtol(c, nullptr, 16);
	}
	return true;
}

/* Fill a mining_tip from a BlockRef reader. Returns 0 on success. */
static int fill_tip(BlockRef::Reader ref, mining_tip *out)
{
	auto hash = ref.getHash();

	if (hash.size() != 32)
		return -1;
	static const char *hexd = "0123456789abcdef";
	for (size_t i = 0; i < 32; ++i) {
		uint8_t b = hash[i];
		out->hash[i * 2]     = hexd[b >> 4];
		out->hash[i * 2 + 1] = hexd[b & 0x0f];
	}
	out->hash[64] = '\0';
	out->height = ref.getHeight();
	return 0;
}

class MiningIpc {
public:
	explicit MiningIpc(std::string socket_path) : socket_path_(std::move(socket_path)) {}

	~MiningIpc() { teardown(); }

	/* Establish the transport and handshake, then attempt to obtain a Mining
	 * capability. Returns false if the socket is absent or the transport
	 * could not be built; a node that is up but not yet ready to mine still
	 * returns true (obtain is retried later). */
	bool connect()
	{
		if (connected_.load())
			return true;

		struct stat st;
		if (stat(socket_path_.c_str(), &st) != 0)
			return false;

		try {
			client_ = std::make_unique<capnp::EzRpcClient>("unix:" + socket_path_);
			auto &ws = client_->getWaitScope();
			auto init = client_->getMain<Init>();

			/* Our ThreadMap and the callback thread bitcoind delivers
			 * long-lived results (e.g. waitTipChanged) on. */
			our_threadmap_ = mp::ThreadMap::Client(kj::heap<ThreadMapServer>());
			{
				auto mk = our_threadmap_.makeThreadRequest();
				mk.setName("ckpool-callback");
				callback_thread_ = mk.send().wait(ws).getResult();
			}

			/* Exchange thread maps, then create the server-side execution
			 * thread that every Mining call runs on. */
			{
				auto req = init.constructRequest();
				req.setThreadMap(our_threadmap_);
				server_threadmap_ = req.send().wait(ws).getThreadMap();
			}
			{
				auto mk = server_threadmap_.makeThreadRequest();
				mk.setName("bitcoin-execution");
				exec_thread_ = mk.send().wait(ws).getResult();
			}

			have_threads_ = true;
			connected_.store(true);
		} catch (const kj::Exception &e) {
			client_.reset();
			return false;
		}

		/* Obtaining Mining may legitimately fail while the node is still
		 * starting up; that is not a transport failure. */
		obtain_mining();
		return true;
	}

	/* Reconnect if needed, then ensure a usable Mining client. Returns true
	 * once mining is available. */
	bool try_obtain_mining()
	{
		if (!connected_.load()) {
			teardown();
			if (!connect())
				return false;
		}
		return have_mining_ || obtain_mining();
	}

	int get_tip(mining_tip *out)
	{
		if (!connected_.load() || !have_mining_ || !out)
			return -1;
		try {
			auto &ws = client_->getWaitScope();
			auto req = mining_.getTipRequest();
			fill_context(req.initContext());
			auto resp = req.send().wait(ws);
			if (!resp.getHasResult())
				return -1;
			return fill_tip(resp.getResult(), out);
		} catch (const kj::Exception &e) {
			note_disconnect(e);
			return -1;
		}
	}

	int wait_tip_changed(const char *current_tip_hex, double timeout_seconds, mining_tip *out)
	{
		if (!connected_.load() || !have_mining_ || !current_tip_hex || !out)
			return -1;
		try {
			uint8_t tip[32];
			if (!hex_to_bin(current_tip_hex, tip, 32))
				return -1;

			auto &ws = client_->getWaitScope();
			auto req = mining_.waitTipChangedRequest();
			fill_context(req.initContext());
			req.setCurrentTip(kj::arrayPtr(reinterpret_cast<const capnp::byte *>(tip), 32));
			req.setTimeout(timeout_seconds > 0 ? timeout_seconds : 1e9);

			auto resp = req.send().wait(ws);
			return fill_tip(resp.getResult(), out);
		} catch (const kj::Exception &e) {
			const char *desc = e.getDescription().cStr();
			/* A timeout is a normal, non-fatal outcome. */
			if (strstr(desc, "timeout") || strstr(desc, "Timeout"))
				return -1;
			note_disconnect(e);
			return -1;
		}
	}

private:
	/* Every Mining call carries a Proxy.Context routing it to the server
	 * execution thread with results delivered on our callback thread. */
	template <typename CtxBuilder>
	void fill_context(CtxBuilder ctx)
	{
		ctx.setThread(exec_thread_);
		ctx.setCallbackThread(callback_thread_);
	}

	/* Flag the connection as lost on a disconnect so the next
	 * try_obtain_mining() rebuilds it. */
	void note_disconnect(const kj::Exception &e)
	{
		if (e.getType() == kj::Exception::Type::DISCONNECTED)
			connected_.store(false);
	}

	/* Obtain and validate a Mining capability on the current transport. */
	bool obtain_mining()
	{
		if (!client_ || !have_threads_)
			return false;
		try {
			auto &ws = client_->getWaitScope();
			auto init = client_->getMain<Init>();

			auto req = init.makeMiningRequest();
			fill_context(req.initContext());
			mining_ = req.send().wait(ws).getResult();

			/* A null Mining capability only errors on a real call, so
			 * probe it before declaring success. */
			auto probe = mining_.isInitialBlockDownloadRequest();
			fill_context(probe.initContext());
			probe.send().wait(ws);

			have_mining_ = true;
			return true;
		} catch (const kj::Exception &e) {
			note_disconnect(e);
			have_mining_ = false;
			return false;
		}
	}

	/* Reset all per-connection state so a subsequent connect() rebuilds it
	 * from scratch. The capability handles below reference the dead client
	 * and become harmless broken capabilities until overwritten. */
	void teardown()
	{
		connected_.store(false);
		have_mining_ = false;
		have_threads_ = false;
		client_.reset();
	}

	std::string socket_path_;
	std::atomic<bool> connected_{false};
	bool have_threads_ = false;
	bool have_mining_ = false;

	std::unique_ptr<capnp::EzRpcClient> client_;
	Mining::Client mining_{nullptr};

	mp::ThreadMap::Client our_threadmap_{nullptr};
	mp::ThreadMap::Client server_threadmap_{nullptr};
	mp::Thread::Client callback_thread_{nullptr};
	mp::Thread::Client exec_thread_{nullptr};
};

} // namespace

/* --------------------------------------------------------------------------
 * C API. No exception may cross this boundary into ckpool's C code.
 * -------------------------------------------------------------------------- */

extern "C" {

mining_ipc_ctx *mining_ipc_connect(const char *socket_path)
{
	try {
		auto *impl = new MiningIpc(socket_path ? socket_path : "");
		if (!impl->connect()) {
			delete impl;
			return nullptr;
		}
		return reinterpret_cast<mining_ipc_ctx *>(impl);
	} catch (...) {
		return nullptr;
	}
}

void mining_ipc_disconnect(mining_ipc_ctx *ctx)
{
	delete reinterpret_cast<MiningIpc *>(ctx);
}

int mining_ipc_try_obtain_mining(mining_ipc_ctx *ctx)
{
	if (!ctx)
		return -1;
	try {
		return reinterpret_cast<MiningIpc *>(ctx)->try_obtain_mining() ? 0 : -1;
	} catch (...) {
		return -1;
	}
}

int mining_ipc_get_tip(mining_ipc_ctx *ctx, mining_tip *out_tip)
{
	if (!ctx || !out_tip)
		return -1;
	try {
		return reinterpret_cast<MiningIpc *>(ctx)->get_tip(out_tip);
	} catch (...) {
		return -1;
	}
}

int mining_ipc_wait_tip_changed(mining_ipc_ctx *ctx, const char *current_tip_hex,
				double timeout_seconds, mining_tip *out_new_tip)
{
	if (!ctx || !current_tip_hex || !out_new_tip)
		return -1;
	try {
		return reinterpret_cast<MiningIpc *>(ctx)->wait_tip_changed(current_tip_hex,
									    timeout_seconds, out_new_tip);
	} catch (...) {
		return -1;
	}
}

} // extern "C"
