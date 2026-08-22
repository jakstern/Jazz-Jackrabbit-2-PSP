// PSP backend seam: nCine::theApplication() is the only backend symbol the engine requires (plus zlib).
// PspApplication supplies it, owning the gfx device and exposing the engine lifecycle to the PSP main.

#include "PspGfxDevice.h"
#include "PspAudioBackend.h"

#include "nCine/Application.h"
#include "nCine/IAppEventHandler.h"
#include "nCine/Input/IInputManager.h"
#include "nCine/ServiceLocator.h"

#include <pspctrl.h>
#include <psppower.h>

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
		void PowerSuspend()
		{
			if (powerSuspended_) return;
			PspGuSuspend();
			Suspend();
			powerSuspended_ = true;
		}
		void PowerResume()
		{
			if (!powerSuspended_) return;
			// Clock and controller configuration are hardware state, not process memory; real units can return
			// with firmware defaults after standby.
			scePowerSetClockFrequency(333, 333, 166);
			sceCtrlSetSamplingCycle(0);
			sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
			PspGuResume();
			Resume();
			powerSuspended_ = false;
		}
		void Shut() { ShutdownCommon(); }
		bool WantsQuit() const { return shouldQuit_; }

	private:
		bool powerSuspended_ = false;
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

	void PspSuspendEngine()
	{
		static_cast<PspApplication&>(theApplication()).PowerSuspend();
	}

	void PspResumeEngine()
	{
		static_cast<PspApplication&>(theApplication()).PowerResume();
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
