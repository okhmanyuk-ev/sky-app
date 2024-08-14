#include "application.h"
#include <sol/sol.hpp>

using namespace skyapp;

static sol::state gSolState;
static std::string gLuaCode;
static skygfx::utils::Scratch gScratch;

static void HandleError(const auto& res)
{
	std::string error_type = "UNKNOWN ERROR";

	if (res.status() == sol::call_status::syntax)
		error_type = "SYNTAX ERROR";
	else if (res.status() == sol::call_status::runtime)
		error_type = "RUNTIME ERROR";

	auto msg = std::string(res.template get<sol::error>().what());

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
	Gfx.Begin(Gfx.Triangles);
	Gfx.Vertex(0.5, -0.5, 0.0, 0.0, 0.0, 1.0, 1.0);
	Gfx.Vertex(-0.5, -0.5, 0.0, 1.0, 0.0, 0.0, 1.0);
	Gfx.Vertex(0.0, 0.5, 0.0, 0.0, 1.0, 0.0, 1.0);
	Gfx.End();
end)";
	ExecuteLuaCode();
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
