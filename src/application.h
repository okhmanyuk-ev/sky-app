#pragma once

#include <sky/sky.h>

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

	class Application : public Shared::Application,
		public Common::FrameSystem::Frameable
	{
	public:
		Application();
		~Application();

	private:
		void onFrame() override;
		void drawShowcaseApps();

	private:
		std::vector<ShowcaseApp> mShowcaseApps;
	};
}
