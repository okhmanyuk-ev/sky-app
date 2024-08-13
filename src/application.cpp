#include "application.h"

using namespace skyapp;

Application::Application() : Shared::Application(PROJECT_NAME, { Flag::Scene })
{
	PLATFORM->setTitle(PRODUCT_NAME);

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
	Scene::Scrollbox::DefaultInertiaFriction = 0.05f;
}

Application::~Application()
{
}

void Application::onFrame()
{
	auto label = IMSCENE->spawn<Scene::Label>(*getScene()->getRoot());
	if (IMSCENE->isFirstCall())
	{
		label->setText(L"Hello, World!");
		label->setAnchor(0.5f);
		label->setPivot(0.5f);
	}

	GAME_STATS("event listeners", EVENT->getListenersCount());
}
