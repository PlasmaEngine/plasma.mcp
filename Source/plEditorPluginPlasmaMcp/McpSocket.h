#pragma once

#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/Types/UniquePtr.h>

/// \brief One accepted TCP connection of the MCP server. The socket never blocks: the server polls it on the main
/// thread, reads what arrived and sends what the socket takes.
///
/// What is written goes into a queue, followed by a file when one is streamed (the GPU dump viewer's files), which is
/// read in chunks while the socket takes them, so a slow client never holds more than a chunk in memory.
class plMcpConnection
{
  PL_DISALLOW_COPY_AND_ASSIGN(plMcpConnection);

public:
  explicit plMcpConnection(plUInt64 uiSocket);
  ~plMcpConnection();

  /// Appends what arrived to GetReceived(). False when the client closed the connection or it failed.
  bool Receive();

  plDynamicArray<plUInt8>& GetReceived() { return m_Received; }

  void Write(plStringView sText);
  void Write(plArrayPtr<const plUInt8> data);

  /// Streams the file once the written bytes are sent (the response header, written before or after this call).
  /// Fails when the file cannot be opened.
  plResult WriteFile(plStringView sPath, plUInt64& out_uiFileSize);

  /// The connection closes once everything queued went out.
  void CloseWhenSent();
  bool IsCloseRequested() const { return m_bCloseWhenSent; }

  /// Sends as much of the queue as the socket takes. False once the connection is done: closed after sending
  /// everything, or failed.
  bool Flush();

  /// Closes the connection now; what is still queued is dropped.
  void Close();
  bool IsOpen() const { return m_bOpen; }

private:
  bool RefillFromFile();

  plUInt64 m_uiSocket = 0;
  bool m_bOpen = true;
  bool m_bCloseWhenSent = false;

  plDynamicArray<plUInt8> m_Received;
  plDynamicArray<plUInt8> m_Outgoing;
  plUInt32 m_uiSent = 0; ///< how much of m_Outgoing went out

  plOSFile m_File; ///< the streamed file while it is open
};

/// \brief Listens for TCP connections on 127.0.0.1 only: the MCP server has no authentication whatsoever.
class plMcpListener
{
  PL_DISALLOW_COPY_AND_ASSIGN(plMcpListener);

public:
  plMcpListener();
  ~plMcpListener();

  /// Listens on the port, exclusively: a second editor on the same port fails here instead of sharing it.
  plResult Listen(plUInt16 uiPort, plStringBuilder& out_sError);

  void Close();

  bool IsListening() const { return m_bListening; }

  /// The port actually bound, or 0 while not listening.
  plUInt16 GetPort() const { return m_uiPort; }

  /// A connection that is waiting to be accepted, or nullptr.
  plUniquePtr<plMcpConnection> Accept();

private:
  plUInt64 m_uiSocket = 0;
  bool m_bListening = false;
  bool m_bWinsockStarted = false;
  plUInt16 m_uiPort = 0;
};
