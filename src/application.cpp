#include "application.h"
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
	sky::Log("fetch {}", url);

#ifndef PLATFORM_EMSCRIPTEN
	auto curl = curl_easy_init();

	auto failed = [&] {
		sky::Log(Console::Color::Red, "fetch failed {}", url);
		if (downloadFailedCallback)
			downloadFailedCallback();
	};

	if (!curl)
	{
		failed();
		return;
	}

	auto write_func = +[](char* memory, size_t size, size_t nmemb, void* userdata) -> size_t {
		size_t real_size = size * nmemb;
		auto* buffer = static_cast<std::vector<uint8_t>*>(userdata);
		buffer->insert(buffer->end(), memory, memory + real_size);
		return real_size;
	};

	std::vector<uint8_t> buffer;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

	auto res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
	{
		failed();
		return;
	}

	sky::Log(Console::Color::Green, "fetched {} bytes from {}", buffer.size(), url);
	downloadedCallback(buffer.data(), buffer.size());
#else
	struct Settings
	{
		std::string url;
		DownloadedCallback downloadedCallback;
		DownloadFailedCallback downloadFailedCallback;
	};

	auto onsuccess = [](emscripten_fetch_t* fetch) {
		auto memory = fetch->data;
		auto size = fetch->numBytes;
		auto settings = (Settings*)fetch->userData;
		sky::Log(Console::Color::Green, "fetched {} bytes from {}", size, settings->url);
		settings->downloadedCallback((void*)memory, size);
		delete settings;
		emscripten_fetch_close(fetch);
	};

	auto onerror = [](emscripten_fetch_t* fetch) {
		auto reason = std::string(fetch->statusText);
		auto settings = (Settings*)fetch->userData;
		sky::Log(Console::Color::Red, "fetch failed from {}, reason: {}", settings->url, reason);
		if (settings->downloadFailedCallback)
			settings->downloadFailedCallback();
		delete settings;
		emscripten_fetch_close(fetch);
	};

	auto onprogress = [](emscripten_fetch_t* fetch) {
		auto settings = (Settings*)fetch->userData;
		sky::Log(Console::Color::Gray, "fetch progress {} of {} from {}", fetch->dataOffset, fetch->totalBytes, settings->url);
	};

	auto settings = new Settings;
	settings->url = url;
	settings->downloadedCallback = downloadedCallback;
	settings->downloadFailedCallback = downloadFailedCallback;

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;
	attr.onsuccess = onsuccess;
	attr.onerror = onerror;
	attr.onprogress = onprogress;
	attr.userData = settings;
	emscripten_fetch(&attr, url.c_str());
#endif
}

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

	CONSOLE->registerCommand("run", std::nullopt, { "url" }, {}, [this](CON_ARGS) {
		auto url = CON_ARG(0);
		runApp(url, false);
	});

	CONSOLE->registerCommand("run_github", std::nullopt, { "user", "repository", "branch"  }, { "filename" }, [this](CON_ARGS) {
		auto user = CON_ARG(0);
		auto repository = CON_ARG(1);
		auto branch = CON_ARG(2);
		auto filename = CON_ARGS_COUNT <= 3 ? "main.lua" : CON_ARG(3);
		runApp(makeGithubUrl(user, repository, branch, filename), false);
	});

	CONSOLE->registerCommand("exit", std::nullopt, {}, {}, [this](CON_ARGS) {
		gLuaCodeLoaded = false;
		if (mApp)
		{
			mApp->getParent()->detach(mApp);
			mApp.reset();
		}
	});

	CONSOLE->registerCommand("toggleconsole", std::nullopt, {}, {}, [this](CON_ARGS) {
		std::static_pointer_cast<Shared::ConsoleDevice>(CONSOLE_DEVICE)->toggle();
	});

	openShowcase(makeGithubUrl("okhmanyuk-ev", "sky-app-showcase", "main", "apps.json"), [this] {
		openShowcase("localhost/apps.json");
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
		scrollbox->getContent()->setAutoSizeHeightEnabled(true);
		scrollbox->setStretch(1.0f);
		scrollbox->getBounding()->setStretch(1.0f);
		scrollbox->getContent()->setStretch({ 1.0f, 0.0f });
	}

	auto grid = IMSCENE->spawn<Scene::AutoSized<Scene::Grid>>(*scrollbox->getContent());
	if (IMSCENE->isFirstCall())
	{
		grid->setDirection(Scene::Grid::Direction::RightDown);
		grid->setAutoSizeWidthEnabled(false);
		grid->setAnchor(0.5f);
		grid->setPivot(0.5f);
		grid->setStretch({ 1.0f, 0.0f });
	}

	for (const auto& app : mShowcaseApps)
	{
		auto item = IMSCENE->spawn<Shared::SceneHelpers::Smoother<Scene::Node>>(*grid);
		if (IMSCENE->isFirstCall())
		{
			item->setSize(256.0f);

			auto rect = std::make_shared<Scene::ClippableStencil<Scene::Rectangle>>();
			rect->setAnchor(0.5f);
			rect->setPivot(0.5f);
			rect->setStretch(1.0f);
			rect->setSize(-8.0f);
			rect->setRounding(24.0f);
			rect->setAlpha(0.125f);
			rect->setAbsoluteRounding(true);
			rect->setSlicedSpriteOptimizationEnabled(false);
			item->attach(rect);

			if (app.avatar)
			{
				auto avatar = std::make_shared<Scene::Sprite>();
				avatar->setStretch(1.0f);
				rect->attach(avatar);
				if (auto url = app.avatar.value(); CACHE->hasTexture(url))
				{
					avatar->setTexture(TEXTURE(url));
				}
				else
				{
					DownloadFileToMemory(app.avatar.value(), [url, avatar](void* memory, size_t size) {
						auto image = Graphics::Image(memory, size);
						CACHE->loadTexture(image, url);
						avatar->setTexture(TEXTURE(url));
					});
				}
				auto white_fade = std::make_shared<Scene::Rectangle>();
				white_fade->setStretch(1.0f);
				white_fade->setColor({ Graphics::Color::White, 0.125f });
				avatar->attach(white_fade);

				auto top_fade = std::make_shared<Scene::Rectangle>();
				top_fade->setStretch({ 1.0f, 0.5f });
				top_fade->getEdgeColor(Scene::Rectangle::Edge::Top)->setColor({ Graphics::Color::Black, 0.5f });
				top_fade->getEdgeColor(Scene::Rectangle::Edge::Bottom)->setColor({ Graphics::Color::Black, 0.0f });
				white_fade->attach(top_fade);

				auto bottom_fade = std::make_shared<Scene::Rectangle>();
				bottom_fade->setStretch({ 1.0f, 0.5f });
				bottom_fade->setAnchor({ 0.0f, 1.0f });
				bottom_fade->setPivot({ 0.0f, 1.0f });
				bottom_fade->getEdgeColor(Scene::Rectangle::Edge::Top)->setColor({ Graphics::Color::Black, 0.0f });
				bottom_fade->getEdgeColor(Scene::Rectangle::Edge::Bottom)->setColor({ Graphics::Color::Black, 0.5f });
				white_fade->attach(bottom_fade);
			}

			auto title_holder = std::make_shared<Scene::ClippableScissor<Scene::AutoSized<Scene::Node>>>();
			title_holder->setAutoSizeWidthEnabled(false);
			title_holder->setPosition({ 0.0f, 24.0f });
			title_holder->setStretch({ 1.0f, 0.0f });
			title_holder->setSize({ -48.0f, 0.0f });
			title_holder->setAnchor({ 0.5f, 0.0f });
			title_holder->setPivot({ 0.5f, 0.0f });
			rect->attach(title_holder);

			auto title = std::make_shared<Scene::Label>();
			title->setText(sky::to_wstring(app.name));
			title->runAction(Actions::Collection::RepeatInfinite([title, title_holder]() -> std::unique_ptr<Actions::Action> {
				if (title->getAbsoluteWidth() <= title_holder->getAbsoluteWidth())
					return Actions::Collection::Wait(1.0f); // do nothing if holder is greater than title

				constexpr float MoveDuration = 2.5f;

				return Actions::Collection::MakeSequence(
					Actions::Collection::Wait(1.0f),
					Actions::Collection::MakeParallel(
						Actions::Collection::ChangeHorizontalAnchor(title, 1.0f, MoveDuration, Easing::CubicInOut),
						Actions::Collection::ChangeHorizontalPivot(title, 1.0f, MoveDuration, Easing::CubicInOut)
					),
					Actions::Collection::Wait(1.0f),
					Actions::Collection::MakeParallel(
						Actions::Collection::ChangeHorizontalAnchor(title, 0.0f, MoveDuration, Easing::CubicInOut),
						Actions::Collection::ChangeHorizontalPivot(title, 0.0f, MoveDuration, Easing::CubicInOut)
					)
				);
			}));
			title_holder->attach(title);

			auto button = std::make_shared<Button>();
			button->setAnchor(1.0f);
			button->setPivot(1.0f);
			button->setPosition({ -24.0f, -24.0f });
			button->setSize({ 96.0f, 32.0f });
			button->getLabel()->setText(L"RUN");
			button->setClickCallback([this, app] {
				runApp(app.entry_point, true);
			});
			button->setRounding(0.5f);
			rect->attach(button);
		}
	}
}

void Application::openShowcase(std::string url, std::function<void()> onFail)
{
	if (!url.starts_with("http://") && !url.starts_with("https://"))
		url = "http://" + url;

	//if (!url.ends_with("/apps.json"))
	//	url += "/apps.json";

	DownloadFileToMemory(url, [this](void* memory, size_t size) {
		auto str = std::string((char*)memory, size);
		auto json = nlohmann::json::parse(str);
		for (auto entry : json)
		{
			std::string type = entry["type"];

			if (type == "showcase")
			{
				std::string showcase_type = entry["showcase_type"];
				if (showcase_type == "url")
				{
					std::string url = entry["url"];
					openShowcase(url);
				}
				else if (showcase_type == "github")
				{
					std::string user = entry["user"];
					std::string repository = entry["repository"];
					std::string branch = entry["branch"];
					std::string filename = [&]() -> std::string {
						if (entry.contains("filename"))
							return entry["filename"];

						return "apps.json";
					}();
					openShowcase(makeGithubUrl(user, repository, branch, filename));
				}
			}
			else if (type == "app")
			{
				std::string app_type = entry["app_type"];
				if (app_type == "url")
				{
					std::string url = entry["url"];
					openAppPreview(url);
				}
				else if (app_type == "github")
				{
					std::string user = entry["user"];
					std::string repository = entry["repository"];
					std::string branch = entry["branch"];
					std::string filename = [&]() -> std::string {
						if (entry.contains("filename"))
							return entry["filename"];

						return "main.lua";
					}();
					openAppPreview(makeGithubUrl(user, repository, branch, filename));
				}
			}
		}
	}, onFail);
}

static std::string RemoveFileNameAndExtension(const std::string& url)
{
	size_t lastSlash = url.find_last_of('/');
	return url.substr(0, lastSlash);
}

void Application::openAppPreview(std::string url)
{
	if (!url.ends_with(".lua") && !url.ends_with(".json"))
	{
		sky::Log("openAppPreview: url must ends with .lua or .json");
		return;
	}
	if (!url.starts_with("http://") && !url.starts_with("https://"))
		url = "http://" + url;

	if (url.ends_with(".lua"))
	{
		ShowcaseApp app;
		app.name = url;
		app.entry_point = url;
		mShowcaseApps.push_back(app);
		return;
	}
	auto base = RemoveFileNameAndExtension(url) + "/";
	DownloadFileToMemory(url, [this, base](void* memory, size_t size) {
		auto str = std::string((char*)memory, size);
		auto json = nlohmann::json::parse(str);
		ShowcaseApp app;
		app.name = json["name"];
		if (json.contains("avatar"))
			app.avatar = json["avatar"];
		app.entry_point = json["entry_point"];
		if (app.avatar)
			app.avatar = base + app.avatar.value();
		app.entry_point = base + app.entry_point;
		mShowcaseApps.push_back(app);
	});
}

void Application::runApp(std::string url, bool drawBackButton)
{
	if (!url.starts_with("http://") && !url.starts_with("https://"))
		url = "http://" + url;

	if (!url.ends_with(".lua"))
		url += "/main.lua";

	DownloadFileToMemory(url, [this, drawBackButton](void* memory, size_t size) {
		if (mApp)
			mApp->getParent()->detach(mApp);

		mApp = std::make_shared<App>(drawBackButton);
		mApp->setLuaCode(std::string((char*)memory, size));
		getScene()->getRoot()->attach(mApp);
	});
}

std::string Application::makeGithubUrl(const std::string& user, const std::string& repository, const std::string& branch,
	const std::string& filename)
{
	return std::format("https://raw.githubusercontent.com/{}/{}/{}/{}", user, repository, branch, filename);
}

void Application::onFrame()
{
	if (!gLuaCodeLoaded)
	{
		drawShowcaseApps();
		return;
	}
}

App::App(bool drawBackButton)
{
	if (drawBackButton)
	{
		auto button = std::make_shared<Button>();
		button->setPosition({ 32.0f, 32.0f });
		button->setSize({ 96.0f, 32.0f });
		button->getLabel()->setText(L"EXIT");
		button->setClickCallback([] {
			CONSOLE->execute("exit");
		});
		button->setRounding(0.5f);
		attach(button);
	}

	mSolState.open_libraries();
	mSolState.set_panic(HandlePanic);

	// console

	auto console = mSolState.create_named_table("Console");
	console["Execute"] = [](const std::string& s) {
		CONSOLE->execute(s);
	};
	console["Log"] = [](const std::string& s, std::optional<int> _color) {
		std::optional color = Console::Color::Default;

		if (_color.has_value())
			color = magic_enum::enum_cast<Console::Color>(_color.value());

		CONSOLE_DEVICE->write("[lua] ", Console::Color::Yellow);
		sky::Log(color.value(), s);
	};
	auto console_color = mSolState.create_table();
	for (auto mode : magic_enum::enum_values<Console::Color>())
	{
		auto name = magic_enum::enum_name(mode);
		console_color[name] = mode;
	}
	console["Color"] = console_color;

	// gfx

	auto gfx = mSolState.create_named_table("Gfx");
	gfx["Clear"] = [](float r, float g, float b, float a) {
		skygfx::Clear(glm::vec4{ r, g, b, a });
	};
	auto gfx_topology = mSolState.create_table();
	for (auto mode : magic_enum::enum_values<skygfx::Topology>())
	{
		auto name = magic_enum::enum_name(mode);
		gfx_topology[name] = mode;
	}
	gfx["Topology"] = gfx_topology;
	gfx["SetTopology"] = [](int _topology) {
		auto topology = magic_enum::enum_cast<skygfx::Topology>(_topology);
		skygfx::SetTopology(topology.value());
	};

	//scratch

	auto gfx_mode = mSolState.create_table();
	for (auto mode : magic_enum::enum_values<skygfx::utils::MeshBuilder::Mode>())
	{
		auto name  = magic_enum::enum_name(mode);
		gfx_mode[name] = mode;
	}
	gfx["Mode"] = gfx_mode;
	gfx["Begin"] = [](int _mode) {
		auto mode = magic_enum::enum_cast<skygfx::utils::MeshBuilder::Mode>(_mode);
		gScratch.begin(mode.value());
	};
	gfx["Vertex"] = [](float x, float y, float z, float r, float g, float b, float a) {
		gScratch.vertex({ .pos = { x, y, z }, .color = { r, g, b, a } });
	};
	gfx["End"] = [] {
		gScratch.end();
	};
	gfx["Flush"] = [] {
		gScratch.flush();
	};
}

static void DisplayTable(const sol::table& tbl, std::string prefix)
{
	for (const auto& pair : tbl)
	{
		std::string name;

		if (pair.first.get_type() == sol::type::string) {
			name = pair.first.as<std::string>();
		}
		else if (pair.first.get_type() == sol::type::number) {
			name = std::to_string(pair.first.as<double>());
		}
		else {
			name = " (non-string key)";
		}

		name = prefix + name;

		sol::object value = pair.second;

		auto drawText = [&](const std::string& text) {
			ImGui::Text("%s", text.c_str());
		};

		if (value.get_type() == sol::type::function) {
			drawText(name + " (function)");
		}
		else if (value.get_type() == sol::type::number) {
			drawText(name + " (number): " + std::to_string(value.as<double>()));
		}
		else if (value.get_type() == sol::type::string) {
			drawText(name + " (string): " + value.as<std::string>());
		}
		else if (value.get_type() == sol::type::table) {
			if (ImGui::TreeNode((name).c_str())) {
				DisplayTable(value.as<sol::table>(), name + ".");
				ImGui::TreePop();
			}
		}
		else {
			drawText(name + " (other)");
		}
	}
}

static void ShowLuaFunctions(sol::state& lua)
{
	ImGui::Begin("Lua Functions");
	sol::table globals = lua.globals();
	DisplayTable(globals, "");
	ImGui::End();
}

void App::draw()
{
	Scene::Node::draw();

	if (mSolState["Frame"].is<sol::function>())
	{
		auto res = mSolState["Frame"]();
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
	}

	ImGui::Begin("Lua");
	ImGui::SetWindowSize({ 512, 256 }, ImGuiCond_Once);

	auto flags = ImGuiInputTextFlags_AllowTabInput;

	if (ImGui::InputTextMultiline("Lua", &mLuaCode, { -1, -1 }, flags))
		setLuaCode(mLuaCode);

	ImGui::End();

	ShowLuaFunctions(mSolState);
}

void App::setLuaCode(const std::string& lua)
{
	mLuaCode = lua;

	if (lua.empty())
		return;

	auto res = mSolState.do_string(lua);

	if (!res.valid())
		HandleError(res);
	else
		gLuaCodeLoaded = true;
}