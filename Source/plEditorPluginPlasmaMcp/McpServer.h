#pragma once

#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpDLL.h>

#include <plEditorPluginPlasmaMcp/McpSocket.h>

#include <EditorUi/Runtime.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Variant.h>

/// A minimal MCP (Model Context Protocol) server, speaking the 'streamable HTTP' transport.
///
/// The sockets are polled on the main thread by a timer of the editor UI runtime, so tool calls run on the main
/// thread, between frames.
class plMcpServer
{
public:
  plMcpServer();
  ~plMcpServer();

  /// Starts listening on 127.0.0.1 at the given port.
  plResult Start(plUInt16 uiPort);

  /// Stops the server and drops all pending connections. Safe to call when not running.
  void Stop();

  bool IsRunning() const { return m_Listener.IsListening(); }

  /// The port that is currently being listened on, or 0 while not running.
  plUInt16 GetPort() const { return m_Listener.GetPort(); }

  /// The running server, or nullptr if none was started. Exists so that tools can report how to
  /// reach this editor, which is not otherwise discoverable from inside a tool call.
  static plMcpServer* GetInstance() { return s_pInstance; }

private:
  /// Accepts new connections, reads, handles complete requests and sends what is queued.
  void Poll();

  /// False once the connection is done and can go.
  bool UpdateConnection(plMcpConnection& ref_connection);

  void RemoveClosedConnections();

  /// Tries to handle one complete request from the accumulated bytes. Does nothing while the
  /// request is still incomplete, because a single TCP read is not guaranteed to contain all of it.
  void TryHandleRequest(plMcpConnection& ref_connection);

  /// Turns one JSON-RPC message into a response body. Returns PL_FAILURE for notifications,
  /// which must not be answered with a body at all.
  plResult HandleMessage(const plVariantDictionary& msg, plStringBuilder& out_sResponse);

  void SendResponse(plMcpConnection& ref_connection, plStringView sStatusLine, plStringView sBody);

  plMcpListener m_Listener;
  plDynamicArray<plUniquePtr<plMcpConnection>> m_Connections;

  plEditorUiTimerGroup m_Timers;
  plUInt32 m_uiPollTimer = 0;

  /// A tool call that runs a modal loop polls again from inside, so other requests are answered meanwhile. Only the
  /// outermost poll removes connections, so the array stays as the outer ones walk it.
  plUInt32 m_uiPollDepth = 0;

  /// The connections whose request a tool handles right now; nested polls leave them alone.
  plHybridArray<plMcpConnection*, 2> m_Handling;

  /// Stop was called while polling (a tool closed the project): the connections go once the poll is done.
  bool m_bDropConnections = false;

  static plMcpServer* s_pInstance;
};
