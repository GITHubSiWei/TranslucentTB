// Standard API
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

// Windows API
#include "arch.h"
#include <PathCch.h>
#include <ShlObj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

// Local stuff
#include "appvisibilitysink.hpp"
#include "autofree.hpp"
#include "autostart.hpp"
#include "blacklist.hpp"
#include "classiccomptr.hpp"
#include "common.hpp"
#include "config.hpp"
#include "eventhook.hpp"
#include "messagewindow.hpp"
#include "resource.h"
#include "swcadata.hpp"
#include "traycontextmenu.hpp"
#include "ttberror.hpp"
#include "ttblog.hpp"
#include "util.hpp"
#ifdef STORE
#include "UWP.hpp"
#endif
#include "win32.hpp"
#include "window.hpp"
#include "windowclass.hpp"

#pragma region Data

enum class EXITREASON {
	NewInstance,		// New instance told us to exit
	UserAction,			// Triggered by the user
	UserActionNoSave	// Triggered by the user, but doesn't saves config
};

static struct {
	EXITREASON exit_reason = EXITREASON::UserAction;
	Window main_taskbar;
	std::unordered_map<HMONITOR, std::pair<Window, const Config::TASKBAR_APPEARANCE *>> taskbars;
	bool should_show_peek = true;
	bool is_running = true;
	std::wstring config_folder;
	std::wstring config_file;
	std::wstring exclude_file;
	bool peek_active = false;
	bool start_opened = false;
} run;

static const std::unordered_map<swca::ACCENT, uint32_t> REGULAR_BUTTOM_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_REGULAR_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_REGULAR_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_REGULAR_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_REGULAR_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_REGULAR_FLUENT }
};

static const std::unordered_map<swca::ACCENT, uint32_t> MAXIMISED_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_MAXIMISED_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_MAXIMISED_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_MAXIMISED_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_MAXIMISED_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_MAXIMISED_FLUENT }
};

static const std::unordered_map<swca::ACCENT, uint32_t> START_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_START_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_START_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_START_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_START_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_START_FLUENT }
};

static const std::unordered_map<swca::ACCENT, uint32_t> CORTANA_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_CORTANA_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_CORTANA_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_CORTANA_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_CORTANA_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_CORTANA_FLUENT }
};

static const std::unordered_map<swca::ACCENT, uint32_t> TIMELINE_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_TIMELINE_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_TIMELINE_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_TIMELINE_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_TIMELINE_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_TIMELINE_FLUENT }
};

static const std::unordered_map<enum Config::PEEK, uint32_t> PEEK_BUTTON_MAP = {
	{ Config::PEEK::Enabled,		IDM_PEEK_SHOW    },
	{ Config::PEEK::Dynamic,		IDM_PEEK_DYNAMIC },
	{ Config::PEEK::Disabled,		IDM_PEEK_HIDE    }
};

#pragma endregion

#pragma region That one function that does all the magic

void SetWindowBlur(const Window &window, const swca::ACCENT &appearance, const uint32_t &color)
{
	if (user32::SetWindowCompositionAttribute)
	{
		static std::unordered_map<Window, bool> is_normal;

		swca::ACCENTPOLICY policy = {
			appearance,
			2,
			(color & 0xFF00FF00) + ((color & 0x00FF0000) >> 16) + ((color & 0x000000FF) << 16),
			0
		};

		if (policy.nAccentState == swca::ACCENT::ACCENT_NORMAL)
		{
			if (is_normal.count(window) == 0 || !is_normal[window])
			{
				// WM_THEMECHANGED makes the taskbar reload the theme and reapply the normal effect.
				// Gotta memoize it because constantly sending it makes explorer's CPU usage jump.
				window.send_message(WM_THEMECHANGED);
				is_normal[window] = true;
			}
			return;
		}
		else if (policy.nAccentState == swca::ACCENT::ACCENT_ENABLE_FLUENT && policy.nColor >> 24 == 0x00)
		{
			// Fluent mode doesn't likes a completely 0 opacity
			policy.nColor = (0x01 << 24) + (policy.nColor & 0x00FFFFFF);
		}

		swca::WINCOMPATTRDATA data = {
			swca::WindowCompositionAttribute::WCA_ACCENT_POLICY,
			&policy,
			sizeof(policy)
		};

		user32::SetWindowCompositionAttribute(window, &data);
		is_normal[window] = false;
	}
}

#pragma endregion

#pragma region Configuration

void GetPaths()
{
#ifndef STORE
	AutoFree::CoTaskMem<wchar_t> appData;
	ErrorHandle(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, NULL, &appData), Error::Level::Fatal, L"Failed to determine configuration files locations!");
#else
	try
	{
		std::wstring appData_str = UWP::GetApplicationFolderPath(UWP::FolderType::Roaming);
		const wchar_t *appData = appData_str.c_str();
#endif

	AutoFree::Local<wchar_t> configFolder;
	AutoFree::Local<wchar_t> configFile;
	AutoFree::Local<wchar_t> excludeFile;

	ErrorHandle(PathAllocCombine(appData, NAME, PATHCCH_ALLOW_LONG_PATHS, &configFolder), Error::Level::Fatal, L"Failed to combine AppData folder and application name!");
	ErrorHandle(PathAllocCombine(configFolder, CONFIG_FILE, PATHCCH_ALLOW_LONG_PATHS, &configFile), Error::Level::Fatal, L"Failed to combine config folder and config file!");
	ErrorHandle(PathAllocCombine(configFolder, EXCLUDE_FILE, PATHCCH_ALLOW_LONG_PATHS, &excludeFile), Error::Level::Fatal, L"Failed to combine config folder and exclude file!");

	run.config_folder = configFolder;
	run.config_file = configFile;
	run.exclude_file = excludeFile;

#ifdef STORE
	}
	catch (const winrt::hresult_error &error)
	{
		ErrorHandle(error.code(), Error::Level::Fatal, L"Getting application folder paths failed!");
	}
#endif
}

void ApplyStock(const std::wstring &filename)
{
	std::wstring exeFolder_str = win32::GetExeLocation();
	exeFolder_str.erase(exeFolder_str.find_last_of(LR"(/\)") + 1);

	AutoFree::Local<wchar_t> stockFile;
	if (!ErrorHandle(PathAllocCombine(exeFolder_str.c_str(), filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &stockFile), Error::Level::Error, L"Failed to combine executable folder and config file!"))
	{
		return;
	}

	AutoFree::Local<wchar_t> configFile;
	if (!ErrorHandle(PathAllocCombine(run.config_folder.c_str(), filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &configFile), Error::Level::Error, L"Failed to combine config folder and config file!"))
	{
		return;
	}

	if (!win32::IsDirectory(run.config_folder))
	{
		if (!CreateDirectory(run.config_folder.c_str(), NULL))
		{
			LastErrorHandle(Error::Level::Error, L"Creating configuration files directory failed!");
			return;
		}
	}

	if (!CopyFile(stockFile, configFile, FALSE))
	{
		LastErrorHandle(Error::Level::Error, L"Copying stock configuration file failed!");
	}
}

bool CheckAndRunWelcome()
{
	if (!win32::IsDirectory(run.config_folder))
	{
		// String concatenation is hard OK
		std::wostringstream message;
		message <<
			L"Welcome to " NAME L"!\n\n"
			L"You can tweak the taskbar's appearance with the tray icon. If it's your cup of tea, you can also edit the configuration files, located at \"" <<
			run.config_folder <<
			L"\"\n\nDo you agree to the GPLv3 license?";

		if (MessageBox(NULL, message.str().c_str(), NAME, MB_ICONINFORMATION | MB_YESNO | MB_SETFOREGROUND) != IDYES)
		{
			return false;
		}
	}
	if (!win32::FileExists(run.config_file))
	{
		ApplyStock(CONFIG_FILE);
	}
	if (!win32::FileExists(run.exclude_file))
	{
		ApplyStock(EXCLUDE_FILE);
	}
	return true;
}

#pragma endregion

#pragma region Utilities

void RefreshHandles()
{
	if (Config::VERBOSE)
	{
		Log::OutputMessage(L"Refreshing taskbar handles");
	}

	// Older handles are invalid, so clear the map to be ready for new ones
	run.taskbars.clear();

	run.main_taskbar = Window::Find(L"Shell_TrayWnd");
	run.taskbars[run.main_taskbar.monitor()] = { run.main_taskbar, &Config::REGULAR_APPEARANCE };

	Window secondtaskbar;
	while ((secondtaskbar = Window::Find(L"Shell_SecondaryTrayWnd", L"", Window::NullWindow, secondtaskbar)) != Window::NullWindow)
	{
		run.taskbars[secondtaskbar.monitor()] = { secondtaskbar, &Config::REGULAR_APPEARANCE };
	}
}

void TogglePeek(const bool &status)
{
	static bool cached_peek = true;
	static Window cached_taskbar = Window(run.main_taskbar);

	if (status != cached_peek || cached_taskbar != run.main_taskbar)
	{
		Window _peek = Window::Find(L"TrayShowDesktopButtonWClass", L"", Window::Find(L"TrayNotifyWnd", L"", run.main_taskbar));

		if (!status)
		{
			SetWindowLong(_peek, GWL_EXSTYLE, GetWindowLong(_peek, GWL_EXSTYLE) | WS_EX_LAYERED);

			SetLayeredWindowAttributes(_peek, 0, 0, LWA_ALPHA);
		}
		else
		{
			SetWindowLong(_peek, GWL_EXSTYLE, GetWindowLong(_peek, GWL_EXSTYLE) & ~WS_EX_LAYERED);
		}

		cached_peek = status;
		cached_taskbar = Window(run.main_taskbar);
	}
}

#pragma endregion

#pragma region Tray

inline void ChangePopupItemText(HMENU menu, const uint32_t &item, std::wstring &&new_text)
{
	MENUITEMINFO item_info = { sizeof(item_info), MIIM_STRING };

	item_info.dwTypeData = new_text.data();
	SetMenuItemInfo(menu, item, false, &item_info);
}

void RefreshMenu(HMENU menu)
{
	static bool initial_check_done = false;
	if (!initial_check_done)
	{
		if (!win32::IsAtLeastBuild(MIN_FLUENT_BUILD))
		{
			RemoveMenu(menu, IDM_REGULAR_FLUENT,   MF_BYCOMMAND);
			RemoveMenu(menu, IDM_MAXIMISED_FLUENT, MF_BYCOMMAND);
			RemoveMenu(menu, IDM_START_FLUENT,     MF_BYCOMMAND);
			RemoveMenu(menu, IDM_CORTANA_FLUENT,   MF_BYCOMMAND);
			RemoveMenu(menu, IDM_TIMELINE_FLUENT,  MF_BYCOMMAND);

			// Same build for Timeline and fluent
			ChangePopupItemText(menu, IDM_TIMELINE_POPUP, L"Task View opened");
		}

		initial_check_done = true;
	}

	const bool has_log = !Log::file().empty();
	TrayContextMenu::RefreshBool(IDM_OPENLOG, menu, has_log, TrayContextMenu::ControlsEnabled);
	ChangePopupItemText(menu, IDM_OPENLOG, has_log ? L"Open log file" : L"Nothing has been logged yet");

	TrayContextMenu::RefreshBool(IDM_REGULAR_COLOR,   menu,
		Config::REGULAR_APPEARANCE.ACCENT != swca::ACCENT::ACCENT_NORMAL,
		TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_MAXIMISED_COLOR, menu,
		Config::MAXIMISED_ENABLED && Config::MAXIMISED_APPEARANCE.ACCENT != swca::ACCENT::ACCENT_NORMAL,
		TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_START_COLOR,     menu,
		Config::START_ENABLED     && Config::START_APPEARANCE.ACCENT != swca::ACCENT::ACCENT_NORMAL,
		TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_CORTANA_COLOR,     menu,
		Config::CORTANA_ENABLED   && Config::CORTANA_APPEARANCE.ACCENT != swca::ACCENT::ACCENT_NORMAL,
		TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_TIMELINE_COLOR,  menu,
		Config::TIMELINE_ENABLED  && Config::TIMELINE_APPEARANCE.ACCENT != swca::ACCENT::ACCENT_NORMAL,
		TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_PEEK_ONLY_MAIN,  menu,
		Config::PEEK == Config::PEEK::Dynamic,
		TrayContextMenu::ControlsEnabled);

	const Autostart::StartupState state = Autostart::GetStartupState();
	TrayContextMenu::RefreshBool(IDM_AUTOSTART, menu, !(state == Autostart::StartupState::DisabledByUser
#ifdef STORE
		|| state == Autostart::StartupState::DisabledByPolicy
		|| state == Autostart::StartupState::EnabledByPolicy
#endif
		), TrayContextMenu::ControlsEnabled);

	std::wstring autostart_text;
	switch (state)
	{
	case Autostart::StartupState::DisabledByUser:
		autostart_text = L"Startup has been disabled in Task Manager";
		break;
#ifdef STORE
	case Autostart::StartupState::DisabledByPolicy:
		autostart_text = L"Startup has been disabled in Group Policy";
		break;
	case Autostart::StartupState::EnabledByPolicy:
		autostart_text = L"Startup has been enabled in Group Policy";
		break;
#endif
	case Autostart::StartupState::Enabled:
	case Autostart::StartupState::Disabled:
		autostart_text = L"Open at boot";
	}
	ChangePopupItemText(menu, IDM_AUTOSTART, std::move(autostart_text));

	TrayContextMenu::RefreshBool(IDM_AUTOSTART, menu, state == Autostart::StartupState::Enabled
#ifdef STORE
		|| state == Autostart::StartupState::EnabledByPolicy
#endif
		, TrayContextMenu::Toggle);
}

#pragma endregion

#pragma region Main logic

BOOL CALLBACK EnumWindowsProcess(const HWND hWnd, LPARAM)
{
	const Window window(hWnd);
	// DWMWA_CLOAKED should take care of checking if it's on the current desktop.
	// But that's undocumented behavior.
	// Do both but with on_current_desktop last.
	if (window.visible() && window.state() == SW_MAXIMIZE && !window.get_attribute<BOOL>(DWMWA_CLOAKED) && !Blacklist::IsBlacklisted(window) && window.on_current_desktop())
	{
		auto &taskbar = run.taskbars.at(window.monitor());
		if (Config::MAXIMISED_ENABLED)
		{
			taskbar.second = &Config::MAXIMISED_APPEARANCE;
		}

		if (Config::PEEK == Config::PEEK::Dynamic)
		{
			if (Config::PEEK_ONLY_MAIN)
			{
				if (taskbar.first == run.main_taskbar)
				{
					run.should_show_peek = true;
				}
			}
			else
			{
				run.should_show_peek = true;
			}
		}
	}
	return true;
}

void SetTaskbarBlur()
{
	static uint8_t counter = 10;

	if (counter >= 10)	// Change this if you want to change the time it takes for the program to update.
	{					// 1 = Config::SLEEP_TIME; we use 10 (assuming the default configuration value of 10),
						// because the difference is less noticeable and it has no large impact on CPU.
						// We can change this if we feel that CPU is more important than response time.
		run.should_show_peek = (Config::PEEK == Config::PEEK::Enabled);

		for (auto &taskbar : run.taskbars)
		{
			taskbar.second.second = &Config::REGULAR_APPEARANCE; // Reset taskbar state
		}
		if (Config::MAXIMISED_ENABLED || Config::PEEK == Config::PEEK::Dynamic)
		{
			EnumWindows(&EnumWindowsProcess, NULL);
		}

		TogglePeek(run.should_show_peek);

		const Window fg_window = Window::ForegroundWindow();
		if (fg_window != Window::NullWindow)
		{
			// TODO: test on multiple monitors
			if (Config::CORTANA_ENABLED && !fg_window.get_attribute<BOOL>(DWMWA_CLOAKED) &&
				Util::IgnoreCaseStringEquals(fg_window.filename(), L"SearchUI.exe"))
			{
				run.taskbars.at(fg_window.monitor()).second = &Config::CORTANA_APPEARANCE;
			}

			if (Config::START_ENABLED && run.start_opened)
			{
				run.taskbars.at(fg_window.monitor()).second = &Config::START_APPEARANCE;
			}
		}

		// Put this between Start/Cortana and Task view/Timeline
		// Task view and Timeline show over Aero Peek, but not Start or Cortana
		if (Config::MAXIMISED_ENABLED && Config::MAXIMISED_REGULAR_ON_PEEK && run.peek_active)
		{
			for (auto &taskbar : run.taskbars)
			{
				taskbar.second.second = &Config::REGULAR_APPEARANCE;
			}
		}

		if (fg_window != Window::NullWindow)
		{
			const static bool timeline_av = win32::IsAtLeastBuild(MIN_FLUENT_BUILD);
			if (Config::TIMELINE_ENABLED && (timeline_av
				? (fg_window.classname() == CORE_WINDOW && Util::IgnoreCaseStringEquals(fg_window.filename(), L"Explorer.exe"))
				: (fg_window.classname() == L"MultitaskingViewFrame")))
			{
				for (auto &taskbar : run.taskbars)
				{
					taskbar.second.second = &Config::TIMELINE_APPEARANCE;
				}
			}
		}

		counter = 0;
	}
	else
	{
		counter++;
	}

	for (const auto &taskbar : run.taskbars)
	{
		const Config::TASKBAR_APPEARANCE &appearance = *taskbar.second.second;
		SetWindowBlur(taskbar.second.first, appearance.ACCENT, appearance.COLOR);
	}
}

#pragma endregion

#pragma region Startup

void InitializeWindowsRuntime()
{
	static Microsoft::WRL::Wrappers::RoInitializeWrapper init(RO_INIT_SINGLETHREADED);
	ErrorHandle(init, Error::Level::Log, L"Initialization of Windows Runtime failed.");
}

void InitializeTray(const HINSTANCE &hInstance)
{
	static MessageWindow window(L"TrayWindow", NAME, hInstance);

	window.RegisterCallback(NEW_TTB_INSTANCE, [](...) {
		run.exit_reason = EXITREASON::NewInstance;
		run.is_running = false;
		return 0;
	});

	window.RegisterCallback(WM_DISPLAYCHANGE, [](...) {
		std::this_thread::sleep_for(std::chrono::seconds(10));	// Sleeping because the taskbar hasn't
																// been created yet when we get this.
																// 10 seconds gives enough time to even
																// the slowest of computers to create
																// the taskbar. (I hope)
		RefreshHandles();
		return 0;
	});

	window.RegisterCallback(WM_TASKBARCREATED, [](...) {
		RefreshHandles();
		return 0;
	});

	window.RegisterCallback(WM_CLOSE, [](...) {
		run.exit_reason = EXITREASON::UserAction;
		run.is_running = false;
		return 0;
	});

#ifdef STORE
	window.RegisterCallback(WM_QUERYENDSESSION, [](...) {
		// https://docs.microsoft.com/en-us/windows/uwp/porting/desktop-to-uwp-extensions#updates
		RegisterApplicationRestart(NULL, NULL);
		return TRUE;
	});
#endif


	if (!Config::NO_TRAY)
	{
		static TrayContextMenu tray(window, MAKEINTRESOURCE(TRAYICON), MAKEINTRESOURCE(IDR_POPUP_MENU), hInstance);

		tray.BindColor(IDM_REGULAR_COLOR, Config::REGULAR_APPEARANCE.COLOR);
		tray.BindEnum(Config::REGULAR_APPEARANCE.ACCENT, REGULAR_BUTTOM_MAP);


		tray.BindBool(IDM_MAXIMISED,      Config::MAXIMISED_ENABLED,         TrayContextMenu::Toggle);
		tray.BindBool(IDM_MAXIMISED_PEEK, Config::MAXIMISED_ENABLED,         TrayContextMenu::ControlsEnabled);
		tray.BindBool(IDM_MAXIMISED_PEEK, Config::MAXIMISED_REGULAR_ON_PEEK, TrayContextMenu::Toggle);
		tray.BindColor(IDM_MAXIMISED_COLOR, Config::MAXIMISED_APPEARANCE.COLOR);
		tray.BindEnum(Config::MAXIMISED_APPEARANCE.ACCENT, MAXIMISED_BUTTON_MAP);
		for (const auto &button_pair : MAXIMISED_BUTTON_MAP)
		{
			tray.BindBool(button_pair.second, Config::MAXIMISED_ENABLED, TrayContextMenu::ControlsEnabled);
		}


		tray.BindBool(IDM_START, Config::START_ENABLED, TrayContextMenu::Toggle);
		tray.BindColor(IDM_START_COLOR, Config::START_APPEARANCE.COLOR);
		tray.BindEnum(Config::START_APPEARANCE.ACCENT, START_BUTTON_MAP);
		for (const auto &button_pair : START_BUTTON_MAP)
		{
			tray.BindBool(button_pair.second, Config::START_ENABLED, TrayContextMenu::ControlsEnabled);
		}

		tray.BindBool(IDM_CORTANA, Config::CORTANA_ENABLED, TrayContextMenu::Toggle);
		tray.BindColor(IDM_CORTANA_COLOR, Config::CORTANA_APPEARANCE.COLOR);
		tray.BindEnum(Config::CORTANA_APPEARANCE.ACCENT, CORTANA_BUTTON_MAP);
		for (const auto &button_pair : CORTANA_BUTTON_MAP)
		{
			tray.BindBool(button_pair.second, Config::CORTANA_ENABLED, TrayContextMenu::ControlsEnabled);
		}


		tray.BindBool(IDM_TIMELINE, Config::TIMELINE_ENABLED, TrayContextMenu::Toggle);
		tray.BindColor(IDM_TIMELINE_COLOR, Config::TIMELINE_APPEARANCE.COLOR);
		tray.BindEnum(Config::TIMELINE_APPEARANCE.ACCENT, TIMELINE_BUTTON_MAP);
		for (const auto &button_pair : TIMELINE_BUTTON_MAP)
		{
			tray.BindBool(button_pair.second, Config::TIMELINE_ENABLED, TrayContextMenu::ControlsEnabled);
		}


		tray.BindEnum(Config::PEEK, PEEK_BUTTON_MAP);
		tray.BindBool(IDM_PEEK_ONLY_MAIN, Config::PEEK_ONLY_MAIN, TrayContextMenu::Toggle);


		tray.RegisterContextMenuCallback(IDM_OPENLOG, [] {
			std::thread([] {
				Log::Flush();
				win32::EditFile(Log::file());
			}).detach();
		});
		tray.BindBool(IDM_VERBOSE, Config::VERBOSE, TrayContextMenu::Toggle);
		tray.RegisterContextMenuCallback(IDM_RELOADSETTINGS, std::bind(&Config::Parse, std::ref(run.config_file)));
		tray.RegisterContextMenuCallback(IDM_EDITSETTINGS, [] {
			Config::Save(run.config_file);
			std::thread([] {
				win32::EditFile(run.config_file);
				Config::Parse(run.config_file);
			}).detach();
		});
		tray.RegisterContextMenuCallback(IDM_RETURNTODEFAULTSETTINGS, [] {
			ApplyStock(CONFIG_FILE);
			Config::Parse(run.config_file);
		});
		tray.RegisterContextMenuCallback(IDM_RELOADDYNAMICBLACKLIST, std::bind(&Blacklist::Parse, std::ref(run.exclude_file)));
		tray.RegisterContextMenuCallback(IDM_EDITDYNAMICBLACKLIST, [] {
			std::thread([] {
				win32::EditFile(run.exclude_file);
				Blacklist::Parse(run.exclude_file);
			}).detach();
		});
		tray.RegisterContextMenuCallback(IDM_RETURNTODEFAULTBLACKLIST, [] {
			ApplyStock(EXCLUDE_FILE);
			Blacklist::Parse(run.exclude_file);
		});
		tray.RegisterContextMenuCallback(IDM_CLEARBLACKLISTCACHE, Blacklist::ClearCache);
		tray.RegisterContextMenuCallback(IDM_EXITWITHOUTSAVING, [] {
			run.exit_reason = EXITREASON::UserActionNoSave;
			run.is_running = false;
		});


		tray.RegisterContextMenuCallback(IDM_AUTOSTART, [] {
			Autostart::SetStartupState(Autostart::GetStartupState() == Autostart::StartupState::Enabled ? Autostart::StartupState::Disabled : Autostart::StartupState::Enabled);
		});
		tray.RegisterContextMenuCallback(IDM_TIPS, std::bind(&win32::OpenLink,
			L"https://github.com/TranslucentTB/TranslucentTB/wiki/Tips-and-tricks-for-a-better-looking-taskbar"));
		tray.RegisterContextMenuCallback(IDM_EXIT, [] {
			run.exit_reason = EXITREASON::UserAction;
			run.is_running = false;
		});


		tray.RegisterCustomRefresh(RefreshMenu);
	}
}

int WINAPI wWinMain(const HINSTANCE hInstance, HINSTANCE, wchar_t *, int)
{
	win32::HardenProcess();

	// If there already is another instance running, tell it to exit
	if (!win32::IsSingleInstance())
	{
		Window::Find(L"TrayWindow", NAME).send_message(NEW_TTB_INSTANCE);
	}

	InitializeWindowsRuntime();

	// Get configuration file paths
	GetPaths();

	// If the configuration files don't exist, restore the files and show welcome to the users
	if (!CheckAndRunWelcome())
	{
		return EXIT_FAILURE;
	}

	// Parse our configuration
	Config::Parse(run.config_file);
	Blacklist::Parse(run.exclude_file);

	// Initialize GUI
	InitializeTray(hInstance);

	// Populate our map
	RefreshHandles();

	// Undoc'd, allows to detect when Aero Peek starts and stops
	EventHook peek_hook(
		0x21,
		0x22,
		[](const DWORD event, ...)
		{
			run.peek_active = event == 0x21;
		},
		WINEVENT_OUTOFCONTEXT
	);

	// Register our start menu detection sink
	ClassicComPtr<IAppVisibility> app_visibility(CLSID_AppVisibility);
	Microsoft::WRL::ComPtr<IAppVisibilityEvents> av_sink;
	DWORD av_cookie = 0;
	if (app_visibility.Get() != nullptr)
	{
		av_sink = Microsoft::WRL::Make<AppVisibilitySink>(run.start_opened);
		ErrorHandle(app_visibility->Advise(av_sink.Get(), &av_cookie), Error::Level::Log, L"Failed to register app visibility sink.");
	}

	// Message loop
	while (run.is_running)
	{
		MSG msg;
		if (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		SetTaskbarBlur();
		std::this_thread::sleep_for(std::chrono::milliseconds(Config::SLEEP_TIME));
	}

	if (av_cookie)
	{
		ErrorHandle(app_visibility->Unadvise(av_cookie), Error::Level::Log, L"Failed to unregister app visibility sink.");
	}

	// If it's a new instance, don't save or restore taskbar to default
	if (run.exit_reason != EXITREASON::NewInstance)
	{
		if (run.exit_reason != EXITREASON::UserActionNoSave)
		{
			Config::Save(run.config_file);
		}

		// Restore default taskbar appearance
		TogglePeek(true);
		for (const auto &taskbar : run.taskbars)
		{
			SetWindowBlur(taskbar.second.first, swca::ACCENT::ACCENT_NORMAL, NULL);
		}
	}

	return EXIT_SUCCESS;
}

#pragma endregion