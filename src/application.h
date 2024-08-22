#pragma once

#include <sky/sky.h>
#include <sol/sol.hpp>

namespace skyapp
{
	struct ShowcaseApp
	{
		std::string name;
		std::optional<std::string> avatar;
		std::string entry_point;
	};

	using Button = Shared::SceneHelpers::BouncingButtonBehavior<Shared::SceneHelpers::RectangleButton>;

	class App : public Scene::Rectangle,
		public std::enable_shared_from_this<App>,
		public Common::FrameSystem::Frameable
	{
	public:
		class Root;

	public:
		App(bool drawBackButton);

	private:
		void drawTheRoot();
		void onFrame() override;

	public:
		void setLuaCode(const std::string& lua);

	private:
		sol::state mSolState;
		std::string mLuaCode;
		std::shared_ptr<Root> mRoot;
	};

	class App::Root : public Scene::Node
	{
	protected:
		void draw() override;

	public:
		void setDrawCallback(std::function<void()> value) { mDrawCallback = value; }

	private:
		std::function<void()> mDrawCallback;
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
		void openAppPreview(std::string url);
		void runApp(std::string url, bool drawBackButton);
		std::string makeGithubUrl(const std::string& user, const std::string& repository, const std::string& branch,
			const std::string& filename);

	private:
		std::vector<ShowcaseApp> mShowcaseApps;
		std::shared_ptr<App> mApp;
	};
}
