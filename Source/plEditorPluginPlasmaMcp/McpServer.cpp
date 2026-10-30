#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpServer.h>
#include <plEditorPluginPlasmaMcp/McpTools/GpuDumpTool.h>
#include <plEditorPluginPlasmaMcp/McpTools/McpToolRegistry.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/ConversionUtils.h>

namespace
{
  /// How often the sockets are polled.
  constexpr plTime s_PollInterval = plTime::MakeFromMilliseconds(10);

  /// Where the header ends ("\r\n\r\n"), or -1 while it has not arrived completely.
  plInt32 FindHeaderEnd(const plDynamicArray<plUInt8>& data)
  {
    for (plUInt32 i = 0; i + 3 < data.GetCount(); ++i)
    {
      if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n')
        return static_cast<plInt32>(i);
    }

    return -1;
  }

  /// The value of the Content-Length header line, or 0.
  plInt32 GetContentLength(const plDynamicArray<plStringView>& lines)
  {
    for (plStringView sLine : lines)
    {
      sLine.Trim(" \t\r");
      if (!sLine.StartsWith_NoCase("content-length:"))
        continue;

      plStringView sValue(sLine.GetStartPointer() + 15, sLine.GetEndPointer());
      sValue.Trim(" \t");

      plInt32 iLength = 0;
      if (plConversionUtils::StringToInt(sValue, iLength).Succeeded())
        return iLength;
    }

    return 0;
  }

  /// \brief Writes the JSON-RPC 'id' back out exactly as the type it came in as.
  ///
  /// The spec allows numbers, strings and null, and the client is entitled to reject a response whose
  /// id doesn't match what it sent.
  void WriteId(plMcpJsonWriter& ref_writer, const plVariantDictionary& msg)
  {
    ref_writer.BeginVariable("id");

    const plVariant* pId = nullptr;

    if (!msg.TryGetValue("id", pId) || !pId->IsValid())
    {
      ref_writer.WriteNULL();
    }
    else if (pId->IsA<plString>())
    {
      ref_writer.WriteString(pId->Get<plString>().GetView());
    }
    else
    {
      // the JSON reader represents every number as a double, but ids are conventionally integers,
      // so write them without a fractional part
      ref_writer.WriteInt64(static_cast<plInt64>(pId->ConvertTo<double>()));
    }

    ref_writer.EndVariable();
  }
} // namespace

plMcpServer* plMcpServer::s_pInstance = nullptr;

plMcpServer::plMcpServer()
{
  // only ever one of these, created by the plugin - the last one wins rather than asserting, because
  // taking the editor down over a reporting detail would be worse than reporting the wrong port
  s_pInstance = this;
}

plMcpServer::~plMcpServer()
{
  Stop();

  if (s_pInstance == this)
  {
    s_pInstance = nullptr;
  }
}

plResult plMcpServer::Start(plUInt16 uiPort)
{
  Stop();

  // bind to the loopback interface only - this server has no authentication whatsoever
  plStringBuilder sError;
  if (m_Listener.Listen(uiPort, sError).Failed())
  {
    plLog::Warning("MCP: Failed to listen on port {}: {}", uiPort, sError);
    return PL_FAILURE;
  }

  m_uiPollTimer = m_Timers.Start(s_PollInterval, [this]()
    { Poll(); }, true);

  // editor plugins that were loaded after this one may bring their own tools along
  plMcpToolRegistry::UpdateProviders();

  plLog::Success("MCP server listening on http://127.0.0.1:{}/mcp, {} tools available.", GetPort(), plMcpToolRegistry::GetTools().GetCount());
  return PL_SUCCESS;
}

void plMcpServer::Stop()
{
  if (!m_Listener.IsListening())
    return;

  m_Timers.Stop(m_uiPollTimer);
  m_Listener.Close();

  // a request being handled right now still sends its response; the poll drops the connections afterwards
  if (m_uiPollDepth > 0)
    m_bDropConnections = true;
  else
    m_Connections.Clear();

  plLog::Info("MCP server stopped.");
}

void plMcpServer::Poll()
{
  ++m_uiPollDepth;

  while (plUniquePtr<plMcpConnection> pConnection = m_Listener.Accept())
  {
    m_Connections.PushBack(std::move(pConnection));
  }

  // the count is read each time: a tool call may poll from inside and accept more connections
  for (plUInt32 i = 0; i < m_Connections.GetCount(); ++i)
  {
    plMcpConnection& connection = *m_Connections[i];

    if (!connection.IsOpen() || m_Handling.Contains(&connection))
      continue;

    if (!UpdateConnection(connection))
      connection.Close();
  }

  --m_uiPollDepth;

  if (m_uiPollDepth == 0)
    RemoveClosedConnections();
}

void plMcpServer::RemoveClosedConnections()
{
  if (m_bDropConnections)
  {
    m_bDropConnections = false;
    m_Connections.Clear();
    return;
  }

  for (plUInt32 i = m_Connections.GetCount(); i > 0; --i)
  {
    if (!m_Connections[i - 1]->IsOpen())
      m_Connections.RemoveAtAndCopy(i - 1);
  }
}

bool plMcpServer::UpdateConnection(plMcpConnection& ref_connection)
{
  // once a request was answered, the connection only sends until it closes
  if (!ref_connection.IsCloseRequested())
  {
    // a client may close its sending end right after the request, which still gets its answer
    const bool bClientSending = ref_connection.Receive();

    m_Handling.PushBack(&ref_connection);
    TryHandleRequest(ref_connection);
    m_Handling.RemoveAndCopy(&ref_connection);

    if (!bClientSending && !ref_connection.IsCloseRequested())
      return false; // gone before the request was complete
  }

  return ref_connection.Flush();
}

void plMcpServer::TryHandleRequest(plMcpConnection& ref_connection)
{
  plDynamicArray<plUInt8>& data = ref_connection.GetReceived();

  const plInt32 iHeaderEnd = FindHeaderEnd(data);

  if (iHeaderEnd < 0)
    return; // headers are not complete yet

  const char* szData = reinterpret_cast<const char*>(data.GetData());

  plDynamicArray<plStringView> lines;
  plStringView(szData, szData + iHeaderEnd).Split(false, lines, "\n");

  // "POST /mcp HTTP/1.1"
  plHybridArray<plStringView, 3> requestLine;
  {
    plStringView sRequestLine = lines.IsEmpty() ? plStringView() : lines[0];
    sRequestLine.Trim(" \t\r");
    sRequestLine.Split(false, requestLine, " ");
  }

  const plStringView sMethod = requestLine.GetCount() > 0 ? requestLine[0] : plStringView();
  const plStringView sPath = requestLine.GetCount() > 1 ? requestLine[1] : plStringView();

  const plInt32 iContentLength = plMath::Max(GetContentLength(lines), 0);
  const plInt32 iBodyStart = iHeaderEnd + 4;

  if (static_cast<plInt32>(data.GetCount()) - iBodyStart < iContentLength)
    return; // body has not arrived completely yet

  // from here on the request is fully read - whatever happens, this connection is done afterwards
  ref_connection.CloseWhenSent();

  if (sMethod == "GET" && plMcpGpuDumpTool::Serve(ref_connection, sPath))
    return;

  if (sPath != "/mcp")
  {
    SendResponse(ref_connection, "404 Not Found", {});
    return;
  }

  if (sMethod != "POST")
  {
    // GET would be the SSE stream and DELETE would end a session - we support neither
    SendResponse(ref_connection, "405 Method Not Allowed", {});
    return;
  }

  plRawMemoryStreamReader reader(data.GetData() + iBodyStart, static_cast<plUInt32>(iContentLength));

  plJSONReader json;
  if (json.Parse(reader).Failed())
  {
    SendResponse(ref_connection, "400 Bad Request", "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
    return;
  }

  plStringBuilder sResponse;
  if (HandleMessage(json.GetTopLevelObject(), sResponse).Failed())
  {
    // this was a notification - it must not be answered with a JSON-RPC response
    SendResponse(ref_connection, "202 Accepted", {});
    return;
  }

  SendResponse(ref_connection, "200 OK", sResponse.GetView());
}

plResult plMcpServer::HandleMessage(const plVariantDictionary& msg, plStringBuilder& out_sResponse)
{
  const plStringView sMethod = plMcpJson::GetString(msg, "method");
  const plVariantDictionary* pParams = plMcpJson::GetDict(msg, "params");

  // notifications carry no 'id' and must be answered with an empty body, never with a JSON-RPC response
  if (sMethod.StartsWith("notifications/"))
    return PL_FAILURE;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("jsonrpc", "2.0");
  WriteId(writer, msg);

  if (sMethod == "initialize")
  {
    // echo the client's protocol version back, rather than guessing at one
    const plStringView sVersion = pParams ? plMcpJson::GetString(*pParams, "protocolVersion", "2025-06-18") : "2025-06-18";

    writer.BeginObject("result");
    writer.AddVariableString("protocolVersion", sVersion);

    writer.BeginObject("capabilities");
    writer.BeginObject("tools");
    writer.EndObject();
    writer.EndObject();

    writer.BeginObject("serverInfo");
    writer.AddVariableString("name", "plEditor");
    writer.AddVariableString("version", "0.1");
    writer.EndObject();

    writer.EndObject();
  }
  else if (sMethod == "tools/list")
  {
    writer.BeginObject("result");
    writer.BeginArray("tools");

    for (const plMcpToolDesc& tool : plMcpToolRegistry::GetTools())
    {
      writer.BeginObject();
      writer.AddVariableString("name", tool.m_sName);
      writer.AddVariableString("description", tool.m_sDescription);

      // the schema is raw JSON authored by the tool, so it is spliced in rather than escaped
      writer.AddVariableRawJson("inputSchema", tool.m_sInputSchema.IsEmpty() ? plStringView("{\"type\":\"object\"}") : tool.m_sInputSchema.GetView());
      writer.EndObject();
    }

    writer.EndArray();
    writer.EndObject();
  }
  else if (sMethod == "tools/call")
  {
    const plStringView sToolName = pParams ? plMcpJson::GetString(*pParams, "name") : plStringView();
    const plVariantDictionary* pArgs = pParams ? plMcpJson::GetDict(*pParams, "arguments") : nullptr;

    // a call without an 'arguments' object is legal for tools that take none
    const plVariantDictionary emptyArgs;

    plMcpToolResult result;

    if (plMcpToolRegistry::Execute(sToolName, pArgs != nullptr ? *pArgs : emptyArgs, result).Failed())
    {
      result.SetError(plStringBuilder("Unknown tool: ", sToolName));
    }

    writer.BeginObject("result");

    if (result.m_bIsError)
    {
      // a failed tool call is reported inside the result, not as a JSON-RPC error - that way the
      // client hands the message to the model instead of treating it as a protocol failure
      writer.AddVariableBool("isError", true);
    }

    writer.BeginArray("content");
    writer.BeginObject();
    writer.AddVariableString("type", "text");
    writer.AddVariableString("text", result.m_sText);
    writer.EndObject();
    writer.EndArray();

    writer.EndObject();
  }
  else
  {
    plStringBuilder sMessage("Method not found: ", sMethod);

    writer.BeginObject("error");
    writer.AddVariableInt32("code", -32601);
    writer.AddVariableString("message", sMessage);
    writer.EndObject();
  }

  writer.EndObject();

  out_sResponse = writer.GetResult();

  return PL_SUCCESS;
}

void plMcpServer::SendResponse(plMcpConnection& ref_connection, plStringView sStatusLine, plStringView sBody)
{
  plStringBuilder sHeader;
  sHeader.SetFormat("HTTP/1.1 {}\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: {}\r\n"
                    "Connection: close\r\n"
                    "\r\n",
    sStatusLine, sBody.GetElementCount());

  ref_connection.Write(sHeader);
  ref_connection.Write(sBody);
  ref_connection.CloseWhenSent();

  // sent right away rather than on the next poll, as the tool may quit the editor before that
  ref_connection.Flush();
}
