#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpSocket.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
  /// The streamed file goes out in pieces of this size.
  constexpr plUInt32 s_uiFileChunkSize = 64 * 1024;

  SOCKET ToSocket(plUInt64 uiSocket)
  {
    return static_cast<SOCKET>(uiSocket);
  }

  void SetNonBlocking(SOCKET hSocket)
  {
    u_long uiNonBlocking = 1;
    ioctlsocket(hSocket, FIONBIO, &uiNonBlocking);
  }

  bool WouldBlock()
  {
    return WSAGetLastError() == WSAEWOULDBLOCK;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plMcpConnection

plMcpConnection::plMcpConnection(plUInt64 uiSocket)
  : m_uiSocket(uiSocket)
{
  SetNonBlocking(ToSocket(m_uiSocket));
}

plMcpConnection::~plMcpConnection()
{
  Close();
}

bool plMcpConnection::Receive()
{
  if (!m_bOpen)
    return false;

  char buffer[4096];

  while (true)
  {
    const int iRead = recv(ToSocket(m_uiSocket), buffer, static_cast<int>(sizeof(buffer)), 0);

    if (iRead > 0)
    {
      m_Received.PushBackRange(plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(buffer), static_cast<plUInt32>(iRead)));
      continue;
    }

    // 0: the client closed its end
    return iRead < 0 && WouldBlock();
  }
}

void plMcpConnection::Write(plStringView sText)
{
  Write(plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(sText.GetStartPointer()), sText.GetElementCount()));
}

void plMcpConnection::Write(plArrayPtr<const plUInt8> data)
{
  m_Outgoing.PushBackRange(data);
}

plResult plMcpConnection::WriteFile(plStringView sPath, plUInt64& out_uiFileSize)
{
  PL_SUCCEED_OR_RETURN(m_File.Open(sPath, plFileOpenMode::Read));

  out_uiFileSize = m_File.GetFileSize();
  return PL_SUCCESS;
}

void plMcpConnection::CloseWhenSent()
{
  m_bCloseWhenSent = true;
}

bool plMcpConnection::RefillFromFile()
{
  if (!m_File.IsOpen())
    return false;

  m_Outgoing.SetCountUninitialized(s_uiFileChunkSize);
  const plUInt64 uiRead = m_File.Read(m_Outgoing.GetData(), s_uiFileChunkSize);
  m_Outgoing.SetCountUninitialized(static_cast<plUInt32>(uiRead));

  if (uiRead == 0)
  {
    m_File.Close();
    return false;
  }

  return true;
}

bool plMcpConnection::Flush()
{
  while (m_bOpen)
  {
    if (m_uiSent == m_Outgoing.GetCount())
    {
      m_Outgoing.Clear();
      m_uiSent = 0;

      if (!RefillFromFile())
        break;
    }

    const int iSent = send(ToSocket(m_uiSocket), reinterpret_cast<const char*>(m_Outgoing.GetData() + m_uiSent), static_cast<int>(m_Outgoing.GetCount() - m_uiSent), 0);

    if (iSent > 0)
    {
      m_uiSent += static_cast<plUInt32>(iSent);
      continue;
    }

    // the socket takes no more right now: the rest goes out on a later poll
    if (iSent < 0 && WouldBlock())
      return true;

    Close();
  }

  if (m_bOpen && m_bCloseWhenSent)
    Close();

  return m_bOpen;
}

void plMcpConnection::Close()
{
  if (!m_bOpen)
    return;

  m_bOpen = false;
  m_File.Close();

  // the client reads to the end of what was sent before it sees the connection close
  shutdown(ToSocket(m_uiSocket), SD_SEND);
  closesocket(ToSocket(m_uiSocket));
}

//////////////////////////////////////////////////////////////////////////
// plMcpListener

plMcpListener::plMcpListener() = default;

plMcpListener::~plMcpListener()
{
  Close();
}

plResult plMcpListener::Listen(plUInt16 uiPort, plStringBuilder& out_sError)
{
  Close();

  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
  {
    out_sError = "Winsock could not be started.";
    return PL_FAILURE;
  }

  m_bWinsockStarted = true;

  const SOCKET hSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (hSocket == INVALID_SOCKET)
  {
    out_sError.SetFormat("The socket could not be created (error {}).", WSAGetLastError());
    Close();
    return PL_FAILURE;
  }

  BOOL bExclusive = TRUE;
  setsockopt(hSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&bExclusive), sizeof(bExclusive));

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(uiPort);

  if (bind(hSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(hSocket, SOMAXCONN) == SOCKET_ERROR)
  {
    out_sError.SetFormat("The port could not be bound (error {}).", WSAGetLastError());
    closesocket(hSocket);
    Close();
    return PL_FAILURE;
  }

  SetNonBlocking(hSocket);

  // read back from the socket, so that it is the port actually bound
  sockaddr_in bound = {};
  int iBoundSize = sizeof(bound);
  getsockname(hSocket, reinterpret_cast<sockaddr*>(&bound), &iBoundSize);

  m_uiSocket = static_cast<plUInt64>(hSocket);
  m_uiPort = ntohs(bound.sin_port);
  m_bListening = true;
  return PL_SUCCESS;
}

void plMcpListener::Close()
{
  if (m_bListening)
  {
    closesocket(ToSocket(m_uiSocket));
    m_bListening = false;
    m_uiPort = 0;
  }

  if (m_bWinsockStarted)
  {
    WSACleanup();
    m_bWinsockStarted = false;
  }
}

plUniquePtr<plMcpConnection> plMcpListener::Accept()
{
  if (!m_bListening)
    return nullptr;

  const SOCKET hConnection = accept(ToSocket(m_uiSocket), nullptr, nullptr);
  if (hConnection == INVALID_SOCKET)
    return nullptr;

  return PL_DEFAULT_NEW(plMcpConnection, static_cast<plUInt64>(hConnection));
}
