#include "application.h"
#include <sol/sol.hpp>
#ifndef PLATFORM_EMSCRIPTEN
#include <curl/curl.h>
#else
#include <emscripten.h>
#include <emscripten/fetch.h>
#endif

using namespace skyapp;

using DownloadedCallback = std::function<void(void*, size_t)>;
using DownloadFailedCallback = std::function<void()>;

static void DownloadFileToMemory(const std::string& url, DownloadedCallback downloadedCallback,
	DownloadFailedCallback downloadFailedCallback = nullptr)
{
	sky::Log("fetching {}", url);

#ifndef PLATFORM_EMSCRIPTEN
	auto curl = curl_easy_init();

	auto failed = [&] {
		sky::Log("fetch failed");
		if (downloadFailedCallback)
			downloadFailedCallback();
	};

	if (!curl)
	{
		failed();
		return;
	}

	auto write_func = +[](char* memory, size_t size, size_t nmemb, void* userdata) -> size_t {
		auto real_size = size * nmemb;
		auto callback = *(DownloadedCallback*)userdata;
		sky::Log("fetched {} bytes", real_size);
		callback((void*)memory, real_size);
		return real_size;
	};

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &downloadedCallback);

	auto res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
		failed();
#else
	struct Callbacks
	{
		DownloadedCallback downloadedCallback;
		DownloadFailedCallback downloadFailedCallback;
	};

	auto onsuccess = [](emscripten_fetch_t* fetch) {
		auto memory = fetch->data;
		auto size = fetch->numBytes;
		auto callbacks = (Callbacks*)fetch->userData;
		callbacks->downloadedCallback((void*)memory, size);
		delete callbacks;
		sky::Log("fetched {} bytes", size);
		emscripten_fetch_close(fetch);
	};

	auto onerror = [](emscripten_fetch_t* fetch) {
		sky::Log("fetch failed");
		auto callbacks = (Callbacks*)fetch->userData;
		if (callbacks->downloadFailedCallback)
			callbacks->downloadFailedCallback();
		delete callbacks;
		emscripten_fetch_close(fetch);
	};

	auto onprogress = [](emscripten_fetch_t* fetch) {
		sky::Log("fetch progress {} of {}", fetch->dataOffset, fetch->totalBytes);
	};

	auto callbacks = new Callbacks;
	callbacks->downloadedCallback = downloadedCallback;
	callbacks->downloadFailedCallback = downloadFailedCallback;

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.onsuccess = onsuccess;
	attr.onerror = onerror;
	attr.onprogress = onprogress;
	attr.userData = callbacks;
	emscripten_fetch(&attr, url.c_str());
#endif
}

static sol::state gSolState;
static std::string gLuaCode;
static skygfx::utils::Scratch gScratch;
static bool gLuaCodeLoaded = false;

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
	else
		gLuaCodeLoaded = true;
}

static void Run(const std::string& url)
{
	DownloadFileToMemory(url, [](void* memory, size_t size) {
		gLuaCode = std::string((char*)memory, size);
		ExecuteLuaCode();
	});
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
//	gLuaCode =
//R"(function Frame()
//	Gfx.Clear(0.125, 0.125, 0.125, 1.0);
//end)";
//	ExecuteLuaCode();

	CONSOLE->registerCommand("run", std::nullopt, { "url" }, {}, [](CON_ARGS) {
		auto url = CON_ARG(0);

		if (!url.starts_with("http://") && !url.starts_with("https://"))
			url = "http://" + url;

		if (!url.ends_with("/main.lua"))
			url += "/main.lua";

		Run(url);
	});

	CONSOLE->registerCommand("run_github", std::nullopt, { "user", "repository", "branch"  }, { "filename" }, [](CON_ARGS) {
		auto user = CON_ARG(0);
		auto repository = CON_ARG(1);
		auto branch = CON_ARG(2);
		auto filename = CON_ARGS_COUNT <= 3 ? "main.lua" : CON_ARG(3);
		Run(std::format("https://raw.githubusercontent.com/{}/{}/{}/{}", user, repository, branch, filename));
	});

	CONSOLE->registerCommand("showcase", std::nullopt, { "url" }, {}, [this](CON_ARGS) {
		auto url = CON_ARG(0);

		if (!url.starts_with("http://") && !url.starts_with("https://"))
			url = "http://" + url;

		if (!url.ends_with("/apps.json"))
			url += "/apps.json";

		DownloadFileToMemory(url, [this](void* memory, size_t size) {
			auto str = std::string((char*)memory, size);
			auto json = nlohmann::json::parse(str);
			for (auto entry : json)
			{
				ShowcaseApp app;
				app.name = entry["name"];
				std::string type = entry["type"];
				if (type == "url")
				{
					ShowcaseApp::UrlSettings settings;
					settings.url = entry["url"];
					app.settings = settings;
				}
				else if (type == "github")
				{
					ShowcaseApp::GithubSettings settings;
					settings.user = entry["user"];
					settings.repository = entry["repository"];
					settings.branch = entry["branch"];

					if (entry.contains("filename"))
						settings.filename = entry["filename"];
					else
						settings.filename = "main.lua";

					app.settings = settings;
				}
				mShowcaseApps.push_back(app);
			}
		});
	});

	CONSOLE->registerCommand("stop", std::nullopt, {}, {}, [](CON_ARGS) {
		gLuaCodeLoaded = false;
	});


	DownloadFileToMemory("http://localhost/apps.json", [](auto, auto) {
		CONSOLE->execute("showcase localhost");
	}, [] {
		CONSOLE->execute("showcase \"https://raw.githubusercontent.com/okhmanyuk-ev/sky-app-showcase/main\"");
	});
}

Application::~Application()
{
}

void Application::drawShowcaseApps()
{
	auto scroll_holder = IMSCENE->spawn(*getScene()->getRoot());
	if (IMSCENE->isFirstCall())
	{
		scroll_holder->setAnchor({ 0.5f, 1.0f });
		scroll_holder->setPivot({ 0.5f, 1.0f });
		scroll_holder->setStretch({ 0.75f, 1.0f });
		scroll_holder->setHeight(-192.0f);

		auto title = std::make_shared<Scene::Label>();
		title->setPivot({ 0.0f, 1.0f });
		title->setText(L"Showcase");
		title->setFontSize(64.0f);
		title->setAlpha(0.75f);
		title->setPosition({ 24.0f, -24.0f });
		scroll_holder->attach(title);
	}

	auto scrollbox = IMSCENE->spawn<Scene::ClippableScissor<Scene::Scrollbox>>(*scroll_holder);
	if (IMSCENE->isFirstCall())
	{
		auto content = std::make_shared<Scene::AutoSized<Scene::Node>>();
		content->setAutoSizeWidthEnabled(false);
		scrollbox->setCustomContent(content);
		scrollbox->setStretch(1.0f);
		scrollbox->getBounding()->setStretch(1.0f);
		scrollbox->getContent()->setStretch({ 1.0f, 0.0f });
	}

	auto grid = IMSCENE->spawn<Scene::AutoSized<Scene::Grid>>(*scrollbox->getContent());
	if (IMSCENE->isFirstCall())
	{
		grid->setOrientation(Scene::Grid::Orientation::Vertical);
		grid->setAutoSizeWidthEnabled(false);
		grid->setAnchor(0.5f);
		grid->setPivot(0.5f);
		grid->setStretch({ 1.0f, 0.0f });
	}

	for (const auto& app : mShowcaseApps)
	{
		auto callback = std::visit(cases{
			[&](const ShowcaseApp::UrlSettings& settings) -> std::function<void()> {
				return [settings] {
					CONSOLE->execute(std::format("run \"{}\"", settings.url));
				};
			},
			[](const ShowcaseApp::GithubSettings& settings) -> std::function<void()> {
				return [settings] {
					CONSOLE->execute(std::format("run_github {} {} {} {}", settings.user, settings.repository,
						settings.branch, settings.filename));
				};
			}
		}, app.settings);

		auto item = IMSCENE->spawn<Shared::SceneHelpers::Smoother<Scene::Node>>(*grid);
		if (IMSCENE->isFirstCall())
		{
			item->setSize(256.0f);

			auto rect = std::make_shared<Scene::Rectangle>();
			rect->setAnchor(0.5f);
			rect->setPivot(0.5f);
			rect->setStretch(1.0f);
			rect->setSize(-8.0f);
			rect->setRounding(24.0f);
			rect->setAlpha(0.125f);
			rect->setAbsoluteRounding(true);
			item->attach(rect);

			auto label = std::make_shared<Scene::Label>();
			label->setAnchor(0.0f);
			label->setPivot(0.0f);
			label->setPosition({ 24.0f, 24.0f });
			label->setText(sky::to_wstring(app.name));
			rect->attach(label);

			auto button = std::make_shared<Shared::SceneHelpers::RectangleButton>();
			button->setAnchor(1.0f);
			button->setPivot(1.0f);
			button->setPosition({ -24.0f, -24.0f });
			button->setSize({ 96.0f, 32.0f });
			button->getLabel()->setText(L"RUN");
			button->setClickCallback(callback);
			button->setRounding(0.5f);
			rect->attach(button);
		}
	}
}

void Application::onFrame()
{
	if (!gLuaCodeLoaded)
	{
		drawShowcaseApps();
		return;
	}

	auto res = gSolState["Frame"]();
	if (!res.valid())
	{
		HandleError(res);
		if (gScratch.isBegan())
		{
			gScratch.end();
		}
	}
	try
	{
		gScratch.flush();
	}
	catch (const std::exception& e)
	{
		sky::Log(Console::Color::Red, e.what());
	}

	ImGui::Begin("Lua Editor");
	ImGui::SetWindowSize({ 512, 256 }, ImGuiCond_Once);

	auto flags = ImGuiInputTextFlags_AllowTabInput;

	if (ImGui::InputTextMultiline("Lua", &gLuaCode, { -1, -1 }, flags))
		ExecuteLuaCode();

	ImGui::End();
}
