#pragma once

#include <sky/sky.h>

namespace skyapp
{
	class Application : public Shared::Application,
		public Common::FrameSystem::Frameable
	{
	public:
		Application();
		~Application();

	private:
		void onFrame() override;
	};
}
