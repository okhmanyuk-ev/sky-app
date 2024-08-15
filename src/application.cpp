#include "application.h"
#include <sol/sol.hpp>
#ifndef PLATFORM_EMSCRIPTEN
#include <curl/curl.h>
#else
#include <emscripten.h>
#include <emscripten/fetch.h>
#endif

using namespace skyapp;

using DownloadCallback = std::function<void(void*, size_t)>;

static void DownloadFileToMemory(const std::string& url, DownloadCallback callback)
{
	sky::Log("fetching {}", url);

#ifndef PLATFORM_EMSCRIPTEN
	auto curl = curl_easy_init();

	if (!curl)
		throw std::runtime_error("cannot initialize curl");

	auto write_func = +[](char* memory, size_t size, size_t nmemb, void* userdata) -> size_t {
		auto real_size = size * nmemb;
		auto callback = *(DownloadCallback*)userdata;
		sky::Log("fetched {} bytes", real_size);
		callback((void*)memory, real_size);
		return real_size;
	};

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callback);

	auto res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
		throw std::runtime_error(std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res));
#else
	auto onsuccess = [](emscripten_fetch_t* fetch) {
		auto memory = fetch->data;
		auto size = fetch->numBytes;
		auto callback = (DownloadCallback*)fetch->userData;
		(*callback)((void*)memory, size);
		delete callback;
		sky::Log("fetched {} bytes", size);
		emscripten_fetch_close(fetch);
	};

	auto onerror = [](emscripten_fetch_t* fetch) {
		sky::Log("fetch failed");
		auto callback = (DownloadCallback*)fetch->userData;
		delete callback;
		emscripten_fetch_close(fetch);
	};

	auto onprogress = [](emscripten_fetch_t* fetch) {
		sky::Log("fetch progress {} of {}", fetch->dataOffset, fetch->totalBytes);
	};

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.onsuccess = onsuccess;
	attr.onerror = onerror;
	attr.onprogress = onprogress;
	attr.userData = new DownloadCallback(callback);
	emscripten_fetch(&attr, url.c_str());
#endif
}

static sol::state gSolState;
static std::string gLuaCode;
static skygfx::utils::Scratch gScratch;

static void HandleError(const sol::protected_function_result& res)
{
	std::string error_type = "UNKNOWN ERROR";

	if (res.status() == sol::call_status::syntax)
		error_type = "SYNTAX ERROR";
	else if (res.status() == sol::call_status::runtime)
		error_type = "RUNTIME ERROR";

	auto msg = std::string(res.get<sol::error>().what());

	sky::Log(Console::Color::Red, "{}: {}", error_type, msg);
}

static int HandlePanic(lua_State* L)
{
	sky::Log(Console::Color::Red, lua_tostring(L, -1));

	auto lua = sol::state_view(L);
	sky::Log(Console::Color::Red, lua["debug"]["traceback"]());

	return 0;
}

static void ExecuteLuaCode()
{
	if (gLuaCode.empty())
		return;

	auto res = gSolState.do_string(gLuaCode);

	if (!res.valid())
		HandleError(res);
}

Application::Application() : Shared::Application(PROJECT_NAME, { Flag::Scene })
{
	PLATFORM->setTitle(PRODUCT_NAME);
	CONSOLE_DEVICE->setEnabled(true);
	STATS->setEnabled(true);

#if defined(PLATFORM_MAC)
	std::static_pointer_cast<Shared::ConsoleDevice>(CONSOLE_DEVICE)->setHiddenButtonEnabled(false);
#endif

	// limit maximum time delta to avoid animation breaks
	FRAME->setTimeDeltaLimit(Clock::FromSeconds(1.0f / 30.0f));

	PRECACHE_FONT_ALIAS("fonts/sansation.ttf", "default");

	STATS->setAlignment(Shared::StatsSystem::Align::BottomRight);

	Scene::Sprite::DefaultSampler = skygfx::Sampler::Linear;
	Scene::Sprite::DefaultTexture = TEXTURE("textures/default.png");
    Scene::Label::DefaultFont = FONT("default");

	// initialize lua

	gSolState.open_libraries();
	gSolState.set_panic(HandlePanic);

	auto nsConsole = gSolState.create_named_table("Console");
	nsConsole["Execute"] = [](const std::string& s) {
		CONSOLE->execute(s);
	};
	nsConsole["Write"] = [](const std::string& s, std::optional<int> _color) {
		auto color = (Console::Color)_color.value_or((int)Console::Color::Default);
		CONSOLE_DEVICE->write(s, color);
	};
	nsConsole["WriteLine"] = [](const std::string& s, std::optional<int> _color) {
		auto color = (Console::Color)_color.value_or((int)Console::Color::Default);
		CONSOLE_DEVICE->writeLine(s, color);
	};

	auto nsGfx = gSolState.create_named_table("Gfx");
	nsGfx["Clear"] = [](float r, float g, float b, float a) {
		skygfx::Clear(glm::vec4{ r, g, b, a });
	};
	for (auto mode : magic_enum::enum_values<skygfx::utils::MeshBuilder::Mode>())
	{
		auto name  = magic_enum::enum_name(mode);
		nsGfx[name] = mode;
	}
	nsGfx["Begin"] = [](int _mode) {
		auto mode = magic_enum::enum_cast<skygfx::utils::MeshBuilder::Mode>(_mode);
		gScratch.begin(mode.value());
	};
	nsGfx["Vertex"] = [](float x, float y, float z, float r, float g, float b, float a) {
		gScratch.vertex({ .pos = { x, y, z }, .color = { r, g, b, a } });
	};
	nsGfx["End"] = [] {
		gScratch.end();
	};
	nsGfx["Flush"] = [] {
		gScratch.flush();
	};

	//lua.do_string("package.path = package.path .. ';./assets/scripting/?.lua'");
	gLuaCode =
R"(function Frame()
	Gfx.Clear(0.125, 0.125, 0.125, 1.0);
end)";
	ExecuteLuaCode();

	CONSOLE->registerCommand("run", std::nullopt, { "url" }, {}, [](CON_ARGS) {
		auto url = CON_ARG(0);

		if (!url.starts_with("http://") && !url.starts_with("https://"))
			url = "http://" + url;

		if (!url.ends_with("/main.lua"))
			url += "/main.lua";

		DownloadFileToMemory(url, [](void* memory, size_t size) {
			gLuaCode = std::string((char*)memory, size);
			sky::Log(gLuaCode);
			ExecuteLuaCode();
		});
	});

	CONSOLE->registerCommand("run_github", std::nullopt, { "user", "repository", "branch" }, {}, [](CON_ARGS) {
		auto user = CON_ARG(0);
		auto repository = CON_ARG(1);
		auto branch = CON_ARG(2);
		CONSOLE->execute(std::format("run \"https://raw.githubusercontent.com/{}/{}/{}\"", user, repository, branch));
	});
}

Application::~Application()
{
}

void Application::onFrame()
{
	auto res = gSolState["Frame"]();

	if (!res.valid())
	{
		HandleError(res);
		if (gScratch.isBegan())
		{
			gScratch.end();
		}
	}

	gScratch.flush();

	ImGui::Begin("Lua Editor");
	ImGui::SetWindowSize({ 512, 256 }, ImGuiCond_Once);

	auto flags = ImGuiInputTextFlags_AllowTabInput;

	if (ImGui::InputTextMultiline("Lua", &gLuaCode, { -1, -1 }, flags))
		ExecuteLuaCode();

	ImGui::End();
}
