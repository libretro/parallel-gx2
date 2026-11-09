#include "Cafe/OS/common/OSCommon.h"
#include "erreula.h"
#include "ErrEulaPrompt.h"
#include "util/helpers/helpers.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"


#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "OS/libs/coreinit/coreinit_DynLoad.h"

namespace nn
{
namespace erreula
{

	enum class ErrorDialogType : uint32
	{
		Code = 0,
		Text = 1,
		TextOneButton = 2,
		TextTwoButton = 3
	};

	static const sint32 FADE_TIME = 80;

	enum class ErrEulaState : uint32
	{
		Hidden = 0,
		Appearing = 1,
		Visible = 2,
		Disappearing = 3
	};

	enum class ResultType : uint32
	{
		None = 0,
		Finish = 1,
		Next = 2,
		Jump = 3,
		Password = 4
	};

	struct AppearError
	{
		AppearError() = default;
		AppearError(const AppearError& o)
		{
			errorType = o.errorType;
			screenType = o.screenType;
			controllerType = o.controllerType;
			holdType = o.holdType;
			errorCode = o.errorCode;
			framerate = o.framerate;
			text = o.text;
			button1Text = o.button1Text;
			button2Text = o.button2Text;
			title = o.title;
			drawCursor = o.drawCursor;
		}

		betype<ErrorDialogType> errorType;
		uint32be screenType;
		uint32be controllerType;
		uint32be holdType;
		uint32be errorCode;
		uint32be framerate;
		MEMPTR<uint16be> text;
		MEMPTR<uint16be> button1Text;
		MEMPTR<uint16be> button2Text;
		MEMPTR<uint16be> title;
		uint8 padding[3];
		bool drawCursor{};
	};

	using AppearArg = AppearError;

	static_assert(sizeof(AppearError) == 0x2C); // maybe larger

	struct HomeNixSignArg_t
	{
		uint32be framerate;
	};

	static_assert(sizeof(HomeNixSignArg_t) == 0x4); // maybe larger

	struct ControllerInfo_t
	{
		MEMPTR<VPADStatus_t> vpadStatus;
		MEMPTR<KPADStatus_t> kpadStatus[4]; // or 7 now like KPAD_MAX_CONTROLLERS?
	};

	static_assert(sizeof(ControllerInfo_t) == 0x14); // maybe larger

	class ErrEulaInstance
	{
	  public:
		enum class BUTTON_SELECTION : uint32
		{
			NONE = 0xFFFFFFFF,
			LEFT = 0,
			RIGHT = 1,
		};

		void Init()
		{
			m_buttonSelection = BUTTON_SELECTION::NONE;
			m_resultCode = -1;
			m_resultCodeForLeftButton = 0;
			m_resultCodeForRightButton = 0;
			SetState(ErrEulaState::Hidden);
		}

		void DoAppearError(AppearArg* arg)
		{
			m_buttonSelection = BUTTON_SELECTION::NONE;
			m_resultCode = -1;
			m_resultCodeForLeftButton = -1;
			m_resultCodeForRightButton = -1;
			// for standard dialog its 0 and 1?
			m_resultCodeForLeftButton = 0;
			m_resultCodeForRightButton = 1;
			SetState(ErrEulaState::Appearing);
		}

		void DoDisappearError()
		{
			if(m_state != ErrEulaState::Visible)
				return;
			SetState(ErrEulaState::Disappearing);
		}

		void DoCalc()
		{
			// appearing and disappearing state will automatically advance after some time
			if (m_state == ErrEulaState::Appearing || m_state == ErrEulaState::Disappearing)
			{
				uint32 elapsedTick = coreinit::OSGetTime() - m_lastStateChange;
				if (elapsedTick > coreinit::EspressoTime::ConvertMsToTimerTicks(FADE_TIME))
				{
					SetState(m_state == ErrEulaState::Appearing ? ErrEulaState::Visible : ErrEulaState::Hidden);
				}
			}
		}

		bool IsDecideSelectButtonError() const
		{
			return m_buttonSelection != BUTTON_SELECTION::NONE;
		}

		bool IsDecideSelectLeftButtonError() const
		{
			return m_buttonSelection != BUTTON_SELECTION::LEFT;
		}

		bool IsDecideSelectRightButtonError() const
		{
			return m_buttonSelection != BUTTON_SELECTION::RIGHT;
		}

		void SetButtonSelection(BUTTON_SELECTION selection)
		{
			cemu_assert_debug(m_buttonSelection == BUTTON_SELECTION::NONE);
			m_buttonSelection = selection;
			cemu_assert_debug(selection == BUTTON_SELECTION::LEFT || selection == BUTTON_SELECTION::RIGHT);
			m_resultCode = selection == BUTTON_SELECTION::LEFT ? m_resultCodeForLeftButton : m_resultCodeForRightButton;
		}

		ErrEulaState GetState() const
		{
			return m_state;
		}

		sint32 GetResultCode() const
		{
			return m_resultCode;
		}

		ResultType GetResultType() const
		{
		  if(m_resultCode == -1)
			  return ResultType::None;
		  if(m_resultCode < 10)
			  return ResultType::Finish;
		  if(m_resultCode >= 9999)
			  return ResultType::Next;
		  if(m_resultCode == 40)
			  return ResultType::Password;
		  return ResultType::Jump;
		}

		float GetFadeTransparency() const
		{
			if(m_state == ErrEulaState::Appearing || m_state == ErrEulaState::Disappearing)
			{
				uint32 elapsedTick = coreinit::OSGetTime() - m_lastStateChange;
				if(m_state == ErrEulaState::Appearing)
					return std::min<float>(1.0f, (float)elapsedTick / (float)coreinit::EspressoTime::ConvertMsToTimerTicks(FADE_TIME));
				else
					return std::max<float>(0.0f, 1.0f - (float)elapsedTick / (float)coreinit::EspressoTime::ConvertMsToTimerTicks(FADE_TIME));
			}
			return 1.0f;
		}

	  private:
		void SetState(ErrEulaState state)
		{
			m_state = state;
			m_lastStateChange = coreinit::OSGetTime();
		}

		ErrEulaState m_state;
		uint32 m_lastStateChange;

		/* +0x30 */ betype<sint32> m_resultCode;
		/* +0x239C */ betype<BUTTON_SELECTION> m_buttonSelection;
		/* +0x23A0 */ betype<sint32> m_resultCodeForLeftButton;
		/* +0x23A4 */ betype<sint32> m_resultCodeForRightButton;
	};

	struct ErrEula_t
	{
		SysAllocator<coreinit::OSMutex> mutex;
		uint32 regionType{0};
		uint32 langType{0};
		MEMPTR<coreinit::FSClient_t> fsClient;
		std::unique_ptr<ErrEulaInstance> errEulaInstance;
		AppearError currentDialog;
		bool homeNixSignVisible{false};
	} g_errEula = {};
	
	std::wstring GetText(uint16be* text)
	{
		std::wstringstream result;
		while(*text != 0)
		{
			auto c = (uint16)*text;
			result << static_cast<wchar_t>(c);
			text++;
		}

		return result.str();
	}
	

	void ErrEulaCreate(void* workmem, uint32 regionType, uint32 langType, coreinit::FSClient_t* fsClient)
	{
		coreinit::OSLockMutex(&g_errEula.mutex);

		g_errEula.regionType = regionType;
		g_errEula.langType = langType;
		g_errEula.fsClient = fsClient;
		cemu_assert_debug(!g_errEula.errEulaInstance);
		g_errEula.errEulaInstance = std::make_unique<ErrEulaInstance>();
		g_errEula.errEulaInstance->Init();

		coreinit::OSUnlockMutex(&g_errEula.mutex);
	}

	void ErrEulaDestroy()
	{
		g_errEula.errEulaInstance.reset();
	}

	// check if any dialog button was selected
	bool IsDecideSelectButtonError()
	{
		if(!g_errEula.errEulaInstance)
			return false;
		return g_errEula.errEulaInstance->IsDecideSelectButtonError();
	}

	// check if left dialog button was selected
	bool IsDecideSelectLeftButtonError()
	{
		if(!g_errEula.errEulaInstance)
			return false;
		return g_errEula.errEulaInstance->IsDecideSelectLeftButtonError();
	}

	// check if right dialog button was selected
	bool IsDecideSelectRightButtonError()
	{
		if(!g_errEula.errEulaInstance)
			return false;
		return g_errEula.errEulaInstance->IsDecideSelectRightButtonError();
	}

	sint32 GetResultCode()
	{
		if(!g_errEula.errEulaInstance)
			return -1;
		return g_errEula.errEulaInstance->GetResultCode();
	}

	ResultType GetResultType()
	{
		if(!g_errEula.errEulaInstance)
			return ResultType::None;
		return g_errEula.errEulaInstance->GetResultType();
	}

	void export_AppearHomeNixSign(PPCInterpreter_t* hCPU)
	{
		g_errEula.homeNixSignVisible = TRUE;
		osLib_returnFromFunction(hCPU, 0);
	}

	void ErrEulaAppearError(AppearArg* arg)
	{
		g_errEula.currentDialog = *arg;
		if(g_errEula.errEulaInstance)
			g_errEula.errEulaInstance->DoAppearError(arg);
	}

	void ErrEulaDisappearError()
	{
		if(g_errEula.errEulaInstance)
			g_errEula.errEulaInstance->DoDisappearError();
	}

	ErrEulaState ErrEulaGetStateErrorViewer()
	{
		if(!g_errEula.errEulaInstance)
			return ErrEulaState::Hidden;
		return g_errEula.errEulaInstance->GetState();
	}

	void export_ChangeLang(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(langType, 0);
		g_errEula.langType = langType;
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_IsAppearHomeNixSign(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, g_errEula.homeNixSignVisible);
	}

	void export_DisappearHomeNixSign(PPCInterpreter_t* hCPU)
	{
		g_errEula.homeNixSignVisible = FALSE;
		osLib_returnFromFunction(hCPU, 0);
	}

	// The dialog as words (ErrEulaPrompt). Returns how many buttons it has.
	static int DialogText(const AppearError& arg, std::string& text)
	{
		const std::string title = arg.title ? boost::nowide::narrow(GetText(arg.title.GetPtr())) : std::string();
		std::string body;
		if (arg.errorType == ErrorDialogType::Text || arg.errorType == ErrorDialogType::TextOneButton || arg.errorType == ErrorDialogType::TextTwoButton)
			body = arg.text ? boost::nowide::narrow(GetText(arg.text.GetPtr())) : std::string("Unknown Error");
		std::string button1 = "OK", button2;
		if (arg.errorType == ErrorDialogType::TextOneButton || arg.errorType == ErrorDialogType::TextTwoButton)
			button1 = arg.button1Text ? boost::nowide::narrow(GetText(arg.button1Text.GetPtr())) : std::string("Yes");
		if (arg.errorType == ErrorDialogType::TextTwoButton)
			button2 = arg.button2Text ? boost::nowide::narrow(GetText(arg.button2Text.GetPtr())) : std::string("No");
		text = ErrEulaPrompt((uint32)arg.errorCode, title, body, button1, button2);
		return button2.empty() ? 1 : 2;
	}

	// The viewer is answered as on the console (ErrEulaAnswer), while what it
	// says is shown through the frontend's own messages
	void ErrEulaCalc(ControllerInfo_t* controllerInfo)
	{
		if(!g_errEula.errEulaInstance)
			return;
		ErrEulaInstance& viewer = *g_errEula.errEulaInstance;
		viewer.DoCalc();
		if (viewer.GetState() != ErrEulaState::Visible || viewer.IsDecideSelectButtonError())
			return;
		std::string text;
		const int buttons = DialogText(g_errEula.currentDialog, text);
		// shown again now and then for as long as it is up: a message fades
		static uint64 s_shownAt = 0;
		static std::string s_shown;
		const uint64 now = coreinit::OSGetTime();
		if (text != s_shown || now - s_shownAt > (uint64)coreinit::EspressoTime::ConvertMsToTimerTicks(4000))
		{
			LatteOverlay_pushNotification(text, 5000);
			s_shown = text;
			s_shownAt = now;
		}
		if (!controllerInfo)
			return;
		uint32 vpadTrig = 0, wpadTrig = 0;
		if (VPADStatus_t* vpad = controllerInfo->vpadStatus.GetPtr())
			vpadTrig = (uint32)vpad->trig;
		for (auto& kpad : controllerInfo->kpadStatus)
		{
			if (KPADStatus_t* status = kpad.GetPtr())
				wpadTrig |= (uint32)status->trig;
		}
		switch (ErrEulaAnswer(vpadTrig, wpadTrig, buttons))
		{
		case ErrEulaPick::First:
			viewer.SetButtonSelection(ErrEulaInstance::BUTTON_SELECTION::LEFT);
			s_shown.clear();
			break;
		case ErrEulaPick::Second:
			viewer.SetButtonSelection(ErrEulaInstance::BUTTON_SELECTION::RIGHT);
			s_shown.clear();
			break;
		case ErrEulaPick::None:
			break;
		}
	}

	bool hasVisibleDialog()
	{
		if (!g_errEula.errEulaInstance)
			return false;
		const ErrEulaState state = g_errEula.errEulaInstance->GetState();
		return state == ErrEulaState::Visible || state == ErrEulaState::Appearing || state == ErrEulaState::Disappearing;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "erreula";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(ErrEulaCreate, "erreula", "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", LogType::SysApi);
			cafeExportRegisterFunc(ErrEulaDestroy, "erreula", "ErrEulaDestroy__3RplFv", LogType::SysApi);

			cafeExportRegisterFunc(IsDecideSelectButtonError, "erreula", "ErrEulaIsDecideSelectButtonError__3RplFv", LogType::SysApi);
			cafeExportRegisterFunc(IsDecideSelectLeftButtonError, "erreula", "ErrEulaIsDecideSelectLeftButtonError__3RplFv", LogType::SysApi);
			cafeExportRegisterFunc(IsDecideSelectRightButtonError, "erreula", "ErrEulaIsDecideSelectRightButtonError__3RplFv", LogType::SysApi);

			cafeExportRegisterFunc(GetResultCode, "erreula", "ErrEulaGetResultCode__3RplFv", LogType::SysApi);
			cafeExportRegisterFunc(GetResultType, "erreula", "ErrEulaGetResultType__3RplFv", LogType::SysApi);

			cafeExportRegisterFunc(ErrEulaAppearError, "erreula", "ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", LogType::SysApi);
			cafeExportRegisterFunc(ErrEulaDisappearError, "erreula", "ErrEulaDisappearError__3RplFv", LogType::SysApi);
			cafeExportRegisterFunc(ErrEulaGetStateErrorViewer, "erreula", "ErrEulaGetStateErrorViewer__3RplFv", LogType::SysApi);

			cafeExportRegisterFunc(ErrEulaCalc, "erreula", "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo", LogType::SysApi);

			osLib_addFunction("erreula", "ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", export_AppearHomeNixSign);
			osLib_addFunction("erreula", "ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", export_ChangeLang);
			osLib_addFunction("erreula", "ErrEulaIsAppearHomeNixSign__3RplFv", export_IsAppearHomeNixSign);
			osLib_addFunction("erreula", "ErrEulaDisappearHomeNixSign__3RplFv", export_DisappearHomeNixSign);
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				g_errEula.errEulaInstance.reset();
				OSInitMutexEx(&g_errEula.mutex, nullptr);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				g_errEula.errEulaInstance.reset();
				// todo - refactor and clean up these variables to be part of errEulaInstance
				g_errEula.regionType = 0;
				g_errEula.langType = 0;
				g_errEula.fsClient = nullptr;
				g_errEula.currentDialog = {};
				g_errEula.homeNixSignVisible = {};
				g_errEula.currentDialog = {};
			}
		}
	}s_COSErreulaModule;

	COSModule* GetModule()
	{
		return &s_COSErreulaModule;
	}
}
}
 
