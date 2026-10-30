#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/LogTool.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpLogTool, 1, plRTTIDefaultAllocator<plMcpLogTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// The names used in the tool arguments and in the results. Kept short and lower case, because this
  /// is what the AI has to type.
  plStringView SeverityToString(plLogMsgType::Enum type)
  {
    switch (type)
    {
      case plLogMsgType::ErrorMsg: return "error";
      case plLogMsgType::SeriousWarningMsg: return "serious-warning";
      case plLogMsgType::WarningMsg: return "warning";
      case plLogMsgType::SuccessMsg: return "success";
      case plLogMsgType::InfoMsg: return "info";
      case plLogMsgType::DevMsg: return "dev";
      case plLogMsgType::DebugMsg: return "debug";
      default: return "other";
    }
  }

  plLogMsgType::Enum SeverityFromString(plStringView sSeverity, plLogMsgType::Enum fallback)
  {
    if (sSeverity.IsEqual_NoCase("error")) return plLogMsgType::ErrorMsg;
    if (sSeverity.IsEqual_NoCase("serious-warning")) return plLogMsgType::SeriousWarningMsg;
    if (sSeverity.IsEqual_NoCase("warning")) return plLogMsgType::WarningMsg;
    if (sSeverity.IsEqual_NoCase("success")) return plLogMsgType::SuccessMsg;
    if (sSeverity.IsEqual_NoCase("info")) return plLogMsgType::InfoMsg;
    if (sSeverity.IsEqual_NoCase("dev")) return plLogMsgType::DevMsg;
    if (sSeverity.IsEqual_NoCase("debug")) return plLogMsgType::DebugMsg;

    return fallback;
  }
} // namespace

plMcpLogTool::plMcpLogTool() = default;

plMcpLogTool::~plMcpLogTool()
{
  OnDeactivate();
}

void plMcpLogTool::OnActivate()
{
  if (m_LogSubscription != 0)
    return;

  m_LogSubscription = plGlobalLog::AddLogWriter(plMakeDelegate(&plMcpLogTool::LogEventHandler, this));

  // TODO: Doesn't see the engine log, only the editor log.
}

void plMcpLogTool::OnDeactivate()
{
  if (m_LogSubscription == 0)
    return;

  plGlobalLog::RemoveLogWriter(m_LogSubscription);
  m_LogSubscription = 0;
}

void plMcpLogTool::LogEventHandler(const plLoggingEventData& e)
{
  // group markers and flushes carry no text worth keeping
  if (e.m_EventType < plLogMsgType::ErrorMsg || e.m_EventType > plLogMsgType::DebugMsg)
    return;

  PL_LOCK(m_Mutex);

  if (m_Entries.GetCount() >= s_uiMaxEntries)
  {
    m_Entries.PopFront();
  }

  Entry& entry = m_Entries.ExpandAndGetRef();
  entry.m_uiId = m_uiNextId++;
  entry.m_Type = e.m_EventType;
  entry.m_sText = e.m_sText;
}

void plMcpLogTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "log_write";
    desc.m_sDescription = "Writes a message to the plEditor log, so that it becomes visible to the user.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("message":{"type":"string","description":"The text to log."},)"
                          R"("severity":{"type":"string","enum":["error","serious-warning","warning","success","info","dev","debug"],"description":"Defaults to 'info'."})"
                          R"(},"required":["message"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "log_read";
    desc.m_sDescription = "Returns the most recent plEditor log messages, oldest first. Use this to find out what the editor "
                          "reported about an operation, e.g. why an asset failed to transform. The response's 'lastId' is "
                          "the id of the newest entry returned; pass it back as 'sinceId' on the next call to only get "
                          "messages that arrived after it, instead of re-reading ones already seen. 'contains' lets 'count' "
                          "reach past the most recent messages to find one further back.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("count":{"type":"number","description":"How many messages to return. Defaults to 50."},)"
                          R"("severity":{"type":"string","enum":["error","serious-warning","warning","success","info","dev","debug"],"description":"Only return messages at least this severe. Defaults to 'debug', i.e. everything."},)"
                          R"("sinceId":{"type":"number","description":"Only return messages with an id greater than this, i.e. logged after a previous log_read whose 'lastId' this is. Omit to read the newest messages regardless of what was read before."},)"
                          R"("contains":{"type":"string","description":"Only return messages whose text contains this, case insensitive. Lets 'count' reach further back than the most recent messages."})"
                          R"(}})";
  }
}

void plMcpLogTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "log_write")
  {
    ExecuteWrite(arguments, out_result);
  }
  else if (sToolName == "log_read")
  {
    ExecuteRead(arguments, out_result);
  }
}

void plMcpLogTool::ExecuteWrite(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sMessage = plMcpJson::GetString(arguments, "message");

  if (sMessage.IsEmpty())
  {
    out_result.SetError("No 'message' argument given.");
    return;
  }

  const plLogMsgType::Enum severity = SeverityFromString(plMcpJson::GetString(arguments, "severity"), plLogMsgType::InfoMsg);

  // the tag makes it obvious in the log where this came from
  switch (severity)
  {
    case plLogMsgType::ErrorMsg: plLog::Error("MCP: {}", sMessage); break;
    case plLogMsgType::SeriousWarningMsg: plLog::SeriousWarning("MCP: {}", sMessage); break;
    case plLogMsgType::WarningMsg: plLog::Warning("MCP: {}", sMessage); break;
    case plLogMsgType::SuccessMsg: plLog::Success("MCP: {}", sMessage); break;
    case plLogMsgType::DevMsg: plLog::Dev("MCP: {}", sMessage); break;
    case plLogMsgType::DebugMsg: plLog::Debug("MCP: {}", sMessage); break;
    default: plLog::Info("MCP: {}", sMessage); break;
  }

  out_result.m_sText = "ok";
}

void plMcpLogTool::ExecuteRead(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plInt64 iCount = plMath::Clamp<plInt64>(plMcpJson::GetInt(arguments, "count", 50), 1, s_uiMaxEntries);
  const plLogMsgType::Enum minSeverity = SeverityFromString(plMcpJson::GetString(arguments, "severity"), plLogMsgType::DebugMsg);
  const plUInt64 uiSinceId = static_cast<plUInt64>(plMath::Max<plInt64>(plMcpJson::GetInt(arguments, "sinceId", 0), 0));
  const plStringView sContains = plMcpJson::GetString(arguments, "contains");

  PL_LOCK(m_Mutex);

  // collect from the back, so 'count' means 'the newest count that pass the filter'
  plHybridArray<const Entry*, 64> selected;

  for (plUInt32 i = m_Entries.GetCount(); i > 0 && selected.GetCount() < iCount; --i)
  {
    const Entry& entry = m_Entries[i - 1];

    if (entry.m_uiId <= uiSinceId)
      break; // entries only get older from here, nothing further back can pass either

    // lower enum value means more severe
    if (entry.m_Type > minSeverity)
      continue;

    if (!sContains.IsEmpty() && entry.m_sText.FindSubString_NoCase(sContains) == nullptr)
      continue;

    selected.PushBack(&entry);
  }

  // the newest id actually seen by this call, i.e. what a follow-up call should pass as 'sinceId';
  // falls back to whatever was asked for so a client that already knows it stays in sync
  const plUInt64 uiLastId = selected.IsEmpty() ? uiSinceId : selected[0]->m_uiId;

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableInt64("lastId", static_cast<plInt64>(uiLastId));
  writer.BeginArray("messages");

  // 'selected' is newest first, but the result reads better oldest first
  for (plUInt32 i = selected.GetCount(); i > 0; --i)
  {
    const Entry& entry = *selected[i - 1];

    writer.BeginObject();
    writer.AddVariableInt64("id", static_cast<plInt64>(entry.m_uiId));
    writer.AddVariableString("severity", SeverityToString(entry.m_Type));
    writer.AddVariableString("text", entry.m_sText);
    writer.EndObject();
  }

  writer.EndArray();
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}
