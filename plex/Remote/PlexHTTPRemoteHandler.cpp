//
//  PlexHTTPRemoteHandler.cpp
//  Plex Home Theater
//
//  Created by Tobias Hieta on 2013-08-16.
//
//

#include "PlexHTTPRemoteHandler.h"
#include "network/WebServer.h"
#include <boost/algorithm/string.hpp>

#include "Client/PlexServerManager.h"
#include "FileSystem/PlexDirectory.h"
#include "ApplicationMessenger.h"
#include "Application.h"
#include "GUIUserMessages.h"
#include "guilib/GUIWindowManager.h"
#include "dialogs/GUIDialogKeyboardGeneric.h"

#include "utils/log.h"
#include "interfaces/Builtins.h"
#include "PlexRemoteSubscriberManager.h"

#include <boost/asio/detail/socket_ops.hpp>

////////////////////////////////////////////////////////////////////////////////////////
bool CPlexHTTPRemoteHandler::CheckHTTPRequest(const HTTPRequest &request)
{
  return boost::starts_with(request.url, "/player");
}

////////////////////////////////////////////////////////////////////////////////////////
int CPlexHTTPRemoteHandler::HandleHTTPRequest(const HTTPRequest &request)
{
  /* defaults */
  m_responseType = HTTPMemoryDownloadNoFreeCopy;
  m_responseCode = MHD_HTTP_OK;
  
  ArgMap argumentMap;
  CStdString path(request.url);
  
  CWebServer::GetRequestHeaderValues(request.connection, MHD_GET_ARGUMENT_KIND, argumentMap);
  
  CLog::Log(LOGDEBUG, "CPlexHTTPRemoteHandler::HandleHTTPRequest handling %s", request.url.c_str());
  
  if (path.Equals("/player/application/playMedia"))
    playMedia(argumentMap);
  else if (path.Equals("/player/application/sendString"))
    sendString(argumentMap);
  else if (path.Equals("/player/application/sendVirtualKey") ||
           path.Equals("/player/application/sendKey"))
    sendVKey(argumentMap);
  else if (path.Equals("/player/playback/bigStepForward") ||
           path.Equals("/player/playback/bigStepBackward") ||
           path.Equals("/player/playback/stepForward") ||
           path.Equals("/player/playback/stepBackward"))
    stepFunction(request.url, argumentMap);
  else if (path.Equals("/player/playback/skipNext"))
    skipNext(argumentMap);
  else if (path.Equals("/player/playback/skipPrevious"))
    skipPrevious(argumentMap);
  else if (path.Equals("/player/playback/pause"))
    pausePlay(argumentMap);
  else if (path.Equals("/player/playback/play"))
    pausePlay(argumentMap);
  else if (path.Equals("/player/playback/stop"))
    stop(argumentMap);
  else if (path.Equals("/player/playback/seekTo"))
    seekTo(argumentMap);
  else if (path.Equals("/player/application/setVolume"))
    setVolume(argumentMap);
  else if (path.Equals("/player/set"))
    set(argumentMap);
  else if (boost::starts_with(path, "/player/navigation"))
    navigation(path, argumentMap);
  else if (path.Equals("/player/subscribe"))
    subscribe(request, argumentMap);
  else if (path.Equals("/player/unsubscribe"))
    unsubscribe(request, argumentMap);
  else
  {
    m_responseCode = MHD_HTTP_NOT_IMPLEMENTED;
    m_responseType = HTTPError;
  }
  
  return MHD_YES;
}

////////////////////////////////////////////////////////////////////////////////////////
void* CPlexHTTPRemoteHandler::GetHTTPResponseData() const
{
  return (void*)m_data.c_str();
}

////////////////////////////////////////////////////////////////////////////////////////
size_t CPlexHTTPRemoteHandler::GetHTTPResonseDataLength() const
{
  return m_data.size();
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::playMedia(const ArgMap &arguments)
{
  CPlexServerPtr server;
  std::string containerPath;
  
  /* Protocol v2 allows for sending machineIndentifier. */
  if (arguments.find("machineIdentifier") != arguments.end())
  {
    std::string uuid = arguments.find("machineIdentifier")->second;
    server = g_plexServerManager.FindByUUID(uuid);
    if (arguments.find("containerKey") != arguments.end())
      containerPath = arguments.find("containerKey")->second;
  }
  else if (arguments.find("path") != arguments.end())
  {
    CURL serverURL(arguments.find("path")->second);
    if (!PlexUtils::IsValidIP(serverURL.GetHostName()))
    {
      CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia got a path, but it's not a valid IP.");
      return;
    }
    /* look up via host and port, this is not super reliable, but it will probably work
     * most of the time */
    server = g_plexServerManager.FindByHostAndPort(serverURL.GetHostName(), serverURL.GetPort());
    containerPath = serverURL.GetFileName();
  }
  else
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia Didn't find machineIdentifier nor path in the request. Can't know what to play?");
    return;
  }
  
  if (!server || containerPath.empty())
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia can't find server to play from");
    return;
  }
  
  CURL itemURL = server->BuildPlexURL(containerPath);
  
  std::string key;
  
  if (arguments.find("key") != arguments.end())
  {
    key = arguments.find("key")->second;
  }
  else
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia no key attribute, can't progress...");
    return;
  }
  
  /* fetch the container */
  CFileItemList list;
  XFILE::CPlexDirectory dir;
  if (!dir.GetDirectory(itemURL.Get(), list))
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia can't fetch container %s", itemURL.Get().c_str());
    return;
  }
  
  CFileItemPtr item;
  int idx;
  for (int i = 0; i < list.Size(); i ++)
  {
    CFileItemPtr it = list.Get(i);
    if (it->HasProperty("unprocessed_key") &&
        it->GetProperty("unprocessed_key") == key)
    {
      item = it;
      idx = i;
      break;
    }
  }
  
  if (!item)
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::playMedia couldn't find %s in %s", key.c_str(), itemURL.Get().c_str());
    return;
  }

  if (arguments.find("viewOffset") != arguments.end())
  {
    item->SetProperty("viewOffset", arguments.find("viewOffset")->second);
    item->m_lStartOffset = STARTOFFSET_RESUME;
  }
  
  
  if (item->GetPlexDirectoryType() == PLEX_DIR_TYPE_TRACK)
  {
    if (g_application.IsPlaying())
      CApplicationMessenger::Get().MediaStop();
    
    g_playlistPlayer.ClearPlaylist(PLAYLIST_MUSIC);
    g_playlistPlayer.Reset();
    g_playlistPlayer.Add(PLAYLIST_MUSIC, list);
    g_playlistPlayer.SetCurrentPlaylist(PLAYLIST_MUSIC);
    g_playlistPlayer.Play(idx);
    
  }
  else if (item->GetPlexDirectoryType() == PLEX_DIR_TYPE_PHOTO)
  {
    /* if we are playing music, we don't need to stop */
    if (g_application.IsPlayingVideo())
      CApplicationMessenger::Get().MediaStop();

    CGUIMessage msg(GUI_MSG_START_SLIDESHOW, 0, 0, false, idx);
    msg.SetStringParam(itemURL.Get());
    
    CGUIWindow* pWindow = g_windowManager.GetWindow(WINDOW_SLIDESHOW);
    if (pWindow)
      pWindow->OnMessage(msg);
  }
  else
  {
    if (g_application.IsPlaying())
      CApplicationMessenger::Get().MediaStop();
    
    CApplicationMessenger::Get().PlayFile(*item);
  }
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::stepFunction(const CStdString &url, const ArgMap &arguments)
{
  
  if (!g_application.IsPlaying())
    return;
  
  if (url.Equals("/player/playback/bigStepForward"))
    CBuiltins::Execute("playercontrol(bigskipforward)");
  else if (url.Equals("/player/playback/bigStepBackwards"))
    CBuiltins::Execute("playercontrol(bigskipbackward)");
  else if (url.Equals("/player/playback/stepForward"))
    CBuiltins::Execute("playercontrol(smallskipforward)");
  else if (url.Equals("/player/playback/stepBackward"))
    CBuiltins::Execute("playercontrol(smallskipbackward)");
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::skipNext(const ArgMap &arguments)
{
  if (g_application.IsPlaying())
    CBuiltins::Execute("playercontrol(next)");
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::skipPrevious(const ArgMap &arguments)
{
  if (g_application.IsPlaying())
    CBuiltins::Execute("playercontrol(previous)");
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::pausePlay(const ArgMap &arguments)
{
  CApplicationMessenger::Get().MediaPause();
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::stop(const ArgMap &arguments)
{
  CApplicationMessenger::Get().MediaStop();
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::seekTo(const ArgMap &arguments)
{
  int64_t seekTo;
  
  if (arguments.find("offset") != arguments.end())
  {
    try {
      seekTo = boost::lexical_cast<int64_t>(arguments.find("offset")->second);
    } catch (...) {
      CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::seekTo failed to convert offset into a int64_t");
      return;
    }
  }
  else
    return;
  
  if (g_application.IsPlaying() && g_application.m_pPlayer)
    g_application.m_pPlayer->SeekTime(seekTo);
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::navigation(const CStdString &url, const ArgMap &arguments)
{
  int action = ACTION_NONE;
  
  CStdString navigation = url.Mid(19, url.length() - 19);
  
  if (navigation.Equals("moveRight"))
    action = ACTION_MOVE_RIGHT;
  else if (navigation.Equals("moveLeft"))
    action = ACTION_MOVE_LEFT;
  else if (navigation.Equals("moveDown"))
    action = ACTION_MOVE_DOWN;
  else if (navigation.Equals("moveUp"))
    action = ACTION_MOVE_UP;
  else if (navigation.Equals("select"))
    action = ACTION_SELECT_ITEM;
  else if (navigation.Equals("back"))
    action = ACTION_NAV_BACK;
  else if (navigation.Equals("contextMenu"))
    action = ACTION_CONTEXT_MENU;
  else if (navigation.Equals("toggleOSD"))
    action = ACTION_SHOW_OSD;
  else if (navigation.Equals("pageUp"))
    action = ACTION_PAGE_UP;
  else if (navigation.Equals("pageDown"))
    action = ACTION_PAGE_DOWN;
  else if (navigation.Equals("nextLetter"))
    action = ACTION_NEXT_LETTER;
  else if (navigation.Equals("previousLetter"))
    action = ACTION_PREV_LETTER;
  
  if (action != ACTION_NONE)
  {
    g_application.WakeUpScreenSaverAndDPMS();
    CApplicationMessenger::Get().SendAction(CAction(action), g_windowManager.GetFocusedWindow(), false);
  }
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::set(const ArgMap &arguments)
{
  if (arguments.find("volume") != arguments.end())
  {
    ArgMap vmap;
    vmap["level"] = arguments.find("volume")->second;
    setVolume(vmap);
  }
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::setVolume(const ArgMap &arguments)
{
  int level;
  
  if (arguments.find("level") == arguments.end())
    return;
  
  level = boost::lexical_cast<int>(arguments.find("level")->second);
  
  int oldVolume = g_application.GetVolume();
  g_application.SetVolume((float)level, true);
  CApplicationMessenger::Get().ShowVolumeBar(oldVolume < level);
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::sendString(const ArgMap &arguments)
{
  if (arguments.find("text") == arguments.end())
    return;
  
  /* this is a bit kludgy, but I couldn't figure out how to get the current text any other way
   * TODO: how does this work with a numeric input? */
  CGUIDialogKeyboardGeneric *keyb = dynamic_cast<CGUIDialogKeyboardGeneric*>(g_windowManager.GetWindow(g_windowManager.GetFocusedWindow()));
  
  if (!keyb)
    return;

  g_application.WakeUpScreenSaverAndDPMS();
  
  CStdString newString = keyb->GetText() + arguments.find("text")->second.c_str();
  
  /* instead of calling keyb->SetText() we want to send this as a message
   * to avoid any thread locking and contention */
  CGUIMessage msg(GUI_MSG_SET_TEXT, 0, 0);
  msg.SetLabel(newString);
  msg.SetParam1(0);
  
  CApplicationMessenger::Get().SendGUIMessage(msg, g_windowManager.GetFocusedWindow());
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::sendVKey(const ArgMap &arguments)
{
  if (arguments.find("code") == arguments.end())
    return;
  
  int code = boost::lexical_cast<int>(arguments.find("code")->second);
  CLog::Log(LOGDEBUG, "CPlexHTTPRemoteHandler::sendVKey got code %d", code);
  int action;
  
  switch (code)
  {
    case 8:
      action = ACTION_BACKSPACE;
      break;
    case 13:
      action = ACTION_SELECT_ITEM;
      break;
    default:
      action = ACTION_NONE;
  }
  
  if (action != ACTION_NONE)
  {
    g_application.WakeUpScreenSaverAndDPMS();
    CApplicationMessenger::Get().SendAction(CAction(action), g_windowManager.GetFocusedWindow(), false);
  }
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::subscribe(const HTTPRequest &request, const ArgMap &arguments)
{
  g_plexRemoteSubscriberManager.addSubscriber(getSubFromRequest(request, arguments));
}

////////////////////////////////////////////////////////////////////////////////////////
void CPlexHTTPRemoteHandler::unsubscribe(const HTTPRequest &request, const ArgMap &arguments)
{
  g_plexRemoteSubscriberManager.removeSubscriber(getSubFromRequest(request, arguments));
}

////////////////////////////////////////////////////////////////////////////////////////
CPlexRemoteSubscriberPtr CPlexHTTPRemoteHandler::getSubFromRequest(const HTTPRequest &request, const ArgMap &arguments)
{
  std::string uuid, ipaddress;
  
  uuid = CWebServer::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, "X-Plex-Client-Identifier");
  if (uuid.empty())
  {
    CLog::Log(LOGWARNING, "CPlexHTTPRemoteHandler::subscribe subscriber didn't set X-Plex-Client-Identifier");
    return CPlexRemoteSubscriberPtr();
  }
  
  struct sockaddr_in *so = MHD_get_connection_info(request.connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr;
  if (!so)
    return CPlexRemoteSubscriberPtr();
  
  char ipstr[INET_ADDRSTRLEN];
  boost::system::error_code ec;
  boost::asio::detail::socket_ops::inet_ntop(AF_INET, &(so->sin_addr), ipstr, INET_ADDRSTRLEN, 0, ec);
  
  int port = 32400;
  if (arguments.find("port") != arguments.end())
    port = boost::lexical_cast<int>(arguments.find("port")->second);
  
  CPlexRemoteSubscriberPtr sub = CPlexRemoteSubscriber::NewSubscriber(uuid, ipstr, port);
  
  return sub;
}