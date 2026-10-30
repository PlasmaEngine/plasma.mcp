#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/ProfilingTool.h>

#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Memory/MemoryTracker.h>
#include <Foundation/Profiling/ProfilingUtils.h>
#include <Foundation/Time/Timestamp.h>

#include <Core/Console/CommandInterpreter.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpProfilingTool, 1, plRTTIDefaultAllocator<plMcpProfilingTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// The names used for console output line types in engine_console results.
  plStringView ConsoleLineTypeToString(plConsoleString::Type type)
  {
    switch (type)
    {
      case plConsoleString::Type::Error: return "error";
      case plConsoleString::Type::SeriousWarning: return "serious-warning";
      case plConsoleString::Type::Warning: return "warning";
      case plConsoleString::Type::Note: return "note";
      case plConsoleString::Type::Success: return "success";
      case plConsoleString::Type::Executed: return "executed";
      case plConsoleString::Type::VarName: return "cvar";
      case plConsoleString::Type::FuncName: return "function";
      case plConsoleString::Type::Dev: return "dev";
      case plConsoleString::Type::Debug: return "debug";
      default: return "text";
    }
  }

  struct AllocatorEntry
  {
    plString m_sName;
    plString m_sParent;
    plUInt64 m_uiSizeBytes = 0;
    plUInt64 m_uiNumAllocations = 0;
    plUInt64 m_uiNumDeallocations = 0;
    plUInt64 m_uiPerFrameBytes = 0;
  };

  /// Writes one process' allocator list as {"allocators":[..],"totalTrackedBytes":..}, largest first.
  void WriteAllocatorSection(plMcpJsonWriter& ref_writer, plDynamicArray<AllocatorEntry>& ref_entries, plStringView sFilter, plUInt32 uiCap)
  {
    // The sum only counts root allocators, because child allocator sizes are included in their parent.
    plUInt64 uiTotalTracked = 0;
    for (const AllocatorEntry& e : ref_entries)
    {
      if (e.m_sParent.IsEmpty())
        uiTotalTracked += e.m_uiSizeBytes;
    }

    if (!sFilter.IsEmpty())
    {
      for (plUInt32 i = ref_entries.GetCount(); i-- > 0;)
      {
        if (ref_entries[i].m_sName.GetView().FindSubString_NoCase(sFilter) == nullptr)
          ref_entries.RemoveAtAndSwap(i);
      }
    }

    ref_entries.Sort([](const AllocatorEntry& lhs, const AllocatorEntry& rhs) { return lhs.m_uiSizeBytes > rhs.m_uiSizeBytes; });

    const plUInt32 uiReturned = plMath::Min(ref_entries.GetCount(), uiCap);

    ref_writer.BeginArray("allocators");
    for (plUInt32 i = 0; i < uiReturned; ++i)
    {
      const AllocatorEntry& e = ref_entries[i];

      ref_writer.BeginObject();
      ref_writer.AddVariableString("name", e.m_sName);

      if (!e.m_sParent.IsEmpty())
        ref_writer.AddVariableString("parent", e.m_sParent);

      ref_writer.AddVariableUInt64("sizeBytes", e.m_uiSizeBytes);

      // Deallocations exceeding allocations would mean tracking started late; clamp rather than wrap.
      const plUInt64 uiLive = e.m_uiNumAllocations >= e.m_uiNumDeallocations ? e.m_uiNumAllocations - e.m_uiNumDeallocations : 0;
      ref_writer.AddVariableUInt64("liveAllocations", uiLive);

      if (e.m_uiPerFrameBytes > 0)
        ref_writer.AddVariableUInt64("perFrameBytes", e.m_uiPerFrameBytes);

      ref_writer.EndObject();
    }
    ref_writer.EndArray();

    ref_writer.AddVariableUInt32("returned", uiReturned);
    ref_writer.AddVariableUInt32("matched", ref_entries.GetCount());
    ref_writer.AddVariableUInt64("totalTrackedBytes", uiTotalTracked);

    if (uiReturned < ref_entries.GetCount())
      ref_writer.AddVariableBool("truncated", true);
  }
} // namespace

plMcpProfilingTool::plMcpProfilingTool() = default;

plMcpProfilingTool::~plMcpProfilingTool()
{
  OnDeactivate();
}

void plMcpProfilingTool::OnActivate()
{
  if (m_bSubscribed)
    return;

  // The engine broadcasts every cvar shortly after it starts and again on every change, so by the time
  // a tool call arrives the cache is complete. Providers outlive the MCP server, so nothing is missed.
  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plMcpProfilingTool::ConnectionEventHandler, this));
  m_bSubscribed = true;
}

void plMcpProfilingTool::OnDeactivate()
{
  if (!m_bSubscribed)
    return;

  plEditorEngineProcessConnection::s_Events.RemoveEventHandler(plMakeDelegate(&plMcpProfilingTool::ConnectionEventHandler, this));
  m_bSubscribed = false;
}

void plMcpProfilingTool::ConnectionEventHandler(const plEditorEngineProcessConnection::Event& e)
{
  switch (e.m_Type)
  {
    case plEditorEngineProcessConnection::Event::Type::ProcessMessage:
    {
      if (const auto* pMsg = plDynamicCast<const plCVarMsgToEditor*>(e.m_pMsg))
      {
        CVarInfo& info = m_CVars[pMsg->m_sName];
        info.m_sPlugin = pMsg->m_sPlugin;
        info.m_sDescription = pMsg->m_sDescription;
        info.m_Value = pMsg->m_Value;
      }
      break;
    }

    // A dead engine's values are stale and a new engine re-broadcasts everything, so the cache is
    // dropped rather than carried across.
    case plEditorEngineProcessConnection::Event::Type::ProcessStarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessRestarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessCrashed:
    case plEditorEngineProcessConnection::Event::Type::ProcessShutdown:
      m_CVars.Clear();
      m_bCVarCacheComplete = false;
      break;

    default:
      break;
  }
}

void plMcpProfilingTool::RefreshCVarCache(plEditorEngineProcessConnection* pConnection)
{
  plSimpleConfigMsgToEngine msg;
  msg.m_sWhatToDo = "TransmitCVars";

  if (!pConnection->SendMessage(&msg))
    return;

  // The broadcast is one message per cvar. The sync marker is answered only after the engine sent
  // them all, and waiting for it pumps every one of them through ConnectionEventHandler first.
  if (pConnection->SyncWithProcess(plTime::MakeFromSeconds(5)).Succeeded())
  {
    m_bCVarCacheComplete = true;
  }
}

plEditorEngineProcessConnection* plMcpProfilingTool::GetEngineConnection(plMcpToolResult& out_result)
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();

  if (pConnection == nullptr)
  {
    out_result.SetError("There is no engine process connection. No project is open.");
    return nullptr;
  }

  if (pConnection->IsProcessCrashed())
  {
    out_result.SetError("The engine process has crashed. Call engine_restart to bring it back, then try again.");
    return nullptr;
  }

  if (!pConnection->IsEngineSetup())
  {
    out_result.SetError("The engine process is not up yet. engine_status reports details; opening a scene document starts it.");
    return nullptr;
  }

  return pConnection;
}

void plMcpProfilingTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  plMcpToolDesc& stats = out_tools.ExpandAndGetRef();
  stats.m_sName = "engine_stats";
  stats.m_sDescription = "Returns a snapshot of the engine process' full statistics map: hundreds of live counters that engine "
                         "systems update as they run - frame rate, draw calls, visible objects and triangles, GPU memory pools, "
                         "resource and streaming counts, per-system numbers. Names are slash-separated group paths, e.g. "
                         "'RenderContext/DrawCalls'. Pass 'filter' to narrow by substring of the name, which is how to use this "
                         "repeatedly - the full map is large and mostly irrelevant to any one question; an unfiltered first call "
                         "is fine for discovering what exists. Blocks up to ten seconds for the engine's answer. Two caveats: an "
                         "idle editor throttles its redraw loop, so per-frame numbers freeze between redraws and are not a "
                         "benchmark - interact with a viewport or enter play mode for live numbers; and a stat keeps its last "
                         "value after the system that wrote it goes quiet. For call-level timing use engine_profile_capture "
                         "instead. Reads only.";
  stats.m_sInputSchema = R"({"type":"object","properties":{"filter":{"type":"string","description":"Only return stats whose name contains this text, case insensitive. Omit for all of them."}}})";

  plMcpToolDesc& memory = out_tools.ExpandAndGetRef();
  memory.m_sName = "engine_memory";
  memory.m_sDescription = "Returns per-allocator CPU memory statistics for the engine process and this editor process, largest "
                          "first: allocator name, parent allocator, current size in bytes, live allocation count, and - for "
                          "allocators that were active this frame - the bytes they allocated in it, which is what to look at "
                          "when hunting per-frame allocation churn. 'totalTrackedBytes' per process sums the root allocators. "
                          "GPU memory is not here: that is 'GPU Resource Pool/Memory Consumption (MB)' in engine_stats. Blocks "
                          "up to ten seconds. Reads only.";
  memory.m_sInputSchema = R"({"type":"object","properties":{"filter":{"type":"string","description":"Only return allocators whose name contains this text, case insensitive."}}})";

  plMcpToolDesc& capture = out_tools.ExpandAndGetRef();
  capture.m_sName = "engine_profile_capture";
  capture.m_sDescription = "Captures profiling data from the engine process and the editor process, merges both into one "
                           "Chrome-trace JSON file and returns its absolute path. The capture is retrospective: the profiling "
                           "system keeps a ring buffer of the most recent frames, so the file describes what just happened - "
                           "reproduce the slow thing first, then call this. Analyze the file offline with a script (top scopes "
                           "by total and self duration, per-thread frame breakdown) instead of reading it whole - captures are "
                           "commonly tens of megabytes, and 'fileSizeBytes' in the result says how big this one is. Format: "
                           "Chrome trace events as 'B'/'E' begin/end pairs with 'ts' in microseconds, 'pid' telling editor "
                           "from engine, one 'tid' per thread, GPU scopes included; a human can open it in ui.perfetto.dev. Blocks for several "
                           "seconds, up to twenty. Each call writes a new timestamped file in the project's appdata; nothing "
                           "cleans them up.";
  capture.m_sInputSchema = R"({"type":"object","properties":{}})";

  plMcpToolDesc& cvarList = out_tools.ExpandAndGetRef();
  cvarList.m_sName = "engine_cvar_list";
  cvarList.m_sDescription = "Lists the engine process' console variables: name, current value, the plugin that declares it and "
                            "its description. CVars are the engine's runtime switches - most rendering and performance features "
                            "hang off one, which makes them the levers for A/B measurements: read a number with engine_stats, "
                            "flip a cvar with engine_cvar_set, read again. Served from a cache fed by the engine's own "
                            "broadcasts and refilled from the engine when empty, so it shows the values the engine last "
                            "reported. An empty result therefore means the engine is not running - engine_status has details. "
                            "'filter' matches name, plugin and description, case insensitive. Reads only.";
  cvarList.m_sInputSchema = R"({"type":"object","properties":{"filter":{"type":"string","description":"Only return cvars whose name, plugin or description contains this text, case insensitive."}}})";

  plMcpToolDesc& cvarSet = out_tools.ExpandAndGetRef();
  cvarSet.m_sName = "engine_cvar_set";
  cvarSet.m_sDescription = "Sets one cvar in the engine process and waits for the engine to confirm the new value. 'value' must "
                           "fit the cvar's type as engine_cvar_list reports it (bool, int, float or string). Setting the value "
                           "it already has is reported without a round trip. Two caveats. CVars flagged to save are written to "
                           "disk by the engine when changed this way and override their compiled-in default in every future "
                           "session - revert what you change once you are done measuring, or the experiment becomes a permanent "
                           "setting. And a confirmed change is not always a lasting one: some cvars are re-asserted every frame "
                           "by engine code or by a component in the open scene, which silently snaps them back - read the value "
                           "again with engine_cvar_list if a change seems to have no effect.";
  cvarSet.m_sInputSchema = R"({"type":"object","properties":{)"
                           R"("name":{"type":"string","description":"The cvar's exact name, from engine_cvar_list."},)"
                           R"("value":{"type":["boolean","number","string"],"description":"The new value. Must be convertible to the cvar's type."})"
                           R"(},"required":["name","value"]})";

  plMcpToolDesc& console = out_tools.ExpandAndGetRef();
  console.m_sName = "engine_console";
  console.m_sDescription = "Runs one command in the engine process' console and returns the output lines. The console "
                           "understands cvar assignment ('Rendering.Foo = 2'), cvar queries (just the name), and console "
                           "function calls. With 'autoComplete' true the command is completed instead of executed, which is how "
                           "to discover cvar and function names by prefix. For plain cvar work prefer engine_cvar_list and "
                           "engine_cvar_set, which are structured; this tool is for console functions and whatever else only "
                           "the console offers. Blocks up to ten seconds.";
  console.m_sInputSchema = R"({"type":"object","properties":{)"
                           R"("command":{"type":"string","description":"The command line to run, e.g. 'Player.Speed = 4' or a console function call."},)"
                           R"("autoComplete":{"type":"boolean","description":"Complete the input instead of executing it. Default false."})"
                           R"(},"required":["command"]})";
}

void plMcpProfilingTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "engine_stats")
  {
    ExecuteStats(arguments, out_result);
  }
  else if (sToolName == "engine_memory")
  {
    ExecuteMemory(arguments, out_result);
  }
  else if (sToolName == "engine_profile_capture")
  {
    ExecuteProfileCapture(arguments, out_result);
  }
  else if (sToolName == "engine_cvar_list")
  {
    ExecuteCVarList(arguments, out_result);
  }
  else if (sToolName == "engine_cvar_set")
  {
    ExecuteCVarSet(arguments, out_result);
  }
  else if (sToolName == "engine_console")
  {
    ExecuteConsole(arguments, out_result);
  }
}

void plMcpProfilingTool::ExecuteStats(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plEditorEngineProcessConnection* pConnection = GetEngineConnection(out_result);
  if (pConnection == nullptr)
    return;

  const plStringView sFilter = plMcpJson::GetString(arguments, "filter");

  plStatsSnapshotMsgToEngine msg;
  msg.m_bIncludeStats = true;
  msg.m_bIncludeMemory = false;

  if (!pConnection->SendMessage(&msg))
  {
    out_result.SetError("Sending to the engine process failed. Call engine_status to see what state it is in.");
    return;
  }

  plVariantDictionary stats;

  auto callback = [&](plProcessMessage* pMsg) -> bool
  {
    auto* pResult = plDynamicCast<plStatsSnapshotResultMsgToEditor*>(pMsg);
    if (pResult == nullptr)
      return false;

    if (pResult->m_Stats.IsA<plVariantDictionary>())
      stats = pResult->m_Stats.Get<plVariantDictionary>();

    return true;
  };
  plProcessCommunicationChannel::WaitForMessageCallback cb = callback;

  if (pConnection->WaitForMessage(plGetStaticRTTI<plStatsSnapshotResultMsgToEditor>(), plTime::MakeFromSeconds(10), &cb).Failed())
  {
    out_result.SetError("The engine process did not answer within 10 seconds. app_ping tells a busy editor from a wedged one; "
                        "engine_status tells whether the engine process crashed. An engine binary built before this tool "
                        "existed never answers - it has to be rebuilt and restarted.");
    return;
  }

  // Sorted output groups related stats next to each other, since names are slash-separated paths.
  plMap<plString, plVariant> sorted;
  for (auto it : stats)
  {
    if (sFilter.IsEmpty() || it.Key().GetView().FindSubString_NoCase(sFilter) != nullptr)
      sorted.Insert(it.Key(), it.Value());
  }

  constexpr plUInt32 uiCap = 500;
  plUInt32 uiReturned = 0;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.BeginObject("stats");
  for (auto it = sorted.GetIterator(); it.IsValid(); ++it)
  {
    if (uiReturned >= uiCap)
      break;

    writer.AddVariableVariant(it.Key(), it.Value());
    ++uiReturned;
  }
  writer.EndObject();

  writer.AddVariableUInt32("returned", uiReturned);
  writer.AddVariableUInt32("matched", sorted.GetCount());
  writer.AddVariableUInt32("totalStats", stats.GetCount());

  if (uiReturned < sorted.GetCount())
    writer.AddVariableBool("truncated", true);

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpProfilingTool::ExecuteMemory(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plEditorEngineProcessConnection* pConnection = GetEngineConnection(out_result);
  if (pConnection == nullptr)
    return;

  const plStringView sFilter = plMcpJson::GetString(arguments, "filter");

  plStatsSnapshotMsgToEngine msg;
  msg.m_bIncludeStats = false;
  msg.m_bIncludeMemory = true;

  if (!pConnection->SendMessage(&msg))
  {
    out_result.SetError("Sending to the engine process failed. Call engine_status to see what state it is in.");
    return;
  }

  plDynamicArray<AllocatorEntry> engineEntries;

  auto callback = [&](plProcessMessage* pMsg) -> bool
  {
    auto* pResult = plDynamicCast<plStatsSnapshotResultMsgToEditor*>(pMsg);
    if (pResult == nullptr)
      return false;

    // All six arrays are filled in lockstep by the engine; the min guards against a mismatched
    // engine build, because an out of range index asserts and takes the editor down.
    const plUInt32 uiCount = plMath::Min(pResult->m_AllocatorNames.GetCount(), pResult->m_AllocatorParentNames.GetCount(),
      plMath::Min(pResult->m_AllocatorSizeBytes.GetCount(), pResult->m_AllocatorNumAllocations.GetCount(),
        plMath::Min(pResult->m_AllocatorNumDeallocations.GetCount(), pResult->m_AllocatorPerFrameSizeBytes.GetCount())));
    engineEntries.Reserve(uiCount);

    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      AllocatorEntry& e = engineEntries.ExpandAndGetRef();
      e.m_sName = pResult->m_AllocatorNames[i];
      e.m_sParent = pResult->m_AllocatorParentNames[i];
      e.m_uiSizeBytes = pResult->m_AllocatorSizeBytes[i];
      e.m_uiNumAllocations = pResult->m_AllocatorNumAllocations[i];
      e.m_uiNumDeallocations = pResult->m_AllocatorNumDeallocations[i];
      e.m_uiPerFrameBytes = pResult->m_AllocatorPerFrameSizeBytes[i];
    }

    return true;
  };
  plProcessCommunicationChannel::WaitForMessageCallback cb = callback;

  if (pConnection->WaitForMessage(plGetStaticRTTI<plStatsSnapshotResultMsgToEditor>(), plTime::MakeFromSeconds(10), &cb).Failed())
  {
    out_result.SetError("The engine process did not answer within 10 seconds. app_ping tells a busy editor from a wedged one; "
                        "engine_status tells whether the engine process crashed. An engine binary built before this tool "
                        "existed never answers - it has to be rebuilt and restarted.");
    return;
  }

  // The editor process' own allocators are read directly - this code runs inside the editor.
  plDynamicArray<AllocatorEntry> editorEntries;
  for (auto it = plMemoryTracker::GetIterator(); it.IsValid(); ++it)
  {
    AllocatorEntry& e = editorEntries.ExpandAndGetRef();
    e.m_sName = it.Name();
    e.m_sParent = it.ParentId().IsInvalidated() ? plStringView() : plMemoryTracker::GetAllocatorName(it.ParentId());

    const plAllocator::Stats& s = it.Stats();
    e.m_uiSizeBytes = s.m_uiAllocationSize;
    e.m_uiNumAllocations = s.m_uiNumAllocations;
    e.m_uiNumDeallocations = s.m_uiNumDeallocations;
    e.m_uiPerFrameBytes = s.m_uiPerFrameAllocationSize;
  }

  constexpr plUInt32 uiCap = 150;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.BeginObject("engine");
  WriteAllocatorSection(writer, engineEntries, sFilter, uiCap);
  writer.EndObject();

  writer.BeginObject("editor");
  WriteAllocatorSection(writer, editorEntries, sFilter, uiCap);
  writer.EndObject();

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpProfilingTool::ExecuteProfileCapture(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plEditorEngineProcessConnection* pConnection = GetEngineConnection(out_result);
  if (pConnection == nullptr)
    return;

  const char* szEngineFile = ":appdata/profilingEngine.json";
  const char* szEditorFile = ":appdata/profilingEditor.json";

  // The engine is told first so both processes capture the same moment; its answer is awaited below.
  {
    plSimpleConfigMsgToEngine msg;
    msg.m_sWhatToDo = "SaveProfiling";
    msg.m_sPayload = szEngineFile;

    if (!pConnection->SendMessage(&msg))
    {
      out_result.SetError("Sending to the engine process failed. Call engine_status to see what state it is in.");
      return;
    }
  }

  if (plProfilingUtils::SaveProfilingCapture(szEditorFile).Failed())
  {
    out_result.SetError("Saving the editor process' own profiling capture failed. log_read will have the reason.");
    return;
  }

  plStringBuilder sEngineFileAbs;
  {
    auto callback = [&](plProcessMessage* pMsg) -> bool
    {
      auto* pResponse = plDynamicCast<plSaveProfilingResponseToEditor*>(pMsg);
      if (pResponse == nullptr)
        return false;

      sEngineFileAbs = pResponse->m_sProfilingFile;
      return true;
    };
    plProcessCommunicationChannel::WaitForMessageCallback cb = callback;

    if (pConnection->WaitForMessage(plGetStaticRTTI<plSaveProfilingResponseToEditor>(), plTime::MakeFromSeconds(20), &cb).Failed())
    {
      out_result.SetError("The engine process did not report its capture within 20 seconds. engine_status tells whether it "
                          "crashed; if it is merely grinding, wait a moment and call again.");
      return;
    }

    if (sEngineFileAbs.IsEmpty())
    {
      out_result.SetError("The engine process failed to write its profiling capture. Its log file has the reason - app_info "
                          "names the engine log file pattern.");
      return;
    }
  }

  // Timestamped so that consecutive captures never overwrite each other.
  plStringBuilder sMergedFile;
  const plDateTime dt = plDateTime::MakeFromTimestamp(plTimestamp::CurrentTimestamp());
  sMergedFile.AppendFormat(":appdata/profilingMcp_{0}-{1}-{2}_{3}-{4}-{5}-{6}.json", dt.GetYear(), plArgU(dt.GetMonth(), 2, true),
    plArgU(dt.GetDay(), 2, true), plArgU(dt.GetHour(), 2, true), plArgU(dt.GetMinute(), 2, true), plArgU(dt.GetSecond(), 2, true),
    plArgU(dt.GetMicroseconds() / 1000, 3, true));

  if (plProfilingUtils::MergeProfilingCaptures(sEngineFileAbs, szEditorFile, sMergedFile).Failed())
  {
    out_result.SetError("Merging the editor and engine captures failed. log_read will have the reason; the unmerged engine "
                        "capture may still be usable and is named in this message's engineFile if it resolved.");
    return;
  }

  plStringBuilder sMergedAbs;
  if (plFileSystem::ResolvePath(sMergedFile, &sMergedAbs, nullptr).Failed())
  {
    out_result.SetError("The merged capture was written but its absolute path did not resolve, which should not happen.");
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("mergedFile", sMergedAbs);

  plFileStats fileStats;
  if (plOSFile::GetFileStats(sMergedAbs, fileStats).Succeeded())
  {
    writer.AddVariableUInt64("fileSizeBytes", fileStats.m_uiFileSize);
  }

  writer.AddVariableString("engineFile", sEngineFileAbs);

  plStringBuilder sEditorFileAbs;
  if (plFileSystem::ResolvePath(szEditorFile, &sEditorFileAbs, nullptr).Succeeded())
  {
    writer.AddVariableString("editorFile", sEditorFileAbs);
  }

  writer.AddVariableString("hint", "Chrome trace event format with 'B'/'E' begin/end pairs ('ts' in microseconds; pair them "
                                   "per pid+tid on a stack to get durations). 'pid' tells editor from engine, process_name "
                                   "metadata events name them, one 'tid' per thread. Analyze with a script rather than reading "
                                   "the file whole. The capture covers the most recent frames only (ring buffer), ending at "
                                   "this call.");

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpProfilingTool::ExecuteCVarList(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sFilter = plMcpJson::GetString(arguments, "filter");

  // A partially filled cache looks plausible but is not: this plugin loads after the engine's startup
  // broadcast, so until a full re-broadcast was pulled once, only cvars that changed since are in it.
  if (!m_bCVarCacheComplete)
  {
    plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();

    if (pConnection != nullptr && pConnection->IsEngineSetup() && !pConnection->IsProcessCrashed())
      RefreshCVarCache(pConnection);
  }

  constexpr plUInt32 uiCap = 200;
  plUInt32 uiReturned = 0;
  plUInt32 uiMatched = 0;

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.BeginArray("cvars");
  for (auto it = m_CVars.GetIterator(); it.IsValid(); ++it)
  {
    const CVarInfo& info = it.Value();

    if (!sFilter.IsEmpty())
    {
      if (it.Key().GetView().FindSubString_NoCase(sFilter) == nullptr &&
          info.m_sPlugin.GetView().FindSubString_NoCase(sFilter) == nullptr &&
          info.m_sDescription.GetView().FindSubString_NoCase(sFilter) == nullptr)
        continue;
    }

    ++uiMatched;

    if (uiReturned >= uiCap)
      continue;

    writer.BeginObject();
    writer.AddVariableString("name", it.Key());
    writer.AddVariableVariant("value", info.m_Value);

    if (!info.m_sPlugin.IsEmpty())
      writer.AddVariableString("plugin", info.m_sPlugin);

    if (!info.m_sDescription.IsEmpty())
      writer.AddVariableString("description", info.m_sDescription);

    writer.EndObject();
    ++uiReturned;
  }
  writer.EndArray();

  writer.AddVariableUInt32("returned", uiReturned);
  writer.AddVariableUInt32("matched", uiMatched);
  writer.AddVariableUInt32("totalCVars", m_CVars.GetCount());

  if (uiReturned < uiMatched)
    writer.AddVariableBool("truncated", true);

  if (m_CVars.IsEmpty())
  {
    writer.AddVariableString("note", "No cvars are known, even after asking the engine to re-broadcast. engine_status tells "
                                     "whether the engine process is running at all; an engine binary that predates the "
                                     "re-broadcast request answers nothing until the next value change.");
  }

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpProfilingTool::ExecuteCVarSet(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");

  if (sName.IsEmpty())
  {
    out_result.SetError("The 'name' argument is required. Call engine_cvar_list to find the cvar's exact name.");
    return;
  }

  const plVariant* pValue = arguments.GetValue("value");

  if (pValue == nullptr || !pValue->IsValid())
  {
    out_result.SetError("The 'value' argument is required: the new value as a bool, number or string.");
    return;
  }

  plEditorEngineProcessConnection* pConnection = GetEngineConnection(out_result);
  if (pConnection == nullptr)
    return;

  const CVarInfo* pInfo = nullptr;
  {
    auto it = m_CVars.Find(sName);

    // A miss can mean a wrong name or a cache that never saw the startup broadcast; a re-broadcast
    // settles which, and is cheap enough for what is an error path either way.
    if (!it.IsValid())
    {
      RefreshCVarCache(pConnection);
      it = m_CVars.Find(sName);
    }

    if (!it.IsValid())
    {
      out_result.SetError("No cvar of that name exists in the engine. Call engine_cvar_list to find the exact name - it is "
                          "case sensitive.");
      return;
    }

    pInfo = &it.Value();
  }

  // The new value is converted to the type the cvar itself reported; the engine would silently warn
  // and ignore a mismatched type, which from here would look like an unexplained timeout.
  plVariant newValue;
  const plVariantType::Enum targetType = pInfo->m_Value.GetType();

  if (!pValue->CanConvertTo(targetType))
  {
    out_result.SetError("The value cannot be converted to the cvar's type. engine_cvar_list shows the current value, whose "
                        "type is what the new value has to match: bool, int, float or string.");
    return;
  }

  newValue = pValue->ConvertTo(targetType);

  if (newValue == pInfo->m_Value)
  {
    // The engine only broadcasts actual changes, so waiting for a confirmation here would time out.
    plMcpJsonWriter writer;
    writer.BeginObject();
    writer.AddVariableString("name", sName);
    writer.AddVariableVariant("value", newValue);
    writer.AddVariableBool("changed", false);
    writer.AddVariableString("note", "The cvar already has this value.");
    writer.EndObject();
    out_result.m_sText = writer.GetResult();
    return;
  }

  const plVariant oldValue = pInfo->m_Value;

  plChangeCVarMsgToEngine msg;
  msg.m_sCVarName = sName;
  msg.m_NewValue = newValue;

  if (!pConnection->SendMessage(&msg))
  {
    out_result.SetError("Sending to the engine process failed. Call engine_status to see what state it is in.");
    return;
  }

  // The engine broadcasts every cvar change, including this one; that echo is the confirmation. The
  // cache is updated by the same broadcast while this waits.
  plVariant confirmedValue;

  auto callback = [&](plProcessMessage* pMsg) -> bool
  {
    auto* pCVarMsg = plDynamicCast<plCVarMsgToEditor*>(pMsg);
    if (pCVarMsg == nullptr || pCVarMsg->m_sName != sName)
      return false;

    confirmedValue = pCVarMsg->m_Value;
    return true;
  };
  plProcessCommunicationChannel::WaitForMessageCallback cb = callback;

  if (pConnection->WaitForMessage(plGetStaticRTTI<plCVarMsgToEditor>(), plTime::MakeFromSeconds(10), &cb).Failed())
  {
    out_result.SetError("The engine did not confirm the change within 10 seconds. Likely causes: the engine is grinding "
                        "through startup work, the cvar only applies after a restart (engine_restart), or the engine rejected "
                        "the value - its log has a warning then. engine_cvar_list shows what the engine last reported; the "
                        "change may well have been applied despite the missing confirmation.");
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("name", sName);
  writer.AddVariableVariant("oldValue", oldValue);
  writer.AddVariableVariant("value", confirmedValue);
  writer.AddVariableBool("changed", true);
  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpProfilingTool::ExecuteConsole(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sCommand = plMcpJson::GetString(arguments, "command");

  if (sCommand.IsEmpty())
  {
    out_result.SetError("The 'command' argument is required: a cvar assignment, a cvar name to query it, or a console "
                        "function call.");
    return;
  }

  const bool bAutoComplete = plMcpJson::GetBool(arguments, "autoComplete", false);

  plEditorEngineProcessConnection* pConnection = GetEngineConnection(out_result);
  if (pConnection == nullptr)
    return;

  plConsoleCmdMsgToEngine msg;
  msg.m_iType = bAutoComplete ? 1 : 0;
  msg.m_sCommand = sCommand;

  if (!pConnection->SendMessage(&msg))
  {
    out_result.SetError("Sending to the engine process failed. Call engine_status to see what state it is in.");
    return;
  }

  plString sRawResult;

  auto callback = [&](plProcessMessage* pMsg) -> bool
  {
    auto* pResult = plDynamicCast<plConsoleCmdResultMsgToEditor*>(pMsg);
    if (pResult == nullptr)
      return false;

    sRawResult = pResult->m_sResult;
    return true;
  };
  plProcessCommunicationChannel::WaitForMessageCallback cb = callback;

  if (pConnection->WaitForMessage(plGetStaticRTTI<plConsoleCmdResultMsgToEditor>(), plTime::MakeFromSeconds(10), &cb).Failed())
  {
    out_result.SetError("The engine process did not answer within 10 seconds. A console function that blocks - or opens a "
                        "window - never answers; engine_status tells whether the process is still alive.");
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  // The wire format is ';;NN||text' per line, NN being the plConsoleString type as a two digit number.
  plHybridArray<plStringView, 16> lines;
  sRawResult.GetView().Split(false, lines, ";;");

  writer.BeginArray("output");
  for (plStringView sLine : lines)
  {
    const char* szSep = sLine.FindSubString("||");
    if (szSep == nullptr)
      continue;

    plInt32 iType = 0;
    plConversionUtils::StringToInt(plStringView(sLine.GetStartPointer(), szSep), iType).IgnoreResult();

    const plStringView sText(szSep + 2, sLine.GetEndPointer());

    writer.BeginObject();
    writer.AddVariableString("type", ConsoleLineTypeToString(static_cast<plConsoleString::Type>(iType)));
    writer.AddVariableString("text", sText);
    writer.EndObject();
  }
  writer.EndArray();

  if (lines.IsEmpty())
  {
    writer.AddVariableString("note", "The command produced no output. For a cvar assignment that usually means it succeeded "
                                     "silently; engine_cvar_list shows the current value.");
  }

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}
