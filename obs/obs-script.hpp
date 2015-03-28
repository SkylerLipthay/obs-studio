#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <string>
#include <thread>
#include <duktape/duktape.h>
#include <util/threading.h>

#include "obs.hpp"

class OBSScript {
	struct Event {
		enum { COLLECT_GARBAGE, STOP, TIMER_CALLBACK } type;
		std::thread::id threadID;
	};

	class EventQueue {
		std::queue<Event>       queue;
		std::mutex              mutex;
		std::condition_variable condition;

	public:
		void Clear();
		bool Empty();
		void Push(Event const& event);
		Event Pop();
	};

	duk_context                            *ctx;
	std::string                            environmentScript;
	std::thread                            eventLoop;
	EventQueue                             eventQueue;
	std::timed_mutex                       garbageLock;
	std::thread                            garbageLoop;
	std::string                            script;
	std::map<std::thread::id, std::thread> timers;
	std::condition_variable                timersHalt;
	std::mutex                             timersMutex;

	void CollectGarbage();
	void DestroyContext();
	void DestroyTimers();
	void EnqueueTimerCallback(std::thread::id threadID);
	void EventLoop();
	void ExecuteTimerCallback(std::thread::id threadID);
	void GarbageLoop();
	bool HandleEvent(Event const& event);
	void InitContext();
	void LoadEnvironmentScript();
	void LogError();
	void RegisterFunction(const char *name, duk_c_function func,
			duk_int_t args, bool internal = false);

	static void *DelayThread(void *data);
	static OBSScript *GetInstance(duk_context *ctx);
	static std::string GetPortableThreadID(std::thread::id threadID);

	static duk_ret_t SceneFind(duk_context *ctx);
	static duk_ret_t SceneRelease(duk_context *ctx);
	static duk_ret_t SceneSelect(duk_context *ctx);
	static duk_ret_t SceneItemFind(duk_context *ctx);
	static duk_ret_t SceneItemRelease(duk_context *ctx);
	static duk_ret_t SceneItemSetVisible(duk_context *ctx);
	static duk_ret_t SetTimer(duk_context *ctx);

public:
	OBSScript();
	virtual ~OBSScript();

	inline const std::string& GetText() const { return script; }
	void Load(const std::string& text);
	void Stop();
};
