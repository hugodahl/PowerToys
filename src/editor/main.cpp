#include "pch.h"
#include <Commdlg.h>
#include "StreamUriResolverFromFile.h"
#include <Shellapi.h>
#include <common/two_way_pipe_message_ipc.h>
#include <ShellScalingApi.h>
#include "resource.h"
#include <common/common.h>
#include <common/debug_trace.h>
#include <common/dpi_aware.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "windowsapp")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dcomp")
#pragma comment(lib, "dwmapi")

#ifdef _DEBUG
#define _DEBUG_WITH_LOCALHOST 0
// Define as 1 For debug purposes, to access localhost servers.
// webview_process_options.PrivateNetworkClientServerCapability(winrt::Windows::Web::UI::Interop::WebViewControlProcessCapabilityState::Enabled);
// To access localhost:8080 for development, you'll also need to disable loopback restrictions for the webview:
// > checknetisolation LoopbackExempt -a -n=Microsoft.Win32WebViewHost_cw5n1h2txyewy
// To remove the exception after development:
// > checknetisolation LoopbackExempt -d -n=Microsoft.Win32WebViewHost_cw5n1h2txyewy
// Source: https://github.com/windows-toolkit/WindowsCommunityToolkit/issues/2226#issuecomment-396360314
#endif
HINSTANCE m_hInst;
HWND main_window_handler = nullptr;
using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Headers;
using namespace winrt::Windows::Web::UI;
using namespace winrt::Windows::Web::UI::Interop;
using namespace winrt::Windows::System;

winrt::Windows::Web::UI::Interop::WebViewControl webview_control = nullptr;
winrt::Windows::Web::UI::Interop::WebViewControlProcess webview_process = nullptr;
winrt::Windows::Web::UI::Interop::WebViewControlProcessOptions webview_process_options = nullptr;
StreamUriResolverFromFile local_uri_resolver;

// Windows message for receiving copied data to send to the webview.
UINT wm_copydata_webview = 0;

// Windows message to destroy the window. Used if:
// - Parent process has terminated.
// - WebView confirms that the Window can close.
UINT wm_my_destroy_window = 0;

// mutex for checking if the window has already been created.
std::mutex m_window_created_mutex;

TwoWayPipeMessageIPC* current_settings_ipc = NULL;

// Set to true if waiting for webview confirmation before closing the Window.
bool m_waiting_for_close_confirmation = false;

#if defined(_DEBUG) && _DEBUG_WITH_LOCALHOST
void NavigateToLocalhostReactServer() {
  // Useful for connecting to instance running in react development server.
  webview_control.Navigate(Uri(hstring(L"http://localhost:8080")));
}
#endif

void NavigateToUri(_In_ LPCWSTR uri_as_string) {
  trace("invoked");
  Uri url = webview_control.BuildLocalStreamUri(hstring(L"settings-html"), hstring(uri_as_string));
  trace("step 2");
  webview_control.NavigateToLocalStreamUri(url, local_uri_resolver);
}

Rect hwnd_client_rect_to_bounds_rect(_In_ HWND hwnd) {
  RECT client_rect = { 0 };
  WINRT_VERIFY(GetClientRect(hwnd, &client_rect));

  Rect bounds =
  {
    0,
    0,
    static_cast<float>(client_rect.right - client_rect.left),
    static_cast<float>(client_rect.bottom - client_rect.top)
  };

  return bounds;
}

void resize_web_view() {
  trace("step 1");
  Rect bounds = hwnd_client_rect_to_bounds_rect(main_window_handler);
  trace("step 2");
  winrt::Windows::Web::UI::Interop::IWebViewControlSite webViewControlSite = (winrt::Windows::Web::UI::Interop::IWebViewControlSite) webview_control;
  trace("step 3");
  webViewControlSite.Bounds(bounds);
}

#define SEND_TO_WEBVIEW_MSG 1

void send_message_to_webview(const std::wstring& msg) {
  if (main_window_handler != NULL && wm_copydata_webview!=0) {
    trace("invoked");
    // Allocate the COPYDATASTRUCT and message to pass to the Webview.
    // This is needed in order to use PostMessage, since COM calls to
    // webview_control.InvokeScriptAsync can't be made from 
    PCOPYDATASTRUCT copy_data_message = new COPYDATASTRUCT();
    const wchar_t* orig_msg = msg.c_str();
    DWORD orig_len = (DWORD)wcslen(orig_msg);
    wchar_t* copy_msg = new wchar_t[orig_len + 1];
    wcscpy_s(copy_msg, orig_len + 1, orig_msg);
    copy_data_message->dwData = SEND_TO_WEBVIEW_MSG;
    copy_data_message->cbData = (orig_len + 1) * sizeof(wchar_t);
    copy_data_message->lpData = (PVOID)copy_msg;
    WINRT_VERIFY(PostMessage(main_window_handler, wm_copydata_webview, (WPARAM)main_window_handler, (LPARAM)copy_data_message));
    // wnd_static_proc will be responsible for freeing these.
  }
}

void send_message_to_powertoys(const std::wstring& msg) {
  if (current_settings_ipc != NULL) {
    trace("invoked");
    current_settings_ipc->send(msg);
  } else {
    // For Debug purposes, in case the webview is being run alone.
#ifdef _DEBUG
    MessageBox(main_window_handler, msg.c_str(), L"From Webview", MB_OK);
    // Debug sample data
    std::wstring debug_settings_info(LR"json({
            "general": {
              "startup": true,
              "enabled": {
                "Shortcut Guide":false,
                "Example PowerToy":true
              }
            },
            "powertoys": {
              "Shortcut Guide": {
                "version": "1.0",
                "name": "Shortcut Guide",
                "description": "Shows a help overlay with Windows shortcuts when the Windows key is pressed.",
                "icon_key": "pt-shortcut-guide",
                "properties": {
                  "press time" : {
                    "display_name": "How long to press the Windows key before showing the Shortcut Guide (ms)",
                    "editor_type": "int_spinner",
                    "value": 300
                  }
                }
              },
              "Example PowerToy": {
                "version": "1.0",
                "name": "Example PowerToy",
                "description": "Shows the different controls for the settings.",
                "overview_link": "https://github.com/microsoft/PowerToys",
                "video_link": "https://www.youtube.com/watch?v=d3LHo2yXKoY&t=21462",
                "properties": {
                  "test bool_toggle": {
                    "display_name": "This is what a bool_toggle looks like",
                    "editor_type": "bool_toggle",
                    "value": false
                  },
                  "test int_spinner": {
                    "display_name": "This is what a int_spinner looks like",
                    "editor_type": "int_spinner",
                    "value": 10
                  },
                  "test string_text": {
                    "display_name": "This is what a string_text looks like",
                    "editor_type": "string_text",
                    "value": "A sample string value"
                  },
                  "test color_picker": {
                    "display_name": "This is what a color_picker looks like",
                    "editor_type": "color_picker",
                    "value": "#0450fd"
                  },
                  "test custom_action": {
                    "display_name": "This is what a custom_action looks like",
                    "editor_type": "custom_action",
                    "value": "This is to be custom data. It\ncan\nhave\nmany\nlines\nthat\nshould\nmake\nthe\nfield\nbigger.",
                    "button_text": "Call a Custom Action!"
                  }
                }
              }
            }
          })json");
    send_message_to_webview(debug_settings_info);
#endif
  }
}

void receive_message_from_webview(const std::wstring& msg) {
  if (msg[0] == '{') {
    trace("JSON");
    // It's a JSON, send message to PowerToys
    std::thread(send_message_to_powertoys, msg).detach();
  } else {
    trace("COMMAND");
    // It's not a JSON, check for expected control messages.
    if (msg == L"exit") {
      // WebView confirms the settings application can exit.
      WINRT_VERIFY(PostMessage(main_window_handler, wm_my_destroy_window, 0, 0));
    } else if (msg == L"cancel-exit") {
      // WebView canceled the exit request.
      m_waiting_for_close_confirmation = false;
    }
  }
}

void initialize_win32_webview(HWND hwnd, int nCmdShow) {
  trace("invoked");
  // initialize the base_path for the html content relative to the executable.
  WCHAR executable_path[MAX_PATH];
  WINRT_VERIFY(GetModuleFileName(NULL, executable_path, MAX_PATH));
  WINRT_VERIFY(PathRemoveFileSpec(executable_path));
  wcscat_s(executable_path, L"\\settings-html");
  wcscpy_s(local_uri_resolver.base_path, executable_path);
  
  try {
    if (!webview_process_options) {
      trace("webview_process_options");
      webview_process_options = winrt::Windows::Web::UI::Interop::WebViewControlProcessOptions();
      WINRT_VERIFY(webview_process_options);
    }

    if (!webview_process) {
      trace("webview_process");
      webview_process = winrt::Windows::Web::UI::Interop::WebViewControlProcess(webview_process_options);
      WINRT_VERIFY(webview_process);
    }
    auto asyncwebview = webview_process.CreateWebViewControlAsync((int64_t)main_window_handler, hwnd_client_rect_to_bounds_rect(main_window_handler));
    WINRT_VERIFY(asyncwebview);
    asyncwebview.Completed([=](IAsyncOperation<WebViewControl> const& sender, AsyncStatus args) {
      trace("asyncwebview.Completed");
      webview_control = sender.GetResults();
      WINRT_VERIFY(webview_control);
      trace("asyncwebview.Completed step 1");
      // In order to receive window.external.notify() calls in ScriptNotify
      webview_control.Settings().IsScriptNotifyAllowed(true);
      trace("asyncwebview.Completed step 2");
      webview_control.Settings().IsJavaScriptEnabled(true);
      
      trace("asyncwebview.Completed step 3");
      webview_control.NewWindowRequested([=](IWebViewControl sender_requester, WebViewControlNewWindowRequestedEventArgs args ) {
        trace("webview_control.NewWindowRequested invoked");
        // Open the requested link in the default browser registered in the Shell
        int res = (int)ShellExecute(NULL, L"open", args.Uri().AbsoluteUri().c_str(), NULL, NULL, SW_SHOWNORMAL);
        WINRT_VERIFY(res > 32);
      });
      
      trace("asyncwebview.Completed step 4");
      webview_control.DOMContentLoaded([=](IWebViewControl sender_loaded, WebViewControlDOMContentLoadedEventArgs const& args_loaded) {
        trace("webview_control.DOMContentLoaded invoked");
        // runs when the content has been loaded.
      });

      trace("asyncwebview.Completed step 5");
      webview_control.ScriptNotify([=](IWebViewControl sender_script_notify, WebViewControlScriptNotifyEventArgs const& args_script_notify) {
        trace("webview_control.ScriptNotify invoked");
        // content called window.external.notify()
        std::wstring message_sent = args_script_notify.Value().c_str();
        receive_message_from_webview(message_sent);
      });

      trace("asyncwebview.Completed step 6");
      webview_control.AcceleratorKeyPressed([&](IWebViewControl sender, WebViewControlAcceleratorKeyPressedEventArgs const& args) {
        if (args.VirtualKey() == winrt::Windows::System::VirtualKey::F4) {
          trace("webview_control.AcceleratorKeyPressed invoked");
          // WebView swallows key-events. Detect Alt-F4 one and close the window manually.
          webview_control.InvokeScriptAsync(hstring(L"exit_settings_app"), {});
        }
      });

      resize_web_view();
#if defined(_DEBUG) && _DEBUG_WITH_LOCALHOST
      // navigates to localhost:8080
      //NavigateToLocalhostReactServer();
#else
      // Navigates to settings-html/index.html
      trace("asyncwebview.Completed step 7");
      BOOL result = ShowWindow(main_window_handler, nCmdShow);
      trace_bool(result, "ShowWindow");
      trace("asyncwebview.Completed step 8");
      NavigateToUri(L"index.html");
#endif
    });
  }
  catch (hresult_error const& e) {
    WCHAR message[1024] = L"";
    StringCchPrintf(message, ARRAYSIZE(message), L"failed: %ls", e.message().c_str());
    MessageBox(main_window_handler, message, L"Error", MB_OK);
  }
}

LRESULT CALLBACK wnd_proc_static(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
  case WM_CLOSE:
    trace("WM_CLOSE");
    if (m_waiting_for_close_confirmation) {
      // If another WM_CLOSE is received while waiting for webview confirmation,
      // allow DefWindowProc to be called and destroy the window.
      break;
    } else {
      // Allow user to confirm exit in the WebView in case there's possible data loss.
      m_waiting_for_close_confirmation = true;
      if (webview_control != NULL) {
        webview_control.InvokeScriptAsync(hstring(L"exit_settings_app"), {});
      } else {
        break;
      }
      return 0;
    }
  case WM_DESTROY:
    trace("WM_DESTROY");
    PostQuitMessage(0);
    break;
  case WM_SIZE:
    trace("WM_SIZE");
    if (webview_control != nullptr) {
      resize_web_view();
    }
    break;
  case WM_CREATE:
    trace("WM_CREATE");
    wm_copydata_webview = RegisterWindowMessageW(L"PTSettingsCopyDataWebView");
    wm_my_destroy_window = RegisterWindowMessageW(L"PTSettingsParentTerminated");
    m_window_created_mutex.unlock();
    break;
  case WM_DPICHANGED:
    {
      trace("WM_DPICHANGED");
      // Resize the window using the suggested rect
      RECT* const prcNewWindow = (RECT*)lParam;
      SetWindowPos(hWnd,
        NULL,
        prcNewWindow->left,
        prcNewWindow->top,
        prcNewWindow->right - prcNewWindow->left,
        prcNewWindow->bottom - prcNewWindow->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;
  case WM_NCCREATE:
    {
      trace("WM_NCCREATE");
      // Enable auto-resizing the title bar
      EnableNonClientDpiScaling(hWnd);
    }
    break;
  default:
    if (message == wm_copydata_webview) {
      trace("wm_copydata_webview");
      PCOPYDATASTRUCT msg = (PCOPYDATASTRUCT)lParam;
      if (msg->dwData == SEND_TO_WEBVIEW_MSG) {
        wchar_t* json_message = (wchar_t*)(msg->lpData);
        if (webview_control != NULL) {
          webview_control.InvokeScriptAsync(hstring(L"receive_from_settings_app"), { hstring(json_message) });
        }
        delete[] json_message;
      }
      // wnd_proc_static is responsible for freeing memory.
      delete msg;
    } else {
      if (message == wm_my_destroy_window) {
        trace("wm_my_destroy_window");
        DestroyWindow(hWnd);
      }
    }
    break;
  }
  return DefWindowProc(hWnd, message, wParam, lParam);;
}

void register_classes(HINSTANCE hInstance) {
  trace("invoked");
  WNDCLASSEXW wcex;
  wcex.cbSize = sizeof(WNDCLASSEX);

  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = wnd_proc_static;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(APPICON));
  wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wcex.lpszMenuName = nullptr;
  wcex.lpszClassName = L"PTSettingsClass";
  wcex.hIconSm = nullptr;

  WINRT_VERIFY(RegisterClassExW(&wcex));
}

int init_instance(HINSTANCE hInstance, int nCmdShow) {
  trace("invoked");
  m_hInst = hInstance;

  RECT desktopRect;
  const HWND hDesktop = GetDesktopWindow();
  WINRT_VERIFY(hDesktop);
  WINRT_VERIFY(GetWindowRect(hDesktop, &desktopRect));

  int wind_width = 1024;
  int wind_height = 700;
  DPIAware::Convert(NULL, wind_width, wind_height);
  
  main_window_handler = CreateWindowW(
    L"PTSettingsClass",
    L"PowerToys Settings",
    WS_OVERLAPPEDWINDOW,
    (desktopRect.right - wind_width)/2,
    (desktopRect.bottom - wind_height)/2,
    wind_width,
    wind_height,
    nullptr,
    nullptr,
    hInstance,
    nullptr);

  WINRT_VERIFY(main_window_handler);
  initialize_win32_webview(main_window_handler, nCmdShow);
  WINRT_VERIFY(UpdateWindow(main_window_handler));

  return TRUE;
}

void wait_on_parent_process_thread(DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  WINRT_VERIFY(process);
  if (process != NULL) {
    if (WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0) {
      // If it's possible to detect when the PowerToys process terminates, message the main window.
      CloseHandle(process);
      {
        // Send a terminated message only after the window has finished initializing.
        std::unique_lock lock(m_window_created_mutex);
      }
      WINRT_VERIFY(PostMessage(main_window_handler, wm_my_destroy_window, 0, 0));
    } else {
      CloseHandle(process);
    }
  }
}

void quit_when_parent_terminates(std::wstring parent_pid) {
  DWORD pid = std::stol(parent_pid);
  std::thread(wait_on_parent_process_thread,pid).detach();
}

void read_arguments() {
  // Expected calling arguments:
  // [0] - This executable's path.
  // [1] - PowerToys pipe server.
  // [2] - Settings pipe server.
  // [3] - PowerToys process pid.
  LPWSTR *argument_list;
  int n_args;

  argument_list = CommandLineToArgvW(GetCommandLineW(), &n_args);
  if (n_args > 3) {
    trace("args ok");
    current_settings_ipc = new TwoWayPipeMessageIPC(std::wstring(argument_list[2]), std::wstring(argument_list[1]), send_message_to_webview);
    current_settings_ipc->start(NULL);
    quit_when_parent_terminates(std::wstring(argument_list[3]));
  } else {
    trace("args missing");
#ifndef _DEBUG
    MessageBox(NULL, L"This executable isn't supposed to be called as a stand-alone process", L"PowerToys Settings Error", MB_OK);
    exit(1);
#endif
  }
  LocalFree(argument_list);
}

int start_webview_window(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
  trace("invoked");
  // To be unlocked after the Window has finished being created.
  m_window_created_mutex.lock();
  read_arguments();
  register_classes(hInstance);
  init_instance(hInstance, nCmdShow);
  MSG msg;
  // Main message loop.
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return (int)msg.wParam;
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
  init_debug_trace_settings();
  
  if (is_process_elevated()) {
    trace("process is elevated");
    drop_elevated_privileges();
  } else {
    trace("process is not elevated");
  }
  
  WINRT_VERIFY_(S_OK,CoInitialize(nullptr));
  return start_webview_window(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}
