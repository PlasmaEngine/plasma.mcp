#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpServer.h>
#include <plEditorPluginPlasmaMcp/McpTools/AppTool.h>

#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/Process.h>
#include <Foundation/Utilities/CommandLineOptions.h>
#include <Foundation/Utilities/CommandLineUtils.h>

#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorUi/Runtime.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/Project/ToolsProject.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpAppTool, 1, plRTTIDefaultAllocator<plMcpAppTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plMcpAppTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
  desc.m_sName = "app_quit";
  desc.m_sDescription = "Shuts this editor process down. The response is sent before the editor exits, but every other tool "
                        "stops answering immediately afterwards - this is the last call to this editor. Refuses to quit while "
                        "documents have unsaved changes and lists them instead, unless 'discardChanges' is set to true, which "
                        "loses those changes permanently.\n"
                        "To start an editor again afterwards, run the plEditor executable with "
                        "'-project <path-to-project-folder> -mcpport <port>'. It serves MCP at http://127.0.0.1:<port>/mcp "
                        "once the project is open, which takes a few seconds - poll the port rather than assuming a delay. "
                        "'-mcpport' defaults to 7391 and is what lets several editors run at once, each on its own port. "
                        "Call app_info for this editor's own port and executable path.";
  desc.m_sInputSchema = R"({"type":"object","properties":{"discardChanges":{"type":"boolean","description":"Quit even though documents have unsaved changes, losing them. Default false."}}})";

  plMcpToolDesc& ping = out_tools.ExpandAndGetRef();
  ping.m_sName = "app_ping";
  ping.m_sDescription = "Returns immediately, to check that the editor is still responsive. Answering requires a working event "
                        "loop on the main thread, so a reply means the editor is genuinely usable, not merely that the port is "
                        "still open. Use it after a call that may have blocked - a modal dialog or a long running action leaves "
                        "the editor stuck with its port bound but nothing being served, and this is the cheapest way to tell "
                        "that apart from an editor that is simply busy. If it does not answer within a few seconds, the process "
                        "has to be killed by process id; app_quit will not get through either. Get the process id from app_info "
                        "up front, since it cannot be retrieved once the editor stops answering.";
  ping.m_sInputSchema = R"({"type":"object","properties":{}})";

  plMcpToolDesc& info = out_tools.ExpandAndGetRef();
  info.m_sName = "app_info";
  info.m_sDescription = "Returns how to talk to and restart this editor: the port it serves MCP on, the executable that runs "
                        "it, the process id and the command line it was started with. Use it to launch another editor the "
                        "same way, or to launch a replacement after app_quit - the executable path is not otherwise "
                        "discoverable, and a different '-mcpport' is what allows a second editor to run alongside this one. "
                        "Also returns 'logFile', the editor's own log on disk, which is worth reading when the editor stopped "
                        "answering: log_read only serves messages from a live editor and only from the editor process.";
  info.m_sInputSchema = R"({"type":"object","properties":{}})";

  plMcpToolDesc& options = out_tools.ExpandAndGetRef();
  options.m_sName = "app_command_line_options";
  options.m_sDescription = "Lists every command line option this editor supports, with its type, default value and description. "
                           "This is more complete than running the editor with '-help': options declared by editor plugins - "
                           "'-mcpport' among them - only exist once their plugin is loaded, which happens after '-help' has "
                           "already printed. Use it to find out how to launch an editor with a particular configuration.";
  options.m_sInputSchema = R"({"type":"object","properties":{"contains":{"type":"string","description":"Only return options whose name or description contains this text, case insensitive. Omit for all of them."}}})";

  plMcpToolDesc& engineStatus = out_tools.ExpandAndGetRef();
  engineStatus.m_sName = "engine_status";
  engineStatus.m_sDescription = "Reports the state of the engine process - the separate process that renders the editor's viewports "
                                "and runs game simulation. Says whether it is running, whether it crashed, its process id, and its "
                                "most recent frame statistics: frames per second, draw calls, triangles and GPU/CPU memory. "
                                "A crashed engine process leaves the editor itself perfectly responsive, so app_ping cannot detect "
                                "it - viewports simply stop updating. Check this whenever something that should be visible is not, "
                                "or before trusting a rendering measurement. Reads only.";
  engineStatus.m_sInputSchema = R"({"type":"object","properties":{}})";

  plMcpToolDesc& engineRestart = out_tools.ExpandAndGetRef();
  engineRestart.m_sName = "engine_restart";
  engineRestart.m_sDescription = "Restarts the engine process without restarting the editor. Use it after the engine process "
                                 "crashed, or to make it pick up rebuilt runtime plugins - unlike app_quit this keeps the editor, "
                                 "its open documents and their undo stacks. Open documents are reloaded into the new process, so "
                                 "unsaved changes survive, but any running game simulation is lost. "
                                 "There is a two second minimum between restarts; asking again inside it is refused rather than "
                                 "queued. Returns the resulting engine state.";
  engineRestart.m_sInputSchema = R"({"type":"object","properties":{}})";
}

void plMcpAppTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "app_ping")
  {
    ExecutePing(arguments, out_result);
  }
  else if (sToolName == "app_quit")
  {
    ExecuteQuit(arguments, out_result);
  }
  else if (sToolName == "app_info")
  {
    ExecuteInfo(arguments, out_result);
  }
  else if (sToolName == "app_command_line_options")
  {
    ExecuteCommandLineOptions(arguments, out_result);
  }
  else if (sToolName == "engine_status")
  {
    ExecuteEngineStatus(arguments, out_result);
  }
  else if (sToolName == "engine_restart")
  {
    ExecuteEngineRestart(arguments, out_result);
  }
}

void plMcpAppTool::WriteEngineStatus(plMcpJsonWriter& ref_writer)
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();

  if (pConnection == nullptr)
  {
    ref_writer.AddVariableBool("running", false);
    ref_writer.AddVariableString("reason", "There is no engine process connection. No project is open.");
    return;
  }

  // 'Setup' is the honest word for it: the connection reports whether the client end has been
  // configured, which is what has to be true before anything can be rendered or simulated.
  const bool bCrashed = pConnection->IsProcessCrashed();
  ref_writer.AddVariableBool("running", pConnection->IsEngineSetup() && !bCrashed);
  ref_writer.AddVariableBool("crashed", bCrashed);
  ref_writer.AddVariableBool("restartAllowed", pConnection->IsRestartAllowed());

  const plOsProcessID uiProcessId = pConnection->GetEngineProcessID();

  // Deliberately distinct from the editor's own process id that app_info and app_ping report - killing
  // the wrong one of the two is an easy and expensive mistake.
  if (uiProcessId != 0)
    ref_writer.AddVariableUInt64("engineProcessId", uiProcessId);

  const plEditorEngineProcessConnection::FrameStats& stats = pConnection->GetFrameStats();

  // Only written once the engine has actually reported a frame. Zeroes would read as 'nothing is being
  // drawn', which is a different statement from 'nothing has been measured yet'.
  if (stats.m_bValid)
  {
    ref_writer.BeginObject("frameStats");
    ref_writer.AddVariableFloat("fps", stats.m_fFPS);
    ref_writer.AddVariableUInt32("drawCalls", stats.m_uiDrawCalls);
    ref_writer.AddVariableUInt32("triangles", stats.m_uiTriangles);
    ref_writer.AddVariableFloat("gpuMemoryMB", stats.m_fGPUMemoryMB);
    ref_writer.AddVariableFloat("cpuMemoryMB", stats.m_fCPUMemoryMB);
    ref_writer.AddVariableString("note", "Measured in the engine process about four times a second, across all open viewports. "
                                         "Not a benchmark: an idle editor throttles its redraw rate.");
    ref_writer.EndObject();
  }
}

void plMcpAppTool::ExecuteEngineStatus(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plMcpJsonWriter writer;
  writer.BeginObject();
  WriteEngineStatus(writer);
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpAppTool::ExecuteEngineRestart(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();

  if (pConnection == nullptr)
  {
    out_result.SetError("There is no engine process connection to restart. No project is open.");
    return;
  }

  // The debounce is checked here rather than left to RestartProcess(), which enforces it again but
  // reports the refusal only as a generic failure. A caller told 'wait' can wait; a caller told
  // 'failed' will retry immediately and be refused again.
  if (!pConnection->IsRestartAllowed())
  {
    out_result.SetError("The engine process was restarted less than two seconds ago. Wait a moment and call again - this limit "
                        "exists because restarting repeatedly in quick succession leaves the process in an inconsistent state.");
    return;
  }

  const plResult res = pConnection->RestartProcess();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableBool("restarted", res.Succeeded());

  if (res.Failed())
  {
    writer.AddVariableString("error", "The engine process did not come back up. log_read will have the reason; a plugin that fails "
                                      "to load is the usual one.");
  }

  WriteEngineStatus(writer);
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
  out_result.m_bIsError = res.Failed();
}

void plMcpAppTool::ExecutePing(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableBool("responsive", true);

  // Included so that a caller which pings once at the start has the process id on hand later, when a
  // blocked editor cannot be asked for anything at all and killing it is the only option left.
  writer.AddVariableUInt32("processId", plProcess::GetCurrentProcessID());

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpAppTool::ExecuteCommandLineOptions(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sContains = plMcpJson::GetString(arguments, "contains");

  plMcpJsonWriter writer;
  writer.BeginObject();

  plUInt32 uiTotalOptions = 0;
  plUInt32 uiReturned = 0;

  plStringBuilder sOptions, sShortDesc, sDefaultValue, sLongDesc, sGroup;

  writer.BeginArray("options");

  // The options register themselves into this list from wherever they are declared, including plugin
  // DLLs, so anything loaded by now is covered.
  for (plCommandLineOption* pOpt = plCommandLineOption::GetFirstInstance(); pOpt != nullptr; pOpt = pOpt->GetNextInstance())
  {
    pOpt->GetOptions(sOptions);
    pOpt->GetLongDesc(sLongDesc);
    sLongDesc.Trim(" \t\n\r");

    ++uiTotalOptions;

    if (!sContains.IsEmpty())
    {
      if (sOptions.FindSubString_NoCase(sContains) == nullptr && sLongDesc.FindSubString_NoCase(sContains) == nullptr)
        continue;
    }

    pOpt->GetParamShortDesc(sShortDesc);
    pOpt->GetParamDefaultValueDesc(sDefaultValue);
    pOpt->GetSortingGroup(sGroup);

    writer.BeginObject();

    // One option can have several spellings, e.g. '-h;-help;-?', so they are reported as a list
    // rather than as the raw separated string.
    plHybridArray<plStringView, 4> names;
    sOptions.Split(false, names, ";", "|");

    writer.BeginArray("names");
    for (plStringView sName : names)
    {
      writer.WriteString(sName);
    }
    writer.EndArray();

    writer.AddVariableString("type", pOpt->GetType());

    if (!sShortDesc.IsEmpty())
    {
      writer.AddVariableString("parameter", sShortDesc);
    }

    if (!sDefaultValue.IsEmpty())
    {
      writer.AddVariableString("default", sDefaultValue);
    }

    if (!sLongDesc.IsEmpty())
    {
      writer.AddVariableString("description", sLongDesc);
    }

    if (!sGroup.IsEmpty())
    {
      writer.AddVariableString("group", sGroup);
    }

    writer.EndObject();

    ++uiReturned;
  }

  writer.EndArray();

  // Not capped, unlike the listing tools: the number of options is bounded by what is compiled in and
  // is small, so there is nothing to truncate and no 'truncated' flag to report. 'totalOptions' is
  // still worth having, because it tells a filtered call how much it filtered away.
  writer.AddVariableUInt32("returned", uiReturned);
  writer.AddVariableUInt32("totalOptions", uiTotalOptions);

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpAppTool::ExecuteInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plMcpServer* pServer = plMcpServer::GetInstance();
  const plUInt16 uiPort = pServer != nullptr ? pServer->GetPort() : 0;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("executable", plOSFile::GetApplicationPath());
  writer.AddVariableUInt32("processId", plProcess::GetCurrentProcessID());

  writer.AddVariableUInt32("mcpPort", uiPort);

  plStringBuilder sUrl;
  sUrl.SetFormat("http://127.0.0.1:{}/mcp", uiPort);
  writer.AddVariableString("mcpUrl", sUrl);

  // The command line this editor was started with, so that another one can be launched the same way.
  // Whether the executable itself appears as the first entry is platform dependent, which is why it
  // is also reported on its own above.
  const plCommandLineUtils* pCmd = plCommandLineUtils::GetGlobalInstance();

  writer.BeginArray("commandLine");
  for (plUInt32 i = 0; i < pCmd->GetParameterCount(); ++i)
  {
    writer.WriteString(pCmd->GetParameter(i));
  }
  writer.EndArray();

  // Where the editor writes its log. Reported as an absolute path because the location is otherwise
  // undiscoverable: it lives in the user data directory under a name derived from '-appid', and
  // nothing in the tool list or in '-help' points at it. It matters most when the editor dies - a
  // crash takes the MCP server with it, so the file is all that is left to read afterwards.
  {
    const plInt32 iApplicationID = plCommandLineUtils::GetGlobalInstance()->GetIntOption("-appid", 0);

    plStringBuilder sLogFile;
    sLogFile.SetFormat(":appdata/Logs/LogEditor_{}.htm", iApplicationID);

    plStringBuilder sAbsLogFile;
    if (plFileSystem::ResolvePath(sLogFile, &sAbsLogFile, nullptr).Succeeded())
    {
      writer.AddVariableString("logFile", sAbsLogFile);

      // The engine runs as a separate process and logs separately, which is why log_read cannot see
      // it. Naming the pattern is the only way an agent can find those files at all.
      plStringBuilder sEngineLogPattern = sAbsLogFile;
      sEngineLogPattern.PathParentDirectory();
      sEngineLogPattern.AppendFormat("LogEditor_{}_Engine_<pid>.htm", iApplicationID);
      sEngineLogPattern.MakeCleanPath();
      writer.AddVariableString("engineLogFilePattern", sEngineLogPattern);
    }
  }

  writer.AddVariableString("launchHint",
    "Run 'executable' with '-project <project-folder> -mcpport <port>' to start another editor. Poll the port until it "
    "accepts connections - startup takes a few seconds. Use a port other than 'mcpPort' to run one alongside this editor. "
    "'logFile' is written as HTML and is the only record left if the editor crashes, since that also kills this server; "
    "note that a crash may lose the last messages, as the log is not flushed on the way down.");

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpAppTool::CollectModifiedDocuments(plDynamicArray<plString>& out_documents)
{
  for (const plDocumentManager* pMan : plDocumentManager::GetAllDocumentManagers())
  {
    for (const plDocument* pDoc : pMan->GetAllOpenDocuments())
    {
      if (pDoc->IsModified())
      {
        out_documents.PushBack(pDoc->GetDocumentPath());
      }
    }
  }
}

void plMcpAppTool::ExecuteQuit(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const bool bDiscardChanges = plMcpJson::GetBool(arguments, "discardChanges", false);

  // Checked here rather than through plToolsProject::CanCloseProject(), which asks the *user* about
  // unsaved documents through a modal dialog. With no one at the keyboard - which is the entire point
  // of this tool - that dialog never returns and the editor hangs, holding its port, unreachable and
  // unkillable through MCP. So the decision is made without any dialog at all.
  plDynamicArray<plString> modifiedDocuments;
  CollectModifiedDocuments(modifiedDocuments);

  if (!modifiedDocuments.IsEmpty() && !bDiscardChanges)
  {
    plMcpJsonWriter writer;
    writer.BeginObject();
    writer.AddVariableBool("quitting", false);
    writer.AddVariableString("reason", "Documents have unsaved changes. Save them, or call again with discardChanges=true to lose them.");

    writer.BeginArray("modifiedDocuments");
    for (const plString& sDoc : modifiedDocuments)
    {
      writer.WriteString(sDoc);
    }
    writer.EndArray();

    writer.EndObject();

    // an error, so the agent cannot mistake this for a successful shutdown and stop waiting
    out_result.m_sText = writer.GetResult();
    out_result.m_bIsError = true;
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableBool("quitting", true);
  writer.AddVariableUInt32("discardedDocuments", modifiedDocuments.GetCount());
  writer.EndObject();
  out_result.m_sText = writer.GetResult();

  // Deferred to the next frame, because this runs inside the request handler: the response above has
  // not been written to the socket yet, and quitting here would drop it, leaving the caller waiting on
  // a connection that closes with no answer.
  plEditorUiRuntime::RunAfterFrame([]()
    {
      // Closes the documents first, discarding unsaved changes without asking. Otherwise the shutdown
      // path finds modified documents and raises the modal save dialog that this tool exists to avoid.
      // Reaching this point means that was either unnecessary or explicitly requested.
      plDocumentManager::CloseAllDocuments();

      // what closing the editor window does once nothing is left to ask about
      plToolsProject::SaveProjectState();
      plEditorApp::GetSingleton()->RequestQuit(); });
}
