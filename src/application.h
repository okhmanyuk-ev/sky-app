#pragma once

#include <sky/sky.h>
#include <sol/sol.hpp>

namespace skyapp
{
	struct ShowcaseApp
	{
		struct UrlSettings
		{
			std::string url;
		};

		struct GithubSettings
		{
			std::string name;
			std::string user;
			std::string repository;
			std::string branch;
			std::string filename;
		};

		using Settings = std::variant<
			UrlSettings,
			GithubSettings
		>;

		std::string name;
		Settings settings;
	};

	using Button = Shared::SceneHelpers::BouncingButtonBehavior<Shared::SceneHelpers::RectangleButton>;

	class App : public Scene::Node
	{
	public:
		App(bool drawBackButton);

	protected:
		void draw() override;

	public:
		void setLuaCode(const std::string& lua);

	private:
		sol::state mSolState;
		std::string mLuaCode;
	};

	class Application : public Shared::Application,
		public Common::FrameSystem::Frameable
	{
	public:
		Application();
		~Application();

	private:
		void onFrame() override;
		void drawShowcaseApps();
		void openShowcase(std::string url, std::function<void()> onFail = nullptr);
		void runApp(std::string url, bool drawBackButton);
		std::string makeGithubUrl(const std::string& user, const std::string& repository, const std::string& branch,
			const std::string& filename);

	private:
		std::vector<ShowcaseApp> mShowcaseApps;
		std::shared_ptr<App> mApp;
	};
}
