#include <fstream>
#include <sstream>
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "obs-script.hpp"
#include "platform.hpp"

void OBSScript::EventQueue::Clear()
{
	std::unique_lock<std::mutex> lock(mutex);
	std::queue<Event>().swap(queue);
}

bool OBSScript::EventQueue::Empty()
{
	std::unique_lock<std::mutex> lock(mutex);
        return queue.empty();
}

OBSScript::Event OBSScript::EventQueue::Pop()
{
	std::unique_lock<std::mutex> lock(mutex);
	while (queue.empty())
		condition.wait(lock);
	Event event = queue.front();
	queue.pop();
	return event;
}

void OBSScript::EventQueue::Push(Event const& event)
{
	std::unique_lock<std::mutex> lock(mutex);
        queue.push(event);
        lock.unlock();
        condition.notify_one();
}

OBSScript::OBSScript() : ctx(nullptr)
{
	LoadEnvironmentScript();
}

OBSScript::~OBSScript()
{
	Stop();
}

void OBSScript::CollectGarbage()
{
	duk_gc(ctx, 0);
	duk_gc(ctx, 0);
}

void OBSScript::DestroyContext()
{
	if (ctx == nullptr)
		return;

	Event event;
	event.type = Event::STOP;
	eventQueue.Push(event);
	eventLoop.join();
	eventQueue.Clear();

	garbageLock.unlock();
	garbageLoop.join();

	duk_destroy_heap(ctx);
	ctx = nullptr;
}

void OBSScript::DestroyTimers()
{
	timersHalt.notify_all();

	for (auto& kv : timers)
		kv.second.join();
	timers.clear();
}

bool OBSScript::HandleEvent(Event const& event)
{
	switch (event.type) {
	case Event::COLLECT_GARBAGE:
		CollectGarbage();
		break;

	case Event::STOP:
		DestroyTimers();
		return false;

	case Event::TIMER_CALLBACK:
		ExecuteTimerCallback(event.threadID);
		break;
	}

	return true;
}

void OBSScript::EnqueueTimerCallback(std::thread::id threadID)
{
	Event event;
	event.type = Event::TIMER_CALLBACK;
	event.threadID = threadID;
	eventQueue.Push(event);
}

void OBSScript::EventLoop()
{
	for (;;) {
		Event event = eventQueue.Pop();
		if (!HandleEvent(event))
			break;
	}
}

void OBSScript::ExecuteTimerCallback(std::thread::id threadID)
{
	const char *portableThreadID = GetPortableThreadID(threadID).c_str();

	// $stash.timers[threadID].call(originalThis);
	duk_push_global_stash(ctx);
	duk_get_prop_string(ctx, -1, "timers");
	duk_get_prop_string(ctx, -1, portableThreadID);
	duk_get_prop_index(ctx, -1, 1);
	duk_get_prop_index(ctx, -1, 0);
	if (duk_pcall_method(ctx, 0) != 0)
		LogError();
	duk_pop_2(ctx);

	// delete $stash.timers[threadID];
	duk_del_prop_string(ctx, -1, portableThreadID);
	duk_pop_2(ctx);

	OBSScript *script = GetInstance(ctx);
	auto iter = script->timers.find(threadID);
	if (iter != script->timers.end()) {
		iter->second.detach();
		script->timers.erase(iter);
	}
}

void OBSScript::GarbageLoop()
{
	for (;;) {
		if (garbageLock.try_lock_for(std::chrono::seconds(10))) {
			garbageLock.unlock();
			break;
		}
		Event event;
		event.type = Event::COLLECT_GARBAGE;
		eventQueue.Push(event);
	}
}

void OBSScript::InitContext()
{
	if (ctx != nullptr)
		return;

	ctx = duk_create_heap(nullptr, nullptr, nullptr, this, nullptr);
}

void OBSScript::Load(const std::string& text)
{
	script = text;

	DestroyContext();

	if (script.empty())
		return;

	InitContext();

	// OBS = { internal: {} };
	duk_push_global_object(ctx);
	duk_push_object(ctx);
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, "internal");
	duk_put_prop_string(ctx, -2, "OBS");
	duk_pop(ctx);

	RegisterFunction("sceneFind", SceneFind, 1, true);
	RegisterFunction("sceneRelease", SceneRelease, 1, true);
	RegisterFunction("sceneSelect", SceneSelect, 1, true);

	RegisterFunction("sceneItemFind", SceneItemFind, 2, true);
	RegisterFunction("sceneItemRelease", SceneItemRelease, 1, true);
	RegisterFunction("sceneItemSetVisible", SceneItemSetVisible, 2, true);

	RegisterFunction("setTimer", SetTimer, 2);

	// $stash.timers = {};
	duk_push_global_stash(ctx);
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, "timers");
	duk_pop(ctx);

	if (duk_peval_string(ctx, environmentScript.c_str()) != 0) {
		LogError();
		throw "Error found in obs-script.js";
	}

	if (duk_peval_string(ctx, script.c_str()) != 0)
		LogError();

	eventLoop = std::thread(&OBSScript::EventLoop, this);

	garbageLock.lock();
	garbageLoop = std::thread(&OBSScript::GarbageLoop, this);
}

void OBSScript::LoadEnvironmentScript()
{
	std::string path;
	if (!GetDataFilePath("obs-script.js", path))
		throw "Could not find obs-script.js path";
	std::ifstream file(path, std::ios::in | std::ios::binary);
	std::ostringstream contents;
	contents << file.rdbuf();
	file.close();
	environmentScript = std::move(contents.str());
}

void OBSScript::LogError()
{
	// At this point, the most recent Duktape JS error is at the top of the
	// stack.

	// error.message
	duk_get_prop_string(ctx, -1, "message");
	const char *message = duk_safe_to_string(ctx, -1);
	duk_pop(ctx);

	// error.lineNumber
	duk_get_prop_string(ctx, -1, "lineNumber");
	const bool hasLine = !duk_is_null_or_undefined(ctx, -1);

	if (hasLine) {
		const char *number = duk_safe_to_string(ctx, -1);
		blog(LOG_ERROR, "Script error: %s (line %s)", message, number);
	} else
		blog(LOG_ERROR, "Script error: %s", message);

	duk_pop(ctx);
}

void OBSScript::Stop()
{
	DestroyContext();
}

void OBSScript::RegisterFunction(const char *name, duk_c_function func,
		duk_int_t args, bool internal)
{
	// OBS[name] = func;
	// or...
	// OBS.internal[name] = func;
	duk_push_global_object(ctx);
	duk_get_prop_string(ctx, -1, "OBS");
	if (internal)
		duk_get_prop_string(ctx, -1, "internal");
	duk_push_c_function(ctx, func, args);
	duk_put_prop_string(ctx, -2, name);
	duk_pop_n(ctx, internal ? 3 : 2);
}

OBSScript *OBSScript::GetInstance(duk_context *ctx)
{
	duk_memory_functions funcs;
	duk_get_memory_functions(ctx, &funcs);
	return reinterpret_cast<OBSScript *>(funcs.udata);
}

std::string OBSScript::GetPortableThreadID(std::thread::id threadID)
{
	std::stringstream ss;
	ss << threadID;
	return ss.str();
}

duk_ret_t OBSScript::SceneFind(duk_context *ctx)
{
	// arguments[0]: (string) name

	const char *name = duk_require_string(ctx, 0);

	obs_source_t *source = obs_get_source_by_name(name);
	if (source == nullptr) {
		duk_push_null(ctx);
		return 1;
	}

	obs_scene_t *scene = obs_scene_from_source(source);
	if (scene == nullptr) {
		obs_source_release(source);
		duk_push_null(ctx);
		return 1;
	}

	obs_scene_addref(scene);
	obs_source_release(source);

	duk_push_pointer(ctx, scene);

	return 1;
}

duk_ret_t OBSScript::SceneRelease(duk_context *ctx)
{
	// arguments[0]: (pointer) scene

	void *scene = duk_require_pointer(ctx, 0);
	obs_scene_release(reinterpret_cast<obs_scene_t *>(scene));

	return 0;
}

duk_ret_t OBSScript::SceneSelect(duk_context *ctx)
{
	// arguments[0]: (pointer) scene

	void *pointer = duk_require_pointer(ctx, 0);
	obs_scene_t *scene = reinterpret_cast<obs_scene_t *>(pointer);
	obs_source_t *source = obs_scene_get_source(scene);
	if (source == nullptr)
		duk_error(ctx, DUK_ERR_ERROR, "invalid scene");

	obs_set_output_source(0, source);

	return 0;
}

duk_ret_t OBSScript::SceneItemFind(duk_context *ctx)
{
	// arguments[0]: (pointer) scene
	// arguments[1]: (string) name

	void *pointer = duk_require_pointer(ctx, 0);
	obs_scene_t *scene = reinterpret_cast<obs_scene_t *>(pointer);
	const char *name = duk_require_string(ctx, 1);

	obs_sceneitem_t *source = obs_scene_find_source(scene, name);
	if (source == nullptr) {
		duk_push_null(ctx);
		return 1;
	}

	obs_sceneitem_addref(source);

	duk_push_pointer(ctx, source);

	return 1;
}

duk_ret_t OBSScript::SceneItemRelease(duk_context *ctx)
{
	// arguments[0]: (pointer) sceneItem

	void *sceneItem = duk_require_pointer(ctx, 0);
	obs_sceneitem_release(reinterpret_cast<obs_sceneitem_t *>(sceneItem));

	return 0;
}

duk_ret_t OBSScript::SceneItemSetVisible(duk_context *ctx)
{
	// arguments[0]: (pointer) sceneItem
	// arguments[1]: (boolean) visible

	void *pointer = duk_require_pointer(ctx, 0);
	obs_sceneitem_t *source = reinterpret_cast<obs_sceneitem_t *>(pointer);
	const bool visible = duk_require_boolean(ctx, 1);

	obs_sceneitem_set_visible(source, visible);

	return 0;
}

duk_ret_t OBSScript::SetTimer(duk_context *ctx)
{
	// arguments[0]: (number) time
	// arguments[1]: (callable) callback

	if (!duk_is_callable(ctx, 1))
		duk_error(ctx, DUK_ERR_TYPE_ERROR, "not a function");

	const duk_double_t time = duk_require_number(ctx, 0);
	OBSScript *script = GetInstance(ctx);

	std::thread thread([script, time]() {
		std::unique_lock<std::mutex> lock(script->timersMutex);
		auto duration = std::chrono::duration<duk_double_t>(time);
		auto result = script->timersHalt.wait_for(lock, duration);
		if (result == std::cv_status::no_timeout)
			return;
		script->EnqueueTimerCallback(std::this_thread::get_id());
	});

	const std::thread::id threadID = thread.get_id();
	script->timers.emplace(threadID, std::move(thread));

	// $stash.timers[threadID] = [this, callback];
	duk_push_global_stash(ctx);
	duk_get_prop_string(ctx, -1, "timers");
	duk_push_string(ctx, GetPortableThreadID(threadID).c_str());
	duk_idx_t array = duk_push_array(ctx);
	duk_push_this(ctx);
	duk_put_prop_index(ctx, array, 0);
	duk_dup(ctx, 1);
	duk_put_prop_index(ctx, array, 1);
	duk_put_prop(ctx, -3);
	duk_pop_2(ctx);

	return 0;
}
