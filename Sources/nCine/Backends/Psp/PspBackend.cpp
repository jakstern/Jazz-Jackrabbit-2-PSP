// PSP backend seam: nCine::theApplication() is the only backend symbol the engine requires (plus zlib).
// PspApplication supplies it, owning the gfx device and exposing the engine lifecycle to the PSP main.

#include "PspGfxDevice.h"
#include "PspAudioBackend.h"

#include "nCine/Application.h"
#include "nCine/IAppEventHandler.h"
#include "nCine/Input/IInputManager.h"
#include "nCine/ServiceLocator.h"

#include <memory>

namespace nCine
{
	class PspApplication : public Application
	{
	public:
		void Boot(CreateAppEventHandlerDelegate createAppEventHandler)
		{
			PspGuInit();
			gfxDevice_ = std::make_unique<PspGfxDevice>();
			inputManager_ = std::make_unique<NullInputManager>();
			PreInitCommon(createAppEventHandler());
			if (appCfg_.withAudio) {
				theServiceLocator().RegisterAudioDevice(CreatePspAudioDevice());
			}
			InitCommon();
		}

		void Frame() { Step(); }
		void Shut() { ShutdownCommon(); }
		bool WantsQuit() const { return shouldQuit_; }
	};

	Application& theApplication()
	{
		static PspApplication instance;
		return instance;
	}

	// Entry points for the PSP main, so main.cpp needs no Application internals.
	void PspBootEngine(CreateAppEventHandlerDelegate createAppEventHandler)
	{
		static_cast<PspApplication&>(theApplication()).Boot(createAppEventHandler);
	}

	void PspStepEngine()
	{
		static_cast<PspApplication&>(theApplication()).Frame();
	}

	bool PspEngineWantsQuit()
	{
		return static_cast<PspApplication&>(theApplication()).WantsQuit();
	}

	void PspShutdownEngine()
	{
		static_cast<PspApplication&>(theApplication()).Shut();
	}
}
